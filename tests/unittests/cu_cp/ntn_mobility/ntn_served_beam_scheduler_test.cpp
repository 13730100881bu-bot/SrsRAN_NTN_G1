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

#include "lib/cu_cp/ntn_mobility/ntn_served_beam_scheduler.h"
#include "srsran/ran/gnb_id.h"
#include <algorithm>
#include <cmath>
#include <gtest/gtest.h>
#include <string>

using namespace srsran;
using namespace srs_cu_cp;

namespace {

nr_cell_identity make_nci(unsigned sector_id)
{
  return nr_cell_identity::create(gnb_id_t{0x19b, 32}, sector_id).value();
}

nr_cell_identity make_dense_nci(unsigned sector_id)
{
  return nr_cell_identity::create(gnb_id_t{0x19b, 22}, sector_id).value();
}

ecef_coordinates_t make_ecef(double latitude_deg, double longitude_deg, double altitude_m)
{
  constexpr double wgs84_a_m = 6378137.0;
  constexpr double wgs84_f   = 1.0 / 298.257223563;
  constexpr double wgs84_e2  = 2.0 * wgs84_f - wgs84_f * wgs84_f;
  constexpr double pi        = 3.14159265358979323846;

  const double lat     = latitude_deg * pi / 180.0;
  const double lon     = longitude_deg * pi / 180.0;
  const double sin_lat = std::sin(lat);
  const double cos_lat = std::cos(lat);
  const double n       = wgs84_a_m / std::sqrt(1.0 - wgs84_e2 * sin_lat * sin_lat);

  ecef_coordinates_t ecef{};
  ecef.position_x = (n + altitude_m) * cos_lat * std::cos(lon);
  ecef.position_y = (n + altitude_m) * cos_lat * std::sin(lon);
  ecef.position_z = (n * (1.0 - wgs84_e2) + altitude_m) * sin_lat;
  return ecef;
}

ntn_served_beam_scheduler_config make_scheduler_config()
{
  ntn_served_beam_scheduler_config cfg;
  cfg.beams = {{"CN-BEAM-0001", make_nci(0), 0.0, 0.0, 50000.0, true},
               {"CN-BEAM-0002", make_nci(1), 0.0, 1.0, 50000.0, true}};
  cfg.min_elevation_deg    = 80.0;
  cfg.max_nof_served_beams = 1;
  return cfg;
}

ntn_served_beam_scheduler_config make_multi_beam_scheduler_config()
{
  ntn_served_beam_scheduler_config cfg;
  cfg.beams = {{"CN-BEAM-0001", make_nci(0), 0.0, -0.5, 50000.0, true},
               {"CN-BEAM-0002", make_nci(1), 0.0, 1.0, 50000.0, true},
               {"CN-BEAM-0003", make_nci(2), 0.0, 2.0, 50000.0, true}};
  cfg.min_elevation_deg    = -90.0;
  cfg.max_nof_served_beams = 3;
  return cfg;
}

ntn_served_beam_scheduler_config make_rotating_multi_beam_scheduler_config()
{
  ntn_served_beam_scheduler_config cfg;
  cfg.beams = {{"CN-BEAM-0001", make_nci(0), 0.0, 0.0, 50000.0, true},
               {"CN-BEAM-0002", make_nci(1), 0.0, 0.0, 50000.0, true},
               {"CN-BEAM-0003", make_nci(2), 0.0, 0.0, 50000.0, true},
               {"CN-BEAM-0004", make_nci(3), 0.0, 0.0, 50000.0, true}};
  cfg.min_elevation_deg          = -90.0;
  cfg.max_nof_served_beams       = 2;
  cfg.served_beam_hopping_enabled = true;
  cfg.served_beam_hopping_dwell_updates = 1;
  return cfg;
}

class recording_served_beam_update_handler : public ntn_served_beam_update_handler
{
public:
  bool update_ntn_served_beams(const std::vector<std::string>& beam_ids) override
  {
    updates.push_back(beam_ids);
    return accept_updates;
  }

  bool update_ntn_served_beam_candidates(const std::vector<ntn_served_beam_candidate>& candidates) override
  {
    candidate_updates.push_back(candidates);
    return ntn_served_beam_update_handler::update_ntn_served_beam_candidates(candidates);
  }

  bool                                  accept_updates = true;
  std::vector<std::vector<std::string>> updates;
  std::vector<std::vector<ntn_served_beam_candidate>> candidate_updates;
};

} // namespace

