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

#include "srsran/adt/spsc_queue.h"
#include "srsran/ran/slot_point_extended.h"
#include "srsran/scheduler/ntn_access_calendar.h"
#include <array>
#include <atomic>
#include <limits>
#include <mutex>

namespace srsran {

/// Per-cell NTN access-calendar gate.
///
/// Requests are validated and compiled on a serialized control-plane caller. Fixed-size commands are then published
/// through a bounded SPSC queue. The slot thread performs no allocation, locking, string comparison or I/O.
class scheduler_ntn_access_calendar_gate
{
public:
  scheduler_ntn_access_calendar_gate(du_cell_index_t cell_index_, unsigned numerology_, uint32_t minimum_lead_slots_) :
    cell_index(cell_index_), numerology(numerology_), minimum_lead_slots(minimum_lead_slots_)
  {
  }

  ntn_access_calendar_response handle_update(const ntn_access_calendar_request& request)
  {
    // This mutex never participates in the slot path. It serializes possible control-plane producers and preserves
    // the command queue's SPSC contract.
    std::lock_guard<std::mutex> lock(control_mutex);

    switch (request.operation) {
      case ntn_access_calendar_operation::prepare:
        return prepare(request);
      case ntn_access_calendar_operation::query:
        return query(request);
      case ntn_access_calendar_operation::clear:
        return clear(request);
      case ntn_access_calendar_operation::preflight:
        return make_rejected_response(request, ntn_access_calendar_reject_reason::unsupported);
    }
    return make_rejected_response(request, ntn_access_calendar_reject_reason::unsupported);
  }

  /// Advance the extended slot clock, consume published commands and perform an atomic activation.
  void slot_indication(slot_point slot)
  {
    advance_current_slot(slot);

    while (command_queue.try_pop(slot_command)) {
      if (slot_command.type == command_type::prepare) {
        const slot_difference activation_lead = slot_command.activation_slot - current_slot.without_hyper_sfn();
        pending.valid                         = true;
        pending.version                       = slot_command.version;
        pending.activation_slot               = current_slot + activation_lead;
        pending.valid_until_slot =
            pending.activation_slot + static_cast<uint32_t>(slot_command.validity_slots);
        pending.cycle_slots   = slot_command.cycle_slots;
        pending.purpose_masks = slot_command.purpose_masks;
      } else if (slot_command.type == command_type::clear) {
        if (pending.valid and pending.version == slot_command.version) {
          pending.valid = false;
        }
        if (active.valid and active.version == slot_command.version) {
          // Restore the plan that was active immediately before this version, provided it has not expired.
          if (previous_active.valid and current_slot < previous_active.valid_until_slot) {
            active = previous_active;
          } else {
            active.valid = false;
          }
          previous_active.valid = false;
        } else if (previous_active.valid and previous_active.version == slot_command.version) {
          previous_active.valid = false;
        }
      }
      runtime_sequence.store(slot_command.sequence, std::memory_order_release);
      publish_runtime_versions();
    }

    if (pending.valid and current_slot >= pending.activation_slot) {
      previous_active = active;
      active          = pending;
      pending.valid   = false;
      publish_runtime_versions();
    }
    active_expired.store(active.valid and current_slot >= active.valid_until_slot, std::memory_order_release);
  }

  /// Return whether a statically configured opportunity is authorized for the target slot.
  bool is_allowed(slot_point target_slot, ntn_access_calendar_purpose purpose) const noexcept
  {
    if (not current_slot.valid()) {
      return true;
    }

    const slot_difference target_delta = target_slot - current_slot.without_hyper_sfn();
    if (target_delta < 0) {
      return true;
    }
    const slot_point_extended target_slot_extended = current_slot + target_delta;

    const compiled_snapshot* selected = nullptr;
    if (pending.valid and target_slot_extended >= pending.activation_slot) {
      selected = &pending;
    } else if (active.valid and target_slot_extended >= active.activation_slot) {
      selected = &active;
    }

    // No snapshot means the terrestrial/default scheduler path is unchanged.
    if (selected == nullptr) {
      return true;
    }
    // A known but expired plan fails closed. Explicit clear restores either the previous plan or the default path.
    if (target_slot_extended >= selected->valid_until_slot) {
      return false;
    }

    const slot_difference offset       = target_slot_extended - selected->activation_slot;
    const uint32_t        cycle_offset = static_cast<uint32_t>(offset) % selected->cycle_slots;
    return (selected->purpose_masks[cycle_offset] & ntn_access_calendar_purpose_bit(purpose)) != 0;
  }

