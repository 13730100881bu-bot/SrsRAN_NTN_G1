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

#include "srsran/ntn/orbit_propagator.h"
#include "srsran/ntn/ta_calculator.h"
#include "srsran/support/srsran_assert.h"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <limits>
#include <stdexcept>
#include <utility>

using namespace srsran;
using namespace srsran::srs_ntn;

namespace {

constexpr double pi                  = 3.14159265358979323846;
constexpr double earth_mu_m3_s2      = 3.986004418e14;
constexpr double earth_rotation_rad_s = 7.2921150e-5;
constexpr double earth_j2            = 1.08262668e-3;

struct vec3 {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
};

double deg_to_rad(double deg)
{
  return deg * pi / 180.0;
}

double rad_to_deg(double rad)
{
  return rad * 180.0 / pi;
}

double wrap_angle_rad(double angle)
{
  double wrapped = std::fmod(angle, 2.0 * pi);
  if (wrapped < 0.0) {
    wrapped += 2.0 * pi;
  }
  return wrapped;
}

double normalize_longitude_deg(double lon_deg)
{
  double normalized = std::fmod(lon_deg + 180.0, 360.0);
  if (normalized < 0.0) {
    normalized += 360.0;
  }
  return normalized - 180.0;
}

double vector_norm(const vec3& v)
{
  return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

double seconds_since_unix_epoch(std::chrono::system_clock::time_point time)
{
  return std::chrono::duration<double>(time.time_since_epoch()).count();
}

double seconds_between(std::chrono::system_clock::time_point lhs, std::chrono::system_clock::time_point rhs)
{
  return std::chrono::duration<double>(lhs - rhs).count();
}

double julian_date(std::chrono::system_clock::time_point time)
{
  return 2440587.5 + seconds_since_unix_epoch(time) / 86400.0;
}

double gmst_rad(std::chrono::system_clock::time_point time)
{
  const double jd = julian_date(time);
  const double t  = (jd - 2451545.0) / 36525.0;
  const double gmst_deg =
      280.46061837 + 360.98564736629 * (jd - 2451545.0) + 0.000387933 * t * t - t * t * t / 38710000.0;
  return wrap_angle_rad(deg_to_rad(gmst_deg));
}

geodetic_coordinates_t ecef_to_geodetic(const ecef_coordinates_t& ecef)
{
  const double a   = ntn_constants::WGS84_A;
  const double f   = ntn_constants::WGS84_F;
  const double e2  = ntn_constants::WGS84_E2;
  const double b   = a * (1.0 - f);
  const double ep2 = (a * a - b * b) / (b * b);

  const double p     = std::hypot(ecef.position_x, ecef.position_y);
  const double theta = std::atan2(ecef.position_z * a, p * b);
  const double st    = std::sin(theta);
  const double ct    = std::cos(theta);

  const double lat = std::atan2(ecef.position_z + ep2 * b * st * st * st, p - e2 * a * ct * ct * ct);
  const double lon = std::atan2(ecef.position_y, ecef.position_x);
  const double sl  = std::sin(lat);
  const double n   = a / std::sqrt(1.0 - e2 * sl * sl);
  const double alt = p / std::cos(lat) - n;

  return {rad_to_deg(lat), normalize_longitude_deg(rad_to_deg(lon)), alt};
}

ecef_coordinates_t eci_to_ecef(const vec3& r_eci, const vec3& v_eci, std::chrono::system_clock::time_point time)
{
  const double theta = gmst_rad(time);
  const double c     = std::cos(theta);
  const double s     = std::sin(theta);

  ecef_coordinates_t ecef{};
  ecef.position_x = c * r_eci.x + s * r_eci.y;
  ecef.position_y = -s * r_eci.x + c * r_eci.y;
  ecef.position_z = r_eci.z;

  const double vx_rot = c * v_eci.x + s * v_eci.y;
  const double vy_rot = -s * v_eci.x + c * v_eci.y;
  const double vz_rot = v_eci.z;

  ecef.velocity_vx = vx_rot + earth_rotation_rad_s * ecef.position_y;
  ecef.velocity_vy = vy_rot - earth_rotation_rad_s * ecef.position_x;
  ecef.velocity_vz = vz_rot;
  return ecef;
}

vec3 rotate_perifocal_to_eci(const vec3& perifocal, double raan, double inclination, double arg_perigee)
{
  const double cos_o = std::cos(raan);
  const double sin_o = std::sin(raan);
  const double cos_i = std::cos(inclination);
  const double sin_i = std::sin(inclination);
  const double cos_w = std::cos(arg_perigee);
  const double sin_w = std::sin(arg_perigee);

  const double r11 = cos_o * cos_w - sin_o * sin_w * cos_i;
  const double r12 = -cos_o * sin_w - sin_o * cos_w * cos_i;
  const double r21 = sin_o * cos_w + cos_o * sin_w * cos_i;
  const double r22 = -sin_o * sin_w + cos_o * cos_w * cos_i;
  const double r31 = sin_w * sin_i;
  const double r32 = cos_w * sin_i;

  return {r11 * perifocal.x + r12 * perifocal.y,
          r21 * perifocal.x + r22 * perifocal.y,
          r31 * perifocal.x + r32 * perifocal.y};
}

double solve_kepler(double mean_anomaly, double eccentricity)
{
  double eccentric_anomaly = eccentricity < 0.8 ? mean_anomaly : pi;
  for (unsigned i = 0; i != 12; ++i) {
    const double f  = eccentric_anomaly - eccentricity * std::sin(eccentric_anomaly) - mean_anomaly;
    const double fp = 1.0 - eccentricity * std::cos(eccentric_anomaly);
    eccentric_anomaly -= f / fp;
  }
  return eccentric_anomaly;
}

orbit_propagation_result propagate_keplerian(double semi_major_axis,
                                             double eccentricity,
                                             double inclination,
                                             double raan,
                                             double arg_perigee,
                                             double mean_anomaly,
                                             std::chrono::system_clock::time_point time)
{
  const double e = eccentricity;
  const double E = solve_kepler(wrap_angle_rad(mean_anomaly), e);
  const double c = std::cos(E);
  const double s = std::sin(E);
  const double r = semi_major_axis * (1.0 - e * c);

  const vec3 r_pf{semi_major_axis * (c - e), semi_major_axis * std::sqrt(1.0 - e * e) * s, 0.0};
  const double vel_factor = std::sqrt(earth_mu_m3_s2 * semi_major_axis) / r;
  const vec3   v_pf{-vel_factor * s, vel_factor * std::sqrt(1.0 - e * e) * c, 0.0};

  const vec3 r_eci = rotate_perifocal_to_eci(r_pf, raan, inclination, arg_perigee);
  const vec3 v_eci = rotate_perifocal_to_eci(v_pf, raan, inclination, arg_perigee);
  const ecef_coordinates_t ecef = eci_to_ecef(r_eci, v_eci, time);
  return {ecef, ecef_to_geodetic(ecef)};
}

std::string trim(std::string_view value)
{
  size_t begin = 0;
  while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) {
    ++begin;
  }

  size_t end = value.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
    --end;
  }

