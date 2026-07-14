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

#include "ntn_beam_placement_planner.h"
#include <algorithm>
#include <map>
#include <unordered_set>

using namespace srsran;
using namespace srs_cu_cp;

namespace {

struct du_usage {
  ntn_du_beam_capacity capacity;
  unsigned             nof_active_beams = 0;
  unsigned             nof_ues          = 0;
  unsigned             nof_drbs         = 0;
};

bool supports_beam(const ntn_du_beam_capacity& capacity, const std::string& beam_id)
{
  return capacity.supported_beam_ids.empty() ||
         std::binary_search(capacity.supported_beam_ids.begin(), capacity.supported_beam_ids.end(), beam_id);
}

bool has_capacity_for(const du_usage& usage, const std::string& beam_id, const ntn_beam_load& load)
{
  const ntn_du_beam_capacity& capacity = usage.capacity;
  if (capacity.du_index == du_index_t::invalid || capacity.max_active_beams == 0 || !supports_beam(capacity, beam_id)) {
    return false;
  }
  if (usage.nof_active_beams >= capacity.max_active_beams) {
    return false;
  }
  if (capacity.max_ues != 0 && usage.nof_ues + load.nof_ues > capacity.max_ues) {
    return false;
  }
  if (capacity.max_drbs != 0 && usage.nof_drbs + load.nof_drbs > capacity.max_drbs) {
    return false;
  }
  return true;
}

bool has_global_active_beam_capacity(unsigned max_active_beams, unsigned nof_active_beams)
{
  return max_active_beams == 0 || nof_active_beams < max_active_beams;
}

void add_service_beam_to_usage(du_usage& usage, const ntn_beam_load& load)
{
  ++usage.nof_active_beams;
  usage.nof_ues += load.nof_ues;
  usage.nof_drbs += load.nof_drbs;
}

ntn_beam_load get_load(const std::map<std::string, ntn_beam_load>& loads, const std::string& beam_id)
{
  auto it = loads.find(beam_id);
  if (it != loads.end()) {
    return it->second;
  }
  return {beam_id, 0, 0};
}

ntn_beam_du_assignment make_assignment(const ntn_beam_position&       beam,
                                       ntn_beam_assignment_state     state,
                                       du_index_t                    du_index,
                                       double                        elevation_deg,
                                       const ntn_beam_load&          load,
                                       bool                          in_hopping_window = false)
{
  return {beam.beam_id, beam.nci, du_index, state, elevation_deg, load.nof_ues, load.nof_drbs, in_hopping_window};
}

bool has_higher_beam_priority(const std::string&                         lhs_beam_id,
                              const std::string&                         rhs_beam_id,
                              const std::map<std::string, ntn_beam_load>& loads,
                              const std::map<std::string, double>&        elevations)
{
  const ntn_beam_load lhs_load = get_load(loads, lhs_beam_id);
  const ntn_beam_load rhs_load = get_load(loads, rhs_beam_id);
  if (lhs_load.nof_ues != rhs_load.nof_ues) {
    return lhs_load.nof_ues > rhs_load.nof_ues;
  }
  if (lhs_load.nof_drbs != rhs_load.nof_drbs) {
    return lhs_load.nof_drbs > rhs_load.nof_drbs;
  }

  const double lhs_elevation = elevations.count(lhs_beam_id) != 0 ? elevations.at(lhs_beam_id) : 0.0;
  const double rhs_elevation = elevations.count(rhs_beam_id) != 0 ? elevations.at(rhs_beam_id) : 0.0;
  if (lhs_elevation != rhs_elevation) {
    return lhs_elevation > rhs_elevation;
  }
  return lhs_beam_id < rhs_beam_id;
}

bool is_better_du_choice(const du_usage& lhs, const du_usage& rhs)
{
  if (lhs.nof_active_beams != rhs.nof_active_beams) {
    return lhs.nof_active_beams < rhs.nof_active_beams;
  }
  if (lhs.nof_ues != rhs.nof_ues) {
    return lhs.nof_ues < rhs.nof_ues;
  }
  if (lhs.nof_drbs != rhs.nof_drbs) {
    return lhs.nof_drbs < rhs.nof_drbs;
  }
  return du_index_to_uint(lhs.capacity.du_index) < du_index_to_uint(rhs.capacity.du_index);
}

bool is_service_assignment(ntn_beam_assignment_state state)
{
  return state == ntn_beam_assignment_state::active || state == ntn_beam_assignment_state::draining;
}

bool has_user_demand(const ntn_beam_du_assignment& assignment)
{
  return assignment.nof_ues != 0 || assignment.nof_drbs != 0;
}

unsigned get_nof_user_driven_antenna_slots(const ntn_beam_du_assignment& assignment)
{
  if (!has_user_demand(assignment)) {
    return 0;
  }

  return assignment.nof_ues != 0 ? assignment.nof_ues : 1;
}

void assign_user_driven_antenna_slots(ntn_beam_placement_plan& plan)
{
  std::vector<unsigned> scheduled_assignment_indexes;
  scheduled_assignment_indexes.reserve(plan.assignments.size());
  unsigned antenna_slot_period = 0;
  for (unsigned i = 0; i != plan.assignments.size(); ++i) {
    auto& assignment = plan.assignments[i];
    assignment.antenna_slot_index  = 0;
    assignment.nof_antenna_slots   = 0;
    assignment.antenna_slot_period = 0;
    assignment.sr_slot_offset      = 0;
    assignment.sr_slot_period      = 0;
    assignment.srs_slot_offset     = 0;
    assignment.srs_slot_period     = 0;

    if (is_service_assignment(assignment.state) && has_user_demand(assignment)) {
      scheduled_assignment_indexes.push_back(i);
      antenna_slot_period += get_nof_user_driven_antenna_slots(assignment);
    }
  }

  std::sort(scheduled_assignment_indexes.begin(),
            scheduled_assignment_indexes.end(),
            [&plan](unsigned lhs_index, unsigned rhs_index) {
              const ntn_beam_du_assignment& lhs = plan.assignments[lhs_index];
              const ntn_beam_du_assignment& rhs = plan.assignments[rhs_index];
              if (lhs.nof_ues != rhs.nof_ues) {
                return lhs.nof_ues > rhs.nof_ues;
              }
              if (lhs.nof_drbs != rhs.nof_drbs) {
                return lhs.nof_drbs > rhs.nof_drbs;
              }
              if (lhs.elevation_deg != rhs.elevation_deg) {
                return lhs.elevation_deg > rhs.elevation_deg;
              }
              return lhs.beam_id < rhs.beam_id;
            });

  unsigned next_slot_index = 0;
  for (unsigned slot_index = 0; slot_index != scheduled_assignment_indexes.size(); ++slot_index) {
    ntn_beam_du_assignment& assignment = plan.assignments[scheduled_assignment_indexes[slot_index]];
    const unsigned          nof_slots  = get_nof_user_driven_antenna_slots(assignment);
    assignment.antenna_slot_index      = next_slot_index;
    assignment.nof_antenna_slots       = nof_slots;
    assignment.antenna_slot_period     = antenna_slot_period;
    assignment.sr_slot_offset          = next_slot_index;
    assignment.sr_slot_period          = antenna_slot_period;
    assignment.srs_slot_offset         = next_slot_index + nof_slots - 1;
    assignment.srs_slot_period         = antenna_slot_period;
    next_slot_index += nof_slots;
  }
}

ntn_beam_load make_draining_load(const ntn_beam_du_assignment&              previous_assignment,
                                 const std::map<std::string, ntn_beam_load>& loads)
{
  const auto load_it = loads.find(previous_assignment.beam_id);
  if (load_it != loads.end()) {
    return load_it->second;
  }

  if (previous_assignment.state == ntn_beam_assignment_state::active) {
    return {previous_assignment.beam_id, previous_assignment.nof_ues, previous_assignment.nof_drbs};
  }

  return {previous_assignment.beam_id, 0, 0};
}

bool should_keep_draining(const ntn_beam_du_assignment&              previous_assignment,
                          const std::map<std::string, ntn_beam_load>& loads)
{
  const ntn_beam_load load = make_draining_load(previous_assignment, loads);
  return load.nof_ues != 0 || load.nof_drbs != 0;
}

bool can_retain_draining_on_previous_du(const ntn_beam_du_assignment& previous_assignment,
                                        const std::map<du_index_t, du_usage>& du_usages)
{
  auto du_it = du_usages.find(previous_assignment.du_index);
  if (du_it == du_usages.end()) {
    return false;
  }

  return du_it->second.capacity.max_active_beams != 0 &&
         supports_beam(du_it->second.capacity, previous_assignment.beam_id);
}

} // namespace

