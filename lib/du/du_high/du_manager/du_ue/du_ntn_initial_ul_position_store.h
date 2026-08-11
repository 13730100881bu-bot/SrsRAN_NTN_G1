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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * A copy of the GNU Affero General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */

#pragma once

#include "srsran/f1ap/ntn_initial_ul_position_query.h"
#include "srsran/mac/mac_ntn_initial_ul_position.h"
#include <algorithm>
#include <chrono>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace srsran {
namespace srs_du {

/// Bounded DU-side store for Initial UL position observations. Entries are consumed once by an exact F1 query.
class du_ntn_initial_ul_position_store
{
public:
  using clock = std::chrono::steady_clock;

  static constexpr size_t                    max_entries = MAX_NOF_DU_UES;
  static constexpr std::chrono::milliseconds entry_ttl{1000};

  /// Stores an immutable observation without replacing an existing UE or observation entry.
  bool store(gnb_du_ue_f1ap_id_t                     f1ap_ue_id,
             const mac_ntn_initial_ul_position_record& observation,
             clock::time_point                         received_at)
  {
    std::lock_guard<std::mutex> lock(mutex);
    prune_expired(received_at);

    if (f1ap_ue_id == gnb_du_ue_f1ap_id_t::invalid || observation.observation_id == 0 ||
        static_cast<unsigned>(observation.cell_index) >= MAX_NOF_DU_CELLS || !is_crnti(observation.c_rnti) ||
        observation.rnti_generation == 0 || observation.rnti_generation == std::numeric_limits<uint32_t>::max() ||
        entries.size() >= max_entries || entries.count(f1ap_ue_id) != 0) {
      return false;
    }
    for (const auto& item : entries) {
      if (item.second.observation.observation_id == observation.observation_id) {
        return false;
      }
    }

    entry value;
    value.observation = observation;
    value.received_at = received_at;
    entries.emplace(f1ap_ue_id, std::move(value));
    return true;
  }

  /// Returns an observation only when the complete query target matches. A different nonce cannot consume it again.
  f1ap_ntn_initial_ul_position_result query(const f1ap_ntn_initial_ul_position_query& request,
                                            clock::time_point                         now)
  {
    std::lock_guard<std::mutex> lock(mutex);
    f1ap_ntn_initial_ul_position_result rejection = make_rejection(request, "observation_missing");

    auto it = entries.find(request.gnb_du_ue_f1ap_id);
    if (it == entries.end()) {
      prune_expired(now);
      return rejection;
    }
    entry& stored = it->second;
    if (now >= stored.received_at && now - stored.received_at >= entry_ttl) {
      entries.erase(it);
      rejection.reason = "observation_expired";
      return rejection;
    }

    if (stored.consumed_query.has_value()) {
      if (stored.consumed_query->nonce != request.nonce) {
        rejection.reason = "observation_consumed";
        return rejection;
      }
      if (!same_target(*stored.consumed_query, request)) {
        rejection.reason = "nonce_identity_mismatch";
        return rejection;
      }
      return *stored.consumed_result;
    }

    const mac_ntn_initial_ul_position_record& observation = stored.observation;
    if (observation.cell_index != request.cell_index || observation.nci != request.cell_cgi.nci ||
        observation.pci != request.pci || observation.c_rnti != request.c_rnti) {
      rejection.reason = "observation_identity_mismatch";
      return rejection;
    }
    if (observation.rnti_generation != request.expected_rnti_generation) {
      rejection.reason = "rnti_generation_mismatch";
      return rejection;
    }

    f1ap_ntn_initial_ul_position_result result = make_result(request, observation);
    stored.consumed_query  = request;
    stored.consumed_result = result;
    return result;
  }

  void erase(gnb_du_ue_f1ap_id_t f1ap_ue_id)
  {
    std::lock_guard<std::mutex> lock(mutex);
    entries.erase(f1ap_ue_id);
  }

  /// Invalidates all records after an F1 connection generation change or an explicit calendar clear.
  void invalidate_all()
  {
    std::lock_guard<std::mutex> lock(mutex);
    entries.clear();
  }

  /// Conservatively invalidates all records whose lease namespace may have changed.
  void invalidate_cell(du_cell_index_t cell_index)
  {
    std::lock_guard<std::mutex> lock(mutex);
    for (auto it = entries.begin(); it != entries.end();) {
      if (it->second.observation.cell_index == cell_index) {
        it = entries.erase(it);
      } else {
        ++it;
      }
    }
  }

  /// Invalidates observations for leases that have been retired on a specific cell.
  void invalidate_rntis(du_cell_index_t cell_index, const std::vector<rnti_t>& rntis)
  {
    std::lock_guard<std::mutex> lock(mutex);
    for (auto it = entries.begin(); it != entries.end();) {
      const auto& observation = it->second.observation;
      if (observation.cell_index == cell_index &&
          std::find(rntis.begin(), rntis.end(), observation.c_rnti) != rntis.end()) {
        it = entries.erase(it);
      } else {
        ++it;
      }
    }
  }

  /// Keeps only observations produced by the currently applied plan.
  void retain_plan(uint64_t schedule_version, const std::string& calendar_hash)
  {
    std::lock_guard<std::mutex> lock(mutex);
    for (auto it = entries.begin(); it != entries.end();) {
      const auto& observation = it->second.observation;
      if (observation.schedule_version != schedule_version || observation.calendar_hash != calendar_hash) {
        it = entries.erase(it);
      } else {
        ++it;
      }
    }
  }

