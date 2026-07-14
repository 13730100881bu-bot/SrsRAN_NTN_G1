/*
 *
 * Copyright 2021-2026 Software Radio Systems Limited
 *
 * This file is part of srsRAN.
 *
 * srsRAN is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * srsRAN is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * A copy of the GNU Affero General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */

#pragma once

#include "srsran/ran/ntn.h"
#include <array>
#include <cstdint>
#include <vector>

namespace srsran {
namespace srs_ntn {

/// Sentinel value for a missing or out-of-range beam position ID.
static constexpr uint16_t INVALID_BEAM_POSITION_ID = 0xFFFFu;

/// Number of hexagonal neighbours each beam position has (at most).
static constexpr uint8_t NOF_HEX_NEIGHBORS = 6;

/// Parameters for generating a hexagonal beam position grid over a geographic service area.
///
/// Grid layout (pointy-top hexagons, rows running west→east):
///   - Even rows start at lon_west; odd rows are offset east by Δlon/2.
///   - Row spacing is constant (Δlat). Column spacing varies per row to keep the
///     physical centre-to-centre distance uniform despite longitude distortion.
struct beam_position_grid_config {
  double lat_north;      ///< Northern boundary of service area (degrees, WGS-84).
  double lat_south;      ///< Southern boundary of service area (degrees, WGS-84).
  double lon_west;       ///< Western boundary of service area (degrees, WGS-84).
  double lon_east;       ///< Eastern boundary of service area (degrees, WGS-84).
  double beam_radius_m;  ///< 3 dB beam footprint radius (metres).
  /// Centre-to-centre distance = 2 * beam_radius_m * overlap_factor.
  /// Typical range 0.85–0.95; lower values increase overlap and coverage robustness.
  double overlap_factor;
};

/// A single ground beam position in a hexagonal grid.
///
/// Coordinate conventions
///   row  : 0 = northernmost row, increases southward.
///   col  : 0 = westernmost column within the row, increases eastward.
///
/// Neighbor ordering (clockwise, index 0 = upper-left):
///   0=UL  1=UR  2=R  3=LR  4=LL  5=L
///
/// Neighbor rules for even row (row % 2 == 0):
///   UL=(r-1,c-1)  UR=(r-1,c)  R=(r,c+1)  LR=(r+1,c)  LL=(r+1,c-1)  L=(r,c-1)
/// Neighbor rules for odd row (row % 2 == 1):
///   UL=(r-1,c)    UR=(r-1,c+1) R=(r,c+1)  LR=(r+1,c+1) LL=(r+1,c)  L=(r,c-1)
struct beam_position_t {
  uint16_t beam_position_id;  ///< Global 1-D index (prefix-sum scheme, see beam_position_grid).
  uint16_t row;               ///< Grid row (north=0, increases southward).
  uint16_t col;               ///< Grid column within the row (west=0, increases eastward).
  double   center_lat;        ///< Beam centre latitude  (degrees, WGS-84).
  double   center_lon;        ///< Beam centre longitude (degrees, WGS-84).
  double   coverage_radius_m; ///< 3 dB footprint radius (metres).
  /// SIB-19 distanceThresh-r17 value: coverage_radius_m / 50 (50-metre steps, TS 38.331).
  uint16_t distance_threshold;
  /// 3-colouring label (0/1/2).  Formula: (col + 2*(row%2)) % 3.
  /// All six neighbours of any beam position have a different colour label, enabling
  /// interference-aware beam hopping slot ordering.
  uint8_t color;
  /// IDs of the (up to) six hexagonal neighbours, clockwise from upper-left.
  /// Absent neighbours (grid boundary) are set to INVALID_BEAM_POSITION_ID.
  std::array<uint16_t, NOF_HEX_NEIGHBORS> neighbors;
};

/// Runtime hexagonal beam position grid.
///
/// Because each row spans fewer degrees of longitude at higher latitudes (cos-law correction),
/// column counts are unequal across rows.  Beam position IDs are assigned row-by-row via a
/// prefix-sum table:
///
///   beam_position_id(row, col) = row_start_id[row] + col
///
/// Reverse lookup (id → row, col) uses binary search over row_start_id.
struct beam_position_grid {
  beam_position_grid_config cfg;
  uint16_t                  n_rows;
  /// Number of columns in each row (length = n_rows).
  std::vector<uint16_t> n_cols_per_row;
  /// Prefix-sum of n_cols_per_row (length = n_rows + 1).
  /// row_start_id[r] = sum of n_cols_per_row[0..r-1]; row_start_id[0] = 0.
  std::vector<uint32_t> row_start_id;
  /// All beam positions indexed by beam_position_id (length = row_start_id[n_rows]).
  std::vector<beam_position_t> positions;

  /// Return the beam_position_id for (row, col), or INVALID_BEAM_POSITION_ID if out of range.
  uint16_t get_id(uint16_t row, uint16_t col) const noexcept;

  /// Return the beam_position_t for a given id (undefined behaviour if id is invalid).
  const beam_position_t& get_position(uint16_t id) const noexcept { return positions[id]; }
};

/// Build a beam_position_grid from the supplied configuration.
///
/// The grid covers the bounding rectangle [lat_south, lat_north] × [lon_west, lon_east].
/// Row spacing is Δlat = d_km * sqrt(3)/2 / 111 degrees (constant).
/// Column spacing varies per row: Δlon[r] = d_km / (111 * cos(lat[r])).
/// Odd rows are shifted east by Δlon[r]/2.
/// Neighbours are pre-computed for all positions.
beam_position_grid make_beam_position_grid(const beam_position_grid_config& cfg);

} // namespace srs_ntn
} // namespace srsran
