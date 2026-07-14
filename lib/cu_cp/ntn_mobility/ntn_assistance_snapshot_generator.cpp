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

#include "ntn_assistance_snapshot_generator.h"
#include "srsran/ntn/ta_calculator.h"
#include <cmath>
#include <map>

using namespace srsran;
using namespace srs_cu_cp;

namespace {

// Keep these local bounds aligned with lib/ntn/ta_calculator.cpp. CU-CP uses
// them as a preflight check so assistance generation degrades gracefully instead
// of aborting when an orbit sample is outside the current ASN.1 TA range.
static constexpr double max_ta_common_us       = 66485757.0 * 0.004072;
static constexpr double max_ta_drift_abs_us_s  = 257303.0 * 0.0002;
static constexpr double max_ta_drift_var_us_s2 = 28949.0 * 0.00002;

bool is_service_or_candidate_state(ntn_beam_assignment_state state)
{
  return state == ntn_beam_assignment_state::candidate || state == ntn_beam_assignment_state::active_loaded ||
         state == ntn_beam_assignment_state::draining;
}

ntn_assistance_beam_state to_assistance_state(ntn_beam_assignment_state state)
{
  switch (state) {
    case ntn_beam_assignment_state::active_loaded:
      return ntn_assistance_beam_state::active_loaded;
    case ntn_beam_assignment_state::draining:
      return ntn_assistance_beam_state::draining;
    case ntn_beam_assignment_state::candidate:
    case ntn_beam_assignment_state::inactive:
      return ntn_assistance_beam_state::candidate;
  }
  return ntn_assistance_beam_state::candidate;
}

bool is_ta_asn1_encodable(const geodetic_coordinates_t& beam_center, const ecef_coordinates_t& satellite)
{
  const ecef_coordinates_t beam = srsran::srs_ntn::geodetic_to_ecef({beam_center.latitude, beam_center.longitude, 0.0});

  const double dx = satellite.position_x - beam.position_x;
  const double dy = satellite.position_y - beam.position_y;
  const double dz = satellite.position_z - beam.position_z;
  const double r  = std::sqrt(dx * dx + dy * dy + dz * dz);
  if (!std::isfinite(r) || r <= 0.0) {
    return false;
  }

  const double radial_velocity =
      (dx * satellite.velocity_vx + dy * satellite.velocity_vy + dz * satellite.velocity_vz) / r;
  const double speed_squared = satellite.velocity_vx * satellite.velocity_vx +
                               satellite.velocity_vy * satellite.velocity_vy +
                               satellite.velocity_vz * satellite.velocity_vz;

  const double ta_common = 2.0 * r / srsran::srs_ntn::ntn_constants::SPEED_OF_LIGHT_M_PER_US;
  const double ta_drift  = 2.0 * radial_velocity / srsran::srs_ntn::ntn_constants::SPEED_OF_LIGHT_M_PER_US;
  const double ta_drift_variant =
      2.0 * (speed_squared - radial_velocity * radial_velocity) /
      (r * srsran::srs_ntn::ntn_constants::SPEED_OF_LIGHT_M_PER_US);

  return std::isfinite(ta_common) && std::isfinite(ta_drift) && std::isfinite(ta_drift_variant) &&
         ta_common >= 0.0 && ta_common <= max_ta_common_us && std::abs(ta_drift) <= max_ta_drift_abs_us_s &&
         ta_drift_variant >= 0.0 && ta_drift_variant <= max_ta_drift_var_us_s2;
}

} // namespace

ntn_assistance_snapshot srsran::srs_cu_cp::build_ntn_assistance_snapshot(
    const ntn_assistance_snapshot_request& request)
{
  ntn_assistance_snapshot snapshot;
  if (!request.satellite_ecef.has_value()) {
    snapshot.valid          = false;
    snapshot.invalid_reason = ntn_assistance_invalid_reason::no_satellite_state;
    return snapshot;
  }

  if (request.satellite_state_max_age.count() > 0 && request.satellite_received_time.has_value() &&
      request.now - request.satellite_received_time.value() > request.satellite_state_max_age) {
    snapshot.valid          = false;
    snapshot.invalid_reason = ntn_assistance_invalid_reason::stale_satellite_state;
    return snapshot;
  }

  std::map<std::string, ntn_beam_position> beams_by_id;
  for (const auto& beam : request.beams) {
    if (beam.enabled && !beam.beam_id.empty()) {
      beams_by_id[beam.beam_id] = beam;
    }
  }

  snapshot.valid           = true;
  snapshot.invalid_reason  = ntn_assistance_invalid_reason::none;
  snapshot.satellite_ecef  = request.satellite_ecef;
  snapshot.satellite_states = request.satellite_states;
  snapshot.satellite_epoch = request.satellite_epoch;

  std::map<std::string, ecef_coordinates_t> satellite_states_by_id;
  for (const ntn_satellite_state& satellite : request.satellite_states) {
    if (!satellite.satellite_id.empty()) {
      satellite_states_by_id[satellite.satellite_id] = satellite.ecef;
    }
  }

  const unsigned max_beams = request.max_snapshot_beams;
  for (const auto& assignment : request.placement_plan.assignments) {
    if (max_beams != 0 && snapshot.beams.size() >= max_beams) {
      break;
    }
    if (!is_service_or_candidate_state(assignment.state)) {
      continue;
    }

    const auto beam_it = beams_by_id.find(assignment.beam_id);
    if (beam_it == beams_by_id.end()) {
      continue;
    }

    const ntn_beam_position& beam = beam_it->second;
    ntn_assistance_beam_snapshot beam_snapshot;
    beam_snapshot.beam_id                = beam.beam_id;
    beam_snapshot.serving_satellite_id =
        assignment.serving_satellite_id.empty() ? std::string{"sat-0"} : assignment.serving_satellite_id;
    beam_snapshot.nci                    = beam.nci;
    beam_snapshot.state                  = to_assistance_state(assignment.state);
    beam_snapshot.reference_location     = {beam.center_latitude_deg, beam.center_longitude_deg, 0.0};
    beam_snapshot.cell_specific_koffset  = request.cell_specific_koffset;
    beam_snapshot.k_mac                  = request.k_mac;
    beam_snapshot.ul_sync_validity_s     = request.ul_sync_validity_s;
    beam_snapshot.t_service              = request.t_service;
    const auto serving_satellite_it = satellite_states_by_id.find(beam_snapshot.serving_satellite_id);
    const ecef_coordinates_t& serving_satellite =
        serving_satellite_it != satellite_states_by_id.end() ? serving_satellite_it->second : request.satellite_ecef.value();
    if (is_ta_asn1_encodable(beam_snapshot.reference_location, serving_satellite)) {
      beam_snapshot.ta_info =
          srsran::srs_ntn::compute_beam_ta(beam_snapshot.reference_location, serving_satellite).ta;
    }
    snapshot.beams.push_back(beam_snapshot);
  }

  return snapshot;
}
