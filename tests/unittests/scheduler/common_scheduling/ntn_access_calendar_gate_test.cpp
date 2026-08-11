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

#include "lib/scheduler/ntn_access_calendar_gate.h"
#include <gtest/gtest.h>

using namespace srsran;

namespace {

constexpr du_cell_index_t test_cell = to_du_cell_index(0);

ntn_access_calendar_request make_prepare_request(uint64_t version, slot_point activation_slot)
{
  ntn_access_calendar_request request;
  request.operation       = ntn_access_calendar_operation::prepare;
  request.cell_index      = test_cell;
  request.version         = version;
  request.content_hash    = "sha256:test" + std::to_string(version);
  request.activation_slot = activation_slot;
  request.validity_slots  = 20;
  request.cycle_slots     = 4;
  request.windows.push_back({0, 1, ntn_access_calendar_purpose_bit(ntn_access_calendar_purpose::ssb)});
  request.windows.push_back({1, 2, ntn_access_calendar_purpose_bit(ntn_access_calendar_purpose::prach)});
  return request;
}

ntn_access_calendar_request make_query_or_clear(const ntn_access_calendar_request& prepared,
                                                ntn_access_calendar_operation      operation)
{
  ntn_access_calendar_request request;
  request.operation    = operation;
  request.cell_index   = prepared.cell_index;
  request.version      = prepared.version;
  request.content_hash = prepared.content_hash;
  return request;
}

} // namespace

TEST(ntn_access_calendar_gate_test, when_no_snapshot_is_prepared_then_all_static_opportunities_are_allowed)
{
  scheduler_ntn_access_calendar_gate gate(test_cell, 0, 4);

  EXPECT_TRUE(gate.is_allowed(slot_point{0, 0}, ntn_access_calendar_purpose::ssb));
  EXPECT_TRUE(gate.is_allowed(slot_point{0, 0}, ntn_access_calendar_purpose::prach));

  prach_occasion_info occasion{};
  gate.configure_prach_rx_port_attribution(slot_point{0, 0}, 0, occasion);
  EXPECT_EQ(occasion.handle, 0U);
  EXPECT_FALSE(occasion.enable_rx_port_attribution);
  EXPECT_FLOAT_EQ(occasion.rx_port_attribution_unique_margin_dB, 6.0F);
  EXPECT_FALSE(occasion.calendar_position_valid);
  EXPECT_EQ(occasion.calendar_schedule_version, 0U);
  EXPECT_EQ(occasion.calendar_cycle_index, 0U);
  EXPECT_EQ(occasion.occasion_offset_us, 0U);
}

TEST(ntn_access_calendar_gate_test, when_attribution_is_disabled_then_authorized_prach_keeps_legacy_defaults)
{
  scheduler_ntn_access_calendar_gate gate(test_cell, 0, 4);
  gate.slot_indication(slot_point{0, 0});
  const auto request = make_prepare_request(1, slot_point{0, 10});
  ASSERT_EQ(gate.handle_update(request).state, ntn_access_calendar_state::ready);
  gate.slot_indication(slot_point{0, 1});

  prach_occasion_info occasion{};
  ASSERT_TRUE(gate.is_allowed(slot_point{0, 11}, ntn_access_calendar_purpose::prach));
  gate.configure_prach_rx_port_attribution(slot_point{0, 11}, 0, occasion);

  EXPECT_EQ(occasion.handle, 0U);
  EXPECT_FALSE(occasion.enable_rx_port_attribution);
  EXPECT_FLOAT_EQ(occasion.rx_port_attribution_unique_margin_dB, 6.0F);
  EXPECT_TRUE(occasion.calendar_position_valid);
  EXPECT_EQ(occasion.calendar_schedule_version, 1U);
  EXPECT_EQ(occasion.calendar_cycle_index, 0U);
  EXPECT_EQ(occasion.occasion_offset_us, 1000U);
}

