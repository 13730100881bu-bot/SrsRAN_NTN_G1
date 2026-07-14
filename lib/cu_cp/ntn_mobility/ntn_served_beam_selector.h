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

#include "srsran/cu_cp/cell_meas_manager_config.h"
#include "srsran/cu_cp/ntn_qos_policy.h"
#include "srsran/ran/ntn.h"
#include <string>
#include <vector>

namespace srsran {
namespace srs_cu_cp {

/// One static NTN beam visible from a satellite state.
struct ntn_served_beam_candidate {
  std::string beam_id;
  double      elevation_deg = 0.0;
  bool        in_hopping_window = true;
  std::string serving_satellite_id = "sat-0";
  unsigned    scheduling_score = 0;
  unsigned    window_rank      = 0;
  std::string scheduling_reason = "legacy";
};

/// Demand known by CU-CP for one NTN digital service beam before selecting the active service window.
struct ntn_served_beam_demand {
  std::string            beam_id;
  unsigned               nof_ues = 0;
  unsigned               nof_drbs = 0;
  unsigned               nof_pending_handovers = 0;
  ntn_qos_demand_summary qos;
};

/// Select the full NTN candidate inventory from a satellite ECEF state and a static CU-CP beam table.
std::vector<ntn_served_beam_candidate>
select_ntn_candidate_inventory_by_elevation(const std::vector<ntn_beam_position>& beams,
                                            const ecef_coordinates_t&             satellite,
                                            double                                min_elevation_deg);

/// Select the full NTN candidate inventory from multiple satellite ECEF states, keeping the best elevation owner for
/// each visible beam.
std::vector<ntn_served_beam_candidate>
select_ntn_candidate_inventory_by_elevation(const std::vector<ntn_beam_position>& beams,
                                            const std::vector<ntn_satellite_state>& satellites,
                                            double                                min_elevation_deg);

/// Select served NTN beam ids from a satellite ECEF state and a static CU-CP beam table.
std::vector<std::string> select_ntn_served_beams_by_elevation(const std::vector<ntn_beam_position>& beams,
                                                              const ecef_coordinates_t&             satellite,
                                                              double                                min_elevation_deg,
                                                              unsigned                              max_nof_served_beams);

/// Select legacy served NTN beam candidates including elevation, sorted by highest elevation first.
/// The max_nof_served_beams value caps this compatibility API. Use select_ntn_candidate_inventory_by_elevation for
/// uncapped CU-CP candidate inventory.
std::vector<ntn_served_beam_candidate>
select_ntn_served_beam_candidates_by_elevation(const std::vector<ntn_beam_position>& beams,
                                               const ecef_coordinates_t&             satellite,
                                               double                                min_elevation_deg,
                                               unsigned                              max_nof_served_beams);

} // namespace srs_cu_cp
} // namespace srsran