std::vector<std::string> srsran::srs_cu_cp::get_active_ntn_beam_ids(
    const ntn_beam_placement_plan&                plan,
    const std::vector<ntn_served_beam_candidate>& visible_beams)
{
  std::vector<std::string> active_beam_ids;
  active_beam_ids.reserve(visible_beams.size());
  std::unordered_set<std::string> active_beams;
  for (const auto& assignment : plan.assignments) {
    if (assignment.state == ntn_beam_assignment_state::active) {
      active_beams.insert(assignment.beam_id);
    }
  }
  for (const auto& candidate : visible_beams) {
    if (active_beams.count(candidate.beam_id) != 0) {
      active_beam_ids.push_back(candidate.beam_id);
    }
  }
  return active_beam_ids;
}

ntn_beam_placement_plan ntn_beam_placement_planner::plan(const ntn_beam_placement_request& request) const
{
  std::map<std::string, ntn_beam_position> enabled_beams;
  for (const auto& beam : request.beams) {
    if (beam.enabled && !beam.beam_id.empty()) {
      enabled_beams[beam.beam_id] = beam;
    }
  }

  std::map<std::string, double> visible_elevations;
  std::map<std::string, bool>   visible_in_hopping_window;
  for (const auto& candidate : request.visible_beams) {
    if (enabled_beams.count(candidate.beam_id) == 0) {
      continue;
    }
    auto it = visible_elevations.find(candidate.beam_id);
    if (it == visible_elevations.end() || candidate.elevation_deg > it->second) {
      visible_elevations[candidate.beam_id] = candidate.elevation_deg;
    }
    visible_in_hopping_window[candidate.beam_id] =
        visible_in_hopping_window[candidate.beam_id] || candidate.in_hopping_window;
  }

  std::map<std::string, ntn_beam_load> loads;
  for (const auto& load : request.beam_loads) {
    if (load.beam_id.empty()) {
      continue;
    }
    ntn_beam_load& entry = loads[load.beam_id];
    entry.beam_id = load.beam_id;
    entry.nof_ues += load.nof_ues;
    entry.nof_drbs += load.nof_drbs;
  }

  std::map<du_index_t, du_usage> du_usages;
  for (const auto& capacity : request.du_capacities) {
    if (capacity.du_index == du_index_t::invalid) {
      continue;
    }
    du_usage usage{capacity, 0, capacity.current_ues, capacity.current_drbs};
    std::sort(usage.capacity.supported_beam_ids.begin(), usage.capacity.supported_beam_ids.end());
    du_usages[capacity.du_index] = std::move(usage);
  }

  std::map<std::string, ntn_beam_du_assignment> previous_service_assignments;
  for (const auto& assignment : request.previous_assignments) {
    if (is_service_assignment(assignment.state) && !assignment.beam_id.empty()) {
      previous_service_assignments[assignment.beam_id] = assignment;
    }
  }

  std::map<std::string, ntn_beam_du_assignment> planned_by_beam_id;
  unsigned                                      nof_active_beams = 0;
  for (const auto& entry : previous_service_assignments) {
    const std::string&                 beam_id             = entry.first;
    const ntn_beam_du_assignment& previous_assignment = entry.second;
    if (enabled_beams.count(beam_id) == 0 || !should_keep_draining(previous_assignment, loads)) {
      continue;
    }

    const bool beam_is_visible           = visible_elevations.count(beam_id) != 0;
    const bool beam_is_in_hopping_window = beam_is_visible && visible_in_hopping_window[beam_id];
    if (previous_assignment.state == ntn_beam_assignment_state::active && beam_is_in_hopping_window) {
      continue;
    }
    if (!can_retain_draining_on_previous_du(previous_assignment, du_usages)) {
      continue;
    }

    auto          du_it = du_usages.find(previous_assignment.du_index);
    ntn_beam_load load  = make_draining_load(previous_assignment, loads);
    add_service_beam_to_usage(du_it->second, load);
    planned_by_beam_id[beam_id] = make_assignment(enabled_beams.at(beam_id),
                                                  ntn_beam_assignment_state::draining,
                                                  previous_assignment.du_index,
                                                  beam_is_visible ? visible_elevations.at(beam_id)
                                                                  : previous_assignment.elevation_deg,
                                                  load,
                                                  false);
  }

  std::vector<std::string> unassigned_visible_beam_ids;
  unassigned_visible_beam_ids.reserve(visible_elevations.size());
  for (const auto& entry : visible_elevations) {
    if (planned_by_beam_id.count(entry.first) == 0) {
      unassigned_visible_beam_ids.push_back(entry.first);
    }
  }
  std::sort(unassigned_visible_beam_ids.begin(),
            unassigned_visible_beam_ids.end(),
            [&loads, &visible_elevations](const std::string& lhs, const std::string& rhs) {
              return has_higher_beam_priority(lhs, rhs, loads, visible_elevations);
            });

  for (const std::string& beam_id : unassigned_visible_beam_ids) {
    const ntn_beam_load load = get_load(loads, beam_id);

    if (!visible_in_hopping_window[beam_id]) {
      planned_by_beam_id[beam_id] = make_assignment(enabled_beams.at(beam_id),
                                                    ntn_beam_assignment_state::candidate,
                                                    du_index_t::invalid,
                                                    visible_elevations.at(beam_id),
                                                    load,
                                                    false);
      continue;
    }

    auto best_du_it = du_usages.end();
    if (has_global_active_beam_capacity(request.max_active_beams, nof_active_beams)) {
      const auto previous_it = previous_service_assignments.find(beam_id);
      if (previous_it != previous_service_assignments.end()) {
        auto previous_du_it = du_usages.find(previous_it->second.du_index);
        if (previous_du_it != du_usages.end() && has_capacity_for(previous_du_it->second, beam_id, load)) {
          best_du_it = previous_du_it;
        }
      }
      if (best_du_it == du_usages.end()) {
        for (auto du_it = du_usages.begin(); du_it != du_usages.end(); ++du_it) {
          if (!has_capacity_for(du_it->second, beam_id, load)) {
            continue;
          }
          if (best_du_it == du_usages.end() || is_better_du_choice(du_it->second, best_du_it->second)) {
            best_du_it = du_it;
          }
        }
      }
    }

    if (best_du_it == du_usages.end()) {
      planned_by_beam_id[beam_id] = make_assignment(enabled_beams.at(beam_id),
                                                    ntn_beam_assignment_state::candidate,
                                                    du_index_t::invalid,
                                                    visible_elevations.at(beam_id),
                                                    load,
                                                    true);
      continue;
    }

    add_service_beam_to_usage(best_du_it->second, load);
    ++nof_active_beams;
    planned_by_beam_id[beam_id] = make_assignment(enabled_beams.at(beam_id),
                                                  ntn_beam_assignment_state::active,
                                                  best_du_it->first,
                                                  visible_elevations.at(beam_id),
                                                  load,
                                                  true);
  }

  ntn_beam_placement_plan result;
  result.assignments.reserve(enabled_beams.size());
  for (const auto& entry : enabled_beams) {
    auto planned_it = planned_by_beam_id.find(entry.first);
    if (planned_it != planned_by_beam_id.end()) {
      result.assignments.push_back(planned_it->second);
      continue;
    }
    result.assignments.push_back(make_assignment(
        entry.second, ntn_beam_assignment_state::inactive, du_index_t::invalid, 0.0, get_load(loads, entry.first)));
  }

  assign_user_driven_antenna_slots(result);

  return result;
}
