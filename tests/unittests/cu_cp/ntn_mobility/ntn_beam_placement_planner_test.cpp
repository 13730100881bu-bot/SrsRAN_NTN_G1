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

#include "lib/cu_cp/ntn_mobility/ntn_beam_assignment_repository.h"
#include "lib/cu_cp/ntn_mobility/ntn_beam_placement_planner.h"
#include "srsran/cu_cp/ntn_qos_policy.h"
#include "srsran/ran/gnb_id.h"
#include <algorithm>
#include <gtest/gtest.h>
#include <optional>

using namespace srsran;
using namespace srs_cu_cp;

namespace {

nr_cell_identity make_nci(unsigned sector_id)
{
  return nr_cell_identity::create(gnb_id_t{0x19b, 32}, sector_id).value();
}

ntn_beam_position make_beam(const std::string& beam_id, unsigned sector_id)
{
  return {beam_id, make_nci(sector_id), 30.0, 110.0 + sector_id, 45000.0, true};
}

ntn_beam_position make_analog_child_beam(const std::string& beam_id, unsigned sector_id, const std::string& analog_id)
{
  ntn_beam_position beam = make_beam(beam_id, sector_id);
  beam.analog_beam_id    = analog_id;
  return beam;
}

ntn_analog_beam_position make_analog_beam(const std::string&              analog_id,
                                          const std::vector<std::string>& child_beam_ids)
{
  ntn_analog_beam_position analog;
  analog.analog_beam_id         = analog_id;
  analog.center_digital_beam_id = child_beam_ids.empty() ? "" : child_beam_ids.front();
  analog.child_digital_beam_ids = child_beam_ids;
  return analog;
}

ntn_qos_demand_summary make_priority(unsigned arp, unsigned qos_priority, bool may_preempt = false)
{
  ntn_qos_demand_summary summary;
  summary.has_qos_demand         = true;
  summary.best_arp_priority      = arp;
  summary.best_qos_priority      = qos_priority;
  summary.may_trigger_preemption = may_preempt;
  summary.nof_qos_flows          = 1;
  return summary;
}

ntn_qos_demand_summary make_delay_critical_gbr_priority()
{
  ntn_qos_demand_summary summary = make_priority(1, 7, true);
  summary.has_gbr                = true;
  summary.has_delay_critical_gbr = true;
  return summary;
}

ntn_beam_du_assignment find_assignment(const ntn_beam_placement_plan& plan, const std::string& beam_id)
{
  auto it = std::find_if(plan.assignments.begin(),
                         plan.assignments.end(),
                         [&beam_id](const ntn_beam_du_assignment& assignment) {
                           return assignment.beam_id == beam_id;
                         });
  EXPECT_NE(it, plan.assignments.end());
  return it != plan.assignments.end() ? *it : ntn_beam_du_assignment{};
}

} // namespace

TEST(ntn_beam_runtime_taxonomy, names_active_loaded_without_collapsing_it_to_active)
{
  EXPECT_EQ(ntn_beam_assignment_state_to_string(ntn_beam_assignment_state::inactive), "inactive");
  EXPECT_EQ(ntn_beam_assignment_state_to_string(ntn_beam_assignment_state::candidate), "candidate");
  EXPECT_EQ(ntn_beam_assignment_state_to_string(ntn_beam_assignment_state::active_loaded), "active_loaded");
  EXPECT_EQ(ntn_beam_assignment_state_to_string(ntn_beam_assignment_state::draining), "draining");
}

TEST(ntn_beam_placement_planner, keeps_previous_active_loaded_assignment_when_still_visible)
{
  ntn_beam_placement_request request;
  request.beams         = {make_beam("CN-BEAM-0001", 1), make_beam("CN-BEAM-0002", 2)};
  request.visible_beams = {{"CN-BEAM-0001", 72.0}, {"CN-BEAM-0002", 60.0}};
  request.du_capacities = {{uint_to_du_index(0), 2, 0, 0, {}}, {uint_to_du_index(1), 2, 0, 0, {}}};
  request.beam_loads    = {{"CN-BEAM-0001", 8, 16}, {"CN-BEAM-0002", 1, 2}};
  request.previous_assignments = {{"CN-BEAM-0001",
                                   make_nci(1),
                                   uint_to_du_index(1),
                                   ntn_beam_assignment_state::active_loaded,
                                   70.0,
                                   8,
                                   16}};

  const ntn_beam_placement_plan plan = ntn_beam_placement_planner{}.plan(request);

  const ntn_beam_du_assignment beam1 = find_assignment(plan, "CN-BEAM-0001");
  EXPECT_EQ(beam1.state, ntn_beam_assignment_state::active_loaded);
  EXPECT_EQ(beam1.du_index, uint_to_du_index(1));
  EXPECT_EQ(beam1.elevation_deg, 72.0);
  EXPECT_EQ(beam1.nof_ues, 8U);
  EXPECT_EQ(beam1.nof_drbs, 16U);
}

TEST(ntn_beam_placement_planner, assigns_new_visible_beam_to_least_loaded_feasible_du)
{
  ntn_beam_placement_request request;
  request.beams         = {make_beam("CN-BEAM-0001", 1), make_beam("CN-BEAM-0002", 2)};
  request.visible_beams = {{"CN-BEAM-0001", 70.0}, {"CN-BEAM-0002", 69.0}};
  request.du_capacities = {{uint_to_du_index(0), 2, 0, 0, {}}, {uint_to_du_index(1), 2, 0, 0, {}}};
  request.beam_loads    = {{"CN-BEAM-0001", 5, 10}, {"CN-BEAM-0002", 1, 1}};
  request.previous_assignments = {{"CN-BEAM-0001",
                                   make_nci(1),
                                   uint_to_du_index(0),
                                   ntn_beam_assignment_state::active_loaded,
                                   70.0,
                                   5,
                                   10}};

  const ntn_beam_placement_plan plan = ntn_beam_placement_planner{}.plan(request);

  const ntn_beam_du_assignment beam2 = find_assignment(plan, "CN-BEAM-0002");
  EXPECT_EQ(beam2.state, ntn_beam_assignment_state::active_loaded);
  EXPECT_EQ(beam2.du_index, uint_to_du_index(1));
}

TEST(ntn_beam_placement_planner, assigns_multi_beam_hopping_window_across_feasible_dus)
{
  ntn_beam_placement_request request;
  request.beams         = {make_beam("CN-BEAM-0001", 1),
                           make_beam("CN-BEAM-0002", 2),
                           make_beam("CN-BEAM-0003", 3)};
  request.visible_beams = {{"CN-BEAM-0001", 70.0}, {"CN-BEAM-0002", 69.0}, {"CN-BEAM-0003", 68.0}};
  request.du_capacities = {{uint_to_du_index(0), 2, 0, 0, {"CN-BEAM-0001", "CN-BEAM-0003"}},
                           {uint_to_du_index(1), 1, 0, 0, {"CN-BEAM-0002"}}};
  request.beam_loads    = {{"CN-BEAM-0001", 8, 16}, {"CN-BEAM-0002", 4, 8}, {"CN-BEAM-0003", 1, 2}};

  const ntn_beam_placement_plan plan = ntn_beam_placement_planner{}.plan(request);

  const ntn_beam_du_assignment beam1 = find_assignment(plan, "CN-BEAM-0001");
  EXPECT_EQ(beam1.state, ntn_beam_assignment_state::active_loaded);
  EXPECT_EQ(beam1.du_index, uint_to_du_index(0));

  const ntn_beam_du_assignment beam2 = find_assignment(plan, "CN-BEAM-0002");
  EXPECT_EQ(beam2.state, ntn_beam_assignment_state::active_loaded);
  EXPECT_EQ(beam2.du_index, uint_to_du_index(1));

  const ntn_beam_du_assignment beam3 = find_assignment(plan, "CN-BEAM-0003");
  EXPECT_EQ(beam3.state, ntn_beam_assignment_state::active_loaded);
  EXPECT_EQ(beam3.du_index, uint_to_du_index(0));

  EXPECT_EQ(get_active_loaded_ntn_beam_ids(plan, request.visible_beams),
            std::vector<std::string>({"CN-BEAM-0001", "CN-BEAM-0002", "CN-BEAM-0003"}));
}

