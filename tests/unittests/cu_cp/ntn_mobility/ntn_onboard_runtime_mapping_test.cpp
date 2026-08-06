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

#include "lib/cu_cp/ntn_mobility/ntn_onboard_runtime_mapping.h"
#include "fmt/format.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <set>

using namespace srsran;
using namespace srsran::srs_cu_cp;

namespace {

const nr_cell_identity first_nci  = nr_cell_identity::create(0x123450001ULL).value();
const nr_cell_identity second_nci = nr_cell_identity::create(0x123450002ULL).value();

std::string make_position_id(unsigned index)
{
  return fmt::format("G{:06}", index);
}

ntn_activated_position_plan make_active_plan(unsigned visible_count, unsigned assigned_count, unsigned schema = 4)
{
  ntn_activated_position_plan result;
  result.source.schema_version   = schema;
  result.source.satellite_id     = "P01-S01";
  result.source.catalog_version  = 17;
  result.source.schedule_version = 29;
  result.source.content_hash     = "sha256:" + std::string(64, 'a');
  result.source.valid_from = std::chrono::system_clock::time_point{std::chrono::milliseconds{640}};
  result.source.activation_epoch = std::chrono::system_clock::time_point{std::chrono::milliseconds{1280}};
  result.source.valid_until = std::chrono::system_clock::time_point{std::chrono::milliseconds{12800}};
  result.source.onboard_cells = {{{first_nci, 101}, {second_nci, 202}}};
  result.calendar_hash        = "sha256:" + std::string(64, 'b');

  result.source.visible_l1_positions.reserve(visible_count);
  for (unsigned i = 0; i != visible_count; ++i) {
    result.source.visible_l1_positions.push_back(
        {make_position_id(i), -40.0 + static_cast<double>(i % 80), 80.0 + static_cast<double>(i % 90),
         static_cast<uint8_t>((i % 0x7fU) + 1U)});
  }

  result.cell_positions[0].identity = result.source.onboard_cells[0];
  result.cell_positions[1].identity = result.source.onboard_cells[1];
  result.source.assigned_l1_position_ids.reserve(assigned_count);
  for (unsigned i = 0; i != assigned_count; ++i) {
    const std::string position_id = make_position_id(i);
    result.source.assigned_l1_position_ids.push_back(position_id);
    result.cell_positions[i % 2].assigned_l1_ids.push_back(position_id);
  }
  return result;
}

std::array<ntn_onboard_runtime_cell_route, 2> make_live_routes()
{
  // Deliberately reverse the route input order. Snapshot order follows the plan's stable cell identities.
  return {{{{second_nci, 202},
            {plmn_identity::test_value(), second_nci},
            202,
            uint_to_du_index(4),
            uint_to_du_cell_index(1),
            9,
            ntn_onboard_tai_status::ready},
           {{first_nci, 101},
            {plmn_identity::test_value(), first_nci},
            101,
            uint_to_du_index(4),
            uint_to_du_cell_index(0),
            9,
            ntn_onboard_tai_status::ready}}};
}

std::shared_ptr<const ntn_onboard_runtime_mapping_snapshot>
build_snapshot(const ntn_activated_position_plan& plan,
               const std::array<ntn_onboard_runtime_cell_route, 2>& routes = make_live_routes())
{
  auto result = ntn_onboard_runtime_mapping_snapshot::create(plan, routes);
  EXPECT_TRUE(result.has_value()) << (result.has_value() ? "" : result.error());
  return result.has_value() ? result.value() : nullptr;
}

} // namespace

TEST(ntn_onboard_runtime_mapping, maps_only_the_87_assigned_positions_from_a_300_position_inventory)
{
  const ntn_activated_position_plan plan     = make_active_plan(300, 87);
  const auto                        snapshot = build_snapshot(plan);
  ASSERT_NE(snapshot, nullptr);

  EXPECT_EQ(snapshot->satellite_id(), "P01-S01");
  EXPECT_EQ(snapshot->catalog_version(), 17U);
  EXPECT_EQ(snapshot->schedule_version(), 29U);
  EXPECT_EQ(snapshot->source_hash(), plan.source.content_hash);
  EXPECT_EQ(snapshot->calendar_hash(), plan.calendar_hash);
  EXPECT_EQ(snapshot->valid_from(), plan.source.valid_from);
  EXPECT_EQ(snapshot->valid_until(), plan.source.valid_until);
  EXPECT_EQ(snapshot->activation_epoch(), plan.source.activation_epoch);
  EXPECT_EQ(snapshot->nof_positions(), 87U);
  EXPECT_EQ(snapshot->positions_for_nci(first_nci).size(), 44U);
  EXPECT_EQ(snapshot->positions_for_nci(second_nci).size(), 43U);

  std::set<std::string> mapped_ids;
  for (const ntn_onboard_runtime_position& mapped : snapshot->positions()) {
    EXPECT_TRUE(mapped_ids.insert(mapped.position.position_id).second);
  }
  EXPECT_EQ(mapped_ids.size(), 87U);
  for (unsigned i = 0; i != 300; ++i) {
    EXPECT_EQ(snapshot->find_position(make_position_id(i)) != nullptr, i < 87);
  }
}

