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

TEST(ntn_beam_placement_planner, keeps_previous_active_assignment_when_still_visible)
{
  ntn_beam_placement_request request;
  request.beams         = {make_beam("CN-BEAM-0001", 1), make_beam("CN-BEAM-0002", 2)};
  request.visible_beams = {{"CN-BEAM-0001", 72.0}, {"CN-BEAM-0002", 60.0}};
  request.du_capacities = {{uint_to_du_index(0), 2, 0, 0, {}}, {uint_to_du_index(1), 2, 0, 0, {}}};
  request.beam_loads    = {{"CN-BEAM-0001", 8, 16}, {"CN-BEAM-0002", 1, 2}};
  request.previous_assignments = {{"CN-BEAM-0001",
                                   make_nci(1),
                                   uint_to_du_index(1),
                                   ntn_beam_assignment_state::active,
                                   70.0,
                                   8,
                                   16}};

  const ntn_beam_placement_plan plan = ntn_beam_placement_planner{}.plan(request);

  const ntn_beam_du_assignment beam1 = find_assignment(plan, "CN-BEAM-0001");
  EXPECT_EQ(beam1.state, ntn_beam_assignment_state::active);
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
                                   ntn_beam_assignment_state::active,
                                   70.0,
                                   5,
                                   10}};

  const ntn_beam_placement_plan plan = ntn_beam_placement_planner{}.plan(request);

  const ntn_beam_du_assignment beam2 = find_assignment(plan, "CN-BEAM-0002");
  EXPECT_EQ(beam2.state, ntn_beam_assignment_state::active);
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
  EXPECT_EQ(beam1.state, ntn_beam_assignment_state::active);
  EXPECT_EQ(beam1.du_index, uint_to_du_index(0));

  const ntn_beam_du_assignment beam2 = find_assignment(plan, "CN-BEAM-0002");
  EXPECT_EQ(beam2.state, ntn_beam_assignment_state::active);
  EXPECT_EQ(beam2.du_index, uint_to_du_index(1));

  const ntn_beam_du_assignment beam3 = find_assignment(plan, "CN-BEAM-0003");
  EXPECT_EQ(beam3.state, ntn_beam_assignment_state::active);
  EXPECT_EQ(beam3.du_index, uint_to_du_index(0));

  EXPECT_EQ(get_active_ntn_beam_ids(plan, request.visible_beams),
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
  EXPECT_EQ(beam1.state, ntn_beam_assignment_state::active);
  EXPECT_EQ(beam1.nof_antenna_slots, 0U);
  EXPECT_EQ(beam1.antenna_slot_period, 0U);
  EXPECT_EQ(beam1.sr_slot_period, 0U);
  EXPECT_EQ(beam1.srs_slot_period, 0U);

  const ntn_beam_du_assignment beam2 = find_assignment(plan, "CN-BEAM-0002");
  EXPECT_EQ(beam2.state, ntn_beam_assignment_state::active);
  EXPECT_EQ(beam2.nof_antenna_slots, 1U);
  EXPECT_EQ(beam2.antenna_slot_index, 3U);
  EXPECT_EQ(beam2.antenna_slot_period, 4U);
  EXPECT_EQ(beam2.sr_slot_offset, 3U);
  EXPECT_EQ(beam2.sr_slot_period, 4U);
  EXPECT_EQ(beam2.srs_slot_offset, 3U);
  EXPECT_EQ(beam2.srs_slot_period, 4U);

  const ntn_beam_du_assignment beam3 = find_assignment(plan, "CN-BEAM-0003");
  EXPECT_EQ(beam3.state, ntn_beam_assignment_state::active);
  EXPECT_EQ(beam3.nof_antenna_slots, 3U);
  EXPECT_EQ(beam3.antenna_slot_index, 0U);
  EXPECT_EQ(beam3.antenna_slot_period, 4U);
  EXPECT_EQ(beam3.sr_slot_offset, 0U);
  EXPECT_EQ(beam3.sr_slot_period, 4U);
  EXPECT_EQ(beam3.srs_slot_offset, 2U);
  EXPECT_EQ(beam3.srs_slot_period, 4U);
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
  EXPECT_EQ(beam1.state, ntn_beam_assignment_state::active);
  EXPECT_EQ(beam1.nof_antenna_slots, 1U);
  EXPECT_EQ(beam1.antenna_slot_index, 0U);
  EXPECT_EQ(beam1.antenna_slot_period, 1U);
  EXPECT_EQ(beam1.sr_slot_offset, 0U);
  EXPECT_EQ(beam1.sr_slot_period, 1U);
  EXPECT_EQ(beam1.srs_slot_offset, 0U);
  EXPECT_EQ(beam1.srs_slot_period, 1U);
}

