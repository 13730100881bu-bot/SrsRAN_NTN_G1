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
using orbit_propagator_entry   = ntn_satellite_state_updater::orbit_propagator_entry;

static srs_ntn::circular_orbit_model_config
make_circular_orbit_model_config(const ntn_circular_orbit_satellite_config& cfg)
{
  srs_ntn::circular_orbit_model_config orbit_cfg;
  orbit_cfg.altitude_m               = cfg.altitude_m;
  orbit_cfg.inclination_deg          = cfg.inclination_deg;
  orbit_cfg.raan_deg                 = cfg.raan_deg;
  orbit_cfg.argument_of_latitude_deg = cfg.argument_of_latitude_deg;
  orbit_cfg.epoch                    = cfg.epoch;
  return orbit_cfg;
}

static ntn_circular_orbit_satellite_config make_legacy_single_circular_satellite(
    const ntn_satellite_state_update_config& cfg)
{
  ntn_circular_orbit_satellite_config satellite;
  satellite.satellite_id             = "sat-0";
  satellite.altitude_m               = cfg.circular_altitude_m;
  satellite.inclination_deg          = cfg.circular_inclination_deg;
  satellite.raan_deg                 = cfg.circular_raan_deg;
  satellite.argument_of_latitude_deg = cfg.circular_argument_of_latitude_deg;
  satellite.epoch                    = cfg.circular_epoch;
  return satellite;
}

static expected<std::vector<orbit_propagator_entry>, std::string>
create_orbit_propagators(const ntn_satellite_state_update_config& cfg)
{
  if (cfg.source == ntn_satellite_state_source::circular_orbit) {
    std::vector<ntn_circular_orbit_satellite_config> satellites = cfg.circular_orbit_satellites;
    if (satellites.empty()) {
      satellites.push_back(make_legacy_single_circular_satellite(cfg));
    }
    std::vector<orbit_propagator_entry> propagators;
    propagators.reserve(satellites.size());
    for (const ntn_circular_orbit_satellite_config& satellite : satellites) {
      if (satellite.satellite_id.empty()) {
        return make_unexpected("circular orbit satellite id must not be empty");
      }
      propagators.push_back({satellite.satellite_id,
                             orbit_propagator_variant{srs_ntn::circular_orbit_propagator{
                                 make_circular_orbit_model_config(satellite)}}});
    }
    return propagators;
  }

  if (cfg.source == ntn_satellite_state_source::tle) {
    auto tle = srs_ntn::parse_tle(cfg.tle_line1, cfg.tle_line2, cfg.tle_satellite_name);
    if (!tle.has_value()) {
      return make_unexpected(tle.error());
    }
    return std::vector<orbit_propagator_entry>{{"sat-0", orbit_propagator_variant{srs_ntn::tle_orbit_propagator{tle.value()}}}};
  }

  return make_unexpected("manual satellite state source has no orbit propagator");
}

ntn_satellite_state_updater::ntn_satellite_state_updater(ntn_satellite_state_update_config cfg_,
                                                         std::vector<orbit_propagator_entry> propagators_,
                                                         cu_cp_ntn_command_handler&        command_handler_,
                                                         timer_manager&                    timers,
                                                         task_executor&                    executor,
                                                         srslog::basic_logger&             logger_) :
  cfg(std::move(cfg_)),
  propagators(std::move(propagators_)),
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
  const auto now = std::chrono::system_clock::now();
  std::vector<ntn_satellite_state> current_satellites;
  current_satellites.reserve(propagators.size());
  for (const orbit_propagator_entry& entry : propagators) {
    auto state = std::visit([now](const auto& orbit) { return orbit.propagate(now); }, entry.propagator);
    current_satellites.push_back({entry.satellite_id, state.ecef});
  }

  std::vector<ntn_satellite_prediction_step> future_steps;
  const std::chrono::milliseconds horizon = cfg.predictive_service_window_horizon.count() > 0
                                                ? cfg.predictive_service_window_horizon
                                                : cfg.update_period;
  for (std::chrono::milliseconds offset = cfg.update_period; offset <= horizon && future_steps.size() < 64U;
       offset += cfg.update_period) {
    ntn_satellite_prediction_step step;
    step.offset = offset;
    step.satellites.reserve(propagators.size());
    const auto step_time = now + offset;
    for (const orbit_propagator_entry& entry : propagators) {
      auto state = std::visit([step_time](const auto& orbit) { return orbit.propagate(step_time); }, entry.propagator);
      step.satellites.push_back({entry.satellite_id, state.ecef});
    }
    future_steps.push_back(std::move(step));
  }

  const bool accepted = command_handler.handle_ntn_satellite_state_update(current_satellites, future_steps);
  logger.debug("NTN orbit state update nof_satellites={} timeline_steps={} accepted={}",
               current_satellites.size(),
               future_steps.size(),
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

  auto propagators = create_orbit_propagators(cfg);
  if (!propagators.has_value()) {
    return make_unexpected(propagators.error());
  }

  return std::make_unique<ntn_satellite_state_updater>(
      cfg, std::move(propagators.value()), command_handler, timers, executor, logger);
}
