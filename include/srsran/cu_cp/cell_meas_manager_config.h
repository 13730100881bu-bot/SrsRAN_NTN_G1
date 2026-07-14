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
#include "srsran/cu_cp/ntn_location.h"
#include "srsran/ran/band_helper.h"
#include "srsran/ran/gnb_id.h"
#include "srsran/ran/nr_cgi.h"
#include "srsran/ran/plmn_identity.h"
#include "srsran/ran/subcarrier_spacing.h"
#include "srsran/rrc/meas_types.h"
#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace srsran {

namespace srs_cu_cp {

/// \brief Essential parameters required to configure serving cell measurements in the UE.
/// Note that some optional values need to be provided by the DU upon F1Setup.

struct serving_cell_meas_config {
  nr_cell_identity nci;                                ///< The NR cell identifier.
  unsigned         gnb_id_bit_length = 0;              ///< gNodeB identifier bit length.
  plmn_identity    plmn = plmn_identity::test_value(); ///< PLMN identity.
  /// If not set in config must be provided by config update after DU attach.
  std::optional<pci_t>              pci;       ///< Physical cell identifier.
  std::optional<nr_band>            band;      ///< NR band.
  std::optional<rrc_ssb_mtc>        ssb_mtc;   ///< SSB measurement and timing config.
  std::optional<unsigned>           ssb_arfcn; ///< SSB ARFCN.
  std::optional<subcarrier_spacing> ssb_scs;   ///< SSB subcarrier spacing.
};

struct neighbor_cell_meas_config {
  nr_cell_identity             nci;            ///< The NR cell identifier.
  std::vector<report_cfg_id_t> report_cfg_ids; ///< The configured report configs
};

/// \brief Essential parameters required to configure serving and neighbor cell measurements in the UE.
/// Note that some optional values need to be provided by the DU upon F1Setup.
struct cell_meas_config {
  serving_cell_meas_config               serving_cell_cfg;       ///< Serving cell measurement config
  std::optional<report_cfg_id_t>         periodic_report_cfg_id; ///< The periodic report config
  std::vector<neighbor_cell_meas_config> ncells;                 ///< List of neighbor cells.
};

/// Static ground-fixed position of one NTN beam as seen by CU-CP mobility.
struct ntn_beam_position {
  std::string      beam_id;                         ///< Stable identifier from the external NTN beam table.
  nr_cell_identity nci = nr_cell_identity::min();   ///< Cell or beam representative NCI.
  double           center_latitude_deg  = 0.0;      ///< Beam center latitude.
  double           center_longitude_deg = 0.0;      ///< Beam center longitude.
  double           coverage_radius_m    = 0.0;      ///< Physical coverage radius used by mobility decisions.
  bool             enabled              = true;     ///< Whether this static beam participates in mobility.
};

/// Static NTN beam table loaded from an external configuration file.
struct ntn_beam_table_config {
  unsigned                       version            = 1;
  std::string                    region;
  std::optional<double>          satellite_height_m;
  std::vector<ntn_beam_position> beams;
};

enum class ntn_satellite_state_source { manual, circular_orbit, tle };

/// Orbit source used to periodically refresh the CU-CP NTN served beam set.
struct ntn_satellite_state_update_config {
  ntn_satellite_state_source source = ntn_satellite_state_source::manual;

  /// Period used to refresh satellite state. A value of zero disables periodic orbit-driven updates.
  std::chrono::milliseconds update_period{0};

  double                                circular_altitude_m               = 500000.0;
  double                                circular_inclination_deg          = 53.0;
  double                                circular_raan_deg                 = 0.0;
  double                                circular_argument_of_latitude_deg = 0.0;
  std::chrono::system_clock::time_point circular_epoch                    = std::chrono::system_clock::time_point{};

  std::string tle_satellite_name;
  std::string tle_line1;
  std::string tle_line2;
};

/// Core-network reporting policy for accepted NTN UE location reports.
struct ntn_core_network_location_reporting_config {
  /// Locally forward every accepted NTN UE location report as an NGAP LocationReport.
  bool local_forwarding_enabled = false;

  /// Accept AMF LocationReportingControl requests and report matching accepted NTN UE locations.
  bool amf_control_enabled = true;

  /// Minimum interval between two core-network reports for the same UE. A value of zero disables throttling.
  std::chrono::milliseconds min_report_interval{0};
};

/// Location-based NTN mobility configuration.
struct ntn_location_mobility_config {
  bool enabled = false;

  ntn_core_network_location_reporting_config core_network_reporting;

  /// Static beam table known by CU-CP. This is typically loaded from JSON at configuration time.
  std::vector<ntn_beam_position> beams;

  /// Minimum satellite elevation angle required for a beam to become part of the runtime served beam set.
  double served_beam_min_elevation_deg = 10.0;