TEST(ntn_beam_placement_planner, allocates_antenna_slots_only_to_loaded_service_beams)
{
  ntn_beam_placement_request request;
  request.beams         = {make_beam("CN-BEAM-0001", 1),
                           make_beam("CN-BEAM-0002", 2),
                           make_beam("CN-BEAM-0003", 3)};
  request.visible_beams = {{"CN-BEAM-0001", 72.0}, {"CN-BEAM-0002", 71.0}, {"CN-BEAM-0003", 70.0}};
  request.du_capacities = {{uint_to_du_index(0), 3, 0, 0, {}}};
  request.beam_loads    = {{"CN-BEAM-0002", 1, 1}, {"CN-BEAM-0003", 3, 1}};

  const ntn_beam_placement_plan plan = ntn_beam_placement_planner{}.plan(request);

  const ntn_beam_du_assignment beam1 = find_assignment(plan, "CN-BEAM-0001");
  EXPECT_EQ(beam1.state, ntn_beam_assignment_state::candidate);
  EXPECT_EQ(beam1.du_index, uint_to_du_index(0));
  EXPECT_EQ(beam1.nof_antenna_slots, 0U);
  EXPECT_EQ(beam1.antenna_slot_period, 0U);
  EXPECT_EQ(beam1.sr_slot_period, 0U);
  EXPECT_EQ(beam1.srs_slot_period, 0U);

  const ntn_beam_du_assignment beam2 = find_assignment(plan, "CN-BEAM-0002");
  EXPECT_EQ(beam2.state, ntn_beam_assignment_state::active_loaded);
  EXPECT_EQ(beam2.nof_antenna_slots, 1U);
  EXPECT_EQ(beam2.antenna_slot_index, 3U);
  EXPECT_EQ(beam2.antenna_slot_period, 4U);
  EXPECT_EQ(beam2.sr_slot_offset, 3U);
  EXPECT_EQ(beam2.sr_slot_period, 4U);
  EXPECT_EQ(beam2.srs_slot_offset, 3U);
  EXPECT_EQ(beam2.srs_slot_period, 4U);

  const ntn_beam_du_assignment beam3 = find_assignment(plan, "CN-BEAM-0003");
  EXPECT_EQ(beam3.state, ntn_beam_assignment_state::active_loaded);
  EXPECT_EQ(beam3.nof_antenna_slots, 3U);
  EXPECT_EQ(beam3.antenna_slot_index, 0U);
  EXPECT_EQ(beam3.antenna_slot_period, 4U);
  EXPECT_EQ(beam3.sr_slot_offset, 0U);
  EXPECT_EQ(beam3.sr_slot_period, 4U);
  EXPECT_EQ(beam3.srs_slot_offset, 2U);
  EXPECT_EQ(beam3.srs_slot_period, 4U);

  EXPECT_EQ(get_active_loaded_ntn_beam_ids(plan, request.visible_beams),
            std::vector<std::string>({"CN-BEAM-0002", "CN-BEAM-0003"}));
  EXPECT_EQ(get_mobility_eligible_ntn_beam_ids(plan, request.visible_beams),
            std::vector<std::string>({"CN-BEAM-0001", "CN-BEAM-0002", "CN-BEAM-0003"}));
}

TEST(ntn_beam_placement_planner, preheated_empty_beam_uses_active_loaded_capacity_until_demoted)
{
  ntn_beam_placement_request request;
  request.beams              = {make_beam("CN-BEAM-0001", 1), make_beam("CN-BEAM-0002", 2)};
  request.visible_beams      = {{"CN-BEAM-0001", 72.0}, {"CN-BEAM-0002", 71.0}};
  request.du_capacities      = {{uint_to_du_index(0), 2, 0, 0, {}}};
  request.preheated_beam_ids = {"CN-BEAM-0002"};

  const ntn_beam_placement_plan preheated_plan = ntn_beam_placement_planner{}.plan(request);
  const ntn_beam_du_assignment  preheated_beam = find_assignment(preheated_plan, "CN-BEAM-0002");
  EXPECT_EQ(preheated_beam.state, ntn_beam_assignment_state::active_loaded);
  EXPECT_EQ(preheated_beam.du_index, uint_to_du_index(0));
  EXPECT_EQ(preheated_beam.nof_ues, 0U);
  EXPECT_EQ(preheated_beam.nof_drbs, 0U);

  request.preheated_beam_ids.clear();
  request.previous_assignments = preheated_plan.assignments;
  const ntn_beam_placement_plan demoted_plan = ntn_beam_placement_planner{}.plan(request);
  const ntn_beam_du_assignment  demoted_beam = find_assignment(demoted_plan, "CN-BEAM-0002");
  EXPECT_EQ(demoted_beam.state, ntn_beam_assignment_state::candidate);
}

TEST(ntn_beam_placement_planner, service_demand_requires_bidirectional_link_ready_beam)
{
  ntn_beam_position downlink_only = make_beam("DL-ONLY", 1);
  downlink_only.uplink_enabled    = false;
  ntn_beam_position uplink_only   = make_beam("UL-ONLY", 2);
  uplink_only.downlink_enabled    = false;
  ntn_beam_position bidirectional = make_beam("BOTH", 3);

  ntn_beam_placement_request request;
  request.beams         = {downlink_only, uplink_only, bidirectional};
  request.visible_beams = {{"DL-ONLY", 72.0}, {"UL-ONLY", 71.0}, {"BOTH", 70.0}};
  request.du_capacities = {{uint_to_du_index(0), 3, 0, 0, {}}};
  request.beam_loads    = {{"DL-ONLY", 1, 1}, {"UL-ONLY", 1, 1}, {"BOTH", 1, 1}};

  const ntn_beam_placement_plan plan = ntn_beam_placement_planner{}.plan(request);

  const ntn_beam_du_assignment dl_only = find_assignment(plan, "DL-ONLY");
  EXPECT_EQ(dl_only.state, ntn_beam_assignment_state::candidate);
  EXPECT_TRUE(dl_only.downlink_enabled);
  EXPECT_FALSE(dl_only.uplink_enabled);
  EXPECT_TRUE(dl_only.downlink_visible);
  EXPECT_FALSE(dl_only.uplink_access_ready);
  EXPECT_FALSE(dl_only.access_roundtrip_ready);
  EXPECT_TRUE(dl_only.downlink_ready);
  EXPECT_FALSE(dl_only.uplink_ready);
  EXPECT_FALSE(dl_only.bidirectional_service_ready);

  const ntn_beam_du_assignment ul_only = find_assignment(plan, "UL-ONLY");
  EXPECT_EQ(ul_only.state, ntn_beam_assignment_state::candidate);
  EXPECT_FALSE(ul_only.downlink_enabled);
  EXPECT_TRUE(ul_only.uplink_enabled);
  EXPECT_FALSE(ul_only.downlink_visible);
  EXPECT_TRUE(ul_only.uplink_access_ready);
  EXPECT_FALSE(ul_only.access_roundtrip_ready);
  EXPECT_FALSE(ul_only.downlink_ready);
  EXPECT_FALSE(ul_only.uplink_ready);
  EXPECT_FALSE(ul_only.bidirectional_service_ready);

  const ntn_beam_du_assignment both = find_assignment(plan, "BOTH");
  EXPECT_EQ(both.state, ntn_beam_assignment_state::active_loaded);
  EXPECT_TRUE(both.downlink_enabled);
  EXPECT_TRUE(both.uplink_enabled);
  EXPECT_TRUE(both.downlink_visible);
  EXPECT_TRUE(both.uplink_access_ready);
  EXPECT_TRUE(both.access_roundtrip_ready);
  EXPECT_TRUE(both.downlink_ready);
  EXPECT_TRUE(both.uplink_ready);
  EXPECT_TRUE(both.bidirectional_service_ready);

  EXPECT_EQ(get_active_loaded_ntn_beam_ids(plan, request.visible_beams), std::vector<std::string>({"BOTH"}));
}

TEST(ntn_beam_placement_planner, allocates_one_antenna_slot_for_drb_only_service_beam)
{
  ntn_beam_placement_request request;
  request.beams         = {make_beam("CN-BEAM-0001", 1)};
  request.visible_beams = {{"CN-BEAM-0001", 72.0}};
  request.du_capacities = {{uint_to_du_index(0), 1, 0, 0, {}}};
  request.beam_loads    = {{"CN-BEAM-0001", 0, 2}};

  const ntn_beam_placement_plan plan = ntn_beam_placement_planner{}.plan(request);

  const ntn_beam_du_assignment beam1 = find_assignment(plan, "CN-BEAM-0001");
  EXPECT_EQ(beam1.state, ntn_beam_assignment_state::active_loaded);
  EXPECT_EQ(beam1.nof_antenna_slots, 1U);
  EXPECT_EQ(beam1.antenna_slot_index, 0U);
  EXPECT_EQ(beam1.antenna_slot_period, 1U);
  EXPECT_EQ(beam1.sr_slot_offset, 0U);
  EXPECT_EQ(beam1.sr_slot_period, 1U);
  EXPECT_EQ(beam1.srs_slot_offset, 0U);
  EXPECT_EQ(beam1.srs_slot_period, 1U);
}

