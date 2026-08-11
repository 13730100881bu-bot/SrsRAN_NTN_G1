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

#include "context_repository_helpers.h"
#include "srsran/ofh/ofh_constants.h"
#include "srsran/ofh/serdes/ofh_cplane_message_properties.h"
#include "srsran/ran/slot_pdu_capacity_constants.h"
#include "srsran/support/srsran_assert.h"
#include <array>
#include <atomic>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace srsran {
namespace ofh {

/// Uplink Control-Plane context.
struct ul_cplane_context {
  /// Filter index.
  filter_index_type filter_index;
  /// Start symbol identifier.
  uint8_t start_symbol;
  /// Starting PRB of data section.
  uint16_t prb_start;
  /// Number of contiguous PRBs per data section.
  uint16_t nof_prb;
  /// Number of symbols.
  uint8_t nof_symbols;
};

/// Exact PRACH Control-Plane context retained for matching a received U-Plane eAxC.
struct prach_cplane_context {
  /// Slot for which this context was created.
  slot_point slot;
  /// Expected PRACH U-Plane eAxC.
  unsigned eaxc;
  /// Radio fields used for regular C-Plane/U-Plane validation.
  ul_cplane_context radio_context;
  /// Optional authoritative beam and calendar context. Absence preserves the legacy PRACH path.
  std::optional<prach_beam_context> beam_context;
};

/// Uplink Control-Plane context repository.
class uplink_cplane_context_repository
{
  using repo_entry = std::array<std::atomic<uint64_t>, MAX_SUPPORTED_EAXC_ID_VALUE>;

  struct exact_prach_entry {
    std::optional<prach_cplane_context> context;
    std::optional<slot_point>           ambiguous_slot;
  };
  using exact_prach_repo_entry = std::array<exact_prach_entry, MAX_SUPPORTED_EAXC_ID_VALUE>;

  /// Repository storage.
  std::vector<repo_entry> repo;
  /// Exact PRACH identities and optional beam context. Used only by PRACH repository instances.
  std::vector<exact_prach_repo_entry> exact_prach_repo;
  mutable std::mutex                  exact_prach_mutex;

  /// Returns the entry of the repository for the given slot and eAxC.
  std::atomic<uint64_t>& get_entry(slot_point slot, unsigned eaxc)
  {
    unsigned index = calculate_repository_index(slot, repo.size());
    return repo[index][eaxc];
  }

  /// Returns the entry of the repository for the given slot and eAxC.
  const std::atomic<uint64_t>& get_entry(slot_point slot, unsigned eaxc) const
  {
    unsigned index = calculate_repository_index(slot, repo.size());
    return repo[index][eaxc];
  }

  /// Packs the given context.
  static uint64_t pack_context(ul_cplane_context context)
  {
    uint64_t data = 0;
    data |= static_cast<uint8_t>(context.filter_index);
    data |= static_cast<uint64_t>(context.start_symbol) << 8;
    data |= static_cast<uint64_t>(context.prb_start) << 16;
    data |= static_cast<uint64_t>(context.nof_prb) << 32;
    data |= static_cast<uint64_t>(context.nof_symbols) << 48;

    return data;
  }

  /// Unpacks the given packed context.
  static ul_cplane_context unpack_context(uint64_t data)
  {
    return {static_cast<filter_index_type>(data),
            static_cast<uint8_t>(data >> 8),
            static_cast<uint16_t>(data >> 16),
            static_cast<uint16_t>(data >> 32),
            static_cast<uint8_t>(data >> 48)};
  }

  static bool contexts_match(const prach_cplane_context& lhs, const prach_cplane_context& rhs)
  {
    const auto& lhs_radio = lhs.radio_context;
    const auto& rhs_radio = rhs.radio_context;
    return lhs.slot == rhs.slot && lhs.eaxc == rhs.eaxc && lhs_radio.filter_index == rhs_radio.filter_index &&
           lhs_radio.start_symbol == rhs_radio.start_symbol && lhs_radio.prb_start == rhs_radio.prb_start &&
           lhs_radio.nof_prb == rhs_radio.nof_prb && lhs_radio.nof_symbols == rhs_radio.nof_symbols &&
           lhs.beam_context == rhs.beam_context;
  }

public:
  explicit uplink_cplane_context_repository(unsigned size_) : repo(size_), exact_prach_repo(size_)
  {
    static_assert(MAX_PRACH_OCCASIONS_PER_SLOT == 1,
                  "Uplink Control-Plane context repository only supports one context per slot and eAxC");
  }

  /// Add the given context to the repo at the given slot and eAxC.
  void add(slot_point slot, unsigned eaxc, ul_cplane_context new_context)
  {
    auto& entry = get_entry(slot, eaxc);
    entry.store(pack_context(new_context), std::memory_order_relaxed);
  }

  /// Adds an exact PRACH context. Conflicting duplicates for the same slot/eAxC make that identity ambiguous.
  bool add_prach(slot_point                         slot,
                 unsigned                           eaxc,
                 ul_cplane_context                  radio_context,
                 std::optional<prach_beam_context> beam_context)
  {
    srsran_assert(eaxc < MAX_SUPPORTED_EAXC_ID_VALUE, "Invalid PRACH eAxC={}", eaxc);

    std::lock_guard<std::mutex> lock(exact_prach_mutex);
    unsigned                    index = calculate_repository_index(slot, exact_prach_repo.size());
    auto&                       entry = exact_prach_repo[index][eaxc];
    prach_cplane_context        new_context{slot, eaxc, radio_context, std::move(beam_context)};

    if (entry.ambiguous_slot && *entry.ambiguous_slot == slot) {
      return false;
    }
    if (entry.context && entry.context->slot == slot) {
      if (contexts_match(*entry.context, new_context)) {
        return true;
      }
      entry.context.reset();
      entry.ambiguous_slot = slot;
      get_entry(slot, eaxc).store(0, std::memory_order_relaxed);
      return false;
    }

    entry.context        = std::move(new_context);
    entry.ambiguous_slot = std::nullopt;
    get_entry(slot, eaxc).store(pack_context(radio_context), std::memory_order_relaxed);
    return true;
  }

  /// Returns a context that matches the given slot and eAxC.
  ul_cplane_context get(slot_point slot, unsigned eaxc) const
  {
    const auto& entry = get_entry(slot, eaxc);
    return unpack_context(entry.load(std::memory_order_relaxed));
  }

  /// Returns the exact PRACH context for the given slot/eAxC, or no value for missing, stale or ambiguous entries.
  std::optional<prach_cplane_context> get_prach(slot_point slot, unsigned eaxc) const
  {
    if (eaxc >= MAX_SUPPORTED_EAXC_ID_VALUE) {
      return std::nullopt;
    }

    std::lock_guard<std::mutex> lock(exact_prach_mutex);
    unsigned                    index = calculate_repository_index(slot, exact_prach_repo.size());
    const auto&                 entry = exact_prach_repo[index][eaxc];
    if (entry.ambiguous_slot && *entry.ambiguous_slot == slot) {
      return std::nullopt;
    }
    if (!entry.context || entry.context->slot != slot || entry.context->eaxc != eaxc) {
      return std::nullopt;
    }
    return entry.context;
  }
};

} // namespace ofh
} // namespace srsran
