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

#include "cu_cp_unit_config_validator.h"
#include "srsran/adt/span.h"
#include "srsran/cu_cp/cell_meas_manager_config.h"
#include "srsran/f1ap/ntn_access_calendar.h"
#include "srsran/pdcp/pdcp_t_reordering.h"
#include "srsran/ran/nr_cgi.h"
#include "srsran/rlc/rlc_config.h"
#include "srsran/scheduler/ntn_access_calendar.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <sstream>

using namespace srsran;

static std::string normalize_sha256_digest(std::string value)
{
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::tolower(c); });
  if (value.rfind("sha256:", 0) != 0) {
    value.insert(0, "sha256:");
  }
  return value;
}

static bool is_sha256_digest(const std::string& value)
{
  const std::string normalized = normalize_sha256_digest(value);
  return normalized.size() == 71 &&
         std::all_of(normalized.begin() + 7, normalized.end(), [](unsigned char c) { return std::isxdigit(c); });
}

static bool is_canonical_ntn_satellite_id(const std::string& value)
{
  return value.size() == 7 && value[0] == 'P' && std::isdigit(static_cast<unsigned char>(value[1])) &&
         std::isdigit(static_cast<unsigned char>(value[2])) && value[3] == '-' && value[4] == 'S' &&
         std::isdigit(static_cast<unsigned char>(value[5])) && std::isdigit(static_cast<unsigned char>(value[6]));
}

