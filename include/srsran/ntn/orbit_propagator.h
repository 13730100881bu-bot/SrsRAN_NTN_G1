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

#include "beam_hopping_table.h"
#include "beam_position.h"
#include "srsran/adt/expected.h"
#include "srsran/ran/ntn.h"
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace srsran {
namespace srs_ntn {

/// ECEF satellite state plus the corresponding sub-satellite point.
struct orbit_propagation_result {
  ecef_coordinates_t     ecef;
  geodetic_coordinates_t sub_satellite_point;
};

/// Configuration for a circular Keplerian LEO orbit.
struct circular_orbit_model_config {
  double                                 altitude_m = 500000.0;
  double                                 inclination_deg = 53.0;
  double                                 raan_deg = 0.0;
  double                                 argument_of_latitude_deg = 0.0;
  std::chrono::system_clock::time_point  epoch = std::chrono::system_clock::time_point{};
};

/// Propagates a circular orbit and converts the resulting ECI state to ECEF.
class circular_orbit_propagator
{
public:
  explicit circular_orbit_propagator(circular_orbit_model_config cfg_);

  orbit_propagation_result propagate(std::chrono::system_clock::time_point time) const;
  double                   orbital_period() const;

private:
  circular_orbit_model_config cfg;
};

/// TLE orbital elements decoded from the two text lines.
struct tle_orbit_model {
  std::string                            satellite_name;
  uint32_t                               satellite_number = 0;
  std::chrono::system_clock::time_point  epoch = std::chrono::system_clock::time_point{};
  orbital_coordinates_t                  elements{};
  double                                 mean_motion_rad_s = 0.0;
  double                                 bstar = 0.0;
};

/// Parse a two-line element set.
expected<tle_orbit_model, std::string> parse_tle(std::string_view line1,
                                                 std::string_view line2,
                                                 std::string_view satellite_name = {});

/// Propagates TLE elements with a compact Kepler + J2 secular model.
///
/// This is intentionally kept behind a TLE-oriented API so that a full SGP4 implementation can replace the propagation
/// core later without changing callers.
class tle_orbit_propagator
{
public:
  explicit tle_orbit_propagator(tle_orbit_model model_);

  orbit_propagation_result propagate(std::chrono::system_clock::time_point time) const;

  const tle_orbit_model& model() const { return tle; }

private:
  tle_orbit_model tle;
};

/// A beam position that is currently visible from the satellite.
struct visible_beam_position {
  uint16_t beam_position_id = INVALID_BEAM_POSITION_ID;
  double   elevation_deg = 0.0;
  double   slant_range_m = 0.0;
};

/// Select currently visible beam positions, sorted by highest elevation first.
std::vector<visible_beam_position> select_visible_beams(const beam_position_grid& grid,
                                                        const ecef_coordinates_t& satellite,
                                                        double                    min_elevation_deg,
                                                        uint16_t                  max_nof_beams);

/// Convert a visible beam vector into the active beam set used by the beam hopping code.
active_beam_set_t make_active_beam_set(const beam_position_grid&                  grid,
                                       const std::vector<visible_beam_position>& beams);

/// Output of one orbit-driven beam scheduling decision.
struct orbit_beam_schedule {
  orbit_propagation_result             satellite;
  std::vector<visible_beam_position>   visible_beams;
  active_beam_set_t                    active_beams;
};

/// Configuration for orbit-driven served beam selection.
struct orbit_beam_scheduler_config {
  beam_position_grid grid;
  double             min_elevation_deg = 10.0;
  uint16_t           max_nof_beams = 1;
};

/// Computes the served beam set from the current satellite orbit state.
class orbit_beam_scheduler
{
public:
  orbit_beam_scheduler(orbit_beam_scheduler_config cfg_, circular_orbit_propagator propagator_);
  orbit_beam_scheduler(orbit_beam_scheduler_config cfg_, tle_orbit_propagator propagator_);

  orbit_beam_schedule compute_schedule(std::chrono::system_clock::time_point time) const;

private:
  orbit_beam_scheduler_config                         cfg;
  std::variant<circular_orbit_propagator, tle_orbit_propagator> propagator;
};

} // namespace srs_ntn
} // namespace srsran
