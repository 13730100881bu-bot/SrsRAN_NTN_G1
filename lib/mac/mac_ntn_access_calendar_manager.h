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

#pragma once

#include "srsran/mac/mac_manager.h"
#include "srsran/scheduler/ntn_access_calendar.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace srsran {

/// Coordinates the two-cell MAC side of an NTN access-calendar transaction.
///
/// Time mapping and intent compilation remain in mac_impl. This class owns only the cross-cell transaction records
/// and aggregation rules, and accepts a scheduler callback so those rules can be tested without a slot thread.
class mac_ntn_access_calendar_manager
{
public:
  template <typename SchedulerHandler>
  mac_ntn_access_calendar_result handle_query_or_clear(const mac_ntn_access_calendar_update& request,
                                                        SchedulerHandler&&                    scheduler_handler)
  {
    mac_ntn_access_calendar_result result = make_result(request);

    const ntn_calendar_record* record            = nullptr;
    bool                       record_is_cleanup = false;
    const bool staged_matches = staged_calendar.has_value() && record_matches(*staged_calendar, request);
    const bool active_matches = active_calendar.has_value() && record_matches(*active_calendar, request);
    const bool previous_active_matches =
        previous_active_calendar.has_value() && record_matches(*previous_active_calendar, request);
    if (staged_matches) {
      record = &*staged_calendar;
    } else if (active_matches) {
      record = &*active_calendar;
    } else if (previous_active_matches) {
      record = &*previous_active_calendar;
    } else {
      const auto cleanup_it = find_cleanup(request);
      if (cleanup_it != cleanup_calendars.end()) {
        record            = &*cleanup_it;
        record_is_cleanup = true;
      }
    }
    if (record == nullptr) {
      result.reason = "calendar_not_found";
      return result;
    }
    result.preflight_reports = record->preflight_reports;
    if (record_is_cleanup && request.operation == mac_ntn_access_calendar_operation::query) {
      result.reason = "rollback_cleanup_pending";
      return result;
    }

    bool all_applied = true;
    bool all_cleared = true;
    bool all_armed   = true;
    for (unsigned i = 0; i != record->nof_cells; ++i) {
      ntn_access_calendar_request scheduler_request;
      scheduler_request.operation = request.operation == mac_ntn_access_calendar_operation::query
                                        ? ntn_access_calendar_operation::query
                                        : ntn_access_calendar_operation::clear;
      scheduler_request.cell_index   = record->cell_indexes[i];
      scheduler_request.version      = record->version;
      scheduler_request.content_hash = record->hash;
      const ntn_access_calendar_response response = scheduler_handler(scheduler_request);
      if (!map_scheduler_result(result, response)) {
        return result;
      }
      all_applied &= response.state == ntn_access_calendar_state::applied;
      all_cleared &= response.state == ntn_access_calendar_state::cleared;
      all_armed &= response.command_consumed;
      result.accepted_intents[i] = record->accepted_intents[i];
    }

    if (request.operation == mac_ntn_access_calendar_operation::clear) {
      if (all_cleared) {
        remove_record(request, staged_matches, active_matches, previous_active_matches);
      }
      result.status = all_cleared ? mac_ntn_access_calendar_status::cleared : mac_ntn_access_calendar_status::ready;
      result.reason = all_cleared ? "cleared" : "clear_armed";
    } else if (all_cleared) {
      remove_record(request, staged_matches, active_matches, previous_active_matches);
      result.status = mac_ntn_access_calendar_status::cleared;
      result.reason = "cleared";
    } else if (all_applied) {
      if (active_calendar.has_value() && !active_matches) {
        previous_active_calendar = active_calendar;
      }
      active_calendar = *record;
      if (staged_calendar.has_value() && record_matches(*staged_calendar, request)) {
        staged_calendar.reset();
      }
      result.status = mac_ntn_access_calendar_status::applied;
      result.reason = "ssb_prach_software_gate_applied_no_position_or_rf_evidence";
    } else if (all_armed) {
      result.status = mac_ntn_access_calendar_status::ready;
      result.reason = "both_cell_slot_threads_armed";
    } else {
      result.status = mac_ntn_access_calendar_status::preparing;
      result.reason = "waiting_for_both_cell_slot_threads_to_arm";
    }
    return result;
  }