TEST(ntn_beam_placement_planner, leaves_overflow_multi_beam_candidate_out_of_active_loaded_set)
{
  ntn_beam_placement_request request;
  request.beams         = {make_beam("CN-BEAM-0001", 1),
                           make_beam("CN-BEAM-0002", 2),
                           make_beam("CN-BEAM-0003", 3)};
  request.visible_beams = {{"CN-BEAM-0001", 70.0}, {"CN-BEAM-0002", 69.0}, {"CN-BEAM-0003", 68.0}};
  request.du_capacities = {{uint_to_du_index(0), 2, 0, 0, {}}};
  request.beam_loads    = {{"CN-BEAM-0001", 4, 4}, {"CN-BEAM-0002", 3, 3}, {"CN-BEAM-0003", 2, 2}};

  const ntn_beam_placement_plan plan = ntn_beam_placement_planner{}.plan(request);

  EXPECT_EQ(find_assignment(plan, "CN-BEAM-0001").state, ntn_beam_assignment_state::active_loaded);
  EXPECT_EQ(find_assignment(plan, "CN-BEAM-0002").state, ntn_beam_assignment_state::active_loaded);

  const ntn_beam_du_assignment beam3 = find_assignment(plan, "CN-BEAM-0003");
  EXPECT_EQ(beam3.state, ntn_beam_assignment_state::candidate);
  EXPECT_EQ(beam3.du_index, du_index_t::invalid);

  EXPECT_EQ(get_active_loaded_ntn_beam_ids(plan, request.visible_beams),
            std::vector<std::string>({"CN-BEAM-0001", "CN-BEAM-0002"}));
}

TEST(ntn_beam_placement_planner, zero_global_active_loaded_cap_does_not_limit_service_beams)
{
  ntn_beam_placement_request request;
  request.beams         = {make_beam("CN-BEAM-0001", 1),
                           make_beam("CN-BEAM-0002", 2),
                           make_beam("CN-BEAM-0003", 3)};
  request.visible_beams = {{"CN-BEAM-0001", 70.0}, {"CN-BEAM-0002", 69.0}, {"CN-BEAM-0003", 68.0}};
  request.max_active_beams = 0;
  request.du_capacities    = {{uint_to_du_index(0), 3, 0, 0, {}}};
  request.beam_loads       = {{"CN-BEAM-0001", 4, 4}, {"CN-BEAM-0002", 3, 3}, {"CN-BEAM-0003", 2, 2}};

  const ntn_beam_placement_plan plan = ntn_beam_placement_planner{}.plan(request);

  EXPECT_EQ(find_assignment(plan, "CN-BEAM-0001").state, ntn_beam_assignment_state::active_loaded);
  EXPECT_EQ(find_assignment(plan, "CN-BEAM-0002").state, ntn_beam_assignment_state::active_loaded);
  EXPECT_EQ(find_assignment(plan, "CN-BEAM-0003").state, ntn_beam_assignment_state::active_loaded);
  EXPECT_EQ(get_active_loaded_ntn_beam_ids(plan, request.visible_beams),
            std::vector<std::string>({"CN-BEAM-0001", "CN-BEAM-0002", "CN-BEAM-0003"}));
}

TEST(ntn_beam_placement_planner, prioritizes_qos_when_active_loaded_capacity_is_limited)
{
  ntn_beam_placement_request request;
  request.beams         = {make_beam("CN-BEAM-LOW", 1), make_beam("CN-BEAM-HIGH", 2)};
  request.visible_beams = {{"CN-BEAM-LOW", 80.0}, {"CN-BEAM-HIGH", 60.0}};
  request.max_active_beams = 1;
  request.du_capacities    = {{uint_to_du_index(0), 1, 0, 0, {}}};
  request.beam_loads       = {{"CN-BEAM-LOW", 8, 8, make_priority(15, 90)},
                              {"CN-BEAM-HIGH", 1, 1, make_priority(1, 7, true)}};

  const ntn_beam_placement_plan plan = ntn_beam_placement_planner{}.plan(request);

  EXPECT_EQ(find_assignment(plan, "CN-BEAM-HIGH").state, ntn_beam_assignment_state::active_loaded);
  EXPECT_EQ(find_assignment(plan, "CN-BEAM-LOW").state, ntn_beam_assignment_state::candidate);
}

TEST(ntn_beam_placement_planner, orders_antenna_slots_by_qos_without_changing_demand_slot_count)
{
  ntn_beam_placement_request request;
  request.beams         = {make_beam("CN-BEAM-LOW", 1), make_beam("CN-BEAM-HIGH", 2)};
  request.visible_beams = {{"CN-BEAM-LOW", 80.0}, {"CN-BEAM-HIGH", 60.0}};
  request.du_capacities = {{uint_to_du_index(0), 2, 0, 0, {}}};
  request.beam_loads    = {{"CN-BEAM-LOW", 3, 3, make_priority(15, 90)},
                           {"CN-BEAM-HIGH", 1, 1, make_priority(1, 7, true)}};

  const ntn_beam_placement_plan plan = ntn_beam_placement_planner{}.plan(request);

  const ntn_beam_du_assignment high = find_assignment(plan, "CN-BEAM-HIGH");
  const ntn_beam_du_assignment low  = find_assignment(plan, "CN-BEAM-LOW");

  EXPECT_EQ(high.state, ntn_beam_assignment_state::active_loaded);
  EXPECT_EQ(low.state, ntn_beam_assignment_state::active_loaded);
  EXPECT_EQ(high.antenna_slot_index, 0U);
  EXPECT_EQ(high.nof_antenna_slots, 1U);
  EXPECT_EQ(low.antenna_slot_index, 1U);
  EXPECT_EQ(low.nof_antenna_slots, 3U);
  EXPECT_EQ(high.antenna_slot_period, 4U);
  EXPECT_EQ(low.antenna_slot_period, 4U);
}

TEST(ntn_beam_placement_planner, weighted_resource_intent_gives_qos_critical_beam_extra_slots)
{
  ntn_beam_placement_request request;
  request.beams         = {make_beam("CN-BEAM-LOW", 1), make_beam("CN-BEAM-HIGH", 2)};
  request.visible_beams = {{"CN-BEAM-LOW", 80.0}, {"CN-BEAM-HIGH", 60.0}};
  request.du_capacities = {{uint_to_du_index(0), 2, 0, 0, {}}};
  request.demand_aware_resource_weighting_enabled = true;
  request.beam_loads = {{"CN-BEAM-LOW", 2, 2, make_priority(15, 90)},
                        {"CN-BEAM-HIGH", 2, 2, make_delay_critical_gbr_priority()}};

  const ntn_beam_placement_plan plan = ntn_beam_placement_planner{}.plan(request);

  const ntn_beam_du_assignment high = find_assignment(plan, "CN-BEAM-HIGH");
  const ntn_beam_du_assignment low  = find_assignment(plan, "CN-BEAM-LOW");

  EXPECT_EQ(high.state, ntn_beam_assignment_state::active_loaded);
  EXPECT_EQ(low.state, ntn_beam_assignment_state::active_loaded);
  EXPECT_GT(high.resource_weight, low.resource_weight);
  EXPECT_GT(high.resource_share, low.resource_share);
  EXPECT_GT(high.nof_antenna_slots, low.nof_antenna_slots);
  EXPECT_EQ(high.nof_antenna_slots + low.nof_antenna_slots, high.antenna_slot_period);
  EXPECT_EQ(high.antenna_slot_period, low.antenna_slot_period);
  EXPECT_EQ(high.resource_weight_reason, "qos_weighted");
}

