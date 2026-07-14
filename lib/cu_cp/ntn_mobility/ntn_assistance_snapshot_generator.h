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

#include "ntn_beam_placement_planner.h"
#include "srsran/cu_cp/ntn_location.h"
#include <chrono>
#include <optional>

namespace srsran {
namespace srs_cu_cp {

/// Inputs used by CU-CP to build an NTN assistance snapshot.
struct ntn_assistance_snapshot_request {
  std::optional<ecef_coordinates_t> satellite_ecef;
  std::vector<ntn_satellite_state>  satellite_states;
  std::chrono::system_clock::time_point satellite_epoch{};
  std::optional<std::chrono::steady_clock::time_point> satellite_received_time;
  std::chrono::steady_clock::time_point                now = std::chrono::steady_clock::now();
  std::chrono::milliseconds satellite_state_max_age{0};

  std::vector<ntn_beam_position> beams;
  ntn_beam_placement_plan        placement_plan;

  unsigned                cell_specific_koffset = 0;
  std::optional<unsigned> k_mac;
  std::optional<unsigned> ul_sync_validity_s;
  std::optional<uint64_t> t_service;

  /// Maximum number of per-beam assistance entries. A value of zero means unlimited.
  unsigned max_snapshot_beams = 0;
};

/// Builds a bounded CU-CP NTN assistance snapshot for RRC/SIB19 contracts.
ntn_assistance_snapshot build_ntn_assistance_snapshot(const ntn_assistance_snapshot_request& request);

} // namespace srs_cu_cp
} // namespace srsran