TEST(ntn_served_beam_scheduler, applies_changed_beam_sets_and_suppresses_duplicates)
{
  ntn_served_beam_scheduler        scheduler(make_scheduler_config());
  recording_served_beam_update_handler update_handler;

  ntn_served_beam_schedule first =
      scheduler.update_from_satellite_state(make_ecef(0.0, 0.0, 500000.0), update_handler);
  ASSERT_TRUE(first.changed);
  ASSERT_TRUE(first.applied);
  ASSERT_EQ(first.beam_ids, std::vector<std::string>({"CN-BEAM-0001"}));
  ASSERT_EQ(update_handler.updates.size(), 1);
  ASSERT_EQ(update_handler.candidate_updates.size(), 1);
  ASSERT_EQ(update_handler.candidate_updates.back().front().beam_id, "CN-BEAM-0001");
  ASSERT_GT(update_handler.candidate_updates.back().front().elevation_deg, 80.0);
  ASSERT_EQ(scheduler.current_served_beam_ids(), std::vector<std::string>({"CN-BEAM-0001"}));

  ntn_served_beam_schedule duplicate =
      scheduler.update_from_satellite_state(make_ecef(0.0, 0.0, 500000.0), update_handler);
  ASSERT_FALSE(duplicate.changed);
  ASSERT_FALSE(duplicate.applied);
  ASSERT_EQ(update_handler.updates.size(), 1);

  ntn_served_beam_schedule second =
      scheduler.update_from_satellite_state(make_ecef(0.0, 1.0, 500000.0), update_handler);
  ASSERT_TRUE(second.changed);
  ASSERT_TRUE(second.applied);
  ASSERT_EQ(second.beam_ids, std::vector<std::string>({"CN-BEAM-0002"}));
  ASSERT_EQ(update_handler.updates.size(), 2);
  ASSERT_EQ(scheduler.current_served_beam_ids(), std::vector<std::string>({"CN-BEAM-0002"}));
}

TEST(ntn_served_beam_scheduler, keeps_last_accepted_set_when_update_is_rejected)
{
  ntn_served_beam_scheduler        scheduler(make_scheduler_config());
  recording_served_beam_update_handler update_handler;
  update_handler.accept_updates = false;

  ntn_served_beam_schedule rejected =
      scheduler.update_from_satellite_state(make_ecef(0.0, 0.0, 500000.0), update_handler);
  ASSERT_TRUE(rejected.changed);
  ASSERT_FALSE(rejected.applied);
  ASSERT_TRUE(scheduler.current_served_beam_ids().empty());
  ASSERT_EQ(update_handler.updates.size(), 1);

  update_handler.accept_updates = true;
  ntn_served_beam_schedule accepted =
      scheduler.update_from_satellite_state(make_ecef(0.0, 0.0, 500000.0), update_handler);
  ASSERT_TRUE(accepted.changed);
  ASSERT_TRUE(accepted.applied);
  ASSERT_EQ(scheduler.current_served_beam_ids(), std::vector<std::string>({"CN-BEAM-0001"}));
  ASSERT_EQ(update_handler.updates.size(), 2);
}

TEST(ntn_served_beam_scheduler, applies_multi_beam_hopping_window_and_preserves_elevation_order)
{
  ntn_served_beam_scheduler        scheduler(make_multi_beam_scheduler_config());
  recording_served_beam_update_handler update_handler;

  ntn_served_beam_schedule first =
      scheduler.update_from_satellite_state(make_ecef(0.0, 1.0, 500000.0), update_handler);
  ASSERT_TRUE(first.changed);
  ASSERT_TRUE(first.applied);
  ASSERT_EQ(first.beam_ids, std::vector<std::string>({"CN-BEAM-0002", "CN-BEAM-0003", "CN-BEAM-0001"}));
  ASSERT_EQ(scheduler.current_served_beam_ids(),
            std::vector<std::string>({"CN-BEAM-0002", "CN-BEAM-0003", "CN-BEAM-0001"}));

  ntn_served_beam_schedule duplicate =
      scheduler.update_from_satellite_state(make_ecef(0.0, 1.0, 500000.0), update_handler);
  ASSERT_FALSE(duplicate.changed);
  ASSERT_FALSE(duplicate.applied);
  ASSERT_EQ(update_handler.updates.size(), 1);

  ntn_served_beam_schedule second =
      scheduler.update_from_satellite_state(make_ecef(0.0, 2.0, 500000.0), update_handler);
  ASSERT_TRUE(second.changed);
  ASSERT_TRUE(second.applied);
  ASSERT_EQ(second.beam_ids, std::vector<std::string>({"CN-BEAM-0003", "CN-BEAM-0002", "CN-BEAM-0001"}));
  ASSERT_EQ(update_handler.updates.size(), 2);
}

