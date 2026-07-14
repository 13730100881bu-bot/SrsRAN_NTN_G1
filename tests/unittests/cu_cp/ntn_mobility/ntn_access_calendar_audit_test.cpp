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

#include "lib/cu_cp/ntn_mobility/ntn_onboard_position_plan.h"
#include "fmt/format.h"
#include "gtest/gtest.h"
#include <algorithm>

using namespace srsran;
using namespace srsran::srs_cu_cp;

namespace {

std::chrono::system_clock::time_point at_ms(int64_t milliseconds)
{
  return std::chrono::system_clock::time_point{std::chrono::milliseconds{milliseconds}};
}

ntn_onboard_position_plan_config make_config()
{
  ntn_onboard_position_plan_config config;
  config.enabled          = true;
  config.satellite_id     = "P01-S001";
  config.onboard_cells[0] = {nr_cell_identity::create(0x123450001ULL).value(), 101};
  config.onboard_cells[1] = {nr_cell_identity::create(0x123450002ULL).value(), 202};
  return config;
}

ntn_versioned_position_plan make_plan(unsigned count)
{
  ntn_versioned_position_plan plan;
  plan.satellite_id     = "P01-S001";
  plan.catalog_version  = 1;
  plan.schedule_version = 7;
  plan.valid_from       = at_ms(640);
  plan.valid_until      = at_ms(64000);
  plan.activation_epoch = at_ms(1920);
  plan.onboard_cells    = make_config().onboard_cells;
  for (unsigned i = 0; i != count; ++i) {
    plan.visible_l1_positions.push_back(
        {fmt::format("G{:06}", i + 1), 10.0 + static_cast<double>(i / 32) * 0.05,
         20.0 + static_cast<double>(i % 32) * 0.05});
  }
  plan.content_hash = compute_ntn_position_plan_content_hash(plan);
  return plan;
}

const ntn_activated_position_plan& build_valid_plan(ntn_onboard_position_plan_controller& controller,
                                                     unsigned                              count = 256)
{
  EXPECT_TRUE(controller.submit(make_plan(count), at_ms(1280)).accepted);
  EXPECT_TRUE(controller.pending_plan().has_value());
  return *controller.pending_plan();
}

} // namespace

TEST(ntn_access_calendar_audit, full_capacity_calendar_meets_port_ssb_prach_and_pairing_limits)
{
  ntn_onboard_position_plan_controller controller(make_config());
  const auto&                          plan = build_valid_plan(controller);

  const auto audit = controller.audit_access_calendar(
      plan.source.schedule_version, plan.cell_positions, plan.access_calendar);

  EXPECT_TRUE(audit.accepted);
  EXPECT_EQ(audit.nof_l1_positions, 256U);
  EXPECT_EQ(audit.max_used_analog_ports_per_cell, 16U);
  EXPECT_EQ(audit.max_used_analog_ports_per_satellite, 32U);
  EXPECT_EQ(audit.max_ssb_interval, std::chrono::milliseconds{80});
  EXPECT_EQ(audit.max_prach_interval, std::chrono::milliseconds{640});
  EXPECT_EQ(audit.prach_ro_without_beam, 0U);
  EXPECT_EQ(audit.resource_conflicts, 0U);
}

TEST(ntn_access_calendar_audit, missing_one_ssb_visit_reports_interval_over_80_ms)
{
  ntn_onboard_position_plan_controller controller(make_config());
  const auto&                          plan = build_valid_plan(controller, 4);
  std::vector<ntn_access_calendar_intent> intents = plan.access_calendar;
  const std::string position_id = plan.cell_positions[0].assigned_l1_ids.front();
  const auto removed = std::find_if(intents.begin(), intents.end(), [&position_id](const auto& intent) {
    return intent.position_id == position_id &&
           (intent.purpose == ntn_access_calendar_purpose::ssb_sib_paging ||
            intent.purpose == ntn_access_calendar_purpose::ssb_sib_paging_rar);
  });
  ASSERT_NE(removed, intents.end());
  intents.erase(removed);

  const auto audit = controller.audit_access_calendar(plan.source.schedule_version, plan.cell_positions, intents);

  EXPECT_FALSE(audit.accepted);
  EXPECT_EQ(audit.reason, ntn_position_plan_reject_reason::ssb_deadline_miss);
  EXPECT_GT(audit.max_ssb_interval, std::chrono::milliseconds{80});
}

