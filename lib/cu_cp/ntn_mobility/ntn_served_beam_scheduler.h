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

#include "ntn_served_beam_selector.h"

namespace srsran {
namespace srs_cu_cp {

/// Receiver of orbit-driven served beam updates.
class ntn_served_beam_update_handler
{
public:
  virtual ~ntn_served_beam_update_handler() = default;

  /// Apply the selected served beam ids. Returns true when the update is accepted.
  virtual bool update_ntn_served_beams(const std::vector<std::string>& beam_ids) = 0;

  /// Apply the selected served beam candidates. Returns true when the update is accepted.
  virtual bool update_ntn_served_beam_candidates(const std::vector<ntn_served_beam_candidate>& candidates);
};

/// Configuration for CU-CP NTN served beam scheduling.
struct ntn_served_beam_scheduler_config {
  std::vector<ntn_beam_position> beams;
  double                         min_elevation_deg    = 10.0;
  unsigned                       max_nof_served_beams = 1;
  bool                           served_beam_hopping_enabled = false;

  /// Number of accepted satellite-state update periods that one active CU-CP beam-set window should hold.
  unsigned served_beam_hopping_dwell_updates = 1;
};

/// Result of one served beam scheduling decision.
struct ntn_served_beam_schedule {
  std::vector<ntn_served_beam_candidate> candidates;
  std::vector<std::string>               beam_ids;
  bool                                   changed = false;
  bool                                   applied = false;
};

/// Converts satellite ECEF states into served beam updates for CU-CP mobility.
class ntn_served_beam_scheduler
{
public:
  explicit ntn_served_beam_scheduler(ntn_served_beam_scheduler_config cfg_);

  /// Computes the served beam set without changing scheduler state.
  ntn_served_beam_schedule compute_schedule(const ecef_coordinates_t& satellite) const;

  /// Computes and applies the served beam set if it differs from the last accepted update.
  ntn_served_beam_schedule update_from_satellite_state(const ecef_coordinates_t&        satellite,
                                                       ntn_served_beam_update_handler& update_handler);

  const std::vector<std::string>& current_served_beam_ids() const { return current_beam_ids; }

private:
  ntn_served_beam_scheduler_config cfg;
  std::vector<std::string>         current_beam_ids;
  std::vector<std::string>         current_visible_beam_ids;
  unsigned                         next_hopping_start_index = 0;
  unsigned                         current_hopping_dwell_updates = 0;
};

} // namespace srs_cu_cp
} // namespace srsran