TEST(ntn_served_beam_scheduler, selects_active_window_from_dense_15km_beam_pool)
{
  ntn_served_beam_scheduler_config cfg;
  cfg.min_elevation_deg    = 10.0;
  cfg.max_nof_served_beams = 5;

  std::string center_beam_id;
  unsigned    sector_id = 0;
  for (int lat_idx = -20; lat_idx <= 20; ++lat_idx) {
    for (int lon_idx = -20; lon_idx <= 20; ++lon_idx) {
      const std::string beam_id = "DENSE-BEAM-" + std::to_string(sector_id);
      if (lat_idx == 0 && lon_idx == 0) {
        center_beam_id = beam_id;
      }
      cfg.beams.push_back({beam_id,
                           make_dense_nci(sector_id),
                           static_cast<double>(lat_idx) * 0.135,
                           static_cast<double>(lon_idx) * 0.135,
                           15000.0,
                           true});
      ++sector_id;
    }
  }

  ntn_served_beam_scheduler        scheduler(cfg);
  recording_served_beam_update_handler update_handler;

  ntn_served_beam_schedule first =
      scheduler.update_from_satellite_state(make_ecef(0.0, 0.0, 500000.0), update_handler);
  ASSERT_TRUE(first.changed);
  ASSERT_TRUE(first.applied);
  ASSERT_EQ(first.candidates.size(), cfg.beams.size());
  ASSERT_EQ(first.beam_ids.size(), 5);
  ASSERT_EQ(first.beam_ids.front(), center_beam_id);
  ASSERT_GT(first.candidates.front().elevation_deg, 89.0);
}

TEST(ntn_served_beam_scheduler, keeps_full_candidate_inventory_when_hopping_window_is_capped)
{
  ntn_served_beam_scheduler_config cfg;
  cfg.min_elevation_deg    = -90.0;
  cfg.max_nof_served_beams = 2;

  for (unsigned sector_id = 0; sector_id != 1000; ++sector_id) {
    cfg.beams.push_back({"CN-BEAM-" + std::to_string(sector_id),
                         make_dense_nci(sector_id),
                         0.0,
                         static_cast<double>(sector_id) * 0.001,
                         15000.0,
                         true});
  }

  ntn_served_beam_scheduler scheduler(cfg);
  const auto                schedule = scheduler.compute_schedule(make_ecef(0.0, 0.0, 500000.0));

  ASSERT_EQ(schedule.candidates.size(), cfg.beams.size());
  ASSERT_EQ(schedule.beam_ids.size(), 2);
  ASSERT_EQ(std::count_if(schedule.candidates.begin(),
                          schedule.candidates.end(),
                          [](const ntn_served_beam_candidate& candidate) { return candidate.in_hopping_window; }),
                          2);
}

TEST(ntn_served_beam_scheduler, active_analog_access_window_marks_all_child_digital_beams_without_capping_inventory)
{
  ntn_served_beam_scheduler_config cfg;
  cfg.min_elevation_deg                    = -90.0;
  cfg.max_nof_served_beams                 = 0;
  cfg.max_nof_active_analog_access_beams   = 1;
  cfg.served_beam_hopping_enabled          = true;
  cfg.served_beam_hopping_dwell_updates    = 1;

  ntn_analog_beam_position analog1;
  analog1.analog_beam_id           = "LEO500-ANALOG-0001";
  analog1.center_hex_q             = 0;
  analog1.center_hex_r             = 0;
  analog1.center_digital_beam_id   = "LEO500-DIGI-0001";
  analog1.child_digital_beam_ids   = {"LEO500-DIGI-0001", "LEO500-DIGI-0002", "LEO500-DIGI-0003"};
  analog1.is_edge_partial          = true;

  ntn_analog_beam_position analog2;
  analog2.analog_beam_id           = "LEO500-ANALOG-0002";
  analog2.center_hex_q             = 3;
  analog2.center_hex_r             = -1;
  analog2.center_digital_beam_id   = "LEO500-DIGI-0004";
  analog2.child_digital_beam_ids   = {"LEO500-DIGI-0004", "LEO500-DIGI-0005", "LEO500-DIGI-0006"};
  analog2.is_edge_partial          = true;
  cfg.analog_beams                 = {analog1, analog2};

  for (unsigned i = 0; i != 6; ++i) {
    ntn_beam_position beam;
    beam.beam_id              = "LEO500-DIGI-000" + std::to_string(i + 1);
    beam.nci                  = make_dense_nci(i);
    beam.center_latitude_deg  = 0.0;
    beam.center_longitude_deg = static_cast<double>(i) * 0.1;
    beam.coverage_radius_m    = 15000.0;
    beam.enabled              = true;
    beam.analog_beam_id       = i < 3 ? "LEO500-ANALOG-0001" : "LEO500-ANALOG-0002";
    beam.hex_q                = static_cast<int>(i);
    beam.hex_r                = 0;
    cfg.beams.push_back(beam);
  }

  ntn_served_beam_scheduler scheduler(cfg);
  const auto                schedule = scheduler.compute_schedule(make_ecef(0.0, 0.0, 500000.0));

  ASSERT_EQ(schedule.candidates.size(), 6);
  ASSERT_EQ(schedule.beam_ids.size(), 3);
  ASSERT_EQ(std::count_if(schedule.candidates.begin(),
                          schedule.candidates.end(),
                          [](const ntn_served_beam_candidate& candidate) {
                            return candidate.in_hopping_window;
                          }),
            3);
  ASSERT_TRUE(std::all_of(schedule.beam_ids.begin(), schedule.beam_ids.end(), [](const std::string& beam_id) {
    return beam_id == "LEO500-DIGI-0001" || beam_id == "LEO500-DIGI-0002" || beam_id == "LEO500-DIGI-0003";
  }));
}

