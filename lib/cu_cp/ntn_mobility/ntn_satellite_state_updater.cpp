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

#include "ntn_satellite_state_updater.h"
#include <utility>

using namespace srsran;
using namespace srs_cu_cp;

using orbit_propagator_variant = ntn_satellite_state_updater::orbit_propagator_variant;

static expected<orbit_propagator_variant, std::string>
create_orbit_propagator(const ntn_satellite_state_update_config& cfg)
{
  if (cfg.source == ntn_satellite_state_source::circular_orbit) {
    srs_ntn::circular_orbit_model_config orbit_cfg;
    orbit_cfg.altitude_m               = cfg.circular_altitude_m;
    orbit_cfg.inclination_deg          = cfg.circular_inclination_deg;
    orbit_cfg.raan_deg                 = cfg.circular_raan_deg;
    orbit_cfg.argument_of_latitude_deg = cfg.circular_argument_of_latitude_deg;
    orbit_cfg.epoch                    = cfg.circular_epoch;
    return orbit_propagator_variant{srs_ntn::circular_orbit_propagator{orbit_cfg}};
  }

  if (cfg.source == ntn_satellite_state_source::tle) {
    auto tle = srs_ntn::parse_tle(cfg.tle_line1, cfg.tle_line2, cfg.tle_satellite_name);
    if (!tle.has_value()) {
      return make_unexpected(tle.error());
    }
    return orbit_propagator_variant{srs_ntn::tle_orbit_propagator{tle.value()}};
  }

  return make_unexpected("manual satellite state source has no orbit propagator");
}

ntn_satellite_state_updater::ntn_satellite_state_updater(ntn_satellite_state_update_config cfg_,
                                                         orbit_propagator_variant          propagator_,
                                                         cu_cp_ntn_command_handler&        command_handler_,
                                                         timer_manager&                    timers,
                                                         task_executor&                    executor,
                                                         srslog::basic_logger&             logger_) :
  cfg(std::move(cfg_)),
  propagator(std::move(propagator_)),
  command_handler(command_handler_),
  update_timer(timers.create_unique_timer(executor)),
  logger(logger_)
{
}

ntn_satellite_state_updater::~ntn_satellite_state_updater()
{
  stop();
}

void ntn_satellite_state_updater::start()
{
  if (running) {
    return;
  }

  running = true;
  update_satellite_state();
  schedule_next_update();
}

void ntn_satellite_state_updater::stop()
{
  running = false;
  if (update_timer.is_valid()) {
    update_timer.stop();
  }
}

void ntn_satellite_state_updater::handle_timer_expired()
{
  if (!running) {
    return;
  }

  update_satellite_state();
  schedule_next_update();
}

void ntn_satellite_state_updater::schedule_next_update()
{
  if (!running) {
    return;
  }

  update_timer.set(cfg.update_period, [this](timer_id_t /*timer_id*/) { handle_timer_expired(); });
  update_timer.run();
}

void ntn_satellite_state_updater::update_satellite_state()
{
  const auto now   = std::chrono::system_clock::now();
  auto       state = std::visit([now](const auto& orbit) { return orbit.propagate(now); }, propagator);

  const bool accepted = command_handler.handle_ntn_satellite_state_update(state.ecef);
  logger.debug("NTN orbit state update sub_satellite_lat={}deg sub_satellite_lon={}deg altitude={}m accepted={}",
               state.sub_satellite_point.latitude,
               state.sub_satellite_point.longitude,
               state.sub_satellite_point.altitude,
               accepted);
}

expected<std::unique_ptr<ntn_satellite_state_updater>, std::string>
srsran::srs_cu_cp::create_ntn_satellite_state_updater(const ntn_satellite_state_update_config& cfg,
                                                      cu_cp_ntn_command_handler& command_handler,
                                                      timer_manager&             timers,
                                                      task_executor&             executor,
                                                      srslog::basic_logger&      logger)
{
  if (cfg.source == ntn_satellite_state_source::manual || cfg.update_period.count() == 0) {
    return std::unique_ptr<ntn_satellite_state_updater>{};
  }

  if (cfg.update_period.count() < 0) {
    return make_unexpected("satellite state update period must not be negative");
  }

  auto propagator = create_orbit_propagator(cfg);
  if (!propagator.has_value()) {
    return make_unexpected(propagator.error());
  }

  return std::make_unique<ntn_satellite_state_updater>(
      cfg, std::move(propagator.value()), command_handler, timers, executor, logger);
}
