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

#include "rnti_value_table.h"
#include "srsran/mac/mac_manager.h"
#include "srsran/ran/du_types.h"
#include "srsran/ran/rnti.h"
#include <algorithm>
#include <chrono>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <vector>

namespace srsran {

using du_rnti_table = rnti_value_table<du_ue_index_t, du_ue_index_t::INVALID_DU_UE_INDEX>;

/// Result of a cell-aware C-RNTI allocation.
///
/// A non-zero generation identifies an NTN lease. Terrestrial allocations intentionally use generation zero because
/// they are not backed by the versioned NTN lease ledger. Failed allocations return INVALID_RNTI and generation zero.
struct rnti_allocation_result {
  rnti_t   rnti       = rnti_t::INVALID_RNTI;
  uint32_t generation = 0;
};

/// \brief Extends DU RNTI Table with the ability to allocate unique RNTIs for UEs.
class rnti_manager : public du_rnti_table
{
public:
  using ntn_lease_clock      = std::chrono::steady_clock;
  using ntn_lease_time_point = ntn_lease_clock::time_point;

  rnti_manager(rnti_t initial_rnti = to_rnti(0x4601)) :
    rnti_counter(to_value(initial_rnti) - to_value(rnti_t::MIN_CRNTI))
  {
    srsran_assert(is_crnti(initial_rnti), "Invalid initial c-rnti={}", initial_rnti);
  }

  /// \brief Allocates new unique TC-RNTI.
  rnti_t allocate() { return allocate(ntn_lease_clock::now()); }

  /// Deterministic-time overload used to test NTN/terrestrial allocation interlocks.
  rnti_t allocate(ntn_lease_time_point now)
  {
    std::lock_guard<std::mutex> lock(ntn_lease_mutex);
    return allocate_terrestrial_locked(now);
  }

  /// Atomically chooses the cell's authoritative NTN pool or the terrestrial allocator.
  rnti_t allocate_for_cell(du_cell_index_t cell_index)
  {
    return allocate_for_cell_with_generation(cell_index, ntn_lease_clock::now()).rnti;
  }

  /// Deterministic-time overload used to verify atomic lease-mode selection.
  rnti_t allocate_for_cell(du_cell_index_t cell_index, ntn_lease_time_point now)
  {
    return allocate_for_cell_with_generation(cell_index, now).rnti;
  }

  /// Atomically allocates a C-RNTI and returns its NTN lease generation when applicable.
  rnti_allocation_result allocate_for_cell_with_generation(du_cell_index_t cell_index)
  {
    return allocate_for_cell_with_generation(cell_index, ntn_lease_clock::now());
  }

  /// Deterministic-time overload used to verify generation-preserving NTN lease allocation.
  rnti_allocation_result allocate_for_cell_with_generation(du_cell_index_t cell_index, ntn_lease_time_point now)
  {
    std::lock_guard<std::mutex> lock(ntn_lease_mutex);
    if (!is_ntn_rnti_lease_mode_enabled_locked(cell_index)) {
      return {allocate_terrestrial_locked(now), 0};
    }
    rnti_allocation_result result = allocate_ntn_lease_with_generation_locked(cell_index, now);
    if (result.rnti != rnti_t::INVALID_RNTI || cell_index == INVALID_DU_CELL_INDEX) {
      return result;
    }
    return allocate_ntn_lease_with_generation_locked(INVALID_DU_CELL_INDEX, now);
  }

  /// Associates an RNTI with a UE while closing any outstanding terrestrial RACH reservation.
  bool add_ue(rnti_t crnti, du_ue_index_t ue_index)
  {
    std::lock_guard<std::mutex> lock(ntn_lease_mutex);
    const bool                  added = du_rnti_table::add_ue(crnti, ue_index);
    if (!added) {
      return false;
    }
    terrestrial_rnti_reservations.erase(crnti);
    for (auto& cell_leases : ntn_rnti_leases) {
      for (ntn_rnti_lease_record& lease : cell_leases.second) {
        if (lease.rnti == crnti && lease.state == ntn_rnti_lease_state::pending) {
          lease.state = ntn_rnti_lease_state::consumed_by_mac;
        }
      }
    }
    return true;
  }

  void rem_ue(rnti_t crnti)
  {
    std::lock_guard<std::mutex> lock(ntn_lease_mutex);
    du_rnti_table::rem_ue(crnti);
  }