TEST(ntn_served_beam_selector, selects_full_candidate_inventory_without_loaded_beam_limit)
{
  std::vector<ntn_beam_position> beams;
  for (unsigned sector_id = 0; sector_id != 16; ++sector_id) {
    beams.push_back({"CN-BEAM-" + std::to_string(sector_id),
                     make_dense_nci(sector_id),
                     0.0,
                     static_cast<double>(sector_id) * 0.01,
                     15000.0,
                     true});
  }

  const std::vector<ntn_served_beam_candidate> candidates =
      select_ntn_candidate_inventory_by_elevation(beams, make_ecef(0.0, 0.0, 500000.0), -90.0);

  ASSERT_EQ(candidates.size(), beams.size());
}

TEST(ntn_served_beam_scheduler, multi_satellite_scheduler_merges_visible_beams_by_best_elevation)
{
  ntn_served_beam_scheduler_config cfg;
  cfg.min_elevation_deg    = -90.0;
  cfg.max_nof_served_beams = 0;
  cfg.beams = {{"CN-BEAM-0001", make_nci(0), 0.0, 0.0, 50000.0, true},
               {"CN-BEAM-0002", make_nci(1), 0.0, 4.0, 50000.0, true}};

  ntn_served_beam_scheduler scheduler(cfg);
  const std::vector<ntn_satellite_state> satellites = {{"sat-low", make_ecef(0.0, 2.0, 500000.0)},
                                                       {"sat-high", make_ecef(0.0, 0.0, 500000.0)}};

  const ntn_served_beam_schedule schedule = scheduler.compute_schedule(satellites);

  ASSERT_EQ(schedule.candidates.size(), 2U);
  const auto beam1 = std::find_if(schedule.candidates.begin(),
                                  schedule.candidates.end(),
                                  [](const ntn_served_beam_candidate& candidate) {
                                    return candidate.beam_id == "CN-BEAM-0001";
                                  });
  ASSERT_NE(beam1, schedule.candidates.end());
  EXPECT_EQ(beam1->serving_satellite_id, "sat-high");
  EXPECT_TRUE(std::all_of(schedule.candidates.begin(),
                          schedule.candidates.end(),
                          [](const ntn_served_beam_candidate& candidate) {
                            return !candidate.serving_satellite_id.empty();
                          }));
  EXPECT_EQ(std::count_if(schedule.candidates.begin(),
                          schedule.candidates.end(),
                          [](const ntn_served_beam_candidate& candidate) {
                            return candidate.beam_id == "CN-BEAM-0001";
                          }),
            1);
}

TEST(ntn_served_beam_scheduler, rotates_hopping_window_when_visible_pool_is_larger_than_active_limit)
{
  ntn_served_beam_scheduler        scheduler(make_rotating_multi_beam_scheduler_config());
  recording_served_beam_update_handler update_handler;

  ntn_served_beam_schedule first =
      scheduler.update_from_satellite_state(make_ecef(0.0, 0.0, 500000.0), update_handler);
  ASSERT_TRUE(first.changed);
  ASSERT_TRUE(first.applied);
  ASSERT_EQ(first.beam_ids, std::vector<std::string>({"CN-BEAM-0001", "CN-BEAM-0002"}));
  ASSERT_EQ(first.candidates.size(), 4);
  EXPECT_TRUE(first.candidates[0].in_hopping_window);
  EXPECT_TRUE(first.candidates[1].in_hopping_window);
  EXPECT_FALSE(first.candidates[2].in_hopping_window);
  EXPECT_FALSE(first.candidates[3].in_hopping_window);

  ntn_served_beam_schedule second =
      scheduler.update_from_satellite_state(make_ecef(0.0, 0.0, 500000.0), update_handler);
  ASSERT_TRUE(second.changed);
  ASSERT_TRUE(second.applied);
  ASSERT_EQ(second.beam_ids, std::vector<std::string>({"CN-BEAM-0003", "CN-BEAM-0004"}));
  ASSERT_EQ(second.candidates.size(), 4);
  EXPECT_FALSE(second.candidates[0].in_hopping_window);
  EXPECT_FALSE(second.candidates[1].in_hopping_window);
  EXPECT_TRUE(second.candidates[2].in_hopping_window);
  EXPECT_TRUE(second.candidates[3].in_hopping_window);

  ntn_served_beam_schedule third =
      scheduler.update_from_satellite_state(make_ecef(0.0, 0.0, 500000.0), update_handler);
  ASSERT_TRUE(third.changed);
  ASSERT_TRUE(third.applied);
  ASSERT_EQ(third.beam_ids, std::vector<std::string>({"CN-BEAM-0001", "CN-BEAM-0002"}));
  ASSERT_EQ(scheduler.current_served_beam_ids(), std::vector<std::string>({"CN-BEAM-0001", "CN-BEAM-0002"}));
  ASSERT_EQ(update_handler.updates.size(), 3);
}