  /// Removes only observations produced by a calendar that has been cleared or rolled back.
  void erase_plan(uint64_t schedule_version, const std::string& calendar_hash)
  {
    std::lock_guard<std::mutex> lock(mutex);
    for (auto it = entries.begin(); it != entries.end();) {
      const auto& observation = it->second.observation;
      if (observation.schedule_version == schedule_version && observation.calendar_hash == calendar_hash) {
        it = entries.erase(it);
      } else {
        ++it;
      }
    }
  }

  size_t size() const
  {
    std::lock_guard<std::mutex> lock(mutex);
    return entries.size();
  }

private:
  struct entry {
    mac_ntn_initial_ul_position_record                 observation;
    clock::time_point                                  received_at;
    std::optional<f1ap_ntn_initial_ul_position_query>  consumed_query;
    std::optional<f1ap_ntn_initial_ul_position_result> consumed_result;
  };

  static bool same_target(const f1ap_ntn_initial_ul_position_query& lhs,
                          const f1ap_ntn_initial_ul_position_query& rhs)
  {
    return lhs.query_generation == rhs.query_generation && lhs.nonce == rhs.nonce &&
           lhs.connection_token == rhs.connection_token && lhs.gnb_du_id == rhs.gnb_du_id &&
           lhs.cell_cgi == rhs.cell_cgi && lhs.cell_index == rhs.cell_index && lhs.pci == rhs.pci &&
           lhs.gnb_du_ue_f1ap_id == rhs.gnb_du_ue_f1ap_id && lhs.c_rnti == rhs.c_rnti &&
           lhs.expected_rnti_generation == rhs.expected_rnti_generation;
  }

  static f1ap_ntn_initial_ul_position_result make_rejection(const f1ap_ntn_initial_ul_position_query& request,
                                                            std::string reason)
  {
    f1ap_ntn_initial_ul_position_result result;
    copy_target(result, request);
    result.accepted  = false;
    result.authority = f1ap_ntn_initial_ul_position_authority::none;
    result.reason    = std::move(reason);
    return result;
  }

  static f1ap_ntn_initial_ul_position_authority
  map_authority(mac_ntn_initial_ul_position_authority authority)
  {
    switch (authority) {
      case mac_ntn_initial_ul_position_authority::software_attributed:
        return f1ap_ntn_initial_ul_position_authority::software_attributed;
      case mac_ntn_initial_ul_position_authority::sdr_rx_port_verified:
        return f1ap_ntn_initial_ul_position_authority::sdr_rx_port_verified;
      case mac_ntn_initial_ul_position_authority::ofh_beam_id_verified:
        return f1ap_ntn_initial_ul_position_authority::ofh_beam_id_verified;
      case mac_ntn_initial_ul_position_authority::none:
      case mac_ntn_initial_ul_position_authority::invalid:
        return f1ap_ntn_initial_ul_position_authority::none;
    }
    return f1ap_ntn_initial_ul_position_authority::none;
  }

  static f1ap_ntn_initial_ul_position_result make_result(const f1ap_ntn_initial_ul_position_query& request,
                                                         const mac_ntn_initial_ul_position_record& observation)
  {
    if (!observation.usable()) {
      return make_rejection(
          request, observation.reason.empty() ? "observation_not_authoritative" : observation.reason);
    }

    f1ap_ntn_initial_ul_position_result result;
    copy_target(result, request);
    result.accepted             = true;
    result.observation_id       = observation.observation_id;
    result.authority            = map_authority(observation.authority);
    result.reason               = "accepted";
    result.schedule_version     = observation.schedule_version;
    result.calendar_hash        = observation.calendar_hash;
    result.mapping_version      = observation.mapping_version;
    result.mapping_hash         = observation.mapping_hash;
    result.position_id          = observation.position_id;
    result.logical_port         = observation.cell_local_port;
    result.physical_port        = observation.physical_rx_port;
    result.eaxc                 = observation.prach_eaxc;
    result.beam_id              = observation.beam_id;
    result.calendar_cycle_index = observation.calendar_cycle_index;
    result.occasion_offset_us   = observation.occasion_offset_us;
    result.confidence_margin_db = observation.receive_port_margin_db;

    // The codec is the single source of bounds and authority-dependent optional-field rules.
    if (encode_f1ap_ntn_initial_ul_position_result(result).empty()) {
      return make_rejection(request, "observation_invalid");
    }
    return result;
  }

  template <typename Result, typename Query>
  static void copy_target(Result& result, const Query& request)
  {
    result.query_generation         = request.query_generation;
    result.nonce                    = request.nonce;
    result.connection_token         = request.connection_token;
    result.gnb_du_id                = request.gnb_du_id;
    result.cell_cgi                 = request.cell_cgi;
    result.cell_index               = request.cell_index;
    result.pci                      = request.pci;
    result.gnb_du_ue_f1ap_id        = request.gnb_du_ue_f1ap_id;
    result.c_rnti                   = request.c_rnti;
    result.expected_rnti_generation = request.expected_rnti_generation;
  }

  void prune_expired(clock::time_point now)
  {
    for (auto it = entries.begin(); it != entries.end();) {
      if (now >= it->second.received_at && now - it->second.received_at >= entry_ttl) {
        it = entries.erase(it);
      } else {
        ++it;
      }
    }
  }

  mutable std::mutex mutex;
  std::unordered_map<gnb_du_ue_f1ap_id_t, entry> entries;
};

} // namespace srs_du
} // namespace srsran