  /// Adds an NTN CU-CP-provided RNTI lease to the local consumption pool.
  bool add_ntn_rnti_lease(rnti_t rnti)
  {
    return add_ntn_rnti_lease(INVALID_DU_CELL_INDEX, rnti);
  }

  /// Adds an NTN CU-CP-provided RNTI lease to the local per-cell consumption pool.
  bool add_ntn_rnti_lease(du_cell_index_t cell_index, rnti_t rnti)
  {
    const ntn_lease_time_point now = ntn_lease_clock::now();
    return add_ntn_rnti_lease_record(cell_index, rnti, 1, ntn_lease_time_point::max(), now);
  }

  /// Adds a bounded-lifetime NTN lease received in one versioned CU-CP update.
  bool add_ntn_rnti_lease(du_cell_index_t      cell_index,
                          rnti_t               rnti,
                          uint32_t             generation_id,
                          uint32_t             expiry_ms,
                          ntn_lease_time_point now = ntn_lease_clock::now())
  {
    if (cell_index >= MAX_NOF_DU_CELLS || generation_id == 0 || expiry_ms == 0) {
      return false;
    }
    return add_ntn_rnti_lease_record(cell_index, rnti, generation_id, now + std::chrono::milliseconds{expiry_ms}, now);
  }

  /// Validates and atomically applies a complete MAC-facing NTN lease-pool update.
  mac_ntn_rnti_lease_pool_result apply_ntn_rnti_lease_pool_update(const mac_ntn_rnti_lease_pool_update& request,
                                                                  ntn_lease_time_point now = ntn_lease_clock::now())
  {
    mac_ntn_rnti_lease_pool_result result;
    if (request.cell_index >= MAX_NOF_DU_CELLS) {
      result.reason = "invalid_cell";
      return result;
    }
    if (request.operation != mac_ntn_rnti_lease_pool_operation::clear && request.generation_id == 0) {
      result.reason = "invalid_generation";
      return result;
    }
    if ((request.operation == mac_ntn_rnti_lease_pool_operation::replace ||
         request.operation == mac_ntn_rnti_lease_pool_operation::add) &&
        request.expiry_ms == 0) {
      result.reason = "invalid_expiry";
      return result;
    }
    if (request.operation == mac_ntn_rnti_lease_pool_operation::retire && request.expiry_ms != 0) {
      result.reason = "invalid_expiry";
      return result;
    }
    if (request.operation == mac_ntn_rnti_lease_pool_operation::retire && request.leases.empty()) {
      result.reason = "empty_retirement";
      return result;
    }

    std::lock_guard<std::mutex> lock(ntn_lease_mutex);
    purge_expired_terrestrial_rnti_reservations_locked(now);
    refresh_ntn_rnti_lease_states_locked(request.cell_index, now);
    if (request.operation == mac_ntn_rnti_lease_pool_operation::clear) {
      const auto cell_generation = ntn_cell_generation_high_water.find(request.cell_index);
      if (request.generation_id == 0) {
        if (cell_generation != ntn_cell_generation_high_water.end() ||
            ntn_rnti_leases.find(request.cell_index) != ntn_rnti_leases.end()) {
          result.reason = "invalid_generation";
          return result;
        }
        ntn_cell_lease_mode_enabled[request.cell_index] = false;
        result.accepted                                 = true;
        result.reason                                   = "accepted";
        return result;
      }
      if (cell_generation == ntn_cell_generation_high_water.end()) {
        result.reason = "lease_not_found";
        return result;
      }
      if (request.generation_id < cell_generation->second) {
        result.reason = "stale_generation";
        return result;
      }
      if (request.generation_id != cell_generation->second) {
        result.reason = "generation_mismatch";
        return result;
      }
      retain_cell_tombstones_locked(request.cell_index);
      ntn_rnti_leases.erase(request.cell_index);
      ntn_cell_lease_mode_enabled[request.cell_index] = false;
      result.accepted                                 = true;
      result.reason                                   = "accepted";
      return result;
    }
    if (request.operation == mac_ntn_rnti_lease_pool_operation::retire) {
      return retire_ntn_rnti_leases_locked(request);
    }

    std::vector<ntn_rnti_lease_record> replacement;
    replacement.reserve(request.leases.size());
    std::set<rnti_t> request_rntis;
    const auto existing_it = ntn_rnti_leases.find(request.cell_index);
    for (rnti_t rnti : request.leases) {
      const auto existing_record = existing_it == ntn_rnti_leases.end()
                                       ? std::vector<ntn_rnti_lease_record>::const_iterator{}
                                       : std::find_if(existing_it->second.cbegin(),
                                                      existing_it->second.cend(),
                                                      [rnti](const auto& lease) { return lease.rnti == rnti; });
      const bool duplicate_in_request = !request_rntis.emplace(rnti).second;
      const bool exists_in_cell = existing_it != ntn_rnti_leases.end() &&
                                  existing_record != existing_it->second.cend();
      // A retry of the exact generation is a no-op even when MAC has already consumed or expired the lease.
      // This acknowledges the original update without refreshing expiry or resurrecting terminal state.
      const bool idempotent_add = request.operation == mac_ntn_rnti_lease_pool_operation::add && exists_in_cell &&
                                  existing_record->generation_id == request.generation_id;
      const auto tombstone_it = ntn_rnti_retired_generations.find(rnti);
      const bool stale_generation =
          !idempotent_add &&
          ((tombstone_it != ntn_rnti_retired_generations.end() && request.generation_id <= tombstone_it->second) ||
           request.generation_id <= ntn_rnti_generation_high_water);
      const bool already_tracked =
          request.operation == mac_ntn_rnti_lease_pool_operation::add && exists_in_cell && !idempotent_add;
      const bool replaces_terminal =
          request.operation == mac_ntn_rnti_lease_pool_operation::replace && exists_in_cell &&
          existing_record->state != ntn_rnti_lease_state::pending;
      const bool tracked_in_other_cell = has_tracked_ntn_rnti_in_other_cell_locked(request.cell_index, rnti);
      const bool terrestrial_rnti_pending =
          terrestrial_rnti_reservations.find(rnti) != terrestrial_rnti_reservations.end();
      if (!is_crnti(rnti) || duplicate_in_request || stale_generation || already_tracked || replaces_terminal ||
          tracked_in_other_cell || terrestrial_rnti_pending || (this->has_rnti(rnti) && !idempotent_add)) {
        result.rejected_leases = request.leases;
        result.reason          = !is_crnti(rnti)            ? "invalid_rnti"
                                 : duplicate_in_request     ? "duplicate_lease"
                                 : stale_generation         ? "stale_generation"
                                 : already_tracked          ? "duplicate_lease"
                                 : replaces_terminal        ? "terminal_lease"
                                 : tracked_in_other_cell    ? "rnti_tracked_in_other_cell"
                                 : terrestrial_rnti_pending ? "terrestrial_rnti_pending"
                                                            : "active_rnti";
        return result;
      }
      if (!idempotent_add) {
        replacement.push_back({rnti,
                               request.generation_id,
                               now + std::chrono::milliseconds{request.expiry_ms},
                               ntn_rnti_lease_state::pending});
      }
    }

    if (request.operation == mac_ntn_rnti_lease_pool_operation::replace) {
      if (existing_it != ntn_rnti_leases.end()) {
        for (const ntn_rnti_lease_record& existing : existing_it->second) {
          if (existing.state != ntn_rnti_lease_state::pending) {
            replacement.push_back(existing);
          } else if (request_rntis.find(existing.rnti) == request_rntis.end()) {
            retain_tombstone_locked(existing.rnti, existing.generation_id);
          }
        }
      }
      ntn_rnti_leases[request.cell_index] = std::move(replacement);
    } else {
      auto& records = ntn_rnti_leases[request.cell_index];
      records.insert(records.end(), replacement.begin(), replacement.end());
    }
    ntn_cell_lease_mode_enabled[request.cell_index] = true;
    ntn_cell_generation_high_water[request.cell_index] =
        std::max(ntn_cell_generation_high_water[request.cell_index], request.generation_id);
    ntn_rnti_generation_high_water                  = std::max(ntn_rnti_generation_high_water, request.generation_id);
    result.accepted_leases                          = request.leases;
    result.accepted                                 = true;
    result.reason                                   = "accepted";
    return result;
  }

