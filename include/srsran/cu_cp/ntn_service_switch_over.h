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

#include "srsran/ran/nr_cell_identity.h"
#include "srsran/ran/ntn.h"
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace srsran {
namespace srs_cu_cp {

enum class ntn_service_switch_over_type { soft, hard };

enum class ntn_service_switch_over_source { manual_override, tle, circular_fallback, operator_command };

enum class ntn_service_switch_over_policy { prepare, drain, handover_preferred, release_allowed };

enum class ntn_service_beam_policy { normal, prepare, block_new_demand, drain, release_allowed };

enum class ntn_manual_override_mode { none, freeze_current_state, replace_service_state, restore_automatic_source };

struct ntn_service_switch_over_event {
  uint64_t event_id                         = 0;
  ntn_service_switch_over_type type         = ntn_service_switch_over_type::soft;
  ntn_service_switch_over_source source     = ntn_service_switch_over_source::operator_command;
  ntn_service_switch_over_policy policy     = ntn_service_switch_over_policy::prepare;
  std::vector<std::string> affected_beam_ids;
  std::vector<nr_cell_identity> affected_ncis;
  std::chrono::steady_clock::time_point start_time = std::chrono::steady_clock::now();
  std::optional<std::chrono::steady_clock::time_point> expected_end_time;
  std::string reason;
};

struct ntn_manual_override_command {
  ntn_manual_override_mode         mode = ntn_manual_override_mode::none;
  std::optional<ecef_coordinates_t> replacement_satellite_ecef;
  std::vector<std::string>         replacement_served_beam_ids;
  std::string                      reason;
};

struct ntn_service_switch_over_beam_snapshot {
  std::string             beam_id;
  nr_cell_identity        nci    = nr_cell_identity::min();
  ntn_service_beam_policy policy = ntn_service_beam_policy::normal;
};

struct ntn_service_switch_over_snapshot {
  ntn_manual_override_mode manual_override_mode = ntn_manual_override_mode::none;
  bool automatic_source_updates_allowed         = true;
  std::vector<ntn_service_switch_over_event> active_events;
  std::vector<ntn_service_switch_over_beam_snapshot> beams;
};

} // namespace srs_cu_cp
} // namespace srsran