  uint32_t get_minimum_lead_slots() const { return minimum_lead_slots; }

private:
  enum class command_type : uint8_t { prepare, clear };

  struct compiled_snapshot {
    bool                                                     valid   = false;
    uint64_t                                                 version = 0;
    slot_point_extended                                      activation_slot;
    slot_point_extended                                      valid_until_slot;
    uint32_t                                                 cycle_slots = 0;
    std::array<uint8_t, MAX_NTN_ACCESS_CALENDAR_CYCLE_SLOTS> purpose_masks;
  };

  struct compiled_command {
    command_type                                             type     = command_type::clear;
    uint64_t                                                 sequence = 0;
    uint64_t                                                 version  = 0;
    slot_point                                               activation_slot;
    uint64_t                                                 validity_slots = 0;
    uint32_t                                                 cycle_slots    = 0;
    std::array<uint8_t, MAX_NTN_ACCESS_CALENDAR_CYCLE_SLOTS> purpose_masks{};
  };

  using command_queue_type = concurrent_queue<compiled_command,
                                              concurrent_queue_policy::lockfree_spsc,
                                              concurrent_queue_wait_policy::non_blocking>;

  ntn_access_calendar_response prepare(const ntn_access_calendar_request& request)
  {
    if (request.cell_index != cell_index) {
      return make_rejected_response(request, ntn_access_calendar_reject_reason::cell_not_configured);
    }
    if (latest_accepted_version != 0 and request.version == latest_accepted_version) {
      if (request.content_hash != latest_content_hash) {
        return make_rejected_response(request, ntn_access_calendar_reject_reason::version_hash_mismatch);
      }
      // Timeout retries are idempotent. A matching plan may only be republished after an explicit clear.
      if (latest_operation == ntn_access_calendar_operation::prepare) {
        return query(request);
      }
    }
    if (request.content_hash.empty() or request.content_hash.size() > MAX_NTN_ACCESS_CALENDAR_HASH_LENGTH) {
      return make_rejected_response(request, ntn_access_calendar_reject_reason::invalid_hash);
    }
    if (request.version == 0 or request.version < latest_accepted_version) {
      return make_rejected_response(request, ntn_access_calendar_reject_reason::stale_version);
    }
    if (not request.activation_slot.valid() or request.activation_slot.numerology() != numerology) {
      return make_rejected_response(request, ntn_access_calendar_reject_reason::invalid_activation_slot);
    }

    const uint32_t current_count = current_raw_slot_count.load(std::memory_order_acquire);
    if (current_count == invalid_slot_count) {
      return make_rejected_response(request, ntn_access_calendar_reject_reason::invalid_activation_slot);
    }
    const slot_point      current{numerology, current_count};
    const slot_difference activation_lead = request.activation_slot - current;
    if (activation_lead < static_cast<slot_difference>(minimum_lead_slots)) {
      return make_rejected_response(request, ntn_access_calendar_reject_reason::activation_too_late);
    }

    const uint64_t extended_half_horizon =
        slot_point_extended{to_subcarrier_spacing(numerology), 0}.nof_slots_in_all_hyper_sfns() / 2U;
    if (request.validity_slots == 0 or request.validity_slots >= extended_half_horizon) {
      return make_rejected_response(request, ntn_access_calendar_reject_reason::invalid_validity);
    }
    if (request.cycle_slots == 0 or request.cycle_slots > MAX_NTN_ACCESS_CALENDAR_CYCLE_SLOTS or
        request.cycle_slots > request.validity_slots) {
      return make_rejected_response(request, ntn_access_calendar_reject_reason::invalid_cycle);
    }
    // An empty checked calendar is an explicit deny-all plan (for example, no currently visible L1 positions).
    // It is distinct from the default state, where no snapshot exists and terrestrial scheduling remains allowed.
    for (const ntn_access_calendar_slot_window& window : request.windows) {
      if (window.nof_slots == 0 or window.start_slot_offset >= request.cycle_slots or
          window.nof_slots > request.cycle_slots - window.start_slot_offset or window.purpose_mask == 0 or
          (window.purpose_mask & ~NTN_ACCESS_CALENDAR_ALL_PURPOSES) != 0) {
        return make_rejected_response(request, ntn_access_calendar_reject_reason::invalid_window);
      }
    }

    compiled_command command;
    command.type            = command_type::prepare;
    command.sequence        = next_sequence + 1;
    command.version         = request.version;
    command.activation_slot = request.activation_slot;
    command.validity_slots  = request.validity_slots;
    command.cycle_slots     = request.cycle_slots;
    command.purpose_masks.fill(0);
    for (const ntn_access_calendar_slot_window& window : request.windows) {
      const uint32_t window_end = window.start_slot_offset + window.nof_slots;
      for (uint32_t offset = window.start_slot_offset; offset != window_end; ++offset) {
        command.purpose_masks[offset] |= window.purpose_mask;
      }
    }

    if (not command_queue.try_push(std::move(command))) {
      return make_rejected_response(request, ntn_access_calendar_reject_reason::command_queue_full);
    }
    ++next_sequence;

    latest_accepted_version = request.version;
    latest_content_hash     = request.content_hash;
    latest_activation_slot  = request.activation_slot;
    latest_operation        = request.operation;
    bool known_entry_updated = false;
    for (unsigned i = 0; i != known_versions.size(); ++i) {
      if (known_versions[i] == request.version) {
        known_hashes[i]      = request.content_hash;
        known_entry_updated  = true;
        break;
      }
    }
    if (not known_entry_updated) {
      known_versions[next_known_entry] = request.version;
      known_hashes[next_known_entry]    = request.content_hash;
      next_known_entry                  = (next_known_entry + 1U) % known_versions.size();
    }
    if (last_clear_version == request.version and last_clear_hash == request.content_hash) {
      last_clear_sequence = 0;
    }

    return make_response(ntn_access_calendar_state::ready,
                         ntn_access_calendar_reject_reason::none,
                         request.version,
                         request.content_hash,
                         request.activation_slot);
  }