TEST(ntn_beam_placement_planner, weighted_resource_intent_keeps_min_slot_for_low_priority_beam)
{
  ntn_beam_placement_request request;
  request.beams         = {make_beam("CN-BEAM-LOW", 1), make_beam("CN-BEAM-HIGH", 2)};
  request.visible_beams = {{"CN-BEAM-LOW", 80.0}, {"CN-BEAM-HIGH", 60.0}};
  request.du_capacities = {{uint_to_du_index(0), 2, 0, 0, {}}};
  request.demand_aware_resource_weighting_enabled = true;
  request.beam_loads = {{"CN-BEAM-LOW", 1, 1, make_priority(15, 90)},
                        {"CN-BEAM-HIGH", 8, 8, make_delay_critical_gbr_priority()}};

  const ntn_beam_placement_plan plan = ntn_beam_placement_planner{}.plan(request);

  const ntn_beam_du_assignment high = find_assignment(plan, "CN-BEAM-HIGH");
  const ntn_beam_du_assignment low  = find_assignment(plan, "CN-BEAM-LOW");

  EXPECT_EQ(low.nof_antenna_slots, 1U);
  EXPECT_GT(high.nof_antenna_slots, low.nof_antenna_slots);
  EXPECT_EQ(low.resource_weight_reason, "fairness_floor");
}

TEST(ntn_beam_placement_planner, weighted_resource_intent_handles_drb_only_service_beam)
{
  ntn_beam_placement_request request;
  request.beams         = {make_beam("CN-BEAM-0001", 1)};
  request.visible_beams = {{"CN-BEAM-0001", 72.0}};
  request.du_capacities = {{uint_to_du_index(0), 1, 0, 0, {}}};
  request.demand_aware_resource_weighting_enabled = true;
  request.beam_loads = {{"CN-BEAM-0001", 0, 3}};

  const ntn_beam_placement_plan plan = ntn_beam_placement_planner{}.plan(request);

  const ntn_beam_du_assignment beam = find_assignment(plan, "CN-BEAM-0001");
  EXPECT_EQ(beam.state, ntn_beam_assignment_state::active_loaded);
  EXPECT_EQ(beam.nof_antenna_slots, 1U);
  EXPECT_EQ(beam.resource_weight, 1U);
  EXPECT_EQ(beam.resource_share, 1.0);
  EXPECT_EQ(beam.resource_weight_reason, "drb_only_min");
}

TEST(ntn_beam_placement_planner, weighted_resource_intent_disabled_keeps_legacy_slot_counts)
{
  ntn_beam_placement_request request;
  request.beams         = {make_beam("CN-BEAM-LOW", 1), make_beam("CN-BEAM-HIGH", 2)};
  request.visible_beams = {{"CN-BEAM-LOW", 80.0}, {"CN-BEAM-HIGH", 60.0}};
  request.du_capacities = {{uint_to_du_index(0), 2, 0, 0, {}}};
  request.beam_loads = {{"CN-BEAM-LOW", 3, 3, make_priority(15, 90)},
                        {"CN-BEAM-HIGH", 1, 1, make_delay_critical_gbr_priority()}};

  const ntn_beam_placement_plan plan = ntn_beam_placement_planner{}.plan(request);

  const ntn_beam_du_assignment high = find_assignment(plan, "CN-BEAM-HIGH");
  const ntn_beam_du_assignment low  = find_assignment(plan, "CN-BEAM-LOW");

  EXPECT_EQ(high.nof_antenna_slots, 1U);
  EXPECT_EQ(low.nof_antenna_slots, 3U);
  EXPECT_EQ(high.resource_weight_reason, "legacy");
  EXPECT_EQ(low.resource_weight_reason, "legacy");
}

TEST(ntn_beam_placement_planner, analog_loaded_child_cap_keeps_extra_digital_child_candidate)
{
  ntn_beam_placement_request request;
  request.beams = {make_analog_child_beam("CN-BEAM-0001", 1, "ANALOG-0001"),
                   make_analog_child_beam("CN-BEAM-0002", 2, "ANALOG-0001")};
  request.analog_beams     = {make_analog_beam("ANALOG-0001", {"CN-BEAM-0001", "CN-BEAM-0002"})};
  request.visible_beams    = {{"CN-BEAM-0001", 80.0}, {"CN-BEAM-0002", 79.0}};
  request.du_capacities    = {{uint_to_du_index(0), 2, 0, 0, {}}};
  request.beam_loads       = {{"CN-BEAM-0001", 1, 1}, {"CN-BEAM-0002", 1, 1}};
  request.resource_policy.analog.max_loaded_digital_children = 1;

  const ntn_beam_placement_plan plan = ntn_beam_placement_planner{}.plan(request);

  const ntn_beam_du_assignment beam1 = find_assignment(plan, "CN-BEAM-0001");
  const ntn_beam_du_assignment beam2 = find_assignment(plan, "CN-BEAM-0002");
  EXPECT_EQ(beam1.state, ntn_beam_assignment_state::active_loaded);
  EXPECT_TRUE(beam1.resource_domain_eligible);
  EXPECT_EQ(beam1.resource_domain_reason, "eligible");
  EXPECT_EQ(beam2.state, ntn_beam_assignment_state::candidate);
  EXPECT_FALSE(beam2.resource_domain_eligible);
  EXPECT_EQ(beam2.resource_domain_reason, "analog_loaded_child_cap_exhausted");
}

TEST(ntn_beam_placement_planner, digital_ue_cap_blocks_loaded_service_demand_without_capping_candidate_inventory)
{
  ntn_beam_placement_request request;
  request.beams         = {make_beam("CN-BEAM-0001", 1)};
  request.visible_beams = {{"CN-BEAM-0001", 80.0}};
  request.du_capacities = {{uint_to_du_index(0), 1, 0, 0, {}}};
  request.beam_loads    = {{"CN-BEAM-0001", 2, 1}};
  request.resource_policy.digital.max_ues = 1;

  const ntn_beam_placement_plan plan = ntn_beam_placement_planner{}.plan(request);

  const ntn_beam_du_assignment beam1 = find_assignment(plan, "CN-BEAM-0001");
  EXPECT_EQ(beam1.state, ntn_beam_assignment_state::candidate);
  EXPECT_TRUE(beam1.in_hopping_window);
  EXPECT_FALSE(beam1.resource_domain_eligible);
  EXPECT_EQ(beam1.resource_domain_reason, "digital_ue_cap_exhausted");
  EXPECT_TRUE(get_active_loaded_ntn_beam_ids(plan, request.visible_beams).empty());
  EXPECT_TRUE(get_mobility_eligible_ntn_beam_ids(plan, request.visible_beams).empty());
}

TEST(ntn_beam_placement_planner, explicit_conflict_group_blocks_second_loaded_digital_service_beam)
{
  ntn_beam_placement_request request;
  request.beams = {make_beam("CN-BEAM-0001", 1), make_beam("CN-BEAM-0002", 2)};
  request.beams[0].resource_policy.emplace();
  request.beams[0].resource_policy->reuse_group_id     = "reuse-a";
  request.beams[0].resource_policy->conflict_group_ids = {"conflict-a"};
  request.beams[1].resource_policy.emplace();
  request.beams[1].resource_policy->reuse_group_id     = "reuse-a";
  request.beams[1].resource_policy->conflict_group_ids = {"conflict-a"};
  request.visible_beams = {{"CN-BEAM-0001", 80.0}, {"CN-BEAM-0002", 79.0}};
  request.du_capacities = {{uint_to_du_index(0), 2, 0, 0, {}}};
  request.beam_loads    = {{"CN-BEAM-0001", 1, 1}, {"CN-BEAM-0002", 1, 1}};

  const ntn_beam_placement_plan plan = ntn_beam_placement_planner{}.plan(request);

  const ntn_beam_du_assignment beam1 = find_assignment(plan, "CN-BEAM-0001");
  const ntn_beam_du_assignment beam2 = find_assignment(plan, "CN-BEAM-0002");
  EXPECT_EQ(beam1.state, ntn_beam_assignment_state::active_loaded);
  EXPECT_EQ(beam1.reuse_group_id, "reuse-a");
  EXPECT_EQ(beam1.conflict_group_ids, std::vector<std::string>({"conflict-a"}));
  EXPECT_EQ(beam2.state, ntn_beam_assignment_state::candidate);
  EXPECT_FALSE(beam2.resource_domain_eligible);
  EXPECT_EQ(beam2.resource_domain_reason, "conflict_group_blocked");
}