  bool has_cleanup_for(const mac_ntn_access_calendar_update& request) const
  {
    return std::any_of(cleanup_calendars.begin(), cleanup_calendars.end(), [&request](const auto& record) {
      return record_matches(record, request);
    });
  }

  std::optional<mac_ntn_access_calendar_result>
  reject_prepare_if_cleanup_pending(const mac_ntn_access_calendar_update& request) const
  {
    // Keep at most active/pending/previous in the scheduler version history. A partial rollback must converge before
    // any later version is admitted, otherwise a fourth live key could evict the version that still needs clearing.
    if (cleanup_calendars.empty()) {
      return std::nullopt;
    }
    mac_ntn_access_calendar_result result = make_result(request);
    result.reason                         = "rollback_cleanup_pending";
    return result;
  }

  template <typename SchedulerHandler>
  mac_ntn_access_calendar_result
  handle_prepare(const mac_ntn_access_calendar_update&              request,
                 const std::array<ntn_access_calendar_request, 2>& scheduler_requests,
                 const std::array<unsigned, 2>&                    accepted_intents,
                 SchedulerHandler&&                                scheduler_handler)
  {
    mac_ntn_access_calendar_result result = make_result(request);
    ntn_calendar_record            prepared_record;
    prepared_record.version          = request.schedule_version;
    prepared_record.hash             = request.calendar_hash;
    prepared_record.valid_until      = request.valid_until;
    prepared_record.cell_indexes     = {scheduler_requests[0].cell_index, scheduler_requests[1].cell_index};
    prepared_record.accepted_intents = accepted_intents;

    std::optional<mac_ntn_access_calendar_status> preflight_failure_status;
    std::string                                   preflight_failure_reason;
    for (unsigned i = 0; i != scheduler_requests.size(); ++i) {
      ntn_access_calendar_request preflight_request = scheduler_requests[i];
      preflight_request.operation                   = ntn_access_calendar_operation::preflight;
      const ntn_access_calendar_response response   = scheduler_handler(preflight_request);
      result.preflight_reports[i]                   = map_preflight_report(response.preflight);

      const bool scheduler_accepted = map_scheduler_result(result, response);
      if ((!scheduler_accepted || !response.preflight.performed || !response.preflight.passed) &&
          !preflight_failure_status.has_value()) {
        preflight_failure_status = response.reason == ntn_access_calendar_reject_reason::unsupported
                                       ? mac_ntn_access_calendar_status::unsupported
                                       : mac_ntn_access_calendar_status::rejected;
        if (!response.preflight.performed) {
          preflight_failure_reason = "scheduler_preflight_not_performed";
        } else if (!response.preflight.passed) {
          preflight_failure_reason = result.reason.empty() ? "static_opportunity_mismatch" : result.reason;
        } else {
          preflight_failure_reason = result.reason;
        }
      }
    }
    if (preflight_failure_status.has_value()) {
      result.status = preflight_failure_status.value();
      result.reason = std::move(preflight_failure_reason);
      return result;
    }
    prepared_record.preflight_reports = result.preflight_reports;

    bool all_armed = true;
    for (unsigned i = 0; i != scheduler_requests.size(); ++i) {
      const ntn_access_calendar_response response = scheduler_handler(scheduler_requests[i]);
      if (!map_scheduler_result(result, response)) {
        bool rollback_complete = true;
        for (unsigned rollback = 0; rollback != i; ++rollback) {
          ntn_access_calendar_request clear_request;
          clear_request.operation    = ntn_access_calendar_operation::clear;
          clear_request.cell_index   = scheduler_requests[rollback].cell_index;
          clear_request.version      = request.schedule_version;
          clear_request.content_hash = request.calendar_hash;
          const ntn_access_calendar_response clear_response = scheduler_handler(clear_request);
          rollback_complete &= clear_response.state == ntn_access_calendar_state::cleared;
        }
        if (prepared_record.nof_cells != 0 && !rollback_complete) {
          const auto cleanup_it = find_cleanup(request);
          if (cleanup_it == cleanup_calendars.end()) {
            cleanup_calendars.push_back(prepared_record);
          } else {
            *cleanup_it = prepared_record;
          }
        }
        return result;
      }
      all_armed &= response.command_consumed;
      result.accepted_intents[i] = prepared_record.accepted_intents[i];
      prepared_record.nof_cells  = i + 1;
    }

    staged_calendar = std::move(prepared_record);
    result.status = all_armed ? mac_ntn_access_calendar_status::ready : mac_ntn_access_calendar_status::preparing;
    result.reason = all_armed ? "both_cell_slot_threads_armed" : "waiting_for_both_cell_slot_threads_to_arm";
    return result;
  }

private:
  struct ntn_calendar_record {
    uint64_t                       version = 0;
    std::string                    hash;
    std::array<du_cell_index_t, 2> cell_indexes{INVALID_DU_CELL_INDEX, INVALID_DU_CELL_INDEX};
    std::array<unsigned, 2>        accepted_intents{};
    std::array<mac_ntn_access_calendar_preflight_report, 2> preflight_reports{};
    unsigned                       nof_cells = 0;
    std::chrono::system_clock::time_point valid_until{};
  };