static bool validate_mobility_appconfig(gnb_id_t gnb_id, const cu_cp_unit_mobility_config& config)
{
  const auto& ntn_cfg = config.ntn_location_mobility;
  const auto& position_plan_cfg = config.ntn_onboard_position_plan;
  if (position_plan_cfg.du_execution_enabled && ntn_cfg.enabled) {
    fmt::print("Invalid CU-CP configuration. Executing onboard position plans and legacy NTN location mobility "
               "cannot both be identity-authoritative\n");
    return false;
  }
  if (ntn_cfg.enabled) {
    if (ntn_cfg.beam_table_json_file.empty()) {
      fmt::print("Invalid CU-CP configuration. NTN location mobility requires beam_table_json_file\n");
      return false;
    }
    if (ntn_cfg.required_consecutive_location_reports == 0) {
      fmt::print("Invalid CU-CP configuration. NTN location mobility requires at least one consecutive report\n");
      return false;
    }
    if (!std::isfinite(ntn_cfg.served_beam_min_elevation_deg) || ntn_cfg.served_beam_min_elevation_deg < -90.0 ||
        ntn_cfg.served_beam_min_elevation_deg > 90.0) {
      fmt::print("Invalid CU-CP configuration. NTN served_beam_min_elevation_deg must be within [-90, 90]\n");
      return false;
    }
    if (ntn_cfg.served_beam_hopping_dwell_updates == 0) {
      fmt::print("Invalid CU-CP configuration. NTN served_beam_hopping_dwell_updates must be greater than zero\n");
      return false;
    }
    if (ntn_cfg.multi_beam_load_balancing_enabled) {
      if (ntn_cfg.multi_beam_load_balancing_min_ue_delta == 0) {
        fmt::print("Invalid CU-CP configuration. NTN multi_beam_load_balancing_min_ue_delta must be greater than zero\n");
        return false;
      }
      if (ntn_cfg.multi_beam_load_balancing_max_handovers_per_eval == 0) {
        fmt::print(
            "Invalid CU-CP configuration. NTN multi_beam_load_balancing_max_handovers_per_eval must be greater than zero\n");
        return false;
      }
    }
    if (ntn_cfg.satellite_state_source != "manual" && ntn_cfg.satellite_state_source != "circular_orbit" &&
        ntn_cfg.satellite_state_source != "tle") {
      fmt::print("Invalid CU-CP configuration. NTN satellite_state_source '{}' is invalid. Valid values are: manual, "
                 "circular_orbit, tle\n",
                 ntn_cfg.satellite_state_source);
      return false;
    }
    if (ntn_cfg.satellite_state_source != "circular_orbit" && !ntn_cfg.circular_orbit_satellites.empty()) {
      fmt::print("Invalid CU-CP configuration. NTN circular_orbit_satellites requires satellite_state_source=circular_orbit\n");
      return false;
    }
    if (ntn_cfg.predictive_service_window_horizon_ms > 0) {
      if (ntn_cfg.satellite_state_source == "manual" || ntn_cfg.satellite_state_update_period_ms == 0) {
        fmt::print("Invalid CU-CP configuration. NTN predictive_service_window_horizon_ms requires orbit-driven satellite updates\n");
        return false;
      }
      if (ntn_cfg.predictive_handover_lead_time_ms > ntn_cfg.predictive_service_window_horizon_ms) {
        fmt::print("Invalid CU-CP configuration. NTN predictive_handover_lead_time_ms must not exceed predictive_service_window_horizon_ms\n");
        return false;
      }
      const unsigned nof_prediction_steps =
          (ntn_cfg.predictive_service_window_horizon_ms + ntn_cfg.satellite_state_update_period_ms - 1) /
          ntn_cfg.satellite_state_update_period_ms;
      if (nof_prediction_steps > 64) {
        fmt::print("Invalid CU-CP configuration. NTN predictive service window must not exceed 64 steps\n");
        return false;
      }
    }
    if (ntn_cfg.satellite_state_source != "manual") {
      if (ntn_cfg.satellite_state_update_period_ms == 0) {
        fmt::print("Invalid CU-CP configuration. NTN satellite_state_update_period_ms must be greater than zero\n");
        return false;
      }
      if (ntn_cfg.satellite_state_source == "circular_orbit") {
        if (ntn_cfg.circular_orbit_satellites.empty()) {
          if (!std::isfinite(ntn_cfg.circular_orbit_altitude_m) || ntn_cfg.circular_orbit_altitude_m <= 0.0 ||
              !std::isfinite(ntn_cfg.circular_orbit_inclination_deg) ||
              !std::isfinite(ntn_cfg.circular_orbit_raan_deg) ||
              !std::isfinite(ntn_cfg.circular_orbit_argument_of_latitude_deg)) {
            fmt::print("Invalid CU-CP configuration. NTN circular orbit configuration is invalid\n");
            return false;
          }
        } else {
          std::set<std::string> satellite_ids;
          for (const cu_cp_unit_ntn_circular_orbit_satellite_config& satellite :
               ntn_cfg.circular_orbit_satellites) {
            if (satellite.satellite_id.empty() || !satellite_ids.insert(satellite.satellite_id).second ||
                !std::isfinite(satellite.altitude_m) || satellite.altitude_m <= 0.0 ||
                !std::isfinite(satellite.inclination_deg) || !std::isfinite(satellite.raan_deg) ||
                !std::isfinite(satellite.argument_of_latitude_deg)) {
              fmt::print("Invalid CU-CP configuration. NTN circular orbit satellite list is invalid\n");
              return false;
            }
          }
        }
      }
      if (ntn_cfg.satellite_state_source == "tle" && (ntn_cfg.tle_line1.empty() || ntn_cfg.tle_line2.empty())) {
        fmt::print("Invalid CU-CP configuration. NTN TLE source requires tle_line1 and tle_line2\n");
        return false;
      }
    }
    if (ntn_cfg.boundary_hysteresis_m < 0.0) {
      fmt::print("Invalid CU-CP configuration. NTN boundary_hysteresis_m must not be negative\n");
      return false;
    }
    if (ntn_cfg.max_horizontal_accuracy_m.has_value() && ntn_cfg.max_horizontal_accuracy_m.value() < 0.0) {
      fmt::print("Invalid CU-CP configuration. NTN max_horizontal_accuracy_m must not be negative\n");
      return false;
    }

    auto beam_table = srs_cu_cp::load_ntn_beam_table_json_file(ntn_cfg.beam_table_json_file);
    if (!beam_table.has_value()) {
      fmt::print("Invalid CU-CP configuration. NTN beam table file '{}' is invalid. Cause: {}\n",
                 ntn_cfg.beam_table_json_file,
                 beam_table.error());
      return false;
    }
    std::set<uint64_t> configured_cell_ncis;
    for (const auto& cell : config.cells) {
      configured_cell_ncis.insert(cell.nr_cell_id);
    }
    for (const auto& beam : beam_table.value().beams) {
      if (configured_cell_ncis.count(beam.nci.value()) == 0) {
        fmt::print("Invalid CU-CP configuration. NTN beam '{}' nci={:#x} has no mobility cell config\n",
                   beam.beam_id,
                   beam.nci);
        return false;
      }
    }
  }

  if (position_plan_cfg.du_execution_enabled && !position_plan_cfg.enabled) {
    fmt::print("Invalid CU-CP configuration. NTN DU calendar execution requires the onboard position plan\n");
    return false;
  }
  if (position_plan_cfg.enabled) {
    if (!is_canonical_ntn_satellite_id(position_plan_cfg.satellite_id) || position_plan_cfg.plan_json_file.empty()) {
      fmt::print("Invalid CU-CP configuration. NTN onboard position plan requires canonical Pxx-Syy satellite_id and "
                 "plan_json_file\n");
      return false;
    }
    if (position_plan_cfg.cell_ncis.size() != 2 || position_plan_cfg.cell_pcis.size() != 2) {
      fmt::print("Invalid CU-CP configuration. NTN onboard position plan requires exactly two cell_ncis and cell_pcis\n");
      return false;
    }
    const bool has_any_planning_context      = !position_plan_cfg.expected_catalog_id.empty() ||
                                               !position_plan_cfg.expected_catalog_hash.empty() ||
                                               !position_plan_cfg.expected_identity_registry_version.empty() ||
                                               !position_plan_cfg.expected_identity_registry_hash.empty() ||
                                               !position_plan_cfg.expected_access_profile_id.empty() ||
                                               !position_plan_cfg.expected_access_profile_hash.empty();
    const bool has_complete_planning_context = !position_plan_cfg.expected_catalog_id.empty() &&
                                               is_sha256_digest(position_plan_cfg.expected_catalog_hash) &&
                                               !position_plan_cfg.expected_identity_registry_version.empty() &&
                                               is_sha256_digest(position_plan_cfg.expected_identity_registry_hash) &&
                                               !position_plan_cfg.expected_access_profile_id.empty() &&
                                               is_sha256_digest(position_plan_cfg.expected_access_profile_hash);
    if ((position_plan_cfg.du_execution_enabled || has_any_planning_context) && !has_complete_planning_context) {
      fmt::print("Invalid CU-CP configuration. NTN schema-v2 planning context requires complete catalog, identity "
                 "registry and access-profile identifiers with SHA-256 digests\n");
      return false;
    }
    if (position_plan_cfg.du_execution_enabled &&
        (position_plan_cfg.expected_access_profile_id != "ntn-access-16a-64d-v1" ||
         normalize_sha256_digest(position_plan_cfg.expected_access_profile_hash) !=
             "sha256:195786f4161e3b0fad6faa0605144948a7401c067a014bde684c1b29a8087d63" ||
         position_plan_cfg.max_l1_positions_per_cell != 128 ||
         position_plan_cfg.max_l1_positions_per_satellite != 256 || position_plan_cfg.max_analog_ports_per_cell != 16 ||
         position_plan_cfg.max_analog_ports_per_satellite != 32 || position_plan_cfg.max_digital_ports_per_cell != 64 ||
         position_plan_cfg.max_digital_ports_per_satellite != 128 || position_plan_cfg.access_slot_us != 10000 ||
         position_plan_cfg.subvisit_duration_us != 2500 || position_plan_cfg.max_ssb_interval_ms != 80 ||
         position_plan_cfg.max_prach_interval_ms != 640 || position_plan_cfg.activation_alignment_ms != 640)) {
      fmt::print("Invalid CU-CP configuration. NTN DU execution requires the bound ntn-access-16a-64d-v1 "
                 "resource profile\n");
      return false;
    }
    for (uint64_t nci : position_plan_cfg.cell_ncis) {
      if (!nr_cell_identity::create(nci).has_value()) {
        fmt::print("Invalid CU-CP configuration. NTN onboard position plan NCI {:#x} exceeds 36 bits\n", nci);
        return false;
      }
    }
    if (position_plan_cfg.cell_ncis[0] == position_plan_cfg.cell_ncis[1]) {
      fmt::print("Invalid CU-CP configuration. NTN onboard position plan cell NCIs must be distinct\n");
      return false;
    }
    for (unsigned pci : position_plan_cfg.cell_pcis) {
      if (pci > MAX_PCI) {
        fmt::print("Invalid CU-CP configuration. NTN onboard position plan PCI {} exceeds {}\n", pci, MAX_PCI);
        return false;
      }
    }

    if (position_plan_cfg.max_l1_positions_per_cell == 0 || position_plan_cfg.max_l1_positions_per_satellite == 0 ||
        position_plan_cfg.max_analog_ports_per_cell == 0 ||
        position_plan_cfg.max_analog_ports_per_cell >= std::numeric_limits<uint16_t>::max() ||
        position_plan_cfg.max_analog_ports_per_satellite == 0 || position_plan_cfg.max_digital_ports_per_cell == 0 ||
        position_plan_cfg.max_digital_ports_per_satellite == 0 || position_plan_cfg.access_slot_us == 0 ||
        position_plan_cfg.subvisit_duration_us == 0 || position_plan_cfg.max_ssb_interval_ms == 0 ||
        position_plan_cfg.max_prach_interval_ms == 0 || position_plan_cfg.activation_alignment_ms == 0) {
      fmt::print("Invalid CU-CP configuration. NTN onboard position-plan capacities and timing values must be positive\n");
      return false;
    }
    if (position_plan_cfg.du_execution_enabled) {
      if (position_plan_cfg.du_prepare_guard_ms == 0 || position_plan_cfg.du_apply_timeout_ms == 0 ||
          position_plan_cfg.du_prepare_horizon_ms <= position_plan_cfg.du_prepare_guard_ms ||
          position_plan_cfg.du_prepare_horizon_ms > 5000) {
        fmt::print("Invalid CU-CP configuration. NTN DU execution requires a positive apply timeout and a prepare "
                   "horizon in (guard, 5000] ms\n");
        return false;
      }
    }
    if (static_cast<uint64_t>(position_plan_cfg.max_l1_positions_per_satellite) >
        2ULL * position_plan_cfg.max_l1_positions_per_cell) {
      fmt::print("Invalid CU-CP configuration. NTN satellite L1 capacity exceeds its two-cell capacity\n");
      return false;
    }
    if (static_cast<uint64_t>(position_plan_cfg.max_analog_ports_per_satellite) >
        2ULL * position_plan_cfg.max_analog_ports_per_cell) {
      fmt::print("Invalid CU-CP configuration. NTN satellite analog-port capacity exceeds its two-cell capacity\n");
      return false;
    }
    if (static_cast<uint64_t>(position_plan_cfg.max_digital_ports_per_satellite) >
        2ULL * position_plan_cfg.max_digital_ports_per_cell) {
      fmt::print("Invalid CU-CP configuration. NTN satellite digital-port capacity exceeds its two-cell capacity\n");
      return false;
    }

    const uint64_t max_ssb_interval_us   = 1000ULL * position_plan_cfg.max_ssb_interval_ms;
    const uint64_t max_prach_interval_us = 1000ULL * position_plan_cfg.max_prach_interval_ms;
    if (max_ssb_interval_us % (2ULL * position_plan_cfg.access_slot_us) != 0 ||
        max_prach_interval_us % max_ssb_interval_us != 0 ||
        4ULL * position_plan_cfg.subvisit_duration_us != position_plan_cfg.access_slot_us) {
      fmt::print("Invalid CU-CP configuration. NTN access-calendar timing values are not exactly schedulable\n");
      return false;
    }
    const uint64_t     cell_occasions_per_ssb_period = max_ssb_interval_us / (2ULL * position_plan_cfg.access_slot_us);
    constexpr uint64_t common_downlink_ports         = 10;
    constexpr uint64_t subvisits_per_slot            = 4;
    if (static_cast<uint64_t>(position_plan_cfg.max_l1_positions_per_cell) >
        common_downlink_ports * subvisits_per_slot * cell_occasions_per_ssb_period) {
      fmt::print("Invalid CU-CP configuration. NTN per-cell L1 capacity cannot meet the configured SSB interval\n");
      return false;
    }
    if (position_plan_cfg.du_execution_enabled) {
      // Reject unsupported execution profiles at startup instead of accepting a plan that cannot fit the private F1
      // envelope or the scheduler's fixed-size real-time gate. The inventory path remains independent and can still
      // retain an oversized management-center candidate when DU execution is disabled.
      const uint64_t nof_intents_per_position = max_prach_interval_us / max_ssb_interval_us + 2ULL;
      const uint64_t max_total_intents =
          static_cast<uint64_t>(position_plan_cfg.max_l1_positions_per_satellite) * nof_intents_per_position;
      if (position_plan_cfg.max_l1_positions_per_satellite >
              f1ap_ntn_access_calendar_detail::max_unique_positions ||
          max_total_intents > f1ap_ntn_access_calendar_detail::max_total_intents) {
        fmt::print("Invalid CU-CP configuration. NTN DU execution exceeds the F1 access-calendar envelope of {} "
                   "positions and {} intents\n",
                   f1ap_ntn_access_calendar_detail::max_unique_positions,
                   f1ap_ntn_access_calendar_detail::max_total_intents);
        return false;
      }

      // NR numerology mu=4 has the largest slot rate (16 slots/ms). A cycle accepted here must therefore fit the
      // scheduler gate for every supported numerology; lower numerologies consume fewer slots for the same duration.
      constexpr uint64_t max_nr_slots_per_ms = 16;
      const uint64_t     max_cycle_slots = position_plan_cfg.max_prach_interval_ms * max_nr_slots_per_ms;
      if (max_cycle_slots > MAX_NTN_ACCESS_CALENDAR_CYCLE_SLOTS) {
        fmt::print("Invalid CU-CP configuration. NTN DU execution cycle can require {} slots, exceeding the scheduler "
                   "access-calendar limit of {}\n",
                   max_cycle_slots,
                   MAX_NTN_ACCESS_CALENDAR_CYCLE_SLOTS);
        return false;
      }
    }
  }

  std::map<unsigned, std::string> report_cfg_ids_to_report_type;
  for (const auto& report_cfg : config.report_configs) {
    // Check that report config ids are unique.
    if (report_cfg_ids_to_report_type.find(report_cfg.report_cfg_id) != report_cfg_ids_to_report_type.end()) {
      fmt::print("Report config ids must be unique\n");
      return false;
    }
    report_cfg_ids_to_report_type.emplace(report_cfg.report_cfg_id, report_cfg.report_type);

    // Check that report configs are valid.
    if (report_cfg.report_type == "event_triggered") {
      if (!report_cfg.event_triggered_report_type.has_value()) {
        fmt::print("Invalid CU-CP configuration. If report type is set to \"event_triggered\" then "
                   "\"event_triggered_report_type\" must be set\n");
        return false;
      }

      if (report_cfg.event_triggered_report_type.value() == "a1" or
          report_cfg.event_triggered_report_type.value() == "a2" or
          report_cfg.event_triggered_report_type.value() == "a4") {
        if (!report_cfg.meas_trigger_quantity.has_value() or
            !report_cfg.meas_trigger_quantity_threshold_db.has_value() or !report_cfg.hysteresis_db.has_value() or
            !report_cfg.time_to_trigger_ms.has_value()) {
          fmt::print("Invalid event A1/A2/A4 measurement report configuration.\n");
          return false;
        }
      }
      if (report_cfg.event_triggered_report_type.value() == "a3" or
          report_cfg.event_triggered_report_type.value() == "a6") {
        if (!report_cfg.meas_trigger_quantity.has_value() or !report_cfg.meas_trigger_quantity_offset_db.has_value() or
            !report_cfg.hysteresis_db.has_value() or !report_cfg.time_to_trigger_ms.has_value()) {
          fmt::print("Invalid event A3/A6 measurement report configuration.\n");
          return false;
        }
      }
      if (report_cfg.event_triggered_report_type.value() == "a5") {
        if (!report_cfg.meas_trigger_quantity.has_value() or
            !report_cfg.meas_trigger_quantity_threshold_db.has_value() or
            !report_cfg.meas_trigger_quantity_threshold_2_db.has_value() or !report_cfg.hysteresis_db.has_value() or
            !report_cfg.time_to_trigger_ms.has_value()) {
          fmt::print("Invalid event A5 measurement report configuration.\n");
          return false;
        }
      }
    }
  }

  std::map<nr_cell_identity, std::set<unsigned>> cell_to_report_cfg_id;

  // Check cu_cp_cell_config.
  std::set<nr_cell_identity> ncis;
  for (const auto& cell : config.cells) {
    nr_cell_identity nci = nr_cell_identity::create(cell.nr_cell_id).value();
    if (!ncis.emplace(nci).second) {
      fmt::print("Cells must be unique ({:#x} already present)\n", cell.nr_cell_id);
      return false;
    }

    if (cell.ssb_period.has_value() && cell.ssb_offset.has_value() &&
        cell.ssb_offset.value() >= cell.ssb_period.value()) {
      fmt::print("ssb_offset must be smaller than ssb_period\n");
      return false;
    }

    if (cell.periodic_report_cfg_id.has_value()) {
      // Try to add report config id to cell_to_report_cfg_id map.
      cell_to_report_cfg_id.emplace(nci, std::set<unsigned>());
      auto& report_cfg_ids = cell_to_report_cfg_id.at(nci);
      if (!report_cfg_ids.emplace(cell.periodic_report_cfg_id.value()).second) {
        fmt::print("cell={:#x}: report_config_id={} already configured for this cell)\n",
                   cell.nr_cell_id,
                   cell.periodic_report_cfg_id.value());
        return false;
      }
      // Check that for the serving cell only periodic reports are configured.
      if (report_cfg_ids_to_report_type.at(cell.periodic_report_cfg_id.value()) != "periodical") {
        fmt::print("For the serving cell only periodic reports are allowed\n");
        return false;
      }
    }

    // Check if cell is an external managed cell.
    if (nci.gnb_id(gnb_id.bit_length) != gnb_id) {
      if (!cell.gnb_id_bit_length.has_value() || !cell.pci.has_value() || !cell.band.has_value() ||
          !cell.ssb_arfcn.has_value() || !cell.ssb_scs.has_value() || !cell.ssb_period.has_value() ||
          !cell.ssb_offset.has_value() || !cell.ssb_duration.has_value()) {
        fmt::print("cell={:#x}: For external cells, the gnb_id_bit_length, pci, band, ssb_arfcn, ssb_scs, ssb_period, "
                   "ssb_offset and "
                   "ssb_duration must be configured in the mobility config\n",
                   cell.nr_cell_id);
        return false;
      }
    } else {
      if (cell.pci.has_value() || cell.band.has_value() || cell.ssb_arfcn.has_value() || cell.ssb_scs.has_value() ||
          cell.ssb_period.has_value() || cell.ssb_offset.has_value() || cell.ssb_duration.has_value()) {
        fmt::print("cell={:#x}: For cells managed by the CU-CP the gnb_id_bit_length, pci, band, ssb_arfcn, ssb_scs, "
                   "ssb_period, "
                   "ssb_offset and "
                   "ssb_duration must not be configured in the mobility config\n",
                   cell.nr_cell_id);
        return false;
      }
    }

    // Check that for neighbor cells managed by this CU-CP no periodic reports are configured.
    for (const auto& ncell : cell.ncells) {
      for (const auto& id : ncell.report_cfg_ids) {
        if (report_cfg_ids_to_report_type.at(id) == "periodical") {
          fmt::print("cell={:#x}: For neighbor cells no periodic reports are allowed\n", cell.nr_cell_id);
          return false;
        }
      }
    }
  }

  // Verify that each configured neighbor cell is present.
  for (const auto& cell : config.cells) {
    for (const auto& ncell : cell.ncells) {
      nr_cell_identity nci = nr_cell_identity::create(ncell.nr_cell_id).value();
      if (ncis.find(nci) == ncis.end()) {
        fmt::print("Neighbor cell config for nci={:#x} incomplete. No valid configuration for cell nci={:#x} found.\n",
                   cell.nr_cell_id,
                   ncell.nr_cell_id);
        return false;
      }
    }
  }

  return true;
}