TEST(ntn_access_calendar_gate_test, when_attribution_is_enabled_then_authorized_prach_gets_stable_nonzero_handle)
{
  scheduler_ntn_access_calendar_gate gate(test_cell, 0, 4);
  gate.slot_indication(slot_point{0, 0});
  auto request                                       = make_prepare_request(1, slot_point{0, 10});
  request.enable_prach_rx_port_attribution           = true;
  request.prach_rx_port_attribution_unique_margin_dB = 8.5F;
  ASSERT_EQ(gate.handle_update(request).state, ntn_access_calendar_state::ready);
  gate.slot_indication(slot_point{0, 1});

  prach_occasion_info first{};
  prach_occasion_info retry{};
  prach_occasion_info next{};
  gate.configure_prach_rx_port_attribution(slot_point{0, 11}, 0, first);
  gate.configure_prach_rx_port_attribution(slot_point{0, 11}, 0, retry);
  gate.configure_prach_rx_port_attribution(slot_point{0, 12}, 0, next);

  EXPECT_NE(first.handle, 0U);
  EXPECT_EQ(retry.handle, first.handle);
  EXPECT_NE(next.handle, first.handle);
  EXPECT_TRUE(first.enable_rx_port_attribution);
  EXPECT_FLOAT_EQ(first.rx_port_attribution_unique_margin_dB, 8.5F);
  EXPECT_TRUE(first.calendar_position_valid);
  EXPECT_EQ(first.calendar_schedule_version, 1U);
  EXPECT_EQ(first.calendar_cycle_index, 0U);
  EXPECT_EQ(first.occasion_offset_us, 1000U);

  prach_occasion_info denied{};
  ASSERT_FALSE(gate.is_allowed(slot_point{0, 10}, ntn_access_calendar_purpose::prach));
  gate.configure_prach_rx_port_attribution(slot_point{0, 10}, 0, denied);
  EXPECT_EQ(denied.handle, 0U);
  EXPECT_FALSE(denied.enable_rx_port_attribution);
  EXPECT_FALSE(denied.calendar_position_valid);
  EXPECT_EQ(denied.calendar_schedule_version, 0U);
}

TEST(ntn_access_calendar_gate_test, calendar_cycle_index_remains_monotonic_after_sfn_wrap)
{
  scheduler_ntn_access_calendar_gate gate(test_cell, 0, 4);
  gate.slot_indication(slot_point{0, 0});
  auto request                                       = make_prepare_request(1, slot_point{0, 10});
  request.validity_slots                             = 20000;
  request.enable_prach_rx_port_attribution           = true;
  request.prach_rx_port_attribution_unique_margin_dB = 8.5F;
  ASSERT_EQ(gate.handle_update(request).state, ntn_access_calendar_state::ready);

  constexpr uint32_t slots_per_sfn_cycle = 10240;
  constexpr uint32_t target_extended_slot = slots_per_sfn_cycle + 3;
  for (uint32_t count = 1; count <= target_extended_slot; ++count) {
    gate.slot_indication(slot_point{0, count % slots_per_sfn_cycle});
  }

  prach_occasion_info occasion{};
  const slot_point    target{0, target_extended_slot % slots_per_sfn_cycle};
  ASSERT_TRUE(gate.is_allowed(target, ntn_access_calendar_purpose::prach));
  gate.configure_prach_rx_port_attribution(target, 0, occasion);

  // 10,243 - 10 = 10,233 slots since activation: 2,558 complete four-slot cycles plus one slot.
  EXPECT_TRUE(occasion.calendar_position_valid);
  EXPECT_EQ(occasion.calendar_schedule_version, 1U);
  EXPECT_EQ(occasion.calendar_cycle_index, 2558U);
  EXPECT_EQ(occasion.occasion_offset_us, 1000U);
}

