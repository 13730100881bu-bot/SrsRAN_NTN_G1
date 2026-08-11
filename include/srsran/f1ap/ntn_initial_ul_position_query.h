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

#include "srsran/adt/byte_buffer.h"
#include "srsran/f1ap/f1ap_ue_id_types.h"
#include "srsran/ran/du_types.h"
#include "srsran/ran/gnb_du_id.h"
#include "srsran/ran/nr_cgi.h"
#include "srsran/ran/pci.h"
#include "srsran/ran/rnti.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace srsran {

/// Authority behind one DU-side Initial UL position observation.
enum class f1ap_ntn_initial_ul_position_authority : uint8_t {
  none                 = 0,
  software_attributed  = 1,
  sdr_rx_port_verified = 2,
  ofh_beam_id_verified = 3,
  invalid              = 255
};

/// Exact target of a private Initial UL position query.
struct f1ap_ntn_initial_ul_position_query {
  uint32_t                query_generation         = 0;
  uint64_t                nonce                    = 0;
  uint64_t                connection_token         = 0;
  gnb_du_id_t             gnb_du_id                = gnb_du_id_t::invalid;
  nr_cell_global_id_t     cell_cgi;
  du_cell_index_t         cell_index               = INVALID_DU_CELL_INDEX;
  pci_t                   pci                      = INVALID_PCI;
  gnb_du_ue_f1ap_id_t     gnb_du_ue_f1ap_id        = gnb_du_ue_f1ap_id_t::invalid;
  rnti_t                  c_rnti                   = rnti_t::INVALID_RNTI;
  uint32_t                expected_rnti_generation = 0;
};

/// Private DU response that echoes the complete query target before reporting an observation.
struct f1ap_ntn_initial_ul_position_result {
  uint32_t                query_generation         = 0;
  uint64_t                nonce                    = 0;
  uint64_t                connection_token         = 0;
  gnb_du_id_t             gnb_du_id                = gnb_du_id_t::invalid;
  nr_cell_global_id_t     cell_cgi;
  du_cell_index_t         cell_index               = INVALID_DU_CELL_INDEX;
  pci_t                   pci                      = INVALID_PCI;
  gnb_du_ue_f1ap_id_t     gnb_du_ue_f1ap_id        = gnb_du_ue_f1ap_id_t::invalid;
  rnti_t                  c_rnti                   = rnti_t::INVALID_RNTI;
  uint32_t                expected_rnti_generation = 0;

  uint64_t                               observation_id = 0;
  bool                                   accepted  = false;
  f1ap_ntn_initial_ul_position_authority authority = f1ap_ntn_initial_ul_position_authority::invalid;
  std::string                            reason;
  uint64_t                               schedule_version = 0;
  std::string                            calendar_hash;
  uint64_t                               mapping_version = 0;
  std::string                            mapping_hash;
  std::string                            position_id;
  uint16_t                               logical_port  = 0;
  uint16_t                               physical_port = 0;
  std::optional<uint16_t>                eaxc;
  std::optional<uint16_t>                beam_id;
  uint64_t                               calendar_cycle_index = 0;
  uint32_t                               occasion_offset_us   = 0;
  float                                  confidence_margin_db = 0.0F;
};

