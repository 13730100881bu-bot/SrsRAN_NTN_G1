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

/// \file
/// \brief In this file, we verify the correct operation of the MAC scheduler, as a whole, for basic operations, in
/// setups without any UEs. The objective is to cover and verify the integration of the scheduler building blocks.
/// The currently covered operations include:
/// - run_slot without any event
/// - handle RACH indication + RAR allocation.

#include "lib/scheduler/scheduler_impl.h"
#include "tests/test_doubles/scheduler/scheduler_config_helper.h"
#include "tests/unittests/scheduler/test_utils/dummy_test_components.h"
#include "tests/unittests/scheduler/test_utils/indication_generators.h"
#include "tests/unittests/scheduler/test_utils/scheduler_test_suite.h"
#include "srsran/scheduler/config/scheduler_expert_config_factory.h"
#include "srsran/support/test_utils.h"
#include <gtest/gtest.h>
#include <optional>

using namespace srsran;

namespace sched_no_ue_test {

class sched_no_ue_tester : public ::testing::Test
{
protected:
  void SetUp() override
  {
    srslog::fetch_basic_logger("SCHED", true).set_level(srslog::basic_levels::info);
    srslog::init();
  }
};

} // namespace sched_no_ue_test

using namespace sched_no_ue_test;

TEST_F(sched_no_ue_tester, test_no_ues)
{
  scheduler_expert_config             sched_cfg = config_helpers::make_default_scheduler_expert_config();
  sched_cfg_dummy_notifier            cfg_notif;
  scheduler_ue_metrics_dummy_notifier metrics_notif;
  scheduler_impl                      sch{scheduler_config{sched_cfg, cfg_notif}};

  // Action 1: Add Cell.
  sched_cell_configuration_request_message cell_cfg_msg =
      sched_config_helper::make_default_sched_cell_configuration_request();
  cell_configuration cell_cfg{sched_cfg, cell_cfg_msg};
  sch.handle_cell_configuration_request(cell_cfg_msg);

  slot_point sl_tx{0, test_rgen::uniform_int<unsigned>(0, 10239)};

  // Action 2: Run slot.
  const sched_result& res = sch.slot_indication(sl_tx, to_du_cell_index(0));
  ASSERT_TRUE(res.success);
  test_scheduler_result_consistency(cell_cfg, sl_tx, res);
  ASSERT_TRUE(res.dl.ue_grants.empty());
  ASSERT_TRUE(res.ul.puschs.empty());
}

TEST_F(sched_no_ue_tester, test_rach_indication)
{
  scheduler_expert_config             sched_cfg = config_helpers::make_default_scheduler_expert_config();
  sched_cfg_dummy_notifier            cfg_notif;
  scheduler_ue_metrics_dummy_notifier metrics_notif;
  scheduler_impl                      sch{scheduler_config{sched_cfg, cfg_notif}};

  // Action 1: Add Cell.
  sched_cell_configuration_request_message cell_cfg_msg =
      sched_config_helper::make_default_sched_cell_configuration_request();
  cell_configuration cell_cfg{sched_cfg, cell_cfg_msg};
  sch.handle_cell_configuration_request(sched_config_helper::make_default_sched_cell_configuration_request());

  // Action 2: Add RACH indication.
  // Note: RACH is added in a slot different than the SIB1 to avoid PDCCH conflicts.
  const unsigned tx_delay = 2;
  slot_point     sl_rx{0, 1}, sl_tx = sl_rx + tx_delay;
  sch.handle_rach_indication(
      test_helper::create_rach_indication(sl_rx, {test_helper::create_preamble(0, to_rnti(0x4601))}));

  // Action 3: Run slot 0.
  const sched_result& res = sch.slot_indication(sl_tx, to_du_cell_index(0));

  // TEST: Result exists. No Data allocated. A RAR has been allocated.
  ASSERT_TRUE(res.success);
  test_scheduler_result_consistency(cell_cfg, sl_tx, res);
  ASSERT_TRUE(res.dl.ue_grants.empty());
  ASSERT_TRUE(not res.dl.rar_grants.empty());
}