TEST(ntn_beam_placement_planner, sibling_digital_beams_without_explicit_conflict_can_both_be_loaded)
{
  ntn_beam_placement_request request;
  request.beams = {make_analog_child_beam("CN-BEAM-0001", 1, "ANALOG-0001"),
                   make_analog_child_beam("CN-BEAM-0002", 2, "ANALOG-0001")};
  request.analog_beams  = {make_analog_beam("ANALOG-0001", {"CN-BEAM-0001", "CN-BEAM-0002"})};
  request.visible_beams = {{"CN-BEAM-0001", 80.0}, {"CN-BEAM-0002", 79.0}};
  request.du_capacities = {{uint_to_du_index(0), 2, 0, 0, {}}};
  request.beam_loads    = {{"CN-BEAM-0001", 1, 1}, {"CN-BEAM-0002", 1, 1}};

  const ntn_beam_placement_plan plan = ntn_beam_placement_planner{}.plan(request);

  EXPECT_EQ(find_assignment(plan, "CN-BEAM-0001").state, ntn_beam_assignment_state::active_loaded);
  EXPECT_EQ(find_assignment(plan, "CN-BEAM-0002").state, ntn_beam_assignment_state::active_loaded);
  EXPECT_EQ(get_active_loaded_ntn_beam_ids(plan, request.visible_beams),
            std::vector<std::string>({"CN-BEAM-0001", "CN-BEAM-0002"}));
}

TEST(ntn_beam_placement_planner, enforces_global_multi_beam_hopping_window_across_dus)
{
  ntn_beam_placement_request request;
  request.beams         = {make_beam("CN-BEAM-0001", 1),
                           make_beam("CN-BEAM-0002", 2),
                           make_beam("CN-BEAM-0003", 3)};
  request.visible_beams    = {{"CN-BEAM-0001", 70.0}, {"CN-BEAM-0002", 69.0}, {"CN-BEAM-0003", 68.0}};
  request.max_active_beams = 2;
  request.du_capacities    = {{uint_to_du_index(0), 2, 0, 0, {"CN-BEAM-0001", "CN-BEAM-0003"}},
                              {uint_to_du_index(1), 2, 0, 0, {"CN-BEAM-0002", "CN-BEAM-0003"}}};
  request.beam_loads       = {{"CN-BEAM-0001", 4, 4}, {"CN-BEAM-0002", 3, 3}, {"CN-BEAM-0003", 2, 2}};

  const ntn_beam_placement_plan plan = ntn_beam_placement_planner{}.plan(request);

  EXPECT_EQ(find_assignment(plan, "CN-BEAM-0001").state, ntn_beam_assignment_state::active_loaded);
  EXPECT_EQ(find_assignment(plan, "CN-BEAM-0002").state, ntn_beam_assignment_state::active_loaded);
  EXPECT_EQ(find_assignment(plan, "CN-BEAM-0003").state, ntn_beam_assignment_state::candidate);
  EXPECT_EQ(get_active_loaded_ntn_beam_ids(plan, request.visible_beams),
            std::vector<std::string>({"CN-BEAM-0001", "CN-BEAM-0002"}));
}

TEST(ntn_beam_placement_planner, keeps_visible_beams_outside_hopping_window_as_candidates)
{
  ntn_beam_placement_request request;
  request.beams         = {make_beam("CN-BEAM-0001", 1),
                           make_beam("CN-BEAM-0002", 2),
                           make_beam("CN-BEAM-0003", 3)};
  request.visible_beams = {{"CN-BEAM-0001", 70.0, true},
                           {"CN-BEAM-0002", 69.0, false},
                           {"CN-BEAM-0003", 68.0, true}};
  request.max_active_beams = 2;
  request.du_capacities    = {{uint_to_du_index(0), 3, 0, 0, {}}};
  request.beam_loads       = {{"CN-BEAM-0001", 2, 2}, {"CN-BEAM-0003", 1, 1}};

  const ntn_beam_placement_plan plan = ntn_beam_placement_planner{}.plan(request);

  const ntn_beam_du_assignment beam1 = find_assignment(plan, "CN-BEAM-0001");
  const ntn_beam_du_assignment beam2 = find_assignment(plan, "CN-BEAM-0002");
  const ntn_beam_du_assignment beam3 = find_assignment(plan, "CN-BEAM-0003");
  EXPECT_EQ(beam1.state, ntn_beam_assignment_state::active_loaded);
  EXPECT_TRUE(beam1.in_hopping_window);
  EXPECT_EQ(beam2.state, ntn_beam_assignment_state::candidate);
  EXPECT_FALSE(beam2.in_hopping_window);
  EXPECT_EQ(beam3.state, ntn_beam_assignment_state::active_loaded);
  EXPECT_TRUE(beam3.in_hopping_window);
  EXPECT_EQ(get_active_loaded_ntn_beam_ids(plan, request.visible_beams),
            std::vector<std::string>({"CN-BEAM-0001", "CN-BEAM-0003"}));
}

TEST(ntn_beam_placement_planner, keeps_loaded_visible_beam_outside_hopping_window_as_candidate_without_previous_service)
{
  ntn_beam_placement_request request;
  request.beams         = {make_beam("CN-BEAM-0001", 1),
                           make_beam("CN-BEAM-0002", 2),
                           make_beam("CN-BEAM-0003", 3)};
  request.visible_beams = {{"CN-BEAM-0001", 70.0, true},
                           {"CN-BEAM-0002", 69.0, true},
                           {"CN-BEAM-0003", 68.0, false}};
  request.max_active_beams = 2;
  request.du_capacities    = {{uint_to_du_index(0), 2, 0, 0, {}}};
  request.beam_loads       = {{"CN-BEAM-0001", 2, 2}, {"CN-BEAM-0002", 1, 1}, {"CN-BEAM-0003", 5, 10}};

  const ntn_beam_placement_plan plan = ntn_beam_placement_planner{}.plan(request);

  const ntn_beam_du_assignment beam1 = find_assignment(plan, "CN-BEAM-0001");
  const ntn_beam_du_assignment beam2 = find_assignment(plan, "CN-BEAM-0002");
  const ntn_beam_du_assignment beam3 = find_assignment(plan, "CN-BEAM-0003");
  EXPECT_EQ(beam1.state, ntn_beam_assignment_state::active_loaded);
  EXPECT_TRUE(beam1.in_hopping_window);
  EXPECT_EQ(beam2.state, ntn_beam_assignment_state::active_loaded);
  EXPECT_TRUE(beam2.in_hopping_window);
  EXPECT_EQ(beam3.state, ntn_beam_assignment_state::candidate);
  EXPECT_FALSE(beam3.in_hopping_window);
  EXPECT_EQ(get_active_loaded_ntn_beam_ids(plan, request.visible_beams),
            std::vector<std::string>({"CN-BEAM-0001", "CN-BEAM-0002"}));
}

TEST(ntn_beam_placement_planner, loaded_visible_beam_outside_hopping_window_does_not_preempt_loaded_window_beam)
{
  ntn_beam_placement_request request;
  request.beams         = {make_beam("CN-BEAM-0001", 1),
                           make_beam("CN-BEAM-0002", 2),
                           make_beam("CN-BEAM-0003", 3)};
  request.visible_beams = {{"CN-BEAM-0001", 70.0, true},
                           {"CN-BEAM-0002", 69.0, true},
                           {"CN-BEAM-0003", 68.0, false}};
  request.max_active_beams = 2;
  request.du_capacities    = {{uint_to_du_index(0), 2, 0, 0, {}}};
  request.beam_loads       = {{"CN-BEAM-0001", 2, 2}, {"CN-BEAM-0002", 1, 1}, {"CN-BEAM-0003", 5, 10}};
  request.previous_assignments = {{"CN-BEAM-0001",
                                   make_nci(1),
                                   uint_to_du_index(0),
                                   ntn_beam_assignment_state::active_loaded,
                                   70.0,
                                   2,
                                   2},
                                  {"CN-BEAM-0002",
                                   make_nci(2),
                                   uint_to_du_index(0),
                                   ntn_beam_assignment_state::active_loaded,
                                   69.0,
                                   1,
                                   1}};

  const ntn_beam_placement_plan plan = ntn_beam_placement_planner{}.plan(request);

  EXPECT_EQ(find_assignment(plan, "CN-BEAM-0001").state, ntn_beam_assignment_state::active_loaded);
  EXPECT_EQ(find_assignment(plan, "CN-BEAM-0002").state, ntn_beam_assignment_state::active_loaded);
  EXPECT_EQ(find_assignment(plan, "CN-BEAM-0003").state, ntn_beam_assignment_state::candidate);
}

