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

#include "ntn_sib19_broadcast_controller.h"
#include "ntn_sib19_assistance_builder.h"
#include "srsran/asn1/asn1_utils.h"

using namespace srsran;
using namespace srsran::srs_cu_cp;

static const char* assistance_invalid_reason_to_string(ntn_assistance_invalid_reason reason)
{
  switch (reason) {
    case ntn_assistance_invalid_reason::none:
      return "none";
    case ntn_assistance_invalid_reason::disabled:
      return "disabled";
    case ntn_assistance_invalid_reason::no_satellite_state:
      return "no_satellite_state";
    case ntn_assistance_invalid_reason::stale_satellite_state:
      return "stale_satellite_state";
  }
  return "unknown";
}

static void count_entry(ntn_sib19_broadcast_snapshot& snapshot, ntn_sib19_broadcast_state state)
{
  switch (state) {
    case ntn_sib19_broadcast_state::desired:
      ++snapshot.nof_desired;
      break;
    case ntn_sib19_broadcast_state::sent_to_du:
      ++snapshot.nof_sent_to_du;
      break;
    case ntn_sib19_broadcast_state::applied_by_du:
      ++snapshot.nof_applied_by_du;
      break;
    case ntn_sib19_broadcast_state::rejected_by_du:
      ++snapshot.nof_rejected_by_du;
      break;
    case ntn_sib19_broadcast_state::clear_desired:
      ++snapshot.nof_clear_desired;
      break;
    case ntn_sib19_broadcast_state::clear_sent:
      ++snapshot.nof_clear_sent;
      break;
    case ntn_sib19_broadcast_state::cleared_by_du:
      ++snapshot.nof_cleared_by_du;
      break;
    case ntn_sib19_broadcast_state::stale_blocked:
      ++snapshot.nof_stale_blocked;
      break;
  }
}

static byte_buffer pack_sib19(const asn1::rrc_nr::sib19_r17_s& sib19)
{
  byte_buffer   pdu;
  asn1::bit_ref bref(pdu);
  if (sib19.pack(bref) != asn1::SRSASN_SUCCESS) {
    return {};
  }
  return pdu;
}

ntn_sib19_broadcast_snapshot
srsran::srs_cu_cp::build_ntn_sib19_broadcast_snapshot(const ntn_sib19_broadcast_request& request)
{
  ntn_sib19_broadcast_snapshot snapshot;

  if (!request.assistance.valid) {
    snapshot.entries.reserve(request.known_beams.size());
    for (const ntn_sib19_broadcast_known_beam& beam : request.known_beams) {
      ntn_sib19_broadcast_entry entry;
      entry.beam_id = beam.beam_id;
      entry.nci     = beam.nci;
      entry.state   = ntn_sib19_broadcast_state::stale_blocked;
      entry.reason  = assistance_invalid_reason_to_string(request.assistance.invalid_reason);
      count_entry(snapshot, entry.state);
      snapshot.entries.push_back(std::move(entry));
    }
    return snapshot;
  }

  snapshot.entries.reserve(request.assistance.entries.size());
  for (const ntn_sib19_assistance_entry& assistance_entry : request.assistance.entries) {
    ntn_sib19_broadcast_entry entry;
    entry.beam_id = assistance_entry.beam_id;
    entry.nci     = assistance_entry.nci;

    if (!assistance_entry.valid) {
      entry.state  = ntn_sib19_broadcast_state::stale_blocked;
      entry.reason = assistance_invalid_reason_to_string(assistance_entry.invalid_reason);
    } else if (assistance_entry.state == ntn_assistance_beam_state::draining) {
      entry.state  = ntn_sib19_broadcast_state::clear_desired;
      entry.reason = "draining";
    } else {
      const expected<asn1::rrc_nr::sib19_r17_s, std::string> sib19 =
          make_asn1_rrc_sib19_from_ntn_assistance(assistance_entry);
      if (!sib19.has_value()) {
        entry.state  = ntn_sib19_broadcast_state::rejected_by_du;
        entry.reason = sib19.error();
      } else {
        entry.packed_sib19 = pack_sib19(sib19.value());
        if (entry.packed_sib19.empty()) {
          entry.state  = ntn_sib19_broadcast_state::rejected_by_du;
          entry.reason = "packing_failed";
        } else {
          entry.state  = ntn_sib19_broadcast_state::desired;
          entry.reason = "broadcastable";
        }
      }
    }
    count_entry(snapshot, entry.state);
    snapshot.entries.push_back(std::move(entry));
  }

  return snapshot;
}

const char* srsran::srs_cu_cp::ntn_sib19_broadcast_state_to_string(ntn_sib19_broadcast_state state)
{
  switch (state) {
    case ntn_sib19_broadcast_state::desired:
      return "desired";
    case ntn_sib19_broadcast_state::sent_to_du:
      return "sent_to_du";
    case ntn_sib19_broadcast_state::applied_by_du:
      return "applied_by_du";
    case ntn_sib19_broadcast_state::rejected_by_du:
      return "rejected_by_du";
    case ntn_sib19_broadcast_state::clear_desired:
      return "clear_desired";
    case ntn_sib19_broadcast_state::clear_sent:
      return "clear_sent";
    case ntn_sib19_broadcast_state::cleared_by_du:
      return "cleared_by_du";
    case ntn_sib19_broadcast_state::stale_blocked:
      return "stale_blocked";
  }
  return "unknown";
}