  return std::string(value.substr(begin, end - begin));
}

expected<double, std::string> parse_double_field(std::string_view value, std::string_view name)
{
  try {
    return std::stod(trim(value));
  } catch (const std::exception&) {
    return make_unexpected("Invalid TLE " + std::string(name));
  }
}

expected<uint32_t, std::string> parse_uint_field(std::string_view value, std::string_view name)
{
  try {
    return static_cast<uint32_t>(std::stoul(trim(value)));
  } catch (const std::exception&) {
    return make_unexpected("Invalid TLE " + std::string(name));
  }
}

expected<double, std::string> parse_implied_decimal(std::string_view value, std::string_view name)
{
  std::string digits = trim(value);
  if (digits.empty()) {
    return make_unexpected("Invalid TLE " + std::string(name));
  }

  bool negative = false;
  if (digits.front() == '+' || digits.front() == '-') {
    negative = digits.front() == '-';
    digits.erase(digits.begin());
  }
  if (digits.empty() || !std::all_of(digits.begin(), digits.end(), [](char c) { return std::isdigit(c); })) {
    return make_unexpected("Invalid TLE " + std::string(name));
  }

  const double value_out = std::stod("0." + digits);
  return negative ? -value_out : value_out;
}

expected<double, std::string> parse_tle_exponential(std::string_view value, std::string_view name)
{
  std::string field = trim(value);
  if (field.empty()) {
    return 0.0;
  }

  size_t exp_pos = std::string::npos;
  for (size_t i = 1; i < field.size(); ++i) {
    if (field[i] == '+' || field[i] == '-') {
      exp_pos = i;
    }
  }
  if (exp_pos == std::string::npos) {
    return make_unexpected("Invalid TLE " + std::string(name));
  }

  std::string mantissa = field.substr(0, exp_pos);
  const int   exponent = std::stoi(field.substr(exp_pos));
  bool        negative = false;
  if (!mantissa.empty() && (mantissa.front() == '+' || mantissa.front() == '-')) {
    negative = mantissa.front() == '-';
    mantissa.erase(mantissa.begin());
  }
  if (mantissa.empty() ||
      !std::all_of(mantissa.begin(), mantissa.end(), [](char c) { return std::isdigit(c); })) {
    return make_unexpected("Invalid TLE " + std::string(name));
  }

  double out = std::stod("0." + mantissa) * std::pow(10.0, exponent);
  return negative ? -out : out;
}