  /// Consumes one NTN CU-CP-provided RNTI lease. Does not fall back to the terrestrial allocator.
  rnti_t allocate_ntn_lease() { return allocate_ntn_lease(INVALID_DU_CELL_INDEX); }

  /// Consumes one NTN CU-CP-provided RNTI lease for a cell. Does not fall back to the terrestrial allocator.
  rnti_t allocate_ntn_lease(du_cell_index_t cell_index)
  {
    return allocate_ntn_lease(cell_index, ntn_lease_clock::now());
  }

  /// Deterministic-time overload used by focused lease lifecycle tests.
  rnti_t allocate_ntn_lease(du_cell_index_t cell_index, ntn_lease_time_point now)
  {
    std::lock_guard<std::mutex> lock(ntn_lease_mutex);
    rnti_allocation_result      result = allocate_ntn_lease_with_generation_locked(cell_index, now);
    if (result.rnti != rnti_t::INVALID_RNTI || cell_index == INVALID_DU_CELL_INDEX) {
      return result.rnti;
    }
    return allocate_ntn_lease_with_generation_locked(INVALID_DU_CELL_INDEX, now).rnti;
  }

  /// Clears the full pending/consumed/expired history for one cell.
  void clear_ntn_rnti_leases(du_cell_index_t cell_index)
  {
    std::lock_guard<std::mutex> lock(ntn_lease_mutex);
    retain_cell_tombstones_locked(cell_index);
    ntn_rnti_leases.erase(cell_index);
  }

