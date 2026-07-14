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

#include "apps/services/cmdline/cmdline_command.h"
#include "apps/services/cmdline/cmdline_command_dispatcher_utils.h"
#include "srsran/adt/expected.h"
#include "srsran/cu_cp/cu_cp_command_handler.h"
#include "srsran/ran/pci.h"
#include "srsran/ran/rnti.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

namespace srsran {

inline const char* ntn_beam_state_to_string(srs_cu_cp::cu_cp_ntn_beam_assignment_state state)
{
  switch (state) {
    case srs_cu_cp::cu_cp_ntn_beam_assignment_state::inactive:
      return "inactive";
    case srs_cu_cp::cu_cp_ntn_beam_assignment_state::candidate:
      return "candidate";
    case srs_cu_cp::cu_cp_ntn_beam_assignment_state::active:
      return "active";
    case srs_cu_cp::cu_cp_ntn_beam_assignment_state::draining:
      return "draining";
  }
  return "unknown";
}

inline bool ntn_beam_status_matches_filter(const srs_cu_cp::cu_cp_ntn_beam_status& status, std::string_view filter)
{
  if (filter == "all") {
    return true;
  }
  if (filter == "window") {
    return status.in_hopping_window;
  }
  if (filter == "scheduled") {
    return status.sr_slot_period != 0 || status.srs_slot_period != 0;
  }
  if (filter == "active") {
    return status.state == srs_cu_cp::cu_cp_ntn_beam_assignment_state::active;
  }
  if (filter == "candidate") {
    return status.state == srs_cu_cp::cu_cp_ntn_beam_assignment_state::candidate;
  }
  if (filter == "draining") {
    return status.state == srs_cu_cp::cu_cp_ntn_beam_assignment_state::draining;
  }
  if (filter == "inactive") {
    return status.state == srs_cu_cp::cu_cp_ntn_beam_assignment_state::inactive;
  }
  return false;
}

inline bool is_valid_ntn_beam_filter(std::string_view filter)
{
  return filter == "summary" || filter == "all" || filter == "window" || filter == "scheduled" || filter == "active" ||
         filter == "candidate" || filter == "draining" || filter == "inactive";
}

inline std::string format_ntn_antenna_slot(const srs_cu_cp::cu_cp_ntn_beam_status& status)
{
  if (status.nof_antenna_slots == 0) {
    return "-";
  }
  if (status.nof_antenna_slots == 1) {
    return fmt::format("{}/{}", status.antenna_slot_index, status.antenna_slot_period);
  }
  return fmt::format("{}-{}/{}",
                     status.antenna_slot_index,
                     status.antenna_slot_index + status.nof_antenna_slots - 1,
                     status.antenna_slot_period);
}

inline std::string format_ntn_periodic_slot(unsigned offset, unsigned period)
{
  return period == 0 ? "-" : fmt::format("{}/{}", offset, period);
}

inline ecef_coordinates_t make_ecef_from_geodetic(double latitude_deg, double longitude_deg, double altitude_m)
{
  constexpr double wgs84_a_m = 6378137.0;
  constexpr double wgs84_f   = 1.0 / 298.257223563;
  constexpr double wgs84_e2  = 2.0 * wgs84_f - wgs84_f * wgs84_f;
  constexpr double pi        = 3.14159265358979323846;

  const double latitude_rad  = latitude_deg * pi / 180.0;
  const double longitude_rad = longitude_deg * pi / 180.0;
  const double sin_latitude  = std::sin(latitude_rad);
  const double cos_latitude  = std::cos(latitude_rad);
  const double prime_vertical_radius_m =
      wgs84_a_m / std::sqrt(1.0 - wgs84_e2 * sin_latitude * sin_latitude);

  ecef_coordinates_t ecef;
  ecef.position_x = (prime_vertical_radius_m + altitude_m) * cos_latitude * std::cos(longitude_rad);
  ecef.position_y = (prime_vertical_radius_m + altitude_m) * cos_latitude * std::sin(longitude_rad);
  ecef.position_z =
      (prime_vertical_radius_m * (1.0 - wgs84_e2) + altitude_m) * sin_latitude;
  return ecef;
}

inline bool inject_ntn_satellite_state(srs_cu_cp::cu_cp_command_handler& cu_cp, const ecef_coordinates_t& satellite)
{
  const bool accepted = cu_cp.get_ntn_command_handler().handle_ntn_satellite_state_update(satellite);
  fmt::print("NTN satellite state {}. ecef=({:.3f},{:.3f},{:.3f})\n",
             accepted ? "accepted" : "rejected",
             satellite.position_x,
             satellite.position_y,
             satellite.position_z);
  return accepted;
}

/// Application command to trigger a handover.
class handover_app_command : public app_services::cmdline_command
{
  srs_cu_cp::cu_cp_command_handler& cu_cp;

public:
  explicit handover_app_command(srs_cu_cp::cu_cp_command_handler& cu_cp_) : cu_cp(cu_cp_) {}