namespace f1ap_ntn_initial_ul_position_detail {

static constexpr std::array<uint8_t, 8> query_magic  = {'N', 'T', 'P', 'O', 'S', 'Q', '0', '1'};
static constexpr std::array<uint8_t, 8> result_magic = {'N', 'T', 'P', 'O', 'S', 'R', '0', '1'};

static constexpr size_t max_container_size  = 1024;
static constexpr size_t max_reason_length   = 96;
static constexpr size_t max_hash_length     = 128;
static constexpr size_t max_position_length = 64;
static constexpr uint16_t max_logical_port_id = std::numeric_limits<uint16_t>::max() - 1U;
static constexpr uint16_t max_physical_port_id = 254U;
static constexpr uint16_t unavailable_physical_port_id = std::numeric_limits<uint16_t>::max();
// Backward-compatible name used by existing logical-port boundary tests.
static constexpr uint16_t max_port_id = max_logical_port_id;
static constexpr uint16_t max_eaxc_id        = 31;
static constexpr uint16_t max_beam_id        = 0x7fff;
static constexpr int32_t min_confidence_centi_db = -12000;
static constexpr int32_t max_confidence_centi_db = 12000;

class writer
{
public:
  void bytes(const uint8_t* begin, const uint8_t* end) { payload.insert(payload.end(), begin, end); }
  void u8(uint8_t value) { payload.push_back(value); }
  void u16(uint16_t value)
  {
    payload.push_back(static_cast<uint8_t>((value >> 8U) & 0xffU));
    payload.push_back(static_cast<uint8_t>(value & 0xffU));
  }
  void u32(uint32_t value)
  {
    payload.push_back(static_cast<uint8_t>((value >> 24U) & 0xffU));
    payload.push_back(static_cast<uint8_t>((value >> 16U) & 0xffU));
    payload.push_back(static_cast<uint8_t>((value >> 8U) & 0xffU));
    payload.push_back(static_cast<uint8_t>(value & 0xffU));
  }
  void u64(uint64_t value)
  {
    for (int shift = 56; shift >= 0; shift -= 8) {
      payload.push_back(static_cast<uint8_t>((value >> static_cast<unsigned>(shift)) & 0xffU));
    }
  }
  void string(const std::string& value)
  {
    u16(static_cast<uint16_t>(value.size()));
    payload.insert(payload.end(), value.begin(), value.end());
  }
  size_t size() const { return payload.size(); }
  byte_buffer finish() const
  {
    return byte_buffer::create(span<const uint8_t>(payload.data(), payload.size())).value();
  }

private:
  std::vector<uint8_t> payload;
};

class reader
{
public:
  explicit reader(const byte_buffer& container_) : container(container_) {}

  bool bytes(const uint8_t* expected, size_t count)
  {
    if (!has(count)) {
      return false;
    }
    for (size_t i = 0; i != count; ++i) {
      if (container[offset + i] != expected[i]) {
        return false;
      }
    }
    offset += count;
    return true;
  }
  bool u8(uint8_t& value)
  {
    if (!has(1)) {
      return false;
    }
    value = container[offset++];
    return true;
  }
  bool u16(uint16_t& value)
  {
    if (!has(2)) {
      return false;
    }
    value = (static_cast<uint16_t>(container[offset]) << 8U) | static_cast<uint16_t>(container[offset + 1]);
    offset += 2;
    return true;
  }
  bool u32(uint32_t& value)
  {
    if (!has(4)) {
      return false;
    }
    value = (static_cast<uint32_t>(container[offset]) << 24U) |
            (static_cast<uint32_t>(container[offset + 1]) << 16U) |
            (static_cast<uint32_t>(container[offset + 2]) << 8U) | static_cast<uint32_t>(container[offset + 3]);
    offset += 4;
    return true;
  }
  bool u64(uint64_t& value)
  {
    if (!has(8)) {
      return false;
    }
    value = 0;
    for (unsigned i = 0; i != 8; ++i) {
      value = (value << 8U) | static_cast<uint64_t>(container[offset + i]);
    }
    offset += 8;
    return true;
  }
  bool string(std::string& value, size_t max_length, bool allow_empty)
  {
    uint16_t length = 0;
    if (!u16(length) || length > max_length || (!allow_empty && length == 0) || !has(length)) {
      return false;
    }
    value.clear();
    value.reserve(length);
    for (unsigned i = 0; i != length; ++i) {
      value.push_back(static_cast<char>(container[offset++]));
    }
    return true;
  }
  bool finished() const { return offset == container.length(); }

private:
  bool has(size_t count) const { return offset <= container.length() && count <= container.length() - offset; }

