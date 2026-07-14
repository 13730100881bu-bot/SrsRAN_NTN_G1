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

#include "lib/cu_cp/ntn_mobility/ntn_beam_rebalance_policy.h"
#include "srsran/ran/gnb_id.h"
#include <gtest/gtest.h>

using namespace srsran;
using namespace srs_cu_cp;

namespace {

nr_cell_identity make_nci(unsigned sector_id)
{
  return nr_cell_identity::create(gnb_id_t{0x19b, 32}, sector_id).value();
}

ntn_beam_position make_beam(const std::string& beam_id, unsigned sector_id, const std::string& analog_beam_id)
{
  ntn_beam_position beam;
  beam.beam_id        = beam_id;
  beam.nci            = make_nci(sector_id);
  beam.center_latitude_deg  = 30.0;
  beam.center_longitude_deg = 110.0 + sector_id;
  beam.coverage_radius_m    = 45000.0;
  beam.enabled        = true;
  beam.analog_beam_id = analog_beam_id;
  return beam;
}

ntn_beam_du_assignment make_assignment(const std::string&          beam_id,
                                       unsigned                    sector_id,
                                       du_index_t                  du_index,
                                       ntn_beam_assignment_state   state,
                                       unsigned                    nof_ues,
                                       unsigned                    nof_drbs)
{
  ntn_beam_du_assignment assignment;
  assignment.beam_id              = beam_id;
  assignment.nci                  = make_nci(sector_id);
  assignment.du_index             = du_index;
  assignment.state                = state;
  assignment.elevation_deg        = 70.0 - sector_id;
  assignment.nof_ues              = nof_ues;
  assignment.nof_drbs             = nof_drbs;
  assignment.digital_ue_load      = nof_ues;
  assignment.digital_drb_load     = nof_drbs;
  assignment.digital_ue_cap       = 8;
  assignment.digital_drb_cap      = 16;
  assignment.analog_service_bound_ue_cap = 8;
  assignment.analog_drb_cap       = 16;
  assignment.in_hopping_window    = true;
  assignment.access_du_index      = du_index;
  assignment.access_du_reason     = "eligible";
  assignment.du_assignment_reason = "eligible";
  assignment.resource_domain_eligible = true;
  return assignment;
}

ntn_rebalance_policy_request make_request()
{
  ntn_rebalance_policy_request request;
  request.beams = {make_beam("beam-1", 1, "analog-a"),
                   make_beam("beam-2", 2, "analog-a"),
                   make_beam("beam-3", 3, "analog-b")};
  request.plan.assignments = {
      make_assignment("beam-1", 1, uint_to_du_index(0), ntn_beam_assignment_state::active_loaded, 4, 4),
      make_assignment("beam-2", 2, uint_to_du_index(0), ntn_beam_assignment_state::active_loaded, 1, 1),
      make_assignment("beam-3", 3, uint_to_du_index(1), ntn_beam_assignment_state::active_loaded, 0, 0)};
  request.mobility_eligible_beam_ids = {"beam-1", "beam-2", "beam-3"};
  request.du_capacities = {{uint_to_du_index(0), 8, 0, 0, {}}, {uint_to_du_index(1), 8, 0, 0, {}}};
  request.source_beam_id  = "beam-1";
  request.source_nci      = make_nci(1);
  request.moving_nof_drbs = 1;
  request.min_ue_delta    = 2;
  return request;
}

ntn_beam_du_assignment& find_assignment(ntn_rebalance_policy_request& request, const std::string& beam_id)
{
  auto it = std::find_if(request.plan.assignments.begin(),
                         request.plan.assignments.end(),
                         [&beam_id](const ntn_beam_du_assignment& assignment) { return assignment.beam_id == beam_id; });
  srsran_assert(it != request.plan.assignments.end(), "Missing test assignment");
  return *it;
}

} // namespace

TEST(ntn_beam_rebalance_policy, prefers_same_analog_sibling_when_available)
{
  ntn_rebalance_policy_request request = make_request();

  const ntn_rebalance_target_selection selection = select_ntn_rebalance_target(request);

  ASSERT_TRUE(selection.assignment.has_value());
  EXPECT_EQ(selection.assignment->beam_id, "beam-2");
  EXPECT_TRUE(selection.same_analog);
  EXPECT_TRUE(is_ntn_rebalance_source_hot(request, selection));
  EXPECT_EQ(selection.reason, "no_eligible_target");
}

