#include "openmc/mesh.h"

#include <algorithm> // for copy, equal, min, min_element
#include <cstddef> // for size_t
#include <cmath>  // for ceil
#include <string>

#ifdef OPENMC_MPI
#include "mpi.h"
#endif
#include "xtensor/xbuilder.hpp"
#include "xtensor/xeval.hpp"
#include "xtensor/xmath.hpp"
#include "xtensor/xsort.hpp"
#include "xtensor/xtensor.hpp"

#include "openmc/capi.h"
#include "openmc/constants.h"
#include "openmc/error.h"
#include "openmc/hdf5_interface.h"
#include "openmc/message_passing.h"
#include "openmc/search.h"
#include "openmc/tallies/filter.h"
#include "openmc/xml_interface.h"

namespace openmc {

//==============================================================================
// Global variables
//==============================================================================

namespace model {

std::vector<std::unique_ptr<RegularMesh>> meshes;
std::unordered_map<int32_t, int32_t> mesh_map;

} // namespace model

//==============================================================================
// Helper functions
//==============================================================================

//! Update an intersection point if the given candidate is closer.
//
//! The first 6 arguments are coordinates for the starting point of a particle
//! and its intersection with a mesh surface.  If the distance between these
//! two points is shorter than the given `min_distance`, then the `r` argument
//! will be updated to match the intersection point, and `min_distance` will
//! also be updated.

inline bool check_intersection_point(double x1, double x0, double y1,
  double y0, double z1, double z0, Position& r, double& min_distance)
{
  double dist = std::pow(x1-x0, 2) + std::pow(y1-y0, 2) + std::pow(z1-z0, 2);
  if (dist < min_distance) {
    r.x = x1;
    r.y = y1;
    r.z = z1;
    min_distance = dist;
    return true;
  }
  return false;
}

//==============================================================================
// RegularMesh implementation
//==============================================================================

RegularMesh::RegularMesh(pugi::xml_node node)
{
  // Copy mesh id
  if (check_for_node(node, "id")) {
    id_ = std::stoi(get_node_value(node, "id"));

    // Check to make sure 'id' hasn't been used
    if (model::mesh_map.find(id_) != model::mesh_map.end()) {
      fatal_error("Two or more meshes use the same unique ID: " +
        std::to_string(id_));
    }
  }

  // Read mesh type
  if (check_for_node(node, "type")) {
    auto temp = get_node_value(node, "type", true, true);
    if (temp == "regular") {
      // TODO: move elsewhere
    } else {
      fatal_error("Invalid mesh type: " + temp);
    }
  }

  // Determine number of dimensions for mesh
  if (check_for_node(node, "dimension")) {
    shape_ = get_node_xarray<int>(node, "dimension");
    int n = n_dimension_ = shape_.size();
    if (n != 1 && n != 2 && n != 3) {
      fatal_error("Mesh must be one, two, or three dimensions.");
    }

    // Check that dimensions are all greater than zero
    if (xt::any(shape_ <= 0)) {
      fatal_error("All entries on the <dimension> element for a tally "
        "mesh must be positive.");
    }
  }

  // Check for lower-left coordinates
  if (check_for_node(node, "lower_left")) {
    // Read mesh lower-left corner location
    lower_left_ = get_node_xarray<double>(node, "lower_left");
  } else {
    fatal_error("Must specify <lower_left> on a mesh.");
  }

  if (check_for_node(node, "width")) {
    // Make sure both upper-right or width were specified
    if (check_for_node(node, "upper_right")) {
      fatal_error("Cannot specify both <upper_right> and <width> on a mesh.");
    }

    width_ = get_node_xarray<double>(node, "width");

    // Check to ensure width has same dimensions
    auto n = width_.size();
    if (n != lower_left_.size()) {
      fatal_error("Number of entries on <width> must be the same as "
        "the number of entries on <lower_left>.");
    }

    // Check for negative widths
    if (xt::any(width_ < 0.0)) {
      fatal_error("Cannot have a negative <width> on a tally mesh.");
    }

    // Set width and upper right coordinate
    upper_right_ = xt::eval(lower_left_ + shape_ * width_);

  } else if (check_for_node(node, "upper_right")) {
    upper_right_ = get_node_xarray<double>(node, "upper_right");

    // Check to ensure width has same dimensions
    auto n = upper_right_.size();
    if (n != lower_left_.size()) {
      fatal_error("Number of entries on <upper_right> must be the "
        "same as the number of entries on <lower_left>.");
    }

    // Check that upper-right is above lower-left
    if (xt::any(upper_right_ < lower_left_)) {
      fatal_error("The <upper_right> coordinates must be greater than "
        "the <lower_left> coordinates on a tally mesh.");
    }

    // Set width and upper right coordinate
    width_ = xt::eval((upper_right_ - lower_left_) / shape_);
  } else {
    fatal_error("Must specify either <upper_right> and <width> on a mesh.");
  }

  if (shape_.dimension() > 0) {
    if (shape_.size() != lower_left_.size()) {
      fatal_error("Number of entries on <lower_left> must be the same "
        "as the number of entries on <dimension>.");
    }

    // Set volume fraction
    volume_frac_ = 1.0/xt::prod(shape_)();
  }
}

int RegularMesh::get_bin(Position r) const
{
  // Loop over the dimensions of the mesh
  for (int i = 0; i < n_dimension_; ++i) {
    // Check for cases where particle is outside of mesh
    if (r[i] < lower_left_[i]) {
      return -1;
    } else if (r[i] > upper_right_[i]) {
      return -1;
    }
  }

  // Determine indices
  int ijk[n_dimension_];
  bool in_mesh;
  get_indices(r, ijk, &in_mesh);
  if (!in_mesh) return -1;

  // Convert indices to bin
  return get_bin_from_indices(ijk);
}

int RegularMesh::get_bin_from_indices(const int* ijk) const
{
  switch (n_dimension_) {
  case 1:
    return ijk[0] - 1;
  case 2:
    return (ijk[1] - 1)*shape_[0] + ijk[0] - 1;
  case 3:
    return ((ijk[2] - 1)*shape_[1] + (ijk[1] - 1))*shape_[0] + ijk[0] - 1;
  default:
    throw std::runtime_error{"Invalid number of mesh dimensions"};
  }
}

void RegularMesh::get_indices(Position r, int* ijk, bool* in_mesh) const
{
  // Find particle in mesh
  *in_mesh = true;
  for (int i = 0; i < n_dimension_; ++i) {
    ijk[i] = std::ceil((r[i] - lower_left_[i]) / width_[i]);

    // Check if indices are within bounds
    if (ijk[i] < 1 || ijk[i] > shape_[i]) *in_mesh = false;
  }
}

void RegularMesh::get_indices_from_bin(int bin, int* ijk) const
{
  if (n_dimension_ == 1) {
    ijk[0] = bin + 1;
  } else if (n_dimension_ == 2) {
    ijk[0] = bin % shape_[0] + 1;
    ijk[1] = bin / shape_[0] + 1;
  } else if (n_dimension_ == 3) {
    ijk[0] = bin % shape_[0] + 1;
    ijk[1] = (bin % (shape_[0] * shape_[1])) / shape_[0] + 1;
    ijk[2] = bin / (shape_[0] * shape_[1]) + 1;
  }
}

bool RegularMesh::intersects(Position& r0, Position r1, int* ijk) const
{
  switch(n_dimension_) {
  case 1:
    return intersects_1d(r0, r1, ijk);
  case 2:
    return intersects_2d(r0, r1, ijk);
  case 3:
    return intersects_3d(r0, r1, ijk);
  default:
    throw std::runtime_error{"Invalid number of mesh dimensions."};
  }
}

bool RegularMesh::intersects_1d(Position& r0, Position r1, int* ijk) const
{
  // Copy coordinates of starting point
  double x0 = r0.x;
  double y0 = r0.y;
  double z0 = r0.z;

  // Copy coordinates of ending point
  double x1 = r1.x;
  double y1 = r1.y;
  double z1 = r1.z;

  // Copy coordinates of mesh lower_left and upper_right
  double xm0 = lower_left_[0];
  double xm1 = upper_right_[0];

  double min_dist = INFTY;

  // Check if line intersects left surface -- calculate the intersection point
  // (y,z)
  if ((x0 < xm0 && x1 > xm0) || (x0 > xm0 && x1 < xm0)) {
    double yi = y0 + (xm0 - x0) * (y1 - y0) / (x1 - x0);
    double zi = z0 + (xm0 - x0) * (z1 - z0) / (x1 - x0);
    if (check_intersection_point(xm0, x0, yi, yi, zi, zi, r0, min_dist)) {
      ijk[0] = 1;
    }
  }

  // Check if line intersects right surface -- calculate the intersection point
  // (y,z)
  if ((x0 < xm1 && x1 > xm1) || (x0 > xm1 && x1 < xm1)) {
    double yi = y0 + (xm1 - x0) * (y1 - y0) / (x1 - x0);
    double zi = z0 + (xm1 - x0) * (z1 - z0) / (x1 - x0);
    if (check_intersection_point(xm1, x0, yi, yi, zi, zi, r0, min_dist)) {
      ijk[0] = shape_[0];
    }
  }

  return min_dist < INFTY;
}

bool RegularMesh::intersects_2d(Position& r0, Position r1, int* ijk) const
{
  // Copy coordinates of starting point
  double x0 = r0.x;
  double y0 = r0.y;
  double z0 = r0.z;

  // Copy coordinates of ending point
  double x1 = r1.x;
  double y1 = r1.y;
  double z1 = r1.z;

  // Copy coordinates of mesh lower_left
  double xm0 = lower_left_[0];
  double ym0 = lower_left_[1];

  // Copy coordinates of mesh upper_right
  double xm1 = upper_right_[0];
  double ym1 = upper_right_[1];

  double min_dist = INFTY;

  // Check if line intersects left surface -- calculate the intersection point
  // (y,z)
  if ((x0 < xm0 && x1 > xm0) || (x0 > xm0 && x1 < xm0)) {
    double yi = y0 + (xm0 - x0) * (y1 - y0) / (x1 - x0);
    double zi = z0 + (xm0 - x0) * (z1 - z0) / (x1 - x0);
    if (yi >= ym0 && yi < ym1) {
      if (check_intersection_point(xm0, x0, yi, y0, zi, zi, r0, min_dist)) {
        ijk[0] = 1;
        ijk[1] = std::ceil((yi - lower_left_[1]) / width_[1]);
      }
    }
  }

  // Check if line intersects back surface -- calculate the intersection point
  // (x,z)
  if ((y0 < ym0 && y1 > ym0) || (y0 > ym0 && y1 < ym0)) {
    double xi = x0 + (ym0 - y0) * (x1 - x0) / (y1 - y0);
    double zi = z0 + (ym0 - y0) * (z1 - z0) / (y1 - y0);
    if (xi >= xm0 && xi < xm1) {
      if (check_intersection_point(xi, x0, ym0, y0, zi, zi, r0, min_dist)) {
        ijk[0] = std::ceil((xi - lower_left_[0]) / width_[0]);
        ijk[1] = 1;
      }
    }
  }

  // Check if line intersects right surface -- calculate the intersection point
  // (y,z)
  if ((x0 < xm1 && x1 > xm1) || (x0 > xm1 && x1 < xm1)) {
    double yi = y0 + (xm1 - x0) * (y1 - y0) / (x1 - x0);
    double zi = z0 + (xm1 - x0) * (z1 - z0) / (x1 - x0);
    if (yi >= ym0 && yi < ym1) {
      if (check_intersection_point(xm1, x0, yi, y0, zi, zi, r0, min_dist)) {
        ijk[0] = shape_[0];
        ijk[1] = std::ceil((yi - lower_left_[1]) / width_[1]);
      }
    }
  }

  // Check if line intersects front surface -- calculate the intersection point
  // (x,z)
  if ((y0 < ym1 && y1 > ym1) || (y0 > ym1 && y1 < ym1)) {
    double xi = x0 + (ym1 - y0) * (x1 - x0) / (y1 - y0);
    double zi = z0 + (ym1 - y0) * (z1 - z0) / (y1 - y0);
    if (xi >= xm0 && xi < xm1) {
      if (check_intersection_point(xi, x0, ym1, y0, zi, zi, r0, min_dist)) {
        ijk[0] = std::ceil((xi - lower_left_[0]) / width_[0]);
        ijk[1] = shape_[1];
      }
    }
  }

  return min_dist < INFTY;
}

bool RegularMesh::intersects_3d(Position& r0, Position r1, int* ijk) const
{
  // Copy coordinates of starting point
  double x0 = r0.x;
  double y0 = r0.y;
  double z0 = r0.z;

  // Copy coordinates of ending point
  double x1 = r1.x;
  double y1 = r1.y;
  double z1 = r1.z;

  // Copy coordinates of mesh lower_left
  double xm0 = lower_left_[0];
  double ym0 = lower_left_[1];
  double zm0 = lower_left_[2];

  // Copy coordinates of mesh upper_right
  double xm1 = upper_right_[0];
  double ym1 = upper_right_[1];
  double zm1 = upper_right_[2];

  double min_dist = INFTY;

  // Check if line intersects left surface -- calculate the intersection point
  // (y,z)
  if ((x0 < xm0 && x1 > xm0) || (x0 > xm0 && x1 < xm0)) {
    double yi = y0 + (xm0 - x0) * (y1 - y0) / (x1 - x0);
    double zi = z0 + (xm0 - x0) * (z1 - z0) / (x1 - x0);
    if (yi >= ym0 && yi < ym1 && zi >= zm0 && zi < zm1) {
      if (check_intersection_point(xm0, x0, yi, y0, zi, z0, r0, min_dist)) {
        ijk[0] = 1;
        ijk[1] = std::ceil((yi - lower_left_[1]) / width_[1]);
        ijk[2] = std::ceil((zi - lower_left_[2]) / width_[2]);
      }
    }
  }

  // Check if line intersects back surface -- calculate the intersection point
  // (x,z)
  if ((y0 < ym0 && y1 > ym0) || (y0 > ym0 && y1 < ym0)) {
    double xi = x0 + (ym0 - y0) * (x1 - x0) / (y1 - y0);
    double zi = z0 + (ym0 - y0) * (z1 - z0) / (y1 - y0);
    if (xi >= xm0 && xi < xm1 && zi >= zm0 && zi < zm1) {
      if (check_intersection_point(xi, x0, ym0, y0, zi, z0, r0, min_dist)) {
        ijk[0] = std::ceil((xi - lower_left_[0]) / width_[0]);
        ijk[1] = 1;
        ijk[2] = std::ceil((zi - lower_left_[2]) / width_[2]);
      }
    }
  }

  // Check if line intersects bottom surface -- calculate the intersection
  // point (x,y)
  if ((z0 < zm0 && z1 > zm0) || (z0 > zm0 && z1 < zm0)) {
    double xi = x0 + (zm0 - z0) * (x1 - x0) / (z1 - z0);
    double yi = y0 + (zm0 - z0) * (y1 - y0) / (z1 - z0);
    if (xi >= xm0 && xi < xm1 && yi >= ym0 && yi < ym1) {
      if (check_intersection_point(xi, x0, yi, y0, zm0, z0, r0, min_dist)) {
        ijk[0] = std::ceil((xi - lower_left_[0]) / width_[0]);
        ijk[1] = std::ceil((yi - lower_left_[1]) / width_[1]);
        ijk[2] = 1;
      }
    }
  }

  // Check if line intersects right surface -- calculate the intersection point
  // (y,z)
  if ((x0 < xm1 && x1 > xm1) || (x0 > xm1 && x1 < xm1)) {
    double yi = y0 + (xm1 - x0) * (y1 - y0) / (x1 - x0);
    double zi = z0 + (xm1 - x0) * (z1 - z0) / (x1 - x0);
    if (yi >= ym0 && yi < ym1 && zi >= zm0 && zi < zm1) {
      if (check_intersection_point(xm1, x0, yi, y0, zi, z0, r0, min_dist)) {
        ijk[0] = shape_[0];
        ijk[1] = std::ceil((yi - lower_left_[1]) / width_[1]);
        ijk[2] = std::ceil((zi - lower_left_[2]) / width_[2]);
      }
    }
  }

  // Check if line intersects front surface -- calculate the intersection point
  // (x,z)
  if ((y0 < ym1 && y1 > ym1) || (y0 > ym1 && y1 < ym1)) {
    double xi = x0 + (ym1 - y0) * (x1 - x0) / (y1 - y0);
    double zi = z0 + (ym1 - y0) * (z1 - z0) / (y1 - y0);
    if (xi >= xm0 && xi < xm1 && zi >= zm0 && zi < zm1) {
      if (check_intersection_point(xi, x0, ym1, y0, zi, z0, r0, min_dist)) {
        ijk[0] = std::ceil((xi - lower_left_[0]) / width_[0]);
        ijk[1] = shape_[1];
        ijk[2] = std::ceil((zi - lower_left_[2]) / width_[2]);
      }
    }
  }

  // Check if line intersects top surface -- calculate the intersection point
  // (x,y)
  if ((z0 < zm1 && z1 > zm1) || (z0 > zm1 && z1 < zm1)) {
    double xi = x0 + (zm1 - z0) * (x1 - x0) / (z1 - z0);
    double yi = y0 + (zm1 - z0) * (y1 - y0) / (z1 - z0);
    if (xi >= xm0 && xi < xm1 && yi >= ym0 && yi < ym1) {
      if (check_intersection_point(xi, x0, yi, y0, zm1, z0, r0, min_dist)) {
        ijk[0] = std::ceil((xi - lower_left_[0]) / width_[0]);
        ijk[1] = std::ceil((yi - lower_left_[1]) / width_[1]);
        ijk[2] = shape_[2];
      }
    }
  }

  return min_dist < INFTY;
}

void RegularMesh::bins_crossed(const Particle* p, std::vector<int>& bins,
                               std::vector<double>& lengths) const
{
  // ========================================================================
  // Determine where the track intersects the mesh and if it intersects at all.

  // Copy the starting and ending coordinates of the particle.
  Position last_r {p->r_last_};
  Position r {p->r()};
  Direction u {p->u()};

  // Compute the length of the entire track.
  double total_distance = (r - last_r).norm();

  // While determining if this track intersects the mesh, offset the starting
  // and ending coords by a bit.  This avoid finite-precision errors that can
  // occur when the mesh surfaces coincide with lattice or geometric surfaces.
  Position r0 = last_r + TINY_BIT*u;
  Position r1 = r - TINY_BIT*u;

  // Determine the mesh indices for the starting and ending coords.
  int n = n_dimension_;
  int ijk0[n], ijk1[n];
  bool start_in_mesh;
  get_indices(r0, ijk0, &start_in_mesh);
  bool end_in_mesh;
  get_indices(r1, ijk1, &end_in_mesh);

  // Reset coordinates and check for a mesh intersection if necessary.
  if (start_in_mesh) {
    // The initial coords lie in the mesh, use those coords for tallying.
    r0 = last_r;
  } else {
    // The initial coords do not lie in the mesh.  Check to see if the particle
    // eventually intersects the mesh and compute the relevant coords and
    // indices.
    if (!intersects(r0, r1, ijk0)) return;
  }
  r1 = r;

  // ========================================================================
  // Find which mesh cells are traversed and the length of each traversal.

  while (true) {
    if (std::equal(ijk0, ijk0+n, ijk1)) {
      // The track ends in this cell.  Use the particle end location rather
      // than the mesh surface and stop iterating.
      double distance = (r1 - r0).norm();
      bins.push_back(get_bin_from_indices(ijk0));
      lengths.push_back(distance / total_distance);
      break;
    }

    // The track exits this cell.  Determine the distance to each mesh surface.
    double d[n];
    for (int k = 0; k < n; ++k) {
      if (std::fabs(u[k]) < FP_PRECISION) {
        d[k] = INFTY;
      } else if (u[k] > 0) {
        double xyz_cross = lower_left_[k] + ijk0[k] * width_[k];
        d[k] = (xyz_cross - r0[k]) / u[k];
      } else {
        double xyz_cross = lower_left_[k] + (ijk0[k] - 1) * width_[k];
        d[k] = (xyz_cross - r0[k]) / u[k];
      }
    }

    // Pick the closest mesh surface and append this traversal to the output.
    auto j = std::min_element(d, d+n) - d;
    double distance = d[j];
    bins.push_back(get_bin_from_indices(ijk0));
    lengths.push_back(distance / total_distance);

    // Translate to the oncoming mesh surface.
    r0 += distance * u;

    // Increment the indices into the next mesh cell.
    if (u[j] > 0.0) {
      ++ijk0[j];
    } else {
      --ijk0[j];
    }

    // If the next indices are invalid, then the track has left the mesh and
    // we are done.
    bool in_mesh = true;
    for (int i = 0; i < n; ++i) {
      if (ijk0[i] < 1 || ijk0[i] > shape_[i]) {
        in_mesh = false;
        break;
      }
    }
    if (!in_mesh) break;
  }
}

void RegularMesh::surface_bins_crossed(const Particle* p,
                                       std::vector<int>& bins) const
{
  // ========================================================================
  // Determine if the track intersects the tally mesh.

  // Copy the starting and ending coordinates of the particle.
  Position r0 {p->r_last_current_};
  Position r1 {p->r()};
  Direction u {p->u()};

  // Determine indices for starting and ending location.
  int n = n_dimension_;
  int ijk0[n], ijk1[n];
  bool start_in_mesh;
  get_indices(r0, ijk0, &start_in_mesh);
  bool end_in_mesh;
  get_indices(r1, ijk1, &end_in_mesh);

  // Check if the track intersects any part of the mesh.
  if (!start_in_mesh) {
    Position r0_copy = r0;
    int ijk0_copy[n];
    for (int i = 0; i < n; ++i) ijk0_copy[i] = ijk0[i];
    if (!intersects(r0_copy, r1, ijk0_copy)) return;
  }

  // ========================================================================
  // Find which mesh surfaces are crossed.

  // Calculate number of surface crossings
  int n_cross = 0;
  for (int i = 0; i < n; ++i) n_cross += std::abs(ijk1[i] - ijk0[i]);
  if (n_cross == 0) return;

  // Bounding coordinates
  Position xyz_cross;
  for (int i = 0; i < n; ++i) {
    if (u[i] > 0.0) {
      xyz_cross[i] = lower_left_[i] + ijk0[i] * width_[i];
    } else {
      xyz_cross[i] = lower_left_[i] + (ijk0[i] - 1) * width_[i];
    }
  }

  for (int j = 0; j < n_cross; ++j) {
    // Set the distances to infinity
    Position d {INFTY, INFTY, INFTY};

    // Determine closest bounding surface. We need to treat
    // special case where the cosine of the angle is zero since this would
    // result in a divide-by-zero.
    double distance = INFTY;
    for (int i = 0; i < n; ++i) {
      if (u[i] == 0) {
        d[i] = INFTY;
      } else {
        d[i] = (xyz_cross[i] - r0[i])/u[i];
      }
      distance = std::min(distance, d[i]);
    }

    // Loop over the dimensions
    for (int i = 0; i < n; ++i) {
      // Check whether distance is the shortest distance
      if (distance == d[i]) {

        // Check whether the current indices are within the mesh bounds
        bool in_mesh = true;
        for (int j = 0; j < n; ++j) {
          if (ijk0[j] < 1 || ijk0[j] > shape_[j]) {
            in_mesh = false;
            break;
          }
        }

        // Check whether particle is moving in positive i direction
        if (u[i] > 0) {

          // Outward current on i max surface
          if (in_mesh) {
            int i_surf = 4*i + 3;
            int i_mesh = get_bin_from_indices(ijk0);
            int i_bin = 4*n*i_mesh + i_surf - 1;

            bins.push_back(i_bin);
          }

          // Advance position
          ++ijk0[i];
          xyz_cross[i] += width_[i];
          in_mesh = true;
          for (int j = 0; j < n; ++j) {
            if (ijk0[j] < 1 || ijk0[j] > shape_[j]) {
              in_mesh = false;
              break;
            }
          }

          // If the particle crossed the surface, tally the inward current on
          // i min surface
          if (in_mesh) {
            int i_surf = 4*i + 2;
            int i_mesh = get_bin_from_indices(ijk0);
            int i_bin = 4*n*i_mesh + i_surf - 1;

            bins.push_back(i_bin);
          }

        } else {
          // The particle is moving in the negative i direction

          // Outward current on i min surface
          if (in_mesh) {
            int i_surf = 4*i + 1;
            int i_mesh = get_bin_from_indices(ijk0);
            int i_bin = 4*n*i_mesh + i_surf - 1;

            bins.push_back(i_bin);
          }

          // Advance position
          --ijk0[i];
          xyz_cross[i] -= width_[i];
          in_mesh = true;
          for (int j = 0; j < n; ++j) {
            if (ijk0[j] < 1 || ijk0[j] > shape_[j]) {
              in_mesh = false;
              break;
            }
          }

          // If the particle crossed the surface, tally the inward current on
          // i max surface
          if (in_mesh) {
            int i_surf = 4*i + 4;
            int i_mesh = get_bin_from_indices(ijk0);
            int i_bin = 4*n*i_mesh + i_surf - 1;

            bins.push_back(i_bin);
          }
        }
      }
    }

    // Calculate new coordinates
    r0 += distance * u;
  }
}

void RegularMesh::to_hdf5(hid_t group) const
{
  hid_t mesh_group = create_group(group, "mesh " + std::to_string(id_));

  write_dataset(mesh_group, "type", "regular");
  write_dataset(mesh_group, "dimension", shape_);
  write_dataset(mesh_group, "lower_left", lower_left_);
  write_dataset(mesh_group, "upper_right", upper_right_);
  write_dataset(mesh_group, "width", width_);

  close_group(mesh_group);
}

xt::xarray<double>
RegularMesh::count_sites(const std::vector<Particle::Bank>& bank,
  bool* outside) const
{
  // Determine shape of array for counts
  std::size_t m = xt::prod(shape_)();
  std::vector<std::size_t> shape = {m};

  // Create array of zeros
  xt::xarray<double> cnt {shape, 0.0};
  bool outside_ = false;

  for (const auto& site : bank) {
    // determine scoring bin for entropy mesh
    int mesh_bin = get_bin(site.r);

    // if outside mesh, skip particle
    if (mesh_bin < 0) {
      outside_ = true;
      continue;
    }

    // Add to appropriate bin
    cnt(mesh_bin) += site.wgt;
  }

  // Create copy of count data
  int total = cnt.size();
  double* cnt_reduced = new double[total];

#ifdef OPENMC_MPI
  // collect values from all processors
  MPI_Reduce(cnt.data(), cnt_reduced, total, MPI_DOUBLE, MPI_SUM, 0,
    mpi::intracomm);

  // Check if there were sites outside the mesh for any processor
  if (outside) {
    MPI_Reduce(&outside_, outside, 1, MPI_C_BOOL, MPI_LOR, 0, mpi::intracomm);
  }
#else
  std::copy(cnt.data(), cnt.data() + total, cnt_reduced);
  if (outside) *outside = outside_;
#endif

  // Adapt reduced values in array back into an xarray
  auto arr = xt::adapt(cnt_reduced, total, xt::acquire_ownership(), shape);
  xt::xarray<double> counts = arr;

  return counts;
}

//==============================================================================
// C API functions
//==============================================================================

//! Extend the meshes array by n elements
extern "C" int
openmc_extend_meshes(int32_t n, int32_t* index_start, int32_t* index_end)
{
  if (index_start) *index_start = model::meshes.size();
  for (int i = 0; i < n; ++i) {
    model::meshes.push_back(std::make_unique<RegularMesh>());
  }
  if (index_end) *index_end = model::meshes.size() - 1;

  return 0;
}

//! Return the index in the meshes array of a mesh with a given ID
extern "C" int
openmc_get_mesh_index(int32_t id, int32_t* index)
{
  auto pair = model::mesh_map.find(id);
  if (pair == model::mesh_map.end()) {
    set_errmsg("No mesh exists with ID=" + std::to_string(id) + ".");
    return OPENMC_E_INVALID_ID;
  }
  *index = pair->second;
  return 0;
}

// Return the ID of a mesh
extern "C" int
openmc_mesh_get_id(int32_t index, int32_t* id)
{
  if (index < 0 || index >= model::meshes.size()) {
    set_errmsg("Index in meshes array is out of bounds.");
    return OPENMC_E_OUT_OF_BOUNDS;
  }
  *id = model::meshes[index]->id_;
  return 0;
}

//! Set the ID of a mesh
extern "C" int
openmc_mesh_set_id(int32_t index, int32_t id)
{
  if (index < 0 || index >= model::meshes.size()) {
    set_errmsg("Index in meshes array is out of bounds.");
    return OPENMC_E_OUT_OF_BOUNDS;
  }
  model::meshes[index]->id_ = id;
  model::mesh_map[id] = index;
  return 0;
}

//! Get the dimension of a mesh
extern "C" int
openmc_mesh_get_dimension(int32_t index, int** dims, int* n)
{
  if (index < 0 || index >= model::meshes.size()) {
    set_errmsg("Index in meshes array is out of bounds.");
    return OPENMC_E_OUT_OF_BOUNDS;
  }
  *dims = model::meshes[index]->shape_.data();
  *n = model::meshes[index]->n_dimension_;
  return 0;
}

//! Set the dimension of a mesh
extern "C" int
openmc_mesh_set_dimension(int32_t index, int n, const int* dims)
{
  if (index < 0 || index >= model::meshes.size()) {
    set_errmsg("Index in meshes array is out of bounds.");
    return OPENMC_E_OUT_OF_BOUNDS;
  }

  // Copy dimension
  std::vector<std::size_t> shape = {static_cast<std::size_t>(n)};
  auto& m = model::meshes[index];
  m->shape_ = xt::adapt(dims, n, xt::no_ownership(), shape);
  m->n_dimension_ = m->shape_.size();

  return 0;
}

//! Get the mesh parameters
extern "C" int
openmc_mesh_get_params(int32_t index, double** ll, double** ur, double** width, int* n)
{
  if (index < 0 || index >= model::meshes.size()) {
    set_errmsg("Index in meshes array is out of bounds.");
    return OPENMC_E_OUT_OF_BOUNDS;
  }

  auto& m = model::meshes[index];
  if (m->lower_left_.dimension() == 0) {
    set_errmsg("Mesh parameters have not been set.");
    return OPENMC_E_ALLOCATE;
  }

  *ll = m->lower_left_.data();
  *ur = m->upper_right_.data();
  *width = m->width_.data();
  *n = m->n_dimension_;
  return 0;
}

//! Set the mesh parameters
extern "C" int
openmc_mesh_set_params(int32_t index, int n, const double* ll, const double* ur,
                       const double* width)
{
  if (index < 0 || index >= model::meshes.size()) {
    set_errmsg("Index in meshes array is out of bounds.");
    return OPENMC_E_OUT_OF_BOUNDS;
  }

  auto& m = model::meshes[index];
  std::vector<std::size_t> shape = {static_cast<std::size_t>(n)};
  if (ll && ur) {
    m->lower_left_ = xt::adapt(ll, n, xt::no_ownership(), shape);
    m->upper_right_ = xt::adapt(ur, n, xt::no_ownership(), shape);
    m->width_ = (m->upper_right_ - m->lower_left_) / m->shape_;
  } else if (ll && width) {
    m->lower_left_ = xt::adapt(ll, n, xt::no_ownership(), shape);
    m->width_ = xt::adapt(width, n, xt::no_ownership(), shape);
    m->upper_right_ = m->lower_left_ + m->shape_ * m->width_;
  } else if (ur && width) {
    m->upper_right_ = xt::adapt(ur, n, xt::no_ownership(), shape);
    m->width_ = xt::adapt(width, n, xt::no_ownership(), shape);
    m->lower_left_ = m->upper_right_ - m->shape_ * m->width_;
  } else {
    set_errmsg("At least two parameters must be specified.");
    return OPENMC_E_INVALID_ARGUMENT;
  }

  return 0;
}

//==============================================================================
// Non-member functions
//==============================================================================

void read_meshes(pugi::xml_node root)
{
  for (auto node : root.children("mesh")) {
    // Read mesh and add to vector
    model::meshes.push_back(std::make_unique<RegularMesh>(node));

    // Map ID to position in vector
    model::mesh_map[model::meshes.back()->id_] = model::meshes.size() - 1;
  }
}

void meshes_to_hdf5(hid_t group)
{
  // Write number of meshes
  hid_t meshes_group = create_group(group, "meshes");
  int32_t n_meshes = model::meshes.size();
  write_attribute(meshes_group, "n_meshes", n_meshes);

  if (n_meshes > 0) {
    // Write IDs of meshes
    std::vector<int> ids;
    for (const auto& m : model::meshes) {
      m->to_hdf5(meshes_group);
      ids.push_back(m->id_);
    }
    write_attribute(meshes_group, "ids", ids);
  }

  close_group(meshes_group);
}

void free_memory_mesh()
{
  model::meshes.clear();
  model::mesh_map.clear();
}

extern "C" int n_meshes() { return model::meshes.size(); }

} // namespace openmc
