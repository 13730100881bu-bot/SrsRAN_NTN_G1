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

#include "lib/cu_cp/ntn_mobility/ntn_qos_policy.h"
#include "srsran/cu_cp/cu_cp_types.h"
#include "srsran/cu_cp/up_context.h"
#include <gtest/gtest.h>

using namespace srsran;
using namespace srs_cu_cp;

namespace {

qos_flow_setup_request_item make_qos_flow(qos_flow_id_t qfi,
                                          unsigned      five_qi,
                                          unsigned      arp,
                                          bool          may_trigger_preemption = false,
                                          bool          is_preemptable = false,
                                          bool          include_gbr_info = false)
{
  qos_flow_setup_request_item item;
  item.qos_flow_id = qfi;

  non_dyn_5qi_descriptor non_dyn_5qi;
  non_dyn_5qi.five_qi                         = uint_to_five_qi(five_qi);
  item.qos_flow_level_qos_params.qos_desc     = non_dyn_5qi;
  item.qos_flow_level_qos_params.alloc_retention_prio.prio_level_arp = arp_prio_level_t{static_cast<uint8_t>(arp)};
  item.qos_flow_level_qos_params.alloc_retention_prio.may_trigger_preemption = may_trigger_preemption;
  item.qos_flow_level_qos_params.alloc_retention_prio.is_preemptable         = is_preemptable;
  if (include_gbr_info) {
    item.qos_flow_level_qos_params.gbr_qos_info =
        gbr_qos_flow_information{1000000, 1000000, 500000, 500000, std::nullopt, std::nullopt};
  }
  return item;
}

cu_cp_pdu_session_resource_setup_request make_setup_request()
{
  cu_cp_pdu_session_resource_setup_request request;
  request.ue_index = uint_to_ue_index(0);

  cu_cp_pdu_session_res_setup_item session;
  session.pdu_session_id = uint_to_pdu_session_id(1);
  session.s_nssai.sst    = slice_service_type{1};
  session.s_nssai.sd     = slice_differentiator::create(0x10203).value();
  session.qos_flow_setup_request_items.emplace(uint_to_qos_flow_id(1),
                                               make_qos_flow(uint_to_qos_flow_id(1), 9, 15));
  session.qos_flow_setup_request_items.emplace(uint_to_qos_flow_id(2),
                                               make_qos_flow(uint_to_qos_flow_id(2), 65, 1, true, false, true));
  request.pdu_session_res_setup_items.emplace(session.pdu_session_id, session);
  return request;
}

qos_flow_level_qos_parameters make_dynamic_qos_params(unsigned qos_priority = 3)
{
  qos_flow_level_qos_parameters params;
  dyn_5qi_descriptor            dyn_5qi;
  dyn_5qi.qos_prio_level      = qos_prio_level_t{static_cast<uint8_t>(qos_priority)};
  dyn_5qi.packet_delay_budget = 40;
  dyn_5qi.per                 = packet_error_rate_t::make(1e-4);
  dyn_5qi.is_delay_critical   = true;
  params.qos_desc             = dyn_5qi;
  params.alloc_retention_prio.prio_level_arp         = arp_prio_level_t{4};
  params.alloc_retention_prio.may_trigger_preemption = true;
  params.gbr_qos_info = gbr_qos_flow_information{2000000, 2000000, 1000000, 1000000, std::nullopt, std::nullopt};
  return params;
}

} // namespace

TEST(ntn_qos_policy, summarizes_pdu_session_setup_request_priority_and_slice)
{
  const ntn_qos_demand_summary summary = summarize_ntn_qos_demand(make_setup_request());

  ASSERT_TRUE(summary.has_qos_demand);
  EXPECT_EQ(summary.nof_qos_flows, 2U);
  EXPECT_EQ(summary.best_arp_priority, 1U);
  EXPECT_EQ(summary.best_qos_priority, 7U);
  EXPECT_EQ(summary.best_five_qi, uint_to_five_qi(65));
  EXPECT_TRUE(summary.has_gbr);
  EXPECT_FALSE(summary.has_delay_critical_gbr);
  EXPECT_TRUE(summary.may_trigger_preemption);
  ASSERT_TRUE(summary.primary_s_nssai.has_value());
  EXPECT_EQ(summary.primary_s_nssai->sst, slice_service_type{1});
}

