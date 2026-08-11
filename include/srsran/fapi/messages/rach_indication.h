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

#include "srsran/adt/static_vector.h"
#include "srsran/fapi/messages/base_message.h"
#include "srsran/ran/prach/verified_prach_rx_context.h"
#include "srsran/ran/rnti.h"
#include <limits>
#include <memory>

namespace srsran {
namespace fapi {

/// Internal receive-port attribution status for a detected PRACH preamble.
enum class prach_rx_port_attribution_status : uint8_t { unavailable, unique, ambiguous };

/// RACH indication pdu preamble.
struct rach_indication_pdu_preamble {
  uint8_t  preamble_index;
  uint16_t timing_advance_offset;
  uint32_t timing_advance_offset_ns;
  uint32_t preamble_pwr;
  uint8_t  preamble_snr;
  /// Optional receive-port attribution produced by the upper PHY.
  prach_rx_port_attribution_status port_attribution_status = prach_rx_port_attribution_status::unavailable;
  /// Physical receive-port identifier. Set to the maximum uint8_t value when unavailable.
  uint8_t strongest_rx_port = std::numeric_limits<uint8_t>::max();
  /// Power difference between the strongest and second strongest receive ports, in dB.
  float strongest_to_second_margin_dB = 0.0F;
  /// OFH position metadata selected by an unambiguous detector port. Empty for the standard and legacy paths.
  std::shared_ptr<const verified_prach_rx_context> verified_rx_context;
};

/// RACH indication pdu.
struct rach_indication_pdu {
  /// Maximum number of supported preambles per slot.
  static constexpr unsigned MAX_NUM_PREAMBLES = 64;

  uint32_t handle = 0;
  uint8_t  symbol_index;
  uint8_t  slot_index;
  uint8_t  ra_index;
  // :TODO: double check this variable. Table says uint16_t, range specifies uint32_t.
  uint32_t                                                       avg_rssi;
  uint16_t                                                       rsrp;
  uint8_t                                                        avg_snr;
  /// Internal NTN calendar position echoed from the corresponding PRACH request.
  bool     calendar_position_valid = false;
  uint64_t calendar_schedule_version = 0;
  uint64_t calendar_cycle_index    = 0;
  uint32_t occasion_offset_us      = 0;
  static_vector<rach_indication_pdu_preamble, MAX_NUM_PREAMBLES> preambles;
};

/// RACH indication message
struct rach_indication_message : public base_message {
  /// Maximum number of supported measurement PDUs in this message.
  static constexpr unsigned MAX_NUM_RACH_PDUS = 64;

  uint16_t                                              sfn;
  uint16_t                                              slot;
  uint8_t                                               num_pdu;
  static_vector<rach_indication_pdu, MAX_NUM_RACH_PDUS> pdus;
};

} // namespace fapi
} // namespace srsran
