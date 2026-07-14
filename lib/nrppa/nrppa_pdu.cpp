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

#include "srsran/nrppa/nrppa_pdu.h"

#include "srsran/ran/bcd_helper.h"
#include <vector>

using namespace srsran;
using namespace srs_cu_cp;

namespace {

constexpr std::array<uint8_t, 4> pdu_magic = {'N', 'P', 'P', 'A'};
constexpr uint8_t                pdu_version = 1;

enum class pdu_message_type : uint8_t {
  trp_information_request = 1,
  trp_information_response = 2,
  trp_information_failure = 3,
  positioning_information_request = 4,
  positioning_information_response = 5,
  positioning_information_failure = 6,
  measurement_request = 7,
  measurement_response = 8,
  measurement_failure = 9,
  positioning_activation_request = 10,
  positioning_activation_response = 11,
  positioning_activation_failure = 12,
  positioning_deactivation_request = 13,
  positioning_deactivation_response = 14,
  positioning_deactivation_failure = 15,
  positioning_assistance_information_control_request = 16,
  positioning_assistance_information_feedback = 17,
  positioning_assistance_information_failure = 18
};

bool append_u8(byte_buffer& pdu, uint8_t value)
{
  return pdu.append(value);
}

bool append_u16(byte_buffer& pdu, uint16_t value)
{
  return pdu.append(static_cast<uint8_t>((value >> 8U) & 0xffU)) && pdu.append(static_cast<uint8_t>(value & 0xffU));
}

bool append_u32(byte_buffer& pdu, uint32_t value)
{
  return pdu.append(static_cast<uint8_t>((value >> 24U) & 0xffU)) &&
         pdu.append(static_cast<uint8_t>((value >> 16U) & 0xffU)) &&
         pdu.append(static_cast<uint8_t>((value >> 8U) & 0xffU)) && pdu.append(static_cast<uint8_t>(value & 0xffU));
}

bool append_i32(byte_buffer& pdu, int32_t value)
{
  return append_u32(pdu, static_cast<uint32_t>(value));
}

bool append_u64(byte_buffer& pdu, uint64_t value)
{
  return append_u32(pdu, static_cast<uint32_t>((value >> 32U) & 0xffffffffU)) &&
         append_u32(pdu, static_cast<uint32_t>(value & 0xffffffffU));
}

bool append_header(byte_buffer& pdu, pdu_message_type type)
{
  return pdu.append(span<const uint8_t>{pdu_magic.data(), pdu_magic.size()}) && append_u8(pdu, pdu_version) &&
         append_u8(pdu, static_cast<uint8_t>(type));
}

bool append_bytes(byte_buffer& pdu, const byte_buffer& bytes)
{
  if (bytes.length() > std::numeric_limits<uint16_t>::max()) {
    return false;
  }
  std::vector<uint8_t> raw_bytes(bytes.begin(), bytes.end());
  return append_u16(pdu, static_cast<uint16_t>(raw_bytes.size())) &&
         pdu.append(span<const uint8_t>{raw_bytes.data(), raw_bytes.size()});
}

bool append_cgi(byte_buffer& pdu, const nr_cell_global_id_t& cgi)
{
  return append_u32(pdu, cgi.plmn_id.to_bcd()) && append_u64(pdu, cgi.nci.value());
}

uint8_t encode_type_item(trp_information_type_item_t type)
{
  return static_cast<uint8_t>(type);
}

std::optional<trp_information_type_item_t> decode_type_item(uint8_t type)
{
  if (type > static_cast<uint8_t>(trp_information_type_item_t::beam_ant_info)) {
    return std::nullopt;
  }
  return static_cast<trp_information_type_item_t>(type);
}

uint8_t encode_measurement_quantity_item(trp_meas_quantities_item_t item)
{
  return static_cast<uint8_t>(item);
}

std::optional<trp_meas_quantities_item_t> decode_measurement_quantity_item(uint8_t item)
{
  if (item > static_cast<uint8_t>(trp_meas_quantities_item_t::ul_srs_rsrp_p)) {
    return std::nullopt;
  }
  return static_cast<trp_meas_quantities_item_t>(item);
}

bool append_cause(byte_buffer& pdu, nrppa_cause_t cause)
{
  if (const auto* radio = std::get_if<nrppa_cause_radio_network_t>(&cause)) {
    return append_u8(pdu, 0) && append_u8(pdu, static_cast<uint8_t>(*radio));
  }
  if (const auto* protocol = std::get_if<nrppa_cause_protocol_t>(&cause)) {
    return append_u8(pdu, 1) && append_u8(pdu, static_cast<uint8_t>(*protocol));
  }
  return append_u8(pdu, 2) && append_u8(pdu, static_cast<uint8_t>(std::get<nrppa_cause_misc_t>(cause)));
}

nrppa_cause_t decode_cause(uint8_t type, uint8_t value)
{
  switch (type) {
    case 0:
      return static_cast<nrppa_cause_radio_network_t>(value);
    case 1:
      return static_cast<nrppa_cause_protocol_t>(value);
    case 2:
    default:
      return nrppa_cause_misc_t::unspecified;
  }
}

bool append_direct_position(byte_buffer& pdu, const ng_ran_access_point_position_t& pos)
{
  return append_u8(pdu, static_cast<uint8_t>(pos.latitude_sign)) && append_u32(pdu, pos.latitude) &&
         append_i32(pdu, pos.longitude) && append_u8(pdu, static_cast<uint8_t>(pos.direction_of_altitude)) &&
         append_u16(pdu, pos.altitude) && append_u8(pdu, pos.uncertainty_semi_major) &&
         append_u8(pdu, pos.uncertainty_semi_minor) && append_u8(pdu, pos.orientation_of_major_axis) &&
         append_u8(pdu, pos.uncertainty_altitude) && append_u8(pdu, pos.confidence);
}

bool append_geo_coordinates(byte_buffer& pdu, const geographical_coordinates_t& geo)
{
  const auto* direct = std::get_if<trp_position_direct_t>(&geo.trp_position_definition_type);
  if (direct == nullptr) {
    return false;
  }
  const auto* pos = std::get_if<ng_ran_access_point_position_t>(&direct->accuracy);
  if (pos == nullptr) {
    return false;
  }
  return append_direct_position(pdu, *pos);
}

bool append_response_item(byte_buffer& pdu, const trp_information_type_response_item_t& item)
{
  if (const auto* pci = std::get_if<pci_t>(&item)) {
    return append_u8(pdu, encode_type_item(trp_information_type_item_t::nr_pci)) && append_u16(pdu, *pci);
  }
  if (const auto* cgi = std::get_if<nr_cell_global_id_t>(&item)) {
    return append_u8(pdu, encode_type_item(trp_information_type_item_t::ng_ran_cgi)) &&
           append_u32(pdu, cgi->plmn_id.to_bcd()) && append_u64(pdu, cgi->nci.value());
  }
  if (const auto* arfcn = std::get_if<uint32_t>(&item)) {
    return append_u8(pdu, encode_type_item(trp_information_type_item_t::arfcn)) && append_u32(pdu, *arfcn);
  }
  if (const auto* geo = std::get_if<geographical_coordinates_t>(&item)) {
    return append_u8(pdu, encode_type_item(trp_information_type_item_t::geo_coord)) && append_geo_coordinates(pdu, *geo);
  }
  if (const auto* trp_type = std::get_if<trp_type_t>(&item)) {
    return append_u8(pdu, encode_type_item(trp_information_type_item_t::trp_type)) &&
           append_u8(pdu, static_cast<uint8_t>(*trp_type));
  }
  return false;
}

bool append_srs_type(byte_buffer& pdu, const srs_type_t& srs_type)
{
  if (const auto* semipersistent = std::get_if<semipersistent_srs_t>(&srs_type)) {
    return append_u8(pdu, 0) && append_u8(pdu, semipersistent->srs_res_set_id);
  }
  const auto* aperiodic = std::get_if<aperiodic_srs_t>(&srs_type);
  return aperiodic != nullptr && append_u8(pdu, 1) && append_u8(pdu, aperiodic->aperiodic ? 1U : 0U);
}

class pdu_reader
{
public:
  explicit pdu_reader(const byte_buffer& pdu) : bytes(pdu.begin(), pdu.end()) {}