TEST(ntn_beam_placement_planner, marks_previous_active_beam_as_draining_when_it_leaves_hopping_window)
{
  ntn_beam_placement_request request;
  request.beams         = {make_beam("CN-BEAM-0001", 1), make_beam("CN-BEAM-0002", 2)};
  request.visible_beams = {{"CN-BEAM-0001", 70.0, false}, {"CN-BEAM-0002", 69.0, true}};
  request.du_capacities = {{uint_to_du_index(0), 2, 0, 0, {}}};
  request.beam_loads    = {{"CN-BEAM-0002", 1, 2}};
  request.previous_assignments = {{"CN-BEAM-0001",
                                   make_nci(1),
                                   uint_to_du_index(0),
                                   ntn_beam_assignment_state::active_loaded,
                                   70.0,
                                   4,
                                   8}};

  const ntn_beam_placement_plan plan = ntn_beam_placement_planner{}.plan(request);

  const ntn_beam_du_assignment beam1 = find_assignment(plan, "CN-BEAM-0001");
  EXPECT_EQ(beam1.state, ntn_beam_assignment_state::draining);
  EXPECT_EQ(beam1.du_index, uint_to_du_index(0));
  EXPECT_EQ(beam1.nof_ues, 4U);
  EXPECT_FALSE(beam1.in_hopping_window);

  const ntn_beam_du_assignment beam2 = find_assignment(plan, "CN-BEAM-0002");
  EXPECT_EQ(beam2.state, ntn_beam_assignment_state::active_loaded);
  EXPECT_TRUE(beam2.in_hopping_window);
  EXPECT_EQ(get_active_loaded_ntn_beam_ids(plan, request.visible_beams), std::vector<std::string>({"CN-BEAM-0002"}));
}

TEST(ntn_beam_placement_planner, keeps_previous_draining_beam_while_load_remains)
{
  ntn_beam_placement_request request;
  request.beams         = {make_beam("CN-BEAM-0001", 1), make_beam("CN-BEAM-0002", 2)};
  request.visible_beams = {{"CN-BEAM-0001", 70.0, false}, {"CN-BEAM-0002", 69.0, true}};
  request.du_capacities = {{uint_to_du_index(0), 2, 0, 0, {}}};
  request.beam_loads    = {{"CN-BEAM-0001", 3, 6}, {"CN-BEAM-0002", 1, 2}};
  request.previous_assignments = {{"CN-BEAM-0001",
                                   make_nci(1),
                                   uint_to_du_index(0),
                                   ntn_beam_assignment_state::draining,
                                   70.0,
                                   4,
                                   8}};

  const ntn_beam_placement_plan plan = ntn_beam_placement_planner{}.plan(request);

  const ntn_beam_du_assignment beam1 = find_assignment(plan, "CN-BEAM-0001");
  EXPECT_EQ(beam1.state, ntn_beam_assignment_state::draining);
  EXPECT_EQ(beam1.du_index, uint_to_du_index(0));
  EXPECT_EQ(beam1.nof_ues, 3U);
  EXPECT_EQ(beam1.nof_drbs, 6U);
  EXPECT_FALSE(beam1.in_hopping_window);

  const ntn_beam_du_assignment beam2 = find_assignment(plan, "CN-BEAM-0002");
  EXPECT_EQ(beam2.state, ntn_beam_assignment_state::active_loaded);
  EXPECT_TRUE(beam2.in_hopping_window);
}

TEST(ntn_beam_placement_planner, retained_draining_beam_occupies_previous_du_capacity)
{
  ntn_beam_placement_request request;
  request.beams         = {make_beam("CN-BEAM-0001", 1), make_beam("CN-BEAM-0002", 2)};
  request.visible_beams = {{"CN-BEAM-0001", 70.0, false}, {"CN-BEAM-0002", 69.0, true}};
  request.du_capacities = {{uint_to_du_index(0), 1, 0, 0, {}}};
  request.beam_loads    = {{"CN-BEAM-0001", 3, 6}, {"CN-BEAM-0002", 1, 2}};
  request.previous_assignments = {{"CN-BEAM-0001",
                                   make_nci(1),
                                   uint_to_du_index(0),
                                   ntn_beam_assignment_state::active_loaded,
                                   70.0,
                                   3,
                                   6}};

  const ntn_beam_placement_plan plan = ntn_beam_placement_planner{}.plan(request);

  EXPECT_EQ(find_assignment(plan, "CN-BEAM-0001").state, ntn_beam_assignment_state::draining);
  EXPECT_EQ(find_assignment(plan, "CN-BEAM-0002").state, ntn_beam_assignment_state::candidate);
  EXPECT_TRUE(get_active_loaded_ntn_beam_ids(plan, request.visible_beams).empty());
}

TEST(ntn_beam_placement_planner, does_not_retain_draining_beam_when_previous_du_is_unavailable)
{
  ntn_beam_placement_request request;
  request.beams = {make_beam("CN-BEAM-0001", 1)};
  request.beam_loads = {{"CN-BEAM-0001", 3, 6}};
  request.previous_assignments = {{"CN-BEAM-0001",
                                   make_nci(1),
                                   uint_to_du_index(0),
                                   ntn_beam_assignment_state::active_loaded,
                                   70.0,
                                   3,
                                   6}};

  const ntn_beam_placement_plan plan = ntn_beam_placement_planner{}.plan(request);

  EXPECT_EQ(find_assignment(plan, "CN-BEAM-0001").state, ntn_beam_assignment_state::inactive);
  EXPECT_EQ(find_assignment(plan, "CN-BEAM-0001").du_index, du_index_t::invalid);
}

TEST(ntn_beam_placement_planner, retires_previous_draining_beam_when_load_is_gone)
{
  ntn_beam_placement_request request;
  request.beams         = {make_beam("CN-BEAM-0001", 1), make_beam("CN-BEAM-0002", 2)};
  request.visible_beams = {{"CN-BEAM-0002", 69.0, true}};
  request.du_capacities = {{uint_to_du_index(0), 2, 0, 0, {}}};
  request.beam_loads    = {{"CN-BEAM-0002", 1, 2}};
  request.previous_assignments = {{"CN-BEAM-0001",
                                   make_nci(1),
                                   uint_to_du_index(0),
                                   ntn_beam_assignment_state::draining,
                                   70.0,
                                   4,
                                   8}};

  const ntn_beam_placement_plan plan = ntn_beam_placement_planner{}.plan(request);

  EXPECT_EQ(find_assignment(plan, "CN-BEAM-0001").state, ntn_beam_assignment_state::inactive);
  EXPECT_EQ(find_assignment(plan, "CN-BEAM-0001").du_index, du_index_t::invalid);
  EXPECT_EQ(find_assignment(plan, "CN-BEAM-0002").state, ntn_beam_assignment_state::active_loaded);
}

TEST(ntn_beam_placement_planner, assigns_analog_access_du_by_supported_child_count)
{
  ntn_beam_placement_request request;
  request.beams = {make_analog_child_beam("CN-BEAM-0001", 1, "ANALOG-001"),
                   make_analog_child_beam("CN-BEAM-0002", 2, "ANALOG-001"),
                   make_analog_child_beam("CN-BEAM-0003", 3, "ANALOG-001")};
  request.analog_beams  = {make_analog_beam("ANALOG-001", {"CN-BEAM-0001", "CN-BEAM-0002", "CN-BEAM-0003"})};
  request.visible_beams = {{"CN-BEAM-0001", 72.0}, {"CN-BEAM-0002", 71.0}, {"CN-BEAM-0003", 70.0}};
  request.du_capacities = {{uint_to_du_index(0), 3, 0, 0, {"CN-BEAM-0001"}},
                           {uint_to_du_index(1), 3, 0, 0, {"CN-BEAM-0001", "CN-BEAM-0002"}}};

  const ntn_beam_placement_plan plan = ntn_beam_placement_planner{}.plan(request);

  ASSERT_EQ(plan.analog_assignments.size(), 1U);
  EXPECT_EQ(plan.analog_assignments.front().analog_beam_id, "ANALOG-001");
  EXPECT_EQ(plan.analog_assignments.front().selected_access_du_index, uint_to_du_index(1));
  EXPECT_EQ(plan.analog_assignments.front().supported_child_count, 2U);
  EXPECT_EQ(plan.analog_assignments.front().assigned_child_count, 2U);
  EXPECT_EQ(plan.analog_assignments.front().reason, "eligible");
  EXPECT_EQ(find_assignment(plan, "CN-BEAM-0001").access_du_index, uint_to_du_index(1));
  EXPECT_EQ(find_assignment(plan, "CN-BEAM-0001").access_du_reason, "eligible");
}