  static mac_ntn_access_calendar_result make_result(const mac_ntn_access_calendar_update& request)
  {
    mac_ntn_access_calendar_result result;
    result.schedule_version = request.schedule_version;
    result.calendar_hash    = request.calendar_hash;
    return result;
  }

  static const char* reject_reason_to_string(ntn_access_calendar_reject_reason reason)
  {
    switch (reason) {
      case ntn_access_calendar_reject_reason::none:
        return "none";
      case ntn_access_calendar_reject_reason::unsupported:
        return "scheduler_gate_unsupported";
      case ntn_access_calendar_reject_reason::cell_not_configured:
        return "cell_not_configured";
      case ntn_access_calendar_reject_reason::invalid_hash:
        return "invalid_calendar_hash";
      case ntn_access_calendar_reject_reason::stale_version:
        return "stale_schedule_version";
      case ntn_access_calendar_reject_reason::invalid_activation_slot:
        return "invalid_activation_slot";
      case ntn_access_calendar_reject_reason::invalid_validity:
        return "invalid_validity";
      case ntn_access_calendar_reject_reason::invalid_cycle:
        return "invalid_cycle";
      case ntn_access_calendar_reject_reason::invalid_window:
        return "calendar_window_not_slot_aligned";
      case ntn_access_calendar_reject_reason::static_opportunity_missing:
        return "static_opportunity_missing";
      case ntn_access_calendar_reject_reason::activation_too_late:
        return "activation_too_late";
      case ntn_access_calendar_reject_reason::expired:
        return "calendar_expired";
      case ntn_access_calendar_reject_reason::version_hash_mismatch:
        return "version_hash_mismatch";
      case ntn_access_calendar_reject_reason::command_queue_full:
        return "scheduler_command_queue_full";
    }
    return "unknown_scheduler_rejection";
  }

