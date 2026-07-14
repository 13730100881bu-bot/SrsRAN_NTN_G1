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

#include "lib/cu_cp/ntn_mobility/ntn_assistance_snapshot_generator.h"
#include "srsran/ran/gnb_id.h"
#include <algorithm>
#include <gtest/gtest.h>

using namespace srsran;
using namespace srs_cu_cp;

namespace {

nr_cell_identity make_nci(unsigned sector_id)
{
  return nr_cell_identity::create(gnb_id_t{0x19b, 32}, sector_id).value();
}

ntn_beam_position make_beam(const std::string& beam_id, unsigned sector_id, double longitude_deg)
{
  return {beam_id, make_nci(sector_id), 0.0, longitude_deg, 50000.0, true};
}

ecef_coordinates_t make_satellite()
{
  ecef_coordinates_t satellite{};
  satellite.position_x  = 6978137.0;
  satellite.position_y  = 0.0;
  satellite.position_z  = 0.0;
  satellite.velocity_vx = 0.0;
  satellite.velocity_vy = 7000.0;
  satellite.velocity_vz = 0.0;
  return satellite;
}

const ntn_assistance_beam_snapshot*
find_assistance_beam(const ntn_assistance_snapshot& snapshot, const std::string& beam_id)
{
  auto it = std::find_if(snapshot.beams.begin(),
                         snapshot.beams.end(),
                         [&beam_id](const ntn_assistance_beam_snapshot& beam) {
                           return beam.beam_id == beam_id;
                         });
  return it != snapshot.beams.end() ? &*it : nullptr;
}

} // namespace

TEST(ntn_assistance_snapshot_generator, builds_valid_bounded_snapshot_for_service_and_candidate_beams)
{
  const auto now = std::chrono::steady_clock::now();

  ntn_assistance_snapshot_request request;
  request.satellite_ecef          = make_satellite();
  request.satellite_epoch         = std::chrono::system_clock::time_point{std::chrono::seconds{1234}};
  request.satellite_received_time = now;
  request.now                     = now;
  request.satellite_state_max_age = std::chrono::milliseconds{1000};
  request.cell_specific_koffset   = 150;
  request.k_mac                   = 12;
  request.ul_sync_validity_s      = 30;
  request.t_service               = 987654321U;
  request.max_snapshot_beams      = 3;
  request.beams = {make_beam("CN-BEAM-0001", 1, 0.0),
                   make_beam("CN-BEAM-0002", 2, 1.0),
                   make_beam("CN-BEAM-0003", 3, 2.0),
                   make_beam("CN-BEAM-0004", 4, 3.0)};
  request.placement_plan.assignments = {{"CN-BEAM-0001",
                                         make_nci(1),
                                         uint_to_du_index(0),
                                         ntn_beam_assignment_state::active_loaded,
                                         80.0,
                                         1,
                                         1,
                                         true},
                                        {"CN-BEAM-0002",
                                         make_nci(2),
                                         uint_to_du_index(0),
                                         ntn_beam_assignment_state::candidate,
                                         70.0,
                                         0,
                                         0,
                                         true},
                                        {"CN-BEAM-0003",
                                         make_nci(3),
                                         uint_to_du_index(0),
                                         ntn_beam_assignment_state::draining,
                                         60.0,
                                         1,
                                         0,
                                         false},
                                        {"CN-BEAM-0004",
                                         make_nci(4),
                                         du_index_t::invalid,
                                         ntn_beam_assignment_state::inactive,
                                         0.0,
                                         0,
                                         0,
                                         false}};

  const ntn_assistance_snapshot snapshot = build_ntn_assistance_snapshot(request);

  ASSERT_TRUE(snapshot.valid);
  EXPECT_EQ(snapshot.invalid_reason, ntn_assistance_invalid_reason::none);
  ASSERT_TRUE(snapshot.satellite_ecef.has_value());
  EXPECT_EQ(snapshot.satellite_epoch, request.satellite_epoch);
  ASSERT_EQ(snapshot.beams.size(), 3U);

  const ntn_assistance_beam_snapshot* active_beam = find_assistance_beam(snapshot, "CN-BEAM-0001");
  ASSERT_NE(active_beam, nullptr);
  EXPECT_EQ(active_beam->state, ntn_assistance_beam_state::active_loaded);
  EXPECT_EQ(active_beam->cell_specific_koffset, 150U);
  EXPECT_EQ(active_beam->k_mac, std::optional<unsigned>{12});
  EXPECT_EQ(active_beam->ul_sync_validity_s, std::optional<unsigned>{30});
  EXPECT_EQ(active_beam->t_service, std::optional<uint64_t>{987654321U});
  EXPECT_NEAR(active_beam->reference_location.latitude, 0.0, 1e-9);
  ASSERT_TRUE(active_beam->ta_info.has_value());
  EXPECT_GT(active_beam->ta_info->ta_common, 0.0);

  const ntn_assistance_beam_snapshot* candidate_beam = find_assistance_beam(snapshot, "CN-BEAM-0002");
  ASSERT_NE(candidate_beam, nullptr);
  EXPECT_EQ(candidate_beam->state, ntn_assistance_beam_state::candidate);
  ASSERT_TRUE(candidate_beam->ta_info.has_value());

  const ntn_assistance_beam_snapshot* draining_beam = find_assistance_beam(snapshot, "CN-BEAM-0003");
  ASSERT_NE(draining_beam, nullptr);
  EXPECT_EQ(draining_beam->state, ntn_assistance_beam_state::draining);
  ASSERT_TRUE(draining_beam->ta_info.has_value());

  EXPECT_EQ(find_assistance_beam(snapshot, "CN-BEAM-0004"), nullptr);
}

