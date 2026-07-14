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

#include "ntn_served_beam_scheduler.h"
#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <utility>

using namespace srsran;
using namespace srs_cu_cp;

namespace {

std::vector<std::string> get_visible_beam_membership_ids(const std::vector<ntn_served_beam_candidate>& candidates)
{
  std::vector<std::string> visible_beam_ids;
  visible_beam_ids.reserve(candidates.size());
  for (const auto& candidate : candidates) {
    visible_beam_ids.push_back(candidate.beam_id);
  }
  std::sort(visible_beam_ids.begin(), visible_beam_ids.end());
  return visible_beam_ids;
}

std::vector<std::string> get_visible_beam_owner_ids(const std::vector<ntn_served_beam_candidate>& candidates)
{
  std::vector<std::string> visible_owner_ids;
  visible_owner_ids.reserve(candidates.size());
  for (const auto& candidate : candidates) {
    visible_owner_ids.push_back(candidate.beam_id + "|" + candidate.serving_satellite_id);
  }
  std::sort(visible_owner_ids.begin(), visible_owner_ids.end());
  return visible_owner_ids;
}

std::map<std::string, ntn_served_beam_demand>
build_demand_by_beam(const std::vector<ntn_served_beam_demand>& demands)
{
  std::map<std::string, ntn_served_beam_demand> result;
  for (const ntn_served_beam_demand& demand : demands) {
    if (demand.beam_id.empty()) {
      continue;
    }
    ntn_served_beam_demand& entry = result[demand.beam_id];
    entry.beam_id = demand.beam_id;
    entry.nof_ues += demand.nof_ues;
    entry.nof_drbs += demand.nof_drbs;
    entry.nof_pending_handovers += demand.nof_pending_handovers;
    if (!entry.qos.has_qos_demand || is_ntn_qos_demand_higher_priority(demand.qos, entry.qos)) {
      entry.qos = demand.qos;
    } else if (demand.qos.has_qos_demand) {
      entry.qos.nof_qos_flows += demand.qos.nof_qos_flows;
    }
  }
  return result;
}

const ntn_served_beam_demand* find_demand(const std::map<std::string, ntn_served_beam_demand>& demands,
                                          const std::string&                                  beam_id)
{
  auto it = demands.find(beam_id);
  return it != demands.end() ? &it->second : nullptr;
}

bool has_demand(const ntn_served_beam_demand* demand)
{
  return demand != nullptr &&
         (demand->nof_ues != 0 || demand->nof_drbs != 0 || demand->nof_pending_handovers != 0 ||
          demand->qos.has_qos_demand);
}

unsigned qos_score(const ntn_qos_demand_summary& qos)
{
  if (!qos.has_qos_demand) {
    return 0;
  }

  unsigned score = 1;
  if (qos.has_delay_critical_gbr) {
    score += 10000;
  }
  if (qos.has_gbr) {
    score += 5000;
  }
  if (qos.may_trigger_preemption) {
    score += 2000;
  }
  score += (15U - std::min(qos.best_arp_priority, 15U)) * 100U;
  score += (127U - std::min(qos.best_qos_priority, 127U));
  score += qos.nof_qos_flows;
  return score;
}

unsigned demand_score(const ntn_served_beam_demand* demand)
{
  if (!has_demand(demand)) {
    return 0;
  }
  return demand->nof_ues * 100000U + demand->nof_drbs * 1000U + demand->nof_pending_handovers * 10000U +
         qos_score(demand->qos);
}

bool demand_is_better(const ntn_served_beam_demand* lhs,
                      const ntn_served_beam_demand* rhs,
                      const ntn_served_beam_candidate& lhs_candidate,
                      const ntn_served_beam_candidate& rhs_candidate)
{
  const bool lhs_has_demand = has_demand(lhs);
  const bool rhs_has_demand = has_demand(rhs);
  if (lhs_has_demand != rhs_has_demand) {
    return lhs_has_demand;
  }
  if (lhs_has_demand) {
    if (lhs->nof_ues != rhs->nof_ues) {
      return lhs->nof_ues > rhs->nof_ues;
    }
    if (lhs->nof_drbs != rhs->nof_drbs) {
      return lhs->nof_drbs > rhs->nof_drbs;
    }
    if (lhs->nof_pending_handovers != rhs->nof_pending_handovers) {
      return lhs->nof_pending_handovers > rhs->nof_pending_handovers;
    }
    if (is_ntn_qos_demand_higher_priority(lhs->qos, rhs->qos)) {
      return true;
    }
    if (is_ntn_qos_demand_higher_priority(rhs->qos, lhs->qos)) {
      return false;
    }
    const unsigned lhs_score = demand_score(lhs);
    const unsigned rhs_score = demand_score(rhs);
    if (lhs_score != rhs_score) {
      return lhs_score > rhs_score;
    }
  }
  if (lhs_candidate.elevation_deg != rhs_candidate.elevation_deg) {
    return lhs_candidate.elevation_deg > rhs_candidate.elevation_deg;
  }
  return lhs_candidate.beam_id < rhs_candidate.beam_id;
}

bool has_any_demand(const std::map<std::string, ntn_served_beam_demand>& demands)
{
  return std::any_of(demands.begin(), demands.end(), [](const auto& entry) { return has_demand(&entry.second); });
}

void apply_hopping_window(ntn_served_beam_schedule&                              schedule,
                          const std::vector<std::string>&                        beam_ids,
                          const std::map<std::string, ntn_served_beam_demand>& demands)
{
  schedule.beam_ids.clear();
  for (auto& candidate : schedule.candidates) {
    candidate.in_hopping_window = false;
    candidate.window_rank        = std::numeric_limits<unsigned>::max();
    candidate.scheduling_score   = demand_score(find_demand(demands, candidate.beam_id));
    candidate.scheduling_reason  = "not_selected";
  }

  unsigned rank = 0;
  for (const auto& beam_id : beam_ids) {
    auto it = std::find_if(schedule.candidates.begin(),
                           schedule.candidates.end(),
                           [&beam_id](const ntn_served_beam_candidate& candidate) {
                             return candidate.beam_id == beam_id;
                           });
    if (it == schedule.candidates.end()) {
      continue;
    }
    it->in_hopping_window = true;
    it->window_rank        = rank++;
    const ntn_served_beam_demand* demand = find_demand(demands, beam_id);
    it->scheduling_reason = has_demand(demand) ? "demand" : "legacy";
    schedule.beam_ids.push_back(beam_id);
  }
}

bool has_analog_access_inventory(const ntn_served_beam_scheduler_config& cfg)
{
  return !cfg.analog_beams.empty() && cfg.max_nof_active_analog_access_beams != 0;
}

std::map<std::string, std::string> build_digital_to_analog_map(const std::vector<ntn_beam_position>& beams)
{
  std::map<std::string, std::string> result;
  for (const auto& beam : beams) {
    if (!beam.beam_id.empty() && !beam.analog_beam_id.empty()) {
      result[beam.beam_id] = beam.analog_beam_id;
    }
  }
  return result;
}

std::vector<std::string> select_analog_window_ids(const ntn_served_beam_scheduler_config&          cfg,
                                                  const std::vector<ntn_served_beam_candidate>& candidates,
                                                  unsigned                                     window_start)
{
  const std::map<std::string, std::string> digital_to_analog = build_digital_to_analog_map(cfg.beams);
  std::vector<std::string> visible_analog_ids;
  std::set<std::string>    seen;
  for (const auto& candidate : candidates) {
    const auto analog_it = digital_to_analog.find(candidate.beam_id);
    if (analog_it == digital_to_analog.end()) {
      continue;
    }
    const auto analog_cfg_it = std::find_if(cfg.analog_beams.begin(),
                                            cfg.analog_beams.end(),
                                            [&analog_it](const ntn_analog_beam_position& analog) {
                                              return analog.analog_beam_id == analog_it->second && analog.enabled;
                                            });
    if (analog_cfg_it == cfg.analog_beams.end()) {
      continue;
    }
    if (seen.emplace(analog_it->second).second) {
      visible_analog_ids.push_back(analog_it->second);
    }
  }

  const unsigned nof_window_analogs =
      std::min(cfg.max_nof_active_analog_access_beams, static_cast<unsigned>(visible_analog_ids.size()));
  std::vector<std::string> selected;
  selected.reserve(nof_window_analogs);
  if (visible_analog_ids.empty()) {
    return selected;
  }
  for (unsigned i = 0; i != nof_window_analogs; ++i) {
    selected.push_back(visible_analog_ids[(window_start + i) % visible_analog_ids.size()]);
  }
  return selected;
}

std::vector<std::string> select_demand_aware_analog_window_ids(
    const ntn_served_beam_scheduler_config&                    cfg,
    const std::vector<ntn_served_beam_candidate>&               candidates,
    const std::map<std::string, ntn_served_beam_demand>&        demands,
    unsigned                                                   window_start)
{
  const std::vector<std::string> legacy_selected = select_analog_window_ids(cfg, candidates, window_start);
  if (!cfg.demand_aware_beam_scheduling_enabled || !has_any_demand(demands)) {
    return legacy_selected;
  }

  const std::map<std::string, std::string> digital_to_analog = build_digital_to_analog_map(cfg.beams);
  std::map<std::string, ntn_served_beam_demand> analog_demands;
  std::map<std::string, unsigned> legacy_order;
  for (unsigned i = 0; i != legacy_selected.size(); ++i) {
    legacy_order[legacy_selected[i]] = i;
  }

  std::set<std::string> visible_analog_ids;
  for (const auto& candidate : candidates) {
    const auto analog_it = digital_to_analog.find(candidate.beam_id);
    if (analog_it == digital_to_analog.end()) {
      continue;
    }
    const auto analog_cfg_it = std::find_if(cfg.analog_beams.begin(),
                                            cfg.analog_beams.end(),
                                            [&analog_it](const ntn_analog_beam_position& analog) {
                                              return analog.analog_beam_id == analog_it->second && analog.enabled;
                                            });
    if (analog_cfg_it == cfg.analog_beams.end()) {
      continue;
    }
    visible_analog_ids.insert(analog_it->second);

    const ntn_served_beam_demand* child_demand = find_demand(demands, candidate.beam_id);
    if (!has_demand(child_demand)) {
      continue;
    }
    ntn_served_beam_demand& analog_demand = analog_demands[analog_it->second];
    analog_demand.beam_id = analog_it->second;
    analog_demand.nof_ues += child_demand->nof_ues;
    analog_demand.nof_drbs += child_demand->nof_drbs;
    analog_demand.nof_pending_handovers += child_demand->nof_pending_handovers;
    if (!analog_demand.qos.has_qos_demand ||
        is_ntn_qos_demand_higher_priority(child_demand->qos, analog_demand.qos)) {
      analog_demand.qos = child_demand->qos;
    } else if (child_demand->qos.has_qos_demand) {
      analog_demand.qos.nof_qos_flows += child_demand->qos.nof_qos_flows;
    }
  }

  std::vector<std::string> visible_analogs(visible_analog_ids.begin(), visible_analog_ids.end());
  if (visible_analogs.empty()) {
    return {};
  }

  std::map<std::string, ntn_served_beam_candidate> analog_candidates;
  for (const std::string& analog_id : visible_analogs) {
    analog_candidates[analog_id] = {analog_id, 0.0, true, "sat-0"};
  }

  std::stable_sort(visible_analogs.begin(), visible_analogs.end(), [&](const std::string& lhs, const std::string& rhs) {
    const ntn_served_beam_demand* lhs_demand = find_demand(analog_demands, lhs);
    const ntn_served_beam_demand* rhs_demand = find_demand(analog_demands, rhs);
    if (demand_is_better(lhs_demand, rhs_demand, analog_candidates[lhs], analog_candidates[rhs])) {
      return true;
    }
    if (demand_is_better(rhs_demand, lhs_demand, analog_candidates[rhs], analog_candidates[lhs])) {
      return false;
    }
    const unsigned lhs_order = legacy_order.count(lhs) != 0 ? legacy_order[lhs] : std::numeric_limits<unsigned>::max();
    const unsigned rhs_order = legacy_order.count(rhs) != 0 ? legacy_order[rhs] : std::numeric_limits<unsigned>::max();
    if (lhs_order != rhs_order) {
      return lhs_order < rhs_order;
    }
    return lhs < rhs;
  });

  const unsigned nof_window_analogs =
      std::min(cfg.max_nof_active_analog_access_beams, static_cast<unsigned>(visible_analogs.size()));
  visible_analogs.resize(nof_window_analogs);
  return visible_analogs;
}

std::vector<std::string> select_digital_window_ids_for_analog_window(
    const ntn_served_beam_scheduler_config&                    cfg,
    const std::vector<ntn_served_beam_candidate>&               candidates,
    const std::map<std::string, ntn_served_beam_demand>& demands,
    const std::vector<std::string>&                             selected_analog_ids,
    bool                                                        demand_aware_active)
{
  const std::set<std::string> selected_analog_set(selected_analog_ids.begin(), selected_analog_ids.end());
  const std::map<std::string, std::string> digital_to_analog = build_digital_to_analog_map(cfg.beams);
  std::vector<ntn_served_beam_candidate> eligible_candidates;
  eligible_candidates.reserve(candidates.size());
  for (const auto& candidate : candidates) {
    const auto analog_it = digital_to_analog.find(candidate.beam_id);
    if (analog_it != digital_to_analog.end() && selected_analog_set.count(analog_it->second) != 0) {
      eligible_candidates.push_back(candidate);
    }
  }

  if (demand_aware_active) {
    std::stable_sort(eligible_candidates.begin(),
                     eligible_candidates.end(),
                     [&](const ntn_served_beam_candidate& lhs, const ntn_served_beam_candidate& rhs) {
                       const ntn_served_beam_demand* lhs_demand = find_demand(demands, lhs.beam_id);
                       const ntn_served_beam_demand* rhs_demand = find_demand(demands, rhs.beam_id);
                       return demand_is_better(lhs_demand, rhs_demand, lhs, rhs);
                     });
  }

  std::map<std::string, unsigned> analog_child_load;
  std::map<std::string, unsigned> analog_child_cap;
  for (const auto& analog : cfg.analog_beams) {
    if (analog.resource_policy.has_value() && analog.resource_policy->max_loaded_digital_children != 0) {
      analog_child_cap[analog.analog_beam_id] = analog.resource_policy->max_loaded_digital_children;
    }
  }

  const unsigned global_window_cap =
      cfg.max_nof_served_beams == 0 ? std::numeric_limits<unsigned>::max() : cfg.max_nof_served_beams;
  std::vector<std::string> window_beam_ids;
  window_beam_ids.reserve(std::min(eligible_candidates.size(), static_cast<size_t>(global_window_cap)));
  for (const auto& candidate : eligible_candidates) {
    if (window_beam_ids.size() >= global_window_cap) {
      break;
    }
    const auto analog_it = digital_to_analog.find(candidate.beam_id);
    if (analog_it == digital_to_analog.end()) {
      continue;
    }
    const auto cap_it = analog_child_cap.find(analog_it->second);
    if (cap_it != analog_child_cap.end() && analog_child_load[analog_it->second] >= cap_it->second) {
      continue;
    }
    window_beam_ids.push_back(candidate.beam_id);
    ++analog_child_load[analog_it->second];
  }
  return window_beam_ids;
}

std::vector<std::string> select_demand_aware_digital_window_ids(
    const std::vector<ntn_served_beam_candidate>&               candidates,
    const std::map<std::string, ntn_served_beam_demand>& demands,
    unsigned                                                   window_size)
{
  if (window_size == 0 || candidates.empty()) {
    std::vector<std::string> all_beam_ids;
    all_beam_ids.reserve(candidates.size());
    for (const auto& candidate : candidates) {
      all_beam_ids.push_back(candidate.beam_id);
    }
    return all_beam_ids;
  }

  std::vector<ntn_served_beam_candidate> ordered = candidates;
  std::stable_sort(ordered.begin(),
                   ordered.end(),
                   [&](const ntn_served_beam_candidate& lhs, const ntn_served_beam_candidate& rhs) {
                     const ntn_served_beam_demand* lhs_demand = find_demand(demands, lhs.beam_id);
                     const ntn_served_beam_demand* rhs_demand = find_demand(demands, rhs.beam_id);
                     return demand_is_better(lhs_demand, rhs_demand, lhs, rhs);
                   });

  const unsigned nof_window_candidates = std::min(window_size, static_cast<unsigned>(ordered.size()));
  std::vector<std::string> result;
  result.reserve(nof_window_candidates);
  for (unsigned i = 0; i != nof_window_candidates; ++i) {
    result.push_back(ordered[i].beam_id);
  }
  return result;
}

unsigned get_analog_window_size(const ntn_served_beam_scheduler_config& cfg,
                                const std::vector<ntn_served_beam_candidate>& candidates)
{
  if (!has_analog_access_inventory(cfg)) {
    return 0;
  }
  return static_cast<unsigned>(select_analog_window_ids(cfg, candidates, 0).size());
}

} // namespace