TEST(ntn_onboard_runtime_mapping, returns_multiple_positions_per_nci_in_stable_order_and_preserves_child_mask)
{
  ntn_activated_position_plan plan = make_active_plan(20, 12);
  std::reverse(plan.cell_positions[0].assigned_l1_ids.begin(), plan.cell_positions[0].assigned_l1_ids.end());
  std::reverse(plan.cell_positions[1].assigned_l1_ids.begin(), plan.cell_positions[1].assigned_l1_ids.end());

  const auto snapshot = build_snapshot(plan);
  ASSERT_NE(snapshot, nullptr);
  const span<const ntn_onboard_runtime_position> first_cell_positions = snapshot->positions_for_nci(first_nci);
  ASSERT_EQ(first_cell_positions.size(), 6U);
  for (size_t i = 1; i != first_cell_positions.size(); ++i) {
    EXPECT_LT(first_cell_positions[i - 1].position.position_id, first_cell_positions[i].position.position_id);
  }

  const ntn_onboard_runtime_position* position = snapshot->find_position(make_position_id(10));
  ASSERT_NE(position, nullptr);
  EXPECT_EQ(position->owner.nci, first_nci);
  EXPECT_EQ(position->owner.pci, 101);
  EXPECT_EQ(position->position.child_mask, 11U);
  EXPECT_TRUE(snapshot->positions_for_nci(nr_cell_identity::create(0x123450003ULL).value()).empty());
}

TEST(ntn_onboard_runtime_mapping, classifies_only_positions_present_in_the_same_snapshot)
{
  const auto snapshot = build_snapshot(make_active_plan(10, 8));
  ASSERT_NE(snapshot, nullptr);

  EXPECT_EQ(snapshot->classify_position_transition(make_position_id(0), make_position_id(0)),
            ntn_position_transition::no_change);
  EXPECT_EQ(snapshot->classify_position_transition(make_position_id(0), make_position_id(2)),
            ntn_position_transition::same_cell);
  EXPECT_EQ(snapshot->classify_position_transition(make_position_id(0), make_position_id(1)),
            ntn_position_transition::cell_change);
  EXPECT_EQ(snapshot->classify_position_transition(make_position_id(0), make_position_id(9)),
            ntn_position_transition::unknown);
  EXPECT_EQ(snapshot->classify_position_transition("G999999", "G999999"), ntn_position_transition::unknown);
}

TEST(ntn_onboard_runtime_mapping, rejects_any_assignment_that_is_not_an_exact_once_partition)
{
  ntn_activated_position_plan duplicate = make_active_plan(10, 8);
  duplicate.cell_positions[1].assigned_l1_ids.push_back(make_position_id(0));
  auto duplicate_result = ntn_onboard_runtime_mapping_snapshot::create(duplicate, make_live_routes());
  ASSERT_FALSE(duplicate_result.has_value());
  EXPECT_EQ(duplicate_result.error(), "duplicate_position_assignment");

  ntn_activated_position_plan missing = make_active_plan(10, 8);
  missing.cell_positions[1].assigned_l1_ids.pop_back();
  auto missing_result = ntn_onboard_runtime_mapping_snapshot::create(missing, make_live_routes());
  ASSERT_FALSE(missing_result.has_value());
  EXPECT_EQ(missing_result.error(), "assignment_set_mismatch");

  ntn_activated_position_plan unexpected = make_active_plan(10, 8);
  unexpected.cell_positions[0].assigned_l1_ids.push_back(make_position_id(9));
  auto unexpected_result = ntn_onboard_runtime_mapping_snapshot::create(unexpected, make_live_routes());
  ASSERT_FALSE(unexpected_result.has_value());
  EXPECT_EQ(unexpected_result.error(), "unexpected_position_assignment");
}

TEST(ntn_onboard_runtime_mapping, accepts_an_empty_assigned_set_without_treating_visible_positions_as_active)
{
  const auto snapshot = build_snapshot(make_active_plan(300, 0));
  ASSERT_NE(snapshot, nullptr);
  EXPECT_EQ(snapshot->nof_positions(), 0U);
  EXPECT_TRUE(snapshot->positions().empty());
  EXPECT_TRUE(snapshot->positions_for_nci(first_nci).empty());
  EXPECT_TRUE(snapshot->positions_for_nci(second_nci).empty());
  EXPECT_NE(snapshot->resolve_cell_route(first_nci), nullptr);
  EXPECT_NE(snapshot->resolve_cell_route(second_nci), nullptr);
}