  bool read_u8(uint8_t& value)
  {
    if (offset >= bytes.size()) {
      return false;
    }
    value = bytes[offset++];
    return true;
  }

  bool read_u16(uint16_t& value)
  {
    uint8_t hi;
    uint8_t lo;
    if (!read_u8(hi) || !read_u8(lo)) {
      return false;
    }
    value = (static_cast<uint16_t>(hi) << 8U) | lo;
    return true;
  }

  bool read_u32(uint32_t& value)
  {
    uint8_t b0;
    uint8_t b1;
    uint8_t b2;
    uint8_t b3;
    if (!read_u8(b0) || !read_u8(b1) || !read_u8(b2) || !read_u8(b3)) {
      return false;
    }
    value = (static_cast<uint32_t>(b0) << 24U) | (static_cast<uint32_t>(b1) << 16U) |
            (static_cast<uint32_t>(b2) << 8U) | b3;
    return true;
  }

  bool read_i32(int32_t& value)
  {
    uint32_t raw;
    if (!read_u32(raw)) {
      return false;
    }
    value = static_cast<int32_t>(raw);
    return true;
  }

  bool read_u64(uint64_t& value)
  {
    uint32_t hi;
    uint32_t lo;
    if (!read_u32(hi) || !read_u32(lo)) {
      return false;
    }
    value = (static_cast<uint64_t>(hi) << 32U) | lo;
    return true;
  }

  bool read_bytes(byte_buffer& value)
  {
    uint16_t length;
    if (!read_u16(length) || offset + length > bytes.size()) {
      return false;
    }
    value = byte_buffer::create(span<const uint8_t>{bytes.data() + offset, length}).value();
    offset += length;
    return true;
  }

