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

#include "apps/helpers/metrics/metrics_config.h"
#include "apps/units/o_cu_cp/cu_cp/cu_cp_unit_pcap_config.h"
#include "cu_cp_unit_logger_config.h"
#include "srsran/ran/gnb_id.h"
#include "srsran/ran/nr_band.h"
#include "srsran/ran/pci.h"
#include "srsran/ran/qos/five_qi.h"
#include "srsran/ran/s_nssai.h"
#include "srsran/ran/tac.h"
#include <optional>
#include <string>
#include <vector>

namespace srsran {

struct cu_cp_unit_plmn_item {
  struct tai_slice_t {
    uint8_t  sst = 0;
    uint32_t sd  = 0xffffffU;
  };

  std::string plmn_id;
  /// Supported Slices by the RAN node.
  std::vector<tai_slice_t> tai_slice_support_list;
};

struct cu_cp_unit_supported_ta_item {
  tac_t                             tac;
  std::vector<cu_cp_unit_plmn_item> plmn_list;
};

struct cu_cp_unit_amf_config_item {
  std::string ip_addr                = "127.0.1.100";
  uint16_t    port                   = 38412;
  std::string bind_addr              = "127.0.0.1";
  std::string bind_interface         = "auto";
  int         sctp_rto_initial_ms    = 120;
  int         sctp_rto_min_ms        = 120;
  int         sctp_rto_max_ms        = 500;
  int         sctp_init_max_attempts = 3;
  int         sctp_max_init_timeo_ms = 500;
  int         sctp_hb_interval_ms    = 30000;
  int         sctp_assoc_max_retx    = 10;
  bool        sctp_nodelay           = false;

  /// List of all tracking areas supported by the AMF.
  std::vector<cu_cp_unit_supported_ta_item> supported_tas = {{7, {{"00101", {cu_cp_unit_plmn_item::tai_slice_t{1}}}}}};
  bool                                      is_default_supported_tas = true;
};

struct cu_cp_unit_amf_config {
  cu_cp_unit_amf_config_item amf;
  /// Allow CU-CP to run without a core, e.g. for test mode.
  bool no_core = false;
  /// Time to wait after a failed AMF reconnection attempt in ms.
  unsigned amf_reconnection_retry_time = 1000;
};

/// Report configuration, for now only supporting the A3 event.
struct cu_cp_unit_report_config {
  unsigned    report_cfg_id;
  std::string report_type;
  unsigned    report_interval_ms;

  std::optional<std::string> event_triggered_report_type;
  std::optional<std::string> meas_trigger_quantity;
  std::optional<int>         meas_trigger_quantity_threshold_db;
  std::optional<int>         meas_trigger_quantity_threshold_2_db;
  std::optional<int> meas_trigger_quantity_offset_db; ///< [-30..30] Note the actual value is field value * 0.5 dB. E.g.
                                                      ///< putting a value of -6 here results in -3dB offset.
  std::optional<unsigned> hysteresis_db;
  std::optional<unsigned> time_to_trigger_ms;
  int                     periodic_ho_rsrp_offset =
      -1; ///< -1 disables handovers from periodic measurements. [0..30] Note the actual value is field value * 0.5 dB.
          ///< E.g. putting a value of -6 here results in -3dB offset.
};

struct cu_cp_unit_neighbor_cell_config_item {
  /// Cell id.
  uint64_t nr_cell_id;
  /// Report config ids.
  std::vector<uint64_t> report_cfg_ids;
};

/// Each item describes the relationship between one cell to all other cells.
struct cu_cp_unit_cell_config_item {
  /// Cell id.
  uint64_t                nr_cell_id;
  std::optional<unsigned> periodic_report_cfg_id;