bool ntn_served_beam_update_handler::update_ntn_served_beam_candidates(
    const std::vector<ntn_served_beam_candidate>& candidates)
{
  std::vector<std::string> beam_ids;
  beam_ids.reserve(candidates.size());
  for (const auto& candidate : candidates) {
    if (candidate.in_hopping_window) {
      beam_ids.push_back(candidate.beam_id);
    }
  }
  return update_ntn_served_beams(beam_ids);
}

ntn_served_beam_scheduler::ntn_served_beam_scheduler(ntn_served_beam_scheduler_config cfg_) : cfg(std::move(cfg_)) {}

ntn_served_beam_schedule ntn_served_beam_scheduler::compute_schedule(const ecef_coordinates_t& satellite) const
{
  return compute_schedule(std::vector<ntn_satellite_state>{{"sat-0", satellite}});
}

ntn_served_beam_schedule
ntn_served_beam_scheduler::compute_schedule(const ecef_coordinates_t&                  satellite,
                                            const std::vector<ntn_served_beam_demand>& demands) const
{
  return compute_schedule(std::vector<ntn_satellite_state>{{"sat-0", satellite}}, demands);
}

ntn_served_beam_schedule
ntn_served_beam_scheduler::compute_schedule(const std::vector<ntn_satellite_state>& satellites) const
{
  return compute_schedule(satellites, {});
}

