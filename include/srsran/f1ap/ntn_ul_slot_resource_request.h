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

enum class f1ap_ntn_ul_slot_resource_operation : uint8_t { legacy = 0, set = 1, clear = 2 };

/// Optional NTN-driven uplink control/sounding slot request carried between F1AP-CU and F1AP-DU.
struct f1ap_ntn_ul_slot_resource_request {
  std::optional<unsigned> sr_slot_offset;
  std::optional<unsigned> srs_slot_offset;
  std::optional<unsigned> sr_slot_period;
  std::optional<unsigned> srs_slot_period;
  std::optional<rnti_t>   requested_c_rnti;
  /// Non-zero identity of a versioned set/clear operation. Zero is reserved for legacy requests.
  uint32_t                                assignment_generation = 0;
  f1ap_ntn_ul_slot_resource_operation     operation = f1ap_ntn_ul_slot_resource_operation::legacy;
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
  timeout,
  assignment_generation_conflict,
  stale_assignment_generation,
  slot_assignment_generation_exhausted
};

struct f1ap_ntn_ul_slot_resource_result {
  bool accepted = false;
  f1ap_ntn_ul_slot_resource_result_reason reason = f1ap_ntn_ul_slot_resource_result_reason::rejected;
  std::optional<f1ap_ntn_ul_slot_resource_request> applied_request;
  /// Echoes the operation identity even when the request is rejected and no applied request is returned.
  uint32_t                            assignment_generation = 0;
  f1ap_ntn_ul_slot_resource_operation operation = f1ap_ntn_ul_slot_resource_operation::legacy;
};

inline bool is_empty(const f1ap_ntn_ul_slot_resource_request& request)
{
  return !request.sr_slot_offset.has_value() && !request.srs_slot_offset.has_value();
}

inline bool is_versioned(const f1ap_ntn_ul_slot_resource_request& request)
{
  return request.assignment_generation != 0 || request.operation != f1ap_ntn_ul_slot_resource_operation::legacy;
}

inline bool are_f1ap_ntn_ul_slot_resource_requests_equal(const f1ap_ntn_ul_slot_resource_request& lhs,
                                                         const f1ap_ntn_ul_slot_resource_request& rhs)
{
  return lhs.sr_slot_offset == rhs.sr_slot_offset && lhs.srs_slot_offset == rhs.srs_slot_offset &&
         lhs.sr_slot_period == rhs.sr_slot_period && lhs.srs_slot_period == rhs.srs_slot_period &&
         lhs.requested_c_rnti == rhs.requested_c_rnti &&
         lhs.assignment_generation == rhs.assignment_generation && lhs.operation == rhs.operation;
}

/// Returns true when a request can be carried in the authoritative DU-applied UE-slot snapshot.
inline bool is_valid_f1ap_ntn_authoritative_ul_slot_set(const f1ap_ntn_ul_slot_resource_request& request)
{
  return request.operation == f1ap_ntn_ul_slot_resource_operation::set && request.assignment_generation != 0 &&
         !is_empty(request) && !request.requested_c_rnti.has_value() &&
         (!request.sr_slot_period.has_value() ||
          (request.sr_slot_offset.has_value() && *request.sr_slot_period != 0)) &&
         (!request.srs_slot_period.has_value() ||
          (request.srs_slot_offset.has_value() && *request.srs_slot_period != 0));
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
    case f1ap_ntn_ul_slot_resource_result_reason::assignment_generation_conflict:
      return "assignment_generation_conflict";
    case f1ap_ntn_ul_slot_resource_result_reason::stale_assignment_generation:
      return "stale_assignment_generation";
    case f1ap_ntn_ul_slot_resource_result_reason::slot_assignment_generation_exhausted:
      return "slot_assignment_generation_exhausted";
  }
  return "rejected";
}