  ntn_access_calendar_response query(const ntn_access_calendar_request& request)
  {
    if (latest_accepted_version == 0 and request.version == 0 and request.content_hash.empty()) {
      return make_response(
          ntn_access_calendar_state::cleared, ntn_access_calendar_reject_reason::none, 0, {}, slot_point{});
    }
    if (request.version != latest_accepted_version or request.content_hash != latest_content_hash) {
      return make_rejected_response(request, ntn_access_calendar_reject_reason::version_hash_mismatch);
    }

    if (latest_operation == ntn_access_calendar_operation::clear) {
      const bool clear_consumed =
          last_clear_sequence != 0 and runtime_sequence.load(std::memory_order_acquire) >= last_clear_sequence;
      return make_response(clear_consumed ? ntn_access_calendar_state::cleared : ntn_access_calendar_state::ready,
                           ntn_access_calendar_reject_reason::none,
                           latest_accepted_version,
                           latest_content_hash,
                           latest_activation_slot);
    }

    const uint64_t observed_active  = active_version.load(std::memory_order_acquire);
    const uint64_t observed_pending = pending_version.load(std::memory_order_acquire);
    if (observed_active == latest_accepted_version && active_expired.load(std::memory_order_acquire)) {
      auto response = make_rejected_response(request, ntn_access_calendar_reject_reason::expired);
      response.command_consumed = true;
      return response;
    }
    const ntn_access_calendar_state state = observed_active == latest_accepted_version
                                                ? ntn_access_calendar_state::applied
                                                : ntn_access_calendar_state::ready;
    auto response = make_response(state,
                                  ntn_access_calendar_reject_reason::none,
                                  latest_accepted_version,
                                  latest_content_hash,
                                  latest_activation_slot);
    response.command_consumed = observed_active == latest_accepted_version ||
                                observed_pending == latest_accepted_version;
    return response;
  }