  bool consumed() const { return offset == bytes.size(); }

private:
  std::vector<uint8_t> bytes;
  size_t               offset = 0;
};

expected<srs_type_t, std::string> decode_srs_type(pdu_reader& reader)
{
  uint8_t raw_type;
  if (!reader.read_u8(raw_type)) {
    return make_unexpected(std::string("truncated_srs_type"));
  }

  if (raw_type == 0) {
    uint8_t srs_res_set_id;
    if (!reader.read_u8(srs_res_set_id)) {
      return make_unexpected(std::string("truncated_semipersistent_srs_type"));
    }
    return srs_type_t{semipersistent_srs_t{srs_res_set_id, std::nullopt, std::nullopt}};
  }
  if (raw_type == 1) {
    uint8_t aperiodic;
    if (!reader.read_u8(aperiodic)) {
      return make_unexpected(std::string("truncated_aperiodic_srs_type"));
    }
    if (aperiodic > 1U) {
      return make_unexpected(std::string("unsupported_aperiodic_srs_type"));
    }
    return srs_type_t{aperiodic_srs_t{aperiodic == 1U, std::nullopt}};
  }

  return make_unexpected(std::string("unsupported_srs_type"));
}

expected<nr_cell_global_id_t, std::string> decode_cgi(pdu_reader& reader)
{
  uint32_t plmn_bcd;
  uint64_t nci;
  if (!reader.read_u32(plmn_bcd) || !reader.read_u64(nci)) {
    return make_unexpected(std::string("truncated_cgi"));
  }
  return nr_cell_global_id_t{plmn_identity::parse(bcd_helper::plmn_bcd_to_string(plmn_bcd)).value(),
                             nr_cell_identity::create(nci).value()};
}

expected<nrppa_minimal_pdu, std::string> decode_request_body(pdu_reader& reader)
{
  trp_information_request_t request;
  uint16_t                  count;

  if (!reader.read_u16(request.transaction_id) || !reader.read_u16(count)) {
    return make_unexpected(std::string("truncated_trp_request_header"));
  }
  for (unsigned i = 0; i != count; ++i) {
    uint32_t trp_id;
    if (!reader.read_u32(trp_id)) {
      return make_unexpected(std::string("truncated_trp_request_trp_id"));
    }
    request.trp_list.push_back(uint_to_trp_id(trp_id));
  }
  if (!reader.read_u16(count)) {
    return make_unexpected(std::string("truncated_trp_request_info_count"));
  }
  for (unsigned i = 0; i != count; ++i) {
    uint8_t raw_type;
    if (!reader.read_u8(raw_type)) {
      return make_unexpected(std::string("truncated_trp_request_info_type"));
    }
    auto type = decode_type_item(raw_type);
    if (!type.has_value()) {
      return make_unexpected(std::string("unsupported_trp_request_info_type"));
    }
    request.trp_info_type_list_trp_req.push_back(type.value());
  }
  return nrppa_minimal_pdu{nrppa_minimal_pdu_type::trp_information_request, std::move(request)};
}

expected<trp_information_type_response_item_t, std::string> decode_response_item(pdu_reader& reader)
{
  uint8_t raw_type;
  if (!reader.read_u8(raw_type)) {
    return make_unexpected(std::string("truncated_trp_response_item_type"));
  }
  auto type = decode_type_item(raw_type);
  if (!type.has_value()) {
    return make_unexpected(std::string("unsupported_trp_response_item_type"));
  }

  switch (type.value()) {
    case trp_information_type_item_t::nr_pci: {
      uint16_t pci;
      if (!reader.read_u16(pci)) {
        return make_unexpected(std::string("truncated_trp_response_pci"));
      }
      return static_cast<pci_t>(pci);
    }
    case trp_information_type_item_t::ng_ran_cgi: {
      uint32_t plmn_bcd;
      uint64_t nci;
      if (!reader.read_u32(plmn_bcd) || !reader.read_u64(nci)) {
        return make_unexpected(std::string("truncated_trp_response_cgi"));
      }
      return nr_cell_global_id_t{plmn_identity::parse(bcd_helper::plmn_bcd_to_string(plmn_bcd)).value(),
                                 nr_cell_identity::create(nci).value()};
    }
    case trp_information_type_item_t::arfcn: {
      uint32_t arfcn;
      if (!reader.read_u32(arfcn)) {
        return make_unexpected(std::string("truncated_trp_response_arfcn"));
      }
      return arfcn;
    }
    case trp_information_type_item_t::geo_coord: {
      ng_ran_access_point_position_t pos = {};
      uint8_t                        latitude_sign;
      uint8_t                        direction_of_altitude;
      if (!reader.read_u8(latitude_sign) || !reader.read_u32(pos.latitude) || !reader.read_i32(pos.longitude) ||
          !reader.read_u8(direction_of_altitude) || !reader.read_u16(pos.altitude) ||
          !reader.read_u8(pos.uncertainty_semi_major) || !reader.read_u8(pos.uncertainty_semi_minor) ||
          !reader.read_u8(pos.orientation_of_major_axis) || !reader.read_u8(pos.uncertainty_altitude) ||
          !reader.read_u8(pos.confidence)) {
        return make_unexpected(std::string("truncated_trp_response_geo"));
      }
      pos.latitude_sign         = static_cast<latitude_sign_t>(latitude_sign);
      pos.direction_of_altitude = static_cast<direction_of_altitude_t>(direction_of_altitude);

      geographical_coordinates_t geo;
      geo.trp_position_definition_type = trp_position_direct_t{pos};
      return geo;
    }
    case trp_information_type_item_t::trp_type: {
      uint8_t trp_type;
      if (!reader.read_u8(trp_type)) {
        return make_unexpected(std::string("truncated_trp_response_trp_type"));
      }
      return static_cast<trp_type_t>(trp_type);
    }
    default:
      return make_unexpected(std::string("unsupported_trp_response_item_type"));
  }
}

expected<nrppa_minimal_pdu, std::string> decode_response_body(pdu_reader& reader)
{
  trp_information_cu_cp_response_t response;
  uint16_t                         du_count;
  if (!reader.read_u16(response.transaction_id) || !reader.read_u16(du_count)) {
    return make_unexpected(std::string("truncated_trp_response_header"));
  }

  for (unsigned du_idx = 0; du_idx != du_count; ++du_idx) {
    uint32_t du_index_raw;
    uint16_t trp_count;
    if (!reader.read_u32(du_index_raw) || !reader.read_u16(trp_count)) {
      return make_unexpected(std::string("truncated_trp_response_du_header"));
    }

    trp_information_response_t du_response;
    for (unsigned trp_idx = 0; trp_idx != trp_count; ++trp_idx) {
      uint32_t trp_id;
      uint16_t item_count;
      if (!reader.read_u32(trp_id) || !reader.read_u16(item_count)) {
        return make_unexpected(std::string("truncated_trp_response_trp_header"));
      }
      trp_information_list_trp_response_item_t trp_item;
      trp_item.trp_info.trp_id = uint_to_trp_id(trp_id);
      for (unsigned item_idx = 0; item_idx != item_count; ++item_idx) {
        auto item = decode_response_item(reader);
        if (!item.has_value()) {
          return make_unexpected(item.error());
        }
        trp_item.trp_info.trp_info_type_resp_list.push_back(std::move(item.value()));
      }
      du_response.trp_info_list_trp_resp.push_back(std::move(trp_item));
    }
    response.trp_info_responses.emplace(static_cast<du_index_t>(du_index_raw), std::move(du_response));
  }

  return nrppa_minimal_pdu{nrppa_minimal_pdu_type::trp_information_response, std::move(response)};
}

expected<nrppa_minimal_pdu, std::string> decode_failure_body(pdu_reader& reader)
{
  trp_information_failure_t failure;
  uint8_t                   cause_type;
  uint8_t                   cause_value;
  if (!reader.read_u16(failure.transaction_id) || !reader.read_u8(cause_type) || !reader.read_u8(cause_value)) {
    return make_unexpected(std::string("truncated_trp_failure"));
  }
  failure.cause = decode_cause(cause_type, cause_value);
  return nrppa_minimal_pdu{nrppa_minimal_pdu_type::trp_information_failure, std::move(failure)};
}

expected<nrppa_minimal_pdu, std::string> decode_positioning_information_request_body(pdu_reader& reader)
{
  positioning_information_request_t request;
  request.ue_index = ue_index_t::invalid;

  uint8_t flags;
  if (!reader.read_u8(flags)) {
    return make_unexpected(std::string("truncated_positioning_information_request"));
  }
  if (flags != 0) {
    return make_unexpected(std::string("unsupported_positioning_information_request_flags"));
  }

  return nrppa_minimal_pdu{nrppa_minimal_pdu_type::positioning_information_request, std::move(request)};
}

expected<nrppa_minimal_pdu, std::string> decode_positioning_information_response_body(pdu_reader& reader)
{
  positioning_information_response_t response;

  uint8_t flags;
  if (!reader.read_u8(flags)) {
    return make_unexpected(std::string("truncated_positioning_information_response"));
  }
  if ((flags & 0xfeU) != 0) {
    return make_unexpected(std::string("unsupported_positioning_information_response_flags"));
  }
  if ((flags & 0x01U) != 0) {
    uint64_t sfn_init_time;
    if (!reader.read_u64(sfn_init_time)) {
      return make_unexpected(std::string("truncated_positioning_information_response_sfn"));
    }
    response.sfn_initialization_time = sfn_init_time;
  }

  return nrppa_minimal_pdu{nrppa_minimal_pdu_type::positioning_information_response, std::move(response)};
}

expected<nrppa_minimal_pdu, std::string> decode_positioning_information_failure_body(pdu_reader& reader)
{
  positioning_information_failure_t failure;
  uint8_t                           cause_type;
  uint8_t                           cause_value;
  if (!reader.read_u8(cause_type) || !reader.read_u8(cause_value)) {
    return make_unexpected(std::string("truncated_positioning_information_failure"));
  }
  failure.cause = decode_cause(cause_type, cause_value);
  return nrppa_minimal_pdu{nrppa_minimal_pdu_type::positioning_information_failure, std::move(failure)};
}

expected<nrppa_minimal_pdu, std::string> decode_measurement_request_body(pdu_reader& reader)
{
  measurement_request_t request;
  request.ue_index = ue_index_t::invalid;

  uint32_t lmf_meas_id;
  uint32_t ran_meas_id;
  uint16_t count;
  uint8_t  report_characteristics;
  if (!reader.read_u32(lmf_meas_id) || !reader.read_u32(ran_meas_id) || !reader.read_u16(count)) {
    return make_unexpected(std::string("truncated_measurement_request_header"));
  }
  request.lmf_meas_id = uint_to_lmf_meas_id(lmf_meas_id);
  request.ran_meas_id = uint_to_ran_meas_id(ran_meas_id);

  for (unsigned i = 0; i != count; ++i) {
    uint32_t trp_id;
    if (!reader.read_u32(trp_id)) {
      return make_unexpected(std::string("truncated_measurement_request_trp_id"));
    }
    request.trp_meas_request_list.push_back({uint_to_trp_id(trp_id)});
  }

  if (!reader.read_u8(report_characteristics) || !reader.read_u16(count)) {
    return make_unexpected(std::string("truncated_measurement_request_quantities_header"));
  }
  if (report_characteristics > static_cast<uint8_t>(report_characteristics_t::periodic)) {
    return make_unexpected(std::string("unsupported_measurement_request_report_characteristics"));
  }
  request.report_characteristics = static_cast<report_characteristics_t>(report_characteristics);

  for (unsigned i = 0; i != count; ++i) {
    uint8_t raw_quantity;
    if (!reader.read_u8(raw_quantity)) {
      return make_unexpected(std::string("truncated_measurement_request_quantity"));
    }
    auto quantity = decode_measurement_quantity_item(raw_quantity);
    if (!quantity.has_value()) {
      return make_unexpected(std::string("unsupported_measurement_request_quantity"));
    }
    request.trp_meas_quantities.push_back({quantity.value()});
  }

  return nrppa_minimal_pdu{nrppa_minimal_pdu_type::measurement_request, std::move(request)};
}

expected<nrppa_minimal_pdu, std::string> decode_measurement_response_body(pdu_reader& reader)
{
  measurement_response_t response;
  uint32_t               lmf_meas_id;
  uint32_t               ran_meas_id;
  if (!reader.read_u32(lmf_meas_id) || !reader.read_u32(ran_meas_id)) {
    return make_unexpected(std::string("truncated_measurement_response"));
  }
  response.lmf_meas_id = uint_to_lmf_meas_id(lmf_meas_id);
  response.ran_meas_id = uint_to_ran_meas_id(ran_meas_id);
  return nrppa_minimal_pdu{nrppa_minimal_pdu_type::measurement_response, std::move(response)};
}

expected<nrppa_minimal_pdu, std::string> decode_measurement_failure_body(pdu_reader& reader)
{
  measurement_failure_t failure;
  uint32_t              lmf_meas_id;
  uint32_t              ran_meas_id;
  uint8_t               cause_type;
  uint8_t               cause_value;
  if (!reader.read_u32(lmf_meas_id) || !reader.read_u32(ran_meas_id) || !reader.read_u8(cause_type) ||
      !reader.read_u8(cause_value)) {
    return make_unexpected(std::string("truncated_measurement_failure"));
  }
  failure.lmf_meas_id = uint_to_lmf_meas_id(lmf_meas_id);
  failure.ran_meas_id = uint_to_ran_meas_id(ran_meas_id);
  failure.cause       = decode_cause(cause_type, cause_value);
  return nrppa_minimal_pdu{nrppa_minimal_pdu_type::measurement_failure, std::move(failure)};
}

expected<nrppa_minimal_pdu, std::string> decode_positioning_activation_request_body(pdu_reader& reader)
{
  positioning_activation_request_t request;
  request.ue_index = ue_index_t::invalid;

  auto srs_type = decode_srs_type(reader);
  if (!srs_type.has_value()) {
    return make_unexpected(srs_type.error());
  }
  request.srs_type = std::move(srs_type.value());

  uint8_t flags;
  if (!reader.read_u8(flags)) {
    return make_unexpected(std::string("truncated_positioning_activation_request_flags"));
  }
  if ((flags & 0xfeU) != 0) {
    return make_unexpected(std::string("unsupported_positioning_activation_request_flags"));
  }
  if ((flags & 0x01U) != 0) {
    uint64_t activation_time;
    if (!reader.read_u64(activation_time)) {
      return make_unexpected(std::string("truncated_positioning_activation_request_time"));
    }
    request.activation_time = activation_time;
  }

  return nrppa_minimal_pdu{nrppa_minimal_pdu_type::positioning_activation_request, std::move(request)};
}

expected<nrppa_minimal_pdu, std::string> decode_positioning_activation_response_body(pdu_reader& reader)
{
  positioning_activation_response_t response;

  uint8_t flags;
  if (!reader.read_u8(flags)) {
    return make_unexpected(std::string("truncated_positioning_activation_response_flags"));
  }
  if ((flags & 0xfcU) != 0) {
    return make_unexpected(std::string("unsupported_positioning_activation_response_flags"));
  }
  if ((flags & 0x01U) != 0) {
    uint16_t sys_frame_num;
    if (!reader.read_u16(sys_frame_num)) {
      return make_unexpected(std::string("truncated_positioning_activation_response_sfn"));
    }
    response.sys_frame_num = sys_frame_num;
  }
  if ((flags & 0x02U) != 0) {
    uint8_t slot_num;
    if (!reader.read_u8(slot_num)) {
      return make_unexpected(std::string("truncated_positioning_activation_response_slot"));
    }
    response.slot_num = slot_num;
  }

  return nrppa_minimal_pdu{nrppa_minimal_pdu_type::positioning_activation_response, std::move(response)};
}

expected<nrppa_minimal_pdu, std::string> decode_positioning_activation_failure_body(pdu_reader& reader)
{
  positioning_activation_failure_t failure;
  uint8_t                          cause_type;
  uint8_t                          cause_value;
  if (!reader.read_u8(cause_type) || !reader.read_u8(cause_value)) {
    return make_unexpected(std::string("truncated_positioning_activation_failure"));
  }
  failure.cause = decode_cause(cause_type, cause_value);
  return nrppa_minimal_pdu{nrppa_minimal_pdu_type::positioning_activation_failure, std::move(failure)};
}

expected<nrppa_minimal_pdu, std::string> decode_positioning_deactivation_request_body(pdu_reader& reader)
{
  positioning_deactivation_request_t request;
  request.ue_index = ue_index_t::invalid;

  uint8_t flags;
  if (!reader.read_u8(flags)) {
    return make_unexpected(std::string("truncated_positioning_deactivation_request_flags"));
  }
  if ((flags & 0xfeU) != 0) {
    return make_unexpected(std::string("unsupported_positioning_deactivation_request_flags"));
  }
  if ((flags & 0x01U) != 0) {
    uint8_t srs_res_set_id;
    if (!reader.read_u8(srs_res_set_id)) {
      return make_unexpected(std::string("truncated_positioning_deactivation_request_srs_res_set"));
    }
    request.srs_res_set_id = srs_res_set_id;
  }

  return nrppa_minimal_pdu{nrppa_minimal_pdu_type::positioning_deactivation_request, std::move(request)};
}

expected<nrppa_minimal_pdu, std::string> decode_positioning_deactivation_response_body(pdu_reader& reader)
{
  if (!reader.consumed()) {
    return make_unexpected(std::string("unsupported_positioning_deactivation_response_body"));
  }
  return nrppa_minimal_pdu{nrppa_minimal_pdu_type::positioning_deactivation_response,
                           positioning_deactivation_response_t{}};
}

expected<nrppa_minimal_pdu, std::string> decode_positioning_deactivation_failure_body(pdu_reader& reader)
{
  positioning_deactivation_failure_t failure;
  uint8_t                            cause_type;
  uint8_t                            cause_value;
  if (!reader.read_u8(cause_type) || !reader.read_u8(cause_value)) {
    return make_unexpected(std::string("truncated_positioning_deactivation_failure"));
  }
  failure.cause = decode_cause(cause_type, cause_value);
  return nrppa_minimal_pdu{nrppa_minimal_pdu_type::positioning_deactivation_failure, std::move(failure)};
}

expected<nrppa_minimal_pdu, std::string>
decode_positioning_assistance_information_control_request_body(pdu_reader& reader)
{
  positioning_assistance_information_control_request_t request;
  uint8_t                                             flags;
  if (!reader.read_u16(request.transaction_id) || !reader.read_u8(flags)) {
    return make_unexpected(std::string("truncated_positioning_assistance_control_header"));
  }
  if ((flags & 0xf0U) != 0) {
    return make_unexpected(std::string("unsupported_positioning_assistance_control_flags"));
  }
  if ((flags & 0x01U) != 0) {
    byte_buffer pos_assist_info;
    if (!reader.read_bytes(pos_assist_info)) {
      return make_unexpected(std::string("truncated_positioning_assistance_control_pos_assist_info"));
    }
    request.pos_assist_info = std::move(pos_assist_info);
  }
  if ((flags & 0x02U) != 0) {
    uint8_t action;
    if (!reader.read_u8(action) || action > 1U) {
      return make_unexpected(std::string("unsupported_positioning_assistance_control_broadcast"));
    }
    request.pos_broadcast =
        action == 0 ? positioning_assistance_broadcast_action::start : positioning_assistance_broadcast_action::stop;
  }
  if ((flags & 0x04U) != 0) {
    uint16_t count;
    if (!reader.read_u16(count)) {
      return make_unexpected(std::string("truncated_positioning_assistance_control_cell_count"));
    }
    for (unsigned i = 0; i != count; ++i) {
      auto cgi = decode_cgi(reader);
      if (!cgi.has_value()) {
        return make_unexpected(std::string("truncated_positioning_assistance_control_cell"));
      }
      request.positioning_broadcast_cells.push_back(cgi.value());
    }
  }
  if ((flags & 0x08U) != 0) {
    byte_buffer routing_id;
    if (!reader.read_bytes(routing_id)) {
      return make_unexpected(std::string("truncated_positioning_assistance_control_routing_id"));
    }
    request.routing_id = std::move(routing_id);
  }

  return nrppa_minimal_pdu{nrppa_minimal_pdu_type::positioning_assistance_information_control_request,
                           std::move(request)};
}

expected<nrppa_minimal_pdu, std::string> decode_positioning_assistance_information_feedback_body(pdu_reader& reader)
{
  positioning_assistance_information_feedback_t feedback;
  uint8_t                                      flags;
  if (!reader.read_u16(feedback.transaction_id) || !reader.read_u8(flags)) {
    return make_unexpected(std::string("truncated_positioning_assistance_feedback_header"));
  }
  if ((flags & 0xf8U) != 0) {
    return make_unexpected(std::string("unsupported_positioning_assistance_feedback_flags"));
  }
  if ((flags & 0x01U) != 0) {
    byte_buffer fail_list;
    if (!reader.read_bytes(fail_list)) {
      return make_unexpected(std::string("truncated_positioning_assistance_feedback_fail_list"));
    }
    feedback.pos_assist_info_fail_list = std::move(fail_list);
  }
  if ((flags & 0x02U) != 0) {
    uint16_t count;
    if (!reader.read_u16(count)) {
      return make_unexpected(std::string("truncated_positioning_assistance_feedback_cell_count"));
    }
    for (unsigned i = 0; i != count; ++i) {
      auto cgi = decode_cgi(reader);
      if (!cgi.has_value()) {
        return make_unexpected(std::string("truncated_positioning_assistance_feedback_cell"));
      }
      feedback.positioning_broadcast_cells.push_back(cgi.value());
    }
  }
  if ((flags & 0x04U) != 0) {
    byte_buffer routing_id;
    if (!reader.read_bytes(routing_id)) {
      return make_unexpected(std::string("truncated_positioning_assistance_feedback_routing_id"));
    }
    feedback.routing_id = std::move(routing_id);
  }

  return nrppa_minimal_pdu{nrppa_minimal_pdu_type::positioning_assistance_information_feedback,
                           std::move(feedback)};
}

expected<nrppa_minimal_pdu, std::string> decode_positioning_assistance_information_failure_body(pdu_reader& reader)
{
  positioning_assistance_information_failure_t failure;
  uint8_t                                      cause_type;
  uint8_t                                      cause_value;
  if (!reader.read_u16(failure.transaction_id) || !reader.read_u8(cause_type) || !reader.read_u8(cause_value)) {
    return make_unexpected(std::string("truncated_positioning_assistance_failure"));
  }
  failure.cause = decode_cause(cause_type, cause_value);
  return nrppa_minimal_pdu{nrppa_minimal_pdu_type::positioning_assistance_information_failure, std::move(failure)};
}

enum class standard_nrppa_pdu_outcome : uint8_t { initiating_message = 0, successful_outcome = 1, unsuccessful_outcome = 2 };

struct standard_nrppa_pdu_header {
  standard_nrppa_pdu_outcome outcome;
  uint8_t                    procedure_code;
  uint8_t                    criticality;
  uint16_t                   transaction_id;
};

constexpr uint8_t  standard_nrppa_criticality_reject       = 0;
constexpr uint8_t  standard_nrppa_procedure_trp_information = 16;
constexpr uint16_t standard_nrppa_max_transaction_id        = 32767;

bool append_standard_header(byte_buffer& pdu, standard_nrppa_pdu_outcome outcome, uint16_t transaction_id)
{
  return append_u8(pdu, static_cast<uint8_t>(outcome)) &&
         append_u8(pdu, standard_nrppa_procedure_trp_information) &&
         append_u8(pdu, standard_nrppa_criticality_reject) && append_u16(pdu, transaction_id);
}

expected<standard_nrppa_pdu_header, std::string> decode_standard_header(pdu_reader& reader)
{
  uint8_t  raw_outcome;
  uint8_t  procedure_code;
  uint8_t  criticality;
  uint16_t transaction_id;
  if (!reader.read_u8(raw_outcome) || !reader.read_u8(procedure_code) || !reader.read_u8(criticality) ||
      !reader.read_u16(transaction_id)) {
    return make_unexpected(std::string("standard_nrppa_truncated_header"));
  }
  if (raw_outcome > static_cast<uint8_t>(standard_nrppa_pdu_outcome::unsuccessful_outcome)) {
    return make_unexpected(std::string("standard_nrppa_invalid_pdu_choice"));
  }
  if (procedure_code != standard_nrppa_procedure_trp_information) {
    return make_unexpected(std::string("standard_nrppa_unsupported_procedure"));
  }
  if (criticality != standard_nrppa_criticality_reject) {
    return make_unexpected(std::string("standard_nrppa_unsupported_criticality"));
  }
  if (transaction_id > standard_nrppa_max_transaction_id) {
    return make_unexpected(std::string("standard_nrppa_invalid_transaction_id"));
  }
  return standard_nrppa_pdu_header{static_cast<standard_nrppa_pdu_outcome>(raw_outcome),
                                   procedure_code,
                                   criticality,
                                   transaction_id};
}

expected<nrppa_minimal_pdu, std::string> decode_standard_trp_request_body(pdu_reader& reader, uint16_t transaction_id)
{
  trp_information_request_t request;
  request.transaction_id = transaction_id;

  uint16_t trp_count;
  if (!reader.read_u16(trp_count)) {
    return make_unexpected(std::string("standard_trp_request_truncated_trp_list"));
  }
  for (unsigned i = 0; i != trp_count; ++i) {
    uint32_t trp_id;
    if (!reader.read_u32(trp_id)) {
      return make_unexpected(std::string("standard_trp_request_truncated_trp_id"));
    }
    request.trp_list.push_back(uint_to_trp_id(trp_id));
  }

  uint16_t info_count;
  if (!reader.read_u16(info_count)) {
    return make_unexpected(std::string("standard_trp_request_truncated_info_list"));
  }
  if (info_count == 0) {
    return make_unexpected(std::string("standard_trp_request_missing_info_type"));
  }
  for (unsigned i = 0; i != info_count; ++i) {
    uint8_t raw_type;
    if (!reader.read_u8(raw_type)) {
      return make_unexpected(std::string("standard_trp_request_truncated_info_type"));
    }
    auto type = decode_type_item(raw_type);
    if (!type.has_value()) {
      return make_unexpected(std::string("standard_trp_request_unsupported_info_type"));
    }
    request.trp_info_type_list_trp_req.push_back(type.value());
  }
  return nrppa_minimal_pdu{nrppa_minimal_pdu_type::trp_information_request, std::move(request)};
}

expected<nrppa_minimal_pdu, std::string> decode_standard_trp_response_body(pdu_reader& reader, uint16_t transaction_id)
{
  trp_information_cu_cp_response_t response;
  response.transaction_id = transaction_id;

  uint16_t trp_count;
  if (!reader.read_u16(trp_count)) {
    return make_unexpected(std::string("standard_trp_response_truncated_trp_list"));
  }

  trp_information_response_t du_response;
  for (unsigned trp_idx = 0; trp_idx != trp_count; ++trp_idx) {
    uint32_t trp_id;
    uint16_t item_count;
    if (!reader.read_u32(trp_id) || !reader.read_u16(item_count)) {
      return make_unexpected(std::string("standard_trp_response_truncated_trp_header"));
    }

    trp_information_list_trp_response_item_t trp_item;
    trp_item.trp_info.trp_id = uint_to_trp_id(trp_id);
    for (unsigned item_idx = 0; item_idx != item_count; ++item_idx) {
      auto item = decode_response_item(reader);
      if (!item.has_value()) {
        return make_unexpected(std::string("standard_") + item.error());
      }
      trp_item.trp_info.trp_info_type_resp_list.push_back(std::move(item.value()));
    }
    du_response.trp_info_list_trp_resp.push_back(std::move(trp_item));
  }

  response.trp_info_responses.emplace(du_index_t::min, std::move(du_response));
  return nrppa_minimal_pdu{nrppa_minimal_pdu_type::trp_information_response, std::move(response)};
}

expected<nrppa_minimal_pdu, std::string> decode_standard_trp_failure_body(pdu_reader& reader, uint16_t transaction_id)
{
  trp_information_failure_t failure;
  failure.transaction_id = transaction_id;
  uint8_t cause_type;
  uint8_t cause_value;
  if (!reader.read_u8(cause_type) || !reader.read_u8(cause_value)) {
    return make_unexpected(std::string("standard_trp_failure_truncated_cause"));
  }
  failure.cause = decode_cause(cause_type, cause_value);
  return nrppa_minimal_pdu{nrppa_minimal_pdu_type::trp_information_failure, std::move(failure)};
}

uint16_t count_supported_standard_trp_response_items(const std::vector<trp_information_type_response_item_t>& items)
{
  uint16_t count = 0;
  for (const auto& item : items) {
    if (std::holds_alternative<pci_t>(item) || std::holds_alternative<nr_cell_global_id_t>(item) ||
        std::holds_alternative<uint32_t>(item) || std::holds_alternative<geographical_coordinates_t>(item) ||
        std::holds_alternative<trp_type_t>(item)) {
      ++count;
    }
  }
  return count;
}

} // namespace