TEST(ntn_served_beam_scheduler, keeps_hopping_window_for_configured_dwell_updates)
{
  ntn_served_beam_scheduler_config cfg = make_rotating_multi_beam_scheduler_config();
  cfg.served_beam_hopping_dwell_updates = 3;
  ntn_served_beam_scheduler        scheduler(cfg);
  recording_served_beam_update_handler update_handler;

  const ecef_coordinates_t satellite = make_ecef(0.0, 0.0, 500000.0);

  ntn_served_beam_schedule first = scheduler.update_from_satellite_state(satellite, update_handler);
  ASSERT_TRUE(first.changed);
  ASSERT_TRUE(first.applied);
  ASSERT_EQ(first.beam_ids, std::vector<std::string>({"CN-BEAM-0001", "CN-BEAM-0002"}));

  ntn_served_beam_schedule second = scheduler.update_from_satellite_state(satellite, update_handler);
  ASSERT_FALSE(second.changed);
  ASSERT_FALSE(second.applied);
  ASSERT_EQ(scheduler.current_served_beam_ids(), std::vector<std::string>({"CN-BEAM-0001", "CN-BEAM-0002"}));

  ntn_served_beam_schedule third = scheduler.update_from_satellite_state(satellite, update_handler);
  ASSERT_FALSE(third.changed);
  ASSERT_FALSE(third.applied);
  ASSERT_EQ(scheduler.current_served_beam_ids(), std::vector<std::string>({"CN-BEAM-0001", "CN-BEAM-0002"}));

  ntn_served_beam_schedule fourth = scheduler.update_from_satellite_state(satellite, update_handler);
  ASSERT_TRUE(fourth.changed);
  ASSERT_TRUE(fourth.applied);
  ASSERT_EQ(fourth.beam_ids, std::vector<std::string>({"CN-BEAM-0003", "CN-BEAM-0004"}));
  ASSERT_EQ(update_handler.updates.size(), 2);
}

TEST(ntn_served_beam_scheduler, keeps_hopping_window_when_visible_pool_only_reorders_before_dwell_expires)
{
  ntn_served_beam_scheduler_config cfg;
  cfg.beams = {{"CN-BEAM-0001", make_nci(0), 0.0, -1.5, 50000.0, true},
               {"CN-BEAM-0002", make_nci(1), 0.0, -0.5, 50000.0, true},
               {"CN-BEAM-0003", make_nci(2), 0.0, 0.5, 50000.0, true},
               {"CN-BEAM-0004", make_nci(3), 0.0, 1.5, 50000.0, true}};
  cfg.min_elevation_deg                  = -90.0;
  cfg.max_nof_served_beams               = 2;
  cfg.served_beam_hopping_enabled        = true;
  cfg.served_beam_hopping_dwell_updates  = 3;

  ntn_served_beam_scheduler        scheduler(cfg);
  recording_served_beam_update_handler update_handler;

  ntn_served_beam_schedule first =
      scheduler.update_from_satellite_state(make_ecef(0.0, -1.5, 500000.0), update_handler);
  ASSERT_TRUE(first.applied);
  ASSERT_EQ(first.beam_ids, std::vector<std::string>({"CN-BEAM-0001", "CN-BEAM-0002"}));

  ntn_served_beam_schedule reordered =
      scheduler.update_from_satellite_state(make_ecef(0.0, 1.5, 500000.0), update_handler);
  ASSERT_FALSE(reordered.applied);
  ASSERT_FALSE(reordered.changed);
  ASSERT_EQ(reordered.candidates.front().beam_id, "CN-BEAM-0004");
  ASSERT_EQ(reordered.beam_ids, std::vector<std::string>({"CN-BEAM-0001", "CN-BEAM-0002"}));
}