  static mac_ntn_access_calendar_preflight_report
  map_preflight_report(const ntn_access_calendar_preflight_report& source)
  {
    mac_ntn_access_calendar_preflight_report target;
    if (!source.performed) {
      return target;
    }

    target.performed           = true;
    target.passed              = source.passed;
    target.numerology          = static_cast<uint8_t>(std::min(source.numerology, 0xffU));
    target.expected_ssb        = source.expected_ssb;
    target.matched_ssb         = source.matched_ssb;
    target.expected_prach      = source.expected_prach;
    target.matched_prach       = source.matched_prach;
    target.max_ssb_gap_slots   = source.max_ssb_gap_slots;
    target.max_prach_gap_slots = source.max_prach_gap_slots;
    if (source.first_unmatched_present) {
      mac_ntn_access_calendar_unmatched_intent unmatched;
      unmatched.position_id       = source.first_unmatched_position_id;
      unmatched.start_slot_offset = source.first_unmatched_start_slot_offset;
      unmatched.nof_slots         = source.first_unmatched_nof_slots;
      switch (source.first_unmatched_purpose) {
        case ntn_access_calendar_purpose::ssb:
          unmatched.purpose = mac_ntn_access_calendar_preflight_purpose::ssb;
          break;
        case ntn_access_calendar_purpose::prach:
          unmatched.purpose = mac_ntn_access_calendar_preflight_purpose::prach;
          break;
        default:
          unmatched.purpose = mac_ntn_access_calendar_preflight_purpose::invalid;
          break;
      }
      target.first_unmatched.emplace(std::move(unmatched));
    }
    return target;
  }

  static bool map_scheduler_result(mac_ntn_access_calendar_result& result,
                                   const ntn_access_calendar_response& response)
  {
    result.effective_activation_slot = response.effective_activation_slot;
    result.activation_numerology =
        response.effective_activation_slot.valid() ? response.effective_activation_slot.numerology() : 0;
    result.minimum_lead_slots = std::max(result.minimum_lead_slots, response.minimum_lead_slots);
    if (response.state == ntn_access_calendar_state::rejected) {
      result.status = response.reason == ntn_access_calendar_reject_reason::unsupported
                          ? mac_ntn_access_calendar_status::unsupported
                          : mac_ntn_access_calendar_status::rejected;
      result.reason = reject_reason_to_string(response.reason);
      return false;
    }
    return true;
  }

  static bool record_matches(const ntn_calendar_record& record, const mac_ntn_access_calendar_update& request)
  {
    return record.version == request.schedule_version && record.hash == request.calendar_hash;
  }

  auto find_cleanup(const mac_ntn_access_calendar_update& request)
  {
    return std::find_if(cleanup_calendars.begin(), cleanup_calendars.end(), [&request](const auto& record) {
      return record_matches(record, request);
    });
  }

  auto find_cleanup(const mac_ntn_access_calendar_update& request) const
  {
    return std::find_if(cleanup_calendars.cbegin(), cleanup_calendars.cend(), [&request](const auto& record) {
      return record_matches(record, request);
    });
  }

  void remove_record(const mac_ntn_access_calendar_update& request,
                     bool                                  staged_matches,
                     bool                                  active_matches,
                     bool                                  previous_active_matches)
  {
    if (staged_matches) {
      staged_calendar.reset();
    }
    if (active_matches) {
      if (previous_active_calendar.has_value() &&
          std::chrono::system_clock::now() < previous_active_calendar->valid_until) {
        active_calendar = previous_active_calendar;
      } else {
        active_calendar.reset();
      }
      previous_active_calendar.reset();
    } else if (previous_active_matches) {
      previous_active_calendar.reset();
    }
    cleanup_calendars.erase(
        std::remove_if(cleanup_calendars.begin(), cleanup_calendars.end(), [&request](const auto& record) {
          return record_matches(record, request);
        }),
        cleanup_calendars.end());
  }

  std::optional<ntn_calendar_record> staged_calendar;
  std::optional<ntn_calendar_record> active_calendar;
  std::optional<ntn_calendar_record> previous_active_calendar;
  /// Partially prepared two-cell transactions retained until every prepared cell confirms clear.
  std::vector<ntn_calendar_record> cleanup_calendars;
};

} // namespace srsran