bool srsran::srs_cu_cp::is_nrppa_minimal_pdu(const byte_buffer& pdu)
{
  if (pdu.length() < pdu_magic.size()) {
    return false;
  }
  for (unsigned i = 0; i != pdu_magic.size(); ++i) {
    if (pdu[i] != pdu_magic[i]) {
      return false;
    }
  }
  return true;
}

byte_buffer srsran::srs_cu_cp::encode_nrppa_standard_trp_information_request(
    const trp_information_request_t& request)
{
  byte_buffer pdu;
  append_standard_header(pdu, standard_nrppa_pdu_outcome::initiating_message, request.transaction_id);
  append_u16(pdu, static_cast<uint16_t>(request.trp_list.size()));
  for (trp_id_t trp_id : request.trp_list) {
    append_u32(pdu, trp_id_to_uint(trp_id));
  }
  append_u16(pdu, static_cast<uint16_t>(request.trp_info_type_list_trp_req.size()));
  for (trp_information_type_item_t type : request.trp_info_type_list_trp_req) {
    append_u8(pdu, encode_type_item(type));
  }
  return pdu;
}

byte_buffer srsran::srs_cu_cp::encode_nrppa_standard_trp_information_response(
    const trp_information_cu_cp_response_t& response)
{
  byte_buffer pdu;
  append_standard_header(pdu, standard_nrppa_pdu_outcome::successful_outcome, response.transaction_id);

  uint16_t trp_count = 0;
  for (const auto& du_response_pair : response.trp_info_responses) {
    trp_count += static_cast<uint16_t>(du_response_pair.second.trp_info_list_trp_resp.size());
  }
  append_u16(pdu, trp_count);

  for (const auto& du_response_pair : response.trp_info_responses) {
    for (const auto& trp_item : du_response_pair.second.trp_info_list_trp_resp) {
      append_u32(pdu, trp_id_to_uint(trp_item.trp_info.trp_id));
      append_u16(pdu, count_supported_standard_trp_response_items(trp_item.trp_info.trp_info_type_resp_list));
      for (const auto& item : trp_item.trp_info.trp_info_type_resp_list) {
        append_response_item(pdu, item);
      }
    }
  }
  return pdu;
}