/// Validates the given security configuration. Returns true on success, otherwise false.
static bool validate_security_appconfig(const cu_cp_unit_security_config& config)
{
  // String splitter helper
  auto split = [](const std::string& s, char delim) -> std::vector<std::string> {
    std::vector<std::string> result;
    std::stringstream        ss(s);
    std::string              item;

    while (getline(ss, item, delim)) {
      result.push_back(item);
    }

    return result;
  };

  // > Remove spaces, convert to lower case and split on comma
  std::string nea_preference_list = config.nea_preference_list;
  nea_preference_list.erase(std::remove_if(nea_preference_list.begin(), nea_preference_list.end(), ::isspace),
                            nea_preference_list.end());
  std::transform(nea_preference_list.begin(),
                 nea_preference_list.end(),
                 nea_preference_list.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  std::vector<std::string> nea_v = split(nea_preference_list, ',');

  // > Check valid ciphering algos
  for (const std::string& algo : nea_v) {
    if (algo != "nea0" and algo != "nea1" and algo != "nea2" and algo != "nea3") {
      fmt::print("Invalid ciphering algorithm. Valid values are \"nea0\", \"nia1\", \"nia2\" and \"nia3\". algo={}\n",
                 algo);
      return false;
    }
  }

  // > Remove spaces, convert to lower case and split on comma
  std::string nia_preference_list = config.nia_preference_list;
  nia_preference_list.erase(std::remove_if(nia_preference_list.begin(), nia_preference_list.end(), ::isspace),
                            nia_preference_list.end());
  std::transform(nia_preference_list.begin(),
                 nia_preference_list.end(),
                 nia_preference_list.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  std::vector<std::string> nia_v = split(nia_preference_list, ',');

  // > Check valid integrity algos
  for (const std::string& algo : nia_v) {
    if (algo == "nia0") {
      fmt::print("NIA0 cannot be selected in the algorithm preferences.\n");
      return false;
    }
    if (algo != "nia1" and algo != "nia2" and algo != "nia3") {
      fmt::print("Invalid integrity algorithm. Valid values are \"nia1\", \"nia2\" and \"nia3\". algo={}\n", algo);
      return false;
    }
  }

  return true;
}

/// Validates the given PDCP configuration. Returns true on success, otherwise false.
static bool validate_pdcp_appconfig(five_qi_t five_qi, const cu_cp_unit_pdcp_config& config)
{
  // Check TX.
  if (config.tx.sn_field_length != 12 && config.tx.sn_field_length != 18) {
    fmt::print("PDCP TX SN length is neither 12 or 18 bits. {} SN={}\n", five_qi, config.tx.sn_field_length);
    return false;
  }
  if (config.tx.status_report_required) {
    fmt::print("PDCP TX status report required not supported yet. {}\n", five_qi);
    return false;
  }

  // Check RX.
  if (config.rx.sn_field_length != 12 && config.rx.sn_field_length != 18) {
    fmt::print("PDCP RX SN length is neither 12 or 18 bits. {} SN={}\n", five_qi, config.rx.sn_field_length);
    return false;
  }

  pdcp_t_reordering t_reordering = {};
  if (!pdcp_t_reordering_from_int(t_reordering, config.rx.t_reordering)) {
    fmt::print("PDCP RX t-Reordering is not a valid value. {}, t-Reordering={}\n", five_qi, config.rx.t_reordering);
    fmt::print("Valid values: "
               "\"infinity, ms0, ms1, ms2, ms4, ms5, ms8, ms10, ms15, ms20, ms30, ms40,ms50, ms60, ms80, "
               "ms100, ms120, ms140, ms160, ms180, ms200, ms220,ms240, ms260, ms280, ms300, ms500, ms750, ms1000, "
               "ms1250, ms1500, ms1750, ms2000, ms2250, ms2500, ms2750\"\n");
    return false;
  }
  if (t_reordering == pdcp_t_reordering::infinity) {
    fmt::print("PDCP t-Reordering=infinity on DRBs is not advised. It can cause data stalls. {}\n", five_qi);
  }

  if (config.rx.out_of_order_delivery) {
    fmt::print("PDCP RX out-of-order delivery is not supported. {}\n", five_qi);
    return false;
  }
  return true;
}

static bool validate_rlc_um_appconfig(five_qi_t five_qi, const cu_cp_unit_rlc_um_config& config)
{
  // Validate TX.

  rlc_um_sn_size tmp_sn_size;
  if (!from_number(tmp_sn_size, config.tx.sn_field_length)) {
    fmt::print("RLC UM TX SN length is neither 6 or 12 bits. {} sn_size={}\n", five_qi, config.tx.sn_field_length);
    return false;
  }

  if (config.tx.queue_size == 0) {
    fmt::print("RLC TX queue size cannot be 0. {}\n", five_qi);
    return false;
  }

  // Validate RX.

  if (!from_number(tmp_sn_size, config.rx.sn_field_length)) {
    fmt::print("RLC TX queue size cannot be 0. {}\n", five_qi);
    return false;
  }

  rlc_t_reassembly tmp_t_reassembly;
  if (!rlc_t_reassembly_from_int(tmp_t_reassembly, config.rx.t_reassembly)) {
    fmt::print("RLC UM RX t-Reassembly is invalid. {} t_reassembly={}\n", five_qi, config.rx.t_reassembly);
    fmt::print("Valid values are:"
               " ms40, ms45, ms50, ms55, ms60, ms65, ms70,"
               " ms75, ms80, ms85, ms90, ms95, ms100, ms110,"
               " ms120, ms130, ms140, ms150, ms160, ms170,"
               " ms180, ms190, ms200\n");
    return false;
  }
  return true;
}

template <typename id_type>
static bool validate_rlc_am_appconfig(id_type id, const cu_cp_unit_rlc_am_config& config)
{
  // Validate TX.

  rlc_am_sn_size tmp_sn_size;
  if (!from_number(tmp_sn_size, config.tx.sn_field_length)) {
    fmt::print("RLC AM TX SN length is neither 12 or 18 bits. {} sn_size={}\n", id, config.tx.sn_field_length);
    return false;
  }

  rlc_t_poll_retransmit tmp_t_poll_retransmit;
  if (!rlc_t_poll_retransmit_from_int(tmp_t_poll_retransmit, config.tx.t_poll_retx)) {
    fmt::print("Invalid RLC AM TX t-PollRetransmission. {} t_poll_retx={}\n", id, config.tx.t_poll_retx);
    fmt::print(" Valid values are: ms5, ms10, ms15, ms20, ms25, ms30, ms35,"
               " ms40, ms45, ms50, ms55, ms60, ms65, ms70, ms75, ms80, ms85,"
               " ms90, ms95, ms100, ms105, ms110, ms115, ms120, ms125, ms130,"
               " ms135, ms140, ms145, ms150, ms155, ms160, ms165, ms170, ms175,"
               " ms180, ms185, ms190, ms195, ms200, ms205, ms210, ms215, ms220,"
               " ms225, ms230, ms235, ms240, ms245, ms250, ms300, ms350, ms400,"
               " ms450, ms500, ms800, ms1000, ms2000, ms4000\n");
    return false;
  }

  rlc_max_retx_threshold tmp_max_retx_threshold;
  if (!rlc_max_retx_threshold_from_int(tmp_max_retx_threshold, config.tx.max_retx_thresh)) {
    fmt::print("Invalid RLC AM TX max retx threshold. {} max_retx_threshold={}\n", id, config.tx.max_retx_thresh);
    fmt::print(" Valid values are: t1, t2, t3, t4, t6, t8, t16, t32\n");
    return false;
  }

  rlc_poll_pdu tmp_poll_pdu;
  if (!rlc_poll_pdu_from_int(tmp_poll_pdu, config.tx.poll_pdu)) {
    fmt::print("Invalid RLC AM TX PollPDU. {} poll_pdu={}\n", id, config.tx.poll_pdu);
    fmt::print(" Valid values are:"
               "p4, p8, p16, p32, p64, p128, p256, p512, p1024, p2048,"
               " p4096, p6144, p8192, p12288, p16384,p20480,"
               " p24576, p28672, p32768, p40960, p49152, p57344, p65536\n");
    return false;
  }

  rlc_poll_kilo_bytes tmp_poll_bytes;
  if (!rlc_poll_kilo_bytes_from_int(tmp_poll_bytes, config.tx.poll_byte)) {
    fmt::print("Invalid RLC AM TX PollBytes. {} poll_bytes={}\n", id, config.tx.poll_byte);
    fmt::print(" Valid values are (in KBytes):"
               " kB1, kB2, kB5, kB8, kB10, kB15, kB25, kB50, kB75,"
               " kB100, kB125, kB250, kB375, kB500, kB750, kB1000,"
               " kB1250, kB1500, kB2000, kB3000, kB4000, kB4500,"
               " kB5000, kB5500, kB6000, kB6500, kB7000, kB7500,"
               " mB8, mB9, mB10, mB11, mB12, mB13, mB14, mB15,"
               " mB16, mB17, mB18, mB20, mB25, mB30, mB40, infinity\n");
    return false;
  }

  if (config.tx.queue_size == 0) {
    fmt::print("RLC AM TX queue size cannot be 0. {}\n", id);
    return false;
  }

  // Validate RX.

  if (!from_number(tmp_sn_size, config.rx.sn_field_length)) {
    fmt::print("RLC AM RX SN length is neither 12 or 18 bits. {} sn_size={}\n", id, config.rx.sn_field_length);
    return false;
  }

  rlc_t_reassembly tmp_t_reassembly;
  if (!rlc_t_reassembly_from_int(tmp_t_reassembly, config.rx.t_reassembly)) {
    fmt::print("RLC AM RX t-Reassembly is invalid. {} t_reassembly={}\n", id, config.rx.t_reassembly);
    fmt::print("Valid values are:"
               " ms40, ms45, ms50, ms55, ms60, ms65, ms70,"
               " ms75, ms80, ms85, ms90, ms95, ms100, ms110,"
               " ms120, ms130, ms140, ms150, ms160, ms170,"
               " ms180, ms190, ms200\n");
    return false;
  }

  rlc_t_status_prohibit tmp_t_status_prohibit;
  if (!rlc_t_status_prohibit_from_int(tmp_t_status_prohibit, config.rx.t_status_prohibit)) {
    fmt::print("RLC AM RX t-statusProhibit is invalid. {} t_status_prohibit={}\n", id, config.rx.t_status_prohibit);
    fmt::print("Valid values are:"
               "ms0, ms5, ms10, ms15, ms20, ms25, ms30, ms35,"
               "ms40, ms45, ms50, ms55, ms60, ms65, ms70,"
               "ms75, ms80, ms85, ms90, ms95, ms100, ms105,"
               "ms110, ms115, ms120, ms125, ms130, ms135,"
               "ms140, ms145, ms150, ms155, ms160, ms165,"
               "ms170, ms175, ms180, ms185, ms190, ms195,"
               "ms200, ms205, ms210, ms215, ms220, ms225,"
               "ms230, ms235, ms240, ms245, ms250, ms300,"
               "ms350, ms400, ms450, ms500, ms800, ms1000,"
               "ms1200, ms1600, ms2000, ms2400\n");
    return false;
  }

  if (config.rx.max_sn_per_status >= window_size(config.rx.sn_field_length)) {
    fmt::print("RLC AM RX max_sn_per_status={} exceeds window_size={}. sn_size={}\n",
               config.rx.max_sn_per_status,
               window_size(config.rx.sn_field_length),
               config.rx.sn_field_length);
    return false;
  }

  return true;
}

static bool validate_rlc_appconfig(five_qi_t five_qi, const cu_cp_unit_rlc_config& config)
{
  // Check mode.
  if (config.mode != "am" && config.mode != "um-bidir") {
    fmt::print("RLC mode is neither \"am\" or \"um-bidir\". {} mode={}\n", five_qi, config.mode);
    return false;
  }

  // Check AM.
  if (config.mode == "am" && !validate_rlc_am_appconfig(five_qi, config.am)) {
    fmt::print("RLC AM config is invalid. {}\n", five_qi);
    return false;
  }

  // Check UM.
  if (config.mode == "um-bidir" && !validate_rlc_um_appconfig(five_qi, config.um)) {
    fmt::print("RLC UM config is invalid. {}\n", five_qi);
    return false;
  }
  return true;
}

/// Validates the given QoS configuration. Returns true on success, otherwise false.
static bool validate_qos_appconfig(span<const cu_cp_unit_qos_config> config)
{
  for (const auto& qos : config) {
    if (!validate_pdcp_appconfig(qos.five_qi, qos.pdcp)) {
      return false;
    }
    if (!validate_rlc_appconfig(qos.five_qi, qos.rlc)) {
      return false;
    }
  }
  return true;
}

/// Validates the given AMF configuration. Returns true on success, otherwise false.
static bool validate_amf_appconfig(const cu_cp_unit_amf_config&                   amf_config,
                                   const std::vector<cu_cp_unit_amf_config_item>& extra_amfs)
{
  std::vector<std::string> plmns;

  std::vector<cu_cp_unit_amf_config_item> amfs;

  amfs.push_back(amf_config.amf);

  amfs.insert(amfs.end(), extra_amfs.begin(), extra_amfs.end());

  for (const auto& config : amfs) {
    // check for non-empty AMF address
    if (config.ip_addr.empty()) {
      return false;
    }

    // check supported tracking areas
    if (config.supported_tas.size() > 1) {
      for (unsigned outer_ta_idx = 0; outer_ta_idx < config.supported_tas.size(); outer_ta_idx++) {
        std::vector<std::string> outer_plmns;
        for (const auto& plmn_item : config.supported_tas[outer_ta_idx].plmn_list) {
          outer_plmns.push_back(plmn_item.plmn_id);
        }

        for (unsigned inner_ta_idx = outer_ta_idx + 1; inner_ta_idx < config.supported_tas.size(); inner_ta_idx++) {
          if (config.supported_tas[outer_ta_idx].tac == config.supported_tas[inner_ta_idx].tac) {
            for (const auto& plmn_item : config.supported_tas[inner_ta_idx].plmn_list) {
              if (std::find(outer_plmns.begin(), outer_plmns.end(), plmn_item.plmn_id) != outer_plmns.end()) {
                fmt::print("Supported tracking areas of a AMF must be unique\n");
                return false;
              }
            }
          }
        }
      }
    }

    for (const auto& ta : config.supported_tas) {
      for (const auto& plmn_item : ta.plmn_list) {
        if (std::find(plmns.begin(), plmns.end(), plmn_item.plmn_id) == plmns.end()) {
          plmns.push_back(plmn_item.plmn_id);
        } else {
          fmt::print("PLMN={} is already supported by another AMF\n", plmn_item.plmn_id);
          return false;
        }

        if (plmn_item.tai_slice_support_list.empty()) {
          fmt::print("TAI slice support list for PLMN={} and TAC={} is empty\n", plmn_item.plmn_id, ta.tac);
          return false;
        }
      }
    }
  }

  return true;
}

/// Validates the given CU-CP configuration. Returns true on success, otherwise false.
static bool validate_cu_cp_appconfig(const gnb_id_t gnb_id, const cu_cp_unit_config& config)
{
  auto is_valid_watermark = [](const cu_cp_unit_admission_watermark_config& watermark) {
    return watermark.max_ue_usage > 0 && watermark.max_ue_usage <= 100 && watermark.max_drb_usage > 0 &&
           watermark.max_drb_usage <= 100;
  };

  if (!is_valid_watermark(config.initial_access_admission) ||
      !is_valid_watermark(config.reestablishment_admission) || !is_valid_watermark(config.handover_admission)) {
    fmt::print("Invalid CU-CP admission configuration. Watermarks must be in the range [1, 100]\n");
    return false;
  }

  // validate AMF config
  if (!validate_amf_appconfig(config.amf_config, config.extra_amfs)) {
    return false;
  }

  // validate mobility config
  if (!validate_mobility_appconfig(gnb_id, config.mobility_config)) {
    return false;
  }

  if (!validate_security_appconfig(config.security_config)) {
    return false;
  }

  if (!validate_qos_appconfig(config.qos_cfg)) {
    return false;
  }

  return true;
}

bool srsran::validate_cu_cp_unit_config(const cu_cp_unit_config& config)
{
  return validate_cu_cp_appconfig(config.gnb_id, config);
}