ntn_served_beam_schedule
ntn_served_beam_scheduler::compute_schedule(const std::vector<ntn_satellite_state>&    satellites,
                                            const std::vector<ntn_served_beam_demand>& demands) const
{
  ntn_served_beam_schedule schedule;
  schedule.candidates = select_ntn_candidate_inventory_by_elevation(cfg.beams, satellites, cfg.min_elevation_deg);
  const std::map<std::string, ntn_served_beam_demand> demand_by_beam = build_demand_by_beam(demands);
  const bool demand_aware_active = cfg.demand_aware_beam_scheduling_enabled && has_any_demand(demand_by_beam);
  schedule.demand_aware_evaluated = cfg.demand_aware_beam_scheduling_enabled;

  if (has_analog_access_inventory(cfg)) {
    const std::vector<std::string> legacy_selected_analog_ids =
        select_analog_window_ids(cfg, schedule.candidates, next_hopping_start_index);
    const std::vector<std::string> legacy_selected_beam_ids =
        select_digital_window_ids_for_analog_window(
            cfg, schedule.candidates, demand_by_beam, legacy_selected_analog_ids, false);
    const std::vector<std::string> selected_analog_ids =
        demand_aware_active ? select_demand_aware_analog_window_ids(
                                  cfg, schedule.candidates, demand_by_beam, next_hopping_start_index)
                            : legacy_selected_analog_ids;
    const std::vector<std::string> selected_beam_ids =
        select_digital_window_ids_for_analog_window(
            cfg, schedule.candidates, demand_by_beam, selected_analog_ids, demand_aware_active);
    apply_hopping_window(schedule, selected_beam_ids, demand_by_beam);
    schedule.demand_prioritized_window =
        demand_aware_active &&
        (selected_analog_ids != legacy_selected_analog_ids || selected_beam_ids != legacy_selected_beam_ids);
    schedule.legacy_fallback = cfg.demand_aware_beam_scheduling_enabled && !demand_aware_active;
    schedule.scheduling_reason =
        schedule.demand_prioritized_window ? "demand_prioritized" : (schedule.legacy_fallback ? "legacy_no_demand" : "legacy");
    std::vector<std::string> visible_beam_ids = get_visible_beam_membership_ids(schedule.candidates);
    std::vector<std::string> visible_owner_ids = get_visible_beam_owner_ids(schedule.candidates);
    schedule.changed = schedule.beam_ids != current_beam_ids || visible_beam_ids != current_visible_beam_ids ||
                       visible_owner_ids != current_visible_beam_owner_ids;
    return schedule;
  }

  const unsigned window_size =
      cfg.max_nof_served_beams == 0 ? static_cast<unsigned>(schedule.candidates.size()) : cfg.max_nof_served_beams;
  const bool rotate_window = cfg.served_beam_hopping_enabled && window_size != 0 &&
                             schedule.candidates.size() > static_cast<size_t>(window_size);
  const unsigned window_start =
      rotate_window ? next_hopping_start_index % static_cast<unsigned>(schedule.candidates.size()) : 0;
  const unsigned nof_window_candidates =
      std::min(window_size, static_cast<unsigned>(schedule.candidates.size()));

  std::vector<std::string> window_beam_ids;
  if (demand_aware_active) {
    window_beam_ids = select_demand_aware_digital_window_ids(schedule.candidates, demand_by_beam, window_size);
  } else {
    window_beam_ids.reserve(nof_window_candidates);
    for (unsigned i = 0; i != nof_window_candidates; ++i) {
      const unsigned candidate_index =
          rotate_window ? (window_start + i) % static_cast<unsigned>(schedule.candidates.size()) : i;
      window_beam_ids.push_back(schedule.candidates[candidate_index].beam_id);
    }
  }
  apply_hopping_window(schedule, window_beam_ids, demand_by_beam);
  schedule.demand_prioritized_window = demand_aware_active;
  schedule.legacy_fallback = cfg.demand_aware_beam_scheduling_enabled && !demand_aware_active;
  schedule.scheduling_reason =
      schedule.demand_prioritized_window ? "demand_prioritized" : (schedule.legacy_fallback ? "legacy_no_demand" : "legacy");

  std::vector<std::string> visible_beam_ids = get_visible_beam_membership_ids(schedule.candidates);
  std::vector<std::string> visible_owner_ids = get_visible_beam_owner_ids(schedule.candidates);
  schedule.changed = schedule.beam_ids != current_beam_ids || visible_beam_ids != current_visible_beam_ids ||
                     visible_owner_ids != current_visible_beam_owner_ids;
  return schedule;
}

