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
#include <set>
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

constexpr const char* reason_eligible                         = "eligible";
constexpr const char* reason_no_supported_child               = "no_supported_child";
constexpr const char* reason_du_capacity_exhausted            = "du_capacity_exhausted";
constexpr const char* reason_draining                         = "draining";
constexpr const char* reason_service_du_differs_from_access_du = "service_du_differs_from_access_du";
constexpr const char* reason_analog_loaded_child_cap_exhausted = "analog_loaded_child_cap_exhausted";
constexpr const char* reason_analog_service_bound_ue_cap_exhausted = "analog_service_bound_ue_cap_exhausted";
constexpr const char* reason_analog_drb_cap_exhausted         = "analog_drb_cap_exhausted";
constexpr const char* reason_digital_ue_cap_exhausted         = "digital_ue_cap_exhausted";
constexpr const char* reason_digital_drb_cap_exhausted        = "digital_drb_cap_exhausted";
constexpr const char* reason_conflict_group_blocked           = "conflict_group_blocked";
constexpr const char* reason_link_direction_incomplete        = "link_direction_incomplete";

struct analog_du_candidate {
  std::map<du_index_t, du_usage>::iterator du_it;
  unsigned                                supported_child_count = 0;
  bool                                    preserves_previous_du = false;
};

struct analog_resource_usage {
  unsigned loaded_digital_children = 0;
  unsigned service_bound_ues       = 0;
  unsigned drbs                    = 0;
};

struct resource_domain_evaluation {
  bool        eligible = true;
  std::string reason   = reason_eligible;
};

bool is_better_analog_du_choice(const analog_du_candidate& lhs,
                                const analog_du_candidate& rhs,
                                const std::map<du_index_t, unsigned>& assigned_analog_counts);

bool analog_du_has_service_capacity(const du_usage& usage);

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

