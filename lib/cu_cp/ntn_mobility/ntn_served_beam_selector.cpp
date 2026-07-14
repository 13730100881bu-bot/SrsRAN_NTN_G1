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

#include "ntn_served_beam_selector.h"
#include <algorithm>
#include <cmath>

using namespace srsran;
using namespace srs_cu_cp;

namespace {

constexpr double pi        = 3.14159265358979323846;
constexpr double wgs84_a_m = 6378137.0;
constexpr double wgs84_f   = 1.0 / 298.257223563;
constexpr double wgs84_e2  = 2.0 * wgs84_f - wgs84_f * wgs84_f;

struct ecef_point {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
};

double deg_to_rad(double value)
{
  return value * pi / 180.0;
}

double rad_to_deg(double value)
{
  return value * 180.0 / pi;
}

ecef_point geodetic_to_ecef_point(double latitude_deg, double longitude_deg, double altitude_m)
{
  const double lat     = deg_to_rad(latitude_deg);
  const double lon     = deg_to_rad(longitude_deg);
  const double sin_lat = std::sin(lat);
  const double cos_lat = std::cos(lat);
  const double n       = wgs84_a_m / std::sqrt(1.0 - wgs84_e2 * sin_lat * sin_lat);

  return {(n + altitude_m) * cos_lat * std::cos(lon),
          (n + altitude_m) * cos_lat * std::sin(lon),
          (n * (1.0 - wgs84_e2) + altitude_m) * sin_lat};
}

double elevation_deg(const ecef_coordinates_t& satellite, const ntn_beam_position& beam)
{
  const ecef_point beam_ecef = geodetic_to_ecef_point(beam.center_latitude_deg, beam.center_longitude_deg, 0.0);
  const double     los_x     = satellite.position_x - beam_ecef.x;
  const double     los_y     = satellite.position_y - beam_ecef.y;
  const double     los_z     = satellite.position_z - beam_ecef.z;
  const double     range     = std::sqrt(los_x * los_x + los_y * los_y + los_z * los_z);
  if (range <= 0.0) {
    return -90.0;
  }

  const double lat = deg_to_rad(beam.center_latitude_deg);
  const double lon = deg_to_rad(beam.center_longitude_deg);
  const double up_x = std::cos(lat) * std::cos(lon);
  const double up_y = std::cos(lat) * std::sin(lon);
  const double up_z = std::sin(lat);
  const double sin_elevation = std::clamp((los_x * up_x + los_y * up_y + los_z * up_z) / range, -1.0, 1.0);
  return rad_to_deg(std::asin(sin_elevation));
}

} // namespace

std::vector<ntn_served_beam_candidate> srsran::srs_cu_cp::select_ntn_served_beam_candidates_by_elevation(
    const std::vector<ntn_beam_position>& beams,
    const ecef_coordinates_t&             satellite,
    double                                min_elevation_deg,
    unsigned                              max_nof_served_beams)
{
  std::vector<ntn_served_beam_candidate> candidates;
  candidates.reserve(beams.size());

  for (const auto& beam : beams) {
    if (!beam.enabled) {
      continue;
    }

    const double beam_elevation_deg = elevation_deg(satellite, beam);
    if (beam_elevation_deg >= min_elevation_deg) {
      candidates.push_back({beam.beam_id, beam_elevation_deg});
    }
  }

  std::sort(candidates.begin(),
            candidates.end(),
            [](const ntn_served_beam_candidate& lhs, const ntn_served_beam_candidate& rhs) {
              if (lhs.elevation_deg == rhs.elevation_deg) {
                return lhs.beam_id < rhs.beam_id;
              }
              return lhs.elevation_deg > rhs.elevation_deg;
            });

  if (max_nof_served_beams != 0 && candidates.size() > max_nof_served_beams) {
    candidates.resize(max_nof_served_beams);
  }
  return candidates;
}

std::vector<std::string> srsran::srs_cu_cp::select_ntn_served_beams_by_elevation(
    const std::vector<ntn_beam_position>& beams,
    const ecef_coordinates_t&             satellite,
    double                                min_elevation_deg,
    unsigned                              max_nof_served_beams)
{
  std::vector<ntn_served_beam_candidate> candidates =
      select_ntn_served_beam_candidates_by_elevation(beams, satellite, min_elevation_deg, max_nof_served_beams);
  std::vector<std::string> served_beam_ids;
  served_beam_ids.reserve(candidates.size());
  for (const auto& candidate : candidates) {
    served_beam_ids.push_back(candidate.beam_id);
  }
  return served_beam_ids;
}