  // These parameters must only be set for external cells
  /// gNodeB identifier bit length.
  std::optional<unsigned> gnb_id_bit_length;
  /// PCI.
  std::optional<pci_t> pci;
  /// NR band.
  std::optional<nr_band> band;
  /// SSB ARFCN.
  std::optional<unsigned> ssb_arfcn;
  /// SSB subcarrier spacing.
  std::optional<unsigned> ssb_scs;
  /// SSB period.
  std::optional<unsigned> ssb_period;
  /// SSB offset.
  std::optional<unsigned> ssb_offset;
  /// SSB duration.
  std::optional<unsigned> ssb_duration;
  /// Vector of cells that are a neighbor of this cell.
  std::vector<cu_cp_unit_neighbor_cell_config_item> ncells;
  // TODO: Add optional SSB parameters.
};

/// Circular-orbit satellite entry for NTN location-based mobility.
struct cu_cp_unit_ntn_circular_orbit_satellite_config {
  std::string satellite_id;
  double      altitude_m               = 500000.0;
  double      inclination_deg          = 53.0;
  double      raan_deg                 = 0.0;
  double      argument_of_latitude_deg = 0.0;
  std::optional<double> epoch_unix_s;
};

/// NTN location-based mobility application configuration.
struct cu_cp_unit_ntn_location_mobility_config {
  bool enabled = false;

  /// JSON file containing the static NTN beam table.
  std::string beam_table_json_file;

  /// Minimum satellite elevation angle for runtime served beam selection.
  double served_beam_min_elevation_deg = 10.0;

  /// Maximum number of beams in the runtime hopping window. A value of zero means no CU-CP cap.
  unsigned max_nof_served_beams = 1;

  /// Maximum number of simultaneously active analog access beams. A value of zero means no CU-CP cap.
  unsigned max_nof_active_analog_access_beams = 0;

  /// Maximum number of simultaneously loaded digital service beams. A value of zero means no CU-CP cap.
  unsigned max_nof_loaded_digital_service_beams = 0;

  /// Idle hold time for preheated NTN analog/digital beams. A value of zero disables idle demotion.
  unsigned preheated_beam_hold_time_ms = 5000;

  /// Minimum ready guard after preheat application before using a cross-analog target. Zero disables.
  unsigned preheated_beam_min_ready_time_ms = 1000;

  /// Cooldown before reusing the same source/target analog pair for load rebalancing. Zero disables.
  unsigned analog_rebalance_pair_cooldown_ms = 10000;

  /// Hold time for target-aware digital capacity reservations. Zero disables expiry.
  unsigned digital_target_reservation_hold_time_ms = 5000;

  /// Rotate the active CU-CP beam-set window across visible beams when more beams are visible than can be active.
  bool served_beam_hopping_enabled = false;

  /// Number of satellite-state update periods that one hopping window should hold before rotating.
  unsigned served_beam_hopping_dwell_updates = 1;

  /// Enable CU-CP steering and controlled handover to balance load across eligible NTN digital service beams.
  bool multi_beam_load_balancing_enabled = false;

  /// Prefer beams with active CU-CP UE/DRB/QoS demand when selecting the served/hopping window.
  bool demand_aware_beam_scheduling_enabled = false;

  /// Reserve multi-beam headroom for NTN handover/rebalance target capacity before admitting low-priority demand.
  bool multi_beam_headroom_admission_enabled = false;

  /// Minimum source-target UE load delta before proactive load-balancing handover.
  unsigned multi_beam_load_balancing_min_ue_delta = 2;

  /// Maximum number of load-balancing handovers scheduled in one CU-CP evaluation.
  unsigned multi_beam_load_balancing_max_handovers_per_eval = 1;

  /// Cooldown before the same UE can be considered for another load-balancing handover.
  unsigned multi_beam_load_balancing_handover_cooldown_ms = 30000;

  /// Satellite state source used for runtime served beam updates: manual, circular_orbit or tle.
  std::string satellite_state_source = "manual";

  /// Period of orbit-driven satellite state updates. Zero disables automatic updates.
  unsigned satellite_state_update_period_ms = 0;

  /// Future service-window prediction horizon. Zero keeps legacy one-step prediction.
  unsigned predictive_service_window_horizon_ms = 0;

  /// Lead time before predicted beam exit when CU-CP prepares handover and blocks new service demand.
  unsigned predictive_handover_lead_time_ms = 0;

