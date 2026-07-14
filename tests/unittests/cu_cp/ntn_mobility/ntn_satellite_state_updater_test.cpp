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

#include "lib/cu_cp/ntn_mobility/ntn_satellite_state_updater.h"
#include "srsran/support/executors/manual_task_worker.h"
#include <cmath>
#include <gtest/gtest.h>

using namespace srsran;
using namespace srs_cu_cp;

namespace {

double ecef_radius(const ecef_coordinates_t& ecef)
{
  return std::sqrt(ecef.position_x * ecef.position_x + ecef.position_y * ecef.position_y +
                   ecef.position_z * ecef.position_z);
}

class recording_ntn_command_handler : public cu_cp_ntn_command_handler
{
public:
  bool handle_ntn_satellite_state_update(const ecef_coordinates_t& satellite) override
  {
    satellite_updates.push_back(satellite);
    return true;
  }

  std::vector<std::string> get_current_ntn_served_beam_ids() const override { return {}; }

  std::vector<cu_cp_ntn_beam_status> get_current_ntn_beam_status() const override { return {}; }

  std::vector<ecef_coordinates_t> satellite_updates;
};

ntn_satellite_state_update_config make_circular_update_config()
{
  ntn_satellite_state_update_config cfg;
  cfg.source                                  = ntn_satellite_state_source::circular_orbit;
  cfg.update_period                           = std::chrono::milliseconds{5};
  cfg.circular_altitude_m                     = 500000.0;
  cfg.circular_inclination_deg                = 53.0;
  cfg.circular_raan_deg                       = 0.0;
  cfg.circular_argument_of_latitude_deg       = 0.0;
  cfg.circular_epoch                          = std::chrono::system_clock::now();
  return cfg;
}

ntn_satellite_state_update_config make_tle_update_config()
{
  ntn_satellite_state_update_config cfg;
  cfg.source             = ntn_satellite_state_source::tle;
  cfg.update_period      = std::chrono::milliseconds{5};
  cfg.tle_satellite_name = "ISS";
  cfg.tle_line1          = "1 25544U 98067A   24001.54791435  .00016717  00000+0  10270-3 0  9993";
  cfg.tle_line2          = "2 25544  51.6416  48.9053 0006703  73.7123  75.0335 15.50000000430604";
  return cfg;
}

} // namespace

TEST(ntn_satellite_state_updater, manual_source_does_not_create_updater)
{
  timer_manager                 timers{8};
  manual_task_worker            worker{8};
  recording_ntn_command_handler command_handler;

  ntn_satellite_state_update_config cfg;
  cfg.source = ntn_satellite_state_source::manual;

  auto updater = create_ntn_satellite_state_updater(
      cfg, command_handler, timers, worker, srslog::fetch_basic_logger("TEST"));

  ASSERT_TRUE(updater.has_value());
  ASSERT_EQ(updater.value(), nullptr);
}

TEST(ntn_satellite_state_updater, circular_orbit_source_pushes_initial_and_periodic_satellite_state)
{
  timer_manager                 timers{8};
  manual_task_worker            worker{8};
  recording_ntn_command_handler command_handler;

  auto updater = create_ntn_satellite_state_updater(
      make_circular_update_config(), command_handler, timers, worker, srslog::fetch_basic_logger("TEST"));

  ASSERT_TRUE(updater.has_value()) << updater.error();
  ASSERT_NE(updater.value(), nullptr);

  updater.value()->start();
  ASSERT_EQ(command_handler.satellite_updates.size(), 1);
  EXPECT_NEAR(ecef_radius(command_handler.satellite_updates.front()), 6878137.0, 1000.0);

  for (unsigned i = 0; i != 5; ++i) {
    timers.tick();
    worker.run_pending_tasks();
  }

  ASSERT_GE(command_handler.satellite_updates.size(), 2);
  updater.value()->stop();
}

TEST(ntn_satellite_state_updater, update_period_zero_disables_orbit_driven_updater)
{
  timer_manager                 timers{8};
  manual_task_worker            worker{8};
  recording_ntn_command_handler command_handler;

  ntn_satellite_state_update_config cfg = make_circular_update_config();
  cfg.update_period                    = std::chrono::milliseconds{0};

  auto updater = create_ntn_satellite_state_updater(
      cfg, command_handler, timers, worker, srslog::fetch_basic_logger("TEST"));

  ASSERT_TRUE(updater.has_value());
  ASSERT_EQ(updater.value(), nullptr);
  ASSERT_TRUE(command_handler.satellite_updates.empty());
}

TEST(ntn_satellite_state_updater, tle_source_creates_satellite_state_updater)
{
  timer_manager                 timers{8};
  manual_task_worker            worker{8};
  recording_ntn_command_handler command_handler;

  auto updater = create_ntn_satellite_state_updater(
      make_tle_update_config(), command_handler, timers, worker, srslog::fetch_basic_logger("TEST"));

  ASSERT_TRUE(updater.has_value()) << updater.error();
  ASSERT_NE(updater.value(), nullptr);

  updater.value()->start();
  ASSERT_EQ(command_handler.satellite_updates.size(), 1);
  EXPECT_GT(ecef_radius(command_handler.satellite_updates.front()), 6000000.0);
  EXPECT_LT(ecef_radius(command_handler.satellite_updates.front()), 8000000.0);

  for (unsigned i = 0; i != 5; ++i) {
    timers.tick();
    worker.run_pending_tasks();
  }

  ASSERT_GE(command_handler.satellite_updates.size(), 2);
  updater.value()->stop();
}

TEST(ntn_satellite_state_updater, invalid_tle_returns_factory_error)
{
  timer_manager                 timers{8};
  manual_task_worker            worker{8};
  recording_ntn_command_handler command_handler;

  ntn_satellite_state_update_config cfg = make_tle_update_config();
  cfg.tle_line1                         = "not a valid TLE";

  auto updater = create_ntn_satellite_state_updater(
      cfg, command_handler, timers, worker, srslog::fetch_basic_logger("TEST"));

  ASSERT_FALSE(updater.has_value());
  ASSERT_NE(updater.error().find("TLE"), std::string::npos);
  ASSERT_TRUE(command_handler.satellite_updates.empty());
}
