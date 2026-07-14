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
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace srsran {
namespace srs_cu_cp {

/// Source of a decoded NTN UE location report.
enum class ntn_ue_location_report_source { ue_assistance_info, measurement_report };

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
  std::string            target_beam_id;
  gnb_id_t               target_gnb_id{0, 22};
  nr_cell_identity       target_nci = nr_cell_identity::min();
  pci_t                  target_pci = INVALID_PCI;
  std::vector<std::string> served_beam_ids_snapshot;
  ntn_ue_location_report last_location_report;
  std::chrono::steady_clock::time_point candidate_since;
  std::chrono::steady_clock::time_point last_report_time;
  unsigned                             consecutive_location_reports = 0;
};

} // namespace srs_cu_cp
} // namespace srsran