TEST(ntn_served_beam_scheduler, demand_aware_scheduler_prioritizes_loaded_service_beam_over_empty_high_elevation_beam)
{
  ntn_served_beam_scheduler_config cfg = make_rotating_multi_beam_scheduler_config();
  cfg.demand_aware_beam_scheduling_enabled = true;

  ntn_served_beam_scheduler scheduler(cfg);

  ntn_served_beam_demand demand;
  demand.beam_id  = "CN-BEAM-0003";
  demand.nof_ues  = 2;
  demand.nof_drbs = 2;

  const ntn_served_beam_schedule schedule =
      scheduler.compute_schedule(make_ecef(0.0, 0.0, 500000.0), std::vector<ntn_served_beam_demand>{demand});

  ASSERT_EQ(schedule.candidates.size(), 4);
  ASSERT_EQ(schedule.beam_ids.size(), 2);
  EXPECT_EQ(schedule.beam_ids.front(), "CN-BEAM-0003");
  EXPECT_TRUE(schedule.demand_prioritized_window);
  EXPECT_FALSE(schedule.legacy_fallback);
  const auto beam3 = std::find_if(schedule.candidates.begin(),
                                  schedule.candidates.end(),
                                  [](const ntn_served_beam_candidate& candidate) {
                                    return candidate.beam_id == "CN-BEAM-0003";
                                  });
  ASSERT_NE(beam3, schedule.candidates.end());
  EXPECT_TRUE(beam3->in_hopping_window);
  EXPECT_EQ(beam3->window_rank, 0U);
  EXPECT_EQ(beam3->scheduling_reason, "demand");
}

TEST(ntn_served_beam_scheduler, demand_aware_scheduler_selects_analog_cluster_with_aggregate_child_demand)
{
  ntn_served_beam_scheduler_config cfg;
  cfg.min_elevation_deg                    = -90.0;
  cfg.max_nof_served_beams                 = 0;
  cfg.max_nof_active_analog_access_beams   = 1;
  cfg.served_beam_hopping_enabled          = true;
  cfg.served_beam_hopping_dwell_updates    = 1;
  cfg.demand_aware_beam_scheduling_enabled = true;

  ntn_analog_beam_position analog1;
  analog1.analog_beam_id         = "LEO500-ANALOG-0001";
  analog1.center_digital_beam_id = "LEO500-DIGI-0001";
  analog1.child_digital_beam_ids = {"LEO500-DIGI-0001", "LEO500-DIGI-0002", "LEO500-DIGI-0003"};

  ntn_analog_beam_position analog2;
  analog2.analog_beam_id         = "LEO500-ANALOG-0002";
  analog2.center_digital_beam_id = "LEO500-DIGI-0004";
  analog2.child_digital_beam_ids = {"LEO500-DIGI-0004", "LEO500-DIGI-0005", "LEO500-DIGI-0006"};
  cfg.analog_beams               = {analog1, analog2};

  for (unsigned i = 0; i != 6; ++i) {
    ntn_beam_position beam;
    beam.beam_id              = "LEO500-DIGI-000" + std::to_string(i + 1);
    beam.nci                  = make_dense_nci(i);
    beam.center_latitude_deg  = 0.0;
    beam.center_longitude_deg = static_cast<double>(i) * 0.1;
    beam.coverage_radius_m    = 15000.0;
    beam.enabled              = true;
    beam.analog_beam_id       = i < 3 ? "LEO500-ANALOG-0001" : "LEO500-ANALOG-0002";
    cfg.beams.push_back(beam);
  }

  ntn_served_beam_demand demand;
  demand.beam_id  = "LEO500-DIGI-0005";
  demand.nof_ues  = 3;
  demand.nof_drbs = 3;

  ntn_served_beam_scheduler scheduler(cfg);
  const auto schedule =
      scheduler.compute_schedule(make_ecef(0.0, 0.0, 500000.0), std::vector<ntn_served_beam_demand>{demand});

  ASSERT_EQ(schedule.candidates.size(), 6);
  ASSERT_EQ(schedule.beam_ids.size(), 3);
  EXPECT_TRUE(std::all_of(schedule.beam_ids.begin(), schedule.beam_ids.end(), [](const std::string& beam_id) {
    return beam_id == "LEO500-DIGI-0004" || beam_id == "LEO500-DIGI-0005" || beam_id == "LEO500-DIGI-0006";
  }));
  EXPECT_TRUE(schedule.demand_prioritized_window);
}

TEST(ntn_served_beam_scheduler, active_analog_access_window_respects_digital_window_limit)
{
  ntn_served_beam_scheduler_config cfg;
  cfg.min_elevation_deg                  = -90.0;
  cfg.max_nof_served_beams               = 2;
  cfg.max_nof_active_analog_access_beams = 1;

  ntn_analog_beam_position analog;
  analog.analog_beam_id         = "LEO500-ANALOG-0001";
  analog.center_digital_beam_id = "LEO500-DIGI-0001";
  analog.child_digital_beam_ids = {"LEO500-DIGI-0001", "LEO500-DIGI-0002", "LEO500-DIGI-0003"};
  cfg.analog_beams             = {analog};

  for (unsigned i = 0; i != 3; ++i) {
    ntn_beam_position beam;
    beam.beam_id              = "LEO500-DIGI-000" + std::to_string(i + 1);
    beam.nci                  = make_dense_nci(i);
    beam.center_latitude_deg  = 0.0;
    beam.center_longitude_deg = static_cast<double>(i) * 0.1;
    beam.coverage_radius_m    = 15000.0;
    beam.enabled              = true;
    beam.analog_beam_id       = "LEO500-ANALOG-0001";
    cfg.beams.push_back(beam);
  }

  ntn_served_beam_scheduler scheduler(cfg);
  const auto                schedule = scheduler.compute_schedule(make_ecef(0.0, 0.0, 500000.0));

  ASSERT_EQ(schedule.candidates.size(), 3);
  EXPECT_EQ(schedule.beam_ids.size(), 2);
  EXPECT_EQ(std::count_if(schedule.candidates.begin(),
                          schedule.candidates.end(),
                          [](const ntn_served_beam_candidate& candidate) {
                            return candidate.in_hopping_window;
                          }),
            2);
}

