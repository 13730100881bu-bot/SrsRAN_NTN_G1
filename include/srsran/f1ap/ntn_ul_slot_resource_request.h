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
#include <array>
#include <cstdint>
#include <optional>

namespace srsran {

/// Optional NTN-driven uplink control/sounding slot request carried between F1AP-CU and F1AP-DU.
struct f1ap_ntn_ul_slot_resource_request {
  std::optional<unsigned> sr_slot_offset;
  std::optional<unsigned> srs_slot_offset;
  std::optional<unsigned> sr_slot_period;
  std::optional<unsigned> srs_slot_period;
};

inline bool is_empty(const f1ap_ntn_ul_slot_resource_request& request)
{
  return !request.sr_slot_offset.has_value() && !request.srs_slot_offset.has_value();
}

inline byte_buffer encode_f1ap_ntn_ul_slot_resource_request(const f1ap_ntn_ul_slot_resource_request& request)
{
  std::array<uint8_t, 25> payload = {'S', 'R', 'S', 'N', 'T', 'N', '0', '2', 0x00, 0x00, 0x00, 0x00, 0x00,
                                     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                     0x00};

  const auto write_uint32 = [&payload](unsigned offset, unsigned value_) {
    const uint32_t value = static_cast<uint32_t>(value_);
    payload[offset]      = static_cast<uint8_t>((value >> 24U) & 0xffU);
    payload[offset + 1]  = static_cast<uint8_t>((value >> 16U) & 0xffU);
    payload[offset + 2]  = static_cast<uint8_t>((value >> 8U) & 0xffU);
    payload[offset + 3]  = static_cast<uint8_t>(value & 0xffU);
  };

  if (request.sr_slot_offset.has_value()) {
    payload[8] |= 0x01;
    write_uint32(9, *request.sr_slot_offset);
  }
  if (request.srs_slot_offset.has_value()) {
    payload[8] |= 0x02;
    write_uint32(13, *request.srs_slot_offset);
  }
  if (request.sr_slot_offset.has_value() && request.sr_slot_period.has_value() && *request.sr_slot_period != 0) {
    payload[8] |= 0x04;
    write_uint32(17, *request.sr_slot_period);
  }
  if (request.srs_slot_offset.has_value() && request.srs_slot_period.has_value() && *request.srs_slot_period != 0) {
    payload[8] |= 0x08;
    write_uint32(21, *request.srs_slot_period);
  }

  return byte_buffer::create(span<const uint8_t>(payload)).value();
}

inline std::optional<f1ap_ntn_ul_slot_resource_request>
decode_f1ap_ntn_ul_slot_resource_request(const byte_buffer& container)
{
  static constexpr std::array<uint8_t, 8> magic_v1 = {'S', 'R', 'S', 'N', 'T', 'N', '0', '1'};
  static constexpr std::array<uint8_t, 8> magic_v2 = {'S', 'R', 'S', 'N', 'T', 'N', '0', '2'};
  if (container.length() != 17 && container.length() != 25) {
    return std::nullopt;
  }

  const bool is_v1 = container.length() == 17;
  const auto& magic = is_v1 ? magic_v1 : magic_v2;
  for (unsigned i = 0; i != magic.size(); ++i) {
    if (container[i] != magic[i]) {
      return std::nullopt;
    }
  }

  const uint8_t flags = container[8];
  if ((flags & (is_v1 ? 0xfcU : 0xf0U)) != 0 || (is_v1 && flags == 0)) {
    return std::nullopt;
  }
  if (is_v1 && (flags & 0x0cU) != 0) {
    return std::nullopt;
  }
  if (!is_v1 && (((flags & 0x04U) != 0 && (flags & 0x01U) == 0) ||
                 ((flags & 0x08U) != 0 && (flags & 0x02U) == 0))) {
    return std::nullopt;
  }

  const auto read_uint32 = [&container](unsigned offset) {
    return (static_cast<uint32_t>(container[offset]) << 24U) | (static_cast<uint32_t>(container[offset + 1]) << 16U) |
           (static_cast<uint32_t>(container[offset + 2]) << 8U) | static_cast<uint32_t>(container[offset + 3]);
  };

  f1ap_ntn_ul_slot_resource_request request;
  if ((flags & 0x01U) != 0) {
    request.sr_slot_offset = read_uint32(9);
  }
  if ((flags & 0x02U) != 0) {
    request.srs_slot_offset = read_uint32(13);
  }
  if (!is_v1 && (flags & 0x04U) != 0) {
    const unsigned period = read_uint32(17);
    if (period == 0) {
      return std::nullopt;
    }
    request.sr_slot_period = period;
  }
  if (!is_v1 && (flags & 0x08U) != 0) {
    const unsigned period = read_uint32(21);
    if (period == 0) {
      return std::nullopt;
    }
    request.srs_slot_period = period;
  }
  return request;
}

} // namespace srsran
