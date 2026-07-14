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

#include "ntn_qos_policy.h"
#include "srsran/ran/qos/five_qi_qos_mapping.h"

using namespace srsran;
using namespace srs_cu_cp;

namespace {

unsigned get_qos_priority(const qos_characteristics& qos_desc)
{
  if (qos_desc.is_dyn_5qi()) {
    return qos_desc.get_dyn_5qi().qos_prio_level.value();
  }

  const non_dyn_5qi_descriptor& non_dyn = qos_desc.get_nondyn_5qi();
  if (non_dyn.qos_prio_level.has_value()) {
    return non_dyn.qos_prio_level->value();
  }

  const standardized_qos_characteristics* mapping = get_5qi_to_qos_characteristics_mapping(non_dyn.five_qi);
  return mapping != nullptr ? mapping->priority.value() : 127;
}

qos_flow_resource_type get_resource_type(const qos_flow_level_qos_parameters& qos_params)
{
  if (qos_params.qos_desc.is_dyn_5qi()) {
    const dyn_5qi_descriptor& dyn = qos_params.qos_desc.get_dyn_5qi();
    if (dyn.is_delay_critical.has_value() && dyn.is_delay_critical.value()) {
      return qos_flow_resource_type::delay_critical_gbr;
    }
    return qos_params.gbr_qos_info.has_value() ? qos_flow_resource_type::gbr : qos_flow_resource_type::non_gbr;
  }

  const standardized_qos_characteristics* mapping =
      get_5qi_to_qos_characteristics_mapping(qos_params.qos_desc.get_nondyn_5qi().five_qi);
  if (mapping != nullptr) {
    return mapping->res_type;
  }
  return qos_params.gbr_qos_info.has_value() ? qos_flow_resource_type::gbr : qos_flow_resource_type::non_gbr;
}

bool qos_priority_is_better(unsigned lhs, unsigned rhs)
{
  return lhs < rhs;
}

} // namespace

bool srsran::srs_cu_cp::is_ntn_qos_demand_higher_priority(const ntn_qos_demand_summary& lhs,
                                                           const ntn_qos_demand_summary& rhs)
{
  if (lhs.has_qos_demand != rhs.has_qos_demand) {
    return lhs.has_qos_demand;
  }
  if (!lhs.has_qos_demand) {
    return false;
  }
  if (lhs.may_trigger_preemption != rhs.may_trigger_preemption) {
    return lhs.may_trigger_preemption;
  }
  if (lhs.best_arp_priority != rhs.best_arp_priority) {
    return qos_priority_is_better(lhs.best_arp_priority, rhs.best_arp_priority);
  }
  if (lhs.has_delay_critical_gbr != rhs.has_delay_critical_gbr) {
    return lhs.has_delay_critical_gbr;
  }
  if (lhs.has_gbr != rhs.has_gbr) {
    return lhs.has_gbr;
  }
  if (lhs.best_qos_priority != rhs.best_qos_priority) {
    return qos_priority_is_better(lhs.best_qos_priority, rhs.best_qos_priority);
  }
  return false;
}

void srsran::srs_cu_cp::merge_ntn_qos_demand(ntn_qos_demand_summary&              summary,
                                             const qos_flow_level_qos_parameters& qos_params,
                                             const std::optional<s_nssai_t>&      s_nssai)
{
  summary.has_qos_demand = true;
  ++summary.nof_qos_flows;

  const unsigned arp_priority = qos_params.alloc_retention_prio.prio_level_arp.value();
  if (qos_priority_is_better(arp_priority, summary.best_arp_priority)) {
    summary.best_arp_priority = arp_priority;
  }

  const unsigned qos_priority = get_qos_priority(qos_params.qos_desc);
  if (qos_priority_is_better(qos_priority, summary.best_qos_priority)) {
    summary.best_qos_priority = qos_priority;
  }

  const five_qi_t five_qi = qos_params.qos_desc.get_5qi();
  if (five_qi != five_qi_t::invalid &&
      (summary.best_five_qi == five_qi_t::invalid || qos_priority == summary.best_qos_priority)) {
    summary.best_five_qi = five_qi;
  }

  const qos_flow_resource_type resource_type = get_resource_type(qos_params);
  summary.has_gbr =
      summary.has_gbr || resource_type == qos_flow_resource_type::gbr ||
      resource_type == qos_flow_resource_type::delay_critical_gbr || qos_params.gbr_qos_info.has_value();
  summary.has_delay_critical_gbr =
      summary.has_delay_critical_gbr || resource_type == qos_flow_resource_type::delay_critical_gbr;
  summary.may_trigger_preemption =
      summary.may_trigger_preemption || qos_params.alloc_retention_prio.may_trigger_preemption;
  summary.is_preemptable = summary.is_preemptable || qos_params.alloc_retention_prio.is_preemptable;

  if (!summary.primary_s_nssai.has_value() && s_nssai.has_value()) {
    summary.primary_s_nssai = s_nssai;
  }
}

ntn_qos_demand_summary
srsran::srs_cu_cp::summarize_ntn_qos_demand(const cu_cp_pdu_session_resource_setup_request& request)
{
  ntn_qos_demand_summary summary;
  for (const auto& session : request.pdu_session_res_setup_items) {
    for (const auto& flow : session.qos_flow_setup_request_items) {
      merge_ntn_qos_demand(summary, flow.qos_flow_level_qos_params, session.s_nssai);
    }
  }
  return summary;
}

ntn_qos_demand_summary srsran::srs_cu_cp::summarize_ntn_qos_demand(const up_context& context)
{
  ntn_qos_demand_summary summary;
  for (const auto& session : context.pdu_sessions) {
    for (const auto& drb : session.second.drbs) {
      if (drb.second.qos_flows.empty()) {
        merge_ntn_qos_demand(summary, drb.second.qos_params, drb.second.s_nssai);
        continue;
      }
      for (const auto& flow : drb.second.qos_flows) {
        merge_ntn_qos_demand(summary, flow.second.qos_params, drb.second.s_nssai);
      }
    }
  }
  return summary;
}