TEST_F(sched_no_ue_tester, ntn_calendar_preflight_reports_matched_partial_and_missing_without_creating_a_gate)
{
  scheduler_expert_config  sched_cfg = config_helpers::make_default_scheduler_expert_config();
  sched_cfg_dummy_notifier cfg_notif;
  scheduler_impl           sch{scheduler_config{sched_cfg, cfg_notif}};

  sched_cell_configuration_request_message cell_cfg_msg =
      sched_config_helper::make_default_sched_cell_configuration_request();
  ASSERT_TRUE(sch.handle_cell_configuration_request(cell_cfg_msg));

  static constexpr uint32_t cycle_slots = 640;
  std::optional<uint32_t>   ssb_slot;
  std::optional<uint32_t>   prach_slot;
  std::optional<uint32_t>   no_ssb_slot;
  std::optional<uint32_t>   no_prach_slot;
  for (uint32_t offset = 0; offset != cycle_slots; ++offset) {
    const sched_result& result = sch.slot_indication(slot_point{0, offset}, cell_cfg_msg.cell_index);
    ASSERT_TRUE(result.success);
    if (!result.dl.bc.ssb_info.empty() && !ssb_slot.has_value()) {
      ssb_slot = offset;
    }
    if (!result.ul.prachs.empty() && !prach_slot.has_value()) {
      prach_slot = offset;
    }
    if (result.dl.bc.ssb_info.empty() && !no_ssb_slot.has_value()) {
      no_ssb_slot = offset;
    }
    if (result.ul.prachs.empty() && !no_prach_slot.has_value()) {
      no_prach_slot = offset;
    }
  }
  ASSERT_TRUE(ssb_slot.has_value());
  ASSERT_TRUE(prach_slot.has_value());
  ASSERT_TRUE(no_ssb_slot.has_value());
  ASSERT_TRUE(no_prach_slot.has_value());

  ntn_access_calendar_request request;
  request.operation       = ntn_access_calendar_operation::preflight;
  request.cell_index      = cell_cfg_msg.cell_index;
  request.version         = 1;
  request.content_hash    = "sha256:preflight";
  request.activation_slot = slot_point{0, 0};
  request.validity_slots  = cycle_slots;
  request.cycle_slots     = cycle_slots;
  request.expectations.push_back({"G000001", ssb_slot.value(), 1, ntn_access_calendar_purpose::ssb});
  request.expectations.push_back({"G000001", prach_slot.value(), 1, ntn_access_calendar_purpose::prach});

  ntn_access_calendar_response response = sch.handle_ntn_access_calendar_update(request);
  EXPECT_EQ(response.state, ntn_access_calendar_state::ready);
  EXPECT_EQ(response.reason, ntn_access_calendar_reject_reason::none);
  EXPECT_TRUE(response.preflight.performed);
  EXPECT_TRUE(response.preflight.passed);
  EXPECT_EQ(response.preflight.numerology, 0U);
  EXPECT_EQ(response.preflight.expected_ssb, 1U);
  EXPECT_EQ(response.preflight.matched_ssb, 1U);
  EXPECT_EQ(response.preflight.expected_prach, 1U);
  EXPECT_EQ(response.preflight.matched_prach, 1U);
  EXPECT_EQ(response.preflight.max_ssb_gap_slots, cycle_slots);
  EXPECT_EQ(response.preflight.max_prach_gap_slots, cycle_slots);
  EXPECT_FALSE(response.preflight.first_unmatched_present);

  request.expectations.push_back({"G000002", no_ssb_slot.value(), 1, ntn_access_calendar_purpose::ssb});
  response = sch.handle_ntn_access_calendar_update(request);
  EXPECT_EQ(response.state, ntn_access_calendar_state::rejected);
  EXPECT_EQ(response.reason, ntn_access_calendar_reject_reason::static_opportunity_missing);
  EXPECT_TRUE(response.preflight.performed);
  EXPECT_FALSE(response.preflight.passed);
  EXPECT_EQ(response.preflight.expected_ssb, 2U);
  EXPECT_EQ(response.preflight.matched_ssb, 1U);
  EXPECT_TRUE(response.preflight.first_unmatched_present);
  EXPECT_EQ(response.preflight.first_unmatched_position_id, "G000002");
  EXPECT_EQ(response.preflight.first_unmatched_purpose, ntn_access_calendar_purpose::ssb);
  EXPECT_EQ(response.preflight.first_unmatched_start_slot_offset, no_ssb_slot.value());
  EXPECT_EQ(response.preflight.first_unmatched_nof_slots, 1U);

  request.expectations.clear();
  request.expectations.push_back({"G000003", no_ssb_slot.value(), 1, ntn_access_calendar_purpose::ssb});
  request.expectations.push_back({"G000003", no_prach_slot.value(), 1, ntn_access_calendar_purpose::prach});
  response = sch.handle_ntn_access_calendar_update(request);
  EXPECT_EQ(response.state, ntn_access_calendar_state::rejected);
  EXPECT_EQ(response.reason, ntn_access_calendar_reject_reason::static_opportunity_missing);
  EXPECT_EQ(response.preflight.matched_ssb, 0U);
  EXPECT_EQ(response.preflight.matched_prach, 0U);
  EXPECT_EQ(response.preflight.max_ssb_gap_slots, cycle_slots + 1U);
  EXPECT_EQ(response.preflight.max_prach_gap_slots, cycle_slots + 1U);

  ntn_access_calendar_request query;
  query.operation    = ntn_access_calendar_operation::query;
  query.cell_index   = cell_cfg_msg.cell_index;
  query.version      = request.version;
  query.content_hash = request.content_hash;
  EXPECT_EQ(sch.handle_ntn_access_calendar_update(query).state, ntn_access_calendar_state::cleared);
}