  // See interface for documentation.
  std::string_view get_name() const override { return "ho"; }

  // See interface for documentation.
  std::string_view get_description() const override { return " <serving pci> <rnti> <target pci>: force UE handover"; }

  // See interface for documentation.
  void execute(span<const std::string> args) override
  {
    if (args.size() != 3) {
      fmt::print("Invalid handover command structure. Usage: ho <serving pci> <rnti> <target pci>\n");
      return;
    }

    const auto*                     arg         = args.begin();
    expected<unsigned, std::string> serving_pci = app_services::parse_int<unsigned>(*arg);
    if (not serving_pci.has_value()) {
      fmt::print("Invalid serving PCI.\n");
      return;
    }
    ++arg;
    expected<unsigned, std::string> rnti = app_services::parse_unsigned_hex<unsigned>(*arg);
    if (not rnti.has_value()) {
      fmt::print("Invalid UE RNTI.\n");
      return;
    }
    ++arg;
    expected<unsigned, std::string> target_pci = app_services::parse_int<unsigned>(*arg);
    if (not target_pci.has_value()) {
      fmt::print("Invalid target PCI.\n");
      return;
    }

    cu_cp.get_mobility_command_handler().trigger_handover(static_cast<pci_t>(serving_pci.value()),
                                                          static_cast<rnti_t>(rnti.value()),
                                                          static_cast<pci_t>(target_pci.value()));
    fmt::print("Handover triggered for UE with pci={} rnti={} to pci={}.\n",
               serving_pci.value(),
               static_cast<rnti_t>(rnti.value()),
               target_pci.value());
  }
};

/// Application command to inspect the current NTN beam placement plan.
class ntn_beams_app_command : public app_services::cmdline_command
{
  srs_cu_cp::cu_cp_command_handler& cu_cp;
  static constexpr unsigned         max_row_limit = 1024;

public:
  explicit ntn_beams_app_command(srs_cu_cp::cu_cp_command_handler& cu_cp_) : cu_cp(cu_cp_) {}

  // See interface for documentation.
  std::string_view get_name() const override { return "ntn_beams"; }

  // See interface for documentation.
  std::string_view get_description() const override
  {
    return " [summary|all|window|scheduled|active|candidate|draining|inactive] [limit]: show NTN beam placement state";
  }