TEST(ntn_access_calendar_gate_test, when_checked_calendar_is_empty_then_it_is_an_explicit_deny_all_snapshot)
{
  scheduler_ntn_access_calendar_gate gate(test_cell, 0, 4);
  gate.slot_indication(slot_point{0, 0});
  auto request = make_prepare_request(1, slot_point{0, 10});
  request.windows.clear();

  ASSERT_EQ(gate.handle_update(request).state, ntn_access_calendar_state::ready);
  gate.slot_indication(slot_point{0, 10});

  EXPECT_FALSE(gate.is_allowed(slot_point{0, 10}, ntn_access_calendar_purpose::ssb));
  EXPECT_FALSE(gate.is_allowed(slot_point{0, 10}, ntn_access_calendar_purpose::prach));
  EXPECT_EQ(gate.handle_update(make_query_or_clear(request, ntn_access_calendar_operation::query)).state,
            ntn_access_calendar_state::applied);
}

TEST(ntn_access_calendar_gate_test, when_plan_is_prepared_then_future_target_slots_use_pending_mask_before_activation)
{
  scheduler_ntn_access_calendar_gate gate(test_cell, 0, 4);
  gate.slot_indication(slot_point{0, 0});
  const auto request = make_prepare_request(1, slot_point{0, 10});

  const auto prepare_response = gate.handle_update(request);
  EXPECT_EQ(prepare_response.state, ntn_access_calendar_state::ready);
  EXPECT_FALSE(prepare_response.command_consumed);
  gate.slot_indication(slot_point{0, 1});

  // No plan applies before activation.
  EXPECT_TRUE(gate.is_allowed(slot_point{0, 9}, ntn_access_calendar_purpose::ssb));
  EXPECT_TRUE(gate.is_allowed(slot_point{0, 9}, ntn_access_calendar_purpose::prach));

  // Future target slots at and after activation already use the pending plan, which is required by lookahead
  // schedulers.
  EXPECT_TRUE(gate.is_allowed(slot_point{0, 10}, ntn_access_calendar_purpose::ssb));
  EXPECT_FALSE(gate.is_allowed(slot_point{0, 10}, ntn_access_calendar_purpose::prach));
  EXPECT_FALSE(gate.is_allowed(slot_point{0, 11}, ntn_access_calendar_purpose::ssb));
  EXPECT_TRUE(gate.is_allowed(slot_point{0, 11}, ntn_access_calendar_purpose::prach));
  EXPECT_TRUE(gate.is_allowed(slot_point{0, 12}, ntn_access_calendar_purpose::prach));
  EXPECT_FALSE(gate.is_allowed(slot_point{0, 13}, ntn_access_calendar_purpose::prach));

  auto query = make_query_or_clear(request, ntn_access_calendar_operation::query);
  const auto armed_response = gate.handle_update(query);
  EXPECT_EQ(armed_response.state, ntn_access_calendar_state::ready);
  EXPECT_TRUE(armed_response.command_consumed);

  gate.slot_indication(slot_point{0, 10});
  EXPECT_EQ(gate.handle_update(query).state, ntn_access_calendar_state::applied);

  // The active plan fails closed after its validity interval.
  gate.slot_indication(slot_point{0, 30});
  EXPECT_FALSE(gate.is_allowed(slot_point{0, 30}, ntn_access_calendar_purpose::ssb));
  EXPECT_FALSE(gate.is_allowed(slot_point{0, 30}, ntn_access_calendar_purpose::prach));
  EXPECT_EQ(gate.handle_update(query).reason, ntn_access_calendar_reject_reason::expired);
}