byte_buffer srsran::srs_cu_cp::encode_nrppa_standard_trp_information_failure(
    const trp_information_failure_t& failure)
{
  byte_buffer pdu;
  append_standard_header(pdu, standard_nrppa_pdu_outcome::unsuccessful_outcome, failure.transaction_id);
  append_cause(pdu, failure.cause);
  return pdu;
}

expected<nrppa_minimal_pdu, std::string> srsran::srs_cu_cp::decode_nrppa_standard_pdu(const byte_buffer& pdu)
{
  pdu_reader reader(pdu);
  auto       header = decode_standard_header(reader);
  if (!header.has_value()) {
    return make_unexpected(header.error());
  }

  expected<nrppa_minimal_pdu, std::string> decoded = make_unexpected(std::string("standard_nrppa_unsupported_outcome"));
  switch (header.value().outcome) {
    case standard_nrppa_pdu_outcome::initiating_message:
      decoded = decode_standard_trp_request_body(reader, header.value().transaction_id);
      break;
    case standard_nrppa_pdu_outcome::successful_outcome:
      decoded = decode_standard_trp_response_body(reader, header.value().transaction_id);
      break;
    case standard_nrppa_pdu_outcome::unsuccessful_outcome:
      decoded = decode_standard_trp_failure_body(reader, header.value().transaction_id);
      break;
  }
  if (!decoded.has_value()) {
    return decoded;
  }
  if (!reader.consumed()) {
    return make_unexpected(std::string("standard_nrppa_trailing_bytes"));
  }
  return decoded;
}