  void set_ntn_rnti_lease_mode(bool enabled) { ntn_rnti_lease_mode_enabled.store(enabled, std::memory_order_relaxed); }

  void set_ntn_rnti_lease_mode(du_cell_index_t cell_index, bool enabled)
  {
    std::lock_guard<std::mutex> lock(ntn_lease_mutex);
    ntn_cell_lease_mode_enabled[cell_index] = enabled;
  }

  bool is_ntn_rnti_lease_mode_enabled() const
  {
    return ntn_rnti_lease_mode_enabled.load(std::memory_order_relaxed);
  }

  bool is_ntn_rnti_lease_mode_enabled(du_cell_index_t cell_index) const
  {
    std::lock_guard<std::mutex> lock(ntn_lease_mutex);
    return is_ntn_rnti_lease_mode_enabled_locked(cell_index);
  }

  unsigned nof_ntn_rnti_leases() const
  {
    return nof_ntn_rnti_leases(INVALID_DU_CELL_INDEX);
  }

  unsigned nof_ntn_rnti_leases(du_cell_index_t cell_index) const
  {
    return nof_ntn_rnti_leases(cell_index, ntn_lease_clock::now());
  }

  /// Returns the number of currently pending leases at an explicit monotonic time.
  unsigned nof_ntn_rnti_leases(du_cell_index_t cell_index, ntn_lease_time_point now) const
  {
    std::lock_guard<std::mutex> lock(ntn_lease_mutex);
    refresh_ntn_rnti_lease_states_locked(cell_index, now);
    auto                        it = ntn_rnti_leases.find(cell_index);
    if (it == ntn_rnti_leases.end()) {
      return 0;
    }
    return static_cast<unsigned>(std::count_if(it->second.begin(), it->second.end(), [](const auto& lease) {
      return lease.state == ntn_rnti_lease_state::pending;
    }));
  }

  mac_ntn_rnti_lease_pool_snapshot get_ntn_rnti_lease_pool_snapshot(du_cell_index_t cell_index)
  {
    return get_ntn_rnti_lease_pool_snapshot(cell_index, ntn_lease_clock::now());
  }

  /// Deterministic-time overload used to audit expiry without wall-clock sleeps.
  mac_ntn_rnti_lease_pool_snapshot get_ntn_rnti_lease_pool_snapshot(du_cell_index_t      cell_index,
                                                                    ntn_lease_time_point now)
  {
    mac_ntn_rnti_lease_pool_snapshot result;
    result.cell_index = cell_index;
    if (cell_index == INVALID_DU_CELL_INDEX || cell_index >= MAX_NOF_DU_CELLS) {
      return result;
    }

    const bool                  global_mode = ntn_rnti_lease_mode_enabled.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(ntn_lease_mutex);
    refresh_ntn_rnti_lease_states_locked(cell_index, now);
    result.complete           = true;
    result.retirement_supported       = true;
    result.rnti_generation_high_water = ntn_rnti_generation_high_water;
    auto mode_it              = ntn_cell_lease_mode_enabled.find(cell_index);
    result.lease_mode_enabled = global_mode || (mode_it != ntn_cell_lease_mode_enabled.end() && mode_it->second);

    auto leases_it = ntn_rnti_leases.find(cell_index);
    if (leases_it == ntn_rnti_leases.end()) {
      return result;
    }
    result.leases.reserve(leases_it->second.size());
    for (const ntn_rnti_lease_record& lease : leases_it->second) {
      mac_ntn_rnti_lease_snapshot_entry& snapshot = result.leases.emplace_back();
      snapshot.rnti                               = lease.rnti;
      snapshot.generation_id                      = lease.generation_id;
      snapshot.state                              = to_string(lease.state);
      snapshot.distribution_state = lease.state == ntn_rnti_lease_state::expired ? "expired_by_du" : "applied_by_du";
    }
    return result;
  }

private:
  static constexpr std::chrono::seconds terrestrial_rnti_reservation_guard{10};