TEST(ntn_onboard_runtime_mapping, keeps_tai_policy_status_separate_from_position_mapping_availability)
{
  const std::array<ntn_onboard_tai_status, 4> statuses{
      ntn_onboard_tai_status::ready,
      ntn_onboard_tai_status::supported_tai_missing,
      ntn_onboard_tai_status::supported_tai_duplicate,
      ntn_onboard_tai_status::plmn_tac_mismatch};

  for (ntn_onboard_tai_status status : statuses) {
    auto routes          = make_live_routes();
    routes[1].tai_status = status;
    const auto snapshot  = build_snapshot(make_active_plan(10, 8), routes);
    ASSERT_NE(snapshot, nullptr);
    EXPECT_EQ(snapshot->nof_positions(), 8U);
    const ntn_onboard_runtime_cell_route* route = snapshot->resolve_cell_route(first_nci);
    ASSERT_NE(route, nullptr);
    EXPECT_EQ(route->tai_status, status);
    EXPECT_EQ(route->ncgi.nci, first_nci);
    EXPECT_EQ(route->tac, 101U);
    EXPECT_EQ(route->du_index, uint_to_du_index(4));
    EXPECT_EQ(route->du_cell_index, uint_to_du_cell_index(0));
    EXPECT_EQ(route->du_connection_generation, 9U);
  }
}

TEST(ntn_onboard_runtime_mapping, hides_a_snapshot_when_the_live_du_cell_route_changes)
{
  const auto snapshot = build_snapshot(make_active_plan(10, 8));
  ASSERT_NE(snapshot, nullptr);
  EXPECT_TRUE(snapshot->matches_cell_routes(make_live_routes()));

  auto changed_tac = make_live_routes();
  changed_tac[1].tac = 102;
  EXPECT_FALSE(snapshot->matches_cell_routes(changed_tac));

  auto changed_cell = make_live_routes();
  changed_cell[1].du_cell_index = uint_to_du_cell_index(7);
  EXPECT_FALSE(snapshot->matches_cell_routes(changed_cell));

  auto changed_connection = make_live_routes();
  changed_connection[1].du_connection_generation = 10;
  EXPECT_FALSE(snapshot->matches_cell_routes(changed_connection));

  auto changed_tai_policy = make_live_routes();
  changed_tai_policy[1].tai_status = ntn_onboard_tai_status::plmn_tac_mismatch;
  EXPECT_FALSE(snapshot->matches_cell_routes(changed_tai_policy));

  auto duplicate_identity = make_live_routes();
  duplicate_identity[0].identity = duplicate_identity[1].identity;
  EXPECT_FALSE(snapshot->matches_cell_routes(duplicate_identity));
}

TEST(ntn_onboard_runtime_mapping, rejects_routes_from_the_wrong_cell_du_or_connection_generation)
{
  const ntn_activated_position_plan plan = make_active_plan(10, 8);

  auto wrong_ncgi            = make_live_routes();
  wrong_ncgi[1].ncgi.nci     = second_nci;
  auto wrong_ncgi_result = ntn_onboard_runtime_mapping_snapshot::create(plan, wrong_ncgi);
  ASSERT_FALSE(wrong_ncgi_result.has_value());
  EXPECT_EQ(wrong_ncgi_result.error(), "cell_route_ncgi_mismatch");

  auto cross_du        = make_live_routes();
  cross_du[0].du_index = uint_to_du_index(5);
  auto cross_du_result = ntn_onboard_runtime_mapping_snapshot::create(plan, cross_du);
  ASSERT_FALSE(cross_du_result.has_value());
  EXPECT_EQ(cross_du_result.error(), "cross_du_mapping_not_supported");

  auto old_connection                       = make_live_routes();
  old_connection[0].du_connection_generation = 8;
  auto old_connection_result = ntn_onboard_runtime_mapping_snapshot::create(plan, old_connection);
  ASSERT_FALSE(old_connection_result.has_value());
  EXPECT_EQ(old_connection_result.error(), "du_connection_generation_mismatch");
}

TEST(ntn_onboard_runtime_mapping, schema_v1_and_v2_empty_assigned_array_keeps_legacy_assigned_equals_visible_semantics)
{
  for (unsigned schema : {1U, 2U}) {
    ntn_activated_position_plan plan = make_active_plan(8, 0, schema);
    if (schema == 1) {
      for (ntn_l1_position& position : plan.source.visible_l1_positions) {
        position.child_mask = 0;
      }
    }
    for (unsigned i = 0; i != 8; ++i) {
      plan.cell_positions[i % 2].assigned_l1_ids.push_back(make_position_id(i));
    }
    const auto snapshot = build_snapshot(plan);
    ASSERT_NE(snapshot, nullptr);
    EXPECT_EQ(snapshot->nof_positions(), 8U);
  }
}

TEST(ntn_onboard_runtime_mapping, string_forms_are_machine_readable)
{
  EXPECT_STREQ(to_string(ntn_onboard_runtime_mapping_stage::awaiting_live_du), "awaiting_live_du");
  EXPECT_STREQ(to_string(ntn_onboard_tai_status::supported_tai_duplicate), "supported_tai_duplicate");
  EXPECT_STREQ(to_string(ntn_position_transition::same_cell), "same_cell");
}
