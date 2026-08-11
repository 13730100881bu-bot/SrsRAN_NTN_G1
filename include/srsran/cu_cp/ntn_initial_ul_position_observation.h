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

/// Describes how the source associated an Initial UL attempt with an onboard position.
enum class ntn_initial_ul_position_observation_authority : uint8_t {
  /// Private injection for audit and tests. It is never accepted as device proof in strict mode.
  injected_trusted,
  /// The active software calendar had one candidate, but no physical receive port was observed.
  software_attributed,
  /// A physical SDR/ZMQ receive port was observed and resolved through the active mapping.
  sdr_rx_port_verified,
  /// An OFH BeamId/eAxC context and matching U-plane receive port were observed.
  ofh_beam_id_verified,
  invalid = 0xffU
};

inline constexpr bool is_device_verified(ntn_initial_ul_position_observation_authority authority)
{
  return authority == ntn_initial_ul_position_observation_authority::sdr_rx_port_verified ||
         authority == ntn_initial_ul_position_observation_authority::ofh_beam_id_verified;
}

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
  /// Non-zero generation of the authoritative NTN C-RNTI lease. Legacy injected observations use zero.
  uint32_t        rnti_lease_generation = 0;
  uint64_t        du_connection_generation = 0;

  ntn_initial_ul_position_observation_authority authority =
      ntn_initial_ul_position_observation_authority::injected_trusted;

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
  uint16_t          physical_rx_port_id = std::numeric_limits<uint16_t>::max();
  uint16_t          ofh_prach_eaxc      = std::numeric_limits<uint16_t>::max();
  uint16_t          ofh_beam_id         = std::numeric_limits<uint16_t>::max();
  uint64_t          mapping_version     = 0;
  std::string       mapping_hash;
  float             receive_port_margin_db = 0.0F;
};

/// Exact key used to consume one Initial UL position observation.
struct ntn_initial_ul_position_observation_key {
  du_index_t      du_index      = du_index_t::invalid;
  du_cell_index_t du_cell_index = du_cell_index_t::invalid;
  rnti_t          c_rnti        = rnti_t::INVALID_RNTI;
  uint32_t        rnti_lease_generation = 0;
  uint64_t        du_connection_generation = 0;
};

enum class ntn_initial_ul_position_take_status {
  found,
  missing,
  expired,
  ambiguous,
  replayed,
  source_unavailable,
  du_generation_mismatch,
  rnti_generation_mismatch
};

struct ntn_initial_ul_position_take_result {
  /// Exactly one observation must be present when status is found. All other statuses must carry no observation.
  /// The CU-CP consumer validates this invariant and fails closed if a provider violates it.
  ntn_initial_ul_position_take_status               status = ntn_initial_ul_position_take_status::missing;
  std::optional<ntn_initial_ul_position_observation> observation;
};

/// Consistent read-only view of one injected observation source.
struct ntn_initial_ul_position_source_snapshot {
  /// The provider/query interface can currently be called. This does not mean that an SDR/OFH backend is live.
  bool        ready = false;
  std::string authority;
  size_t      pending = 0;
  /// True when every record is bound to the authoritative C-RNTI lease generation.
  bool        rnti_generation_authoritative = false;
  /// True when the source can produce observations verified by an actual receive backend.
  bool        device_verification_capable = false;
  /// True only while the source has current confirmation from an SDR/OFH receive backend.
  bool        live_device_backend_ready = false;
  /// Human-readable backend associated with the current confirmation (sdr_zmq, ofh, mixed or none).
  std::string live_device_backend = "none";
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