TEST(ntn_beam_placement_planner, breaks_analog_access_du_ties_by_assigned_analog_load_then_du_index)
{
  ntn_beam_placement_request request;
  request.beams = {make_analog_child_beam("CN-BEAM-0001", 1, "ANALOG-001"),
                   make_analog_child_beam("CN-BEAM-0002", 2, "ANALOG-002")};
  request.analog_beams = {make_analog_beam("ANALOG-001", {"CN-BEAM-0001"}),
                          make_analog_beam("ANALOG-002", {"CN-BEAM-0002"})};
  request.visible_beams = {{"CN-BEAM-0001", 72.0}, {"CN-BEAM-0002", 71.0}};
  request.du_capacities = {{uint_to_du_index(0), 2, 0, 0, {"CN-BEAM-0001", "CN-BEAM-0002"}},
                           {uint_to_du_index(1), 2, 0, 0, {"CN-BEAM-0001", "CN-BEAM-0002"}}};

  const ntn_beam_placement_plan plan = ntn_beam_placement_planner{}.plan(request);

  ASSERT_EQ(plan.analog_assignments.size(), 2U);
  EXPECT_EQ(plan.analog_assignments[0].analog_beam_id, "ANALOG-001");
  EXPECT_EQ(plan.analog_assignments[0].selected_access_du_index, uint_to_du_index(0));
  EXPECT_EQ(plan.analog_assignments[1].analog_beam_id, "ANALOG-002");
  EXPECT_EQ(plan.analog_assignments[1].selected_access_du_index, uint_to_du_index(1));
}

TEST(ntn_beam_placement_planner, prefers_parent_analog_access_du_for_loaded_digital_service)
{
  ntn_beam_placement_request request;
  request.beams = {make_analog_child_beam("CN-BEAM-0001", 1, "ANALOG-001"),
                   make_analog_child_beam("CN-BEAM-0002", 2, "ANALOG-001")};
  request.analog_beams  = {make_analog_beam("ANALOG-001", {"CN-BEAM-0001", "CN-BEAM-0002"})};
  request.visible_beams = {{"CN-BEAM-0001", 72.0}, {"CN-BEAM-0002", 71.0}};
  request.du_capacities = {{uint_to_du_index(0), 2, 0, 0, {"CN-BEAM-0001"}},
                           {uint_to_du_index(1), 2, 0, 0, {"CN-BEAM-0001", "CN-BEAM-0002"}}};
  request.beam_loads    = {{"CN-BEAM-0001", 1, 1}};

  const ntn_beam_placement_plan plan = ntn_beam_placement_planner{}.plan(request);

  const ntn_beam_du_assignment beam1 = find_assignment(plan, "CN-BEAM-0001");
  EXPECT_EQ(beam1.state, ntn_beam_assignment_state::active_loaded);
  EXPECT_EQ(beam1.access_du_index, uint_to_du_index(1));
  EXPECT_EQ(beam1.du_index, uint_to_du_index(1));
  EXPECT_EQ(beam1.du_assignment_reason, "eligible");
}

TEST(ntn_beam_placement_planner, keeps_loaded_digital_candidate_when_parent_access_du_does_not_support_it)
{
  ntn_beam_placement_request request;
  request.beams = {make_analog_child_beam("CN-BEAM-0001", 1, "ANALOG-001"),
                   make_analog_child_beam("CN-BEAM-0002", 2, "ANALOG-001"),
                   make_analog_child_beam("CN-BEAM-0003", 3, "ANALOG-001")};
  request.analog_beams  = {make_analog_beam("ANALOG-001", {"CN-BEAM-0001", "CN-BEAM-0002", "CN-BEAM-0003"})};
  request.visible_beams = {{"CN-BEAM-0001", 72.0}, {"CN-BEAM-0002", 71.0}, {"CN-BEAM-0003", 70.0}};
  request.du_capacities = {{uint_to_du_index(0), 3, 0, 0, {"CN-BEAM-0001", "CN-BEAM-0002"}},
                           {uint_to_du_index(1), 3, 0, 0, {"CN-BEAM-0003"}}};
  request.beam_loads    = {{"CN-BEAM-0003", 1, 1}};

  const ntn_beam_placement_plan plan = ntn_beam_placement_planner{}.plan(request);

  ASSERT_EQ(plan.analog_assignments.size(), 1U);
  EXPECT_EQ(plan.analog_assignments.front().selected_access_du_index, uint_to_du_index(0));
  const ntn_beam_du_assignment beam3 = find_assignment(plan, "CN-BEAM-0003");
  EXPECT_EQ(beam3.state, ntn_beam_assignment_state::candidate);
  EXPECT_EQ(beam3.access_du_index, uint_to_du_index(0));
  EXPECT_EQ(beam3.du_index, du_index_t::invalid);
  EXPECT_EQ(beam3.du_assignment_reason, "service_du_differs_from_access_du");
}

TEST(ntn_beam_placement_planner, leaves_visible_beam_as_candidate_when_no_du_can_accept_it)
{
  ntn_beam_placement_request request;
  request.beams         = {make_beam("CN-BEAM-0001", 1)};
  request.visible_beams = {{"CN-BEAM-0001", 66.0}};
  request.du_capacities = {{uint_to_du_index(0), 0, 0, 0, {}}};
  request.beam_loads    = {{"CN-BEAM-0001", 2, 4}};

  const ntn_beam_placement_plan plan = ntn_beam_placement_planner{}.plan(request);

  const ntn_beam_du_assignment beam1 = find_assignment(plan, "CN-BEAM-0001");
  EXPECT_EQ(beam1.state, ntn_beam_assignment_state::candidate);
  EXPECT_EQ(beam1.du_index, du_index_t::invalid);
  EXPECT_EQ(beam1.nof_ues, 2U);
  EXPECT_EQ(beam1.nof_drbs, 4U);
}

TEST(ntn_beam_placement_planner, leaves_visible_beam_as_candidate_when_no_du_is_connected)
{
  ntn_beam_placement_request request;
  request.beams         = {make_beam("CN-BEAM-0001", 1)};
  request.visible_beams = {{"CN-BEAM-0001", 66.0}};
  request.beam_loads    = {{"CN-BEAM-0001", 2, 4}};

  const ntn_beam_placement_plan plan = ntn_beam_placement_planner{}.plan(request);

  const ntn_beam_du_assignment beam1 = find_assignment(plan, "CN-BEAM-0001");
  EXPECT_EQ(beam1.state, ntn_beam_assignment_state::candidate);
  EXPECT_EQ(beam1.du_index, du_index_t::invalid);
  EXPECT_TRUE(get_active_loaded_ntn_beam_ids(plan, request.visible_beams).empty());
}

TEST(ntn_beam_placement_planner, leaves_visible_beam_as_candidate_when_du_load_watermark_is_exceeded)
{
  ntn_beam_placement_request request;
  request.beams         = {make_beam("CN-BEAM-0001", 1)};
  request.visible_beams = {{"CN-BEAM-0001", 66.0}};
  request.du_capacities = {{uint_to_du_index(0), 1, 4, 0, {}}};
  request.beam_loads    = {{"CN-BEAM-0001", 5, 1}};

  const ntn_beam_placement_plan plan = ntn_beam_placement_planner{}.plan(request);

  const ntn_beam_du_assignment beam1 = find_assignment(plan, "CN-BEAM-0001");
  EXPECT_EQ(beam1.state, ntn_beam_assignment_state::candidate);
  EXPECT_EQ(beam1.du_index, du_index_t::invalid);
  EXPECT_TRUE(get_active_loaded_ntn_beam_ids(plan, request.visible_beams).empty());
}

TEST(ntn_beam_placement_planner, leaves_visible_beam_as_candidate_when_du_current_load_watermark_is_exceeded)
{
  ntn_beam_placement_request request;
  request.beams         = {make_beam("CN-BEAM-0001", 1)};
  request.visible_beams = {{"CN-BEAM-0001", 66.0}};
  request.du_capacities = {{uint_to_du_index(0), 1, 4, 0, {}, 3, 0}};
  request.beam_loads    = {{"CN-BEAM-0001", 2, 1}};

  const ntn_beam_placement_plan plan = ntn_beam_placement_planner{}.plan(request);

  const ntn_beam_du_assignment beam1 = find_assignment(plan, "CN-BEAM-0001");
  EXPECT_EQ(beam1.state, ntn_beam_assignment_state::candidate);
  EXPECT_EQ(beam1.du_index, du_index_t::invalid);
  EXPECT_TRUE(get_active_loaded_ntn_beam_ids(plan, request.visible_beams).empty());
}

