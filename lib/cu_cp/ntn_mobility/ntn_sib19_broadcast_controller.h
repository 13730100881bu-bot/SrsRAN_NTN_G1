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
#include "srsran/cu_cp/ntn_location.h"
#include <string>
#include <vector>

namespace srsran {
namespace srs_cu_cp {

/// CU-CP SIB19 broadcast application state. This is DU application state intent/result,
/// not proof of PHY-layer broadcast.
enum class ntn_sib19_broadcast_state {
  desired,
  sent_to_du,
  applied_by_du,
  rejected_by_du,
  clear_desired,
  clear_sent,
  cleared_by_du,
  stale_blocked
};

struct ntn_sib19_broadcast_known_beam {
  std::string      beam_id;
  nr_cell_identity nci = nr_cell_identity::min();
};

struct ntn_sib19_broadcast_entry {
  std::string               beam_id;
  nr_cell_identity          nci = nr_cell_identity::min();
  ntn_sib19_broadcast_state state = ntn_sib19_broadcast_state::stale_blocked;
  std::string               reason;
  byte_buffer               packed_sib19;
};

struct ntn_sib19_broadcast_request {
  ntn_sib19_assistance_snapshot                 assistance;
  std::vector<ntn_sib19_broadcast_known_beam>   known_beams;
};

struct ntn_sib19_broadcast_snapshot {
  std::vector<ntn_sib19_broadcast_entry> entries;
  unsigned                               nof_desired       = 0;
  unsigned                               nof_sent_to_du    = 0;
  unsigned                               nof_applied_by_du = 0;
  unsigned                               nof_rejected_by_du = 0;
  unsigned                               nof_clear_desired = 0;
  unsigned                               nof_clear_sent    = 0;
  unsigned                               nof_cleared_by_du = 0;
  unsigned                               nof_stale_blocked = 0;
};

ntn_sib19_broadcast_snapshot build_ntn_sib19_broadcast_snapshot(const ntn_sib19_broadcast_request& request);

const char* ntn_sib19_broadcast_state_to_string(ntn_sib19_broadcast_state state);

} // namespace srs_cu_cp
} // namespace srsran