int64_t days_from_civil(int year, unsigned month, unsigned day)
{
  year -= month <= 2;
  const int      era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int>(doe) - 719468;
}

std::chrono::system_clock::time_point make_utc_time_point(int year, double day_of_year)
{
  const double days_since_unix_epoch = static_cast<double>(days_from_civil(year, 1, 1)) + day_of_year - 1.0;
  return std::chrono::system_clock::time_point{
      std::chrono::duration_cast<std::chrono::system_clock::duration>(
          std::chrono::duration<double>(days_since_unix_epoch * 86400.0))};
}

} // namespace

circular_orbit_propagator::circular_orbit_propagator(circular_orbit_model_config cfg_) : cfg(cfg_)
{
  srsran_assert(cfg.altitude_m > 0.0, "Circular orbit altitude must be positive");
}

orbit_propagation_result circular_orbit_propagator::propagate(std::chrono::system_clock::time_point time) const
{
  const double semi_major_axis = ntn_constants::WGS84_A + cfg.altitude_m;
  const double mean_motion     = std::sqrt(earth_mu_m3_s2 / (semi_major_axis * semi_major_axis * semi_major_axis));
  const double dt_s            = seconds_between(time, cfg.epoch);
  const double mean_anomaly    = deg_to_rad(cfg.argument_of_latitude_deg) + mean_motion * dt_s;

  return propagate_keplerian(semi_major_axis,
                             0.0,
                             deg_to_rad(cfg.inclination_deg),
                             deg_to_rad(cfg.raan_deg),
                             0.0,
                             mean_anomaly,
                             time);
}

double circular_orbit_propagator::orbital_period() const
{
  const double semi_major_axis = ntn_constants::WGS84_A + cfg.altitude_m;
  return 2.0 * pi * std::sqrt(semi_major_axis * semi_major_axis * semi_major_axis / earth_mu_m3_s2);
}

