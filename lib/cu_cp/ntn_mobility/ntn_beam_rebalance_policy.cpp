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

#include "ntn_beam_rebalance_policy.h"
#include <algorithm>
#include <limits>

using namespace srsran;
using namespace srs_cu_cp;

namespace {

struct projected_load {
  unsigned nof_ues  = 0;
  unsigned nof_drbs = 0;
};

bool contains_beam_id(const std::vector<std::string>& beam_ids, const std::string& beam_id)
{
  return std::find(beam_ids.begin(), beam_ids.end(), beam_id) != beam_ids.end();
}

bool contains_beam_id(const std::set<std::string>& beam_ids, const std::string& beam_id)
{
  return beam_ids.find(beam_id) != beam_ids.end();
}

const ntn_beam_position* find_beam_cfg(const std::vector<ntn_beam_position>& beams, const std::string& beam_id)
{
  auto it = std::find_if(beams.begin(), beams.end(), [&beam_id](const ntn_beam_position& beam) {
    return beam.beam_id == beam_id;
  });
  return it != beams.end() ? &*it : nullptr;
}

std::string get_analog_beam_id(const ntn_rebalance_policy_request& request, const std::string& beam_id)
{
  const ntn_beam_position* beam = find_beam_cfg(request.beams, beam_id);
  return beam != nullptr ? beam->analog_beam_id : std::string{};
}

const ntn_beam_du_assignment* find_assignment(const ntn_beam_placement_plan& plan, const std::string& beam_id)
{
  auto it = std::find_if(plan.assignments.begin(), plan.assignments.end(), [&beam_id](const ntn_beam_du_assignment& assignment) {
    return assignment.beam_id == beam_id;
  });
  return it != plan.assignments.end() ? &*it : nullptr;
}

unsigned get_served_candidate_order(const std::vector<ntn_served_beam_candidate>& candidates,
                                    const std::string&                            beam_id)
{
  for (unsigned i = 0; i != candidates.size(); ++i) {
    if (candidates[i].beam_id == beam_id) {
      return i;
    }
  }
  return std::numeric_limits<unsigned>::max();
}

ntn_service_beam_policy get_beam_service_policy(const ntn_service_switch_over_snapshot& switch_over_snapshot,
                                                const std::string&                      beam_id,
                                                nr_cell_identity                        nci)
{
  const auto beam_it =
      std::find_if(switch_over_snapshot.beams.begin(),
                   switch_over_snapshot.beams.end(),
                   [&beam_id, nci](const ntn_service_switch_over_beam_snapshot& beam) {
                     return beam.beam_id == beam_id || beam.nci.value() == nci.value();
                   });
  return beam_it != switch_over_snapshot.beams.end() ? beam_it->policy : ntn_service_beam_policy::normal;
}

bool service_policy_blocks_new_demand(ntn_service_beam_policy policy)
{
  return policy == ntn_service_beam_policy::block_new_demand || policy == ntn_service_beam_policy::drain ||
         policy == ntn_service_beam_policy::release_allowed;
}

projected_load get_pending_target_load(const ntn_rebalance_policy_request& request, const std::string& beam_id)
{
  projected_load load;
  for (const ntn_rebalance_pending_handover& handover : request.pending_handovers) {
    if (handover.target_beam_id != beam_id) {
      continue;
    }
    ++load.nof_ues;
    load.nof_drbs += handover.nof_drbs;
  }
  return load;
}

projected_load get_current_analog_load(const ntn_rebalance_policy_request& request, const std::string& analog_beam_id)
{
  projected_load load;
  if (analog_beam_id.empty()) {
    return load;
  }
  for (const ntn_beam_du_assignment& assignment : request.plan.assignments) {
    if (get_analog_beam_id(request, assignment.beam_id) != analog_beam_id) {
      continue;
    }
    load.nof_ues += assignment.nof_ues;
    load.nof_drbs += assignment.nof_drbs;
  }
  return load;
}

projected_load get_pending_incoming_analog_load(const ntn_rebalance_policy_request& request,
                                                const std::string&                  analog_beam_id)
{
  projected_load load;
  if (analog_beam_id.empty()) {
    return load;
  }
  for (const ntn_rebalance_pending_handover& handover : request.pending_handovers) {
    std::string target_analog = handover.target_analog_beam_id;
    if (target_analog.empty()) {
      target_analog = get_analog_beam_id(request, handover.target_beam_id);
    }
    if (target_analog != analog_beam_id) {
      continue;
    }
    std::string source_analog = handover.source_analog_beam_id;
    if (source_analog.empty()) {
      source_analog = get_analog_beam_id(request, handover.source_beam_id);
    }
    if (source_analog == target_analog) {
      continue;
    }
    ++load.nof_ues;
    load.nof_drbs += handover.nof_drbs;
  }
  return load;
}

bool is_analog_active_or_preheated(const ntn_rebalance_policy_request& request, const std::string& analog_beam_id)
{
  if (analog_beam_id.empty()) {
    return true;
  }
  for (const ntn_beam_du_assignment& assignment : request.plan.assignments) {
    if (get_analog_beam_id(request, assignment.beam_id) == analog_beam_id &&
        assignment.access_du_index != du_index_t::invalid && assignment.access_du_reason == "eligible") {
      return true;
    }
  }
  return false;
}

const ntn_rebalance_preheat_status* find_preheat_status(const ntn_rebalance_policy_request& request,
                                                        const std::string&                  beam_id)
{
  auto it = std::find_if(request.preheat_states.begin(),
                         request.preheat_states.end(),
                         [&beam_id](const ntn_rebalance_preheat_status& state) { return state.beam_id == beam_id; });
  return it != request.preheat_states.end() ? &*it : nullptr;
}

bool du_capacity_allows(const ntn_rebalance_policy_request& request,
                        const ntn_beam_du_assignment&       assignment,
                        const ntn_beam_du_assignment*       source_assignment)
{
  auto capacity_it =
      std::find_if(request.du_capacities.begin(), request.du_capacities.end(), [&](const ntn_du_beam_capacity& capacity) {
        return capacity.du_index == assignment.du_index;
      });
  if (capacity_it == request.du_capacities.end()) {
    return false;
  }
  if (capacity_it->max_ues == 0 && capacity_it->max_drbs == 0) {
    return true;
  }

  projected_load load{capacity_it->current_ues, capacity_it->current_drbs};
  for (const ntn_beam_du_assignment& beam_assignment : request.plan.assignments) {
    if (beam_assignment.du_index != assignment.du_index) {
      continue;
    }
    load.nof_ues += beam_assignment.nof_ues;
    load.nof_drbs += beam_assignment.nof_drbs;
  }
  for (const ntn_rebalance_pending_handover& handover : request.pending_handovers) {
    if (handover.target_du_index != assignment.du_index) {
      continue;
    }
    ++load.nof_ues;
    load.nof_drbs += handover.nof_drbs;
  }
  if (source_assignment == nullptr || source_assignment->du_index != assignment.du_index) {
    ++load.nof_ues;
    load.nof_drbs += request.moving_nof_drbs;
  }
  if (capacity_it->max_ues != 0 && load.nof_ues > capacity_it->max_ues) {
    return false;
  }
  if (capacity_it->max_drbs != 0 && load.nof_drbs > capacity_it->max_drbs) {
    return false;
  }
  return true;
}

bool projected_capacity_allows(const ntn_rebalance_policy_request& request,
                               const ntn_beam_du_assignment&       assignment,
                               const ntn_beam_du_assignment*       source_assignment,
                               const std::string&                  source_analog_beam_id)
{
  const projected_load pending_target_load = get_pending_target_load(request, assignment.beam_id);
  const unsigned       projected_digital_ues = assignment.nof_ues + pending_target_load.nof_ues + 1U;
  const unsigned       projected_digital_drbs = assignment.nof_drbs + pending_target_load.nof_drbs + request.moving_nof_drbs;
  if (assignment.digital_ue_cap != 0 && projected_digital_ues > assignment.digital_ue_cap) {
    return false;
  }
  if (assignment.digital_drb_cap != 0 && projected_digital_drbs > assignment.digital_drb_cap) {
    return false;
  }

  const std::string target_analog_beam_id = get_analog_beam_id(request, assignment.beam_id);
  if (!target_analog_beam_id.empty()) {
    projected_load       projected_analog_load = get_current_analog_load(request, target_analog_beam_id);
    const projected_load pending_analog_load   = get_pending_incoming_analog_load(request, target_analog_beam_id);
    projected_analog_load.nof_ues += pending_analog_load.nof_ues;
    projected_analog_load.nof_drbs += pending_analog_load.nof_drbs;
    if (target_analog_beam_id != source_analog_beam_id) {
      ++projected_analog_load.nof_ues;
      projected_analog_load.nof_drbs += request.moving_nof_drbs;
    }
    if (assignment.analog_service_bound_ue_cap != 0 &&
        projected_analog_load.nof_ues > assignment.analog_service_bound_ue_cap) {
      return false;
    }
    if (assignment.analog_drb_cap != 0 && projected_analog_load.nof_drbs > assignment.analog_drb_cap) {
      return false;
    }
  }

  return du_capacity_allows(request, assignment, source_assignment);
}

bool is_source_analog_at_capacity(const ntn_rebalance_policy_request& request,
                                  const ntn_beam_du_assignment*       source_assignment,
                                  const std::string&                  source_analog_beam_id)
{
  if (source_assignment == nullptr) {
    return false;
  }
  const projected_load source_analog_load = get_current_analog_load(request, source_analog_beam_id);
  return (source_assignment->analog_service_bound_ue_cap != 0 &&
          source_analog_load.nof_ues >= source_assignment->analog_service_bound_ue_cap) ||
         (source_assignment->analog_drb_cap != 0 &&
          source_analog_load.nof_drbs >= source_assignment->analog_drb_cap);
}

} // namespace