  double circular_orbit_altitude_m               = 500000.0;
  double circular_orbit_inclination_deg          = 53.0;
  double circular_orbit_raan_deg                 = 0.0;
  double circular_orbit_argument_of_latitude_deg = 0.0;
  std::optional<double> circular_orbit_epoch_unix_s;
  std::vector<cu_cp_unit_ntn_circular_orbit_satellite_config> circular_orbit_satellites;

  std::string tle_satellite_name;
  std::string tle_line1;
  std::string tle_line2;

  /// Period used when UE position is derived from periodic MeasurementReport messages. Zero disables report-count
  /// derivation from this period.
  unsigned measurement_report_period_ms = 0;

  /// Minimum stable candidate time before handover.
  unsigned time_to_trigger_ms = 0;

  /// Maximum gap between consecutive location samples for the same candidate. Zero disables the gap check.
  unsigned max_report_gap_ms = 0;

  /// Maximum accepted age of a location sample. Zero disables the age check.
  unsigned location_max_age_ms = 0;

  /// Grace period after a service-bound UE location becomes missing/stale before release. Zero derives the default.
  unsigned location_lost_release_grace_period_ms = 0;

  /// Maximum age of cached NTN idle paging context. Zero disables context expiry.
  unsigned idle_paging_context_max_age_ms = 300000;

  /// Retry timeout after an accepted NTN handover trigger. Zero disables automatic retry.
  unsigned handover_retry_timeout_ms = 0;

  /// Required consecutive location samples for the same candidate beam.
  unsigned required_consecutive_location_reports = 1;

  /// Additional serving-beam margin to reduce boundary ping-pong.
  double boundary_hysteresis_m = 0.0;

  /// Optional maximum accepted UE horizontal position error.
  std::optional<double> max_horizontal_accuracy_m;

  /// Locally forward accepted NTN UE location reports to NGAP LocationReport.
  bool core_network_reporting_local_forwarding_enabled = false;

  /// Accept AMF LocationReportingControl requests for NTN location reporting.
  bool core_network_reporting_amf_control_enabled = true;