TEST(ntn_assistance_snapshot_generator, marks_snapshot_invalid_without_satellite_state)
{
  ntn_assistance_snapshot_request request;
  request.now = std::chrono::steady_clock::now();

  const ntn_assistance_snapshot snapshot = build_ntn_assistance_snapshot(request);

  ASSERT_FALSE(snapshot.valid);
  EXPECT_EQ(snapshot.invalid_reason, ntn_assistance_invalid_reason::no_satellite_state);
  EXPECT_TRUE(snapshot.beams.empty());
}

TEST(ntn_assistance_snapshot_generator, marks_snapshot_invalid_when_satellite_state_is_stale)
{
  const auto now = std::chrono::steady_clock::now();

  ntn_assistance_snapshot_request request;
  request.satellite_ecef          = make_satellite();
  request.satellite_received_time = now - std::chrono::milliseconds{2001};
  request.now                     = now;
  request.satellite_state_max_age = std::chrono::milliseconds{2000};
  request.beams                   = {make_beam("CN-BEAM-0001", 1, 0.0)};
  request.placement_plan.assignments = {{"CN-BEAM-0001",
                                         make_nci(1),
                                         uint_to_du_index(0),
                                         ntn_beam_assignment_state::active_loaded,
                                         80.0,
                                         1,
                                         1,
                                         true}};

  const ntn_assistance_snapshot snapshot = build_ntn_assistance_snapshot(request);

  ASSERT_FALSE(snapshot.valid);
  EXPECT_EQ(snapshot.invalid_reason, ntn_assistance_invalid_reason::stale_satellite_state);
  EXPECT_TRUE(snapshot.beams.empty());
}

TEST(ntn_assistance_snapshot_generator, omits_out_of_range_ta_info_without_invalidating_snapshot)
{
  const auto now = std::chrono::steady_clock::now();

  ecef_coordinates_t fast_leo = make_satellite();
  fast_leo.velocity_vy        = 7600.0;

  ntn_assistance_snapshot_request request;
  request.satellite_ecef          = fast_leo;
  request.satellite_received_time = now;
  request.now                     = now;
  request.satellite_state_max_age = std::chrono::milliseconds{1000};
  request.beams                   = {make_beam("CN-BEAM-0001", 1, 0.0)};
  request.placement_plan.assignments = {{"CN-BEAM-0001",
                                         make_nci(1),
                                         uint_to_du_index(0),
                                         ntn_beam_assignment_state::candidate,
                                         80.0,
                                         0,
                                         0,
                                         true}};

  const ntn_assistance_snapshot snapshot = build_ntn_assistance_snapshot(request);

  ASSERT_TRUE(snapshot.valid);
  ASSERT_EQ(snapshot.beams.size(), 1U);
  EXPECT_FALSE(snapshot.beams.front().ta_info.has_value());
}

TEST(ntn_assistance_snapshot_generator, computes_beam_assistance_from_serving_satellite_owner)
{
  const auto now = std::chrono::steady_clock::now();

  ecef_coordinates_t sat_a = make_satellite();
  ecef_coordinates_t sat_b = make_satellite();
  sat_b.position_x += 250000.0;

  ntn_assistance_snapshot_request request;
  request.satellite_ecef          = sat_a;
  request.satellite_states        = {{"sat-A", sat_a}, {"sat-B", sat_b}};
  request.satellite_received_time = now;
  request.now                     = now;
  request.satellite_state_max_age = std::chrono::milliseconds{1000};
  request.beams                   = {make_beam("CN-BEAM-0001", 1, 0.0)};
  request.placement_plan.assignments = {{"CN-BEAM-0001",
                                         make_nci(1),
                                         uint_to_du_index(0),
                                         ntn_beam_assignment_state::candidate,
                                         80.0,
                                         0,
                                         0,
                                         true}};
  request.placement_plan.assignments.front().serving_satellite_id = "sat-B";

  const ntn_assistance_snapshot snapshot = build_ntn_assistance_snapshot(request);

  ASSERT_TRUE(snapshot.valid);
  ASSERT_EQ(snapshot.beams.size(), 1U);
  EXPECT_EQ(snapshot.beams.front().serving_satellite_id, "sat-B");
  ASSERT_TRUE(snapshot.beams.front().ta_info.has_value());

  ntn_assistance_snapshot_request sat_b_reference = request;
  sat_b_reference.satellite_ecef = sat_b;
  sat_b_reference.satellite_states.clear();
  const ntn_assistance_snapshot expected_sat_b_snapshot = build_ntn_assistance_snapshot(sat_b_reference);
  ASSERT_TRUE(expected_sat_b_snapshot.beams.front().ta_info.has_value());
  EXPECT_NEAR(snapshot.beams.front().ta_info->ta_common,
              expected_sat_b_snapshot.beams.front().ta_info->ta_common,
              1e-9);
}