  enum class ntn_rnti_lease_state { pending, consumed_by_mac, expired };

  struct ntn_rnti_lease_record {
    rnti_t               rnti          = rnti_t::INVALID_RNTI;
    uint32_t             generation_id = 0;
    ntn_lease_time_point expires_at{};
    ntn_rnti_lease_state state = ntn_rnti_lease_state::pending;
  };

  static const char* to_string(ntn_rnti_lease_state state)
  {
    switch (state) {
      case ntn_rnti_lease_state::pending:
        return "pending";
      case ntn_rnti_lease_state::consumed_by_mac:
        return "consumed_by_mac";
      case ntn_rnti_lease_state::expired:
        return "expired";
    }
    return "expired";
  }

  bool has_tracked_ntn_rnti_locked(rnti_t rnti) const
  {
    return std::any_of(ntn_rnti_leases.begin(), ntn_rnti_leases.end(), [rnti](const auto& cell_leases) {
      return std::any_of(cell_leases.second.begin(), cell_leases.second.end(), [rnti](const auto& lease) {
        return lease.rnti == rnti;
      });
    });
  }

  bool has_tracked_ntn_rnti_in_other_cell_locked(du_cell_index_t cell_index, rnti_t rnti) const
  {
    return std::any_of(ntn_rnti_leases.begin(), ntn_rnti_leases.end(), [cell_index, rnti](const auto& cell_leases) {
      return cell_leases.first != cell_index &&
             std::any_of(cell_leases.second.begin(), cell_leases.second.end(), [rnti](const auto& lease) {
               return lease.rnti == rnti;
             });
    });
  }

  void retain_tombstone_locked(rnti_t rnti, uint32_t generation_id)
  {
    auto [it, inserted] = ntn_rnti_retired_generations.emplace(rnti, generation_id);
    if (!inserted) {
      it->second = std::max(it->second, generation_id);
    }
    ntn_rnti_generation_high_water = std::max(ntn_rnti_generation_high_water, generation_id);
  }

  void retain_cell_tombstones_locked(du_cell_index_t cell_index)
  {
    const auto records_it = ntn_rnti_leases.find(cell_index);
    if (records_it == ntn_rnti_leases.end()) {
      return;
    }
    for (const ntn_rnti_lease_record& record : records_it->second) {
      retain_tombstone_locked(record.rnti, record.generation_id);
    }
  }

