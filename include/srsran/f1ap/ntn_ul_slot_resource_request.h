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
#include "srsran/ran/rnti.h"
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
  std::optional<rnti_t>   requested_c_rnti;
};

enum class f1ap_ntn_ul_slot_resource_result_reason : uint8_t {
  applied = 0,
  clear_applied,
  rejected,
  malformed_request,
  unsupported_period,
  sr_offset_unavailable,
  srs_offset_unavailable,
  du_resource_conflict,
  scheduler_mismatch,
  du_context_update_failed,
  timeout
};

struct f1ap_ntn_ul_slot_resource_result {
  bool accepted = false;
  f1ap_ntn_ul_slot_resource_result_reason reason = f1ap_ntn_ul_slot_resource_result_reason::rejected;
  std::optional<f1ap_ntn_ul_slot_resource_request> applied_request;
};

inline bool is_empty(const f1ap_ntn_ul_slot_resource_request& request)
{
  return !request.sr_slot_offset.has_value() && !request.srs_slot_offset.has_value();
}

inline const char* to_string(f1ap_ntn_ul_slot_resource_result_reason reason)
{
  switch (reason) {
    case f1ap_ntn_ul_slot_resource_result_reason::applied:
      return "applied";
    case f1ap_ntn_ul_slot_resource_result_reason::clear_applied:
      return "clear_applied";
    case f1ap_ntn_ul_slot_resource_result_reason::rejected:
      return "rejected";
    case f1ap_ntn_ul_slot_resource_result_reason::malformed_request:
      return "malformed_request";
    case f1ap_ntn_ul_slot_resource_result_reason::unsupported_period:
      return "unsupported_period";
    case f1ap_ntn_ul_slot_resource_result_reason::sr_offset_unavailable:
      return "sr_offset_unavailable";
    case f1ap_ntn_ul_slot_resource_result_reason::srs_offset_unavailable:
      return "srs_offset_unavailable";
    case f1ap_ntn_ul_slot_resource_result_reason::du_resource_conflict:
      return "du_resource_conflict";
    case f1ap_ntn_ul_slot_resource_result_reason::scheduler_mismatch:
      return "scheduler_mismatch";
    case f1ap_ntn_ul_slot_resource_result_reason::du_context_update_failed:
      return "du_context_update_failed";
    case f1ap_ntn_ul_slot_resource_result_reason::timeout:
      return "timeout";
  }
  return "rejected";
}