TEST_F(sched_no_ue_tester, when_calendar_allows_only_prach_then_ssb_is_gated_without_gating_prach)
{
  scheduler_expert_config  sched_cfg = config_helpers::make_default_scheduler_expert_config();
  sched_cfg_dummy_notifier cfg_notif;
  scheduler_impl           sch{scheduler_config{sched_cfg, cfg_notif}};

  sched_cell_configuration_request_message cell_cfg_msg =
      sched_config_helper::make_default_sched_cell_configuration_request();
  ASSERT_TRUE(sch.handle_cell_configuration_request(cell_cfg_msg));
  ASSERT_TRUE(sch.slot_indication(slot_point{0, 0}, cell_cfg_msg.cell_index).success);

  ntn_access_calendar_request request;
  request.operation       = ntn_access_calendar_operation::prepare;
  request.cell_index      = cell_cfg_msg.cell_index;
  request.version         = 1;
  request.content_hash    = "sha256:prach-only";
  request.activation_slot = slot_point{0, 32};
  request.validity_slots  = 1000;
  request.cycle_slots     = 1;
  request.windows.push_back({0, 1, ntn_access_calendar_purpose_bit(ntn_access_calendar_purpose::prach)});
  ASSERT_EQ(sch.handle_ntn_access_calendar_update(request).state, ntn_access_calendar_state::ready);

  unsigned nof_ssbs   = 0;
  unsigned nof_prachs = 0;
  for (unsigned count = 1; count != 200; ++count) {
    const sched_result& result = sch.slot_indication(slot_point{0, count}, cell_cfg_msg.cell_index);
    ASSERT_TRUE(result.success);
    if (count >= request.activation_slot.to_uint()) {
      nof_ssbs += result.dl.bc.ssb_info.size();
      nof_prachs += result.ul.prachs.size();
    }
  }

  EXPECT_EQ(nof_ssbs, 0U);
  EXPECT_GT(nof_prachs, 0U);

  ntn_access_calendar_request query;
  query.operation    = ntn_access_calendar_operation::query;
  query.cell_index   = request.cell_index;
  query.version      = request.version;
  query.content_hash = request.content_hash;
  EXPECT_EQ(sch.handle_ntn_access_calendar_update(query).state, ntn_access_calendar_state::applied);
}

