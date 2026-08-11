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
 * the LICENSE file in the top-level directory of this distribution.
 *
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>

namespace srsran {

/// Describes the receive backend that associated a PRACH buffer port with a planned position.
enum class prach_rx_context_authority : uint8_t {
  unavailable,
  sdr_rx_port_verified,
  ofh_beam_id_verified
};

/// Maximum length of opaque position, calendar and receive-mapping identifiers carried with verified PRACH context.
inline constexpr size_t MAX_PRACH_RX_CONTEXT_IDENTIFIER_LENGTH = 256;
/// Maximum number of receive ports represented by one verified PRACH context list.
inline constexpr unsigned MAX_VERIFIED_PRACH_RX_PORTS = 16;
/// Exclusive upper bound for the Open Fronthaul eAxC identifier supported by this implementation.
inline constexpr unsigned MAX_VERIFIED_PRACH_EAXC_ID_VALUE = 32;

/// Immutable receive metadata associated with one zero-based port in a PRACH buffer.
struct verified_prach_rx_context {
  /// Backend that verified this association.
  prach_rx_context_authority authority = prach_rx_context_authority::unavailable;
  /// Zero-based port index in the PRACH buffer and detector input.
  /// This is not a physical receive-port identifier. The corresponding physical port is held separately in
  /// prach_buffer_context::ports[buffer_port] and is exported independently by the PRACH indication.
  unsigned buffer_port = std::numeric_limits<unsigned>::max();
  /// Cell-local logical uplink port selected by the active access calendar.
  uint16_t logical_port_id = std::numeric_limits<uint16_t>::max();
  /// Open Fronthaul PRACH eAxC. Present only for an OFH source.
  std::optional<uint16_t> ofh_prach_eaxc;
  /// Open Fronthaul BeamId. Present only for an OFH source.
  std::optional<uint16_t> ofh_beam_id;
  /// Opaque earth-fixed position identifier.
  std::string position_id;
  /// Access-calendar schedule version.
  uint64_t schedule_version = 0;
  /// Opaque access-calendar hash.
  std::string calendar_hash;
  /// Generation of the logical-port-to-receive-backend mapping.
  /// Consumers compare this value with their local mapping version. It is independent of calendar_hash and does not
  /// replace the local mapping content hash.
  uint64_t mapping_generation = 0;
  /// Opaque hash of the logical-port-to-receive-backend mapping.
  std::string mapping_hash;
};

/// Returns true when a verified PRACH receive context is complete and bounded for its declared backend.
inline bool is_valid_verified_prach_rx_context(const verified_prach_rx_context& context)
{
  if (context.authority == prach_rx_context_authority::unavailable ||
      context.buffer_port >= MAX_VERIFIED_PRACH_RX_PORTS ||
      context.logical_port_id == std::numeric_limits<uint16_t>::max() || context.position_id.empty() ||
      context.position_id.size() > MAX_PRACH_RX_CONTEXT_IDENTIFIER_LENGTH || context.schedule_version == 0 ||
      context.calendar_hash.empty() || context.calendar_hash.size() > MAX_PRACH_RX_CONTEXT_IDENTIFIER_LENGTH ||
      context.mapping_generation == 0 || context.mapping_hash.empty() ||
      context.mapping_hash.size() > MAX_PRACH_RX_CONTEXT_IDENTIFIER_LENGTH) {
    return false;
  }

  if (context.authority == prach_rx_context_authority::ofh_beam_id_verified) {
    return context.ofh_prach_eaxc.has_value() && context.ofh_beam_id.has_value() &&
           context.ofh_prach_eaxc.value() < MAX_VERIFIED_PRACH_EAXC_ID_VALUE &&
           context.ofh_beam_id.value() <= 0x7fffU;
  }
  if (context.authority == prach_rx_context_authority::sdr_rx_port_verified) {
    return !context.ofh_prach_eaxc.has_value() && !context.ofh_beam_id.has_value();
  }
  return false;
}

} // namespace srsran