expected<tle_orbit_model, std::string> srsran::srs_ntn::parse_tle(std::string_view line1,
                                                                  std::string_view line2,
                                                                  std::string_view satellite_name)
{
  if (line1.size() < 63 || line2.size() < 63) {
    return make_unexpected("TLE lines are too short");
  }
  if (line1[0] != '1' || line2[0] != '2') {
    return make_unexpected("TLE line numbers are invalid");
  }

  auto sat_number = parse_uint_field(line1.substr(2, 5), "satellite number");
  if (!sat_number) {
    return make_unexpected(sat_number.error());
  }
  auto sat_number_line2 = parse_uint_field(line2.substr(2, 5), "line 2 satellite number");
  if (!sat_number_line2) {
    return make_unexpected(sat_number_line2.error());
  }
  if (sat_number.value() != sat_number_line2.value()) {
    return make_unexpected("TLE satellite numbers do not match");
  }

  auto epoch_year_field = parse_uint_field(line1.substr(18, 2), "epoch year");
  auto epoch_day_field  = parse_double_field(line1.substr(20, 12), "epoch day");
  auto inclination      = parse_double_field(line2.substr(8, 8), "inclination");
  auto raan             = parse_double_field(line2.substr(17, 8), "RAAN");
  auto eccentricity     = parse_implied_decimal(line2.substr(26, 7), "eccentricity");
  auto arg_perigee      = parse_double_field(line2.substr(34, 8), "argument of perigee");
  auto mean_anomaly     = parse_double_field(line2.substr(43, 8), "mean anomaly");
  auto mean_motion      = parse_double_field(line2.substr(52, 11), "mean motion");
  auto bstar            = line1.size() >= 61 ? parse_tle_exponential(line1.substr(53, 8), "BSTAR") :
                                               expected<double, std::string>{0.0};

  if (!epoch_year_field || !epoch_day_field || !inclination || !raan || !eccentricity || !arg_perigee ||
      !mean_anomaly || !mean_motion || !bstar) {
    return make_unexpected("Failed to parse TLE orbital elements");
  }
  if (eccentricity.value() < 0.0 || eccentricity.value() >= 1.0 || mean_motion.value() <= 0.0) {
    return make_unexpected("TLE orbital elements are out of range");
  }

  const int full_year =
      epoch_year_field.value() >= 57 ? 1900 + static_cast<int>(epoch_year_field.value()) :
                                       2000 + static_cast<int>(epoch_year_field.value());
  const double mean_motion_rad_s = mean_motion.value() * 2.0 * pi / 86400.0;
  const double semi_major_axis =
      std::cbrt(earth_mu_m3_s2 / (mean_motion_rad_s * mean_motion_rad_s));

  tle_orbit_model tle;
  tle.satellite_name     = trim(satellite_name);
  tle.satellite_number   = sat_number.value();
  tle.epoch              = make_utc_time_point(full_year, epoch_day_field.value());
  tle.mean_motion_rad_s  = mean_motion_rad_s;
  tle.bstar              = bstar.value();
  tle.elements.semi_major_axis = semi_major_axis;
  tle.elements.eccentricity    = eccentricity.value();
  tle.elements.periapsis       = deg_to_rad(arg_perigee.value());
  tle.elements.longitude       = deg_to_rad(raan.value());
  tle.elements.mean_anomaly    = deg_to_rad(mean_anomaly.value());
  tle.elements.inclination     = deg_to_rad(inclination.value());
  return tle;
}

tle_orbit_propagator::tle_orbit_propagator(tle_orbit_model model_) : tle(std::move(model_))
{
  srsran_assert(tle.mean_motion_rad_s > 0.0, "TLE mean motion must be positive");
}

orbit_propagation_result tle_orbit_propagator::propagate(std::chrono::system_clock::time_point time) const
{
  const double dt_s = seconds_between(time, tle.epoch);
  const double a    = tle.elements.semi_major_axis;
  const double e    = tle.elements.eccentricity;
  const double i    = tle.elements.inclination;
  const double n    = tle.mean_motion_rad_s;
  const double p    = a * (1.0 - e * e);
  const double k    = earth_j2 * std::pow(ntn_constants::WGS84_A / p, 2.0);
  const double ci   = std::cos(i);

  const double raan_dot = -1.5 * k * n * ci;
  const double argp_dot = 0.75 * k * n * (5.0 * ci * ci - 1.0);

  const double raan         = tle.elements.longitude + raan_dot * dt_s;
  const double arg_perigee  = tle.elements.periapsis + argp_dot * dt_s;
  const double mean_anomaly = tle.elements.mean_anomaly + n * dt_s;

  return propagate_keplerian(a, e, i, raan, arg_perigee, mean_anomaly, time);
}