  /// Minimum interval between two core-network LocationReports for the same UE. Zero disables throttling.
  unsigned core_network_reporting_min_report_interval_ms = 0;
};

/// Independent, opt-in management-center source for the two-cell onboard L1 position plan.
struct cu_cp_unit_ntn_onboard_position_plan_config {
  bool                  enabled = false;
  bool                  du_execution_enabled = false;
  std::string           satellite_id;
  std::string           plan_json_file;
  std::string           state_file;
  std::string           expected_catalog_id;
  std::string           expected_catalog_hash;
  std::string           expected_identity_registry_version;
  std::string           expected_identity_registry_hash;
  std::string           expected_access_profile_id;
  std::string           expected_access_profile_hash;
  unsigned              reload_period_ms = 0;
  unsigned              du_prepare_guard_ms = 1000;
  unsigned              du_prepare_horizon_ms = 4000;
  unsigned              du_apply_timeout_ms = 500;
  std::vector<uint64_t> cell_ncis;
  std::vector<unsigned> cell_pcis;
  unsigned              max_l1_positions_per_cell      = 128;
  unsigned              max_l1_positions_per_satellite = 256;
  unsigned              max_analog_ports_per_cell      = 16;
  unsigned              max_analog_ports_per_satellite = 32;
  unsigned              max_digital_ports_per_cell      = 64;
  unsigned              max_digital_ports_per_satellite = 128;
  unsigned              access_slot_us                 = 10000;
  unsigned              subvisit_duration_us           = 2500;
  unsigned              max_ssb_interval_ms            = 80;
  unsigned              max_prach_interval_ms          = 640;
  unsigned              activation_alignment_ms        = 640;
};

/// All mobility related configuration parameters.
struct cu_cp_unit_mobility_config {
  /// List of all cells known to the CU-CP.
  std::vector<cu_cp_unit_cell_config_item> cells;
  /// JSON file containing static neighbor-cell information and relations.
  std::string neighbor_cell_info_json_file;
  /// Report config.
  std::vector<cu_cp_unit_report_config> report_configs;
  /// Whether to start HO if neighbor cell measurements arrive.
  bool trigger_handover_from_measurements = false;
  /// Location-based NTN mobility configuration.
  cu_cp_unit_ntn_location_mobility_config ntn_location_mobility;
  /// Versioned onboard position-plan input. Disabled by default and independent of legacy beam-to-NCI mobility.
  cu_cp_unit_ntn_onboard_position_plan_config ntn_onboard_position_plan;
};

/// RRC specific configuration parameters.
struct cu_cp_unit_rrc_config {
  bool force_reestablishment_fallback = false;
  /// Guard time in ms that is added to the RRC procedure timeout.
  /// NOTE: Guard time needs to be larger then SRB max retx thres * t-PollRetransmit.
  /// (2 * default SRB maxRetxThreshold * t-PollRetransmit = 2 * 8 * 45ms = 720ms, see TS 38.331 Sec 9.2.1)
  unsigned rrc_procedure_guard_time_ms = 1000;
};

/// Security configuration parameters.
struct cu_cp_unit_security_config {
  std::string integrity_protection       = "not_needed";
  std::string confidentiality_protection = "required";
  std::string nea_preference_list        = "nea0,nea2,nea1,nea3";
  std::string nia_preference_list        = "nia2,nia1,nia3";
};

/// F1AP-CU configuration parameters.
struct cu_cp_unit_f1ap_config {
  /// Timeout for the F1AP procedures in milliseconds.
  unsigned procedure_timeout = 1000;
};

/// E1AP-CU-CP configuration parameters.
struct cu_cp_unit_e1ap_config {
  /// Timeout for the E1AP procedures in milliseconds.
  unsigned procedure_timeout = 1000;
};

/// RLC UM TX configuration
struct cu_cp_unit_rlc_tx_um_config {
  /// Number of bits used for sequence number.
  uint16_t sn_field_length;
  /// RLC SDU queue size.
  uint32_t queue_size;
};

/// RLC UM RX configuration
struct cu_cp_unit_rlc_rx_um_config {
  /// Number of bits used for sequence number.
  uint16_t sn_field_length;
  /// Timer used by rx to detect PDU loss (ms).
  int32_t t_reassembly;
};

/// RLC UM configuration
struct cu_cp_unit_rlc_um_config {
  cu_cp_unit_rlc_tx_um_config tx;
  cu_cp_unit_rlc_rx_um_config rx;
};

/// RLC UM TX configuration
struct cu_cp_unit_rlc_tx_am_config {
  /// Number of bits used for sequence number.
  uint16_t sn_field_length;
  /// Poll retx timeout (ms).
  int32_t t_poll_retx;
  /// Max retx threshold.
  uint32_t max_retx_thresh;
  /// Insert poll bit after this many PDUs.
  int32_t poll_pdu;
  /// Insert poll bit after this much data (bytes).
  int32_t poll_byte;
  /// Custom parameter to limit the maximum window size for memory reasons. 0 means no limit.
  uint32_t max_window = 0;
  /// RLC SDU queue size.
  uint32_t queue_size = 4096;
};

/// RLC UM RX configuration
struct cu_cp_unit_rlc_rx_am_config {
  /// Number of bits used for sequence number.
  uint16_t sn_field_length;
  /// Timer used by rx to detect PDU loss (ms).
  int32_t t_reassembly;
  /// Timer used by rx to prohibit tx of status PDU (ms).
  int32_t t_status_prohibit;

  /// Implementation-specific parameters that are not specified by 3GPP