TEST(ntn_access_calendar_audit, missing_prach_opportunity_reports_interval_over_640_ms)
{
  ntn_onboard_position_plan_controller controller(make_config());
  const auto&                          plan = build_valid_plan(controller, 4);
  std::vector<ntn_access_calendar_intent> intents = plan.access_calendar;
  const std::string position_id = plan.cell_positions[0].assigned_l1_ids.front();
  intents.erase(std::remove_if(intents.begin(), intents.end(), [&position_id](const auto& intent) {
                  return intent.position_id == position_id &&
                         (intent.purpose == ntn_access_calendar_purpose::prach_ro ||
                          intent.purpose == ntn_access_calendar_purpose::prach_ul_beam);
                }),
                intents.end());

  const auto audit = controller.audit_access_calendar(plan.source.schedule_version, plan.cell_positions, intents);

  EXPECT_FALSE(audit.accepted);
  EXPECT_EQ(audit.reason, ntn_position_plan_reject_reason::prach_deadline_miss);
  EXPECT_GT(audit.max_prach_interval, std::chrono::milliseconds{640});
}

TEST(ntn_access_calendar_audit, advertised_prach_ro_without_ul_beam_is_rejected)
{
  ntn_onboard_position_plan_controller controller(make_config());
  const auto&                          plan = build_valid_plan(controller, 4);
  std::vector<ntn_access_calendar_intent> intents = plan.access_calendar;
  const auto removed = std::find_if(intents.begin(), intents.end(), [](const auto& intent) {
    return intent.purpose == ntn_access_calendar_purpose::prach_ul_beam;
  });
  ASSERT_NE(removed, intents.end());
  intents.erase(removed);

  const auto audit = controller.audit_access_calendar(plan.source.schedule_version, plan.cell_positions, intents);

  EXPECT_FALSE(audit.accepted);
  EXPECT_EQ(audit.reason, ntn_position_plan_reject_reason::prach_ro_without_beam);
  EXPECT_EQ(audit.prach_ro_without_beam, 1U);
}

TEST(ntn_access_calendar_audit, duplicate_port_time_resource_is_rejected)
{
  ntn_onboard_position_plan_controller controller(make_config());
  const auto&                          plan = build_valid_plan(controller, 4);
  std::vector<ntn_access_calendar_intent> intents = plan.access_calendar;
  const auto resource = std::find_if(intents.begin(), intents.end(), [](const auto& intent) {
    return intent.port_id != ntn_access_calendar_intent::no_resource_port;
  });
  ASSERT_NE(resource, intents.end());
  intents.push_back(*resource);

  const auto audit = controller.audit_access_calendar(plan.source.schedule_version, plan.cell_positions, intents);

  EXPECT_FALSE(audit.accepted);
  EXPECT_EQ(audit.reason, ntn_position_plan_reject_reason::resource_conflict);
  EXPECT_EQ(audit.resource_conflicts, 1U);
}

TEST(ntn_access_calendar_audit, sib_paging_and_rar_are_coalesced_with_existing_ssb_windows)
{
  ntn_onboard_position_plan_controller controller(make_config());
  const auto&                          plan = build_valid_plan(controller, 8);

  unsigned ssb_windows = 0;
  unsigned rar_merged_windows = 0;
  for (const ntn_access_calendar_intent& intent : plan.access_calendar) {
    if (intent.purpose == ntn_access_calendar_purpose::ssb_sib_paging ||
        intent.purpose == ntn_access_calendar_purpose::ssb_sib_paging_rar) {
      ++ssb_windows;
      rar_merged_windows += intent.purpose == ntn_access_calendar_purpose::ssb_sib_paging_rar;
    }
  }

  EXPECT_EQ(ssb_windows, 8U * 8U);
  EXPECT_EQ(rar_merged_windows, 8U);
  EXPECT_TRUE(plan.calendar_audit.accepted);
}