TEST_F(sched_no_ue_tester, when_calendar_allows_only_ssb_then_prach_is_gated_without_gating_ssb)
{
  scheduler_expert_config  sched_cfg = config_helpers::make_default_scheduler_expert_config();
  sched_cfg_dummy_notifier cfg_notif;
  scheduler_impl           sch{scheduler_config{sched_cfg, cfg_notif}};

  sched_cell_configuration_request_message cell_cfg_msg =
      sched_config_helper::make_default_sched_cell_configuration_request();
  ASSERT_TRUE(sch.handle_cell_configuration_request(cell_cfg_msg));
  ASSERT_TRUE(sch.slot_indication(slot_point{0, 0}, cell_cfg_msg.cell_index).success);

  ntn_access_calendar_request request;
  request.operation       = ntn_access_calendar_operation::prepare;
  request.cell_index      = cell_cfg_msg.cell_index;
  request.version         = 1;
  request.content_hash    = "sha256:ssb-only";
  request.activation_slot = slot_point{0, 32};
  request.validity_slots  = 1000;
  request.cycle_slots     = 1;
  request.windows.push_back({0, 1, ntn_access_calendar_purpose_bit(ntn_access_calendar_purpose::ssb)});
  ASSERT_EQ(sch.handle_ntn_access_calendar_update(request).state, ntn_access_calendar_state::ready);

  unsigned nof_ssbs   = 0;
  unsigned nof_prachs = 0;
  for (unsigned count = 1; count != 200; ++count) {
    const sched_result& result = sch.slot_indication(slot_point{0, count}, cell_cfg_msg.cell_index);
    ASSERT_TRUE(result.success);
    if (count >= request.activation_slot.to_uint()) {
      nof_ssbs += result.dl.bc.ssb_info.size();
      nof_prachs += result.ul.prachs.size();
    }
  }

  EXPECT_GT(nof_ssbs, 0U);
  EXPECT_EQ(nof_prachs, 0U);
}

TEST_F(sched_no_ue_tester, when_active_update_is_cleared_then_current_results_restore_previous_plan)
{
  scheduler_expert_config  sched_cfg = config_helpers::make_default_scheduler_expert_config();
  sched_cfg_dummy_notifier cfg_notif;
  scheduler_impl           sch{scheduler_config{sched_cfg, cfg_notif}};

  sched_cell_configuration_request_message cell_cfg_msg =
      sched_config_helper::make_default_sched_cell_configuration_request();
  ASSERT_TRUE(sch.handle_cell_configuration_request(cell_cfg_msg));
  ASSERT_TRUE(sch.slot_indication(slot_point{0, 0}, cell_cfg_msg.cell_index).success);

  ntn_access_calendar_request version1;
  version1.operation       = ntn_access_calendar_operation::prepare;
  version1.cell_index      = cell_cfg_msg.cell_index;
  version1.version         = 1;
  version1.content_hash    = "sha256:ssb-v1";
  version1.activation_slot = slot_point{0, 32};
  version1.validity_slots  = 1000;
  version1.cycle_slots     = 1;
  version1.windows.push_back({0, 1, ntn_access_calendar_purpose_bit(ntn_access_calendar_purpose::ssb)});
  ASSERT_EQ(sch.handle_ntn_access_calendar_update(version1).state, ntn_access_calendar_state::ready);

  for (unsigned count = 1; count <= 40; ++count) {
    ASSERT_TRUE(sch.slot_indication(slot_point{0, count}, cell_cfg_msg.cell_index).success);
  }

  ntn_access_calendar_request version2 = version1;
  version2.version                     = 2;
  version2.content_hash                = "sha256:prach-v2";
  version2.activation_slot             = slot_point{0, 80};
  version2.windows.clear();
  version2.windows.push_back({0, 1, ntn_access_calendar_purpose_bit(ntn_access_calendar_purpose::prach)});
  ASSERT_EQ(sch.handle_ntn_access_calendar_update(version2).state, ntn_access_calendar_state::ready);

  for (unsigned count = 41; count <= 100; ++count) {
    ASSERT_TRUE(sch.slot_indication(slot_point{0, count}, cell_cfg_msg.cell_index).success);
  }

  ntn_access_calendar_request clear_version2;
  clear_version2.operation    = ntn_access_calendar_operation::clear;
  clear_version2.cell_index   = version2.cell_index;
  clear_version2.version      = version2.version;
  clear_version2.content_hash = version2.content_hash;
  ASSERT_EQ(sch.handle_ntn_access_calendar_update(clear_version2).state, ntn_access_calendar_state::ready);

  unsigned nof_ssbs_after_restore   = 0;
  unsigned nof_prachs_after_restore = 0;
  for (unsigned count = 101; count <= 160; ++count) {
    const sched_result& result = sch.slot_indication(slot_point{0, count}, cell_cfg_msg.cell_index);
    ASSERT_TRUE(result.success);
    nof_ssbs_after_restore += result.dl.bc.ssb_info.size();
    nof_prachs_after_restore += result.ul.prachs.size();
  }

  EXPECT_GT(nof_ssbs_after_restore, 0U);
  EXPECT_EQ(nof_prachs_after_restore, 0U);
  EXPECT_EQ(sch.handle_ntn_access_calendar_update(clear_version2).state, ntn_access_calendar_state::cleared);
}