void update_link_direction_readiness(ntn_beam_du_assignment& assignment)
{
  assignment.downlink_ready =
      assignment.downlink_enabled && (assignment.state == ntn_beam_assignment_state::candidate ||
                                      assignment.state == ntn_beam_assignment_state::active_loaded);
  assignment.uplink_ready =
      assignment.uplink_enabled && (assignment.state == ntn_beam_assignment_state::active_loaded ||
                                    assignment.state == ntn_beam_assignment_state::draining);
  assignment.downlink_visible = assignment.downlink_ready;
  assignment.uplink_access_ready =
      assignment.uplink_enabled && (assignment.state == ntn_beam_assignment_state::candidate ||
                                    assignment.state == ntn_beam_assignment_state::active_loaded);
  assignment.access_roundtrip_ready = assignment.downlink_visible && assignment.uplink_access_ready;
  assignment.bidirectional_service_ready =
      assignment.downlink_enabled && assignment.uplink_enabled &&
      assignment.state == ntn_beam_assignment_state::active_loaded && assignment.du_index != du_index_t::invalid &&
      assignment.resource_domain_eligible;
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

bool has_higher_beam_priority(const std::string&                         lhs_beam_id,
                              const std::string&                         rhs_beam_id,
                              const std::map<std::string, ntn_beam_load>& loads,
                              const std::map<std::string, double>&        elevations);

ntn_beam_du_assignment make_assignment(const ntn_beam_position&       beam,
                                       ntn_beam_assignment_state     state,
                                       du_index_t                    du_index,
                                       double                        elevation_deg,
                                       const ntn_beam_load&          load,
                                       bool                          in_hopping_window = false,
                                       const std::string&            serving_satellite_id = "sat-0")
{
  ntn_beam_du_assignment assignment;
  assignment.beam_id           = beam.beam_id;
  assignment.serving_satellite_id = serving_satellite_id;
  assignment.nci               = beam.nci;
  assignment.du_index          = du_index;
  assignment.state             = state;
  assignment.elevation_deg     = elevation_deg;
  assignment.nof_ues           = load.nof_ues;
  assignment.nof_drbs          = load.nof_drbs;
  assignment.in_hopping_window = in_hopping_window;
  assignment.qos               = load.qos;
  assignment.downlink_enabled  = beam.downlink_enabled;
  assignment.uplink_enabled    = beam.uplink_enabled;
  assignment.downlink_service_required = load.downlink_service_required;
  assignment.uplink_resource_required  = load.uplink_resource_required;
  update_link_direction_readiness(assignment);
  return assignment;
}

void set_du_policy(ntn_beam_du_assignment& assignment,
                   du_index_t              access_du_index,
                   const std::string&      access_du_reason,
                   const std::string&      du_assignment_reason)
{
  assignment.access_du_index      = access_du_index;
  assignment.access_du_reason     = access_du_reason;
  assignment.du_assignment_reason = du_assignment_reason;
}

std::string choose_du_assignment_reason(du_index_t              access_du_index,
                                        du_index_t              service_du_index,
                                        ntn_beam_assignment_state state,
                                        const std::string&      access_du_reason)
{
  if (state == ntn_beam_assignment_state::draining) {
    return reason_draining;
  }
  if (access_du_index == du_index_t::invalid) {
    return access_du_reason;
  }
  if (service_du_index == du_index_t::invalid) {
    return reason_service_du_differs_from_access_du;
  }
  return access_du_index == service_du_index ? reason_eligible : reason_service_du_differs_from_access_du;
}

std::vector<std::string> get_active_window_children(const ntn_analog_beam_position&             analog,
                                                    const std::map<std::string, ntn_beam_position>& enabled_beams,
                                                    const std::map<std::string, bool>&          visible_in_hopping_window)
{
  std::vector<std::string> children;
  children.reserve(analog.child_digital_beam_ids.size());
  for (const auto& child_id : analog.child_digital_beam_ids) {
    auto beam_it = enabled_beams.find(child_id);
    if (beam_it == enabled_beams.end()) {
      continue;
    }
    auto visible_it = visible_in_hopping_window.find(child_id);
    if (visible_it == visible_in_hopping_window.end() || !visible_it->second) {
      continue;
    }
    children.push_back(child_id);
  }
  return children;
}

std::string get_best_analog_child_for_ordering(const std::vector<std::string>&               children,
                                               const std::map<std::string, ntn_beam_load>&   loads,
                                               const std::map<std::string, double>&          elevations)
{
  if (children.empty()) {
    return {};
  }
  return *std::max_element(children.begin(),
                           children.end(),
                           [&loads, &elevations](const std::string& lhs, const std::string& rhs) {
                             return has_higher_beam_priority(rhs, lhs, loads, elevations);
                           });
}

unsigned count_supported_children(const ntn_du_beam_capacity& capacity, const std::vector<std::string>& children)
{
  unsigned count = 0;
  for (const auto& child_id : children) {
    if (supports_beam(capacity, child_id)) {
      ++count;
    }
  }
  return count;
}

std::vector<ntn_analog_access_du_assignment> build_analog_access_du_assignments(
    const std::vector<ntn_analog_beam_position>&           analog_beams,
    const std::map<std::string, ntn_beam_position>&        enabled_beams,
    const std::map<std::string, bool>&                     visible_in_hopping_window,
    const std::map<std::string, ntn_beam_load>&            loads,
    const std::map<std::string, double>&                   elevations,
    std::map<du_index_t, du_usage>&                        du_usages,
    const std::map<std::string, ntn_beam_du_assignment>&   previous_service_assignments)
{
  std::map<std::string, std::vector<std::string>> active_children_by_analog;
  for (const auto& analog : analog_beams) {
    if (!analog.enabled || analog.analog_beam_id.empty()) {
      continue;
    }
    std::vector<std::string> children = get_active_window_children(analog, enabled_beams, visible_in_hopping_window);
    if (!children.empty()) {
      active_children_by_analog[analog.analog_beam_id] = std::move(children);
    }
  }

  std::vector<std::string> active_analog_ids;
  active_analog_ids.reserve(active_children_by_analog.size());
  for (const auto& entry : active_children_by_analog) {
    active_analog_ids.push_back(entry.first);
  }
  std::sort(active_analog_ids.begin(),
            active_analog_ids.end(),
            [&active_children_by_analog, &loads, &elevations](const std::string& lhs, const std::string& rhs) {
              const std::string lhs_best =
                  get_best_analog_child_for_ordering(active_children_by_analog.at(lhs), loads, elevations);
              const std::string rhs_best =
                  get_best_analog_child_for_ordering(active_children_by_analog.at(rhs), loads, elevations);
              if (lhs_best != rhs_best) {
                if (has_higher_beam_priority(lhs_best, rhs_best, loads, elevations)) {
                  return true;
                }
                if (has_higher_beam_priority(rhs_best, lhs_best, loads, elevations)) {
                  return false;
                }
              }
              return lhs < rhs;
            });

  std::vector<ntn_analog_access_du_assignment> assignments;
  assignments.reserve(active_analog_ids.size());
  std::map<du_index_t, unsigned> assigned_analog_counts;
  for (const auto& analog_id : active_analog_ids) {
    const std::vector<std::string>& children = active_children_by_analog.at(analog_id);
    bool                            saw_supported_child_without_capacity = false;
    auto                            best_candidate = du_usages.end();
    analog_du_candidate             best{du_usages.end(), 0, false};

    for (auto du_it = du_usages.begin(); du_it != du_usages.end(); ++du_it) {
      const unsigned supported_child_count = count_supported_children(du_it->second.capacity, children);
      if (supported_child_count == 0) {
        continue;
      }
      if (!analog_du_has_service_capacity(du_it->second)) {
        saw_supported_child_without_capacity = true;
        continue;
      }

      bool preserves_previous_du = false;
      for (const auto& child_id : children) {
        auto previous_it = previous_service_assignments.find(child_id);
        if (previous_it != previous_service_assignments.end() && previous_it->second.du_index == du_it->first) {
          preserves_previous_du = true;
          break;
        }
      }

      analog_du_candidate candidate{du_it, supported_child_count, preserves_previous_du};
      if (best_candidate == du_usages.end() || is_better_analog_du_choice(candidate, best, assigned_analog_counts)) {
        best_candidate = du_it;
        best           = candidate;
      }
    }

    ntn_analog_access_du_assignment assignment;
    assignment.analog_beam_id = analog_id;
    if (best_candidate != du_usages.end()) {
      assignment.selected_access_du_index = best_candidate->first;
      assignment.supported_child_count    = best.supported_child_count;
      assignment.assigned_child_count     = best.supported_child_count;
      assignment.reason                   = reason_eligible;
      ++assigned_analog_counts[best_candidate->first];
    } else {
      assignment.reason =
          saw_supported_child_without_capacity ? reason_du_capacity_exhausted : reason_no_supported_child;
    }
    assignments.push_back(std::move(assignment));
  }

  return assignments;
}

void merge_qos_summary(ntn_qos_demand_summary& dst, const ntn_qos_demand_summary& src)
{
  if (!src.has_qos_demand) {
    return;
  }
  if (!dst.has_qos_demand) {
    dst = src;
    return;
  }

  dst.nof_qos_flows += src.nof_qos_flows;
  if (src.may_trigger_preemption) {
    dst.may_trigger_preemption = true;
  }
  if (src.is_preemptable) {
    dst.is_preemptable = true;
  }
  if (src.has_gbr) {
    dst.has_gbr = true;
  }
  if (src.has_delay_critical_gbr) {
    dst.has_delay_critical_gbr = true;
  }
  if (src.best_arp_priority != 0 && (dst.best_arp_priority == 0 || src.best_arp_priority < dst.best_arp_priority)) {
    dst.best_arp_priority = src.best_arp_priority;
  }
  if (src.best_qos_priority != 0 && (dst.best_qos_priority == 0 || src.best_qos_priority < dst.best_qos_priority)) {
    dst.best_qos_priority = src.best_qos_priority;
    dst.best_five_qi      = src.best_five_qi;
  }
  if (!dst.primary_s_nssai.has_value() && src.primary_s_nssai.has_value()) {
    dst.primary_s_nssai = src.primary_s_nssai;
  }
}

ntn_analog_beam_resource_policy merge_policy(const ntn_analog_beam_resource_policy& defaults,
                                             const std::optional<ntn_analog_beam_resource_policy>& override)
{
  if (!override.has_value()) {
    return defaults;
  }
  ntn_analog_beam_resource_policy merged = defaults;
  if (override->max_access_only_ues != 0) {
    merged.max_access_only_ues = override->max_access_only_ues;
  }
  if (override->max_service_bound_ues != 0) {
    merged.max_service_bound_ues = override->max_service_bound_ues;
  }
  if (override->max_loaded_digital_children != 0) {
    merged.max_loaded_digital_children = override->max_loaded_digital_children;
  }
  if (override->max_drbs != 0) {
    merged.max_drbs = override->max_drbs;
  }
  return merged;
}

ntn_digital_beam_resource_policy merge_policy(const ntn_digital_beam_resource_policy& defaults,
                                              const std::optional<ntn_digital_beam_resource_policy>& override)
{
  if (!override.has_value()) {
    return defaults;
  }
  ntn_digital_beam_resource_policy merged = defaults;
  if (override->max_ues != 0) {
    merged.max_ues = override->max_ues;
  }
  if (override->max_drbs != 0) {
    merged.max_drbs = override->max_drbs;
  }
  if (override->max_loaded_ues != 0) {
    merged.max_loaded_ues = override->max_loaded_ues;
  }
  if (!override->reuse_group_id.empty()) {
    merged.reuse_group_id = override->reuse_group_id;
  }
  if (!override->conflict_group_ids.empty()) {
    merged.conflict_group_ids = override->conflict_group_ids;
  }
  return merged;
}

resource_domain_evaluation evaluate_resource_domain(
    const ntn_beam_position&                                  beam,
    const ntn_beam_load&                                      load,
    const ntn_digital_beam_resource_policy&                   digital_policy,
    const std::map<std::string, ntn_analog_beam_resource_policy>& analog_policies,
    const std::map<std::string, analog_resource_usage>&       analog_usages,
    const std::set<std::string>&                              active_conflict_groups)
{
  if (digital_policy.max_ues != 0 && load.nof_ues > digital_policy.max_ues) {
    return {false, reason_digital_ue_cap_exhausted};
  }
  if (digital_policy.max_loaded_ues != 0 && load.nof_ues > digital_policy.max_loaded_ues) {
    return {false, reason_digital_ue_cap_exhausted};
  }
  if (digital_policy.max_drbs != 0 && load.nof_drbs > digital_policy.max_drbs) {
    return {false, reason_digital_drb_cap_exhausted};
  }
  for (const auto& group_id : digital_policy.conflict_group_ids) {
    if (active_conflict_groups.count(group_id) != 0) {
      return {false, reason_conflict_group_blocked};
    }
  }

  if (beam.analog_beam_id.empty()) {
    return {};
  }
  auto policy_it = analog_policies.find(beam.analog_beam_id);
  if (policy_it == analog_policies.end()) {
    return {};
  }
  const ntn_analog_beam_resource_policy& analog_policy = policy_it->second;
  const analog_resource_usage usage =
      analog_usages.count(beam.analog_beam_id) != 0 ? analog_usages.at(beam.analog_beam_id) : analog_resource_usage{};
  if (analog_policy.max_loaded_digital_children != 0 &&
      usage.loaded_digital_children >= analog_policy.max_loaded_digital_children) {
    return {false, reason_analog_loaded_child_cap_exhausted};
  }
  if (analog_policy.max_drbs != 0 && usage.drbs + load.nof_drbs > analog_policy.max_drbs) {
    return {false, reason_analog_drb_cap_exhausted};
  }
  return {};
}

void populate_resource_domain_fields(ntn_beam_du_assignment&                                assignment,
                                     const ntn_beam_position&                               beam,
                                     const ntn_beam_load&                                   load,
                                     const ntn_digital_beam_resource_policy&                digital_policy,
                                     const std::map<std::string, ntn_analog_beam_resource_policy>& analog_policies,
                                     const std::map<std::string, analog_resource_usage>&     analog_usages,
                                     const resource_domain_evaluation&                       evaluation)
{
  assignment.resource_domain_eligible = evaluation.eligible;
  assignment.resource_domain_reason   = evaluation.reason;
  assignment.reuse_group_id           = digital_policy.reuse_group_id;
  assignment.conflict_group_ids       = digital_policy.conflict_group_ids;
  assignment.digital_ue_cap           = digital_policy.max_ues != 0 ? digital_policy.max_ues : digital_policy.max_loaded_ues;
  assignment.digital_ue_load          = load.nof_ues;
  assignment.digital_drb_cap          = digital_policy.max_drbs;
  assignment.digital_drb_load         = load.nof_drbs;

  if (!beam.analog_beam_id.empty()) {
    auto analog_policy_it = analog_policies.find(beam.analog_beam_id);
    if (analog_policy_it != analog_policies.end()) {
      assignment.analog_loaded_digital_child_cap = analog_policy_it->second.max_loaded_digital_children;
      assignment.analog_service_bound_ue_cap     = analog_policy_it->second.max_service_bound_ues;
      assignment.analog_drb_cap                  = analog_policy_it->second.max_drbs;
    }
    auto analog_usage_it = analog_usages.find(beam.analog_beam_id);
    if (analog_usage_it != analog_usages.end()) {
      assignment.analog_loaded_digital_child_load = analog_usage_it->second.loaded_digital_children;
      assignment.analog_service_bound_ue_load     = analog_usage_it->second.service_bound_ues;
      assignment.analog_drb_load                  = analog_usage_it->second.drbs;
    }
  }
  update_link_direction_readiness(assignment);
}

void reserve_resource_domain(const ntn_beam_position&                    beam,
                             const ntn_beam_load&                        load,
                             const ntn_digital_beam_resource_policy&     digital_policy,
                             std::map<std::string, analog_resource_usage>& analog_usages,
                             std::set<std::string>&                      active_conflict_groups)
{
  if (!beam.analog_beam_id.empty()) {
    analog_resource_usage& usage = analog_usages[beam.analog_beam_id];
    ++usage.loaded_digital_children;
    usage.drbs += load.nof_drbs;
  }
  for (const auto& group_id : digital_policy.conflict_group_ids) {
    active_conflict_groups.insert(group_id);
  }
}

bool has_higher_beam_priority(const std::string&                         lhs_beam_id,
                              const std::string&                         rhs_beam_id,
                              const std::map<std::string, ntn_beam_load>& loads,
                              const std::map<std::string, double>&        elevations)
{
  const ntn_beam_load lhs_load = get_load(loads, lhs_beam_id);
  const ntn_beam_load rhs_load = get_load(loads, rhs_beam_id);
  if (is_ntn_qos_demand_higher_priority(lhs_load.qos, rhs_load.qos)) {
    return true;
  }
  if (is_ntn_qos_demand_higher_priority(rhs_load.qos, lhs_load.qos)) {
    return false;
  }
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

bool is_better_analog_du_choice(const analog_du_candidate& lhs,
                                const analog_du_candidate& rhs,
                                const std::map<du_index_t, unsigned>& assigned_analog_counts)
{
  if (lhs.preserves_previous_du != rhs.preserves_previous_du) {
    return lhs.preserves_previous_du;
  }
  if (lhs.supported_child_count != rhs.supported_child_count) {
    return lhs.supported_child_count > rhs.supported_child_count;
  }

  const unsigned lhs_assigned =
      assigned_analog_counts.count(lhs.du_it->first) != 0 ? assigned_analog_counts.at(lhs.du_it->first) : 0;
  const unsigned rhs_assigned =
      assigned_analog_counts.count(rhs.du_it->first) != 0 ? assigned_analog_counts.at(rhs.du_it->first) : 0;
  if (lhs_assigned != rhs_assigned) {
    return lhs_assigned < rhs_assigned;
  }
  if (lhs.du_it->second.nof_ues != rhs.du_it->second.nof_ues) {
    return lhs.du_it->second.nof_ues < rhs.du_it->second.nof_ues;
  }
  if (lhs.du_it->second.nof_drbs != rhs.du_it->second.nof_drbs) {
    return lhs.du_it->second.nof_drbs < rhs.du_it->second.nof_drbs;
  }
  return du_index_to_uint(lhs.du_it->first) < du_index_to_uint(rhs.du_it->first);
}

bool analog_du_has_service_capacity(const du_usage& usage)
{
  return usage.capacity.du_index != du_index_t::invalid && usage.capacity.max_active_beams != 0;
}

bool is_service_assignment(ntn_beam_assignment_state state)
{
  return state == ntn_beam_assignment_state::active_loaded || state == ntn_beam_assignment_state::draining;
}

bool has_user_demand(const ntn_beam_load& load)
{
  return load.nof_ues != 0 || load.nof_drbs != 0;
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

bool has_resource_weight_qos_boost(const ntn_qos_demand_summary& qos)
{
  return qos.has_delay_critical_gbr || qos.has_gbr || qos.may_trigger_preemption || qos.best_arp_priority <= 3;
}

unsigned get_demand_aware_resource_weight(const ntn_beam_du_assignment& assignment)
{
  if (!has_user_demand(assignment)) {
    return 0;
  }
  if (assignment.nof_ues == 0) {
    return 1;
  }

  unsigned weight = assignment.nof_ues;
  if (assignment.nof_drbs > assignment.nof_ues) {
    weight += assignment.nof_drbs - assignment.nof_ues;
  }
  if (assignment.qos.has_delay_critical_gbr) {
    weight += 4;
  } else if (assignment.qos.has_gbr) {
    weight += 2;
  }
  if (assignment.qos.may_trigger_preemption) {
    ++weight;
  }
  if (assignment.qos.best_arp_priority <= 3) {
    ++weight;
  }
  return std::max(1U, weight);
}

std::string get_weighted_resource_reason(const ntn_beam_du_assignment& assignment, unsigned nof_slots)
{
  if (assignment.nof_ues == 0 && assignment.nof_drbs != 0) {
    return "drb_only_min";
  }
  if (nof_slots == 1) {
    return "fairness_floor";
  }
  if (has_resource_weight_qos_boost(assignment.qos)) {
    return "qos_weighted";
  }
  return "demand_weighted";
}

std::vector<unsigned> get_weighted_resource_slot_counts(const ntn_beam_placement_plan& plan,
                                                        const std::vector<unsigned>&   scheduled_assignment_indexes,
                                                        unsigned                       antenna_slot_period)
{
  std::vector<unsigned> slot_counts(scheduled_assignment_indexes.size(), 1);
  if (scheduled_assignment_indexes.empty() || antenna_slot_period <= scheduled_assignment_indexes.size()) {
    return slot_counts;
  }

  struct remainder_entry {
    unsigned slot_index = 0;
    unsigned remainder  = 0;
    unsigned weight     = 0;
  };

  unsigned total_weight = 0;
  for (unsigned i = 0; i != scheduled_assignment_indexes.size(); ++i) {
    const ntn_beam_du_assignment& assignment = plan.assignments[scheduled_assignment_indexes[i]];
    const unsigned                weight     = get_demand_aware_resource_weight(assignment);
    total_weight += weight;
  }
  if (total_weight == 0) {
    return slot_counts;
  }

  const unsigned remaining_slots = antenna_slot_period - scheduled_assignment_indexes.size();
  unsigned       assigned_extra_slots = 0;
  std::vector<remainder_entry> remainders;
  remainders.reserve(scheduled_assignment_indexes.size());
  for (unsigned i = 0; i != scheduled_assignment_indexes.size(); ++i) {
    const ntn_beam_du_assignment& assignment = plan.assignments[scheduled_assignment_indexes[i]];
    const unsigned                weight     = get_demand_aware_resource_weight(assignment);
    const unsigned                weighted_slots = remaining_slots * weight;
    const unsigned                extra_slots    = weighted_slots / total_weight;
    slot_counts[i] += extra_slots;
    assigned_extra_slots += extra_slots;
    remainders.push_back({i, weighted_slots % total_weight, weight});
  }

  std::sort(remainders.begin(), remainders.end(), [](const remainder_entry& lhs, const remainder_entry& rhs) {
    if (lhs.remainder != rhs.remainder) {
      return lhs.remainder > rhs.remainder;
    }
    if (lhs.weight != rhs.weight) {
      return lhs.weight > rhs.weight;
    }
    return lhs.slot_index < rhs.slot_index;
  });

  for (unsigned i = 0; assigned_extra_slots < remaining_slots && i != remainders.size(); ++i) {
    ++slot_counts[remainders[i].slot_index];
    ++assigned_extra_slots;
  }
  return slot_counts;
}

void assign_user_driven_antenna_slots(ntn_beam_placement_plan& plan, bool demand_aware_resource_weighting_enabled)
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
    assignment.resource_weight      = 0;
    assignment.resource_share       = 0.0;
    assignment.resource_weight_reason = "none";

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
              if (is_ntn_qos_demand_higher_priority(lhs.qos, rhs.qos)) {
                return true;
              }
              if (is_ntn_qos_demand_higher_priority(rhs.qos, lhs.qos)) {
                return false;
              }
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

  const std::vector<unsigned> weighted_slot_counts =
      demand_aware_resource_weighting_enabled
          ? get_weighted_resource_slot_counts(plan, scheduled_assignment_indexes, antenna_slot_period)
          : std::vector<unsigned>{};

  unsigned next_slot_index = 0;
  for (unsigned slot_index = 0; slot_index != scheduled_assignment_indexes.size(); ++slot_index) {
    ntn_beam_du_assignment& assignment = plan.assignments[scheduled_assignment_indexes[slot_index]];
    const unsigned nof_slots = demand_aware_resource_weighting_enabled
                                   ? weighted_slot_counts[slot_index]
                                   : get_nof_user_driven_antenna_slots(assignment);
    assignment.antenna_slot_index      = next_slot_index;
    assignment.nof_antenna_slots       = nof_slots;
    assignment.antenna_slot_period     = antenna_slot_period;
    if (assignment.uplink_resource_required && assignment.uplink_ready) {
      assignment.sr_slot_offset  = next_slot_index;
      assignment.sr_slot_period  = antenna_slot_period;
      assignment.srs_slot_offset = next_slot_index + nof_slots - 1;
      assignment.srs_slot_period = antenna_slot_period;
    }
    assignment.resource_weight = demand_aware_resource_weighting_enabled
                                     ? get_demand_aware_resource_weight(assignment)
                                     : get_nof_user_driven_antenna_slots(assignment);
    assignment.resource_share = antenna_slot_period == 0 ? 0.0 : static_cast<double>(nof_slots) / antenna_slot_period;
    assignment.resource_weight_reason =
        demand_aware_resource_weighting_enabled ? get_weighted_resource_reason(assignment, nof_slots) : "legacy";
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

  if (previous_assignment.state == ntn_beam_assignment_state::active_loaded) {
    return {previous_assignment.beam_id, previous_assignment.nof_ues, previous_assignment.nof_drbs, previous_assignment.qos};
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

std::string srsran::srs_cu_cp::ntn_beam_assignment_state_to_string(ntn_beam_assignment_state state)
{
  switch (state) {
    case ntn_beam_assignment_state::inactive:
      return "inactive";
    case ntn_beam_assignment_state::candidate:
      return "candidate";
    case ntn_beam_assignment_state::active_loaded:
      return "active_loaded";
    case ntn_beam_assignment_state::draining:
      return "draining";
  }
  return "unknown";
}

std::vector<std::string> srsran::srs_cu_cp::get_active_loaded_ntn_beam_ids(
    const ntn_beam_placement_plan&                plan,
    const std::vector<ntn_served_beam_candidate>& visible_beams)
{
  std::vector<std::string> active_loaded_beam_ids;
  active_loaded_beam_ids.reserve(visible_beams.size());
  std::unordered_set<std::string> active_loaded_beams;
  for (const auto& assignment : plan.assignments) {
    if (assignment.state == ntn_beam_assignment_state::active_loaded) {
      active_loaded_beams.insert(assignment.beam_id);
    }
  }
  for (const auto& candidate : visible_beams) {
    if (active_loaded_beams.count(candidate.beam_id) != 0) {
      active_loaded_beam_ids.push_back(candidate.beam_id);
    }
  }
  return active_loaded_beam_ids;
}

std::vector<std::string> srsran::srs_cu_cp::get_loaded_service_calendar_ntn_beam_ids(
    const ntn_beam_placement_plan&                plan,
    const std::vector<ntn_served_beam_candidate>& visible_beams)
{
  std::vector<std::string> loaded_service_beam_ids;
  loaded_service_beam_ids.reserve(visible_beams.size());
  std::unordered_set<std::string> loaded_service_beams;
  for (const auto& assignment : plan.assignments) {
    if (!is_service_assignment(assignment.state) || !has_user_demand(assignment)) {
      continue;
    }
    loaded_service_beams.insert(assignment.beam_id);
  }
  for (const auto& candidate : visible_beams) {
    if (loaded_service_beams.count(candidate.beam_id) != 0) {
      loaded_service_beam_ids.push_back(candidate.beam_id);
    }
  }
  return loaded_service_beam_ids;
}

std::vector<std::string> srsran::srs_cu_cp::get_mobility_eligible_ntn_beam_ids(
    const ntn_beam_placement_plan&                plan,
    const std::vector<ntn_served_beam_candidate>& visible_beams)
{
  std::vector<std::string> mobility_eligible_beam_ids;
  mobility_eligible_beam_ids.reserve(visible_beams.size());
  std::unordered_set<std::string> mobility_eligible_beams;
  for (const auto& assignment : plan.assignments) {
    const bool is_active_loaded = assignment.bidirectional_service_ready;
    const bool is_eligible_candidate =
        assignment.state == ntn_beam_assignment_state::candidate && assignment.in_hopping_window &&
        assignment.du_index != du_index_t::invalid && assignment.downlink_enabled && assignment.uplink_enabled;
    if (is_active_loaded || is_eligible_candidate) {
      mobility_eligible_beams.insert(assignment.beam_id);
    }
  }
  for (const auto& candidate : visible_beams) {
    if (mobility_eligible_beams.count(candidate.beam_id) != 0) {
      mobility_eligible_beam_ids.push_back(candidate.beam_id);
    }
  }
  return mobility_eligible_beam_ids;
}

ntn_beam_placement_plan ntn_beam_placement_planner::plan(const ntn_beam_placement_request& request) const
{
  std::map<std::string, ntn_beam_position> enabled_beams;
  for (const auto& beam : request.beams) {
    if (beam.enabled && !beam.beam_id.empty()) {
      enabled_beams[beam.beam_id] = beam;
    }
  }

  std::map<std::string, ntn_digital_beam_resource_policy> digital_policies;
  for (const auto& entry : enabled_beams) {
    digital_policies[entry.first] = merge_policy(request.resource_policy.digital, entry.second.resource_policy);
  }

  std::map<std::string, ntn_analog_beam_resource_policy> analog_policies;
  for (const auto& analog : request.analog_beams) {
    if (!analog.analog_beam_id.empty()) {
      analog_policies[analog.analog_beam_id] = merge_policy(request.resource_policy.analog, analog.resource_policy);
    }
  }

  std::map<std::string, double> visible_elevations;
  std::map<std::string, bool>   visible_in_hopping_window;
  std::map<std::string, std::string> visible_satellite_ids;
  for (const auto& candidate : request.visible_beams) {
    if (enabled_beams.count(candidate.beam_id) == 0) {
      continue;
    }
    auto it = visible_elevations.find(candidate.beam_id);
    if (it == visible_elevations.end() || candidate.elevation_deg > it->second) {
      visible_elevations[candidate.beam_id] = candidate.elevation_deg;
      visible_satellite_ids[candidate.beam_id] = candidate.serving_satellite_id;
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
    const bool is_first_load = entry.beam_id.empty();
    entry.beam_id           = load.beam_id;
    entry.nof_ues += load.nof_ues;
    entry.nof_drbs += load.nof_drbs;
    if (is_first_load) {
      entry.downlink_service_required = load.downlink_service_required;
      entry.uplink_resource_required  = load.uplink_resource_required;
    } else {
      entry.downlink_service_required = entry.downlink_service_required || load.downlink_service_required;
      entry.uplink_resource_required  = entry.uplink_resource_required || load.uplink_resource_required;
    }
    merge_qos_summary(entry.qos, load.qos);
  }
  std::set<std::string> preheated_beam_ids;
  for (const std::string& beam_id : request.preheated_beam_ids) {
    if (!beam_id.empty()) {
      preheated_beam_ids.insert(beam_id);
    }
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

  ntn_beam_placement_plan result;
  result.analog_assignments = build_analog_access_du_assignments(request.analog_beams,
                                                                 enabled_beams,
                                                                 visible_in_hopping_window,
                                                                 loads,
                                                                 visible_elevations,
                                                                 du_usages,
                                                                 previous_service_assignments);
  std::map<std::string, ntn_analog_access_du_assignment> analog_assignment_by_id;
  for (const auto& assignment : result.analog_assignments) {
    analog_assignment_by_id[assignment.analog_beam_id] = assignment;
  }
  const bool analog_access_policy_enabled = !request.analog_beams.empty();

  auto get_access_assignment = [&analog_assignment_by_id](const ntn_beam_position& beam)
      -> const ntn_analog_access_du_assignment* {
    if (beam.analog_beam_id.empty()) {
      return nullptr;
    }
    auto it = analog_assignment_by_id.find(beam.analog_beam_id);
    return it != analog_assignment_by_id.end() ? &it->second : nullptr;
  };

  auto apply_policy = [&](ntn_beam_du_assignment& assignment, const ntn_beam_position& beam) {
    if (!analog_access_policy_enabled) {
      const std::string reason = assignment.du_index == du_index_t::invalid ? reason_no_supported_child : reason_eligible;
      set_du_policy(assignment, assignment.du_index, reason, reason);
      return;
    }

    const ntn_analog_access_du_assignment* access_assignment = get_access_assignment(beam);
    const du_index_t access_du =
        access_assignment != nullptr ? access_assignment->selected_access_du_index : du_index_t::invalid;
    const std::string access_reason =
        access_assignment != nullptr ? access_assignment->reason : std::string{reason_no_supported_child};
    set_du_policy(assignment,
                  access_du,
                  access_reason,
                  choose_du_assignment_reason(access_du, assignment.du_index, assignment.state, access_reason));
  };

  std::map<std::string, ntn_beam_du_assignment> planned_by_beam_id;
  std::map<std::string, analog_resource_usage>   analog_resource_usages;
  std::set<std::string>                          active_conflict_groups;
  unsigned                                      nof_active_beams = 0;
  for (const auto& entry : previous_service_assignments) {
    const std::string&                 beam_id             = entry.first;
    const ntn_beam_du_assignment& previous_assignment = entry.second;
    if (enabled_beams.count(beam_id) == 0 || !should_keep_draining(previous_assignment, loads)) {
      continue;
    }

    const bool beam_is_visible           = visible_elevations.count(beam_id) != 0;
    const bool beam_is_in_hopping_window = beam_is_visible && visible_in_hopping_window[beam_id];
    if (previous_assignment.state == ntn_beam_assignment_state::active_loaded && beam_is_in_hopping_window) {
      continue;
    }
    if (!can_retain_draining_on_previous_du(previous_assignment, du_usages)) {
      continue;
    }

    auto          du_it = du_usages.find(previous_assignment.du_index);
    ntn_beam_load load  = make_draining_load(previous_assignment, loads);
    add_service_beam_to_usage(du_it->second, load);
    ntn_beam_du_assignment assignment = make_assignment(enabled_beams.at(beam_id),
                                                        ntn_beam_assignment_state::draining,
                                                        previous_assignment.du_index,
                                                        beam_is_visible ? visible_elevations.at(beam_id)
                                                                        : previous_assignment.elevation_deg,
                                                        load,
                                                        false,
                                                        beam_is_visible ? visible_satellite_ids[beam_id]
                                                                        : previous_assignment.serving_satellite_id);
    const ntn_digital_beam_resource_policy& digital_policy = digital_policies.at(beam_id);
    reserve_resource_domain(enabled_beams.at(beam_id),
                            load,
                            digital_policy,
                            analog_resource_usages,
                            active_conflict_groups);
    apply_policy(assignment, enabled_beams.at(beam_id));
    populate_resource_domain_fields(assignment,
                                    enabled_beams.at(beam_id),
                                    load,
                                    digital_policy,
                                    analog_policies,
                                    analog_resource_usages,
                                    {true, reason_draining});
    planned_by_beam_id[beam_id] = std::move(assignment);
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
    const ntn_beam_position&                beam = enabled_beams.at(beam_id);
    const ntn_beam_load                     load = get_load(loads, beam_id);
    const bool                              beam_has_user_demand = has_user_demand(load);
    const bool                              beam_is_preheat_requested = preheated_beam_ids.count(beam_id) != 0;
    const bool beam_requires_service_assignment = beam_has_user_demand || beam_is_preheat_requested;
    const bool beam_can_host_required_service =
        (!load.downlink_service_required || beam.downlink_enabled) &&
        (!load.uplink_resource_required || beam.uplink_enabled);
    const ntn_digital_beam_resource_policy& digital_policy = digital_policies.at(beam_id);
    const resource_domain_evaluation resource_eval =
        beam_requires_service_assignment
            ? evaluate_resource_domain(beam,
                                       load,
                                       digital_policy,
                                       analog_policies,
                                       analog_resource_usages,
                                       active_conflict_groups)
            : resource_domain_evaluation{};

    if (beam_requires_service_assignment && !beam_can_host_required_service) {
      ntn_beam_du_assignment assignment = make_assignment(beam,
                                                          ntn_beam_assignment_state::candidate,
                                                          du_index_t::invalid,
                                                          visible_elevations.at(beam_id),
                                                          load,
                                                          true,
                                                          visible_satellite_ids[beam_id]);
      apply_policy(assignment, beam);
      assignment.du_assignment_reason = reason_link_direction_incomplete;
      populate_resource_domain_fields(assignment,
                                      beam,
                                      load,
                                      digital_policy,
                                      analog_policies,
                                      analog_resource_usages,
                                      {});
      planned_by_beam_id[beam_id] = std::move(assignment);
      continue;
    }

    if (!visible_in_hopping_window[beam_id]) {
      ntn_beam_du_assignment assignment = make_assignment(beam,
                                                          ntn_beam_assignment_state::candidate,
                                                          du_index_t::invalid,
                                                          visible_elevations.at(beam_id),
                                                          load,
                                                          false,
                                                          visible_satellite_ids[beam_id]);
      apply_policy(assignment, beam);
      populate_resource_domain_fields(assignment,
                                      beam,
                                      load,
                                      digital_policy,
                                      analog_policies,
                                      analog_resource_usages,
                                      resource_eval);
      planned_by_beam_id[beam_id] = std::move(assignment);
      continue;
    }

    if (beam_requires_service_assignment && !resource_eval.eligible) {
      ntn_beam_du_assignment assignment = make_assignment(beam,
                                                          ntn_beam_assignment_state::candidate,
                                                          du_index_t::invalid,
                                                          visible_elevations.at(beam_id),
                                                          load,
                                                          true,
                                                          visible_satellite_ids[beam_id]);
      apply_policy(assignment, beam);
      populate_resource_domain_fields(assignment,
                                      beam,
                                      load,
                                      digital_policy,
                                      analog_policies,
                                      analog_resource_usages,
                                      resource_eval);
      planned_by_beam_id[beam_id] = std::move(assignment);
      continue;
    }

    auto best_du_it = du_usages.end();
    if (!beam_requires_service_assignment || has_global_active_beam_capacity(request.max_active_beams, nof_active_beams)) {
      const auto previous_it = previous_service_assignments.find(beam_id);
      if (previous_it != previous_service_assignments.end()) {
        auto previous_du_it = du_usages.find(previous_it->second.du_index);
        if (previous_du_it != du_usages.end() && has_capacity_for(previous_du_it->second, beam_id, load)) {
          best_du_it = previous_du_it;
        }
      }
      if (best_du_it == du_usages.end() && analog_access_policy_enabled) {
        const ntn_analog_access_du_assignment* access_assignment = get_access_assignment(beam);
        if (access_assignment != nullptr && access_assignment->selected_access_du_index != du_index_t::invalid) {
          auto access_du_it = du_usages.find(access_assignment->selected_access_du_index);
          if (access_du_it != du_usages.end()) {
            if (beam_requires_service_assignment) {
              if (has_capacity_for(access_du_it->second, beam_id, load)) {
                best_du_it = access_du_it;
              }
            } else if (analog_du_has_service_capacity(access_du_it->second) &&
                       supports_beam(access_du_it->second.capacity, beam_id)) {
              best_du_it = access_du_it;
            }
          }
        }
      }
      if (best_du_it == du_usages.end() && !analog_access_policy_enabled) {
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
      ntn_beam_du_assignment assignment = make_assignment(beam,
                                                          ntn_beam_assignment_state::candidate,
                                                          du_index_t::invalid,
                                                          visible_elevations.at(beam_id),
                                                          load,
                                                          true,
                                                          visible_satellite_ids[beam_id]);
      apply_policy(assignment, beam);
      if (analog_access_policy_enabled && assignment.access_du_index != du_index_t::invalid) {
        auto access_du_it = du_usages.find(assignment.access_du_index);
        if (access_du_it != du_usages.end() && analog_du_has_service_capacity(access_du_it->second) &&
            supports_beam(access_du_it->second.capacity, beam_id)) {
          assignment.du_assignment_reason = reason_du_capacity_exhausted;
        }
      }
      populate_resource_domain_fields(assignment,
                                      beam,
                                      load,
                                      digital_policy,
                                      analog_policies,
                                      analog_resource_usages,
                                      resource_eval);
      planned_by_beam_id[beam_id] = std::move(assignment);
      continue;
    }

    if (!beam_requires_service_assignment) {
      ntn_beam_du_assignment assignment = make_assignment(beam,
                                                          ntn_beam_assignment_state::candidate,
                                                          best_du_it->first,
                                                          visible_elevations.at(beam_id),
                                                          load,
                                                          true,
                                                          visible_satellite_ids[beam_id]);
      apply_policy(assignment, beam);
      populate_resource_domain_fields(assignment,
                                      beam,
                                      load,
                                      digital_policy,
                                      analog_policies,
                                      analog_resource_usages,
                                      resource_eval);
      planned_by_beam_id[beam_id] = std::move(assignment);
      continue;
    }

    add_service_beam_to_usage(best_du_it->second, load);
    ++nof_active_beams;
    reserve_resource_domain(beam, load, digital_policy, analog_resource_usages, active_conflict_groups);
    ntn_beam_du_assignment assignment = make_assignment(beam,
                                                        ntn_beam_assignment_state::active_loaded,
                                                        best_du_it->first,
                                                        visible_elevations.at(beam_id),
                                                        load,
                                                        true,
                                                        visible_satellite_ids[beam_id]);
    apply_policy(assignment, beam);
    populate_resource_domain_fields(assignment,
                                    beam,
                                    load,
                                    digital_policy,
                                    analog_policies,
                                    analog_resource_usages,
                                    resource_eval);
    planned_by_beam_id[beam_id] = std::move(assignment);
  }

  result.assignments.reserve(enabled_beams.size());
  for (const auto& entry : enabled_beams) {
    auto planned_it = planned_by_beam_id.find(entry.first);
    if (planned_it != planned_by_beam_id.end()) {
      result.assignments.push_back(planned_it->second);
      continue;
    }
    ntn_beam_du_assignment assignment = make_assignment(
        entry.second, ntn_beam_assignment_state::inactive, du_index_t::invalid, 0.0, get_load(loads, entry.first));
    apply_policy(assignment, entry.second);
    const ntn_beam_load load = get_load(loads, entry.first);
    populate_resource_domain_fields(assignment,
                                    entry.second,
                                    load,
                                    digital_policies.at(entry.first),
                                    analog_policies,
                                    analog_resource_usages,
                                    {});
    result.assignments.push_back(std::move(assignment));
  }

  assign_user_driven_antenna_slots(result, request.demand_aware_resource_weighting_enabled);

  return result;
}
