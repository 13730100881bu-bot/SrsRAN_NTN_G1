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
#include "srsran/ran/du_types.h"
#include "srsran/ran/rnti.h"
#include <algorithm>
#include <deque>
#include <map>
#include <mutex>

namespace srsran {

using du_rnti_table = rnti_value_table<du_ue_index_t, du_ue_index_t::INVALID_DU_UE_INDEX>;

/// \brief Extends DU RNTI Table with the ability to allocate unique RNTIs for UEs.
class rnti_manager : public du_rnti_table
{
public:
  rnti_manager(rnti_t initial_rnti = to_rnti(0x4601)) :
    rnti_counter(to_value(initial_rnti) - to_value(rnti_t::MIN_CRNTI))
  {
    srsran_assert(is_crnti(initial_rnti), "Invalid initial c-rnti={}", initial_rnti);
  }

  /// \brief Allocates new unique TC-RNTI.
  rnti_t allocate()
  {
    if (this->nof_ues() >= MAX_NOF_DU_UES) {
      // If the number of UEs is already maximum, ignore RACH.
      return rnti_t::INVALID_RNTI;
    }
    // Increments rnti counter until it finds an available temp C-RNTI.
    rnti_t temp_crnti;
    do {
      uint16_t prev_counter = rnti_counter.fetch_add(1, std::memory_order_relaxed) % CRNTI_RANGE;
      temp_crnti            = to_rnti(prev_counter + to_value(rnti_t::MIN_CRNTI));
    } while (this->has_rnti(temp_crnti));
    return temp_crnti;
  }

  /// Adds an NTN CU-CP-provided RNTI lease to the local consumption pool.
  bool add_ntn_rnti_lease(rnti_t rnti)
  {
    return add_ntn_rnti_lease(INVALID_DU_CELL_INDEX, rnti);
  }

  /// Adds an NTN CU-CP-provided RNTI lease to the local per-cell consumption pool.
  bool add_ntn_rnti_lease(du_cell_index_t cell_index, rnti_t rnti)
  {
    if (!is_crnti(rnti) || this->has_rnti(rnti)) {
      return false;
    }

    std::lock_guard<std::mutex> lock(ntn_lease_mutex);
    auto& leases = ntn_rnti_leases[cell_index];
    if (std::find(leases.begin(), leases.end(), rnti) != leases.end()) {
      return false;
    }
    leases.push_back(rnti);
    return true;
  }

  /// Consumes one NTN CU-CP-provided RNTI lease. Does not fall back to the terrestrial allocator.
  rnti_t allocate_ntn_lease()
  {
    return allocate_ntn_lease(INVALID_DU_CELL_INDEX);
  }

  /// Consumes one NTN CU-CP-provided RNTI lease for a cell. Does not fall back to the terrestrial allocator.
  rnti_t allocate_ntn_lease(du_cell_index_t cell_index)
  {
    std::lock_guard<std::mutex> lock(ntn_lease_mutex);
    rnti_t result = allocate_ntn_lease_locked(cell_index);
    if (result != rnti_t::INVALID_RNTI || cell_index == INVALID_DU_CELL_INDEX) {
      return result;
    }
    return allocate_ntn_lease_locked(INVALID_DU_CELL_INDEX);
  }

  /// Clears all pending NTN leases for a cell.
  void clear_ntn_rnti_leases(du_cell_index_t cell_index)
  {
    std::lock_guard<std::mutex> lock(ntn_lease_mutex);
    ntn_rnti_leases[cell_index].clear();
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
    if (ntn_rnti_lease_mode_enabled.load(std::memory_order_relaxed)) {
      return true;
    }
    std::lock_guard<std::mutex> lock(ntn_lease_mutex);
    auto                        it = ntn_cell_lease_mode_enabled.find(cell_index);
    return it != ntn_cell_lease_mode_enabled.end() && it->second;
  }

  unsigned nof_ntn_rnti_leases() const
  {
    return nof_ntn_rnti_leases(INVALID_DU_CELL_INDEX);
  }

  unsigned nof_ntn_rnti_leases(du_cell_index_t cell_index) const
  {
    std::lock_guard<std::mutex> lock(ntn_lease_mutex);
    auto                        it = ntn_rnti_leases.find(cell_index);
    return it == ntn_rnti_leases.end() ? 0U : static_cast<unsigned>(it->second.size());
  }

private:
  rnti_t allocate_ntn_lease_locked(du_cell_index_t cell_index)
  {
    auto it = ntn_rnti_leases.find(cell_index);
    if (it == ntn_rnti_leases.end()) {
      return rnti_t::INVALID_RNTI;
    }
    auto& leases = it->second;
    while (!leases.empty()) {
      const rnti_t next_rnti = leases.front();
      leases.pop_front();
      if (!this->has_rnti(next_rnti) && this->nof_ues() < MAX_NOF_DU_UES) {
        return next_rnti;
      }
    }
    return rnti_t::INVALID_RNTI;
  }

  static constexpr int CRNTI_RANGE = to_value(rnti_t::MAX_CRNTI) + 1 - to_value(rnti_t::MIN_CRNTI);

  std::atomic<std::underlying_type_t<rnti_t>> rnti_counter;
  std::atomic<bool>                           ntn_rnti_lease_mode_enabled{false};
  mutable std::mutex                          ntn_lease_mutex;
  std::map<du_cell_index_t, std::deque<rnti_t>> ntn_rnti_leases;
  std::map<du_cell_index_t, bool>               ntn_cell_lease_mode_enabled;
};

} // namespace srsran