TEST(ntn_access_calendar_gate_test, when_identical_prepare_is_retried_then_current_state_is_returned_idempotently)
{
  scheduler_ntn_access_calendar_gate gate(test_cell, 0, 4);
  gate.slot_indication(slot_point{0, 0});
  const auto request = make_prepare_request(1, slot_point{0, 10});

  EXPECT_EQ(gate.handle_update(request).state, ntn_access_calendar_state::ready);
  EXPECT_EQ(gate.handle_update(request).state, ntn_access_calendar_state::ready);

  gate.slot_indication(slot_point{0, 10});
  EXPECT_EQ(gate.handle_update(request).state, ntn_access_calendar_state::applied);

  auto changed_hash         = request;
  changed_hash.content_hash = "sha256:different";
  const auto response       = gate.handle_update(changed_hash);
  EXPECT_EQ(response.state, ntn_access_calendar_state::rejected);
  EXPECT_EQ(response.reason, ntn_access_calendar_reject_reason::version_hash_mismatch);
}

TEST(ntn_access_calendar_gate_test, when_prepare_is_invalid_then_active_plan_is_unchanged)
{
  scheduler_ntn_access_calendar_gate gate(test_cell, 0, 4);
  gate.slot_indication(slot_point{0, 0});
  const auto active_request = make_prepare_request(1, slot_point{0, 10});
  ASSERT_EQ(gate.handle_update(active_request).state, ntn_access_calendar_state::ready);
  gate.slot_indication(slot_point{0, 10});

  auto invalid_request         = make_prepare_request(2, slot_point{0, 20});
  invalid_request.content_hash = {};
  const auto response          = gate.handle_update(invalid_request);
  EXPECT_EQ(response.state, ntn_access_calendar_state::rejected);
  EXPECT_EQ(response.reason, ntn_access_calendar_reject_reason::invalid_hash);

  // Version 1 remains active and continues to enforce its cycle.
  EXPECT_TRUE(gate.is_allowed(slot_point{0, 14}, ntn_access_calendar_purpose::ssb));
  EXPECT_FALSE(gate.is_allowed(slot_point{0, 14}, ntn_access_calendar_purpose::prach));
}

TEST(ntn_access_calendar_gate_test, when_activation_does_not_cover_lookahead_then_prepare_is_rejected)
{
  scheduler_ntn_access_calendar_gate gate(test_cell, 0, 4);
  gate.slot_indication(slot_point{0, 100});

  auto too_late = make_prepare_request(1, slot_point{0, 103});
  EXPECT_EQ(gate.handle_update(too_late).reason, ntn_access_calendar_reject_reason::activation_too_late);

  auto on_time = make_prepare_request(1, slot_point{0, 104});
  EXPECT_EQ(gate.handle_update(on_time).state, ntn_access_calendar_state::ready);
}

TEST(ntn_access_calendar_gate_test, when_matching_plan_is_cleared_then_default_allow_path_is_restored)
{
  scheduler_ntn_access_calendar_gate gate(test_cell, 0, 4);
  gate.slot_indication(slot_point{0, 0});
  const auto request = make_prepare_request(1, slot_point{0, 10});
  ASSERT_EQ(gate.handle_update(request).state, ntn_access_calendar_state::ready);
  gate.slot_indication(slot_point{0, 0});
  ASSERT_FALSE(gate.is_allowed(slot_point{0, 10}, ntn_access_calendar_purpose::prach));

  auto clear = make_query_or_clear(request, ntn_access_calendar_operation::clear);
  EXPECT_EQ(gate.handle_update(clear).state, ntn_access_calendar_state::ready);
  gate.slot_indication(slot_point{0, 1});

  auto query = make_query_or_clear(request, ntn_access_calendar_operation::query);
  EXPECT_EQ(gate.handle_update(query).state, ntn_access_calendar_state::cleared);
  EXPECT_TRUE(gate.is_allowed(slot_point{0, 12}, ntn_access_calendar_purpose::ssb));
  EXPECT_TRUE(gate.is_allowed(slot_point{0, 12}, ntn_access_calendar_purpose::prach));

  // A two-cell rollback may clear one side before retrying the same management-center plan.
  EXPECT_EQ(gate.handle_update(request).state, ntn_access_calendar_state::ready);
  gate.slot_indication(slot_point{0, 2});
  EXPECT_EQ(gate.handle_update(query).state, ntn_access_calendar_state::ready);
  gate.slot_indication(slot_point{0, 10});
  EXPECT_EQ(gate.handle_update(query).state, ntn_access_calendar_state::applied);
}