byte_buffer srsran::srs_cu_cp::encode_nrppa_trp_information_request(const trp_information_request_t& request)
{
  byte_buffer pdu;
  append_header(pdu, pdu_message_type::trp_information_request);
  append_u16(pdu, request.transaction_id);
  append_u16(pdu, static_cast<uint16_t>(request.trp_list.size()));
  for (trp_id_t trp_id : request.trp_list) {
    append_u32(pdu, trp_id_to_uint(trp_id));
  }
  append_u16(pdu, static_cast<uint16_t>(request.trp_info_type_list_trp_req.size()));
  for (trp_information_type_item_t type : request.trp_info_type_list_trp_req) {
    append_u8(pdu, encode_type_item(type));
  }
  return pdu;
}

byte_buffer srsran::srs_cu_cp::encode_nrppa_trp_information_response(const trp_information_cu_cp_response_t& response)
{
  byte_buffer pdu;
  append_header(pdu, pdu_message_type::trp_information_response);
  append_u16(pdu, response.transaction_id);
  append_u16(pdu, static_cast<uint16_t>(response.trp_info_responses.size()));
  for (const auto& du_response_pair : response.trp_info_responses) {
    append_u32(pdu, static_cast<uint32_t>(du_response_pair.first));
    append_u16(pdu, static_cast<uint16_t>(du_response_pair.second.trp_info_list_trp_resp.size()));
    for (const auto& trp_item : du_response_pair.second.trp_info_list_trp_resp) {
      append_u32(pdu, trp_id_to_uint(trp_item.trp_info.trp_id));
      uint16_t encoded_item_count = 0;
      for (const auto& item : trp_item.trp_info.trp_info_type_resp_list) {
        if (std::holds_alternative<pci_t>(item) || std::holds_alternative<nr_cell_global_id_t>(item) ||
            std::holds_alternative<uint32_t>(item) || std::holds_alternative<geographical_coordinates_t>(item) ||
            std::holds_alternative<trp_type_t>(item)) {
          ++encoded_item_count;
        }
      }
      append_u16(pdu, encoded_item_count);
      for (const auto& item : trp_item.trp_info.trp_info_type_resp_list) {
        append_response_item(pdu, item);
      }
    }
  }
  return pdu;
}

