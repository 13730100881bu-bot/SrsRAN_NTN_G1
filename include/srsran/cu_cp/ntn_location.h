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
#include "srsran/ran/nr_cgi.h"
#include "srsran/ran/ntn.h"
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace srsran {
namespace srs_cu_cp {

/// Source of a decoded NTN UE location report.
enum class ntn_ue_location_report_source { ue_assistance_info, measurement_report };

/// RRC-side outcome while processing UE location payloads before CU-CP admission gates run.
enum class ntn_rrc_ue_location_report_outcome { received, decoded, unsupported, decode_failed };

/// Result of accepting a decoded NTN UE location report into CU-CP state.
enum class ntn_location_report_result {
  accepted,
  disabled,
  invalid,
  unknown_cell,
  unknown_ue,
  stale,
  inaccurate,
  out_of_order,
  duplicate
};

/// Decoded UE location used by CU-CP NTN location-based mobility.
struct ntn_ue_location_report {
  ue_index_t                    ue_index    = ue_index_t::invalid;
  nr_cell_identity              serving_nci = nr_cell_identity::min();
  double                        latitude_deg  = 0.0;
  double                        longitude_deg = 0.0;
  double                        altitude_m = 0.0;
  std::optional<double>         horizontal_accuracy_m;
  std::optional<double>         velocity_mps;
  std::optional<double>         heading_deg;
  std::chrono::steady_clock::time_point received_time = std::chrono::steady_clock::now();
  ntn_ue_location_report_source source        = ntn_ue_location_report_source::ue_assistance_info;
};

/// NTN location-based handover trigger produced after a UE stays in a served target beam long enough.
struct ntn_location_handover_trigger {
  ue_index_t             ue_index    = ue_index_t::invalid;
  nr_cell_identity       serving_nci = nr_cell_identity::min();
  uint64_t               handover_attempt_id = 0;
  std::string            source_beam_id;
  std::string            source_analog_beam_id;
  std::string            target_beam_id;
  std::string            target_analog_beam_id;
  std::string            handover_reason;
  gnb_id_t               target_gnb_id{0, 22};
  nr_cell_identity       target_nci = nr_cell_identity::min();
  pci_t                  target_pci = INVALID_PCI;
  bool                   target_preloaded = false;
  du_index_t             target_du_index  = du_index_t::invalid;
  rnti_t                 target_c_rnti    = rnti_t::INVALID_RNTI;
  std::string            target_uplink_resource_beam_id;
  nr_cell_identity       target_uplink_resource_nci = nr_cell_identity::min();
  du_index_t             target_uplink_resource_du_index = du_index_t::invalid;
  std::string            target_service_pair_reason = "none";
  std::optional<f1ap_ntn_ul_slot_resource_request> target_ul_slot_request;
  std::string            target_resource_state = "none";
  bool                   target_sr_srs_applied = false;
  std::vector<std::string> served_beam_ids_snapshot;
  ntn_ue_location_report last_location_report;
  std::chrono::steady_clock::time_point candidate_since;
  std::chrono::steady_clock::time_point last_report_time;
  unsigned                             consecutive_location_reports = 0;
};

/// Reason why the current CU-CP NTN assistance snapshot is not usable.
enum class ntn_assistance_invalid_reason { none, disabled, no_satellite_state, stale_satellite_state };

/// One propagated NTN satellite state known by CU-CP.
struct ntn_satellite_state {
  std::string        satellite_id = "sat-0";
  ecef_coordinates_t ecef;
};

/// Propagated NTN satellite states at a future offset from the current state update.
struct ntn_satellite_prediction_step {
  std::chrono::milliseconds       offset{0};
  std::vector<ntn_satellite_state> satellites;
};

/// Beam state represented in a CU-CP NTN assistance snapshot.
enum class ntn_assistance_beam_state { candidate, active_loaded, draining };

/// CU-CP-side NTN assistance for one beam. This is packaging input only; it does not imply DU SI scheduling.
struct ntn_assistance_beam_snapshot {
  std::string               beam_id;
  std::string               serving_satellite_id = "sat-0";
  nr_cell_identity          nci = nr_cell_identity::min();
  ntn_assistance_beam_state state = ntn_assistance_beam_state::candidate;
  geodetic_coordinates_t    reference_location{};
  std::optional<ta_info_t>  ta_info;
  unsigned                  cell_specific_koffset = 0;
  std::optional<unsigned>   k_mac;
  std::optional<unsigned>   ul_sync_validity_s;
  std::optional<uint64_t>   t_service;
};

/// CU-CP-side NTN assistance snapshot. This is an RRC/SIB19 contract input, not DU-owned broadcast execution.
struct ntn_assistance_snapshot {
  bool                          valid = false;
  ntn_assistance_invalid_reason invalid_reason = ntn_assistance_invalid_reason::no_satellite_state;
  std::optional<ecef_coordinates_t> satellite_ecef;
  std::vector<ntn_satellite_state> satellite_states;
  std::chrono::system_clock::time_point satellite_epoch{};
  std::vector<ntn_assistance_beam_snapshot> beams;
};

/// CU-CP-side SIB19 assistance packaging contract for one beam.
///
/// This is input for RRC/SIB19 packaging only. It does not imply DU SI scheduling
/// or SIB19 broadcast execution.
struct ntn_sib19_assistance_entry {
  bool                          valid = true;
  ntn_assistance_invalid_reason invalid_reason = ntn_assistance_invalid_reason::none;
  std::string                   beam_id;
  std::string                   serving_satellite_id = "sat-0";
  nr_cell_identity              nci = nr_cell_identity::min();
  ntn_assistance_beam_state     state = ntn_assistance_beam_state::candidate;
  geodetic_coordinates_t        reference_location{};
  std::optional<ecef_coordinates_t> satellite_ecef;
  std::chrono::system_clock::time_point satellite_epoch{};
  std::optional<epoch_time_t> epoch_time;
  std::optional<ta_info_t>    ta_info;
  std::optional<unsigned>     cell_specific_koffset;
  std::optional<unsigned>     k_mac;
  std::optional<unsigned>     ul_sync_validity_s;
  std::optional<uint64_t>     t_service;
};

/// Inputs used to derive a bounded CU-CP SIB19 assistance contract.
struct ntn_sib19_assistance_request {
  ntn_assistance_snapshot assistance;

  /// Optional SFN/subframe epoch. Wall-clock satellite epochs must not be
  /// converted into this ASN.1 field implicitly.
  std::optional<epoch_time_t> epoch_time;

  /// Maximum number of per-beam SIB19 entries. A value of zero means unlimited.
  unsigned max_entries = 0;
};

/// CU-CP-side SIB19 assistance packaging snapshot.
struct ntn_sib19_assistance_snapshot {
  bool                          valid = false;
  ntn_assistance_invalid_reason invalid_reason = ntn_assistance_invalid_reason::no_satellite_state;
  std::chrono::system_clock::time_point satellite_epoch{};
  std::vector<ntn_sib19_assistance_entry> entries;
};

} // namespace srs_cu_cp
} // namespace srsran
