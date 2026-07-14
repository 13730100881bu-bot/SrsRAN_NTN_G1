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

#include "lib/mac/mac_ntn_access_calendar_manager.h"
#include <gtest/gtest.h>
#include <array>
#include <chrono>
#include <functional>
#include <initializer_list>
#include <string>
#include <vector>

using namespace srsran;

namespace {

struct expected_scheduler_call {
  ntn_access_calendar_operation     operation;
  du_cell_index_t                   cell_index;
  uint64_t                          version;
  ntn_access_calendar_state         response_state;
  ntn_access_calendar_reject_reason response_reason   = ntn_access_calendar_reject_reason::none;
  bool                              command_consumed = true;
};

class scripted_scheduler
{
public:
  scripted_scheduler(std::initializer_list<expected_scheduler_call> calls_) : calls(calls_) {}

  ntn_access_calendar_response operator()(const ntn_access_calendar_request& request)
  {
    if (next_call >= calls.size()) {
      ADD_FAILURE() << "Unexpected scheduler calendar call";
      return {};
    }
    const expected_scheduler_call& expected = calls[next_call++];
    EXPECT_EQ(request.operation, expected.operation);
    EXPECT_EQ(request.cell_index, expected.cell_index);
    EXPECT_EQ(request.version, expected.version);
    EXPECT_EQ(request.content_hash, calendar_hash(expected.version));

    ntn_access_calendar_response response;
    response.state                     = expected.response_state;
    response.reason                    = expected.response_reason;
    response.version                   = request.version;
    response.content_hash              = request.content_hash;
    response.effective_activation_slot = slot_point{0, 100};
    response.minimum_lead_slots        = 20;
    response.command_consumed          = expected.command_consumed;
    return response;
  }

  void verify_complete() const { EXPECT_EQ(next_call, calls.size()); }

