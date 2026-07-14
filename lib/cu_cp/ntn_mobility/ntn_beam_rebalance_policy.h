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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * A copy of the GNU Affero General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */

#pragma once

#include "ntn_beam_placement_planner.h"
#include "ntn_served_beam_selector.h"
#include "srsran/cu_cp/ntn_service_switch_over.h"
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace srsran {
namespace srs_cu_cp {

struct ntn_rebalance_pending_handover {
  std::string      source_beam_id;
  std::string      target_beam_id;
  std::string      source_analog_beam_id;
  std::string      target_analog_beam_id;
  du_index_t       target_du_index = du_index_t::invalid;
  unsigned         nof_drbs        = 0;
};

struct ntn_rebalance_preheat_status {
  std::string beam_id;
  bool        applied = false;
  bool        ready   = false;
};

struct ntn_rebalance_policy_request {
  std::vector<ntn_beam_position>          beams;
  ntn_beam_placement_plan                 plan;
  std::vector<ntn_served_beam_candidate>  served_beam_candidates;
  std::vector<std::string>                mobility_eligible_beam_ids;
  bool                                    predictive_window_valid = false;
  std::set<std::string>                   predictive_drain_soon_beam_ids;
  ntn_service_switch_over_snapshot        switch_over_snapshot;
  std::vector<ntn_du_beam_capacity>       du_capacities;
  std::vector<ntn_rebalance_pending_handover> pending_handovers;
  std::vector<ntn_rebalance_preheat_status>   preheat_states;
  std::set<std::string>                   active_analog_pair_cooldowns;
  std::string                             source_beam_id;
  std::optional<nr_cell_identity>         source_nci;
  unsigned                                moving_nof_drbs = 0;
  unsigned                                min_ue_delta    = 2;
  bool                                    analog_access_policy_configured = false;
};

struct ntn_rebalance_target_selection {
  std::optional<ntn_beam_du_assignment> assignment;
  bool                                  same_analog = false;
  bool                                  skipped_projected_capacity = false;
  bool                                  skipped_cold_analog = false;
  bool                                  skipped_pair_cooldown = false;
  bool                                  skipped_preheat_ready_guard = false;
  std::string                           reason = "no_eligible_target";
  std::string                           source_analog_beam_id = "none";
  std::string                           target_analog_beam_id = "none";
  std::string                           preheat_beam_id = "none";
  std::string                           preheat_analog_beam_id = "none";
};

std::string make_ntn_analog_rebalance_pair_key(const std::string& source_analog_beam_id,
                                               const std::string& target_analog_beam_id);

ntn_rebalance_target_selection select_ntn_rebalance_target(const ntn_rebalance_policy_request& request);

bool is_ntn_rebalance_source_hot(const ntn_rebalance_policy_request&    request,
                                 const ntn_rebalance_target_selection& selection);

} // namespace srs_cu_cp
} // namespace srsran