  /// Maximum number of visited SNs in the RX window when building a status report. 0 means no limit.
  uint32_t max_sn_per_status = 0;
};

/// RLC AM configuration
struct cu_cp_unit_rlc_am_config {
  cu_cp_unit_rlc_tx_am_config tx;
  cu_cp_unit_rlc_rx_am_config rx;
};

/// RLC configuration
struct cu_cp_unit_rlc_config {
  std::string              mode = "am";
  cu_cp_unit_rlc_um_config um;
  cu_cp_unit_rlc_am_config am;
};

struct cu_cp_unit_pdcp_rx_config {
  /// Number of bits used for sequence number.
  uint16_t sn_field_length;
  /// Timer used to detect PDUs losses (ms).
  int32_t t_reordering;
  /// Whether out-of-order delivery to upper layers is enabled.
  bool out_of_order_delivery;
};

struct cu_cp_unit_pdcp_tx_config {
  /// Number of bits used for sequence number.
  uint16_t sn_field_length;
  /// Timer used to notify lower layers to discard PDUs (ms).
  int32_t discard_timer;
  /// Whether PDCP status report is required.
  bool status_report_required;
};

struct cu_cp_unit_pdcp_config {
  cu_cp_unit_pdcp_tx_config tx;
  cu_cp_unit_pdcp_rx_config rx;
};

/// QoS configuration.
struct cu_cp_unit_qos_config {
  five_qi_t              five_qi = uint_to_five_qi(9);
  cu_cp_unit_rlc_config  rlc;
  cu_cp_unit_pdcp_config pdcp;
};

/// Configuration to enable/disable metrics per layer.
struct cu_cp_unit_metrics_layer_config {
  bool enable_ngap           = false;
  bool enable_pdcp           = false;
  bool enable_rrc            = false;
  bool enable_cu_cp_executor = false;

  /// Returns true if one or more layers are enabled, false otherwise.
  bool are_metrics_enabled() const { return enable_ngap || enable_pdcp || enable_rrc; }
};

/// Metrics configuration.
struct cu_cp_unit_metrics_config {
  /// CU-CP statistics report period in milliseconds.
  unsigned                        cu_cp_report_period = 1000;
  app_helpers::metrics_config     common_metrics_cfg;
  cu_cp_unit_metrics_layer_config layers_cfg;
};

struct cu_cp_unit_admission_watermark_config {
  /// Maximum accepted UE usage in percent.
  unsigned max_ue_usage = 100;
  /// Maximum accepted DRB usage in percent.
  unsigned max_drb_usage = 100;
};

/// CU-CP application unit configuration.
struct cu_cp_unit_config {
  /// Node name.
  std::string ran_node_name = "srscucp01";
  /// gNB identifier.
  gnb_id_t gnb_id = {411, 22};
  /// Maximum number of DUs.
  uint16_t max_nof_dus = 6;
  /// Maximum number of CU-UPs.
  uint16_t max_nof_cu_ups = 6;
  /// Maximum number of UEs.
  uint64_t max_nof_ues = 8192;
  /// Maximum number of DRBs per UE.
  uint8_t max_nof_drbs_per_ue = 8;
  /// Admission watermarks for initial accesses.
  cu_cp_unit_admission_watermark_config initial_access_admission = {};
  /// Admission watermarks for RRC reestablishments.
  cu_cp_unit_admission_watermark_config reestablishment_admission = {};
  /// Admission watermarks for handover target admissions.
  cu_cp_unit_admission_watermark_config handover_admission = {};
  /// Inactivity timer in seconds.
  int inactivity_timer = 120;
  /// PDU session request timeout in seconds (must be larger than T310).
  unsigned request_pdu_session_timeout = 3;
  /// Loggers configuration.
  cu_cp_unit_logger_config loggers;
  /// PCAPs configuration.
  cu_cp_unit_pcap_config pcap_cfg;
  /// Metrics configuration.
  cu_cp_unit_metrics_config metrics;
  /// AMF configuration.
  cu_cp_unit_amf_config amf_config;
  // List of all AMFs the CU-CP should connect to.
  std::vector<cu_cp_unit_amf_config_item> extra_amfs;
  /// Mobility configuration.
  cu_cp_unit_mobility_config mobility_config;
  /// RRC configuration.
  cu_cp_unit_rrc_config rrc_config;
  /// Security configuration.
  cu_cp_unit_security_config security_config;
  /// F1AP configuration.
  cu_cp_unit_f1ap_config f1ap_config;
  /// E1AP configuration.
  cu_cp_unit_e1ap_config e1ap_config;
  /// QoS configuration.
  std::vector<cu_cp_unit_qos_config> qos_cfg;
  /// Network slice configuration.
  std::vector<s_nssai_t> slice_cfg = {s_nssai_t{slice_service_type{1}}};
};

} // namespace srsran
