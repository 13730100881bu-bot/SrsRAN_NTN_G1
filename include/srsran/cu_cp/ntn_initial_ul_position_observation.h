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
 * the LICENSE file in the top-level directory of this distribution.
 *
 */

#pragma once

#include "srsran/cu_cp/cu_cp_types.h"
#include "srsran/ran/nr_cell_identity.h"
#include "srsran/ran/pci.h"
#include "srsran/ran/rnti.h"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>

namespace srsran {
namespace srs_cu_cp {

/// Maximum number of Initial UL position observations retained by the standard bounded CU-CP store.
inline constexpr size_t max_ntn_initial_ul_position_observations = 1024U;

/// Default lifetime of one Initial UL position observation in the standard bounded CU-CP store.
inline constexpr std::chrono::milliseconds ntn_initial_ul_position_observation_ttl{1000};

/// Private extension record for binding one Initial UL attempt to an active onboard position plan.
///
/// No standard F1AP message carries these fields. The source must bind the record to the applied software calendar
/// and must not derive position_id from NCI, coordinates or a legacy beam ID.
struct ntn_initial_ul_position_observation {
  uint64_t observation_id = 0;

  ue_index_t      ue_index      = ue_index_t::invalid;
  du_index_t      du_index      = du_index_t::invalid;
  du_cell_index_t du_cell_index = du_cell_index_t::invalid;
  rnti_t          c_rnti        = rnti_t::INVALID_RNTI;
  uint64_t        du_connection_generation = 0;

  std::string      satellite_id;
  uint64_t         catalog_version  = 0;
  uint64_t         schedule_version = 0;
  std::string      source_content_hash;
  std::string      calendar_hash;
  nr_cell_identity nci = nr_cell_identity::min();
  pci_t            pci = INVALID_PCI;
  std::string      position_id;
  std::chrono::system_clock::time_point occasion_time{};
  uint16_t          ul_beam_port_id = std::numeric_limits<uint16_t>::max();
};

/// Exact key used to consume one Initial UL position observation.
struct ntn_initial_ul_position_observation_key {
  du_index_t      du_index      = du_index_t::invalid;
  du_cell_index_t du_cell_index = du_cell_index_t::invalid;
  rnti_t          c_rnti        = rnti_t::INVALID_RNTI;
  uint64_t        du_connection_generation = 0;
};

enum class ntn_initial_ul_position_take_status {
  found,
  missing,
  expired,
  ambiguous,
  replayed,
  source_unavailable,
  du_generation_mismatch
};

struct ntn_initial_ul_position_take_result {
  /// Exactly one observation must be present when status is found. All other statuses must carry no observation.
  /// The CU-CP consumer validates this invariant and fails closed if a provider violates it.
  ntn_initial_ul_position_take_status               status = ntn_initial_ul_position_take_status::missing;
  std::optional<ntn_initial_ul_position_observation> observation;
};

/// Consistent read-only view of one injected observation source.
struct ntn_initial_ul_position_source_snapshot {
  bool        ready = false;
  std::string authority;
  size_t      pending = 0;
};

/// Thread-safe, non-blocking source injected through cu_cp_configuration.
class ntn_initial_ul_position_observation_provider
{
public:
  virtual ~ntn_initial_ul_position_observation_provider() = default;

  virtual ntn_initial_ul_position_source_snapshot source_snapshot() const = 0;

  virtual ntn_initial_ul_position_take_result
  take(const ntn_initial_ul_position_observation_key& key,
       std::chrono::steady_clock::time_point          now = std::chrono::steady_clock::now()) = 0;

  virtual void invalidate_du(du_index_t du_index) = 0;
  virtual void invalidate_all()                    = 0;
};

} // namespace srs_cu_cp
} // namespace srsran