  mac_ntn_rnti_lease_pool_result retire_ntn_rnti_leases_locked(const mac_ntn_rnti_lease_pool_update& request)
  {
    mac_ntn_rnti_lease_pool_result result;
    std::set<rnti_t>               request_rntis;
    const auto                     records_it = ntn_rnti_leases.find(request.cell_index);

    // Validate the complete batch before modifying either the active ledger or the replay tombstones.
    for (rnti_t rnti : request.leases) {
      if (!is_crnti(rnti)) {
        result.reason = "invalid_rnti";
      } else if (!request_rntis.emplace(rnti).second) {
        result.reason = "duplicate_lease";
      } else if (this->has_rnti(rnti)) {
        result.reason = "active_rnti";
      } else if (terrestrial_rnti_reservations.find(rnti) != terrestrial_rnti_reservations.end()) {
        result.reason = "terrestrial_rnti_pending";
      } else if (has_tracked_ntn_rnti_in_other_cell_locked(request.cell_index, rnti)) {
        result.reason = "rnti_tracked_in_other_cell";
      }
      if (!result.reason.empty()) {
        result.rejected_leases = request.leases;
        return result;
      }

      const auto record_it  = records_it == ntn_rnti_leases.end()
                                  ? std::vector<ntn_rnti_lease_record>::const_iterator{}
                                  : std::find_if(records_it->second.cbegin(),
                                                 records_it->second.cend(),
                                                 [rnti](const auto& record) { return record.rnti == rnti; });
      const bool has_record = records_it != ntn_rnti_leases.end() && record_it != records_it->second.cend();
      if (has_record) {
        if (request.generation_id < record_it->generation_id) {
          result.reason = "stale_generation";
        } else if (request.generation_id != record_it->generation_id) {
          result.reason = "generation_mismatch";
        } else if (record_it->state != ntn_rnti_lease_state::expired) {
          result.reason = "lease_not_expired";
        }
      } else {
        const auto tombstone_it = ntn_rnti_retired_generations.find(rnti);
        if (tombstone_it == ntn_rnti_retired_generations.end()) {
          result.reason = "lease_not_found";
        } else if (request.generation_id < tombstone_it->second) {
          result.reason = "stale_generation";
        } else if (request.generation_id != tombstone_it->second) {
          result.reason = "generation_mismatch";
        }
      }
      if (!result.reason.empty()) {
        result.rejected_leases = request.leases;
        return result;
      }
    }

    if (records_it != ntn_rnti_leases.end()) {
      auto& records = records_it->second;
      records.erase(std::remove_if(records.begin(),
                                   records.end(),
                                   [&request_rntis, &request](const ntn_rnti_lease_record& record) {
                                     return request_rntis.find(record.rnti) != request_rntis.end() &&
                                            record.generation_id == request.generation_id;
                                   }),
                    records.end());
      if (records.empty()) {
        ntn_rnti_leases.erase(records_it);
      }
    }
    for (rnti_t rnti : request.leases) {
      retain_tombstone_locked(rnti, request.generation_id);
    }
    result.accepted        = true;
    result.reason          = "accepted";
    result.accepted_leases = request.leases;
    return result;
  }

  bool is_ntn_rnti_lease_mode_enabled_locked(du_cell_index_t cell_index) const
  {
    if (ntn_rnti_lease_mode_enabled.load(std::memory_order_relaxed)) {
      return true;
    }
    auto it = ntn_cell_lease_mode_enabled.find(cell_index);
    return it != ntn_cell_lease_mode_enabled.end() && it->second;
  }

  bool has_active_ntn_interlock_locked() const
  {
    return ntn_rnti_lease_mode_enabled.load(std::memory_order_relaxed) || !ntn_rnti_leases.empty() ||
           std::any_of(ntn_cell_lease_mode_enabled.begin(),
                       ntn_cell_lease_mode_enabled.end(),
                       [](const auto& entry) { return entry.second; });
  }

  void purge_expired_terrestrial_rnti_reservations_locked(ntn_lease_time_point now)
  {
    for (auto it = terrestrial_rnti_reservations.begin(); it != terrestrial_rnti_reservations.end();) {
      if (it->second <= now) {
        it = terrestrial_rnti_reservations.erase(it);
      } else {
        ++it;
      }
    }
  }

  bool add_ntn_rnti_lease_record(du_cell_index_t      cell_index,
                                 rnti_t               rnti,
                                 uint32_t             generation_id,
                                 ntn_lease_time_point expires_at,
                                 ntn_lease_time_point now)
  {
    if (!is_crnti(rnti)) {
      return false;
    }

    std::lock_guard<std::mutex> lock(ntn_lease_mutex);
    purge_expired_terrestrial_rnti_reservations_locked(now);
    const auto tombstone_it = ntn_rnti_retired_generations.find(rnti);
    if (this->has_rnti(rnti) || has_tracked_ntn_rnti_locked(rnti) ||
        (tombstone_it != ntn_rnti_retired_generations.end() && generation_id <= tombstone_it->second) ||
        terrestrial_rnti_reservations.find(rnti) != terrestrial_rnti_reservations.end()) {
      return false;
    }
    auto& records = ntn_rnti_leases[cell_index];
    if (std::find_if(records.begin(), records.end(), [rnti](const auto& record) { return record.rnti == rnti; }) !=
        records.end()) {
      return false;
    }
    records.push_back({rnti, generation_id, expires_at, ntn_rnti_lease_state::pending});
    ntn_cell_generation_high_water[cell_index] = std::max(ntn_cell_generation_high_water[cell_index], generation_id);
    ntn_rnti_generation_high_water             = std::max(ntn_rnti_generation_high_water, generation_id);
    return true;
  }

