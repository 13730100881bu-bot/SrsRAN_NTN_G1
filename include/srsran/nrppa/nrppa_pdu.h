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

#include "srsran/adt/byte_buffer.h"
#include "srsran/adt/expected.h"
#include "srsran/nrppa/nrppa.h"
#include <string>
#include <variant>

namespace srsran::srs_cu_cp {

enum class nrppa_minimal_pdu_type : uint8_t {
  trp_information_request,
  trp_information_response,
  trp_information_failure,
  positioning_information_request,
  positioning_information_response,
  positioning_information_failure,
  measurement_request,
  measurement_response,
  measurement_failure,
  positioning_activation_request,
  positioning_activation_response,
  positioning_activation_failure,
  positioning_deactivation_request,
  positioning_deactivation_response,
  positioning_deactivation_failure,
  positioning_assistance_information_control_request,
  positioning_assistance_information_feedback,
  positioning_assistance_information_failure
};

struct nrppa_minimal_pdu {
  nrppa_minimal_pdu_type type;
  std::variant<trp_information_request_t,
               trp_information_cu_cp_response_t,
               trp_information_failure_t,
               positioning_information_request_t,
               positioning_information_response_t,
               positioning_information_failure_t,
               measurement_request_t,
               measurement_response_t,
               measurement_failure_t,
               positioning_activation_request_t,
               positioning_activation_response_t,
               positioning_activation_failure_t,
               positioning_deactivation_request_t,
               positioning_deactivation_response_t,
               positioning_deactivation_failure_t,
               positioning_assistance_information_control_request_t,
               positioning_assistance_information_feedback_t,
               positioning_assistance_information_failure_t>
      value;
};

expected<nrppa_minimal_pdu, std::string> decode_nrppa_pdu(const byte_buffer& pdu);

bool is_nrppa_minimal_pdu(const byte_buffer& pdu);

expected<nrppa_minimal_pdu, std::string> decode_nrppa_standard_pdu(const byte_buffer& pdu);

byte_buffer encode_nrppa_standard_trp_information_request(const trp_information_request_t& request);
byte_buffer encode_nrppa_standard_trp_information_response(const trp_information_cu_cp_response_t& response);
byte_buffer encode_nrppa_standard_trp_information_failure(const trp_information_failure_t& failure);

byte_buffer encode_nrppa_trp_information_request(const trp_information_request_t& request);
byte_buffer encode_nrppa_trp_information_response(const trp_information_cu_cp_response_t& response);
byte_buffer encode_nrppa_trp_information_failure(const trp_information_failure_t& failure);
byte_buffer encode_nrppa_positioning_information_request(const positioning_information_request_t& request);
byte_buffer encode_nrppa_positioning_information_response(const positioning_information_response_t& response);
byte_buffer encode_nrppa_positioning_information_failure(const positioning_information_failure_t& failure);
byte_buffer encode_nrppa_measurement_request(const measurement_request_t& request);
byte_buffer encode_nrppa_measurement_response(const measurement_response_t& response);
byte_buffer encode_nrppa_measurement_failure(const measurement_failure_t& failure);
byte_buffer encode_nrppa_positioning_activation_request(const positioning_activation_request_t& request);
byte_buffer encode_nrppa_positioning_activation_response(const positioning_activation_response_t& response);
byte_buffer encode_nrppa_positioning_activation_failure(const positioning_activation_failure_t& failure);
byte_buffer encode_nrppa_positioning_deactivation_request(const positioning_deactivation_request_t& request);
byte_buffer encode_nrppa_positioning_deactivation_response(const positioning_deactivation_response_t& response);
byte_buffer encode_nrppa_positioning_deactivation_failure(const positioning_deactivation_failure_t& failure);
byte_buffer encode_nrppa_positioning_assistance_information_control_request(
    const positioning_assistance_information_control_request_t& request);
byte_buffer encode_nrppa_positioning_assistance_information_feedback(
    const positioning_assistance_information_feedback_t& feedback);
byte_buffer encode_nrppa_positioning_assistance_information_failure(
    const positioning_assistance_information_failure_t& failure);

} // namespace srsran::srs_cu_cp
