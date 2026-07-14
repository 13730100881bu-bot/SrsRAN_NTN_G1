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

#include "srsran/ran/du_types.h"
#include "srsran/ran/slot_point.h"
#include <cstdint>
#include <string>
#include <vector>

namespace srsran {

/// Maximum cycle size accepted by the scheduler access-calendar gate.
constexpr unsigned MAX_NTN_ACCESS_CALENDAR_CYCLE_SLOTS = 16384;

/// Maximum content-hash text length accepted by the scheduler access-calendar gate.
constexpr unsigned MAX_NTN_ACCESS_CALENDAR_HASH_LENGTH = 128;

enum class ntn_access_calendar_operation : uint8_t { prepare, query, clear };

/// Common-channel purposes that can be authorized by an access-calendar window.
enum class ntn_access_calendar_purpose : uint8_t {
  ssb    = 1U << 0U,
  prach  = 1U << 1U,
  sib    = 1U << 2U,
  paging = 1U << 3U,
  rar    = 1U << 4U
};

constexpr uint8_t ntn_access_calendar_purpose_bit(ntn_access_calendar_purpose purpose)
{
  return static_cast<uint8_t>(purpose);
}

constexpr uint8_t NTN_ACCESS_CALENDAR_ALL_PURPOSES =
    ntn_access_calendar_purpose_bit(ntn_access_calendar_purpose::ssb) |
    ntn_access_calendar_purpose_bit(ntn_access_calendar_purpose::prach) |
    ntn_access_calendar_purpose_bit(ntn_access_calendar_purpose::sib) |
    ntn_access_calendar_purpose_bit(ntn_access_calendar_purpose::paging) |
    ntn_access_calendar_purpose_bit(ntn_access_calendar_purpose::rar);

/// A half-open access window [start_slot_offset, start_slot_offset + nof_slots) within a repeating cycle.
struct ntn_access_calendar_slot_window {
  uint32_t start_slot_offset = 0;
  uint32_t nof_slots         = 0;
  uint8_t  purpose_mask      = 0;
};

/// Per-cell, already slot-compiled access calendar request.
///
/// The valid interval is [activation_slot, activation_slot + validity_slots). Windows repeat every cycle_slots from
/// activation_slot. validity_slots is explicit so a multi-SFN-hyperframe plan cannot be silently truncated by the
/// plain slot_point representation.
/// The calendar only filters already configured static SSB and PRACH opportunities; it never creates a new radio
/// opportunity or changes the cell TDD pattern.
struct ntn_access_calendar_request {
  ntn_access_calendar_operation                operation  = ntn_access_calendar_operation::query;
  du_cell_index_t                              cell_index = INVALID_DU_CELL_INDEX;
  uint64_t                                     version    = 0;
  std::string                                  content_hash;
  slot_point                                   activation_slot;
  uint64_t                                     validity_slots = 0;
  uint32_t                                     cycle_slots    = 0;
  std::vector<ntn_access_calendar_slot_window> windows;
};

enum class ntn_access_calendar_state : uint8_t { ready, applied, cleared, rejected };

enum class ntn_access_calendar_reject_reason : uint8_t {
  none,
  unsupported,
  cell_not_configured,
  invalid_hash,
  stale_version,
  invalid_activation_slot,
  invalid_validity,
  invalid_cycle,
  invalid_window,
  activation_too_late,
  expired,
  version_hash_mismatch,
  command_queue_full
};

struct ntn_access_calendar_response {
  ntn_access_calendar_state         state   = ntn_access_calendar_state::rejected;
  ntn_access_calendar_reject_reason reason  = ntn_access_calendar_reject_reason::unsupported;
  uint64_t                          version = 0;
  std::string                       content_hash;
  slot_point                        effective_activation_slot;
  uint32_t                          minimum_lead_slots = 0;
  /// True once the target cell slot thread has consumed and armed the prepared command.
  bool                              command_consumed = false;
};

} // namespace srsran