TEST(ntn_beam_placement_planner, leaves_visible_beam_as_candidate_when_du_does_not_support_beam)
{
  ntn_beam_placement_request request;
  request.beams         = {make_beam("CN-BEAM-0001", 1), make_beam("CN-BEAM-0002", 2)};
  request.visible_beams = {{"CN-BEAM-0002", 66.0}};
  request.du_capacities = {{uint_to_du_index(0), 1, 0, 0, {"CN-BEAM-0001"}}};
  request.beam_loads    = {{"CN-BEAM-0002", 2, 4}};

  const ntn_beam_placement_plan plan = ntn_beam_placement_planner{}.plan(request);

  const ntn_beam_du_assignment beam2 = find_assignment(plan, "CN-BEAM-0002");
  EXPECT_EQ(beam2.state, ntn_beam_assignment_state::candidate);
  EXPECT_EQ(beam2.du_index, du_index_t::invalid);
  EXPECT_TRUE(get_active_loaded_ntn_beam_ids(plan, request.visible_beams).empty());
}

TEST(ntn_beam_placement_planner, marks_previous_active_assignment_as_draining_when_not_visible)
{
  ntn_beam_placement_request request;
  request.beams         = {make_beam("CN-BEAM-0001", 1), make_beam("CN-BEAM-0002", 2)};
  request.visible_beams = {{"CN-BEAM-0002", 64.0}};
  request.du_capacities = {{uint_to_du_index(0), 2, 0, 0, {}}};
  request.beam_loads    = {{"CN-BEAM-0001", 3, 6}, {"CN-BEAM-0002", 1, 2}};
  request.previous_assignments = {{"CN-BEAM-0001",
                                   make_nci(1),
                                   uint_to_du_index(0),
                                   ntn_beam_assignment_state::active_loaded,
                                   55.0,
                                   3,
                                   6}};

  const ntn_beam_placement_plan plan = ntn_beam_placement_planner{}.plan(request);

  const ntn_beam_du_assignment beam1 = find_assignment(plan, "CN-BEAM-0001");
  EXPECT_EQ(beam1.state, ntn_beam_assignment_state::draining);
  EXPECT_EQ(beam1.du_index, uint_to_du_index(0));
  EXPECT_EQ(beam1.nof_ues, 3U);

  const ntn_beam_du_assignment beam2 = find_assignment(plan, "CN-BEAM-0002");
  EXPECT_EQ(beam2.state, ntn_beam_assignment_state::active_loaded);
}

TEST(ntn_beam_assignment_repository, indexes_active_assignments_by_beam_id_and_nci)
{
  const nr_cell_identity beam1_nci = make_nci(1);
  const nr_cell_identity beam2_nci = make_nci(2);

  ntn_beam_placement_plan plan;
  plan.assignments = {{"CN-BEAM-0001",
                       beam1_nci,
                       uint_to_du_index(0),
                       ntn_beam_assignment_state::active_loaded,
                       70.0,
                       4,
                       8},
                      {"CN-BEAM-0002",
                       beam2_nci,
                       du_index_t::invalid,
                       ntn_beam_assignment_state::candidate,
                       65.0,
                       1,
                       2}};

  ntn_beam_assignment_repository repository;
  repository.update(plan);

  ASSERT_TRUE(repository.find_by_beam_id("CN-BEAM-0001").has_value());
  EXPECT_TRUE(repository.is_beam_active("CN-BEAM-0001"));
  EXPECT_EQ(repository.get_du_for_beam("CN-BEAM-0001"), std::optional<du_index_t>{uint_to_du_index(0)});
  EXPECT_EQ(repository.get_du_for_nci(beam1_nci), std::optional<du_index_t>{uint_to_du_index(0)});

  ASSERT_TRUE(repository.find_by_nci(beam2_nci).has_value());
  EXPECT_FALSE(repository.is_beam_active("CN-BEAM-0002"));
  EXPECT_FALSE(repository.get_du_for_beam("CN-BEAM-0002").has_value());
}

TEST(ntn_beam_placement_plan_helpers, extracts_active_loaded_beams_in_visible_candidate_order)
{
  ntn_beam_placement_plan plan;
  plan.assignments = {{"CN-BEAM-0001",
                       make_nci(1),
                       uint_to_du_index(0),
                       ntn_beam_assignment_state::active_loaded,
                       70.0,
                       1,
                       1},
                      {"CN-BEAM-0002",
                       make_nci(2),
                       du_index_t::invalid,
                       ntn_beam_assignment_state::candidate,
                       65.0,
                       1,
                       1},
                      {"CN-BEAM-0003",
                       make_nci(3),
                       uint_to_du_index(1),
                       ntn_beam_assignment_state::active_loaded,
                       60.0,
                       1,
                       1}};

  const std::vector<std::string> active_loaded_beam_ids =
      get_active_loaded_ntn_beam_ids(plan, {{"CN-BEAM-0003", 60.0}, {"CN-BEAM-0002", 65.0}, {"CN-BEAM-0001", 70.0}});

  EXPECT_EQ(active_loaded_beam_ids, std::vector<std::string>({"CN-BEAM-0003", "CN-BEAM-0001"}));
}

TEST(ntn_beam_placement_plan_helpers, extracts_loaded_service_calendar_beams_with_demand_only)
{
  ntn_beam_placement_plan plan;
  plan.assignments = {{"CN-BEAM-0001",
                       make_nci(1),
                       uint_to_du_index(0),
                       ntn_beam_assignment_state::candidate,
                       70.0,
                       0,
                       0,
                       true},
                      {"CN-BEAM-0002",
                       make_nci(2),
                       uint_to_du_index(0),
                       ntn_beam_assignment_state::active_loaded,
                       65.0,
                       1,
                       1,
                       true},
                      {"CN-BEAM-0003",
                       make_nci(3),
                       uint_to_du_index(0),
                       ntn_beam_assignment_state::draining,
                       60.0,
                       1,
                       0,
                       false},
                      {"CN-BEAM-0004",
                       make_nci(4),
                       du_index_t::invalid,
                       ntn_beam_assignment_state::inactive,
                       0.0,
                       0,
                       0,
                       false}};

  const std::vector<std::string> loaded_beam_ids = get_loaded_service_calendar_ntn_beam_ids(
      plan,
      {{"CN-BEAM-0001", 70.0, true},
       {"CN-BEAM-0002", 65.0, true},
       {"CN-BEAM-0003", 60.0, false},
       {"CN-BEAM-0004", 0.0, false}});

  EXPECT_EQ(loaded_beam_ids, std::vector<std::string>({"CN-BEAM-0002", "CN-BEAM-0003"}));
}

TEST(ntn_beam_placement_plan_helpers, extracts_mobility_eligible_beams_without_draining_or_inactive_entries)
{
  ntn_beam_placement_plan plan;
  plan.assignments = {{"CN-BEAM-0001",
                       make_nci(1),
                       uint_to_du_index(0),
                       ntn_beam_assignment_state::active_loaded,
                       70.0,
                       1,
                       1,
                       true},
                      {"CN-BEAM-0002",
                       make_nci(2),
                       uint_to_du_index(0),
                       ntn_beam_assignment_state::candidate,
                       65.0,
                       0,
                       0,
                       true},
                      {"CN-BEAM-0003",
                       make_nci(3),
                       uint_to_du_index(0),
                       ntn_beam_assignment_state::draining,
                       60.0,
                       1,
                       1,
                       false},
                      {"CN-BEAM-0004",
                       make_nci(4),
                       du_index_t::invalid,
                       ntn_beam_assignment_state::inactive,
                       0.0,
                       0,
                       0,
                       false}};

  const std::vector<std::string> mobility_eligible_beam_ids = get_mobility_eligible_ntn_beam_ids(
      plan,
      {{"CN-BEAM-0003", 60.0, false},
       {"CN-BEAM-0002", 65.0, true},
       {"CN-BEAM-0001", 70.0, true},
       {"CN-BEAM-0004", 55.0, true}});

  EXPECT_EQ(mobility_eligible_beam_ids, std::vector<std::string>({"CN-BEAM-0002", "CN-BEAM-0001"}));
}
