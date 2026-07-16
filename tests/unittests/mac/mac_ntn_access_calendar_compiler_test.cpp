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

#include "lib/mac/mac_ntn_access_calendar_compiler.h"
#include <gtest/gtest.h>
#include <array>
#include <chrono>
#include <limits>
#include <utility>

using namespace srsran;
using namespace std::chrono_literals;

namespace {

struct mu4_compile_context {
  mac_ntn_access_calendar_update request;
  mac_cell_slot_time_info        last_mapping;
  slot_point                     activation_slot;
};

mu4_compile_context make_mu4_compile_context()
{
  mu4_compile_context context;
  context.last_mapping.sl_tx      = slot_point{4, 100};
  context.last_mapping.time_point = std::chrono::system_clock::time_point{100s};
  context.activation_slot         = slot_point{4, 64100};

  context.request.operation        = mac_ntn_access_calendar_operation::prepare;
  context.request.schedule_version = 42;
  context.request.calendar_hash    = "sha256:calendar";
  context.request.activation_epoch = context.last_mapping.time_point + 4s;
  context.request.valid_until      = context.request.activation_epoch + 4s;
  context.request.cycle_duration   = 640ms;
  context.request.cells[0].cell_index = to_du_cell_index(0);
  context.request.cells[1].cell_index = to_du_cell_index(1);
  return context;
}

mac_ntn_access_calendar_intent make_intent(mac_ntn_access_calendar_direction direction,
                                           mac_ntn_access_calendar_purpose   purpose,
                                           std::chrono::microseconds         start_time = 2ms,
                                           std::chrono::microseconds         duration   = 5ms)
{
  mac_ntn_access_calendar_intent intent;
  intent.position_id = "G000001";
  intent.start_time  = start_time;
  intent.duration    = duration;
  intent.direction   = direction;
  intent.purpose     = purpose;
  intent.port_id     = 0;
  return intent;
}

mac_ntn_access_calendar_compile_result compile_first_cell(const mu4_compile_context& context)
{
  return compile_mac_ntn_access_calendar_cell(
      context.request, context.request.cells[0], context.last_mapping, context.activation_slot);
}

TEST(mac_ntn_access_calendar_compiler_test,
     when_mu4_calendar_is_slot_aligned_then_wall_clock_periods_and_merged_purposes_are_compiled_exactly)
{
  mu4_compile_context context = make_mu4_compile_context();
  auto&               intents = context.request.cells[0].intents;
  intents.push_back(make_intent(mac_ntn_access_calendar_direction::downlink,
                                mac_ntn_access_calendar_purpose::ssb_sib_paging));
  intents.push_back(make_intent(mac_ntn_access_calendar_direction::downlink,
                                mac_ntn_access_calendar_purpose::ssb_sib_paging_rar));
  intents.push_back(
      make_intent(mac_ntn_access_calendar_direction::uplink, mac_ntn_access_calendar_purpose::prach_ro));
  intents.push_back(
      make_intent(mac_ntn_access_calendar_direction::uplink, mac_ntn_access_calendar_purpose::prach_ul_beam));
  intents[0].position_id = "G000001";
  intents[1].position_id = "G000002";
  intents[2].position_id = "G000003";
  intents[3].position_id = "G000003";

  const mac_ntn_access_calendar_compile_result result = compile_first_cell(context);

  ASSERT_TRUE(result.successful());
  EXPECT_TRUE(result.reject_reason.empty());
  EXPECT_EQ(result.accepted_intents, 4);
  const ntn_access_calendar_request& scheduler_request = result.scheduler_request.value();
  EXPECT_EQ(scheduler_request.operation, ntn_access_calendar_operation::prepare);
  EXPECT_EQ(scheduler_request.cell_index, to_du_cell_index(0));
  EXPECT_EQ(scheduler_request.version, 42);
  EXPECT_EQ(scheduler_request.content_hash, "sha256:calendar");
  EXPECT_EQ(scheduler_request.activation_slot, context.activation_slot);
  EXPECT_EQ(scheduler_request.activation_slot - context.last_mapping.sl_tx, 64000);
  EXPECT_EQ(scheduler_request.validity_slots, 64000);
  EXPECT_EQ(scheduler_request.cycle_slots, 10240);
  ASSERT_EQ(scheduler_request.windows.size(), 1);
  EXPECT_EQ(scheduler_request.windows[0].start_slot_offset, 32);
  EXPECT_EQ(scheduler_request.windows[0].nof_slots, 80);
  EXPECT_EQ(scheduler_request.windows[0].purpose_mask, NTN_ACCESS_CALENDAR_ALL_PURPOSES);

  ASSERT_EQ(scheduler_request.expectations.size(), 3U);
  EXPECT_EQ(scheduler_request.expectations[0].position_id, "G000001");
  EXPECT_EQ(scheduler_request.expectations[0].purpose, ntn_access_calendar_purpose::ssb);
  EXPECT_EQ(scheduler_request.expectations[1].position_id, "G000002");
  EXPECT_EQ(scheduler_request.expectations[1].purpose, ntn_access_calendar_purpose::ssb);
  EXPECT_EQ(scheduler_request.expectations[2].position_id, "G000003");
  EXPECT_EQ(scheduler_request.expectations[2].purpose, ntn_access_calendar_purpose::prach);
  for (const ntn_access_calendar_expectation& expectation : scheduler_request.expectations) {
    EXPECT_EQ(expectation.start_slot_offset, 32U);
    EXPECT_EQ(expectation.nof_slots, 80U);
  }
}

TEST(mac_ntn_access_calendar_compiler_test, when_direction_does_not_match_purpose_then_compile_is_rejected)
{
  const std::array<std::pair<mac_ntn_access_calendar_direction, mac_ntn_access_calendar_purpose>, 4> invalid_pairs = {
      {{mac_ntn_access_calendar_direction::uplink, mac_ntn_access_calendar_purpose::ssb_sib_paging},
       {mac_ntn_access_calendar_direction::uplink, mac_ntn_access_calendar_purpose::ssb_sib_paging_rar},
       {mac_ntn_access_calendar_direction::downlink, mac_ntn_access_calendar_purpose::prach_ro},
       {mac_ntn_access_calendar_direction::downlink, mac_ntn_access_calendar_purpose::prach_ul_beam}}};

  for (const auto& [direction, purpose] : invalid_pairs) {
    mu4_compile_context context = make_mu4_compile_context();
    context.request.cells[0].intents.push_back(make_intent(direction, purpose));

    const mac_ntn_access_calendar_compile_result result = compile_first_cell(context);

    EXPECT_FALSE(result.successful());
    EXPECT_EQ(result.reject_reason, "invalid_intent_direction");
  }
}

TEST(mac_ntn_access_calendar_compiler_test,
     when_mu4_time_uses_one_microsecond_or_rounded_single_slot_then_compile_rejects_inexact_conversion)
{
  // One mu4 slot is 62.5 us, while this contract carries integral microseconds. Neither integer rounding is accepted.
  const std::array<std::pair<std::chrono::microseconds, std::chrono::microseconds>, 3> inexact_windows = {
      {{1us, 125us}, {0us, 62us}, {0us, 63us}}};

  for (const auto& [start_time, duration] : inexact_windows) {
    mu4_compile_context context = make_mu4_compile_context();
    context.request.cells[0].intents.push_back(make_intent(mac_ntn_access_calendar_direction::downlink,
                                                           mac_ntn_access_calendar_purpose::ssb_sib_paging,
                                                           start_time,
                                                           duration));

    const mac_ntn_access_calendar_compile_result result = compile_first_cell(context);

    EXPECT_FALSE(result.successful());
    EXPECT_EQ(result.reject_reason, "intent_not_slot_aligned");
  }
}

TEST(mac_ntn_access_calendar_compiler_test, when_validity_period_is_not_slot_aligned_then_compile_is_rejected)
{
  mu4_compile_context context = make_mu4_compile_context();
  context.request.valid_until = context.request.activation_epoch + 4000001us;

  const mac_ntn_access_calendar_compile_result result = compile_first_cell(context);

  EXPECT_FALSE(result.successful());
  EXPECT_EQ(result.reject_reason, "activation_or_validity_not_slot_aligned");
}

TEST(mac_ntn_access_calendar_compiler_test, when_activation_slot_disagrees_with_wall_clock_then_compile_is_rejected)
{
  mu4_compile_context context = make_mu4_compile_context();
  context.activation_slot += 2U;

  const mac_ntn_access_calendar_compile_result result = compile_first_cell(context);

  EXPECT_FALSE(result.successful());
  EXPECT_EQ(result.reject_reason, "activation_outside_wrap_safe_slot_horizon");
}

TEST(mac_ntn_access_calendar_compiler_test, when_intent_exceeds_cycle_range_then_compile_is_rejected)
{
  mu4_compile_context context = make_mu4_compile_context();
  context.request.cells[0].intents.push_back(make_intent(mac_ntn_access_calendar_direction::downlink,
                                                         mac_ntn_access_calendar_purpose::ssb_sib_paging,
                                                         639ms,
                                                         2ms));

  const mac_ntn_access_calendar_compile_result result = compile_first_cell(context);

  EXPECT_FALSE(result.successful());
  EXPECT_EQ(result.reject_reason, "intent_not_slot_aligned");
}

TEST(mac_ntn_access_calendar_compiler_test, when_cycle_slot_count_exceeds_wire_range_then_compile_is_rejected)
{
  mu4_compile_context context = make_mu4_compile_context();
  constexpr int64_t   one_past_uint32_slots_in_mu4_us =
      ((static_cast<int64_t>(std::numeric_limits<uint32_t>::max()) + 1) * 1000) / 16;
  context.request.cycle_duration = std::chrono::microseconds{one_past_uint32_slots_in_mu4_us};

  const mac_ntn_access_calendar_compile_result result = compile_first_cell(context);

  EXPECT_FALSE(result.successful());
  EXPECT_EQ(result.reject_reason, "cycle_not_slot_aligned");
}

} // namespace
