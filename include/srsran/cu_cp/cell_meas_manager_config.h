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

/// CU-CP resource-domain caps applied to an analog access beam. A cap value of zero means unlimited.
struct ntn_analog_beam_resource_policy {
  unsigned max_access_only_ues           = 0;
  unsigned max_service_bound_ues         = 0;
  unsigned max_loaded_digital_children   = 0;
  unsigned max_drbs                      = 0;
};

/// CU-CP resource-domain caps and explicit reuse/conflict metadata applied to a digital service beam.
struct ntn_digital_beam_resource_policy {
  unsigned                 max_ues             = 0;
  unsigned                 max_drbs            = 0;
  unsigned                 max_loaded_ues      = 0;
  std::string              reuse_group_id;
  std::vector<std::string> conflict_group_ids;
};

/// CU-CP resource-domain policy defaults. Per-beam overrides may refine these defaults.
struct ntn_resource_domain_policy {
  ntn_analog_beam_resource_policy  analog;
  ntn_digital_beam_resource_policy digital;
};

/// Static ground-fixed position of one NTN beam as seen by CU-CP mobility.
struct ntn_beam_position {
  std::string      beam_id;                         ///< Stable identifier from the external NTN beam table.
  nr_cell_identity nci = nr_cell_identity::min();   ///< Cell or beam representative NCI.
  double           center_latitude_deg  = 0.0;      ///< Beam center latitude.
  double           center_longitude_deg = 0.0;      ///< Beam center longitude.
  double           coverage_radius_m    = 0.0;      ///< Physical coverage radius used by mobility decisions.
  bool             enabled              = true;     ///< Whether this static beam participates in mobility.
  bool             downlink_enabled     = true;     ///< Whether CU-CP may use this beam for downlink assistance/paging.
  bool             uplink_enabled       = true;     ///< Whether CU-CP may use this beam for uplink SR/SRS resources.
  std::string      analog_beam_id;                  ///< Parent analog access beam identifier, when configured.
  std::optional<int> hex_q;                          ///< Optional axial hex q coordinate from the planning grid.
  std::optional<int> hex_r;                          ///< Optional axial hex r coordinate from the planning grid.
  std::optional<ntn_digital_beam_resource_policy> resource_policy; ///< Optional digital service resource policy.
};

/// Static CU-CP-only analog access beam grouping over digital service beams.
struct ntn_analog_beam_position {
  std::string              analog_beam_id;         ///< Stable analog access beam identifier.
  int                      center_hex_q = 0;       ///< Axial q coordinate of the analog cluster center.
  int                      center_hex_r = 0;       ///< Axial r coordinate of the analog cluster center.
  std::string              center_digital_beam_id; ///< Digital beam at the analog cluster center.
  std::vector<std::string> child_digital_beam_ids; ///< Digital service beams controlled by this analog access beam.
  bool                     is_edge_partial = false; ///< True when the footprint boundary cuts the 7-cell cluster.
  bool                     enabled         = true;  ///< Whether this analog access beam participates in access gating.
  bool                     downlink_enabled = true; ///< Whether child beams may be used for downlink access/service intent.
  bool                     uplink_enabled   = true; ///< Whether child beams may be used for uplink access/service intent.
  std::optional<ntn_analog_beam_resource_policy> resource_policy; ///< Optional analog access resource policy.
};

/// Static NTN beam table loaded from an external configuration file.
struct ntn_beam_table_config {
  unsigned                       version            = 1;
  std::string                    region;
  std::optional<double>          satellite_height_m;
  ntn_resource_domain_policy     resource_policy;
  std::vector<ntn_analog_beam_position> analog_beams;
  std::vector<ntn_beam_position> beams;
};

enum class ntn_satellite_state_source { manual, circular_orbit, tle };

