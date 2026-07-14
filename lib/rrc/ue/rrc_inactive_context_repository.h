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

#include "srsran/cu_cp/cu_cp_types.h"
#include "srsran/cu_cp/cu_cp_ue_messages.h"
#include "srsran/ran/nr_cgi.h"
#include "srsran/ran/rnti.h"
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace srsran {
namespace srs_cu_cp {

/// Stored UE context for an RRC_INACTIVE UE (3GPP TS 38.331 Sec 5.3.13).
///
/// Holds the identifiers needed to look the UE up on a subsequent RRCResumeRequest
/// plus a snapshot of the AS context (security, UP, capabilities, SRB list) that
/// the new rrc_ue_impl object created on resume needs to restore SRB1/SRB2 and
/// active DRBs without re-running RRC Setup or NAS authentication.
///
/// The snapshot is captured via rrc_ue_impl::get_transfer_context() at suspend
/// time and reused on resume in the same way mobility/handover already does.
struct rrc_inactive_ue_context {
  ue_index_t              ue_index{ue_index_t::invalid};
  rnti_t                  old_c_rnti{rnti_t::INVALID_RNTI};
  nr_cell_global_id_t     cell{};
  uint64_t                full_i_rnti             = 0; // 40-bit
  uint32_t                short_i_rnti            = 0; // 24-bit
  uint8_t                 next_hop_chaining_count = 0;
  /// Snapshot of the AS context at suspend time. Empty until populated.
  rrc_ue_transfer_context transfer_context;
};

/// Process-wide repository for RRC_INACTIVE UE contexts stored at the gNB-CU.
/// MVP implementation: in-memory map indexed by I-RNTI. Thread-safe via mutex.
class rrc_inactive_context_repository
{
public:
  static rrc_inactive_context_repository& get_instance()
  {
    static rrc_inactive_context_repository instance;
    return instance;
  }

  /// Allocate a fresh 40-bit Full I-RNTI (sequential, wraps below 2^40, never 0).
  uint64_t allocate_full_i_rnti()
  {
    std::lock_guard<std::mutex> lock(mtx);
    uint64_t                    v = next_i_rnti;
    next_i_rnti                   = (next_i_rnti + 1) & 0xFFFFFFFFFFULL;
    if (next_i_rnti == 0) {
      next_i_rnti = 1;
    }
    return v;
  }

  /// Derive the Short I-RNTI (24-bit) by taking the lower 24 bits of the Full I-RNTI.
  static uint32_t derive_short_i_rnti(uint64_t full_i_rnti)
  {
    return static_cast<uint32_t>(full_i_rnti & 0xFFFFFFULL);
  }

  /// Store a suspended UE context. Overwrites any existing entry with the same I-RNTI.
  /// Takes the context by value so callers can std::move() into it; required because
  /// the embedded byte_buffer fields have deleted copy assignment (only emplace works).
  void store(rrc_inactive_ue_context ctx)
  {
    std::lock_guard<std::mutex> lock(mtx);
    uint64_t                    full = ctx.full_i_rnti;
    uint32_t                    shrt = ctx.short_i_rnti;
    by_full.erase(full);
    by_full.emplace(full, std::move(ctx));
    by_short[shrt] = full;
  }

  /// Look up by Full I-RNTI (40-bit, sent on UL-CCCH1 RRCResumeRequest1).
  std::optional<rrc_inactive_ue_context> lookup_full(uint64_t full_i_rnti) const
  {
    std::lock_guard<std::mutex> lock(mtx);
    auto                        it = by_full.find(full_i_rnti);
    if (it == by_full.end()) {
      return std::nullopt;
    }
    return it->second;
  }

  /// Look up by Short I-RNTI (24-bit, sent on UL-CCCH RRCResumeRequest).
  std::optional<rrc_inactive_ue_context> lookup_short(uint32_t short_i_rnti) const
  {
    std::lock_guard<std::mutex> lock(mtx);
    auto                        it = by_short.find(short_i_rnti);
    if (it == by_short.end()) {
      return std::nullopt;
    }
    auto fit = by_full.find(it->second);
    if (fit == by_full.end()) {
      return std::nullopt;
    }
    return fit->second;
  }

  /// Remove a stored inactive context by Full I-RNTI.
  bool remove(uint64_t full_i_rnti)
  {
    std::lock_guard<std::mutex> lock(mtx);
    auto                        it = by_full.find(full_i_rnti);
    if (it == by_full.end()) {
      return false;
    }
    by_short.erase(it->second.short_i_rnti);
    by_full.erase(it);
    return true;
  }

  size_t size() const
  {
    std::lock_guard<std::mutex> lock(mtx);
    return by_full.size();
  }

private:
  rrc_inactive_context_repository() = default;

  mutable std::mutex                                    mtx;
  uint64_t                                              next_i_rnti = 1;
  std::unordered_map<uint64_t, rrc_inactive_ue_context> by_full;
  std::unordered_map<uint32_t, uint64_t>                by_short;
};

} // namespace srs_cu_cp
} // namespace srsran
