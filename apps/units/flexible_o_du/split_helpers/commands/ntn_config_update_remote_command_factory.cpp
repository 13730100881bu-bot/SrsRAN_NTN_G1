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

#include "ntn_config_update_remote_command_factory.h"
#include "apps/services/remote_control/remote_command.h"
#include "apps/units/application_unit_commands.h"
#include "fmt/format.h"
#include "nlohmann/json.hpp"
#include "srsran/ntn/beam_hopping_table.h"
#include "srsran/ntn/ntn_configuration_manager.h"
#include "srsran/ran/nr_cgi.h"
#include "srsran/ran/slot_point.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>

using namespace srsran;

#ifndef SRSRAN_HAS_ENTERPRISE_NTN

namespace {

static expected<double, std::string> get_required_number(const nlohmann::json& obj, const char* key)
{
  if (!obj.is_object()) {
    return make_unexpected("JSON object value type should be an object");
  }

  auto it = obj.find(key);
  if (it == obj.end()) {
    return make_unexpected(fmt::format("'{}' object is missing and it is mandatory", key));
  }
  if (!it->is_number()) {
    return make_unexpected(fmt::format("'{}' object value type should be a number", key));
  }
  return it->get<double>();
}

static expected<uint64_t, std::string> get_required_unsigned(const nlohmann::json& obj, const char* key)
{
  if (!obj.is_object()) {
    return make_unexpected("JSON object value type should be an object");
  }

  auto it = obj.find(key);
  if (it == obj.end()) {
    return make_unexpected(fmt::format("'{}' object is missing and it is mandatory", key));
  }
  if (!it->is_number_unsigned()) {
    return make_unexpected(fmt::format("'{}' object value type should be an unsigned integer", key));
  }
  return it->get<uint64_t>();
}

static expected<geodetic_coordinates_t, std::string> parse_geodetic(const nlohmann::json& obj)
{
  geodetic_coordinates_t out;
  auto                  latitude = get_required_number(obj, "latitude");
  if (!latitude) {
    return make_unexpected(latitude.error());
  }
  auto longitude = get_required_number(obj, "longitude");
  if (!longitude) {
    return make_unexpected(longitude.error());
  }
  auto altitude = get_required_number(obj, "altitude");
  if (!altitude) {
    return make_unexpected(altitude.error());
  }

  out.latitude  = latitude.value();
  out.longitude = longitude.value();
  out.altitude  = altitude.value();
  return out;
}

static expected<ecef_coordinates_t, std::string> parse_ecef_ephemeris(const nlohmann::json& obj)
{
  ecef_coordinates_t out;
  auto               pos_x = get_required_number(obj, "pos_x");
  if (!pos_x) {
    return make_unexpected(pos_x.error());
  }
  auto pos_y = get_required_number(obj, "pos_y");
  if (!pos_y) {
    return make_unexpected(pos_y.error());
  }
  auto pos_z = get_required_number(obj, "pos_z");
  if (!pos_z) {
    return make_unexpected(pos_z.error());
  }
  auto vel_x = get_required_number(obj, "vel_x");
  if (!vel_x) {
    return make_unexpected(vel_x.error());
  }
  auto vel_y = get_required_number(obj, "vel_y");
  if (!vel_y) {
    return make_unexpected(vel_y.error());
  }
  auto vel_z = get_required_number(obj, "vel_z");
  if (!vel_z) {
    return make_unexpected(vel_z.error());
  }

  out.position_x  = pos_x.value();
  out.position_y  = pos_y.value();
  out.position_z  = pos_z.value();
  out.velocity_vx = vel_x.value();
  out.velocity_vy = vel_y.value();
  out.velocity_vz = vel_z.value();
  return out;
}

static expected<orbital_coordinates_t, std::string> parse_orbital_ephemeris(const nlohmann::json& obj)
{
  orbital_coordinates_t out;
  auto                  semi_major_axis = get_required_number(obj, "semi_major_axis");
  if (!semi_major_axis) {
    return make_unexpected(semi_major_axis.error());
  }
  auto eccentricity = get_required_number(obj, "eccentricity");
  if (!eccentricity) {
    return make_unexpected(eccentricity.error());
  }
  auto periapsis = get_required_number(obj, "periapsis");
  if (!periapsis) {
    return make_unexpected(periapsis.error());
  }
  auto longitude = get_required_number(obj, "longitude");
  if (!longitude) {
    return make_unexpected(longitude.error());
  }
  auto mean_anomaly = get_required_number(obj, "mean_anomaly");
  if (!mean_anomaly) {
    return make_unexpected(mean_anomaly.error());
  }
  auto inclination = get_required_number(obj, "inclination");
  if (!inclination) {
    return make_unexpected(inclination.error());
  }

  out.semi_major_axis = semi_major_axis.value();
  out.eccentricity    = eccentricity.value();
  out.periapsis       = periapsis.value();
  out.longitude       = longitude.value();
  out.mean_anomaly    = mean_anomaly.value();
  out.inclination     = inclination.value();
  return out;
}

static expected<ta_info_t, std::string> parse_ta_info(const nlohmann::json& obj)
{
  ta_info_t out;
  auto      ta_common = get_required_number(obj, "ta_common");
  if (!ta_common) {
    return make_unexpected(ta_common.error());
  }
  auto ta_common_drift = get_required_number(obj, "ta_common_drift");
  if (!ta_common_drift) {
    return make_unexpected(ta_common_drift.error());
  }
  auto ta_common_drift_variant = get_required_number(obj, "ta_common_drift_variant");
  if (!ta_common_drift_variant) {
    return make_unexpected(ta_common_drift_variant.error());
  }

  out.ta_common               = ta_common.value();
  out.ta_common_drift         = ta_common_drift.value();
  out.ta_common_drift_variant = ta_common_drift_variant.value();

  auto ta_common_offset = obj.find("ta_common_offset");
  if (ta_common_offset != obj.end() && !ta_common_offset->is_number()) {
    return make_unexpected("'ta_common_offset' object value type should be a number");
  }
  out.ta_common_offset = ta_common_offset == obj.end() ? 0.0 : ta_common_offset->get<double>();
  return out;
}

static expected<srs_ntn::beam_hopping_update_info, std::string> parse_beam_hopping(const nlohmann::json& obj)
{
  if (!obj.is_object()) {
    return make_unexpected("'beam_hopping' object value type should be an object");
  }

  srs_ntn::beam_hopping_update_info out;
  auto                              lat_north = get_required_number(obj, "lat_north");
  if (!lat_north) {
    return make_unexpected(lat_north.error());
  }
  auto lat_south = get_required_number(obj, "lat_south");
  if (!lat_south) {
    return make_unexpected(lat_south.error());
  }
  auto lon_west = get_required_number(obj, "lon_west");
  if (!lon_west) {
    return make_unexpected(lon_west.error());
  }
  auto lon_east = get_required_number(obj, "lon_east");
  if (!lon_east) {
    return make_unexpected(lon_east.error());
  }
  auto beam_radius_m = get_required_number(obj, "beam_radius_m");
  if (!beam_radius_m) {
    return make_unexpected(beam_radius_m.error());
  }
  auto overlap_factor = get_required_number(obj, "overlap_factor");
  if (!overlap_factor) {
    return make_unexpected(overlap_factor.error());
  }
  auto n_active = get_required_unsigned(obj, "n_active");
  if (!n_active) {
    return make_unexpected(n_active.error());
  }
  auto dwell_frames = get_required_unsigned(obj, "dwell_frames");
  if (!dwell_frames) {
    return make_unexpected(dwell_frames.error());
  }

  out.grid.lat_north      = lat_north.value();
  out.grid.lat_south      = lat_south.value();
  out.grid.lon_west       = lon_west.value();
  out.grid.lon_east       = lon_east.value();
  out.grid.beam_radius_m  = beam_radius_m.value();
  out.grid.overlap_factor = overlap_factor.value();

  if (out.grid.lat_north <= out.grid.lat_south) {
    return make_unexpected("'beam_hopping.lat_north' must be greater than 'beam_hopping.lat_south'");
  }
  if (out.grid.lon_east <= out.grid.lon_west) {
    return make_unexpected("'beam_hopping.lon_east' must be greater than 'beam_hopping.lon_west'");
  }
  if (out.grid.beam_radius_m <= 0.0) {
    return make_unexpected("'beam_hopping.beam_radius_m' must be positive");
  }
  if (out.grid.overlap_factor <= 0.0 || out.grid.overlap_factor > 1.0) {
    return make_unexpected("'beam_hopping.overlap_factor' must be in the interval (0, 1]");
  }
  if (n_active.value() == 0 || n_active.value() > srs_ntn::MAX_ACTIVE_BEAM_POSITIONS) {
    return make_unexpected(fmt::format("'beam_hopping.n_active' must be in the range [1, {}]",
                                       srs_ntn::MAX_ACTIVE_BEAM_POSITIONS));
  }
  if (n_active.value() % 3u != 0u) {
    return make_unexpected("'beam_hopping.n_active' must be divisible by 3");
  }
  if (dwell_frames.value() == 0) {
    return make_unexpected("'beam_hopping.dwell_frames' must be at least 1");
  }

  const uint64_t cycle_frames = n_active.value() * dwell_frames.value();
  if (cycle_frames > NOF_SFNS) {
    return make_unexpected("'beam_hopping.n_active * beam_hopping.dwell_frames' must not exceed 1024 SFNs");
  }

  out.n_active     = static_cast<uint16_t>(n_active.value());
  out.dwell_frames = static_cast<uint16_t>(dwell_frames.value());
  return out;
}

static bool is_valid_ntn_ul_sync_validity_duration(unsigned value)
{
  static constexpr std::array<unsigned, 16> valid_values = {5, 10, 15, 20, 25, 30, 35, 40,
                                                            45, 50, 55, 60, 120, 180, 240, 900};
  return std::find(valid_values.begin(), valid_values.end(), value) != valid_values.end();
}

static error_type<std::string> fill_nr_cgi(nr_cell_global_id_t& out, const nlohmann::json& json)
{
  auto plmn_key = json.find("plmn");
  if (plmn_key == json.end()) {
    return make_unexpected("'plmn' object is missing and it is mandatory");
  }
  if (!plmn_key->is_string()) {
    return make_unexpected("'plmn' object value type should be a string");
  }

  auto nci_key = json.find("nci");
  if (nci_key == json.end()) {
    return make_unexpected("'nci' object is missing and it is mandatory");
  }
  if (!nci_key->is_number_unsigned()) {
    return make_unexpected("'nci' object value type should be an integer");
  }

  auto plmn = plmn_identity::parse(plmn_key.value().get_ref<const nlohmann::json::string_t&>());
  if (!plmn) {
    return make_unexpected("Invalid PLMN identity value");
  }
  auto nci = nr_cell_identity::create(nci_key->get<uint64_t>());
  if (!nci) {
    return make_unexpected("Invalid NR cell identity value");
  }
  out.plmn_id = plmn.value();
  out.nci     = nci.value();
  return {};
}

class ntn_config_update_remote_command final : public app_services::remote_command
{
public:
  explicit ntn_config_update_remote_command(srs_ntn::ntn_configuration_manager& ntn_manager_) :
    ntn_manager(ntn_manager_)
  {
  }