TEST(ntn_beam_rebalance_policy, uses_cross_analog_when_source_analog_is_hot_and_no_sibling_capacity)
{
  ntn_rebalance_policy_request request = make_request();
  ntn_beam_du_assignment& sibling = find_assignment(request, "beam-2");
  sibling.digital_ue_cap = 1;
  sibling.digital_drb_cap = 1;
  ntn_beam_du_assignment& source = find_assignment(request, "beam-1");
  source.analog_service_bound_ue_cap = 5;

  const ntn_rebalance_target_selection selection = select_ntn_rebalance_target(request);

  ASSERT_TRUE(selection.assignment.has_value());
  EXPECT_EQ(selection.assignment->beam_id, "beam-3");
  EXPECT_FALSE(selection.same_analog);
  EXPECT_TRUE(is_ntn_rebalance_source_hot(request, selection));
}

TEST(ntn_beam_rebalance_policy, cold_cross_analog_target_requests_preheat_without_selecting_target)
{
  ntn_rebalance_policy_request request = make_request();
  find_assignment(request, "beam-2").digital_ue_cap = 1;
  find_assignment(request, "beam-2").digital_drb_cap = 1;
  find_assignment(request, "beam-3").access_du_index = du_index_t::invalid;

  const ntn_rebalance_target_selection selection = select_ntn_rebalance_target(request);

  EXPECT_FALSE(selection.assignment.has_value());
  EXPECT_TRUE(selection.skipped_cold_analog);
  EXPECT_EQ(selection.reason, "cold_analog");
  EXPECT_EQ(selection.preheat_beam_id, "beam-3");
  EXPECT_EQ(selection.preheat_analog_beam_id, "analog-b");
}

TEST(ntn_beam_rebalance_policy, projected_capacity_blocks_target)
{
  ntn_rebalance_policy_request request = make_request();
  find_assignment(request, "beam-2").digital_ue_cap = 1;
  find_assignment(request, "beam-2").digital_drb_cap = 1;
  find_assignment(request, "beam-3").digital_ue_cap = 0;
  find_assignment(request, "beam-3").digital_drb_cap = 0;

  const ntn_rebalance_target_selection selection = select_ntn_rebalance_target(request);

  EXPECT_FALSE(selection.assignment.has_value());
  EXPECT_TRUE(selection.skipped_projected_capacity);
  EXPECT_EQ(selection.reason, "projected_capacity");
}

TEST(ntn_beam_rebalance_policy, pending_handover_counts_as_projected_target_load)
{
  ntn_rebalance_policy_request request = make_request();
  find_assignment(request, "beam-2").digital_ue_cap = 2;
  request.pending_handovers.push_back({"beam-9", "beam-2", "analog-x", "analog-a", uint_to_du_index(0), 1});
  find_assignment(request, "beam-3").access_du_index = du_index_t::invalid;

  const ntn_rebalance_target_selection selection = select_ntn_rebalance_target(request);

  EXPECT_FALSE(selection.assignment.has_value());
  EXPECT_TRUE(selection.skipped_projected_capacity);
  EXPECT_EQ(selection.reason, "projected_capacity");
}

TEST(ntn_beam_rebalance_policy, pair_cooldown_blocks_cross_analog_target)
{
  ntn_rebalance_policy_request request = make_request();
  find_assignment(request, "beam-2").digital_ue_cap = 1;
  find_assignment(request, "beam-2").digital_drb_cap = 1;
  request.active_analog_pair_cooldowns.insert(make_ntn_analog_rebalance_pair_key("analog-a", "analog-b"));

  const ntn_rebalance_target_selection selection = select_ntn_rebalance_target(request);

  EXPECT_FALSE(selection.assignment.has_value());
  EXPECT_TRUE(selection.skipped_pair_cooldown);
  EXPECT_EQ(selection.reason, "pair_cooldown");
}

TEST(ntn_beam_rebalance_policy, preheat_ready_guard_blocks_until_target_is_ready)
{
  ntn_rebalance_policy_request request = make_request();
  find_assignment(request, "beam-2").digital_ue_cap = 1;
  find_assignment(request, "beam-2").digital_drb_cap = 1;
  request.preheat_states.push_back({"beam-3", true, false});

  const ntn_rebalance_target_selection guarded_selection = select_ntn_rebalance_target(request);

  EXPECT_FALSE(guarded_selection.assignment.has_value());
  EXPECT_TRUE(guarded_selection.skipped_preheat_ready_guard);
  EXPECT_EQ(guarded_selection.reason, "preheat_ready_guard");

  request.preheat_states.front().ready = true;
  const ntn_rebalance_target_selection ready_selection = select_ntn_rebalance_target(request);

  ASSERT_TRUE(ready_selection.assignment.has_value());
  EXPECT_EQ(ready_selection.assignment->beam_id, "beam-3");
}
