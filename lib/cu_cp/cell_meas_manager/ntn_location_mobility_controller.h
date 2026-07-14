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

#include "measurement_context.h"
#include "srsran/cu_cp/cell_meas_manager_config.h"
#include "srsran/cu_cp/cu_cp_types.h"
#include "srsran/cu_cp/ntn_location.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace srsran {
namespace srs_cu_cp {

class cell_meas_mobility_manager_notifier;
class ue_manager;

/// Handles NTN location-based mobility state that is orthogonal to radio measurement configuration.
class ntn_location_mobility_controller
{
public:
  ntn_location_mobility_controller(cell_meas_manager_cfg&                cfg_,
                                   cell_meas_mobility_manager_notifier& mobility_mng_notifier_,
                                   ue_manager&                          ue_mng_,
                                   srslog::basic_logger&                logger_);

  void rebuild_beam_lookup();

  bool update_served_beams(const std::vector<std::string>& beam_ids);
  ntn_location_report_result report_ue_location(const ntn_ue_location_report& report);
  void handle_handover_result(const ntn_handover_result& result);

private:
  void clear_candidate_beam(cell_meas_manager_ue_context& ue_meas_context);
  bool resolve_served_beams(const std::vector<std::string>&        beam_ids,
                            std::vector<const ntn_beam_position*>& resolved_beams) const;

  cell_meas_manager_cfg&                cfg;
  cell_meas_mobility_manager_notifier& mobility_mng_notifier;
  ue_manager&                          ue_mng;
  srslog::basic_logger&                logger;

  std::unordered_map<std::string, const ntn_beam_position*> beam_lookup;
  std::vector<std::string>                                  current_served_beam_ids;
  std::vector<const ntn_beam_position*>                     current_served_beams;
};

} // namespace srs_cu_cp
} // namespace srsran