TEST(ntn_access_calendar_gate_test, when_new_active_plan_is_cleared_then_previous_unexpired_plan_is_restored)
{
  scheduler_ntn_access_calendar_gate gate(test_cell, 0, 4);
  gate.slot_indication(slot_point{0, 0});

  const auto version1 = make_prepare_request(1, slot_point{0, 10});
  ASSERT_EQ(gate.handle_update(version1).state, ntn_access_calendar_state::ready);
  gate.slot_indication(slot_point{0, 10});

  auto version2 = make_prepare_request(2, slot_point{0, 20});
  version2.windows.clear();
  version2.windows.push_back({0, 4, ntn_access_calendar_purpose_bit(ntn_access_calendar_purpose::prach)});
  ASSERT_EQ(gate.handle_update(version2).state, ntn_access_calendar_state::ready);
  gate.slot_indication(slot_point{0, 20});
  ASSERT_FALSE(gate.is_allowed(slot_point{0, 20}, ntn_access_calendar_purpose::ssb));
  ASSERT_TRUE(gate.is_allowed(slot_point{0, 20}, ntn_access_calendar_purpose::prach));

  auto clear_version2 = make_query_or_clear(version2, ntn_access_calendar_operation::clear);
  ASSERT_EQ(gate.handle_update(clear_version2).state, ntn_access_calendar_state::ready);
  gate.slot_indication(slot_point{0, 21});

  auto query_version2 = make_query_or_clear(version2, ntn_access_calendar_operation::query);
  EXPECT_EQ(gate.handle_update(query_version2).state, ntn_access_calendar_state::cleared);

  // A reconnect query names the CU's recovered plan, not the last cleared successor.
  auto query_version1   = make_query_or_clear(version1, ntn_access_calendar_operation::query);
  auto wrong_hash_query = query_version1;
  wrong_hash_query.content_hash = "sha256:not-version-1";
  EXPECT_EQ(gate.handle_update(wrong_hash_query).reason,
            ntn_access_calendar_reject_reason::version_hash_mismatch);
  const auto version1_result = gate.handle_update(query_version1);
  EXPECT_EQ(version1_result.state, ntn_access_calendar_state::applied);
  EXPECT_EQ(version1_result.reason, ntn_access_calendar_reject_reason::none);
  EXPECT_TRUE(version1_result.command_consumed);
  EXPECT_EQ(version1_result.version, version1.version);
  EXPECT_EQ(version1_result.content_hash, version1.content_hash);
  EXPECT_EQ(version1_result.effective_activation_slot, version1.activation_slot);

  // Slot 22 is offset 0 in the version-1 cycle, proving that version 1 is active again rather than default-allow.
  EXPECT_TRUE(gate.is_allowed(slot_point{0, 22}, ntn_access_calendar_purpose::ssb));
  EXPECT_FALSE(gate.is_allowed(slot_point{0, 22}, ntn_access_calendar_purpose::prach));
}

