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

#include "srsran/ntn/beam_hopping_table.h"
#include "srsran/ran/slot_point.h"   // for NOF_SFNS
#include "srsran/support/srsran_assert.h"
#include <algorithm>
#include <array>
#include <limits>
#include <vector>

using namespace srsran;
using namespace srsran::srs_ntn;

// ---------------------------------------------------------------------------
// beam_hopping_table_t member functions
// ---------------------------------------------------------------------------

active_beam_set_t beam_hopping_table_t::get_active_beams(uint32_t sfn) const noexcept
{
  active_beam_set_t out;
  const uint32_t    dwell_idx = (sfn % cycle_frames) / dwell_frames;
  const uint16_t    first_entry =
      static_cast<uint16_t>(static_cast<uint16_t>(dwell_idx) * nof_beams_per_dwell);

  out.nof_beams = nof_beams_per_dwell;
  for (uint16_t i = 0; i != nof_beams_per_dwell; ++i) {
    out.entries[i] = entries[first_entry + i];
  }
  return out;
}

uint32_t beam_hopping_table_t::frames_until_next_visit(uint32_t sfn, uint16_t target_id) const noexcept
{
  const uint32_t current_dwell = (sfn % cycle_frames) / dwell_frames;
  for (uint16_t offset = 1; offset <= nof_dwell_entries; ++offset) {
    const uint16_t dwell = static_cast<uint16_t>((current_dwell + offset) % nof_dwell_entries);
    const uint16_t first_entry = static_cast<uint16_t>(dwell * nof_beams_per_dwell);
    for (uint16_t i = 0; i != nof_beams_per_dwell; ++i) {
      if (entries[first_entry + i].beam_position_id == target_id) {
        return static_cast<uint32_t>(offset) * dwell_frames;
      }
    }
  }
  return std::numeric_limits<uint32_t>::max();
}

// ---------------------------------------------------------------------------
// make_beam_hopping_table
// ---------------------------------------------------------------------------

beam_hopping_table_t srsran::srs_ntn::make_beam_hopping_table(const beam_position_grid& grid,
                                                               uint16_t                  n_active,
                                                               uint16_t                  dwell_frames,
                                                               uint16_t                  nof_beams_per_dwell)
{
  srsran_assert(n_active % 3u == 0u,
                "n_active={} must be divisible by 3 (3-phase colour interleaving)", n_active);
  srsran_assert(n_active <= MAX_ACTIVE_BEAM_POSITIONS,
                "n_active={} exceeds MAX_ACTIVE_BEAM_POSITIONS={}", n_active, MAX_ACTIVE_BEAM_POSITIONS);
  srsran_assert(n_active <= grid.positions.size(),
                "n_active={} exceeds available beam positions={}", n_active, grid.positions.size());
  srsran_assert(dwell_frames > 0u, "dwell_frames must be at least 1");
  srsran_assert(nof_beams_per_dwell > 0u, "nof_beams_per_dwell must be at least 1");

  const uint16_t per_group = n_active / 3u;
  srsran_assert(nof_beams_per_dwell <= per_group,
                "nof_beams_per_dwell={} exceeds active beams per colour group={}",
                nof_beams_per_dwell,
                per_group);
  srsran_assert(per_group % nof_beams_per_dwell == 0u,
                "nof_beams_per_dwell={} must divide active beams per colour group={}",
                nof_beams_per_dwell,
                per_group);

  const uint16_t nof_dwell_entries = static_cast<uint16_t>(n_active / nof_beams_per_dwell);
  const uint32_t cycle_frames = static_cast<uint32_t>(nof_dwell_entries) * static_cast<uint32_t>(dwell_frames);
  srsran_assert(cycle_frames <= NOF_SFNS,
                "cycle_frames={} ((n_active / nof_beams_per_dwell) * dwell_frames) must not exceed NOF_SFNS={}",
                cycle_frames,
                NOF_SFNS);

  // -------------------------------------------------------------------------
  // Group all grid positions by their 3-colour label, sorted by (row, col).
  // -------------------------------------------------------------------------
  std::array<std::vector<uint16_t>, 3> color_groups;
  for (const auto& pos : grid.positions) {
    color_groups[pos.color].push_back(pos.beam_position_id);
  }

  for (auto& group : color_groups) {
    std::sort(group.begin(), group.end(), [&](uint16_t a, uint16_t b) {
      const beam_position_t& pa = grid.positions[a];
      const beam_position_t& pb = grid.positions[b];
      return (pa.row != pb.row) ? (pa.row < pb.row) : (pa.col < pb.col);
    });
  }

  for (uint8_t color = 0; color < 3u; ++color) {
    srsran_assert(color_groups[color].size() >= per_group,
                  "colour group {} has only {} positions but {} are required",
                  color, color_groups[color].size(), per_group);
  }

  // -------------------------------------------------------------------------
  // Build the interleaved table: C0[0], C1[0], C2[0], C0[1], C1[1], C2[1], …
  //
  // This guarantees that any two consecutive time slots serve non-adjacent
  // beam positions (adjacent positions have different colour labels).
  // -------------------------------------------------------------------------
  beam_hopping_table_t table;
  table.n_active              = n_active;
  table.nof_beams_per_dwell  = nof_beams_per_dwell;
  table.nof_dwell_entries    = nof_dwell_entries;
  table.dwell_frames          = dwell_frames;
  table.cycle_frames          = static_cast<uint16_t>(cycle_frames);

  uint16_t dwell_idx = 0;
  const uint16_t nof_batches_per_group = static_cast<uint16_t>(per_group / nof_beams_per_dwell);
  for (uint16_t batch = 0; batch < nof_batches_per_group; ++batch) {
    for (uint8_t color = 0; color < 3u; ++color) {
      const uint16_t first_entry = static_cast<uint16_t>(dwell_idx * nof_beams_per_dwell);
      const uint16_t first_beam  = static_cast<uint16_t>(batch * nof_beams_per_dwell);
      for (uint16_t beam_idx = 0; beam_idx != nof_beams_per_dwell; ++beam_idx) {
        const uint16_t id = color_groups[color][first_beam + beam_idx];
        table.entries[first_entry + beam_idx] = {id, color};
      }
      ++dwell_idx;
    }
  }

  return table;
}
