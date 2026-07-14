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

#include "lib/cu_cp/ntn_mobility/ntn_sib19_broadcast_controller.h"
#include <gtest/gtest.h>

using namespace srsran;
using namespace srs_cu_cp;

static nr_cell_identity make_nci(unsigned idx)
{
  return nr_cell_identity::create(gnb_id_t{411, 22}, idx).value();
}

static ntn_sib19_assistance_entry make_sib19_entry(std::string beam_id, ntn_assistance_beam_state state, unsigned idx)
{
  ntn_sib19_assistance_entry entry;
  entry.valid                 = true;
  entry.beam_id               = std::move(beam_id);
  entry.nci                   = make_nci(idx);
  entry.state                 = state;
  entry.reference_location    = {0.0, 0.0, 0.0};
  entry.satellite_ecef        = ecef_coordinates_t{1.0, 2.0, 3.0, 0.1, 0.2, 0.3};
  entry.ta_info               = ta_info_t{0.004072 * 100.0, 0.0002 * 50.0, 0.00002 * 7.0, 0.004072 * 10.0};
  entry.cell_specific_koffset = 150;
  entry.k_mac                 = 64;
  entry.ul_sync_validity_s    = 120;
  return entry;
}

TEST(ntn_sib19_broadcast_controller, valid_active_and_candidate_entries_generate_updates_while_draining_clears)
{
  ntn_sib19_assistance_snapshot snapshot;
  snapshot.valid          = true;
  snapshot.invalid_reason = ntn_assistance_invalid_reason::none;
  snapshot.entries.push_back(make_sib19_entry("CN-BEAM-0001", ntn_assistance_beam_state::active_loaded, 1));
  snapshot.entries.push_back(make_sib19_entry("CN-BEAM-0002", ntn_assistance_beam_state::candidate, 2));
  snapshot.entries.push_back(make_sib19_entry("CN-BEAM-0003", ntn_assistance_beam_state::draining, 3));

  ntn_sib19_broadcast_request request;
  request.assistance = snapshot;
  request.known_beams = {{"CN-BEAM-0001", make_nci(1)},
                         {"CN-BEAM-0002", make_nci(2)},
                         {"CN-BEAM-0003", make_nci(3)}};

  const ntn_sib19_broadcast_snapshot result = build_ntn_sib19_broadcast_snapshot(request);

  ASSERT_EQ(result.entries.size(), 3);
  EXPECT_EQ(result.entries[0].state, ntn_sib19_broadcast_state::desired);
  EXPECT_FALSE(result.entries[0].packed_sib19.empty());
  EXPECT_EQ(result.entries[1].state, ntn_sib19_broadcast_state::desired);
  EXPECT_FALSE(result.entries[1].packed_sib19.empty());
  EXPECT_EQ(result.entries[2].state, ntn_sib19_broadcast_state::clear_desired);
  EXPECT_EQ(result.entries[2].reason, "draining");
  EXPECT_EQ(result.nof_desired, 2U);
  EXPECT_EQ(result.nof_clear_desired, 1U);
}

TEST(ntn_sib19_broadcast_controller, stale_or_invalid_assistance_clears_all_known_beams)
{
  ntn_sib19_assistance_snapshot snapshot;
  snapshot.valid          = false;
  snapshot.invalid_reason = ntn_assistance_invalid_reason::stale_satellite_state;

  ntn_sib19_broadcast_request request;
  request.assistance  = snapshot;
  request.known_beams = {{"CN-BEAM-0001", make_nci(1)}, {"CN-BEAM-0002", make_nci(2)}};

  const ntn_sib19_broadcast_snapshot result = build_ntn_sib19_broadcast_snapshot(request);

  ASSERT_EQ(result.entries.size(), 2);
  EXPECT_EQ(result.entries[0].state, ntn_sib19_broadcast_state::stale_blocked);
  EXPECT_EQ(result.entries[0].reason, "stale_satellite_state");
  EXPECT_EQ(result.entries[1].state, ntn_sib19_broadcast_state::stale_blocked);
  EXPECT_EQ(result.nof_stale_blocked, 2U);
  EXPECT_EQ(result.nof_desired, 0U);
}