  ntn_access_calendar_response clear(const ntn_access_calendar_request& request)
  {
    if (request.cell_index != cell_index) {
      return make_rejected_response(request, ntn_access_calendar_reject_reason::cell_not_configured);
    }
    if (request.version == 0 or request.content_hash.empty() or
        request.content_hash.size() > MAX_NTN_ACCESS_CALENDAR_HASH_LENGTH) {
      return make_rejected_response(request, ntn_access_calendar_reject_reason::invalid_hash);
    }
    if (request.version == latest_accepted_version and request.content_hash != latest_content_hash) {
      return make_rejected_response(request, ntn_access_calendar_reject_reason::version_hash_mismatch);
    }
    bool known_version_hash = false;
    for (unsigned i = 0; i != known_versions.size(); ++i) {
      known_version_hash |= known_versions[i] == request.version && known_hashes[i] == request.content_hash;
    }
    known_version_hash |= request.version == last_clear_version && request.content_hash == last_clear_hash;
    if (not known_version_hash) {
      return make_rejected_response(request, ntn_access_calendar_reject_reason::version_hash_mismatch);
    }

    if (request.version == last_clear_version and request.content_hash == last_clear_hash and
        last_clear_sequence != 0) {
      const uint64_t processed_sequence = runtime_sequence.load(std::memory_order_acquire);
      if (processed_sequence >= last_clear_sequence) {
        return make_response(ntn_access_calendar_state::cleared,
                             ntn_access_calendar_reject_reason::none,
                             request.version,
                             request.content_hash,
                             request.version == latest_accepted_version ? latest_activation_slot : slot_point{});
      }
      return make_response(ntn_access_calendar_state::ready,
                           ntn_access_calendar_reject_reason::none,
                           request.version,
                           request.content_hash,
                           request.version == latest_accepted_version ? latest_activation_slot : slot_point{});
    }

    compiled_command command;
    command.type     = command_type::clear;
    command.sequence = next_sequence + 1;
    command.version  = request.version;
    command.purpose_masks.fill(0);
    if (not command_queue.try_push(std::move(command))) {
      return make_rejected_response(request, ntn_access_calendar_reject_reason::command_queue_full);
    }
    ++next_sequence;

    last_clear_version  = request.version;
    last_clear_hash     = request.content_hash;
    last_clear_sequence = next_sequence;
    if (request.version == latest_accepted_version and request.content_hash == latest_content_hash) {
      latest_operation = request.operation;
    }

    return make_response(ntn_access_calendar_state::ready,
                         ntn_access_calendar_reject_reason::none,
                         request.version,
                         request.content_hash,
                         request.version == latest_accepted_version ? latest_activation_slot : slot_point{});
  }

  void advance_current_slot(slot_point slot)
  {
    if (not current_slot.valid()) {
      current_slot = slot_point_extended{slot, 0};
    } else {
      const slot_difference delta = slot - current_slot.without_hyper_sfn();
      if (delta > 0) {
        current_slot += delta;
      }
    }
    current_raw_slot_count.store(slot.to_uint(), std::memory_order_release);
  }

  void publish_runtime_versions()
  {
    active_version.store(active.valid ? active.version : 0, std::memory_order_release);
    pending_version.store(pending.valid ? pending.version : 0, std::memory_order_release);
  }

  ntn_access_calendar_response make_rejected_response(const ntn_access_calendar_request& request,
                                                      ntn_access_calendar_reject_reason  reason) const
  {
    return make_response(
        ntn_access_calendar_state::rejected, reason, request.version, request.content_hash, request.activation_slot);
  }

  ntn_access_calendar_response make_response(ntn_access_calendar_state         state,
                                             ntn_access_calendar_reject_reason reason,
                                             uint64_t                          version,
                                             const std::string&                hash,
                                             slot_point                        activation) const
  {
    ntn_access_calendar_response response;
    response.state                     = state;
    response.reason                    = reason;
    response.version                   = version;
    response.content_hash              = hash;
    response.effective_activation_slot = activation;
    response.minimum_lead_slots        = minimum_lead_slots;
    return response;
  }

  static constexpr uint32_t invalid_slot_count     = std::numeric_limits<uint32_t>::max();
  static constexpr unsigned command_queue_capacity = 8;

  const du_cell_index_t cell_index;
  const unsigned        numerology;
  const uint32_t        minimum_lead_slots;

  // Control-plane producer state. Never read directly from the slot path.
  std::mutex                    control_mutex;
  uint64_t                      next_sequence           = 0;
  uint64_t                      latest_accepted_version = 0;
  std::string                   latest_content_hash;
  slot_point                    latest_activation_slot;
  ntn_access_calendar_operation latest_operation   = ntn_access_calendar_operation::query;
  uint64_t                      last_clear_version = 0;
  std::string                   last_clear_hash;
  uint64_t                      last_clear_sequence = 0;
  std::array<uint64_t, 3>       known_versions{};
  std::array<std::string, 3>    known_hashes{};
  unsigned                      next_known_entry = 0;

  command_queue_type command_queue{command_queue_capacity};

  // Slot-thread-only state.
  compiled_command    slot_command;
  slot_point_extended current_slot;
  compiled_snapshot   active;
  compiled_snapshot   pending;
  compiled_snapshot   previous_active;

  // Slot-to-control observations.
  std::atomic<uint32_t> current_raw_slot_count{invalid_slot_count};
  std::atomic<uint64_t> runtime_sequence{0};
  std::atomic<uint64_t> active_version{0};
  std::atomic<uint64_t> pending_version{0};
  std::atomic<bool>     active_expired{false};
};

} // namespace srsran