std::string srs_cu_cp::make_ntn_analog_rebalance_pair_key(const std::string& source_analog_beam_id,
                                                          const std::string& target_analog_beam_id)
{
  return source_analog_beam_id + "->" + target_analog_beam_id;
}

ntn_rebalance_target_selection srs_cu_cp::select_ntn_rebalance_target(const ntn_rebalance_policy_request& request)
{
  const std::string source_analog_beam_id = get_analog_beam_id(request, request.source_beam_id);
  const ntn_beam_du_assignment* source_assignment = find_assignment(request.plan, request.source_beam_id);

  ntn_rebalance_target_selection selection;
  selection.source_analog_beam_id = source_analog_beam_id.empty() ? "none" : source_analog_beam_id;

  const bool source_analog_at_capacity =
      is_source_analog_at_capacity(request, source_assignment, source_analog_beam_id);

  auto is_assignment_eligible = [&](const ntn_beam_du_assignment& assignment) {
    if (assignment.beam_id.empty() || assignment.beam_id == request.source_beam_id) {
      return false;
    }
    if (request.source_nci.has_value() && assignment.nci.value() == request.source_nci->value()) {
      return false;
    }
    if (!contains_beam_id(request.mobility_eligible_beam_ids, assignment.beam_id)) {
      return false;
    }
    if (request.predictive_window_valid && contains_beam_id(request.predictive_drain_soon_beam_ids, assignment.beam_id)) {
      return false;
    }
    if (!assignment.downlink_enabled || !assignment.uplink_enabled) {
      return false;
    }
    if (service_policy_blocks_new_demand(
            get_beam_service_policy(request.switch_over_snapshot, assignment.beam_id, assignment.nci))) {
      return false;
    }
    const bool active_loaded = assignment.state == ntn_beam_assignment_state::active_loaded;
    const bool candidate_in_window =
        assignment.state == ntn_beam_assignment_state::candidate && assignment.in_hopping_window;
    if (!active_loaded && !candidate_in_window) {
      return false;
    }

    const std::string target_analog_beam_id = get_analog_beam_id(request, assignment.beam_id);
    if (const ntn_rebalance_preheat_status* preheat = find_preheat_status(request, assignment.beam_id);
        preheat != nullptr && preheat->applied && !preheat->ready && target_analog_beam_id != source_analog_beam_id) {
      selection.skipped_preheat_ready_guard = true;
      selection.preheat_beam_id            = assignment.beam_id;
      selection.preheat_analog_beam_id     = target_analog_beam_id.empty() ? "none" : target_analog_beam_id;
      if (selection.reason == "no_eligible_target") {
        selection.reason = "preheat_ready_guard";
      }
      return false;
    }
    if (target_analog_beam_id != source_analog_beam_id && !is_analog_active_or_preheated(request, target_analog_beam_id)) {
      selection.skipped_cold_analog    = true;
      selection.preheat_beam_id        = assignment.beam_id;
      selection.preheat_analog_beam_id = target_analog_beam_id.empty() ? "none" : target_analog_beam_id;
      if (selection.reason == "no_eligible_target") {
        selection.reason = "cold_analog";
      }
      return false;
    }
    if (assignment.du_index == du_index_t::invalid || assignment.du_assignment_reason != "eligible") {
      return false;
    }
    if (request.analog_access_policy_configured && assignment.access_du_index == du_index_t::invalid) {
      return false;
    }
    if (!assignment.resource_domain_eligible) {
      return false;
    }
    if (target_analog_beam_id != source_analog_beam_id &&
        contains_beam_id(request.active_analog_pair_cooldowns,
                         make_ntn_analog_rebalance_pair_key(source_analog_beam_id, target_analog_beam_id))) {
      selection.skipped_pair_cooldown = true;
      if (selection.reason == "no_eligible_target") {
        selection.reason = "pair_cooldown";
      }
      return false;
    }
    if (!projected_capacity_allows(request, assignment, source_assignment, source_analog_beam_id)) {
      selection.skipped_projected_capacity = true;
      selection.reason = "projected_capacity";
      return false;
    }
    return true;
  };

  auto has_same_source_analog = [&](const ntn_beam_du_assignment& assignment) {
    if (source_analog_beam_id.empty()) {
      return false;
    }
    return get_analog_beam_id(request, assignment.beam_id) == source_analog_beam_id;
  };

  auto is_better_target = [&](const ntn_beam_du_assignment& candidate, const ntn_beam_du_assignment& current) {
    const bool candidate_same_analog = has_same_source_analog(candidate);
    const bool current_same_analog   = has_same_source_analog(current);
    if (candidate_same_analog != current_same_analog) {
      return source_analog_at_capacity ? !candidate_same_analog : candidate_same_analog;
    }

    const projected_load candidate_pending_load = get_pending_target_load(request, candidate.beam_id);
    const projected_load current_pending_load   = get_pending_target_load(request, current.beam_id);
    const unsigned       candidate_projected_ues = candidate.nof_ues + candidate_pending_load.nof_ues;
    const unsigned       current_projected_ues   = current.nof_ues + current_pending_load.nof_ues;
    if (candidate_projected_ues != current_projected_ues) {
      return candidate_projected_ues < current_projected_ues;
    }
    const unsigned candidate_projected_drbs = candidate.nof_drbs + candidate_pending_load.nof_drbs;
    const unsigned current_projected_drbs   = current.nof_drbs + current_pending_load.nof_drbs;
    if (candidate_projected_drbs != current_projected_drbs) {
      return candidate_projected_drbs < current_projected_drbs;
    }

    const bool candidate_active = candidate.state == ntn_beam_assignment_state::active_loaded;
    const bool current_active   = current.state == ntn_beam_assignment_state::active_loaded;
    if (candidate_active != current_active) {
      return candidate_active;
    }
    if (candidate.elevation_deg != current.elevation_deg) {
      return candidate.elevation_deg > current.elevation_deg;
    }

    const unsigned candidate_order = get_served_candidate_order(request.served_beam_candidates, candidate.beam_id);
    const unsigned current_order   = get_served_candidate_order(request.served_beam_candidates, current.beam_id);
    if (candidate_order != current_order) {
      return candidate_order < current_order;
    }
    return candidate.beam_id < current.beam_id;
  };

  for (const ntn_beam_du_assignment& assignment : request.plan.assignments) {
    if (!is_assignment_eligible(assignment)) {
      continue;
    }
    if (!selection.assignment.has_value() || is_better_target(assignment, selection.assignment.value())) {
      selection.assignment = assignment;
    }
  }

  if (!selection.assignment.has_value() && !source_analog_beam_id.empty()) {
    for (const ntn_served_beam_candidate& candidate : request.served_beam_candidates) {
      if (candidate.beam_id.empty() || candidate.beam_id == request.source_beam_id) {
        continue;
      }
      const std::string candidate_analog_beam_id = get_analog_beam_id(request, candidate.beam_id);
      if (candidate_analog_beam_id.empty() || candidate_analog_beam_id == source_analog_beam_id) {
        continue;
      }
      if (!is_analog_active_or_preheated(request, candidate_analog_beam_id)) {
        selection.skipped_cold_analog       = true;
        selection.reason                    = "cold_analog";
        selection.preheat_beam_id           = candidate.beam_id;
        selection.preheat_analog_beam_id    = candidate_analog_beam_id;
        break;
      }
    }
  }

  if (selection.assignment.has_value()) {
    selection.reason = "eligible";
    selection.target_analog_beam_id = get_analog_beam_id(request, selection.assignment->beam_id);
    if (selection.target_analog_beam_id.empty()) {
      selection.target_analog_beam_id = "none";
    }
    selection.same_analog = !source_analog_beam_id.empty() && selection.target_analog_beam_id == source_analog_beam_id;
  }

  return selection;
}

