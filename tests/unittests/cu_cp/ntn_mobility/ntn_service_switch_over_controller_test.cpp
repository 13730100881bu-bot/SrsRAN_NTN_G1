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

#include "lib/cu_cp/ntn_mobility/ntn_service_switch_over_controller.h"
#include "srsran/ran/gnb_id.h"
#include <algorithm>
#include <chrono>
#include <gtest/gtest.h>
#include <string>
#include <utility>

using namespace srsran;
using namespace srs_cu_cp;

namespace {

nr_cell_identity make_nci(unsigned sector_id)
{
  return nr_cell_identity::create(gnb_id_t{411, 22}, sector_id).value();
}

ntn_service_switch_over_event make_event(uint64_t id, ntn_service_switch_over_type type, std::string beam_id)
{
  ntn_service_switch_over_event event;
  event.event_id = id;
  event.type     = type;
  event.source   = ntn_service_switch_over_source::operator_command;
  event.policy   = type == ntn_service_switch_over_type::soft ? ntn_service_switch_over_policy::prepare
                                                              : ntn_service_switch_over_policy::drain;
  event.affected_beam_ids.push_back(std::move(beam_id));
  event.start_time = std::chrono::steady_clock::now();
  event.reason     = "unit-test";
  return event;
}

const ntn_service_switch_over_beam_snapshot* find_beam(const ntn_service_switch_over_snapshot& snapshot,
                                                       const std::string&                      beam_id)
{
  const auto it = std::find_if(
      snapshot.beams.begin(), snapshot.beams.end(), [&beam_id](const ntn_service_switch_over_beam_snapshot& beam) {
        return beam.beam_id == beam_id;
      });
  return it == snapshot.beams.end() ? nullptr : &*it;
}

} // namespace

TEST(ntn_service_switch_over_controller_test, soft_event_marks_beam_for_preparation)
{
  ntn_service_switch_over_controller controller;
  ASSERT_TRUE(controller.apply_event(make_event(1, ntn_service_switch_over_type::soft, "CN-BEAM-0001")));

  ntn_service_switch_over_snapshot snapshot =
      controller.build_snapshot({{"CN-BEAM-0001", make_nci(0)}, {"CN-BEAM-0002", make_nci(1)}});

  ASSERT_EQ(snapshot.active_events.size(), 1);
  ASSERT_EQ(snapshot.beams.size(), 2);
  const ntn_service_switch_over_beam_snapshot* first_beam = find_beam(snapshot, "CN-BEAM-0001");
  ASSERT_NE(first_beam, nullptr);
  EXPECT_EQ(first_beam->policy, ntn_service_beam_policy::prepare);
  const ntn_service_switch_over_beam_snapshot* second_beam = find_beam(snapshot, "CN-BEAM-0002");
  ASSERT_NE(second_beam, nullptr);
  EXPECT_EQ(second_beam->policy, ntn_service_beam_policy::normal);
  EXPECT_FALSE(controller.blocks_new_demand_for_beam("CN-BEAM-0001"));
  EXPECT_FALSE(controller.forces_drain_for_beam("CN-BEAM-0001"));
}

TEST(ntn_service_switch_over_controller_test, hard_event_blocks_new_demand_and_forces_drain)
{
  ntn_service_switch_over_controller controller;
  ASSERT_TRUE(controller.apply_event(make_event(2, ntn_service_switch_over_type::hard, "CN-BEAM-0001")));

  EXPECT_TRUE(controller.blocks_new_demand_for_beam("CN-BEAM-0001"));
  EXPECT_TRUE(controller.forces_drain_for_beam("CN-BEAM-0001"));
  EXPECT_FALSE(controller.blocks_new_demand_for_beam("CN-BEAM-0002"));
}

TEST(ntn_service_switch_over_controller_test, hard_prepare_event_blocks_new_demand_without_forcing_drain)
{
  ntn_service_switch_over_controller controller;
  ntn_service_switch_over_event      event = make_event(6, ntn_service_switch_over_type::hard, "CN-BEAM-0001");
  event.policy                             = ntn_service_switch_over_policy::prepare;
  ASSERT_TRUE(controller.apply_event(event));

  ntn_service_switch_over_snapshot snapshot = controller.build_snapshot({{"CN-BEAM-0001", make_nci(0)}});

  const ntn_service_switch_over_beam_snapshot* beam = find_beam(snapshot, "CN-BEAM-0001");
  ASSERT_NE(beam, nullptr);
  EXPECT_EQ(beam->policy, ntn_service_beam_policy::block_new_demand);
  EXPECT_TRUE(controller.blocks_new_demand_for_beam("CN-BEAM-0001"));
  EXPECT_FALSE(controller.forces_drain_for_beam("CN-BEAM-0001"));
}