  /// Maximum number of beams that may be simultaneously served by the runtime hopping schedule.
  unsigned max_nof_served_beams = 1;

  /// Rotate the active CU-CP beam-set window across visible beams when visibility exceeds max_nof_served_beams.
  bool served_beam_hopping_enabled = false;

  /// Number of accepted satellite-state update periods that one hopping window should hold before rotating.
  unsigned served_beam_hopping_dwell_updates = 1;

  /// Optional automatic satellite state source for orbit-driven served beam updates.
  ntn_satellite_state_update_config satellite_state_update;

  /// Period used when location is derived from periodic MeasurementReport messages. A value of zero disables the
  /// report-count derivation and relies on explicit consecutive report/time settings.
  std::chrono::milliseconds measurement_report_period{0};

  /// Minimum stable candidate time for aperiodic sources such as UE Assistance Information.
  std::chrono::milliseconds time_to_trigger{0};

  /// Maximum allowed gap between consecutive location samples for the same candidate. A value of zero disables it.
  std::chrono::milliseconds max_report_gap{0};

  /// Maximum age of a received location sample before it is ignored. A value of zero disables it.
  std::chrono::milliseconds location_max_age{0};

  /// Time after an accepted NTN handover trigger before the same stable candidate may be retried. A value of zero
  /// disables automatic retry from the measurement manager.
  std::chrono::milliseconds handover_retry_timeout{0};

  /// Minimum consecutive samples that must select the same target beam before handover.
  unsigned required_consecutive_location_reports = 1;

  /// Extra serving-beam margin used to avoid ping-pong around a beam boundary.
  double boundary_hysteresis_m = 0.0;

  /// Optional accuracy gate for UE-provided or measurement-derived positions.
  std::optional<double> max_horizontal_accuracy_m;
};

/// \brief Verifies required parameters are set. Returns true if config is valid, false otherwise.
bool is_complete(const serving_cell_meas_config& cfg);

/// Parses a static NTN beam table JSON document.
expected<ntn_beam_table_config, std::string> parse_ntn_beam_table_json(const std::string& json_text);

/// Loads and parses a static NTN beam table JSON file.
expected<ntn_beam_table_config, std::string> load_ntn_beam_table_json_file(const std::string& path);

/// \brief Cell manager configuration.
struct cell_meas_manager_cfg {
  std::map<nr_cell_identity, cell_meas_config> cells; // Measurement related configs for all known cells.
  std::map<report_cfg_id_t, rrc_report_cfg_nr> report_config_ids;
  ntn_location_mobility_config                 ntn_location_mobility;
};

/// \brief Validates configuration but doesn't verify if all provided cells have complete configuration (yet). Returns
/// true if config is valid, false otherwise.
bool is_valid_configuration(const cell_meas_manager_cfg&                                cfg,
                            const std::unordered_map<ssb_frequency_t, rrc_meas_obj_nr>& ssb_freq_to_meas_object = {});

/// \brief Same as config validation but additionally verfies that the measurement related parameters are present for
/// all cells.
bool is_complete(const cell_meas_manager_cfg& cfg);

} // namespace srs_cu_cp

} // namespace srsran

namespace fmt {

// Cell meas config formatter
template <>
struct formatter<srsran::srs_cu_cp::cell_meas_config> {
  template <typename ParseContext>
  auto parse(ParseContext& ctx)
  {
    return ctx.begin();
  }

  template <typename FormatContext>
  auto format(const srsran::srs_cu_cp::cell_meas_config& cfg, FormatContext& ctx) const
  {
    std::string ncell_str = "[ ";
    for (const auto& ncell : cfg.ncells) {
      ncell_str += fmt::format("{:#x} ", ncell.nci);
    }
    ncell_str = ncell_str + "]";

    return format_to(
        ctx.out(),
        "nci={:#x} complete={} gnb_id={} pci={} band={} ssb_arfcn={} ssb_scs={} ncells={}",
        cfg.serving_cell_cfg.nci,
        is_complete(cfg.serving_cell_cfg) ? "yes" : "no",
        cfg.serving_cell_cfg.nci.gnb_id(cfg.serving_cell_cfg.gnb_id_bit_length).id,
        cfg.serving_cell_cfg.pci.has_value() ? to_string(cfg.serving_cell_cfg.pci.value()) : "?",
        cfg.serving_cell_cfg.band.has_value() ? to_string(nr_band_to_uint(cfg.serving_cell_cfg.band.value())) : "?",
        cfg.serving_cell_cfg.ssb_arfcn.has_value() ? to_string(cfg.serving_cell_cfg.ssb_arfcn.value()) : "?",
        cfg.serving_cell_cfg.ssb_scs.has_value() ? to_string(cfg.serving_cell_cfg.ssb_scs.value()) : "?",
        ncell_str);
  }
};

} // namespace fmt