byte_buffer srsran::srs_cu_cp::encode_nrppa_trp_information_failure(const trp_information_failure_t& failure)
{
  byte_buffer pdu;
  append_header(pdu, pdu_message_type::trp_information_failure);
  append_u16(pdu, failure.transaction_id);
  append_cause(pdu, failure.cause);
  return pdu;
}

byte_buffer
srsran::srs_cu_cp::encode_nrppa_positioning_information_request(const positioning_information_request_t& request)
{
  (void)request;
  byte_buffer pdu;
  append_header(pdu, pdu_message_type::positioning_information_request);
  append_u8(pdu, 0);
  return pdu;
}

byte_buffer
srsran::srs_cu_cp::encode_nrppa_positioning_information_response(const positioning_information_response_t& response)
{
  byte_buffer pdu;
  append_header(pdu, pdu_message_type::positioning_information_response);
  uint8_t flags = response.sfn_initialization_time.has_value() ? 0x01U : 0x00U;
  append_u8(pdu, flags);
  if (response.sfn_initialization_time.has_value()) {
    append_u64(pdu, response.sfn_initialization_time.value());
  }
  return pdu;
}

byte_buffer
srsran::srs_cu_cp::encode_nrppa_positioning_information_failure(const positioning_information_failure_t& failure)
{
  byte_buffer pdu;
  append_header(pdu, pdu_message_type::positioning_information_failure);
  append_cause(pdu, failure.cause);
  return pdu;
}

byte_buffer srsran::srs_cu_cp::encode_nrppa_measurement_request(const measurement_request_t& request)
{
  byte_buffer pdu;
  append_header(pdu, pdu_message_type::measurement_request);
  append_u32(pdu, lmf_meas_id_to_uint(request.lmf_meas_id));
  append_u32(pdu, ran_meas_id_to_uint(request.ran_meas_id));
  append_u16(pdu, static_cast<uint16_t>(request.trp_meas_request_list.size()));
  for (const auto& trp_request : request.trp_meas_request_list) {
    append_u32(pdu, trp_id_to_uint(trp_request.trp_id));
  }
  append_u8(pdu, static_cast<uint8_t>(request.report_characteristics));
  append_u16(pdu, static_cast<uint16_t>(request.trp_meas_quantities.size()));
  for (const auto& quantity : request.trp_meas_quantities) {
    append_u8(pdu, encode_measurement_quantity_item(quantity.trp_meas_quantities_item));
  }
  return pdu;
}

byte_buffer srsran::srs_cu_cp::encode_nrppa_measurement_response(const measurement_response_t& response)
{
  byte_buffer pdu;
  append_header(pdu, pdu_message_type::measurement_response);
  append_u32(pdu, lmf_meas_id_to_uint(response.lmf_meas_id));
  append_u32(pdu, ran_meas_id_to_uint(response.ran_meas_id));
  return pdu;
}

byte_buffer srsran::srs_cu_cp::encode_nrppa_measurement_failure(const measurement_failure_t& failure)
{
  byte_buffer pdu;
  append_header(pdu, pdu_message_type::measurement_failure);
  append_u32(pdu, lmf_meas_id_to_uint(failure.lmf_meas_id));
  append_u32(pdu, ran_meas_id_to_uint(failure.ran_meas_id));
  append_cause(pdu, failure.cause);
  return pdu;
}

byte_buffer
srsran::srs_cu_cp::encode_nrppa_positioning_activation_request(const positioning_activation_request_t& request)
{
  byte_buffer pdu;
  append_header(pdu, pdu_message_type::positioning_activation_request);
  append_srs_type(pdu, request.srs_type);
  const uint8_t flags = request.activation_time.has_value() ? 0x01U : 0x00U;
  append_u8(pdu, flags);
  if (request.activation_time.has_value()) {
    append_u64(pdu, request.activation_time.value());
  }
  return pdu;
}

byte_buffer
srsran::srs_cu_cp::encode_nrppa_positioning_activation_response(const positioning_activation_response_t& response)
{
  byte_buffer pdu;
  append_header(pdu, pdu_message_type::positioning_activation_response);
  const uint8_t flags = (response.sys_frame_num.has_value() ? 0x01U : 0x00U) |
                        (response.slot_num.has_value() ? 0x02U : 0x00U);
  append_u8(pdu, flags);
  if (response.sys_frame_num.has_value()) {
    append_u16(pdu, response.sys_frame_num.value());
  }
  if (response.slot_num.has_value()) {
    append_u8(pdu, response.slot_num.value());
  }
  return pdu;
}

