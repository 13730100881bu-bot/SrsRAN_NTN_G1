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

#include "srsran/ntn/beam_hopping_controller.h"
#include "srsran/ntn/beam_hopping_table.h"
#include "srsran/ntn/ta_calculator.h"
#include "srsran/srslog/srslog.h"
#include "gtest/gtest.h"
#include <cmath>
#include <limits>
#include <vector>

using namespace srsran;
using namespace srsran::srs_ntn;

namespace {

beam_position_grid make_test_grid()
{
  return make_beam_position_grid({1.0, 0.0, 0.0, 1.0, 20000.0, 0.9});
}

struct captured_sib19_update {
  nr_cell_global_id_t nr_cgi;
  sib19_info          sib19;
  slot_point          valid_from;
};

class capturing_sib19_notifier final : public sib19_update_notifier
{
public:
  void on_sib19_update(const nr_cell_global_id_t& nr_cgi,
                       const sib19_info&         sib19,
                       slot_point                valid_from) override
  {
    updates.push_back({nr_cgi, sib19, valid_from});
  }

  std::vector<captured_sib19_update> updates;
};

void expect_ref_location_matches_beam(const sib19_info& sib19, const beam_position_t& beam)
{
  ASSERT_TRUE(sib19.ref_location.has_value());
  EXPECT_NEAR(sib19.ref_location->latitude, beam.center_lat, 1e-9);
  EXPECT_NEAR(sib19.ref_location->longitude, beam.center_lon, 1e-9);
}

} // namespace

TEST(ntn_beam_position_grid, assigns_hex_neighbours_with_different_colours)
{
  const beam_position_grid grid = make_test_grid();

  ASSERT_GE(grid.positions.size(), 6U);
  for (const beam_position_t& pos : grid.positions) {
    for (uint16_t neighbour_id : pos.neighbors) {
      if (neighbour_id == INVALID_BEAM_POSITION_ID) {
        continue;
      }
      ASSERT_LT(neighbour_id, grid.positions.size());
      EXPECT_NE(pos.color, grid.get_position(neighbour_id).color);
    }
  }
}

TEST(ntn_beam_hopping_table, supports_three_phase_cycles_that_do_not_divide_sfn_wrap)
{
  const beam_position_grid    grid  = make_test_grid();
  const beam_hopping_table_t table = make_beam_hopping_table(grid, 6, 7);

  ASSERT_EQ(table.n_active, 6U);
  ASSERT_EQ(table.dwell_frames, 7U);
  ASSERT_EQ(table.cycle_frames, 42U);
  ASSERT_NE(NOF_SFNS % table.cycle_frames, 0U);

  for (uint16_t i = 0; i != table.n_active; ++i) {
    EXPECT_EQ(table.entries[i].color, i % 3U);
  }

  EXPECT_EQ(table.get_active_beam(0), table.entries[0].beam_position_id);
  EXPECT_EQ(table.get_active_beam(6), table.entries[0].beam_position_id);
  EXPECT_EQ(table.get_active_beam(7), table.entries[1].beam_position_id);
  EXPECT_EQ(table.frames_until_next_visit(0, table.entries[1].beam_position_id), 7U);
  EXPECT_EQ(table.frames_until_next_visit(0, INVALID_BEAM_POSITION_ID), std::numeric_limits<uint32_t>::max());
}

TEST(ntn_ta_calculator, computes_service_link_ta_for_nadir_beam)
{
  ecef_coordinates_t satellite = geodetic_to_ecef({0.0, 0.0, 600000.0});
  satellite.velocity_vx        = 0.0;
  satellite.velocity_vy        = 7000.0;
  satellite.velocity_vz        = 0.0;

  const ta_calc_result result = compute_beam_ta({0.0, 0.0, 0.0}, satellite);

  constexpr double range_m = 600000.0;
  EXPECT_NEAR(result.slant_range_m, range_m, 1e-6);
  EXPECT_NEAR(result.radial_vel_ms, 0.0, 1e-9);
  EXPECT_NEAR(result.ta.ta_common, 2.0 * range_m / ntn_constants::SPEED_OF_LIGHT_M_PER_US, 1e-9);
  EXPECT_NEAR(result.ta.ta_common_drift, 0.0, 1e-12);
  EXPECT_GT(result.ta.ta_common_drift_variant, 0.0);
  EXPECT_EQ(result.ta.ta_common_offset, 0.0);
}

TEST(ntn_beam_hopping_controller, schedules_next_dwell_when_tick_arrives_after_boundary)
{
  const beam_position_grid    grid  = make_test_grid();
  const beam_hopping_table_t table = make_beam_hopping_table(grid, 6, 7);

  capturing_sib19_notifier notifier;
  auto&                    logger = srslog::fetch_basic_logger("TEST", false);

  beam_hopping_controller::config cfg;
  cfg.grid                     = grid;
  cfg.hop_table                = table;
  cfg.cell_specific_koffset    = 150;
  cfg.ntn_ul_sync_validity_dur = 120;

  std::unique_ptr<beam_hopping_controller> controller =
      make_beam_hopping_controller(std::move(cfg), notifier, logger);

  ecef_coordinates_t satellite = geodetic_to_ecef({0.5, 0.5, 600000.0});
  satellite.velocity_vx        = 0.0;
  satellite.velocity_vy        = 0.0;
  satellite.velocity_vz        = 0.0;

  controller->update_satellite_ephemeris(satellite, slot_point(subcarrier_spacing::kHz15, 1));

  controller->on_slot_indication(slot_point(subcarrier_spacing::kHz15, 1));
  ASSERT_EQ(notifier.updates.size(), 1U);
  EXPECT_EQ(notifier.updates.back().valid_from.count(), 1U);
  expect_ref_location_matches_beam(notifier.updates.back().sib19, grid.get_position(table.entries[0].beam_position_id));

  controller->on_slot_indication(slot_point(subcarrier_spacing::kHz15, 3));
  ASSERT_EQ(notifier.updates.size(), 2U);
  EXPECT_EQ(notifier.updates.back().valid_from.count(), 70U);
  expect_ref_location_matches_beam(notifier.updates.back().sib19, grid.get_position(table.entries[1].beam_position_id));

  controller->on_slot_indication(slot_point(subcarrier_spacing::kHz15, 4));
  EXPECT_EQ(notifier.updates.size(), 2U);

  controller->on_slot_indication(slot_point(subcarrier_spacing::kHz15, 71));
  ASSERT_EQ(notifier.updates.size(), 3U);
  EXPECT_EQ(notifier.updates.back().valid_from.count(), 140U);
  expect_ref_location_matches_beam(notifier.updates.back().sib19, grid.get_position(table.entries[2].beam_position_id));
}