  std::string_view get_name() const override { return "ntn_config_update"; }

  std::string_view get_description() const override { return "Updates NTN SIB19 assistance information"; }

  error_type<std::string> execute(const nlohmann::json& json) override
  {
    srs_ntn::ntn_config_update_info req;

    if (auto ret = fill_nr_cgi(req.nr_cgi, json); !ret) {
      return ret;
    }

    auto stop_beam_hopping_key = json.find("stop_beam_hopping");
    if (stop_beam_hopping_key != json.end()) {
      if (!stop_beam_hopping_key->is_boolean()) {
        return make_unexpected("'stop_beam_hopping' object value type should be a boolean");
      }
      req.stop_beam_hopping = stop_beam_hopping_key->get<bool>();
    }

    if (req.stop_beam_hopping) {
      if (!ntn_manager.handle_ntn_config_update(req)) {
        return make_unexpected("NTN beam hopping stop command failed to be applied");
      }
      return {};
    }

    auto validity_key = json.find("ntn_ul_sync_validity_duration");
    if (validity_key == json.end()) {
      return make_unexpected("'ntn_ul_sync_validity_duration' object is missing and it is mandatory");
    }
    if (!validity_key->is_number_unsigned()) {
      return make_unexpected("'ntn_ul_sync_validity_duration' object value type should be an integer");
    }
    req.ntn_ul_sync_validity_duration = validity_key->get<unsigned>();
    if (!is_valid_ntn_ul_sync_validity_duration(req.ntn_ul_sync_validity_duration)) {
      return make_unexpected("'ntn_ul_sync_validity_duration' value is not a valid SIB19 enum");
    }

    auto epoch_time_key = json.find("epoch_time_unix_ms");
    req.epoch_time     = std::chrono::system_clock::now();
    if (epoch_time_key != json.end()) {
      if (!epoch_time_key->is_number_integer() && !epoch_time_key->is_number_unsigned()) {
        return make_unexpected("'epoch_time_unix_ms' object value type should be an integer");
      }
      req.epoch_time = srs_ntn::ntn_config_update_info::time_point{std::chrono::milliseconds{
          epoch_time_key->get<int64_t>()}};
    }

    auto ecef_key    = json.find("ephemeris_info_ecef");
    auto orbital_key = json.find("ephemeris_orbital");
    if (ecef_key == json.end() && orbital_key == json.end()) {
      return make_unexpected("'ephemeris_info_ecef' or 'ephemeris_orbital' object is missing and one is mandatory");
    }
    if (ecef_key != json.end()) {
      auto ecef = parse_ecef_ephemeris(ecef_key.value());
      if (!ecef) {
        return make_unexpected(ecef.error());
      }
      req.ephemeris_info = ecef.value();
    } else {
      auto orbital = parse_orbital_ephemeris(orbital_key.value());
      if (!orbital) {
        return make_unexpected(orbital.error());
      }
      req.ephemeris_info = orbital.value();
    }

    auto ta_info_key = json.find("ta_info");
    if (ta_info_key != json.end()) {
      auto ta_info = parse_ta_info(ta_info_key.value());
      if (!ta_info) {
        return make_unexpected(ta_info.error());
      }
      req.ta_info = ta_info.value();
    }

    auto gateway_key = json.find("ntn_gateway_location");
    if (gateway_key != json.end()) {
      auto gateway = parse_geodetic(gateway_key.value());
      if (!gateway) {
        return make_unexpected(gateway.error());
      }
      req.ntn_gateway_location = gateway.value();
    }

    auto beam_hopping_key = json.find("beam_hopping");
    if (beam_hopping_key != json.end()) {
      auto beam_hopping = parse_beam_hopping(beam_hopping_key.value());
      if (!beam_hopping) {
        return make_unexpected(beam_hopping.error());
      }
      if (!std::holds_alternative<ecef_coordinates_t>(req.ephemeris_info)) {
        return make_unexpected("'beam_hopping' requires 'ephemeris_info_ecef'");
      }
      req.beam_hopping = beam_hopping.value();
    }

    if (!ntn_manager.handle_ntn_config_update(req)) {
      return make_unexpected("NTN config update command failed to be applied");
    }
    return {};
  }

private:
  srs_ntn::ntn_configuration_manager& ntn_manager;
};

} // namespace

void srsran::add_ntn_config_update_remote_command(application_unit_commands&          commands,
                                                  srs_ntn::ntn_configuration_manager& ntn_manager)
{
  commands.remote.push_back(std::make_unique<ntn_config_update_remote_command>(ntn_manager));
}

#endif // SRSRAN_HAS_ENTERPRISE_NTN