byte_buffer
srsran::srs_cu_cp::encode_nrppa_positioning_activation_failure(const positioning_activation_failure_t& failure)
{
  byte_buffer pdu;
  append_header(pdu, pdu_message_type::positioning_activation_failure);
  append_cause(pdu, failure.cause);
  return pdu;
}

byte_buffer
srsran::srs_cu_cp::encode_nrppa_positioning_deactivation_request(const positioning_deactivation_request_t& request)
{
  byte_buffer pdu;
  append_header(pdu, pdu_message_type::positioning_deactivation_request);
  const uint8_t flags = request.srs_res_set_id.has_value() ? 0x01U : 0x00U;
  append_u8(pdu, flags);
  if (request.srs_res_set_id.has_value()) {
    append_u8(pdu, request.srs_res_set_id.value());
  }
  return pdu;
}

byte_buffer
srsran::srs_cu_cp::encode_nrppa_positioning_deactivation_response(const positioning_deactivation_response_t& response)
{
  (void)response;
  byte_buffer pdu;
  append_header(pdu, pdu_message_type::positioning_deactivation_response);
  return pdu;
}

byte_buffer
srsran::srs_cu_cp::encode_nrppa_positioning_deactivation_failure(const positioning_deactivation_failure_t& failure)
{
  byte_buffer pdu;
  append_header(pdu, pdu_message_type::positioning_deactivation_failure);
  append_cause(pdu, failure.cause);
  return pdu;
}

byte_buffer srsran::srs_cu_cp::encode_nrppa_positioning_assistance_information_control_request(
    const positioning_assistance_information_control_request_t& request)
{
  byte_buffer pdu;
  append_header(pdu, pdu_message_type::positioning_assistance_information_control_request);
  append_u16(pdu, request.transaction_id);
  const uint8_t flags = (request.pos_assist_info.has_value() ? 0x01U : 0x00U) |
                        (request.pos_broadcast.has_value() ? 0x02U : 0x00U) |
                        (!request.positioning_broadcast_cells.empty() ? 0x04U : 0x00U) |
                        (request.routing_id.has_value() ? 0x08U : 0x00U);
  append_u8(pdu, flags);
  if (request.pos_assist_info.has_value()) {
    append_bytes(pdu, request.pos_assist_info.value());
  }
  if (request.pos_broadcast.has_value()) {
    append_u8(pdu, request.pos_broadcast.value() == positioning_assistance_broadcast_action::start ? 0U : 1U);
  }
  if (!request.positioning_broadcast_cells.empty()) {
    append_u16(pdu, static_cast<uint16_t>(request.positioning_broadcast_cells.size()));
    for (const nr_cell_global_id_t& cgi : request.positioning_broadcast_cells) {
      append_cgi(pdu, cgi);
    }
  }
  if (request.routing_id.has_value()) {
    append_bytes(pdu, request.routing_id.value());
  }
  return pdu;
}

byte_buffer srsran::srs_cu_cp::encode_nrppa_positioning_assistance_information_feedback(
    const positioning_assistance_information_feedback_t& feedback)
{
  byte_buffer pdu;
  append_header(pdu, pdu_message_type::positioning_assistance_information_feedback);
  append_u16(pdu, feedback.transaction_id);
  const uint8_t flags = (feedback.pos_assist_info_fail_list.has_value() ? 0x01U : 0x00U) |
                        (!feedback.positioning_broadcast_cells.empty() ? 0x02U : 0x00U) |
                        (feedback.routing_id.has_value() ? 0x04U : 0x00U);
  append_u8(pdu, flags);
  if (feedback.pos_assist_info_fail_list.has_value()) {
    append_bytes(pdu, feedback.pos_assist_info_fail_list.value());
  }
  if (!feedback.positioning_broadcast_cells.empty()) {
    append_u16(pdu, static_cast<uint16_t>(feedback.positioning_broadcast_cells.size()));
    for (const nr_cell_global_id_t& cgi : feedback.positioning_broadcast_cells) {
      append_cgi(pdu, cgi);
    }
  }
  if (feedback.routing_id.has_value()) {
    append_bytes(pdu, feedback.routing_id.value());
  }
  return pdu;
}

byte_buffer srsran::srs_cu_cp::encode_nrppa_positioning_assistance_information_failure(
    const positioning_assistance_information_failure_t& failure)
{
  byte_buffer pdu;
  append_header(pdu, pdu_message_type::positioning_assistance_information_failure);
  append_u16(pdu, failure.transaction_id);
  append_cause(pdu, failure.cause);
  return pdu;
}

expected<nrppa_minimal_pdu, std::string> srsran::srs_cu_cp::decode_nrppa_pdu(const byte_buffer& pdu)
{
  pdu_reader reader(pdu);
  for (uint8_t magic_byte : pdu_magic) {
    uint8_t value;
    if (!reader.read_u8(value) || value != magic_byte) {
      return make_unexpected(std::string("invalid_nrppa_minimal_pdu_magic"));
    }
  }

  uint8_t version;
  uint8_t raw_type;
  if (!reader.read_u8(version) || !reader.read_u8(raw_type)) {
    return make_unexpected(std::string("truncated_nrppa_minimal_pdu_header"));
  }
  if (version != pdu_version) {
    return make_unexpected(std::string("unsupported_nrppa_minimal_pdu_version"));
  }

  switch (static_cast<pdu_message_type>(raw_type)) {
    case pdu_message_type::trp_information_request:
      return decode_request_body(reader);
    case pdu_message_type::trp_information_response:
      return decode_response_body(reader);
    case pdu_message_type::trp_information_failure:
      return decode_failure_body(reader);
    case pdu_message_type::positioning_information_request:
      return decode_positioning_information_request_body(reader);
    case pdu_message_type::positioning_information_response:
      return decode_positioning_information_response_body(reader);
    case pdu_message_type::positioning_information_failure:
      return decode_positioning_information_failure_body(reader);
    case pdu_message_type::measurement_request:
      return decode_measurement_request_body(reader);
    case pdu_message_type::measurement_response:
      return decode_measurement_response_body(reader);
    case pdu_message_type::measurement_failure:
      return decode_measurement_failure_body(reader);
    case pdu_message_type::positioning_activation_request:
      return decode_positioning_activation_request_body(reader);
    case pdu_message_type::positioning_activation_response:
      return decode_positioning_activation_response_body(reader);
    case pdu_message_type::positioning_activation_failure:
      return decode_positioning_activation_failure_body(reader);
    case pdu_message_type::positioning_deactivation_request:
      return decode_positioning_deactivation_request_body(reader);
    case pdu_message_type::positioning_deactivation_response:
      return decode_positioning_deactivation_response_body(reader);
    case pdu_message_type::positioning_deactivation_failure:
      return decode_positioning_deactivation_failure_body(reader);
    case pdu_message_type::positioning_assistance_information_control_request:
      return decode_positioning_assistance_information_control_request_body(reader);
    case pdu_message_type::positioning_assistance_information_feedback:
      return decode_positioning_assistance_information_feedback_body(reader);
    case pdu_message_type::positioning_assistance_information_failure:
      return decode_positioning_assistance_information_failure_body(reader);
    default:
      return make_unexpected(std::string("unsupported_nrppa_minimal_pdu_type"));
  }
}