TEST(ntn_served_beam_scheduler, demand_aware_scheduler_selects_demanded_child_inside_selected_analog_window)
{
  ntn_served_beam_scheduler_config cfg;
  cfg.min_elevation_deg                    = -90.0;
  cfg.max_nof_served_beams                 = 1;
  cfg.max_nof_active_analog_access_beams   = 1;
  cfg.demand_aware_beam_scheduling_enabled = true;

  ntn_analog_beam_position analog;
  analog.analog_beam_id         = "LEO500-ANALOG-0001";
  analog.center_digital_beam_id = "LEO500-DIGI-0001";
  analog.child_digital_beam_ids = {"LEO500-DIGI-0001", "LEO500-DIGI-0002", "LEO500-DIGI-0003"};
  cfg.analog_beams             = {analog};

  for (unsigned i = 0; i != 3; ++i) {
    ntn_beam_position beam;
    beam.beam_id              = "LEO500-DIGI-000" + std::to_string(i + 1);
    beam.nci                  = make_dense_nci(i);
    beam.center_latitude_deg  = 0.0;
    beam.center_longitude_deg = static_cast<double>(i) * 0.1;
    beam.coverage_radius_m    = 15000.0;
    beam.enabled              = true;
    beam.analog_beam_id       = "LEO500-ANALOG-0001";
    cfg.beams.push_back(beam);
  }

  ntn_served_beam_demand demand;
  demand.beam_id  = "LEO500-DIGI-0003";
  demand.nof_ues  = 1;
  demand.nof_drbs = 1;

  ntn_served_beam_scheduler scheduler(cfg);
  const auto schedule =
      scheduler.compute_schedule(make_ecef(0.0, 0.0, 500000.0), std::vector<ntn_served_beam_demand>{demand});

  ASSERT_EQ(schedule.beam_ids, std::vector<std::string>({"LEO500-DIGI-0003"}));
  EXPECT_TRUE(schedule.demand_prioritized_window);
  const auto selected_child = std::find_if(schedule.candidates.begin(),
                                           schedule.candidates.end(),
                                           [](const ntn_served_beam_candidate& candidate) {
                                             return candidate.beam_id == "LEO500-DIGI-0003";
                                           });
  ASSERT_NE(selected_child, schedule.candidates.end());
  EXPECT_TRUE(selected_child->in_hopping_window);
  EXPECT_EQ(selected_child->scheduling_reason, "demand");
}

TEST(ntn_served_beam_scheduler, analog_loaded_child_cap_limits_selected_digital_child_window)
{
  ntn_served_beam_scheduler_config cfg;
  cfg.min_elevation_deg                  = -90.0;
  cfg.max_nof_served_beams               = 0;
  cfg.max_nof_active_analog_access_beams = 1;

  ntn_analog_beam_position analog;
  analog.analog_beam_id         = "LEO500-ANALOG-0001";
  analog.center_digital_beam_id = "LEO500-DIGI-0001";
  analog.child_digital_beam_ids = {"LEO500-DIGI-0001", "LEO500-DIGI-0002"};
  analog.resource_policy.emplace();
  analog.resource_policy->max_loaded_digital_children = 1;
  cfg.analog_beams = {analog};

  for (unsigned i = 0; i != 2; ++i) {
    ntn_beam_position beam;
    beam.beam_id              = "LEO500-DIGI-000" + std::to_string(i + 1);
    beam.nci                  = make_dense_nci(i);
    beam.center_latitude_deg  = 0.0;
    beam.center_longitude_deg = static_cast<double>(i) * 0.1;
    beam.coverage_radius_m    = 15000.0;
    beam.enabled              = true;
    beam.analog_beam_id       = "LEO500-ANALOG-0001";
    cfg.beams.push_back(beam);
  }

  ntn_served_beam_scheduler scheduler(cfg);
  const auto                schedule = scheduler.compute_schedule(make_ecef(0.0, 0.0, 500000.0));

  EXPECT_EQ(schedule.beam_ids.size(), 1);
  EXPECT_EQ(std::count_if(schedule.candidates.begin(),
                          schedule.candidates.end(),
                          [](const ntn_served_beam_candidate& candidate) {
                            return candidate.in_hopping_window;
                          }),
            1);
}