  // See interface for documentation.
  void execute(span<const std::string> args) override
  {
    if (args.size() > 2) {
      fmt::print("Invalid NTN beam command structure. Usage: ntn_beams "
                 "[summary|all|window|scheduled|active|candidate|draining|inactive] [limit]\n");
      return;
    }

    const std::string_view filter = args.empty() ? std::string_view{"summary"} : std::string_view{args[0]};
    if (!is_valid_ntn_beam_filter(filter)) {
      fmt::print("Invalid NTN beam filter. Usage: ntn_beams "
                 "[summary|all|window|scheduled|active|candidate|draining|inactive] [limit]\n");
      return;
    }

    unsigned row_limit = 128;
    if (args.size() == 2) {
      expected<unsigned, std::string> parsed_limit = app_services::parse_int<unsigned>(args[1]);
      if (!parsed_limit.has_value()) {
        fmt::print("Invalid NTN beam row limit.\n");
        return;
      }
      row_limit = parsed_limit.value();
    }
    if (row_limit > max_row_limit) {
      fmt::print("NTN beam row limit capped to {} rows.\n", max_row_limit);
      row_limit = max_row_limit;
    }

    const std::vector<srs_cu_cp::cu_cp_ntn_beam_status> beam_status =
        cu_cp.get_ntn_command_handler().get_current_ntn_beam_status();
    if (beam_status.empty()) {
      fmt::print("No NTN beam placement plan is available.\n");
      return;
    }

    unsigned nof_active          = 0;
    unsigned nof_candidate       = 0;
    unsigned nof_draining        = 0;
    unsigned nof_inactive        = 0;
    unsigned nof_hopping_window  = 0;
    unsigned nof_scheduled       = 0;
    unsigned nof_sr_scheduled    = 0;
    unsigned nof_srs_scheduled   = 0;
    unsigned nof_filtered_status = 0;
    for (const auto& status : beam_status) {
      switch (status.state) {
        case srs_cu_cp::cu_cp_ntn_beam_assignment_state::active:
          ++nof_active;
          break;
        case srs_cu_cp::cu_cp_ntn_beam_assignment_state::candidate:
          ++nof_candidate;
          break;
        case srs_cu_cp::cu_cp_ntn_beam_assignment_state::draining:
          ++nof_draining;
          break;
        case srs_cu_cp::cu_cp_ntn_beam_assignment_state::inactive:
          ++nof_inactive;
          break;
      }
      if (status.in_hopping_window) {
        ++nof_hopping_window;
      }
      if (status.sr_slot_period != 0 || status.srs_slot_period != 0) {
        ++nof_scheduled;
      }
      if (status.sr_slot_period != 0) {
        ++nof_sr_scheduled;
      }
      if (status.srs_slot_period != 0) {
        ++nof_srs_scheduled;
      }
      if (filter != "summary" && ntn_beam_status_matches_filter(status, filter)) {
        ++nof_filtered_status;
      }
    }

    fmt::print("NTN beams: total={} active={} candidate={} draining={} inactive={} hopping_window={} scheduled={} sr={} srs={}\n",
               beam_status.size(),
               nof_active,
               nof_candidate,
               nof_draining,
               nof_inactive,
               nof_hopping_window,
               nof_scheduled,
               nof_sr_scheduled,
               nof_srs_scheduled);

    if (filter == "summary") {
      return;
    }

    fmt::print("{:<18} {:<9} {:<6} {:<6} {:<12} {:>9} {:>5} {:>5} {:>8} {:>8} {:>8} {:>5}\n",
               "beam",
               "state",
               "window",
               "du",
               "nci",
               "elev_deg",
               "ues",
               "drbs",
               "slot",
               "sr",
               "srs",
               "nslot");
    unsigned nof_printed = 0;
    for (const auto& status : beam_status) {
      if (!ntn_beam_status_matches_filter(status, filter)) {
        continue;
      }
      if (nof_printed >= row_limit) {
        break;
      }
      const std::string du_index = status.du_index == srs_cu_cp::du_index_t::invalid
                                       ? "-"
                                       : std::to_string(du_index_to_uint(status.du_index));
      const std::string antenna_slot = format_ntn_antenna_slot(status);
      const std::string sr_slot      = format_ntn_periodic_slot(status.sr_slot_offset, status.sr_slot_period);
      const std::string srs_slot     = format_ntn_periodic_slot(status.srs_slot_offset, status.srs_slot_period);
      fmt::print("{:<18} {:<9} {:<6} {:<6} {:#012x} {:>9.2f} {:>5} {:>5} {:>8} {:>8} {:>8} {:>5}\n",
                 status.beam_id,
                 ntn_beam_state_to_string(status.state),
                 status.in_hopping_window ? "yes" : "no",
                 du_index,
                 status.nci.value(),
                 status.elevation_deg,
                 status.nof_ues,
                 status.nof_drbs,
                 antenna_slot,
                 sr_slot,
                 srs_slot,
                 status.nof_antenna_slots);
      ++nof_printed;
    }
    if (nof_printed < nof_filtered_status) {
      fmt::print("Showing {} of {} matching beams. Increase [limit] to inspect more rows.\n",
                 nof_printed,
                 nof_filtered_status);
    }
  }
};

/// Application command to inject an NTN satellite ECEF state into CU-CP mobility.
class ntn_satellite_state_app_command : public app_services::cmdline_command
{
  srs_cu_cp::cu_cp_command_handler& cu_cp;

public:
  explicit ntn_satellite_state_app_command(srs_cu_cp::cu_cp_command_handler& cu_cp_) : cu_cp(cu_cp_) {}