/// One configured satellite in a CU-CP-only circular-orbit constellation.
struct ntn_circular_orbit_satellite_config {
  std::string satellite_id = "sat-0";
  double      altitude_m               = 500000.0;
  double      inclination_deg          = 53.0;
  double      raan_deg                 = 0.0;
  double      argument_of_latitude_deg = 0.0;
  std::chrono::system_clock::time_point epoch = std::chrono::system_clock::time_point{};
};

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

  /// Optional multi-satellite circular orbit list. When empty, the legacy single circular_orbit_* fields are used.
  std::vector<ntn_circular_orbit_satellite_config> circular_orbit_satellites;

  /// Future service-window horizon for predictive beam mobility. Zero keeps the legacy one-step prediction.
  std::chrono::milliseconds predictive_service_window_horizon{0};

  /// Lead time before predicted beam exit when CU-CP should block new demand and prepare handover.
  std::chrono::milliseconds predictive_handover_lead_time{0};

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

  /// Static CU-CP-only analog access beam groups over digital beams.
  std::vector<ntn_analog_beam_position> analog_beams;

  /// CU-CP-only resource-domain policy defaults. Zero caps mean unlimited.
  ntn_resource_domain_policy resource_policy;

  /// Minimum satellite elevation angle required for a beam to become part of the runtime served beam set.
  double served_beam_min_elevation_deg = 10.0;

  /// Maximum number of beams in the runtime hopping window. A value of zero means no CU-CP cap.
  unsigned max_nof_served_beams = 1;

  /// Maximum number of simultaneously active analog access beams. A value of zero means no CU-CP cap.
  unsigned max_nof_active_analog_access_beams = 0;

  /// Maximum number of loaded digital service beams. A value of zero means no CU-CP cap.
  unsigned max_nof_loaded_digital_service_beams = 0;

  /// Time an idle preheated analog/digital beam is retained before returning to candidate/inactive. Zero disables.
  std::chrono::milliseconds preheated_beam_hold_time{5000};

  /// Minimum time after preheat application before a cross-analog target is considered ready. Zero disables.
  std::chrono::milliseconds preheated_beam_min_ready_time{1000};

  /// Cooldown before reusing the same source/target analog pair for load rebalancing. Zero disables.
  std::chrono::milliseconds analog_rebalance_pair_cooldown{10000};

  /// Hold time for target-aware digital capacity reservations. Zero disables reservation expiry.
  std::chrono::milliseconds digital_target_reservation_hold_time{5000};

  /// Rotate the active CU-CP access/service window across visible beams when configured caps are exceeded.
  bool served_beam_hopping_enabled = false;

  /// Number of accepted satellite-state update periods that one hopping window should hold before rotating.
  unsigned served_beam_hopping_dwell_updates = 1;

  /// Enable CU-CP steering and controlled handover to balance load across eligible digital service beams.
  bool multi_beam_load_balancing_enabled = false;

  /// Prefer beams with active CU-CP UE/DRB/QoS demand when selecting the active served/hopping window.
  bool demand_aware_beam_scheduling_enabled = false;

  /// Reserve CU-CP multi-beam headroom for handover/rebalance target capacity before admitting low-priority demand.
  bool multi_beam_headroom_admission_enabled = false;

  /// Minimum source-target UE load delta before CU-CP considers proactive load-balancing handover.
  unsigned multi_beam_load_balancing_min_ue_delta = 2;

  /// Maximum number of load-balancing handovers scheduled in one CU-CP evaluation.
  unsigned multi_beam_load_balancing_max_handovers_per_eval = 1;

  /// Cooldown before the same UE can be considered for another load-balancing handover.
  std::chrono::milliseconds multi_beam_load_balancing_handover_cooldown{30000};

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

  /// Grace period after a service-bound UE location becomes missing/stale before CU-CP releases the UE. A value of zero
  /// derives the grace from the periodic report configuration.
  std::chrono::milliseconds location_lost_release_grace_period{0};

  /// Maximum age of a cached NTN idle paging context. A value of zero disables context expiry.
  std::chrono::milliseconds idle_paging_context_max_age{300000};

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