std::vector<visible_beam_position> srsran::srs_ntn::select_visible_beams(const beam_position_grid& grid,
                                                                         const ecef_coordinates_t& satellite,
                                                                         double                    min_elevation_deg,
                                                                         uint16_t                  max_nof_beams)
{
  std::vector<visible_beam_position> visible;
  if (max_nof_beams == 0) {
    return visible;
  }

  for (const beam_position_t& beam : grid.positions) {
    const geodetic_coordinates_t beam_geo{beam.center_lat, beam.center_lon, 0.0};
    const ecef_coordinates_t     beam_ecef = geodetic_to_ecef(beam_geo);

    const vec3 los{satellite.position_x - beam_ecef.position_x,
                   satellite.position_y - beam_ecef.position_y,
                   satellite.position_z - beam_ecef.position_z};
    const double range = vector_norm(los);
    if (range <= 0.0) {
      continue;
    }

    const double lat = deg_to_rad(beam.center_lat);
    const double lon = deg_to_rad(beam.center_lon);
    const vec3   up{std::cos(lat) * std::cos(lon), std::cos(lat) * std::sin(lon), std::sin(lat)};
    const double sin_el = (los.x * up.x + los.y * up.y + los.z * up.z) / range;
    const double elevation_deg = rad_to_deg(std::asin(std::clamp(sin_el, -1.0, 1.0)));
    if (elevation_deg < min_elevation_deg) {
      continue;
    }

    visible.push_back({beam.beam_position_id, elevation_deg, range});
  }

  std::sort(visible.begin(), visible.end(), [](const visible_beam_position& lhs, const visible_beam_position& rhs) {
    if (lhs.elevation_deg == rhs.elevation_deg) {
      return lhs.beam_position_id < rhs.beam_position_id;
    }
    return lhs.elevation_deg > rhs.elevation_deg;
  });
  if (visible.size() > max_nof_beams) {
    visible.resize(max_nof_beams);
  }
  return visible;
}

active_beam_set_t srsran::srs_ntn::make_active_beam_set(const beam_position_grid&                  grid,
                                                        const std::vector<visible_beam_position>& beams)
{
  active_beam_set_t active{};
  const uint16_t    nof_beams =
      static_cast<uint16_t>(std::min<size_t>(beams.size(), MAX_ACTIVE_BEAM_POSITIONS));
  active.nof_beams = nof_beams;

  for (uint16_t i = 0; i != nof_beams; ++i) {
    const uint16_t beam_id = beams[i].beam_position_id;
    active.entries[i]     = {beam_id, grid.get_position(beam_id).color};
  }
  return active;
}

orbit_beam_scheduler::orbit_beam_scheduler(orbit_beam_scheduler_config cfg_, circular_orbit_propagator propagator_) :
  cfg(std::move(cfg_)), propagator(std::move(propagator_))
{
}

orbit_beam_scheduler::orbit_beam_scheduler(orbit_beam_scheduler_config cfg_, tle_orbit_propagator propagator_) :
  cfg(std::move(cfg_)), propagator(std::move(propagator_))
{
}

orbit_beam_schedule orbit_beam_scheduler::compute_schedule(std::chrono::system_clock::time_point time) const
{
  orbit_beam_schedule schedule;
  schedule.satellite = std::visit([time](const auto& model) { return model.propagate(time); }, propagator);
  schedule.visible_beams =
      select_visible_beams(cfg.grid, schedule.satellite.ecef, cfg.min_elevation_deg, cfg.max_nof_beams);
  schedule.active_beams = make_active_beam_set(cfg.grid, schedule.visible_beams);
  return schedule;
}