  // See interface for documentation.
  std::string_view get_name() const override { return "ntn_sat"; }

  // See interface for documentation.
  std::string_view get_description() const override
  {
    return " <ecef_x_m> <ecef_y_m> <ecef_z_m>: inject NTN satellite state";
  }

  // See interface for documentation.
  void execute(span<const std::string> args) override
  {
    if (args.size() != 3) {
      fmt::print("Invalid NTN satellite command structure. Usage: ntn_sat <ecef_x_m> <ecef_y_m> <ecef_z_m>\n");
      return;
    }

    expected<double, std::string> x = app_services::parse_double(args[0]);
    expected<double, std::string> y = app_services::parse_double(args[1]);
    expected<double, std::string> z = app_services::parse_double(args[2]);
    if (!x.has_value() || !y.has_value() || !z.has_value()) {
      fmt::print("Invalid NTN satellite ECEF state.\n");
      return;
    }
    if (!std::isfinite(x.value()) || !std::isfinite(y.value()) || !std::isfinite(z.value())) {
      fmt::print("Invalid NTN satellite ECEF state. Coordinates must be finite.\n");
      return;
    }

    ecef_coordinates_t satellite;
    satellite.position_x = x.value();
    satellite.position_y = y.value();
    satellite.position_z = z.value();

    inject_ntn_satellite_state(cu_cp, satellite);
  }
};

/// Application command to inject an NTN satellite geodetic state into CU-CP mobility.
class ntn_satellite_geo_state_app_command : public app_services::cmdline_command
{
  srs_cu_cp::cu_cp_command_handler& cu_cp;

public:
  explicit ntn_satellite_geo_state_app_command(srs_cu_cp::cu_cp_command_handler& cu_cp_) : cu_cp(cu_cp_) {}

  // See interface for documentation.
  std::string_view get_name() const override { return "ntn_sat_geo"; }

  // See interface for documentation.
  std::string_view get_description() const override
  {
    return " <lat_deg> <lon_deg> [alt_m]: inject NTN satellite state from geodetic coordinates";
  }

  // See interface for documentation.
  void execute(span<const std::string> args) override
  {
    if (args.size() != 2 && args.size() != 3) {
      fmt::print("Invalid NTN satellite geo command structure. Usage: ntn_sat_geo <lat_deg> <lon_deg> [alt_m]\n");
      return;
    }

    expected<double, std::string> latitude = app_services::parse_double(args[0]);
    expected<double, std::string> longitude = app_services::parse_double(args[1]);
    double altitude_m = 500000.0;
    if (args.size() == 3) {
      expected<double, std::string> altitude = app_services::parse_double(args[2]);
      if (!altitude.has_value()) {
        fmt::print("Invalid NTN satellite altitude.\n");
        return;
      }
      altitude_m = altitude.value();
    }
    if (!latitude.has_value() || !longitude.has_value()) {
      fmt::print("Invalid NTN satellite geodetic state.\n");
      return;
    }
    if (!std::isfinite(latitude.value()) || !std::isfinite(longitude.value()) || !std::isfinite(altitude_m) ||
        latitude.value() < -90.0 || latitude.value() > 90.0 || longitude.value() < -180.0 ||
        longitude.value() > 180.0 || altitude_m <= 0.0) {
      fmt::print("Invalid NTN satellite geodetic state. Use lat in [-90,90], lon in [-180,180], alt_m > 0.\n");
      return;
    }

    const ecef_coordinates_t satellite =
        make_ecef_from_geodetic(latitude.value(), longitude.value(), altitude_m);
    inject_ntn_satellite_state(cu_cp, satellite);
  }
};

} // namespace srsran