  const byte_buffer& container;
  size_t             offset = 0;
};

inline bool valid_generation(uint32_t value)
{
  return value != 0 && value != std::numeric_limits<uint32_t>::max();
}

inline bool valid_version(uint64_t value)
{
  return value != 0 && value != std::numeric_limits<uint64_t>::max();
}

inline bool valid_position_id(const std::string& value)
{
  return value.size() == 7 && value.front() == 'G' &&
         std::all_of(value.begin() + 1, value.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

template <typename T>
bool valid_target(const T& target)
{
  return valid_generation(target.query_generation) && target.nonce != 0 && target.connection_token != 0 &&
         target.gnb_du_id != gnb_du_id_t::invalid &&
         gnb_du_id_to_int(target.gnb_du_id) <= gnb_du_id_to_int(gnb_du_id_t::max) &&
         static_cast<unsigned>(target.cell_index) < MAX_NOF_DU_CELLS && is_valid(target.pci) &&
         target.gnb_du_ue_f1ap_id != gnb_du_ue_f1ap_id_t::invalid &&
         gnb_du_ue_f1ap_id_to_uint(target.gnb_du_ue_f1ap_id) <=
             gnb_du_ue_f1ap_id_to_uint(gnb_du_ue_f1ap_id_t::max) &&
         is_crnti(target.c_rnti) && valid_generation(target.expected_rnti_generation);
}

inline bool valid_authority(f1ap_ntn_initial_ul_position_authority authority)
{
  switch (authority) {
    case f1ap_ntn_initial_ul_position_authority::none:
    case f1ap_ntn_initial_ul_position_authority::software_attributed:
    case f1ap_ntn_initial_ul_position_authority::sdr_rx_port_verified:
    case f1ap_ntn_initial_ul_position_authority::ofh_beam_id_verified:
      return true;
    case f1ap_ntn_initial_ul_position_authority::invalid:
      return false;
  }
  return false;
}

inline bool valid_result(const f1ap_ntn_initial_ul_position_result& result)
{
  if (!valid_target(result) || !valid_authority(result.authority) || result.reason.empty() ||
      result.reason.size() > max_reason_length || result.calendar_hash.size() > max_hash_length ||
      result.mapping_hash.size() > max_hash_length || result.position_id.size() > max_position_length ||
      result.logical_port > max_logical_port_id ||
      (result.physical_port > max_physical_port_id &&
       result.physical_port != unavailable_physical_port_id) ||
      (result.eaxc.has_value() && *result.eaxc > max_eaxc_id) ||
      (result.beam_id.has_value() && *result.beam_id > max_beam_id) ||
      !std::isfinite(result.confidence_margin_db) ||
      result.confidence_margin_db < static_cast<float>(min_confidence_centi_db) / 100.0F ||
      result.confidence_margin_db > static_cast<float>(max_confidence_centi_db) / 100.0F) {
    return false;
  }

  if (!result.accepted) {
    return result.observation_id == 0 && result.authority == f1ap_ntn_initial_ul_position_authority::none &&
           result.schedule_version == 0 &&
           result.calendar_hash.empty() && result.mapping_version == 0 && result.mapping_hash.empty() &&
           result.position_id.empty() && result.logical_port == 0 && result.physical_port == 0 &&
           !result.eaxc.has_value() && !result.beam_id.has_value() && result.calendar_cycle_index == 0 &&
           result.occasion_offset_us == 0 && result.confidence_margin_db == 0.0F;
  }

  if (result.observation_id == 0 || result.authority == f1ap_ntn_initial_ul_position_authority::none ||
      !valid_version(result.schedule_version) || result.calendar_hash.empty() || !valid_position_id(result.position_id)) {
    return false;
  }

  if (result.authority == f1ap_ntn_initial_ul_position_authority::ofh_beam_id_verified) {
    return result.physical_port <= max_physical_port_id && valid_version(result.mapping_version) &&
           !result.mapping_hash.empty() && result.eaxc.has_value() && result.beam_id.has_value();
  }
  if (result.authority == f1ap_ntn_initial_ul_position_authority::sdr_rx_port_verified) {
    return result.physical_port <= max_physical_port_id && valid_version(result.mapping_version) &&
           !result.mapping_hash.empty() && !result.eaxc.has_value() && !result.beam_id.has_value();
  }
  // A software-attributed observation is derived from the active calendar only. Requiring the explicit no-port
  // sentinel and an absent receive-mapping identity prevents an audit-only result from masquerading as device proof.
  return result.authority == f1ap_ntn_initial_ul_position_authority::software_attributed &&
         result.physical_port == unavailable_physical_port_id && result.mapping_version == 0 &&
         result.mapping_hash.empty() && !result.eaxc.has_value() && !result.beam_id.has_value();
}

template <typename T>
void write_target(writer& out, const T& target)
{
  out.u32(target.query_generation);
  out.u64(target.nonce);
  out.u64(target.connection_token);
  out.u64(gnb_du_id_to_int(target.gnb_du_id));
  const std::array<uint8_t, 3> plmn_bytes = target.cell_cgi.plmn_id.to_bytes();
  out.bytes(plmn_bytes.data(), plmn_bytes.data() + plmn_bytes.size());
  out.u64(target.cell_cgi.nci.value());
  out.u16(static_cast<uint16_t>(target.cell_index));
  out.u16(target.pci);
  out.u64(gnb_du_ue_f1ap_id_to_uint(target.gnb_du_ue_f1ap_id));
  out.u16(to_value(target.c_rnti));
  out.u32(target.expected_rnti_generation);
}

template <typename T>
bool read_target(reader& in, T& target)
{
  uint64_t du_id_value       = 0;
  uint64_t nci_value         = 0;
  uint16_t cell_index_value  = 0;
  uint16_t pci_value         = 0;
  uint64_t du_ue_id_value    = 0;
  uint16_t c_rnti_value      = 0;
  std::array<uint8_t, 3> plmn_bytes{};

  if (!in.u32(target.query_generation) || !in.u64(target.nonce) || !in.u64(target.connection_token) ||
      !in.u64(du_id_value)) {
    return false;
  }
  for (uint8_t& value : plmn_bytes) {
    if (!in.u8(value)) {
      return false;
    }
  }
  if (!in.u64(nci_value) || !in.u16(cell_index_value) || !in.u16(pci_value) || !in.u64(du_ue_id_value) ||
      !in.u16(c_rnti_value) || !in.u32(target.expected_rnti_generation)) {
    return false;
  }

  const auto plmn = plmn_identity::from_bytes(plmn_bytes);
  const auto nci  = nr_cell_identity::create(nci_value);
  if (!plmn.has_value() || !nci.has_value() || du_id_value > gnb_du_id_to_int(gnb_du_id_t::max) ||
      cell_index_value >= MAX_NOF_DU_CELLS || !is_valid(static_cast<pci_t>(pci_value)) ||
      du_ue_id_value > gnb_du_ue_f1ap_id_to_uint(gnb_du_ue_f1ap_id_t::max)) {
    return false;
  }

  target.gnb_du_id         = int_to_gnb_du_id(du_id_value);
  target.cell_cgi          = nr_cell_global_id_t{plmn.value(), nci.value()};
  target.cell_index        = to_du_cell_index(cell_index_value);
  target.pci               = static_cast<pci_t>(pci_value);
  target.gnb_du_ue_f1ap_id = int_to_gnb_du_ue_f1ap_id(du_ue_id_value);
  target.c_rnti            = to_rnti(c_rnti_value);
  return valid_target(target);
}

} // namespace f1ap_ntn_initial_ul_position_detail

inline bool is_f1ap_ntn_initial_ul_position_query_container(const byte_buffer& container)
{
  using namespace f1ap_ntn_initial_ul_position_detail;
  if (container.length() < query_magic.size()) {
    return false;
  }
  for (size_t i = 0; i != query_magic.size(); ++i) {
    if (container[i] != query_magic[i]) {
      return false;
    }
  }
  return true;
}

inline byte_buffer encode_f1ap_ntn_initial_ul_position_query(const f1ap_ntn_initial_ul_position_query& query)
{
  using namespace f1ap_ntn_initial_ul_position_detail;
  if (!valid_target(query)) {
    return {};
  }

  writer out;
  out.bytes(query_magic.data(), query_magic.data() + query_magic.size());
  write_target(out, query);
  return out.size() <= max_container_size ? out.finish() : byte_buffer{};
}

inline std::optional<f1ap_ntn_initial_ul_position_query>
decode_f1ap_ntn_initial_ul_position_query(const byte_buffer& container)
{
  using namespace f1ap_ntn_initial_ul_position_detail;
  if (container.length() > max_container_size) {
    return std::nullopt;
  }

  reader                             in(container);
  f1ap_ntn_initial_ul_position_query query;
  if (!in.bytes(query_magic.data(), query_magic.size()) || !read_target(in, query) || !in.finished()) {
    return std::nullopt;
  }
  return query;
}

inline byte_buffer encode_f1ap_ntn_initial_ul_position_result(const f1ap_ntn_initial_ul_position_result& result)
{
  using namespace f1ap_ntn_initial_ul_position_detail;
  if (!valid_result(result)) {
    return {};
  }

  writer out;
  out.bytes(result_magic.data(), result_magic.data() + result_magic.size());
  write_target(out, result);
  out.u64(result.observation_id);
  out.u8(result.accepted ? 1U : 0U);
  out.u8(static_cast<uint8_t>(result.authority));
  out.string(result.reason);
  out.u64(result.schedule_version);
  out.string(result.calendar_hash);
  out.u64(result.mapping_version);
  out.string(result.mapping_hash);
  out.string(result.position_id);
  out.u16(result.logical_port);
  out.u16(result.physical_port);
  uint8_t optional_flags = result.eaxc.has_value() ? 0x01U : 0U;
  optional_flags |= result.beam_id.has_value() ? 0x02U : 0U;
  out.u8(optional_flags);
  if (result.eaxc.has_value()) {
    out.u16(*result.eaxc);
  }
  if (result.beam_id.has_value()) {
    out.u16(*result.beam_id);
  }
  out.u64(result.calendar_cycle_index);
  out.u32(result.occasion_offset_us);
  const int32_t confidence_centi_db = static_cast<int32_t>(std::lround(result.confidence_margin_db * 100.0F));
  out.u16(static_cast<uint16_t>(confidence_centi_db - min_confidence_centi_db));
  return out.size() <= max_container_size ? out.finish() : byte_buffer{};
}

inline std::optional<f1ap_ntn_initial_ul_position_result>
decode_f1ap_ntn_initial_ul_position_result(const byte_buffer& container)
{
  using namespace f1ap_ntn_initial_ul_position_detail;
  if (container.length() > max_container_size) {
    return std::nullopt;
  }

  reader                              in(container);
  f1ap_ntn_initial_ul_position_result result;
  uint8_t                             accepted       = 0;
  uint8_t                             authority      = 0;
  uint8_t                             optional_flags = 0;
  uint16_t                            confidence     = 0;
  if (!in.bytes(result_magic.data(), result_magic.size()) || !read_target(in, result) ||
      !in.u64(result.observation_id) || !in.u8(accepted) ||
      accepted > 1 || !in.u8(authority) || authority > 3 ||
      !in.string(result.reason, max_reason_length, false) || !in.u64(result.schedule_version) ||
      !in.string(result.calendar_hash, max_hash_length, true) || !in.u64(result.mapping_version) ||
      !in.string(result.mapping_hash, max_hash_length, true) ||
      !in.string(result.position_id, max_position_length, true) || !in.u16(result.logical_port) ||
      !in.u16(result.physical_port) || !in.u8(optional_flags) || (optional_flags & 0xfcU) != 0) {
    return std::nullopt;
  }

  result.accepted  = accepted == 1;
  result.authority = static_cast<f1ap_ntn_initial_ul_position_authority>(authority);
  if ((optional_flags & 0x01U) != 0) {
    uint16_t value = 0;
    if (!in.u16(value)) {
      return std::nullopt;
    }
    result.eaxc = value;
  }
  if ((optional_flags & 0x02U) != 0) {
    uint16_t value = 0;
    if (!in.u16(value)) {
      return std::nullopt;
    }
    result.beam_id = value;
  }
  if (!in.u64(result.calendar_cycle_index) || !in.u32(result.occasion_offset_us) || !in.u16(confidence) ||
      confidence > static_cast<uint16_t>(max_confidence_centi_db - min_confidence_centi_db) || !in.finished()) {
    return std::nullopt;
  }
  result.confidence_margin_db = static_cast<float>(static_cast<int32_t>(confidence) + min_confidence_centi_db) / 100.0F;
  return valid_result(result) ? std::optional<f1ap_ntn_initial_ul_position_result>{std::move(result)} : std::nullopt;
}

} // namespace srsran