TEST(ntn_service_switch_over_controller_test, release_allowed_hard_policy_wins_over_drain_policy)
{
  ntn_service_switch_over_controller controller;
  ASSERT_TRUE(controller.apply_event(make_event(3, ntn_service_switch_over_type::hard, "CN-BEAM-0001")));
  ntn_service_switch_over_event release_event = make_event(4, ntn_service_switch_over_type::hard, "CN-BEAM-0001");
  release_event.policy = ntn_service_switch_over_policy::release_allowed;
  ASSERT_TRUE(controller.apply_event(release_event));

  ntn_service_switch_over_snapshot snapshot = controller.build_snapshot({{"CN-BEAM-0001", make_nci(0)}});

  const ntn_service_switch_over_beam_snapshot* beam = find_beam(snapshot, "CN-BEAM-0001");
  ASSERT_NE(beam, nullptr);
  EXPECT_EQ(beam->policy, ntn_service_beam_policy::release_allowed);
  EXPECT_TRUE(controller.blocks_new_demand_for_beam("CN-BEAM-0001"));
  EXPECT_TRUE(controller.forces_drain_for_beam("CN-BEAM-0001"));
}

TEST(ntn_service_switch_over_controller_test, nci_only_event_marks_matching_known_beam)
{
  ntn_service_switch_over_controller controller;
  ntn_service_switch_over_event      event = make_event(5, ntn_service_switch_over_type::hard, "unused");
  event.affected_beam_ids.clear();
  event.affected_ncis.push_back(make_nci(1));
  ASSERT_TRUE(controller.apply_event(event));

  ntn_service_switch_over_snapshot snapshot =
      controller.build_snapshot({{"CN-BEAM-0001", make_nci(0)}, {"CN-BEAM-0002", make_nci(1)}});

  ASSERT_EQ(snapshot.beams.size(), 2);
  const ntn_service_switch_over_beam_snapshot* first_beam = find_beam(snapshot, "CN-BEAM-0001");
  ASSERT_NE(first_beam, nullptr);
  EXPECT_EQ(first_beam->policy, ntn_service_beam_policy::normal);
  const ntn_service_switch_over_beam_snapshot* second_beam = find_beam(snapshot, "CN-BEAM-0002");
  ASSERT_NE(second_beam, nullptr);
  EXPECT_EQ(second_beam->policy, ntn_service_beam_policy::drain);
  EXPECT_TRUE(controller.blocks_new_demand_for_beam("CN-BEAM-0002", make_nci(1)));
  EXPECT_TRUE(controller.forces_drain_for_beam("CN-BEAM-0002", make_nci(1)));
  EXPECT_FALSE(controller.blocks_new_demand_for_beam("CN-BEAM-0002"));
}

TEST(ntn_service_switch_over_controller_test, clear_event_restores_normal_policy)
{
  ntn_service_switch_over_controller controller;
  ASSERT_TRUE(controller.apply_event(make_event(5, ntn_service_switch_over_type::hard, "CN-BEAM-0001")));
  ASSERT_TRUE(controller.clear_event(5));

  EXPECT_FALSE(controller.blocks_new_demand_for_beam("CN-BEAM-0001"));
  EXPECT_FALSE(controller.forces_drain_for_beam("CN-BEAM-0001"));
}

TEST(ntn_service_switch_over_controller_test, manual_freeze_disables_automatic_source_updates_until_restore)
{
  ntn_service_switch_over_controller controller;

  ntn_manual_override_command freeze;
  freeze.mode   = ntn_manual_override_mode::freeze_current_state;
  freeze.reason = "freeze-test";
  ASSERT_TRUE(controller.apply_manual_override(freeze));
  EXPECT_FALSE(controller.automatic_source_updates_allowed());

  ntn_manual_override_command restore;
  restore.mode = ntn_manual_override_mode::restore_automatic_source;
  ASSERT_TRUE(controller.apply_manual_override(restore));
  EXPECT_TRUE(controller.automatic_source_updates_allowed());
}