TEST(ntn_served_beam_scheduler, qos_critical_beam_wins_scheduling_tie)
{
  ntn_served_beam_scheduler_config cfg = make_rotating_multi_beam_scheduler_config();
  cfg.max_nof_served_beams                 = 1;
  cfg.demand_aware_beam_scheduling_enabled = true;

  ntn_served_beam_demand ordinary;
  ordinary.beam_id  = "CN-BEAM-0001";
  ordinary.nof_ues  = 1;
  ordinary.nof_drbs = 1;

  ntn_served_beam_demand critical;
  critical.beam_id                     = "CN-BEAM-0002";
  critical.nof_ues                     = 1;
  critical.nof_drbs                    = 1;
  critical.qos.has_qos_demand          = true;
  critical.qos.has_gbr                 = true;
  critical.qos.has_delay_critical_gbr  = true;
  critical.qos.best_arp_priority       = 1;
  critical.qos.best_qos_priority       = 1;
  critical.qos.best_five_qi            = uint_to_five_qi(82);

  ntn_served_beam_scheduler scheduler(cfg);
  const auto schedule = scheduler.compute_schedule(
      make_ecef(0.0, 0.0, 500000.0), std::vector<ntn_served_beam_demand>{ordinary, critical});

  ASSERT_EQ(schedule.beam_ids, std::vector<std::string>({"CN-BEAM-0002"}));
  EXPECT_TRUE(schedule.demand_prioritized_window);
}

TEST(ntn_served_beam_scheduler, empty_demand_keeps_legacy_hopping_rotation)
{
  ntn_served_beam_scheduler_config cfg = make_rotating_multi_beam_scheduler_config();
  cfg.demand_aware_beam_scheduling_enabled = true;

  ntn_served_beam_scheduler scheduler(cfg);
  const auto schedule =
      scheduler.compute_schedule(make_ecef(0.0, 0.0, 500000.0), std::vector<ntn_served_beam_demand>{});

  ASSERT_EQ(schedule.beam_ids, std::vector<std::string>({"CN-BEAM-0001", "CN-BEAM-0002"}));
  EXPECT_FALSE(schedule.demand_prioritized_window);
  EXPECT_TRUE(schedule.legacy_fallback);
}

TEST(ntn_served_beam_scheduler, demand_aware_scheduler_replaces_empty_sticky_window_when_demand_appears)
{
  ntn_served_beam_scheduler_config cfg = make_rotating_multi_beam_scheduler_config();
  cfg.served_beam_hopping_dwell_updates        = 5;
  cfg.demand_aware_beam_scheduling_enabled     = true;

  ntn_served_beam_scheduler        scheduler(cfg);
  recording_served_beam_update_handler update_handler;
  const ecef_coordinates_t satellite = make_ecef(0.0, 0.0, 500000.0);

  const ntn_served_beam_schedule first =
      scheduler.update_from_satellite_state(satellite, std::vector<ntn_served_beam_demand>{}, update_handler);
  ASSERT_TRUE(first.applied);
  ASSERT_EQ(first.beam_ids, std::vector<std::string>({"CN-BEAM-0001", "CN-BEAM-0002"}));

  ntn_served_beam_demand demand;
  demand.beam_id  = "CN-BEAM-0003";
  demand.nof_ues  = 1;
  demand.nof_drbs = 1;
  const ntn_served_beam_schedule second =
      scheduler.update_from_satellite_state(satellite, std::vector<ntn_served_beam_demand>{demand}, update_handler);

  ASSERT_TRUE(second.changed);
  ASSERT_TRUE(second.applied);
  EXPECT_EQ(second.beam_ids.front(), "CN-BEAM-0003");
  EXPECT_TRUE(second.demand_prioritized_window);
  EXPECT_FALSE(second.sticky_kept);
}

TEST(ntn_served_beam_scheduler, applies_empty_set_when_no_configured_beam_is_visible)
{
  ntn_served_beam_scheduler        scheduler(make_scheduler_config());
  recording_served_beam_update_handler update_handler;

  ASSERT_TRUE(scheduler.update_from_satellite_state(make_ecef(0.0, 0.0, 500000.0), update_handler).applied);
  ASSERT_EQ(scheduler.current_served_beam_ids(), std::vector<std::string>({"CN-BEAM-0001"}));

  ntn_served_beam_schedule empty_schedule =
      scheduler.update_from_satellite_state(make_ecef(0.0, 180.0, 500000.0), update_handler);
  ASSERT_TRUE(empty_schedule.changed);
  ASSERT_TRUE(empty_schedule.applied);
  ASSERT_TRUE(empty_schedule.beam_ids.empty());
  ASSERT_TRUE(scheduler.current_served_beam_ids().empty());
  ASSERT_EQ(update_handler.updates.size(), 2);
}