ntn_served_beam_schedule ntn_served_beam_scheduler::update_from_satellite_state(
    const ecef_coordinates_t&        satellite,
    ntn_served_beam_update_handler& update_handler)
{
  return update_from_satellite_state(std::vector<ntn_satellite_state>{{"sat-0", satellite}}, update_handler);
}

ntn_served_beam_schedule ntn_served_beam_scheduler::update_from_satellite_state(
    const ecef_coordinates_t&                  satellite,
    const std::vector<ntn_served_beam_demand>& demands,
    ntn_served_beam_update_handler&            update_handler)
{
  return update_from_satellite_state(std::vector<ntn_satellite_state>{{"sat-0", satellite}}, demands, update_handler);
}

ntn_served_beam_schedule ntn_served_beam_scheduler::update_from_satellite_state(
    const std::vector<ntn_satellite_state>& satellites,
    ntn_served_beam_update_handler&         update_handler)
{
  return update_from_satellite_state(satellites, {}, update_handler);
}

ntn_served_beam_schedule ntn_served_beam_scheduler::update_from_satellite_state(
    const std::vector<ntn_satellite_state>&    satellites,
    const std::vector<ntn_served_beam_demand>& demands,
    ntn_served_beam_update_handler&            update_handler)
{
  ntn_served_beam_schedule schedule = compute_schedule(satellites, demands);
  std::vector<std::string> visible_beam_ids = get_visible_beam_membership_ids(schedule.candidates);
  std::vector<std::string> visible_owner_ids = get_visible_beam_owner_ids(schedule.candidates);
  if (visible_beam_ids != current_visible_beam_ids) {
    next_hopping_start_index       = 0;
    current_hopping_dwell_updates = 0;
    schedule                      = compute_schedule(satellites, demands);
    visible_beam_ids              = get_visible_beam_membership_ids(schedule.candidates);
    visible_owner_ids             = get_visible_beam_owner_ids(schedule.candidates);
  }

  const unsigned window_size =
      cfg.max_nof_served_beams == 0 ? static_cast<unsigned>(schedule.candidates.size()) : cfg.max_nof_served_beams;
  const unsigned analog_window_size = get_analog_window_size(cfg, schedule.candidates);
  const bool rotate_analog_window = has_analog_access_inventory(cfg) && cfg.served_beam_hopping_enabled &&
                                    analog_window_size != 0 &&
                                    cfg.analog_beams.size() > static_cast<size_t>(analog_window_size);
  const bool rotate_window = rotate_analog_window ||
                             (cfg.served_beam_hopping_enabled && window_size != 0 &&
                              schedule.candidates.size() > static_cast<size_t>(window_size));
  const bool demand_window_changed =
      schedule.demand_prioritized_window && schedule.beam_ids != current_beam_ids && !current_beam_ids.empty();
  if (rotate_window && visible_beam_ids == current_visible_beam_ids && !current_beam_ids.empty() &&
      !demand_window_changed) {
    const unsigned dwell_updates = std::max(1U, cfg.served_beam_hopping_dwell_updates);
    if (current_hopping_dwell_updates >= dwell_updates) {
      const unsigned increment = rotate_analog_window ? analog_window_size : window_size;
      const unsigned modulo    = rotate_analog_window ? static_cast<unsigned>(cfg.analog_beams.size())
                                                      : static_cast<unsigned>(schedule.candidates.size());
      next_hopping_start_index = (next_hopping_start_index + increment) % modulo;
      current_hopping_dwell_updates = 0;
      schedule                      = compute_schedule(satellites, demands);
      visible_beam_ids              = get_visible_beam_membership_ids(schedule.candidates);
      visible_owner_ids             = get_visible_beam_owner_ids(schedule.candidates);
    } else {
      ++current_hopping_dwell_updates;
      apply_hopping_window(schedule, current_beam_ids, build_demand_by_beam(demands));
      schedule.sticky_kept = true;
      schedule.changed = visible_owner_ids != current_visible_beam_owner_ids;
    }
  }

  if (!schedule.changed) {
    return schedule;
  }

  schedule.applied = update_handler.update_ntn_served_beam_candidates(schedule.candidates);
  if (schedule.applied) {
    current_beam_ids = schedule.beam_ids;
    current_visible_beam_ids = std::move(visible_beam_ids);
    current_visible_beam_owner_ids = std::move(visible_owner_ids);

    if (rotate_window) {
      current_hopping_dwell_updates = 1;
    } else {
      next_hopping_start_index = 0;
      current_hopping_dwell_updates = 0;
    }
  }
  return schedule;
}
