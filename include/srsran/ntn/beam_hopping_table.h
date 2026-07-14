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

#include "beam_position.h"
#include <array>
#include <cstdint>
#include <limits>

namespace srsran {
namespace srs_ntn {

/// Maximum number of active beam positions supported in one hopping cycle.
static constexpr uint16_t MAX_ACTIVE_BEAM_POSITIONS = 256u;

/// One slot entry in the beam hopping table.
struct beam_hop_entry_t {
  uint16_t beam_position_id; ///< Beam position to illuminate during this slot.
  uint8_t  color;            ///< 3-colour group of this position (0/1/2), cached for convenience.
};

/// Beam positions that are active during one dwell interval.
struct active_beam_set_t {
  std::array<beam_hop_entry_t, MAX_ACTIVE_BEAM_POSITIONS> entries{};
  uint16_t                                                nof_beams = 0;
};

/// Quasi-static beam hopping table.
///
/// Maps each dwell-in-cycle index to one or more simultaneously active beam positions.  The active set at a given SFN
/// is:
///
///   dwell_in_cycle = (SFN % cycle_frames) / dwell_frames
///   first_entry    = dwell_in_cycle * nof_beams_per_dwell
///   active_beams   = entries[first_entry .. first_entry + nof_beams_per_dwell - 1]
///
/// Construction constraints (all enforced by make_beam_hopping_table):
///   1. n_active % 3 == 0.
///   2. nof_beams_per_dwell <= n_active / 3 and divides n_active / 3.
///   3. (n_active / nof_beams_per_dwell) * dwell_frames <= 1024.
///   4. n_active <= MAX_ACTIVE_BEAM_POSITIONS.
///
/// Dwell ordering uses a 3-phase interleaved colour scheme.  For nof_beams_per_dwell = 2, the sequence is:
///   dwell 0 = colour-0 beam[0..1], dwell 1 = colour-1 beam[0..1], dwell 2 = colour-2 beam[0..1],
///   dwell 3 = colour-0 beam[2..3], ...
///
/// This keeps each dwell set within one colour group, so simultaneously active beams are non-adjacent in the current
/// hexagonal grid, and consecutive dwell sets rotate across colour groups.
struct beam_hopping_table_t {
  uint16_t n_active;                ///< Active beam positions per cycle.
  uint16_t nof_beams_per_dwell = 1; ///< Simultaneously active beam positions per dwell.
  uint16_t nof_dwell_entries   = 0; ///< Number of dwell entries per cycle.
  uint16_t dwell_frames;            ///< Radio frames (10 ms each) the beam set stays active.
  uint16_t cycle_frames;            ///< Total frames per cycle = nof_dwell_entries * dwell_frames.

  std::array<beam_hop_entry_t, MAX_ACTIVE_BEAM_POSITIONS> entries{};

  /// Return the first active beam position id for the given SFN.
  uint16_t get_active_beam(uint32_t sfn) const noexcept
  {
    return get_active_beams(sfn).entries[0].beam_position_id;
  }

  /// Return the active beam positions for the given SFN.
  active_beam_set_t get_active_beams(uint32_t sfn) const noexcept;

  /// Return the number of radio frames until the beam next visits target_id, starting from sfn.
  /// Returns std::numeric_limits<uint32_t>::max() if target_id is not in the table.
  uint32_t frames_until_next_visit(uint32_t sfn, uint16_t target_id) const noexcept;
};

/// Build a beam hopping table from a grid using the 3-phase interleaved colour scheme.
///
/// \param grid         Source beam position grid (must contain >= n_active positions).
/// \param n_active     Number of active beam positions per cycle.
/// \param dwell_frames Radio frames the beam set dwells at each position group (10 ms per frame).
/// \param nof_beams_per_dwell Number of same-colour beam positions to activate in one dwell.
///
/// Active beams are chosen as the first n_active/3 positions from each colour group,
/// sorted by (row, col) within each group for spatial locality, then batched by nof_beams_per_dwell.
beam_hopping_table_t make_beam_hopping_table(const beam_position_grid& grid,
                                             uint16_t                  n_active,
                                             uint16_t                  dwell_frames,
                                             uint16_t                  nof_beams_per_dwell = 1);

} // namespace srs_ntn
} // namespace srsran
