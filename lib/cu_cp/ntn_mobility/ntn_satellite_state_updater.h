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

#include "srsran/adt/expected.h"
#include "srsran/cu_cp/cell_meas_manager_config.h"
#include "srsran/cu_cp/cu_cp_command_handler.h"
#include "srsran/ntn/orbit_propagator.h"
#include "srsran/srslog/srslog.h"
#include "srsran/support/executors/task_executor.h"
#include "srsran/support/timers.h"
#include <memory>
#include <variant>

namespace srsran {
namespace srs_cu_cp {

/// Periodically computes NTN satellite ECEF state and forwards it to the CU-CP NTN command handler.
class ntn_satellite_state_updater
{
public:
  using orbit_propagator_variant =
      std::variant<srs_ntn::circular_orbit_propagator, srs_ntn::tle_orbit_propagator>;
  struct orbit_propagator_entry {
    std::string              satellite_id = "sat-0";
    orbit_propagator_variant propagator;
  };

  ntn_satellite_state_updater(ntn_satellite_state_update_config cfg_,
                              std::vector<orbit_propagator_entry> propagators_,
                              cu_cp_ntn_command_handler&        command_handler_,
                              timer_manager&                    timers,
                              task_executor&                    executor,
                              srslog::basic_logger&             logger_);
  ~ntn_satellite_state_updater();

  void start();
  void stop();

private:
  void handle_timer_expired();
  void schedule_next_update();
  void update_satellite_state();

  ntn_satellite_state_update_config cfg;
  std::vector<orbit_propagator_entry> propagators;
  cu_cp_ntn_command_handler&        command_handler;
  unique_timer                      update_timer;
  srslog::basic_logger&             logger;
  bool                              running = false;
};

expected<std::unique_ptr<ntn_satellite_state_updater>, std::string>
create_ntn_satellite_state_updater(const ntn_satellite_state_update_config& cfg,
                                   cu_cp_ntn_command_handler&               command_handler,
                                   timer_manager&                           timers,
                                   task_executor&                           executor,
                                   srslog::basic_logger&                    logger);

} // namespace srs_cu_cp
} // namespace srsran