TEST(ntn_access_calendar_gate_test, when_non_latest_active_plan_expires_then_exact_query_reports_expired)
{
  scheduler_ntn_access_calendar_gate gate(test_cell, 0, 4);
  gate.slot_indication(slot_point{0, 0});

  auto version1           = make_prepare_request(1, slot_point{0, 10});
  version1.validity_slots = 12;
  ASSERT_EQ(gate.handle_update(version1).state, ntn_access_calendar_state::ready);
  gate.slot_indication(slot_point{0, 10});

  const auto version2 = make_prepare_request(2, slot_point{0, 30});
  ASSERT_EQ(gate.handle_update(version2).state, ntn_access_calendar_state::ready);
  gate.slot_indication(slot_point{0, 23});

  const auto version1_result =
      gate.handle_update(make_query_or_clear(version1, ntn_access_calendar_operation::query));
  EXPECT_EQ(version1_result.state, ntn_access_calendar_state::rejected);
  EXPECT_EQ(version1_result.reason, ntn_access_calendar_reject_reason::expired);
  EXPECT_TRUE(version1_result.command_consumed);
  EXPECT_EQ(version1_result.version, version1.version);
  EXPECT_EQ(version1_result.content_hash, version1.content_hash);
  EXPECT_EQ(version1_result.effective_activation_slot, version1.activation_slot);
}

TEST(ntn_access_calendar_gate_test, when_hour_long_validity_is_prepared_then_extended_interval_does_not_sfn_wrap)
{
  scheduler_ntn_access_calendar_gate gate(test_cell, 0, 4);
  gate.slot_indication(slot_point{0, 100});

  auto request           = make_prepare_request(1, slot_point{0, 110});
  request.validity_slots = 3600000U;
  ASSERT_EQ(gate.handle_update(request).state, ntn_access_calendar_state::ready);

  gate.slot_indication(slot_point{0, 110});
  EXPECT_EQ(gate.handle_update(make_query_or_clear(request, ntn_access_calendar_operation::query)).state,
            ntn_access_calendar_state::applied);

  // Drive the slot thread through one complete plain-SFN wrap; the extended validity must remain active.
  for (unsigned count = 111; count != 10240; ++count) {
    gate.slot_indication(slot_point{0, count});
  }
  for (unsigned count = 0; count != 101; ++count) {
    gate.slot_indication(slot_point{0, count});
  }
  EXPECT_EQ(gate.handle_update(make_query_or_clear(request, ntn_access_calendar_operation::query)).state,
            ntn_access_calendar_state::applied);

  auto beyond_horizon           = make_prepare_request(2, slot_point{0, 120});
  beyond_horizon.validity_slots = 5242880U;
  EXPECT_EQ(gate.handle_update(beyond_horizon).reason, ntn_access_calendar_reject_reason::invalid_validity);
}

TEST(ntn_access_calendar_gate_test, when_non_latest_plan_clear_is_retried_then_one_command_converges_to_cleared)
{
  scheduler_ntn_access_calendar_gate gate(test_cell, 0, 4);
  gate.slot_indication(slot_point{0, 0});

  const auto version1 = make_prepare_request(1, slot_point{0, 10});
  ASSERT_EQ(gate.handle_update(version1).state, ntn_access_calendar_state::ready);
  gate.slot_indication(slot_point{0, 10});

  const auto version2 = make_prepare_request(2, slot_point{0, 20});
  ASSERT_EQ(gate.handle_update(version2).state, ntn_access_calendar_state::ready);

  auto clear_version1 = make_query_or_clear(version1, ntn_access_calendar_operation::clear);
  auto wrong_hash_clear = clear_version1;
  wrong_hash_clear.content_hash = "sha256:not-version-1";
  EXPECT_EQ(gate.handle_update(wrong_hash_clear).reason,
            ntn_access_calendar_reject_reason::version_hash_mismatch);
  EXPECT_EQ(gate.handle_update(clear_version1).state, ntn_access_calendar_state::ready);
  EXPECT_EQ(gate.handle_update(clear_version1).state, ntn_access_calendar_state::ready);

  gate.slot_indication(slot_point{0, 11});
  EXPECT_EQ(gate.handle_update(clear_version1).state, ntn_access_calendar_state::cleared);

  // Clearing the old target does not discard the newer pending plan.
  gate.slot_indication(slot_point{0, 20});
  EXPECT_EQ(gate.handle_update(make_query_or_clear(version2, ntn_access_calendar_operation::query)).state,
            ntn_access_calendar_state::applied);
}