TEST(ntn_qos_policy, dynamic_5qi_priority_is_used_when_standardized_5qi_is_absent)
{
  ntn_qos_demand_summary summary;
  merge_ntn_qos_demand(summary, make_dynamic_qos_params(), std::nullopt);

  ASSERT_TRUE(summary.has_qos_demand);
  EXPECT_EQ(summary.nof_qos_flows, 1U);
  EXPECT_EQ(summary.best_arp_priority, 4U);
  EXPECT_EQ(summary.best_qos_priority, 3U);
  EXPECT_EQ(summary.best_five_qi, five_qi_t::invalid);
  EXPECT_TRUE(summary.has_gbr);
  EXPECT_TRUE(summary.has_delay_critical_gbr);
  EXPECT_TRUE(summary.may_trigger_preemption);
}

TEST(ntn_qos_policy, zero_dynamic_5qi_priority_is_kept_as_highest_priority)
{
  ntn_qos_demand_summary summary;
  merge_ntn_qos_demand(summary, make_dynamic_qos_params(0), std::nullopt);

  ASSERT_TRUE(summary.has_qos_demand);
  EXPECT_EQ(summary.best_qos_priority, 0U);
}

TEST(ntn_qos_policy, summarizes_committed_up_context_from_drb_qos_flows)
{
  up_context context;
  up_pdu_session_context session(uint_to_pdu_session_id(1));
  session.id = uint_to_pdu_session_id(1);

  up_drb_context drb;
  drb.drb_id         = drb_id_t::drb1;
  drb.pdu_session_id = session.id;
  drb.s_nssai.sst    = slice_service_type{2};
  drb.qos_params     = make_dynamic_qos_params();

  up_qos_flow_context flow;
  flow.qfi        = uint_to_qos_flow_id(5);
  flow.qos_params = make_qos_flow(uint_to_qos_flow_id(5), 69, 2, true).qos_flow_level_qos_params;
  drb.qos_flows.emplace(flow.qfi, flow);
  session.drbs.emplace(drb.drb_id, drb);
  context.pdu_sessions.emplace(session.id, session);

  const ntn_qos_demand_summary summary = summarize_ntn_qos_demand(context);

  ASSERT_TRUE(summary.has_qos_demand);
  EXPECT_EQ(summary.nof_qos_flows, 1U);
  EXPECT_EQ(summary.best_arp_priority, 2U);
  EXPECT_EQ(summary.best_qos_priority, 5U);
  EXPECT_EQ(summary.best_five_qi, uint_to_five_qi(69));
  EXPECT_FALSE(summary.has_gbr);
  EXPECT_TRUE(summary.may_trigger_preemption);
  ASSERT_TRUE(summary.primary_s_nssai.has_value());
  EXPECT_EQ(summary.primary_s_nssai->sst, slice_service_type{2});
}

TEST(ntn_qos_policy, comparator_prefers_preemptive_arp_then_delay_critical_gbr)
{
  ntn_qos_demand_summary preemptive;
  preemptive.has_qos_demand        = true;
  preemptive.best_arp_priority     = 5;
  preemptive.best_qos_priority     = 40;
  preemptive.may_trigger_preemption = true;

  ntn_qos_demand_summary low_arp;
  low_arp.has_qos_demand    = true;
  low_arp.best_arp_priority = 1;
  low_arp.best_qos_priority = 7;
  low_arp.has_gbr           = true;

  EXPECT_TRUE(is_ntn_qos_demand_higher_priority(preemptive, low_arp));
  EXPECT_FALSE(is_ntn_qos_demand_higher_priority(low_arp, preemptive));

  preemptive.may_trigger_preemption = false;
  low_arp.has_delay_critical_gbr    = true;
  EXPECT_TRUE(is_ntn_qos_demand_higher_priority(low_arp, preemptive));
}
