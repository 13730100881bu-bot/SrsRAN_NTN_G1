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

#include "srsran/ntn/beam_position.h"
#include "srsran/support/srsran_assert.h"
#include <algorithm>
#include <cmath>

using namespace srsran;
using namespace srsran::srs_ntn;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr double KM_PER_DEGREE_LAT = 111.0; // 1° latitude ≈ 111 km (constant)
static constexpr double PI                 = 3.14159265358979323846;

// ---------------------------------------------------------------------------
// beam_position_grid member functions
// ---------------------------------------------------------------------------

uint16_t beam_position_grid::get_id(uint16_t row, uint16_t col) const noexcept
{
  if (row >= n_rows || col >= n_cols_per_row[row]) {
    return INVALID_BEAM_POSITION_ID;
  }
  const uint32_t id = row_start_id[row] + col;
  // Guard against overflow when the grid is very large.
  if (id > std::numeric_limits<uint16_t>::max()) {
    return INVALID_BEAM_POSITION_ID;
  }
  return static_cast<uint16_t>(id);
}

// ---------------------------------------------------------------------------
// make_beam_position_grid
// ---------------------------------------------------------------------------

beam_position_grid srsran::srs_ntn::make_beam_position_grid(const beam_position_grid_config& cfg)
{
  srsran_assert(cfg.lat_north > cfg.lat_south, "lat_north must be greater than lat_south");
  srsran_assert(cfg.lon_east > cfg.lon_west, "lon_east must be greater than lon_west");
  srsran_assert(cfg.beam_radius_m > 0.0, "beam_radius_m must be positive");
  srsran_assert(cfg.overlap_factor > 0.0 && cfg.overlap_factor <= 1.0,
                "overlap_factor must be in (0, 1]");

  beam_position_grid grid;
  grid.cfg = cfg;

  // Centre-to-centre distance and derived spacings.
  const double d_km      = 2.0 * (cfg.beam_radius_m / 1000.0) * cfg.overlap_factor;
  const double delta_lat = d_km * (std::sqrt(3.0) / 2.0) / KM_PER_DEGREE_LAT;

  grid.n_rows = static_cast<uint16_t>(std::ceil((cfg.lat_north - cfg.lat_south) / delta_lat));
  srsran_assert(grid.n_rows > 0, "Grid has zero rows — check service area boundaries");

  grid.n_cols_per_row.resize(grid.n_rows);
  grid.row_start_id.resize(static_cast<size_t>(grid.n_rows) + 1u);

  // -------------------------------------------------------------------------
  // Pass 1: compute per-row column counts and prefix-sum table.
  // -------------------------------------------------------------------------
  grid.row_start_id[0] = 0;
  for (uint16_t r = 0; r < grid.n_rows; ++r) {
    const double lat_r     = cfg.lat_north - r * delta_lat;
    const double delta_lon = d_km / (KM_PER_DEGREE_LAT * std::cos(lat_r * PI / 180.0));
    // Odd rows are offset east by half a column spacing.
    const double offset  = (r % 2u == 1u) ? delta_lon / 2.0 : 0.0;
    const double span    = cfg.lon_east - cfg.lon_west - offset;
    const auto   n_cols  = (span > 0.0) ? static_cast<uint16_t>(std::floor(span / delta_lon) + 1u) : uint16_t{0};
    grid.n_cols_per_row[r]     = n_cols;
    grid.row_start_id[r + 1u]  = grid.row_start_id[r] + n_cols;
  }

  const uint32_t total_positions = grid.row_start_id[grid.n_rows];
  srsran_assert(total_positions <= std::numeric_limits<uint16_t>::max(),
                "Grid has {} positions which exceeds uint16_t range — reduce service area or increase beam size",
                total_positions);

  grid.positions.resize(total_positions);

  // -------------------------------------------------------------------------
  // Pass 2: fill beam_position_t for every grid cell.
  // -------------------------------------------------------------------------
  for (uint16_t r = 0; r < grid.n_rows; ++r) {
    const double lat_r     = cfg.lat_north - r * delta_lat;
    const double delta_lon = d_km / (KM_PER_DEGREE_LAT * std::cos(lat_r * PI / 180.0));
    const double offset    = (r % 2u == 1u) ? delta_lon / 2.0 : 0.0;

    for (uint16_t c = 0; c < grid.n_cols_per_row[r]; ++c) {
      const uint16_t id = static_cast<uint16_t>(grid.row_start_id[r] + c);
      auto&          p  = grid.positions[id];

      p.beam_position_id  = id;
      p.row               = r;
      p.col               = c;
      p.center_lat        = lat_r;
      p.center_lon        = cfg.lon_west + offset + c * delta_lon;
      p.coverage_radius_m = cfg.beam_radius_m;
      p.distance_threshold = static_cast<uint16_t>(cfg.beam_radius_m / 50.0);
      p.color             = static_cast<uint8_t>((c + 2u * (r % 2u)) % 3u);
      p.neighbors.fill(INVALID_BEAM_POSITION_ID);
    }
  }

  // -------------------------------------------------------------------------
  // Pass 3: fill neighbour IDs for every position.
  //
  // Neighbour index layout (clockwise from upper-left):
  //   0=UL  1=UR  2=R  3=LR  4=LL  5=L
  //
  // Even row (r%2==0):  UL=(r-1,c-1) UR=(r-1,c) R=(r,c+1) LR=(r+1,c) LL=(r+1,c-1) L=(r,c-1)
  // Odd  row (r%2==1):  UL=(r-1,c)   UR=(r-1,c+1) R=(r,c+1) LR=(r+1,c+1) LL=(r+1,c) L=(r,c-1)
  // -------------------------------------------------------------------------
  auto safe_id = [&](int r, int c) -> uint16_t {
    if (r < 0 || r >= static_cast<int>(grid.n_rows)) {
      return INVALID_BEAM_POSITION_ID;
    }
    if (c < 0 || c >= static_cast<int>(grid.n_cols_per_row[r])) {
      return INVALID_BEAM_POSITION_ID;
    }
    return static_cast<uint16_t>(grid.row_start_id[r] + c);
  };

  for (auto& p : grid.positions) {
    const int r = static_cast<int>(p.row);
    const int c = static_cast<int>(p.col);

    if (p.row % 2u == 0u) {
      p.neighbors[0] = safe_id(r - 1, c - 1); // UL
      p.neighbors[1] = safe_id(r - 1, c);     // UR
      p.neighbors[2] = safe_id(r,     c + 1); // R
      p.neighbors[3] = safe_id(r + 1, c);     // LR
      p.neighbors[4] = safe_id(r + 1, c - 1); // LL
      p.neighbors[5] = safe_id(r,     c - 1); // L
    } else {
      p.neighbors[0] = safe_id(r - 1, c);     // UL
      p.neighbors[1] = safe_id(r - 1, c + 1); // UR
      p.neighbors[2] = safe_id(r,     c + 1); // R
      p.neighbors[3] = safe_id(r + 1, c + 1); // LR
      p.neighbors[4] = safe_id(r + 1, c);     // LL
      p.neighbors[5] = safe_id(r,     c - 1); // L
    }
  }

  return grid;
}
