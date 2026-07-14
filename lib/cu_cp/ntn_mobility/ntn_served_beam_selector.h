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
};

/// Select served NTN beam ids from a satellite ECEF state and a static CU-CP beam table.
std::vector<std::string> select_ntn_served_beams_by_elevation(const std::vector<ntn_beam_position>& beams,
                                                              const ecef_coordinates_t&             satellite,
                                                              double                                min_elevation_deg,
                                                              unsigned                              max_nof_served_beams);

/// Select served NTN beam candidates including elevation, sorted by highest elevation first.
/// A max_nof_served_beams value of zero returns all visible candidates.
std::vector<ntn_served_beam_candidate>
select_ntn_served_beam_candidates_by_elevation(const std::vector<ntn_beam_position>& beams,
                                               const ecef_coordinates_t&             satellite,
                                               double                                min_elevation_deg,
                                               unsigned                              max_nof_served_beams);

} // namespace srs_cu_cp
} // namespace srsran