bool srs_cu_cp::is_ntn_rebalance_source_hot(const ntn_rebalance_policy_request&    request,
                                            const ntn_rebalance_target_selection& selection)
{
  const ntn_beam_du_assignment* source_assignment = find_assignment(request.plan, request.source_beam_id);
  if (source_assignment == nullptr || source_assignment->state != ntn_beam_assignment_state::active_loaded) {
    return false;
  }

  const bool source_at_ue_cap =
      source_assignment->digital_ue_cap != 0 && source_assignment->digital_ue_load >= source_assignment->digital_ue_cap;
  const bool source_at_drb_cap =
      source_assignment->digital_drb_cap != 0 && source_assignment->digital_drb_load >= source_assignment->digital_drb_cap;

  const std::string  source_analog_beam_id = get_analog_beam_id(request, request.source_beam_id);
  const projected_load source_analog_load  = get_current_analog_load(request, source_analog_beam_id);
  const bool source_analog_at_ue_cap = source_assignment->analog_service_bound_ue_cap != 0 &&
                                       source_analog_load.nof_ues >= source_assignment->analog_service_bound_ue_cap;
  const bool source_analog_at_drb_cap =
      source_assignment->analog_drb_cap != 0 && source_analog_load.nof_drbs >= source_assignment->analog_drb_cap;

  const bool digital_hot_without_target =
      source_assignment->nof_ues >= request.min_ue_delta || source_at_ue_cap || source_at_drb_cap;
  if (!selection.assignment.has_value()) {
    return digital_hot_without_target || source_analog_at_ue_cap || source_analog_at_drb_cap;
  }

  const ntn_beam_du_assignment& target_assignment = selection.assignment.value();
  if (source_assignment->nof_ues <= target_assignment.nof_ues) {
    const std::string target_analog_beam_id =
        selection.target_analog_beam_id == "none" ? std::string{} : selection.target_analog_beam_id;
    if (!source_analog_beam_id.empty() && target_analog_beam_id != source_analog_beam_id) {
      const projected_load target_analog_load = get_current_analog_load(request, target_analog_beam_id);
      if (source_analog_load.nof_ues > target_analog_load.nof_ues) {
        const unsigned analog_delta = source_analog_load.nof_ues - target_analog_load.nof_ues;
        return analog_delta >= request.min_ue_delta || source_analog_at_ue_cap || source_analog_at_drb_cap;
      }
      return source_analog_at_ue_cap || source_analog_at_drb_cap;
    }
    return false;
  }

  const unsigned ue_delta = source_assignment->nof_ues - target_assignment.nof_ues;
  if (ue_delta >= request.min_ue_delta || source_at_ue_cap || source_at_drb_cap) {
    return true;
  }

  const std::string target_analog_beam_id =
      selection.target_analog_beam_id == "none" ? std::string{} : selection.target_analog_beam_id;
  if (!source_analog_beam_id.empty() && target_analog_beam_id != source_analog_beam_id) {
    const projected_load target_analog_load = get_current_analog_load(request, target_analog_beam_id);
    if (source_analog_load.nof_ues > target_analog_load.nof_ues) {
      const unsigned analog_delta = source_analog_load.nof_ues - target_analog_load.nof_ues;
      return analog_delta >= request.min_ue_delta || source_analog_at_ue_cap || source_analog_at_drb_cap;
    }
    return source_analog_at_ue_cap || source_analog_at_drb_cap;
  }
  return false;
}
