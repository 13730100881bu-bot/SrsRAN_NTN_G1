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

#include "srsran/mac/mac_cell_manager.h"
#include "srsran/mac/mac_manager.h"
#include "srsran/scheduler/ntn_access_calendar.h"
#include <limits>
#include <map>
#include <optional>
#include <tuple>
#include <utility>

namespace srsran {

/// Result of compiling one MAC access-calendar cell into the scheduler slot domain.
///
/// This is a private MAC translation contract. It does not mutate scheduler state and does not claim PHY or RF
/// application.
struct mac_ntn_access_calendar_compile_result {
  std::optional<ntn_access_calendar_request> scheduler_request;
  unsigned                                   accepted_intents = 0;
  std::string                                reject_reason;

  bool successful() const { return scheduler_request.has_value(); }
};

namespace mac_ntn_access_calendar_compiler_detail {

inline std::optional<int64_t> microseconds_to_slots(int64_t microseconds, unsigned slots_per_ms)
{
  if (slots_per_ms == 0 ||
      microseconds > std::numeric_limits<int64_t>::max() / static_cast<int64_t>(slots_per_ms) ||
      microseconds < std::numeric_limits<int64_t>::min() / static_cast<int64_t>(slots_per_ms)) {
    return std::nullopt;
  }
  const int64_t scaled = microseconds * static_cast<int64_t>(slots_per_ms);
  if (scaled % 1000 != 0) {
    return std::nullopt;
  }
  return scaled / 1000;
}

struct purpose_mask_result {
  uint8_t mask            = 0;
  bool    direction_valid = true;
};

inline purpose_mask_result purpose_mask(const mac_ntn_access_calendar_intent& intent)
{
  switch (intent.purpose) {
    case mac_ntn_access_calendar_purpose::ssb_sib_paging:
      if (intent.direction != mac_ntn_access_calendar_direction::downlink) {
        return {0, false};
      }
      return {static_cast<uint8_t>(ntn_access_calendar_purpose_bit(ntn_access_calendar_purpose::ssb) |
                                   ntn_access_calendar_purpose_bit(ntn_access_calendar_purpose::sib) |
                                   ntn_access_calendar_purpose_bit(ntn_access_calendar_purpose::paging)),
              true};
    case mac_ntn_access_calendar_purpose::ssb_sib_paging_rar:
      if (intent.direction != mac_ntn_access_calendar_direction::downlink) {
        return {0, false};
      }
      return {static_cast<uint8_t>(ntn_access_calendar_purpose_bit(ntn_access_calendar_purpose::ssb) |
                                   ntn_access_calendar_purpose_bit(ntn_access_calendar_purpose::sib) |
                                   ntn_access_calendar_purpose_bit(ntn_access_calendar_purpose::paging) |
                                   ntn_access_calendar_purpose_bit(ntn_access_calendar_purpose::rar)),
              true};
    case mac_ntn_access_calendar_purpose::prach_ro:
    case mac_ntn_access_calendar_purpose::prach_ul_beam:
      if (intent.direction != mac_ntn_access_calendar_direction::uplink) {
        return {0, false};
      }
      return {ntn_access_calendar_purpose_bit(ntn_access_calendar_purpose::prach), true};
  }
  // Preserve the previous internal behavior for an out-of-domain enum. DU/F1 validation prevents this path.
  return {};
}

} // namespace mac_ntn_access_calendar_compiler_detail

/// Compile one cell of a versioned MAC access calendar into scheduler slot offsets.
///
/// The supplied mapping and activation slot must come from the same cell time mapper. Conversion is exact: a duration
/// that cannot be represented as an integral number of slots is rejected instead of being rounded.
inline mac_ntn_access_calendar_compile_result
compile_mac_ntn_access_calendar_cell(const mac_ntn_access_calendar_update& request,
                                     const mac_ntn_access_calendar_cell&   cell,
                                     const mac_cell_slot_time_info&        last_mapping,
                                     slot_point                            activation_slot)
{
  using namespace mac_ntn_access_calendar_compiler_detail;

  mac_ntn_access_calendar_compile_result result;
  if (!activation_slot.valid() || !last_mapping.sl_tx.valid() ||
      activation_slot.numerology() != last_mapping.sl_tx.numerology()) {
    result.reject_reason = "unsupported_slot_duration";
    return result;
  }

  const unsigned slots_per_ms = activation_slot.nof_slots_per_subframe();
  if (slots_per_ms == 0) {
    result.reject_reason = "unsupported_slot_duration";
    return result;
  }

  const int64_t activation_delta_us =
      std::chrono::duration_cast<std::chrono::microseconds>(request.activation_epoch - last_mapping.time_point)
          .count();
  const int64_t validity_duration_us =
      std::chrono::duration_cast<std::chrono::microseconds>(request.valid_until - request.activation_epoch).count();
  const auto expected_activation_slots = microseconds_to_slots(activation_delta_us, slots_per_ms);
  const auto expected_validity_slots   = microseconds_to_slots(validity_duration_us, slots_per_ms);
  const auto cycle_slots               = microseconds_to_slots(request.cycle_duration.count(), slots_per_ms);
  if (!expected_activation_slots.has_value() || !expected_validity_slots.has_value() || !cycle_slots.has_value() ||
      expected_validity_slots.value() <= 0 || cycle_slots.value() <= 0) {
    result.reject_reason = "activation_or_validity_not_slot_aligned";
    return result;
  }

  const int64_t mapped_activation_slots = activation_slot - last_mapping.sl_tx;
  // Plain slot_point subtraction is wrap-aware and therefore only unambiguous within half a hyperframe. Long validity
  // is represented separately as a slot count and tracked by the scheduler in its extended monotonic slot domain.
  if (mapped_activation_slots != expected_activation_slots.value() ||
      expected_validity_slots.value() > static_cast<int64_t>(std::numeric_limits<uint32_t>::max())) {
    result.reject_reason = "activation_outside_wrap_safe_slot_horizon";
    return result;
  }
  if (cycle_slots.value() > static_cast<int64_t>(std::numeric_limits<uint32_t>::max())) {
    result.reject_reason = "cycle_not_slot_aligned";
    return result;
  }

  ntn_access_calendar_request scheduler_request;
  scheduler_request.operation       = ntn_access_calendar_operation::prepare;
  scheduler_request.cell_index      = cell.cell_index;
  scheduler_request.version         = request.schedule_version;
  scheduler_request.content_hash    = request.calendar_hash;
  scheduler_request.activation_slot = activation_slot;
  scheduler_request.validity_slots  = static_cast<uint32_t>(expected_validity_slots.value());
  scheduler_request.cycle_slots     = static_cast<uint32_t>(cycle_slots.value());

  std::map<std::tuple<uint32_t, uint32_t>, uint8_t> merged_windows;
  for (const mac_ntn_access_calendar_intent& intent : cell.intents) {
    const int64_t start_time_us = intent.start_time.count();
    const int64_t duration_us   = intent.duration.count();
    const int64_t cycle_us      = request.cycle_duration.count();
    const auto    intent_start_slots    = microseconds_to_slots(start_time_us, slots_per_ms);
    const auto    intent_duration_slots = microseconds_to_slots(duration_us, slots_per_ms);
    if (start_time_us < 0 || duration_us <= 0 || start_time_us > cycle_us || duration_us > cycle_us - start_time_us ||
        !intent_start_slots.has_value() || !intent_duration_slots.has_value() ||
        intent_duration_slots.value() <= 0) {
      result.reject_reason = "intent_not_slot_aligned";
      return result;
    }

    const purpose_mask_result mask = purpose_mask(intent);
    if (!mask.direction_valid) {
      result.reject_reason = "invalid_intent_direction";
      return result;
    }
    const auto key = std::make_tuple(static_cast<uint32_t>(intent_start_slots.value()),
                                     static_cast<uint32_t>(intent_duration_slots.value()));
    merged_windows[key] |= mask.mask;
  }

  scheduler_request.windows.reserve(merged_windows.size());
  for (const auto& [window, mask] : merged_windows) {
    scheduler_request.windows.push_back({std::get<0>(window), std::get<1>(window), mask});
  }
  result.accepted_intents = cell.intents.size();
  result.scheduler_request.emplace(std::move(scheduler_request));
  return result;
}

} // namespace srsran