inline byte_buffer encode_f1ap_ntn_ul_slot_resource_request(const f1ap_ntn_ul_slot_resource_request& request)
{
  if (request.requested_c_rnti.has_value()) {
    std::array<uint8_t, 29> payload = {'S', 'R', 'S', 'N', 'T', 'N', '0', '4', 0x00, 0x00, 0x00, 0x00, 0x00,
                                       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                       0x00, 0x00, 0x00, 0x00, 0x00};

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
    payload[8] |= 0x10;
    write_uint32(25, to_value(*request.requested_c_rnti));

    return byte_buffer::create(span<const uint8_t>(payload)).value();
  }

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
  static constexpr std::array<uint8_t, 8> magic_v4 = {'S', 'R', 'S', 'N', 'T', 'N', '0', '4'};
  if (container.length() != 17 && container.length() != 25 && container.length() != 29) {
    return std::nullopt;
  }

  const bool is_v1 = container.length() == 17;
  const bool is_v4 = container.length() == 29;
  const auto& magic = is_v1 ? magic_v1 : (is_v4 ? magic_v4 : magic_v2);
  for (unsigned i = 0; i != magic.size(); ++i) {
    if (container[i] != magic[i]) {
      return std::nullopt;
    }
  }

  const uint8_t flags = container[8];
  if ((flags & (is_v1 ? 0xfcU : (is_v4 ? 0xe0U : 0xf0U))) != 0 || (is_v1 && flags == 0)) {
    return std::nullopt;
  }
  if (!is_v4 && (flags & 0x10U) != 0) {
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
  if (is_v4 && (flags & 0x10U) != 0) {
    const rnti_t requested_c_rnti = to_rnti(static_cast<uint16_t>(read_uint32(25)));
    if (!is_crnti(requested_c_rnti)) {
      return std::nullopt;
    }
    request.requested_c_rnti = requested_c_rnti;
  }
  return request;
}

inline byte_buffer encode_f1ap_ntn_ul_slot_resource_result(const f1ap_ntn_ul_slot_resource_result& result)
{
  std::array<uint8_t, 26> payload = {'S', 'R', 'S', 'N', 'T', 'N', '0', '3', 0x00, 0x00, 0x00, 0x00, 0x00,
                                     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                     0x00, 0x00};

  const auto write_uint32 = [&payload](unsigned offset, unsigned value_) {
    const uint32_t value = static_cast<uint32_t>(value_);
    payload[offset]      = static_cast<uint8_t>((value >> 24U) & 0xffU);
    payload[offset + 1]  = static_cast<uint8_t>((value >> 16U) & 0xffU);
    payload[offset + 2]  = static_cast<uint8_t>((value >> 8U) & 0xffU);
    payload[offset + 3]  = static_cast<uint8_t>(value & 0xffU);
  };

  if (result.accepted) {
    payload[8] |= 0x01;
  }
  payload[9] = static_cast<uint8_t>(result.reason);

  if (result.applied_request.has_value()) {
    const f1ap_ntn_ul_slot_resource_request& request = *result.applied_request;
    if (request.sr_slot_offset.has_value()) {
      payload[8] |= 0x02;
      write_uint32(10, *request.sr_slot_offset);
    }
    if (request.srs_slot_offset.has_value()) {
      payload[8] |= 0x04;
      write_uint32(14, *request.srs_slot_offset);
    }
    if (request.sr_slot_offset.has_value() && request.sr_slot_period.has_value() && *request.sr_slot_period != 0) {
      payload[8] |= 0x08;
      write_uint32(18, *request.sr_slot_period);
    }
    if (request.srs_slot_offset.has_value() && request.srs_slot_period.has_value() && *request.srs_slot_period != 0) {
      payload[8] |= 0x10;
      write_uint32(22, *request.srs_slot_period);
    }
  }

  return byte_buffer::create(span<const uint8_t>(payload)).value();
}

inline std::optional<f1ap_ntn_ul_slot_resource_result>
decode_f1ap_ntn_ul_slot_resource_result(const byte_buffer& container)
{
  static constexpr std::array<uint8_t, 8> magic = {'S', 'R', 'S', 'N', 'T', 'N', '0', '3'};
  if (container.length() != 26) {
    return std::nullopt;
  }
  for (unsigned i = 0; i != magic.size(); ++i) {
    if (container[i] != magic[i]) {
      return std::nullopt;
    }
  }

  const uint8_t flags = container[8];
  if ((flags & 0xe0U) != 0 || ((flags & 0x08U) != 0 && (flags & 0x02U) == 0) ||
      ((flags & 0x10U) != 0 && (flags & 0x04U) == 0)) {
    return std::nullopt;
  }
  if (container[9] > static_cast<uint8_t>(f1ap_ntn_ul_slot_resource_result_reason::timeout)) {
    return std::nullopt;
  }

  const auto read_uint32 = [&container](unsigned offset) {
    return (static_cast<uint32_t>(container[offset]) << 24U) | (static_cast<uint32_t>(container[offset + 1]) << 16U) |
           (static_cast<uint32_t>(container[offset + 2]) << 8U) | static_cast<uint32_t>(container[offset + 3]);
  };

  f1ap_ntn_ul_slot_resource_result result;
  result.accepted = (flags & 0x01U) != 0;
  result.reason   = static_cast<f1ap_ntn_ul_slot_resource_result_reason>(container[9]);

  f1ap_ntn_ul_slot_resource_request request;
  if ((flags & 0x02U) != 0) {
    request.sr_slot_offset = read_uint32(10);
  }
  if ((flags & 0x04U) != 0) {
    request.srs_slot_offset = read_uint32(14);
  }
  if ((flags & 0x08U) != 0) {
    const unsigned period = read_uint32(18);
    if (period == 0) {
      return std::nullopt;
    }
    request.sr_slot_period = period;
  }
  if ((flags & 0x10U) != 0) {
    const unsigned period = read_uint32(22);
    if (period == 0) {
      return std::nullopt;
    }
    request.srs_slot_period = period;
  }
  if (!is_empty(request)) {
    result.applied_request = request;
  }
  return result;
}

} // namespace srsran