TEST(ntn_beam_placement_planner, leaves_overflow_multi_beam_candidate_out_of_active_served_set)
{
  ntn_beam_placement_request request;
  request.beams         = {make_beam("CN-BEAM-0001", 1),
                           make_beam("CN-BEAM-0002", 2),
                           make_beam("CN-BEAM-0003", 3)};
  request.visible_beams = {{"CN-BEAM-0001", 70.0}, {"CN-BEAM-0002", 69.0}, {"CN-BEAM-0003", 68.0}};
  request.du_capacities = {{uint_to_du_index(0), 2, 0, 0, {}}};

  const ntn_beam_placement_plan plan = ntn_beam_placement_planner{}.plan(request);

  EXPECT_EQ(find_assignment(plan, "CN-BEAM-0001").state, ntn_beam_assignment_state::active);
  EXPECT_EQ(find_assignment(plan, "CN-BEAM-0002").state, ntn_beam_assignment_state::active);

  const ntn_beam_du_assignment beam3 = find_assignment(plan, "CN-BEAM-0003");
  EXPECT_EQ(beam3.state, ntn_beam_assignment_state::candidate);
  EXPECT_EQ(beam3.du_index, du_index_t::invalid);

  EXPECT_EQ(get_active_ntn_beam_ids(plan, request.visible_beams),
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

  const ntn_beam_placement_plan plan = ntn_beam_placement_planner{}.plan(request);

  EXPECT_EQ(find_assignment(plan, "CN-BEAM-0001").state, ntn_beam_assignment_state::active);
  EXPECT_EQ(find_assignment(plan, "CN-BEAM-0002").state, ntn_beam_assignment_state::active);
  EXPECT_EQ(find_assignment(plan, "CN-BEAM-0003").state, ntn_beam_assignment_state::candidate);
  EXPECT_EQ(get_active_ntn_beam_ids(plan, request.visible_beams),
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

  const ntn_beam_placement_plan plan = ntn_beam_placement_planner{}.plan(request);

  const ntn_beam_du_assignment beam1 = find_assignment(plan, "CN-BEAM-0001");
  const ntn_beam_du_assignment beam2 = find_assignment(plan, "CN-BEAM-0002");
  const ntn_beam_du_assignment beam3 = find_assignment(plan, "CN-BEAM-0003");
  EXPECT_EQ(beam1.state, ntn_beam_assignment_state::active);
  EXPECT_TRUE(beam1.in_hopping_window);
  EXPECT_EQ(beam2.state, ntn_beam_assignment_state::candidate);
  EXPECT_FALSE(beam2.in_hopping_window);
  EXPECT_EQ(beam3.state, ntn_beam_assignment_state::active);
  EXPECT_TRUE(beam3.in_hopping_window);
  EXPECT_EQ(get_active_ntn_beam_ids(plan, request.visible_beams),
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
  request.beam_loads       = {{"CN-BEAM-0003", 5, 10}};

  const ntn_beam_placement_plan plan = ntn_beam_placement_planner{}.plan(request);

  const ntn_beam_du_assignment beam1 = find_assignment(plan, "CN-BEAM-0001");
  const ntn_beam_du_assignment beam2 = find_assignment(plan, "CN-BEAM-0002");
  const ntn_beam_du_assignment beam3 = find_assignment(plan, "CN-BEAM-0003");
  EXPECT_EQ(beam1.state, ntn_beam_assignment_state::active);
  EXPECT_TRUE(beam1.in_hopping_window);
  EXPECT_EQ(beam2.state, ntn_beam_assignment_state::active);
  EXPECT_TRUE(beam2.in_hopping_window);
  EXPECT_EQ(beam3.state, ntn_beam_assignment_state::candidate);
  EXPECT_FALSE(beam3.in_hopping_window);
  EXPECT_EQ(get_active_ntn_beam_ids(plan, request.visible_beams),
            std::vector<std::string>({"CN-BEAM-0001", "CN-BEAM-0002"}));
}

TEST(ntn_beam_placement_planner, loaded_visible_beam_outside_hopping_window_does_not_preempt_unloaded_window_beam)
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
  request.beam_loads       = {{"CN-BEAM-0003", 5, 10}};
  request.previous_assignments = {{"CN-BEAM-0001",
                                   make_nci(1),
                                   uint_to_du_index(0),
                                   ntn_beam_assignment_state::active,
                                   70.0,
                                   0,
                                   0},
                                  {"CN-BEAM-0002",
                                   make_nci(2),
                                   uint_to_du_index(0),
                                   ntn_beam_assignment_state::active,
                                   69.0,
                                   0,
                                   0}};

  const ntn_beam_placement_plan plan = ntn_beam_placement_planner{}.plan(request);

  EXPECT_EQ(find_assignment(plan, "CN-BEAM-0001").state, ntn_beam_assignment_state::active);
  EXPECT_EQ(find_assignment(plan, "CN-BEAM-0002").state, ntn_beam_assignment_state::active);
  EXPECT_EQ(find_assignment(plan, "CN-BEAM-0003").state, ntn_beam_assignment_state::candidate);
}

TEST(ntn_beam_placement_planner, marks_previous_active_beam_as_draining_when_it_leaves_hopping_window)
{
  ntn_beam_placement_request request;
  request.beams         = {make_beam("CN-BEAM-0001", 1), make_beam("CN-BEAM-0002", 2)};
  request.visible_beams = {{"CN-BEAM-0001", 70.0, false}, {"CN-BEAM-0002", 69.0, true}};
  request.du_capacities = {{uint_to_du_index(0), 2, 0, 0, {}}};
  request.previous_assignments = {{"CN-BEAM-0001",
                                   make_nci(1),
                                   uint_to_du_index(0),
                                   ntn_beam_assignment_state::active,
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
  EXPECT_EQ(beam2.state, ntn_beam_assignment_state::active);
  EXPECT_TRUE(beam2.in_hopping_window);
  EXPECT_EQ(get_active_ntn_beam_ids(plan, request.visible_beams), std::vector<std::string>({"CN-BEAM-0002"}));
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
  EXPECT_EQ(beam2.state, ntn_beam_assignment_state::active);
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
                                   ntn_beam_assignment_state::active,
                                   70.0,
                                   3,
                                   6}};

  const ntn_beam_placement_plan plan = ntn_beam_placement_planner{}.plan(request);

  EXPECT_EQ(find_assignment(plan, "CN-BEAM-0001").state, ntn_beam_assignment_state::draining);
  EXPECT_EQ(find_assignment(plan, "CN-BEAM-0002").state, ntn_beam_assignment_state::candidate);
  EXPECT_TRUE(get_active_ntn_beam_ids(plan, request.visible_beams).empty());
}

TEST(ntn_beam_placement_planner, does_not_retain_draining_beam_when_previous_du_is_unavailable)
{
  ntn_beam_placement_request request;
  request.beams = {make_beam("CN-BEAM-0001", 1)};
  request.beam_loads = {{"CN-BEAM-0001", 3, 6}};
  request.previous_assignments = {{"CN-BEAM-0001",
                                   make_nci(1),
                                   uint_to_du_index(0),
                                   ntn_beam_assignment_state::active,
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
  EXPECT_EQ(find_assignment(plan, "CN-BEAM-0002").state, ntn_beam_assignment_state::active);
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
  EXPECT_TRUE(get_active_ntn_beam_ids(plan, request.visible_beams).empty());
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
  EXPECT_TRUE(get_active_ntn_beam_ids(plan, request.visible_beams).empty());
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
  EXPECT_TRUE(get_active_ntn_beam_ids(plan, request.visible_beams).empty());
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
  EXPECT_TRUE(get_active_ntn_beam_ids(plan, request.visible_beams).empty());
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
                                   ntn_beam_assignment_state::active,
                                   55.0,
                                   3,
                                   6}};

  const ntn_beam_placement_plan plan = ntn_beam_placement_planner{}.plan(request);

  const ntn_beam_du_assignment beam1 = find_assignment(plan, "CN-BEAM-0001");
  EXPECT_EQ(beam1.state, ntn_beam_assignment_state::draining);
  EXPECT_EQ(beam1.du_index, uint_to_du_index(0));
  EXPECT_EQ(beam1.nof_ues, 3U);

  const ntn_beam_du_assignment beam2 = find_assignment(plan, "CN-BEAM-0002");
  EXPECT_EQ(beam2.state, ntn_beam_assignment_state::active);
}

TEST(ntn_beam_assignment_repository, indexes_active_assignments_by_beam_id_and_nci)
{
  const nr_cell_identity beam1_nci = make_nci(1);
  const nr_cell_identity beam2_nci = make_nci(2);

  ntn_beam_placement_plan plan;
  plan.assignments = {{"CN-BEAM-0001",
                       beam1_nci,
                       uint_to_du_index(0),
                       ntn_beam_assignment_state::active,
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

TEST(ntn_beam_placement_plan_helpers, extracts_active_beams_in_visible_candidate_order)
{
  ntn_beam_placement_plan plan;
  plan.assignments = {{"CN-BEAM-0001",
                       make_nci(1),
                       uint_to_du_index(0),
                       ntn_beam_assignment_state::active,
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
                       ntn_beam_assignment_state::active,
                       60.0,
                       1,
                       1}};

  const std::vector<std::string> active_beam_ids =
      get_active_ntn_beam_ids(plan, {{"CN-BEAM-0003", 60.0}, {"CN-BEAM-0002", 65.0}, {"CN-BEAM-0001", 70.0}});

  EXPECT_EQ(active_beam_ids, std::vector<std::string>({"CN-BEAM-0003", "CN-BEAM-0001"}));
}
