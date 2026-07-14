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
#include "srsran/ran/cause/ngap_cause.h"
#include <cstdint>
#include <optional>
#include <vector>

namespace srsran {
namespace srs_cu_cp {

enum class ngap_location_reporting_event_type {
  direct,
  change_of_serving_cell,
  ue_presence_in_area_of_interest,
  stop_change_of_serving_cell,
  stop_ue_presence_in_area_of_interest,
  cancel_location_report_for_the_ue
};

struct ngap_location_reporting_request_type {
  ngap_location_reporting_event_type event_type = ngap_location_reporting_event_type::direct;
  std::vector<uint8_t>               area_of_interest_ref_ids;
  std::optional<uint8_t>             location_report_ref_id_to_be_cancelled;
};

struct ngap_location_reporting_control {
  ue_index_t                           ue_index = ue_index_t::invalid;
  ngap_location_reporting_request_type request_type;
};

struct ngap_location_reporting_control_response {
  bool         accepted = true;
  ngap_cause_t cause    = ngap_cause_radio_network_t::unspecified;
};

struct ngap_ue_presence_in_area_of_interest {
  uint8_t location_report_ref_id = 1;
};

struct ngap_location_report {
  ue_index_t                           ue_index = ue_index_t::invalid;
  cu_cp_user_location_info_nr          user_location_info;
  ngap_location_reporting_request_type request_type;
  std::vector<ngap_ue_presence_in_area_of_interest> ue_presence_in_area_of_interest_list;
};

} // namespace srs_cu_cp
} // namespace srsran