inline byte_buffer encode_f1ap_ntn_ul_slot_resource_request(const f1ap_ntn_ul_slot_resource_request& request)
{
  if (is_versioned(request)) {
    std::array<uint8_t, 34> payload = {'S', 'R', 'S', 'N', 'T', 'N', '0', '5', 0x00, 0x00, 0x00, 0x00, 0x00,
                                       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    const auto write_uint32 = [&payload](unsigned offset, uint32_t value) {
      payload[offset]     = static_cast<uint8_t>((value >> 24U) & 0xffU);
      payload[offset + 1] = static_cast<uint8_t>((value >> 16U) & 0xffU);
      payload[offset + 2] = static_cast<uint8_t>((value >> 8U) & 0xffU);
      payload[offset + 3] = static_cast<uint8_t>(value & 0xffU);
    };

    if (request.sr_slot_offset.has_value()) {
      payload[8] |= 0x01U;
      write_uint32(14, *request.sr_slot_offset);
    }
    if (request.srs_slot_offset.has_value()) {
      payload[8] |= 0x02U;
      write_uint32(18, *request.srs_slot_offset);
    }
    if (request.sr_slot_period.has_value()) {
      payload[8] |= 0x04U;
      write_uint32(22, *request.sr_slot_period);
    }
    if (request.srs_slot_period.has_value()) {
      payload[8] |= 0x08U;
      write_uint32(26, *request.srs_slot_period);
    }
    if (request.requested_c_rnti.has_value()) {
      payload[8] |= 0x10U;
      write_uint32(30, to_value(*request.requested_c_rnti));
    }
    payload[9] = static_cast<uint8_t>(request.operation);
    write_uint32(10, request.assignment_generation);
    return byte_buffer::create(span<const uint8_t>(payload)).value();
  }

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
  static constexpr std::array<uint8_t, 8> magic_v5 = {'S', 'R', 'S', 'N', 'T', 'N', '0', '5'};
  if (container.length() != 17 && container.length() != 25 && container.length() != 29 && container.length() != 34) {
    return std::nullopt;
  }

  const bool is_v1 = container.length() == 17;
  const bool is_v4 = container.length() == 29;
  const bool is_v5 = container.length() == 34;
  const auto& magic = is_v1 ? magic_v1 : (is_v4 ? magic_v4 : (is_v5 ? magic_v5 : magic_v2));
  for (unsigned i = 0; i != magic.size(); ++i) {
    if (container[i] != magic[i]) {
      return std::nullopt;
    }
  }

  const uint8_t flags = container[8];
  if ((flags & (is_v1 ? 0xfcU : ((is_v4 || is_v5) ? 0xe0U : 0xf0U))) != 0 || (is_v1 && flags == 0)) {
    return std::nullopt;
  }
  if (!is_v4 && !is_v5 && (flags & 0x10U) != 0) {
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
  if (is_v5) {
    const uint8_t operation = container[9];
    request.assignment_generation = read_uint32(10);
    if (operation < static_cast<uint8_t>(f1ap_ntn_ul_slot_resource_operation::set) ||
        operation > static_cast<uint8_t>(f1ap_ntn_ul_slot_resource_operation::clear) ||
        request.assignment_generation == 0) {
      return std::nullopt;
    }
    request.operation = static_cast<f1ap_ntn_ul_slot_resource_operation>(operation);
  }
  const unsigned sr_offset_pos   = is_v5 ? 14U : 9U;
  const unsigned srs_offset_pos  = is_v5 ? 18U : 13U;
  const unsigned sr_period_pos   = is_v5 ? 22U : 17U;
  const unsigned srs_period_pos  = is_v5 ? 26U : 21U;
  const unsigned requested_rnti_pos = is_v5 ? 30U : 25U;
  if ((flags & 0x01U) != 0) {
    request.sr_slot_offset = read_uint32(sr_offset_pos);
  }
  if ((flags & 0x02U) != 0) {
    request.srs_slot_offset = read_uint32(srs_offset_pos);
  }
  if (!is_v1 && (flags & 0x04U) != 0) {
    const unsigned period = read_uint32(sr_period_pos);
    if (period == 0) {
      return std::nullopt;
    }
    request.sr_slot_period = period;
  }
  if (!is_v1 && (flags & 0x08U) != 0) {
    const unsigned period = read_uint32(srs_period_pos);
    if (period == 0) {
      return std::nullopt;
    }
    request.srs_slot_period = period;
  }
  if ((is_v4 || is_v5) && (flags & 0x10U) != 0) {
    const uint32_t raw_requested_c_rnti = read_uint32(requested_rnti_pos);
    if (raw_requested_c_rnti > 0xffffU) {
      return std::nullopt;
    }
    const rnti_t requested_c_rnti = to_rnti(static_cast<uint16_t>(raw_requested_c_rnti));
    if (!is_crnti(requested_c_rnti)) {
      return std::nullopt;
    }
    request.requested_c_rnti = requested_c_rnti;
  }
  if (is_v5) {
    const bool set_has_resources = !is_empty(request);
    if ((request.operation == f1ap_ntn_ul_slot_resource_operation::set && !set_has_resources) ||
        (request.operation == f1ap_ntn_ul_slot_resource_operation::clear &&
         (set_has_resources || request.requested_c_rnti.has_value()))) {
      return std::nullopt;
    }
  }
  return request;
}

inline byte_buffer encode_f1ap_ntn_ul_slot_resource_result(const f1ap_ntn_ul_slot_resource_result& result)
{
  if (result.assignment_generation != 0 || result.operation != f1ap_ntn_ul_slot_resource_operation::legacy ||
      (result.applied_request.has_value() && is_versioned(*result.applied_request))) {
    std::array<uint8_t, 35> payload = {'S', 'R', 'S', 'N', 'T', 'N', '0', '6', 0x00, 0x00, 0x00, 0x00, 0x00,
                                       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    const auto write_uint32 = [&payload](unsigned offset, uint32_t value) {
      payload[offset]     = static_cast<uint8_t>((value >> 24U) & 0xffU);
      payload[offset + 1] = static_cast<uint8_t>((value >> 16U) & 0xffU);
      payload[offset + 2] = static_cast<uint8_t>((value >> 8U) & 0xffU);
      payload[offset + 3] = static_cast<uint8_t>(value & 0xffU);
    };
    payload[8] = result.accepted ? 0x01U : 0U;
    payload[9] = static_cast<uint8_t>(result.reason);
    const auto operation =
        result.operation != f1ap_ntn_ul_slot_resource_operation::legacy
            ? result.operation
            : (result.applied_request.has_value() ? result.applied_request->operation
                                                  : f1ap_ntn_ul_slot_resource_operation::legacy);
    const uint32_t generation =
        result.assignment_generation != 0
            ? result.assignment_generation
            : (result.applied_request.has_value() ? result.applied_request->assignment_generation : 0U);
    // A successful clear has no active resource assignment to report. Older callers may still echo the clear
    // request, so normalize it to an absent applied payload on the wire.
    const f1ap_ntn_ul_slot_resource_request* applied =
        operation == f1ap_ntn_ul_slot_resource_operation::clear
            ? nullptr
            : (result.applied_request.has_value() ? &*result.applied_request : nullptr);
    if (applied != nullptr) {
      payload[8] |= 0x02U;
      if (applied->sr_slot_offset.has_value()) {
        payload[8] |= 0x04U;
        write_uint32(15, *applied->sr_slot_offset);
      }
      if (applied->srs_slot_offset.has_value()) {
        payload[8] |= 0x08U;
        write_uint32(19, *applied->srs_slot_offset);
      }
      if (applied->sr_slot_period.has_value()) {
        payload[8] |= 0x10U;
        write_uint32(23, *applied->sr_slot_period);
      }
      if (applied->srs_slot_period.has_value()) {
        payload[8] |= 0x20U;
        write_uint32(27, *applied->srs_slot_period);
      }
      if (applied->requested_c_rnti.has_value()) {
        payload[8] |= 0x40U;
        write_uint32(31, to_value(*applied->requested_c_rnti));
      }
    }
    payload[10] = static_cast<uint8_t>(operation);
    write_uint32(11, generation);
    return byte_buffer::create(span<const uint8_t>(payload)).value();
  }

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
  static constexpr std::array<uint8_t, 8> magic_v6 = {'S', 'R', 'S', 'N', 'T', 'N', '0', '6'};
  if (container.length() != 26 && container.length() != 35) {
    return std::nullopt;
  }
  const bool is_v6 = container.length() == 35;
  const auto& expected_magic = is_v6 ? magic_v6 : magic;
  for (unsigned i = 0; i != expected_magic.size(); ++i) {
    if (container[i] != expected_magic[i]) {
      return std::nullopt;
    }
  }

  const uint8_t flags = container[8];
  if ((!is_v6 && ((flags & 0xe0U) != 0 || ((flags & 0x08U) != 0 && (flags & 0x02U) == 0) ||
                  ((flags & 0x10U) != 0 && (flags & 0x04U) == 0))) ||
      (is_v6 && ((flags & 0x80U) != 0 || ((flags & 0x7cU) != 0 && (flags & 0x02U) == 0) ||
                 ((flags & 0x10U) != 0 && (flags & 0x04U) == 0) ||
                 ((flags & 0x20U) != 0 && (flags & 0x08U) == 0)))) {
    return std::nullopt;
  }
  const auto max_reason =
      is_v6 ? f1ap_ntn_ul_slot_resource_result_reason::slot_assignment_generation_exhausted
            : f1ap_ntn_ul_slot_resource_result_reason::timeout;
  if (container[9] > static_cast<uint8_t>(max_reason)) {
    return std::nullopt;
  }

  const auto read_uint32 = [&container](unsigned offset) {
    return (static_cast<uint32_t>(container[offset]) << 24U) | (static_cast<uint32_t>(container[offset + 1]) << 16U) |
           (static_cast<uint32_t>(container[offset + 2]) << 8U) | static_cast<uint32_t>(container[offset + 3]);
  };

  f1ap_ntn_ul_slot_resource_result result;
  result.accepted = (flags & 0x01U) != 0;
  result.reason   = static_cast<f1ap_ntn_ul_slot_resource_result_reason>(container[9]);

  if (is_v6) {
    const uint8_t operation = container[10];
    result.assignment_generation = read_uint32(11);
    if (operation < static_cast<uint8_t>(f1ap_ntn_ul_slot_resource_operation::set) ||
        operation > static_cast<uint8_t>(f1ap_ntn_ul_slot_resource_operation::clear) ||
        result.assignment_generation == 0) {
      return std::nullopt;
    }
    result.operation = static_cast<f1ap_ntn_ul_slot_resource_operation>(operation);
  }

  f1ap_ntn_ul_slot_resource_request request;
  const uint8_t sr_flag      = is_v6 ? 0x04U : 0x02U;
  const uint8_t srs_flag     = is_v6 ? 0x08U : 0x04U;
  const uint8_t sr_period_flag  = is_v6 ? 0x10U : 0x08U;
  const uint8_t srs_period_flag = is_v6 ? 0x20U : 0x10U;
  if ((flags & sr_flag) != 0) {
    request.sr_slot_offset = read_uint32(is_v6 ? 15U : 10U);
  }
  if ((flags & srs_flag) != 0) {
    request.srs_slot_offset = read_uint32(is_v6 ? 19U : 14U);
  }
  if ((flags & sr_period_flag) != 0) {
    const unsigned period = read_uint32(is_v6 ? 23U : 18U);
    if (period == 0) {
      return std::nullopt;
    }
    request.sr_slot_period = period;
  }
  if ((flags & srs_period_flag) != 0) {
    const unsigned period = read_uint32(is_v6 ? 27U : 22U);
    if (period == 0) {
      return std::nullopt;
    }
    request.srs_slot_period = period;
  }
  if (is_v6 && (flags & 0x40U) != 0) {
    const uint32_t raw_requested_c_rnti = read_uint32(31);
    if (raw_requested_c_rnti > 0xffffU) {
      return std::nullopt;
    }
    const rnti_t requested_c_rnti = to_rnti(static_cast<uint16_t>(raw_requested_c_rnti));
    if (!is_crnti(requested_c_rnti)) {
      return std::nullopt;
    }
    request.requested_c_rnti = requested_c_rnti;
  }
  if (is_v6 && (flags & 0x02U) != 0) {
    request.assignment_generation = result.assignment_generation;
    request.operation              = result.operation;
    if ((request.operation == f1ap_ntn_ul_slot_resource_operation::set && is_empty(request)) ||
        (request.operation == f1ap_ntn_ul_slot_resource_operation::clear &&
         (!is_empty(request) || request.requested_c_rnti.has_value()))) {
      return std::nullopt;
    }
    result.applied_request = request;
  } else if (!is_v6 && !is_empty(request)) {
    result.applied_request = request;
  }
  if (is_v6 && result.accepted) {
    const bool applied_request_present = (flags & 0x02U) != 0;
    if ((result.operation == f1ap_ntn_ul_slot_resource_operation::set && !applied_request_present) ||
        (result.operation == f1ap_ntn_ul_slot_resource_operation::clear && applied_request_present)) {
      return std::nullopt;
    }
  }
  return result;
}

} // namespace srsran