  void refresh_ntn_rnti_lease_states_locked(du_cell_index_t cell_index, ntn_lease_time_point now) const
  {
    auto it = ntn_rnti_leases.find(cell_index);
    if (it == ntn_rnti_leases.end()) {
      return;
    }
    for (ntn_rnti_lease_record& lease : it->second) {
      if (lease.state != ntn_rnti_lease_state::expired && lease.expires_at <= now) {
        lease.state = ntn_rnti_lease_state::expired;
      } else if (lease.state == ntn_rnti_lease_state::pending && this->has_rnti(lease.rnti)) {
        // A requested C-RNTI can enter the base DU table through UE setup without calling allocate_ntn_lease().
        lease.state = ntn_rnti_lease_state::consumed_by_mac;
      }
    }
  }

  rnti_t allocate_terrestrial_locked(ntn_lease_time_point now)
  {
    if (this->nof_ues() >= MAX_NOF_DU_UES) {
      return rnti_t::INVALID_RNTI;
    }
    const bool ntn_interlock_active = has_active_ntn_interlock_locked();
    if (ntn_interlock_active) {
      purge_expired_terrestrial_rnti_reservations_locked(now);
    }
    // Before NTN is enabled, reservations are recorded only so a concurrent first pool update can reject a collision;
    // they do not alter the original terrestrial allocation sequence. Once NTN is active, both tracked leases and
    // outstanding terrestrial TC-RNTIs are excluded from allocation.
    for (unsigned nof_attempts = 0; nof_attempts != CRNTI_RANGE; ++nof_attempts) {
      uint16_t     prev_counter = rnti_counter.fetch_add(1, std::memory_order_relaxed) % CRNTI_RANGE;
      const rnti_t temp_crnti   = to_rnti(prev_counter + to_value(rnti_t::MIN_CRNTI));
      if (!this->has_rnti(temp_crnti) &&
          (!ntn_interlock_active || (!has_tracked_ntn_rnti_locked(temp_crnti) &&
                                    terrestrial_rnti_reservations.find(temp_crnti) ==
                                        terrestrial_rnti_reservations.end()))) {
        terrestrial_rnti_reservations.insert_or_assign(temp_crnti, now + terrestrial_rnti_reservation_guard);
        return temp_crnti;
      }
    }
    return rnti_t::INVALID_RNTI;
  }

  rnti_allocation_result allocate_ntn_lease_with_generation_locked(du_cell_index_t cell_index,
                                                                    ntn_lease_time_point now)
  {
    refresh_ntn_rnti_lease_states_locked(cell_index, now);
    auto it = ntn_rnti_leases.find(cell_index);
    if (it == ntn_rnti_leases.end()) {
      return {};
    }
    if (this->nof_ues() >= MAX_NOF_DU_UES) {
      return {};
    }
    for (ntn_rnti_lease_record& lease : it->second) {
      if (lease.state != ntn_rnti_lease_state::pending) {
        continue;
      }
      if (this->has_rnti(lease.rnti)) {
        continue;
      }
      // UINT32_MAX cannot be followed by a strictly newer generation, so consuming it would make safe reuse
      // impossible. Keep the lease pending and fail closed.
      if (lease.generation_id == std::numeric_limits<uint32_t>::max()) {
        continue;
      }
      lease.state = ntn_rnti_lease_state::consumed_by_mac;
      return {lease.rnti, lease.generation_id};
    }
    return {};
  }

  static constexpr int CRNTI_RANGE = to_value(rnti_t::MAX_CRNTI) + 1 - to_value(rnti_t::MIN_CRNTI);

  std::atomic<std::underlying_type_t<rnti_t>> rnti_counter;
  std::atomic<bool>                           ntn_rnti_lease_mode_enabled{false};
  mutable std::mutex                          ntn_lease_mutex;
  // Live/expired records remain visible to audit until an explicit atomic retirement. Compact per-RNTI tombstones
  // then prevent delayed add/retire messages from resurrecting or deleting a newer use of the same C-RNTI.
  mutable std::map<du_cell_index_t, std::vector<ntn_rnti_lease_record>> ntn_rnti_leases;
  std::map<rnti_t, uint32_t>                                            ntn_rnti_retired_generations;
  uint32_t                                                              ntn_rnti_generation_high_water = 0;
  std::map<du_cell_index_t, uint32_t>                                   ntn_cell_generation_high_water;
  std::map<du_cell_index_t, bool>               ntn_cell_lease_mode_enabled;
  std::map<rnti_t, ntn_lease_time_point>                                terrestrial_rnti_reservations;
};

} // namespace srsran