  static std::string calendar_hash(uint64_t version) { return "calendar-hash-" + std::to_string(version); }

private:
  std::vector<expected_scheduler_call> calls;
  size_t                               next_call = 0;
};

mac_ntn_access_calendar_update make_update(uint64_t version, mac_ntn_access_calendar_operation operation)
{
  mac_ntn_access_calendar_update request;
  request.operation         = operation;
  request.schedule_version  = version;
  request.calendar_hash     = scripted_scheduler::calendar_hash(version);
  request.activation_epoch  = std::chrono::system_clock::now() + std::chrono::seconds{10};
  request.valid_until       = request.activation_epoch + std::chrono::hours{1};
  request.cycle_duration    = std::chrono::milliseconds{80};
  request.cells[0].cell_index = to_du_cell_index(0);
  request.cells[1].cell_index = to_du_cell_index(1);
  return request;
}

std::array<ntn_access_calendar_request, 2> make_scheduler_requests(uint64_t version)
{
  std::array<ntn_access_calendar_request, 2> requests;
  for (unsigned i = 0; i != requests.size(); ++i) {
    requests[i].operation        = ntn_access_calendar_operation::prepare;
    requests[i].cell_index       = to_du_cell_index(i);
    requests[i].version          = version;
    requests[i].content_hash     = scripted_scheduler::calendar_hash(version);
    requests[i].activation_slot  = slot_point{0, 100};
    requests[i].validity_slots   = 3600;
    requests[i].cycle_slots      = 80;
  }
  return requests;
}

TEST(mac_ntn_access_calendar_manager_test,
     when_only_one_cell_has_consumed_prepare_command_then_two_cell_transaction_is_preparing)
{
  mac_ntn_access_calendar_manager manager;
  const auto                      request            = make_update(1, mac_ntn_access_calendar_operation::prepare);
  const auto                      scheduler_requests = make_scheduler_requests(1);
  scripted_scheduler scheduler{{ntn_access_calendar_operation::prepare,
                                 to_du_cell_index(0),
                                 1,
                                 ntn_access_calendar_state::ready,
                                 ntn_access_calendar_reject_reason::none,
                                 true},
                                {ntn_access_calendar_operation::prepare,
                                 to_du_cell_index(1),
                                 1,
                                 ntn_access_calendar_state::ready,
                                 ntn_access_calendar_reject_reason::none,
                                 false}};

  const auto result = manager.handle_prepare(
      request, scheduler_requests, std::array<unsigned, 2>{3, 5}, std::ref(scheduler));

  EXPECT_EQ(result.status, mac_ntn_access_calendar_status::preparing);
  EXPECT_EQ(result.reason, "waiting_for_both_cell_slot_threads_to_arm");
  EXPECT_EQ(result.accepted_intents, (std::array<unsigned, 2>{3, 5}));
  scheduler.verify_complete();
}

TEST(mac_ntn_access_calendar_manager_test,
     when_second_cell_rejects_prepare_then_same_key_is_blocked_until_partial_cleanup_converges)
{
  mac_ntn_access_calendar_manager manager;
  const auto                      prepare_request    = make_update(7, mac_ntn_access_calendar_operation::prepare);
  const auto                      scheduler_requests = make_scheduler_requests(7);
  scripted_scheduler prepare_scheduler{{ntn_access_calendar_operation::prepare,
                                         to_du_cell_index(0),
                                         7,
                                         ntn_access_calendar_state::ready},
                                        {ntn_access_calendar_operation::prepare,
                                         to_du_cell_index(1),
                                         7,
                                         ntn_access_calendar_state::rejected,
                                         ntn_access_calendar_reject_reason::command_queue_full,
                                         false},
                                        {ntn_access_calendar_operation::clear,
                                         to_du_cell_index(0),
                                         7,
                                         ntn_access_calendar_state::ready,
                                         ntn_access_calendar_reject_reason::none,
                                         false}};

  const auto prepare_result = manager.handle_prepare(
      prepare_request, scheduler_requests, std::array<unsigned, 2>{4, 6}, std::ref(prepare_scheduler));

  EXPECT_EQ(prepare_result.status, mac_ntn_access_calendar_status::rejected);
  EXPECT_EQ(prepare_result.reason, "scheduler_command_queue_full");
  ASSERT_TRUE(manager.reject_prepare_if_cleanup_pending(prepare_request).has_value());
  EXPECT_EQ(manager.reject_prepare_if_cleanup_pending(prepare_request)->reason, "rollback_cleanup_pending");
  EXPECT_EQ(manager.reject_prepare_if_cleanup_pending(make_update(8, mac_ntn_access_calendar_operation::prepare))
                ->reason,
            "rollback_cleanup_pending");
  prepare_scheduler.verify_complete();

  const auto clear_request = make_update(7, mac_ntn_access_calendar_operation::clear);
  scripted_scheduler first_clear{{ntn_access_calendar_operation::clear,
                                   to_du_cell_index(0),
                                   7,
                                   ntn_access_calendar_state::ready,
                                   ntn_access_calendar_reject_reason::none,
                                   false}};
  const auto first_clear_result = manager.handle_query_or_clear(clear_request, std::ref(first_clear));
  EXPECT_EQ(first_clear_result.status, mac_ntn_access_calendar_status::ready);
  EXPECT_EQ(first_clear_result.reason, "clear_armed");
  EXPECT_TRUE(manager.reject_prepare_if_cleanup_pending(prepare_request).has_value());
  first_clear.verify_complete();

  scripted_scheduler final_clear{{ntn_access_calendar_operation::clear,
                                   to_du_cell_index(0),
                                   7,
                                   ntn_access_calendar_state::cleared}};
  const auto final_clear_result = manager.handle_query_or_clear(clear_request, std::ref(final_clear));
  EXPECT_EQ(final_clear_result.status, mac_ntn_access_calendar_status::cleared);
  EXPECT_EQ(final_clear_result.reason, "cleared");
  EXPECT_FALSE(manager.reject_prepare_if_cleanup_pending(prepare_request).has_value());
  EXPECT_FALSE(
      manager.reject_prepare_if_cleanup_pending(make_update(8, mac_ntn_access_calendar_operation::prepare)).has_value());
  final_clear.verify_complete();
}

TEST(mac_ntn_access_calendar_manager_test,
     when_applied_successor_is_cleared_then_previous_calendar_is_restored_and_remains_clearable)
{
  mac_ntn_access_calendar_manager manager;

  for (uint64_t version : {1U, 2U}) {
    const auto prepare_request    = make_update(version, mac_ntn_access_calendar_operation::prepare);
    const auto scheduler_requests = make_scheduler_requests(version);
    scripted_scheduler prepare_scheduler{{ntn_access_calendar_operation::prepare,
                                           to_du_cell_index(0),
                                           version,
                                           ntn_access_calendar_state::ready},
                                          {ntn_access_calendar_operation::prepare,
                                           to_du_cell_index(1),
                                           version,
                                           ntn_access_calendar_state::ready}};
    const auto prepare_result = manager.handle_prepare(
        prepare_request, scheduler_requests, std::array<unsigned, 2>{2, 2}, std::ref(prepare_scheduler));
    EXPECT_EQ(prepare_result.status, mac_ntn_access_calendar_status::ready);
    prepare_scheduler.verify_complete();

    const auto query_request = make_update(version, mac_ntn_access_calendar_operation::query);
    scripted_scheduler query_scheduler{{ntn_access_calendar_operation::query,
                                         to_du_cell_index(0),
                                         version,
                                         ntn_access_calendar_state::applied},
                                        {ntn_access_calendar_operation::query,
                                         to_du_cell_index(1),
                                         version,
                                         ntn_access_calendar_state::applied}};
    const auto query_result = manager.handle_query_or_clear(query_request, std::ref(query_scheduler));
    EXPECT_EQ(query_result.status, mac_ntn_access_calendar_status::applied);
    query_scheduler.verify_complete();
  }

  const auto clear_v2 = make_update(2, mac_ntn_access_calendar_operation::clear);
  scripted_scheduler clear_v2_scheduler{{ntn_access_calendar_operation::clear,
                                         to_du_cell_index(0),
                                         2,
                                         ntn_access_calendar_state::cleared},
                                        {ntn_access_calendar_operation::clear,
                                         to_du_cell_index(1),
                                         2,
                                         ntn_access_calendar_state::cleared}};
  EXPECT_EQ(manager.handle_query_or_clear(clear_v2, std::ref(clear_v2_scheduler)).status,
            mac_ntn_access_calendar_status::cleared);
  clear_v2_scheduler.verify_complete();

  const auto clear_v1 = make_update(1, mac_ntn_access_calendar_operation::clear);
  scripted_scheduler clear_v1_scheduler{{ntn_access_calendar_operation::clear,
                                         to_du_cell_index(0),
                                         1,
                                         ntn_access_calendar_state::cleared},
                                        {ntn_access_calendar_operation::clear,
                                         to_du_cell_index(1),
                                         1,
                                         ntn_access_calendar_state::cleared}};
  EXPECT_EQ(manager.handle_query_or_clear(clear_v1, std::ref(clear_v1_scheduler)).status,
            mac_ntn_access_calendar_status::cleared);
  clear_v1_scheduler.verify_complete();

  const auto query_v1 = make_update(1, mac_ntn_access_calendar_operation::query);
  scripted_scheduler no_scheduler_calls{};
  const auto calendar_not_found = manager.handle_query_or_clear(query_v1, std::ref(no_scheduler_calls));
  EXPECT_EQ(calendar_not_found.status, mac_ntn_access_calendar_status::rejected);
  EXPECT_EQ(calendar_not_found.reason, "calendar_not_found");
  no_scheduler_calls.verify_complete();
}

} // namespace
