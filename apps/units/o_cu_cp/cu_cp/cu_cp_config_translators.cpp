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

#include "cu_cp_config_translators.h"
#include "apps/services/worker_manager/worker_manager_config.h"
#include "cu_cp_unit_config.h"
#include "srsran/cu_cp/cell_meas_manager_config.h"
#include "srsran/cu_cp/cu_cp_configuration_helpers.h"
#include "srsran/cu_cp/neighbor_cell_manager_config.h"
#include "srsran/ran/plmn_identity.h"
#include "srsran/rlc/rlc_config.h"
#include <sstream>

using namespace srsran;

static std::map<five_qi_t, srs_cu_cp::cu_cp_qos_config> generate_cu_cp_qos_config(const cu_cp_unit_config& config)
{
  std::map<five_qi_t, srs_cu_cp::cu_cp_qos_config> out_cfg = {};
  if (config.qos_cfg.empty()) {
    out_cfg = config_helpers::make_default_cu_cp_qos_config_list();
    return out_cfg;
  }

  for (const auto& qos : config.qos_cfg) {
    if (out_cfg.find(qos.five_qi) != out_cfg.end()) {
      report_error("Duplicate 5QI configuration: {}\n", qos.five_qi);
    }
    // Convert PDCP config
    pdcp_config& out_pdcp = out_cfg[qos.five_qi].pdcp;

    // RB type
    out_pdcp.rb_type = pdcp_rb_type::drb;

    // RLC mode
    rlc_mode mode = {};
    if (!from_string(mode, qos.rlc.mode)) {
      report_error("Invalid RLC mode: {}, mode={}\n", qos.five_qi, qos.rlc.mode);
    }
    if (mode == rlc_mode::um_bidir || mode == rlc_mode::um_unidir_ul || mode == rlc_mode::um_unidir_dl) {
      out_pdcp.rlc_mode = pdcp_rlc_mode::um;
    } else if (mode == rlc_mode::am) {
      out_pdcp.rlc_mode = pdcp_rlc_mode::am;
    } else {
      report_error("Invalid RLC mode: {}, mode={}\n", qos.five_qi, qos.rlc.mode);
    }

    out_pdcp.integrity_protection_required = false;
    out_pdcp.ciphering_required            = true;

    // > Tx
    // >> SN size
    if (!pdcp_sn_size_from_uint(out_pdcp.tx.sn_size, qos.pdcp.tx.sn_field_length)) {
      report_error("Invalid PDCP TX SN: {}, SN={}\n", qos.five_qi, qos.pdcp.tx.sn_field_length);
    }

    // >> discard timer
    out_pdcp.tx.discard_timer = pdcp_discard_timer{};
    if (!pdcp_discard_timer_from_int(out_pdcp.tx.discard_timer.value(), qos.pdcp.tx.discard_timer)) {
      report_error("Invalid PDCP discard timer. 5QI {} discard_timer {}\n", qos.five_qi, qos.pdcp.tx.discard_timer);
    }

    // >> status report required
    out_pdcp.tx.status_report_required = qos.pdcp.tx.status_report_required;

    // > Rx
    // >> SN size
    if (!pdcp_sn_size_from_uint(out_pdcp.rx.sn_size, qos.pdcp.rx.sn_field_length)) {
      report_error("Invalid PDCP RX SN: {}, SN={}\n", qos.five_qi, qos.pdcp.rx.sn_field_length);
    }

    // >> out of order delivery
    out_pdcp.rx.out_of_order_delivery = qos.pdcp.rx.out_of_order_delivery;

    // >> t-Reordering
    if (!pdcp_t_reordering_from_int(out_pdcp.rx.t_reordering, qos.pdcp.rx.t_reordering)) {
      report_error("Invalid PDCP t-Reordering. {} t-Reordering {}\n", qos.five_qi, qos.pdcp.rx.t_reordering);
    }
  }
  return out_cfg;
}

static security::preferred_integrity_algorithms
generate_preferred_integrity_algorithms_list(const cu_cp_unit_config& config)
{
  // String splitter helper
  auto split = [](const std::string& s, char delim) -> std::vector<std::string> {
    std::vector<std::string> result;
    std::stringstream        ss(s);
    for (std::string item; getline(ss, item, delim);) {
      result.push_back(item);
    }
    return result;
  };

  // > Remove spaces, convert to lower case and split on comma
  std::string nia_preference_list = config.security_config.nia_preference_list;
  nia_preference_list.erase(std::remove_if(nia_preference_list.begin(), nia_preference_list.end(), ::isspace),
                            nia_preference_list.end());
  std::transform(nia_preference_list.begin(),
                 nia_preference_list.end(),
                 nia_preference_list.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  std::vector<std::string> nea_v = split(nia_preference_list, ',');

  security::preferred_integrity_algorithms algo_list = {};
  int                                      idx       = 0;
  for (const std::string& nea : nea_v) {
    if (nea == "nia0") {
      algo_list[idx] = security::integrity_algorithm::nia0;
    } else if (nea == "nia1") {
      algo_list[idx] = security::integrity_algorithm::nia1;
    } else if (nea == "nia2") {
      algo_list[idx] = security::integrity_algorithm::nia2;
    } else if (nea == "nia3") {
      algo_list[idx] = security::integrity_algorithm::nia3;
    }
    idx++;
  }
  return algo_list;
}

static security::preferred_ciphering_algorithms
generate_preferred_ciphering_algorithms_list(const cu_cp_unit_config& config)
{
  // String splitter helper
  auto split = [](const std::string& s, char delim) -> std::vector<std::string> {
    std::vector<std::string> result;
    std::stringstream        ss(s);
    for (std::string item; getline(ss, item, delim);) {
      result.push_back(item);
    }
    return result;
  };

  // > Remove spaces, convert to lower case and split on comma
  std::string nea_preference_list = config.security_config.nea_preference_list;
  nea_preference_list.erase(std::remove_if(nea_preference_list.begin(), nea_preference_list.end(), ::isspace),
                            nea_preference_list.end());
  std::transform(nea_preference_list.begin(),
                 nea_preference_list.end(),
                 nea_preference_list.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  std::vector<std::string> nea_v = split(nea_preference_list, ',');

  security::preferred_ciphering_algorithms algo_list = {};
  int                                      idx       = 0;
  for (const std::string& nea : nea_v) {
    if (nea == "nea0") {
      algo_list[idx] = security::ciphering_algorithm::nea0;
    } else if (nea == "nea1") {
      algo_list[idx] = security::ciphering_algorithm::nea1;
    } else if (nea == "nea2") {
      algo_list[idx] = security::ciphering_algorithm::nea2;
    } else if (nea == "nea3") {
      algo_list[idx] = security::ciphering_algorithm::nea3;
    }
    idx++;
  }
  return algo_list;
}

static srs_cu_cp::rrc_ssb_mtc generate_rrc_ssb_mtc(unsigned period, unsigned offset, unsigned duration)
{
  srs_cu_cp::rrc_ssb_mtc ssb_mtc;
  ssb_mtc.periodicity_and_offset.periodicity = (srs_cu_cp::rrc_periodicity_and_offset::periodicity_t)period;
  ssb_mtc.periodicity_and_offset.offset      = offset;
  ssb_mtc.dur                                = duration;

  return ssb_mtc;
}

static srs_cu_cp::rrc_periodical_report_cfg
generate_cu_cp_periodical_report_config(const cu_cp_unit_report_config& report_cfg_item)
{
  srs_cu_cp::rrc_periodical_report_cfg periodical;

  periodical.rs_type                = srs_cu_cp::rrc_nr_rs_type::ssb;
  periodical.report_interv          = report_cfg_item.report_interval_ms;
  periodical.report_amount          = -1;
  periodical.report_quant_cell.rsrp = true;
  periodical.report_quant_cell.rsrq = true;
  periodical.report_quant_cell.sinr = true;
  periodical.max_report_cells       = 4;

  srs_cu_cp::rrc_meas_report_quant report_quant_rs_idxes;
  report_quant_rs_idxes.rsrp       = true;
  report_quant_rs_idxes.rsrq       = true;
  report_quant_rs_idxes.sinr       = true;
  periodical.report_quant_rs_idxes = report_quant_rs_idxes;

  periodical.max_nrof_rs_idxes_to_report = 4;
  periodical.include_beam_meass          = true;
  periodical.use_allowed_cell_list       = false;

  periodical.periodic_ho_rsrp_offset = report_cfg_item.periodic_ho_rsrp_offset;

  return periodical;
}

static srs_cu_cp::rrc_event_trigger_cfg
generate_cu_cp_event_trigger_report_config(const cu_cp_unit_report_config& report_cfg_item)
{
  srs_cu_cp::rrc_event_trigger_cfg event_trigger_cfg;

  {
    srs_cu_cp::rrc_event_id event_id;

    if (report_cfg_item.event_triggered_report_type.value() == "a1") {
      event_id.id = srs_cu_cp::rrc_event_id::event_id_t::a1;
    }
    if (report_cfg_item.event_triggered_report_type.value() == "a2") {
      event_id.id = srs_cu_cp::rrc_event_id::event_id_t::a2;
    }
    if (report_cfg_item.event_triggered_report_type.value() == "a3") {
      event_id.id = srs_cu_cp::rrc_event_id::event_id_t::a3;
    }
    if (report_cfg_item.event_triggered_report_type.value() == "a4") {
      event_id.id = srs_cu_cp::rrc_event_id::event_id_t::a4;
    }
    if (report_cfg_item.event_triggered_report_type.value() == "a5") {
      event_id.id = srs_cu_cp::rrc_event_id::event_id_t::a5;
    }
    if (report_cfg_item.event_triggered_report_type.value() == "a6") {
      event_id.id = srs_cu_cp::rrc_event_id::event_id_t::a6;
    }

    event_id.meas_trigger_quant_thres_or_offset.emplace();

    // Event id
    if (report_cfg_item.event_triggered_report_type.value() == "a1" or
        report_cfg_item.event_triggered_report_type.value() == "a2" or
        report_cfg_item.event_triggered_report_type.value() == "a4" or
        report_cfg_item.event_triggered_report_type.value() == "a5") {
      if (report_cfg_item.event_triggered_report_type.value() == "a5") {
        event_id.meas_trigger_quant_thres_2.emplace();
      }
      // Meas trigger quantity threshold
      if (report_cfg_item.meas_trigger_quantity.value() == "rsrp") {
        event_id.meas_trigger_quant_thres_or_offset.value().rsrp =
            report_cfg_item.meas_trigger_quantity_threshold_db.value();
        if (report_cfg_item.event_triggered_report_type.value() == "a5") {
          event_id.meas_trigger_quant_thres_2.value().rsrp =
              report_cfg_item.meas_trigger_quantity_threshold_2_db.value();
        }
      } else if (report_cfg_item.meas_trigger_quantity.value() == "rsrq") {
        event_id.meas_trigger_quant_thres_or_offset.value().rsrq =
            report_cfg_item.meas_trigger_quantity_threshold_db.value();
        if (report_cfg_item.event_triggered_report_type.value() == "a5") {
          event_id.meas_trigger_quant_thres_2.value().rsrq =
              report_cfg_item.meas_trigger_quantity_threshold_2_db.value();
        }
      } else if (report_cfg_item.meas_trigger_quantity.value() == "sinr") {
        event_id.meas_trigger_quant_thres_or_offset.value().sinr =
            report_cfg_item.meas_trigger_quantity_threshold_db.value();
        if (report_cfg_item.event_triggered_report_type.value() == "a5") {
          event_id.meas_trigger_quant_thres_2.value().sinr =
              report_cfg_item.meas_trigger_quantity_threshold_2_db.value();
        }
      }
    }

    if (report_cfg_item.event_triggered_report_type.value() == "a3" or
        report_cfg_item.event_triggered_report_type.value() == "a6") {
      // Meas trigger quantity offset
      if (report_cfg_item.meas_trigger_quantity.value() == "rsrp") {
        event_id.meas_trigger_quant_thres_or_offset.value().rsrp =
            report_cfg_item.meas_trigger_quantity_offset_db.value();
      } else if (report_cfg_item.meas_trigger_quantity.value() == "rsrq") {
        event_id.meas_trigger_quant_thres_or_offset.value().rsrq =
            report_cfg_item.meas_trigger_quantity_offset_db.value();
      } else if (report_cfg_item.meas_trigger_quantity.value() == "sinr") {
        event_id.meas_trigger_quant_thres_or_offset.value().sinr =
            report_cfg_item.meas_trigger_quantity_offset_db.value();
      }
    }

    if (report_cfg_item.event_triggered_report_type.value() == "a3" or
        report_cfg_item.event_triggered_report_type.value() == "a4" or
        report_cfg_item.event_triggered_report_type.value() == "a5" or
        report_cfg_item.event_triggered_report_type.value() == "a6") {
      // Report on leave
      event_id.use_allowed_cell_list = false;
    }

    // Common parameters

    // Report on leave
    event_id.report_on_leave = false;

    // Hysteresis
    event_id.hysteresis = report_cfg_item.hysteresis_db.value();

    // Time to trigger
    event_id.time_to_trigger = report_cfg_item.time_to_trigger_ms.value();

    event_trigger_cfg.event_id = event_id;
  }

  event_trigger_cfg.rs_type                = srs_cu_cp::rrc_nr_rs_type::ssb;
  event_trigger_cfg.report_interv          = report_cfg_item.report_interval_ms;
  event_trigger_cfg.report_amount          = -1;
  event_trigger_cfg.report_quant_cell.rsrp = true;
  event_trigger_cfg.report_quant_cell.rsrq = true;
  event_trigger_cfg.report_quant_cell.sinr = true;
  event_trigger_cfg.max_report_cells       = 4;

  srs_cu_cp::rrc_meas_report_quant report_quant_rs_idxes;
  report_quant_rs_idxes.rsrp              = true;
  report_quant_rs_idxes.rsrq              = true;
  report_quant_rs_idxes.sinr              = true;
  event_trigger_cfg.report_quant_rs_idxes = report_quant_rs_idxes;

  return event_trigger_cfg;
}

static srs_cu_cp::ntn_location_mobility_config
generate_ntn_location_mobility_config(const cu_cp_unit_ntn_location_mobility_config& app_cfg)
{
  srs_cu_cp::ntn_location_mobility_config out_cfg;
  out_cfg.enabled                               = app_cfg.enabled;
  out_cfg.served_beam_min_elevation_deg         = app_cfg.served_beam_min_elevation_deg;
  out_cfg.max_nof_served_beams                  = app_cfg.max_nof_served_beams;
  out_cfg.max_nof_active_analog_access_beams    = app_cfg.max_nof_active_analog_access_beams;
  out_cfg.max_nof_loaded_digital_service_beams  = app_cfg.max_nof_loaded_digital_service_beams;
  out_cfg.preheated_beam_hold_time              = std::chrono::milliseconds{app_cfg.preheated_beam_hold_time_ms};
  out_cfg.preheated_beam_min_ready_time = std::chrono::milliseconds{app_cfg.preheated_beam_min_ready_time_ms};
  out_cfg.analog_rebalance_pair_cooldown = std::chrono::milliseconds{app_cfg.analog_rebalance_pair_cooldown_ms};
  out_cfg.digital_target_reservation_hold_time =
      std::chrono::milliseconds{app_cfg.digital_target_reservation_hold_time_ms};
  out_cfg.served_beam_hopping_enabled           = app_cfg.served_beam_hopping_enabled;
  out_cfg.served_beam_hopping_dwell_updates     = app_cfg.served_beam_hopping_dwell_updates;
  out_cfg.multi_beam_load_balancing_enabled     = app_cfg.multi_beam_load_balancing_enabled;
  out_cfg.demand_aware_beam_scheduling_enabled  = app_cfg.demand_aware_beam_scheduling_enabled;
  out_cfg.multi_beam_headroom_admission_enabled = app_cfg.multi_beam_headroom_admission_enabled;
  out_cfg.multi_beam_load_balancing_min_ue_delta = app_cfg.multi_beam_load_balancing_min_ue_delta;
  out_cfg.multi_beam_load_balancing_max_handovers_per_eval =
      app_cfg.multi_beam_load_balancing_max_handovers_per_eval;
  out_cfg.multi_beam_load_balancing_handover_cooldown =
      std::chrono::milliseconds{app_cfg.multi_beam_load_balancing_handover_cooldown_ms};
  out_cfg.measurement_report_period            = std::chrono::milliseconds{app_cfg.measurement_report_period_ms};
  out_cfg.time_to_trigger                      = std::chrono::milliseconds{app_cfg.time_to_trigger_ms};
  out_cfg.max_report_gap                       = std::chrono::milliseconds{app_cfg.max_report_gap_ms};
  out_cfg.location_max_age                     = std::chrono::milliseconds{app_cfg.location_max_age_ms};
  out_cfg.location_lost_release_grace_period =
      std::chrono::milliseconds{app_cfg.location_lost_release_grace_period_ms};
  out_cfg.idle_paging_context_max_age         = std::chrono::milliseconds{app_cfg.idle_paging_context_max_age_ms};
  out_cfg.handover_retry_timeout               = std::chrono::milliseconds{app_cfg.handover_retry_timeout_ms};
  out_cfg.required_consecutive_location_reports = app_cfg.required_consecutive_location_reports;
  out_cfg.boundary_hysteresis_m                = app_cfg.boundary_hysteresis_m;
  out_cfg.max_horizontal_accuracy_m            = app_cfg.max_horizontal_accuracy_m;
  out_cfg.core_network_reporting.local_forwarding_enabled =
      app_cfg.core_network_reporting_local_forwarding_enabled;
  out_cfg.core_network_reporting.amf_control_enabled = app_cfg.core_network_reporting_amf_control_enabled;
  out_cfg.core_network_reporting.min_report_interval =
      std::chrono::milliseconds{app_cfg.core_network_reporting_min_report_interval_ms};
  auto& sat_state_cfg                           = out_cfg.satellite_state_update;
  if (app_cfg.satellite_state_source == "manual") {
    sat_state_cfg.source = srs_cu_cp::ntn_satellite_state_source::manual;
  } else if (app_cfg.satellite_state_source == "circular_orbit") {
    sat_state_cfg.source = srs_cu_cp::ntn_satellite_state_source::circular_orbit;
  } else if (app_cfg.satellite_state_source == "tle") {
    sat_state_cfg.source = srs_cu_cp::ntn_satellite_state_source::tle;
  } else {
    report_error("Invalid NTN satellite_state_source '{}'.\n", app_cfg.satellite_state_source);
  }
  sat_state_cfg.update_period = std::chrono::milliseconds{app_cfg.satellite_state_update_period_ms};
  sat_state_cfg.predictive_service_window_horizon =
      std::chrono::milliseconds{app_cfg.predictive_service_window_horizon_ms};
  sat_state_cfg.predictive_handover_lead_time = std::chrono::milliseconds{app_cfg.predictive_handover_lead_time_ms};
  sat_state_cfg.circular_altitude_m               = app_cfg.circular_orbit_altitude_m;
  sat_state_cfg.circular_inclination_deg          = app_cfg.circular_orbit_inclination_deg;
  sat_state_cfg.circular_raan_deg                 = app_cfg.circular_orbit_raan_deg;
  sat_state_cfg.circular_argument_of_latitude_deg = app_cfg.circular_orbit_argument_of_latitude_deg;
  if (app_cfg.circular_orbit_epoch_unix_s.has_value()) {
    sat_state_cfg.circular_epoch = std::chrono::system_clock::time_point{
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::duration<double>{app_cfg.circular_orbit_epoch_unix_s.value()})};
  }
  for (const cu_cp_unit_ntn_circular_orbit_satellite_config& satellite : app_cfg.circular_orbit_satellites) {
    srs_cu_cp::ntn_circular_orbit_satellite_config satellite_cfg;
    satellite_cfg.satellite_id                     = satellite.satellite_id;
    satellite_cfg.altitude_m                       = satellite.altitude_m;
    satellite_cfg.inclination_deg                  = satellite.inclination_deg;
    satellite_cfg.raan_deg                         = satellite.raan_deg;
    satellite_cfg.argument_of_latitude_deg         = satellite.argument_of_latitude_deg;
    if (satellite.epoch_unix_s.has_value()) {
      satellite_cfg.epoch = std::chrono::system_clock::time_point{
          std::chrono::duration_cast<std::chrono::system_clock::duration>(
              std::chrono::duration<double>{satellite.epoch_unix_s.value()})};
    }
    sat_state_cfg.circular_orbit_satellites.push_back(std::move(satellite_cfg));
  }
  sat_state_cfg.tle_satellite_name = app_cfg.tle_satellite_name;
  sat_state_cfg.tle_line1          = app_cfg.tle_line1;
  sat_state_cfg.tle_line2          = app_cfg.tle_line2;

  if (!app_cfg.beam_table_json_file.empty()) {
    auto beam_table = srs_cu_cp::load_ntn_beam_table_json_file(app_cfg.beam_table_json_file);
    if (!beam_table.has_value()) {
      report_error("Invalid NTN beam table file '{}'. Cause: {}\n", app_cfg.beam_table_json_file, beam_table.error());
    }
    auto beam_table_value = std::move(beam_table.value());
    out_cfg.analog_beams = std::move(beam_table_value.analog_beams);
    out_cfg.beams        = std::move(beam_table_value.beams);
  }

  return out_cfg;
}

static void apply_neighbor_cell_info_config(const cu_cp_unit_mobility_config& app_cfg,
                                            srs_cu_cp::cell_meas_manager_cfg& meas_cfg)
{
  if (app_cfg.neighbor_cell_info_json_file.empty()) {
    return;
  }

  auto neighbor_cfg = srs_cu_cp::load_neighbor_cell_info_json_file(app_cfg.neighbor_cell_info_json_file);
  if (!neighbor_cfg.has_value()) {
    report_error("Invalid neighbor cell info file '{}'. Cause: {}\n",
                 app_cfg.neighbor_cell_info_json_file,
                 neighbor_cfg.error());
  }

  srs_cu_cp::neighbor_cell_manager neighbor_mng(neighbor_cfg.value());
  if (auto merge_error = neighbor_mng.merge_into(meas_cfg); merge_error.has_value()) {
    report_error("Invalid neighbor cell info file '{}'. Cause: {}\n",
                 app_cfg.neighbor_cell_info_json_file,
                 merge_error.value());
  }
}

srs_cu_cp::cu_cp_configuration srsran::generate_cu_cp_config(const cu_cp_unit_config& cu_cfg)
{
  srs_cu_cp::cu_cp_configuration out_cfg = config_helpers::make_default_cu_cp_config();
  out_cfg.admission.max_nof_dus          = cu_cfg.max_nof_dus;
  out_cfg.admission.max_nof_cu_ups       = cu_cfg.max_nof_cu_ups;
  out_cfg.admission.max_nof_ues          = cu_cfg.max_nof_ues;
  out_cfg.admission.max_nof_drbs_per_ue  = cu_cfg.max_nof_drbs_per_ue;
  out_cfg.admission.initial_access_watermark.max_ue_usage   = cu_cfg.initial_access_admission.max_ue_usage;
  out_cfg.admission.initial_access_watermark.max_drb_usage  = cu_cfg.initial_access_admission.max_drb_usage;
  out_cfg.admission.reestablishment_watermark.max_ue_usage  = cu_cfg.reestablishment_admission.max_ue_usage;
  out_cfg.admission.reestablishment_watermark.max_drb_usage = cu_cfg.reestablishment_admission.max_drb_usage;
  out_cfg.admission.handover_watermark.max_ue_usage         = cu_cfg.handover_admission.max_ue_usage;
  out_cfg.admission.handover_watermark.max_drb_usage        = cu_cfg.handover_admission.max_drb_usage;

  out_cfg.node.gnb_id        = cu_cfg.gnb_id;
  out_cfg.node.ran_node_name = cu_cfg.ran_node_name;

  out_cfg.ngap.amf_reconnection_retry_time = std::chrono::milliseconds{cu_cfg.amf_config.amf_reconnection_retry_time};
  out_cfg.ngap.no_core                     = cu_cfg.amf_config.no_core;

  {
    std::vector<srs_cu_cp::supported_tracking_area> supported_tas;
    for (const auto& supported_ta : cu_cfg.amf_config.amf.supported_tas) {
      std::vector<srs_cu_cp::plmn_item> plmn_list;
      for (const auto& plmn_item : supported_ta.plmn_list) {
        expected<plmn_identity> plmn = plmn_identity::parse(plmn_item.plmn_id);
        srsran_assert(plmn.has_value(), "Invalid PLMN: {}", plmn_item.plmn_id);
        plmn_list.push_back({plmn.value(), {}});
        for (const auto& elem : plmn_item.tai_slice_support_list) {
          plmn_list.back().slice_support_list.push_back(
              s_nssai_t{slice_service_type{elem.sst}, slice_differentiator::create(elem.sd).value()});
        }
      }
      supported_tas.push_back({supported_ta.tac, plmn_list});
    }
    out_cfg.ngap.ngaps.push_back(srs_cu_cp::cu_cp_configuration::ngap_config{nullptr, supported_tas});
  }

#ifndef SRSRAN_HAS_ENTERPRISE
  for (const auto& cfg : cu_cfg.extra_amfs) {
    std::vector<srs_cu_cp::supported_tracking_area> supported_tas;
    for (const auto& supported_ta : cfg.supported_tas) {
      std::vector<srs_cu_cp::plmn_item> plmn_list;
      for (const auto& plmn_item : supported_ta.plmn_list) {
        expected<plmn_identity> plmn = plmn_identity::parse(plmn_item.plmn_id);
        srsran_assert(plmn.has_value(), "Invalid PLMN: {}", plmn_item.plmn_id);
        plmn_list.push_back({plmn.value(), {}});
        for (const auto& elem : plmn_item.tai_slice_support_list) {
          plmn_list.back().slice_support_list.push_back(
              s_nssai_t{slice_service_type{elem.sst}, slice_differentiator::create(elem.sd).value()});
        }
      }
      supported_tas.push_back({supported_ta.tac, plmn_list});
    }
    out_cfg.ngap.ngaps.push_back(srs_cu_cp::cu_cp_configuration::ngap_config{nullptr, supported_tas});
  }
#else
  if (!cu_cfg.extra_amfs.empty()) {
    report_error("Invalid CU-CP configuration. \"extra_amfs\" parameter is only supported in Enterprise version.\n");
  }
#endif // SRSRAN_HAS_ENTERPRISE

  out_cfg.rrc.force_reestablishment_fallback = cu_cfg.rrc_config.force_reestablishment_fallback;
  out_cfg.rrc.rrc_procedure_guard_time_ms    = std::chrono::milliseconds{cu_cfg.rrc_config.rrc_procedure_guard_time_ms};

  out_cfg.bearers.drb_config = generate_cu_cp_qos_config(cu_cfg);

  out_cfg.security.int_algo_pref_list = generate_preferred_integrity_algorithms_list(cu_cfg);
  out_cfg.security.enc_algo_pref_list = generate_preferred_ciphering_algorithms_list(cu_cfg);
  if (!from_string(out_cfg.security.default_security_indication.integrity_protection_ind,
                   cu_cfg.security_config.integrity_protection)) {
    report_error("Invalid value for integrity_protection={}.\n", cu_cfg.security_config.integrity_protection);
  }
  if (!from_string(out_cfg.security.default_security_indication.confidentiality_protection_ind,
                   cu_cfg.security_config.confidentiality_protection)) {
    report_error("Invalid value for confidentiality_protection={}.\n",
                 cu_cfg.security_config.confidentiality_protection);
  }

  // Timers
  out_cfg.ue.inactivity_timer              = std::chrono::seconds{cu_cfg.inactivity_timer};
  out_cfg.ue.request_pdu_session_timeout   = std::chrono::seconds{cu_cfg.request_pdu_session_timeout};
  out_cfg.metrics.statistics_report_period = std::chrono::seconds{cu_cfg.metrics.cu_cp_report_period};

  // Metrics
  out_cfg.metrics.layers_cfg.enable_ngap = cu_cfg.metrics.layers_cfg.enable_ngap;
  out_cfg.metrics.layers_cfg.enable_rrc  = cu_cfg.metrics.layers_cfg.enable_rrc;

  // Mobility
  out_cfg.mobility.mobility_manager_config.trigger_handover_from_measurements =
      cu_cfg.mobility_config.trigger_handover_from_measurements;
  out_cfg.mobility.mobility_manager_config.enable_ngap_metrics = cu_cfg.metrics.layers_cfg.enable_ngap;
  out_cfg.mobility.mobility_manager_config.enable_rrc_metrics  = cu_cfg.metrics.layers_cfg.enable_rrc;
  out_cfg.mobility.meas_manager_config.ntn_location_mobility =
      generate_ntn_location_mobility_config(cu_cfg.mobility_config.ntn_location_mobility);
  out_cfg.mobility.onboard_position_plan.enabled = cu_cfg.mobility_config.ntn_onboard_position_plan.enabled;
  out_cfg.mobility.onboard_position_plan.du_execution_enabled =
      cu_cfg.mobility_config.ntn_onboard_position_plan.du_execution_enabled;
  out_cfg.mobility.onboard_position_plan.satellite_id =
      cu_cfg.mobility_config.ntn_onboard_position_plan.satellite_id;
  out_cfg.mobility.onboard_position_plan.plan_json_file =
      cu_cfg.mobility_config.ntn_onboard_position_plan.plan_json_file;
  out_cfg.mobility.onboard_position_plan.expected_catalog_id =
      cu_cfg.mobility_config.ntn_onboard_position_plan.expected_catalog_id;
  out_cfg.mobility.onboard_position_plan.expected_catalog_hash =
      cu_cfg.mobility_config.ntn_onboard_position_plan.expected_catalog_hash;
  out_cfg.mobility.onboard_position_plan.expected_identity_registry_version =
      cu_cfg.mobility_config.ntn_onboard_position_plan.expected_identity_registry_version;
  out_cfg.mobility.onboard_position_plan.expected_identity_registry_hash =
      cu_cfg.mobility_config.ntn_onboard_position_plan.expected_identity_registry_hash;
  out_cfg.mobility.onboard_position_plan.expected_access_profile_id =
      cu_cfg.mobility_config.ntn_onboard_position_plan.expected_access_profile_id;
  out_cfg.mobility.onboard_position_plan.expected_access_profile_hash =
      cu_cfg.mobility_config.ntn_onboard_position_plan.expected_access_profile_hash;
  out_cfg.mobility.onboard_position_plan.reload_period =
      std::chrono::milliseconds{cu_cfg.mobility_config.ntn_onboard_position_plan.reload_period_ms};
  out_cfg.mobility.onboard_position_plan.du_prepare_guard =
      std::chrono::milliseconds{cu_cfg.mobility_config.ntn_onboard_position_plan.du_prepare_guard_ms};
  out_cfg.mobility.onboard_position_plan.du_prepare_horizon =
      std::chrono::milliseconds{cu_cfg.mobility_config.ntn_onboard_position_plan.du_prepare_horizon_ms};
  out_cfg.mobility.onboard_position_plan.du_apply_timeout =
      std::chrono::milliseconds{cu_cfg.mobility_config.ntn_onboard_position_plan.du_apply_timeout_ms};
  if (cu_cfg.mobility_config.ntn_onboard_position_plan.cell_ncis.size() == 2 &&
      cu_cfg.mobility_config.ntn_onboard_position_plan.cell_pcis.size() == 2) {
    for (unsigned i = 0; i != out_cfg.mobility.onboard_position_plan.cell_ncis.size(); ++i) {
      out_cfg.mobility.onboard_position_plan.cell_ncis[i] =
          nr_cell_identity::create(cu_cfg.mobility_config.ntn_onboard_position_plan.cell_ncis[i]).value();
      out_cfg.mobility.onboard_position_plan.cell_pcis[i] =
          static_cast<pci_t>(cu_cfg.mobility_config.ntn_onboard_position_plan.cell_pcis[i]);
    }
  }
  out_cfg.mobility.onboard_position_plan.max_l1_positions_per_cell =
      cu_cfg.mobility_config.ntn_onboard_position_plan.max_l1_positions_per_cell;
  out_cfg.mobility.onboard_position_plan.max_l1_positions_per_satellite =
      cu_cfg.mobility_config.ntn_onboard_position_plan.max_l1_positions_per_satellite;
  out_cfg.mobility.onboard_position_plan.max_analog_ports_per_cell =
      cu_cfg.mobility_config.ntn_onboard_position_plan.max_analog_ports_per_cell;
  out_cfg.mobility.onboard_position_plan.max_analog_ports_per_satellite =
      cu_cfg.mobility_config.ntn_onboard_position_plan.max_analog_ports_per_satellite;
  out_cfg.mobility.onboard_position_plan.max_digital_ports_per_cell =
      cu_cfg.mobility_config.ntn_onboard_position_plan.max_digital_ports_per_cell;
  out_cfg.mobility.onboard_position_plan.max_digital_ports_per_satellite =
      cu_cfg.mobility_config.ntn_onboard_position_plan.max_digital_ports_per_satellite;
  out_cfg.mobility.onboard_position_plan.access_slot =
      std::chrono::microseconds{cu_cfg.mobility_config.ntn_onboard_position_plan.access_slot_us};
  out_cfg.mobility.onboard_position_plan.subvisit_duration =
      std::chrono::microseconds{cu_cfg.mobility_config.ntn_onboard_position_plan.subvisit_duration_us};
  out_cfg.mobility.onboard_position_plan.max_ssb_interval =
      std::chrono::milliseconds{cu_cfg.mobility_config.ntn_onboard_position_plan.max_ssb_interval_ms};
  out_cfg.mobility.onboard_position_plan.max_prach_interval =
      std::chrono::milliseconds{cu_cfg.mobility_config.ntn_onboard_position_plan.max_prach_interval_ms};
  out_cfg.mobility.onboard_position_plan.activation_alignment =
      std::chrono::milliseconds{cu_cfg.mobility_config.ntn_onboard_position_plan.activation_alignment_ms};

  // F1AP-CU config.
  out_cfg.f1ap.proc_timeout     = std::chrono::milliseconds{cu_cfg.f1ap_config.procedure_timeout};
  out_cfg.f1ap.json_log_enabled = cu_cfg.loggers.f1ap_json_enabled;

  // E1AP-CU-CP config.
  out_cfg.e1ap.proc_timeout     = std::chrono::milliseconds{cu_cfg.e1ap_config.procedure_timeout};
  out_cfg.e1ap.json_log_enabled = cu_cfg.loggers.e1ap_json_enabled;

  // Convert appconfig's cell list into cell manager type.
  for (const auto& app_cfg_item : cu_cfg.mobility_config.cells) {
    nr_cell_identity            nci = nr_cell_identity::create(app_cfg_item.nr_cell_id).value();
    srs_cu_cp::cell_meas_config meas_cfg_item;
    meas_cfg_item.serving_cell_cfg.nci = nci;
    if (app_cfg_item.periodic_report_cfg_id.has_value()) {
      meas_cfg_item.periodic_report_cfg_id =
          srs_cu_cp::uint_to_report_cfg_id(app_cfg_item.periodic_report_cfg_id.value());
    }

    meas_cfg_item.serving_cell_cfg.gnb_id_bit_length = app_cfg_item.gnb_id_bit_length.value();
    meas_cfg_item.serving_cell_cfg.pci               = app_cfg_item.pci;
    meas_cfg_item.serving_cell_cfg.band              = app_cfg_item.band;
    meas_cfg_item.serving_cell_cfg.ssb_arfcn         = app_cfg_item.ssb_arfcn;
    if (app_cfg_item.ssb_scs.has_value()) {
      meas_cfg_item.serving_cell_cfg.ssb_scs.emplace() =
          to_subcarrier_spacing(std::to_string(app_cfg_item.ssb_scs.value()));
    }
    if (app_cfg_item.ssb_duration.has_value() && app_cfg_item.ssb_offset.has_value() &&
        app_cfg_item.ssb_period.has_value()) {
      // Add MTC config.
      meas_cfg_item.serving_cell_cfg.ssb_mtc.emplace() = generate_rrc_ssb_mtc(
          app_cfg_item.ssb_period.value(), app_cfg_item.ssb_offset.value(), app_cfg_item.ssb_duration.value());
    }

    for (const auto& ncell : app_cfg_item.ncells) {
      srs_cu_cp::neighbor_cell_meas_config ncell_meas_cfg;
      ncell_meas_cfg.nci = nr_cell_identity::create(ncell.nr_cell_id).value();
      for (const auto& report_id : ncell.report_cfg_ids) {
        ncell_meas_cfg.report_cfg_ids.push_back(srs_cu_cp::uint_to_report_cfg_id(report_id));
      }

      meas_cfg_item.ncells.push_back(ncell_meas_cfg);
    }

    // Store config.
    out_cfg.mobility.meas_manager_config.cells[meas_cfg_item.serving_cell_cfg.nci] = meas_cfg_item;
  }

  // Convert report config.
  for (const auto& report_cfg_item : cu_cfg.mobility_config.report_configs) {
    srs_cu_cp::rrc_report_cfg_nr report_cfg;

    if (report_cfg_item.report_type == "periodical") {
      report_cfg = generate_cu_cp_periodical_report_config(report_cfg_item);
    } else {
      report_cfg = generate_cu_cp_event_trigger_report_config(report_cfg_item);
    }

    // Store config.
    out_cfg.mobility.meas_manager_config
        .report_config_ids[srs_cu_cp::uint_to_report_cfg_id(report_cfg_item.report_cfg_id)] = report_cfg;
  }

  apply_neighbor_cell_info_config(cu_cfg.mobility_config, out_cfg.mobility.meas_manager_config);

  if (!config_helpers::is_valid_configuration(out_cfg)) {
    report_error("Invalid CU-CP configuration.\n");
  }

  return out_cfg;
}

srs_cu_cp::n2_connection_client_config srsran::generate_n2_client_config(bool                              no_core,
                                                                         const cu_cp_unit_amf_config_item& amf_cfg,
                                                                         dlt_pcap&                         pcap_writer,
                                                                         io_broker&                        broker,
                                                                         task_executor& io_rx_executor)
{
  using no_core_mode_t = srs_cu_cp::n2_connection_client_config::no_core;
  using network_mode_t = srs_cu_cp::n2_connection_client_config::network;
  using ngap_mode_t    = std::variant<no_core_mode_t, network_mode_t>;

  ngap_mode_t mode = no_core ? ngap_mode_t{no_core_mode_t{}} : ngap_mode_t{network_mode_t{broker, io_rx_executor}};
  if (not no_core) {
    network_mode_t& nw_mode = std::get<network_mode_t>(mode);
    nw_mode.amf_address     = amf_cfg.ip_addr;
    nw_mode.amf_port        = amf_cfg.port;
    nw_mode.bind_address    = amf_cfg.bind_addr;
    nw_mode.bind_interface  = amf_cfg.bind_interface;
    if (amf_cfg.sctp_rto_initial_ms >= 0) {
      nw_mode.rto_initial = std::chrono::milliseconds{amf_cfg.sctp_rto_initial_ms};
    }
    if (amf_cfg.sctp_rto_min_ms >= 0) {
      nw_mode.rto_min = std::chrono::milliseconds{amf_cfg.sctp_rto_min_ms};
    }
    if (amf_cfg.sctp_rto_max_ms >= 0) {
      nw_mode.rto_max = std::chrono::milliseconds{amf_cfg.sctp_rto_max_ms};
    }
    if (amf_cfg.sctp_init_max_attempts >= 0) {
      nw_mode.init_max_attempts = amf_cfg.sctp_init_max_attempts;
    }
    if (amf_cfg.sctp_max_init_timeo_ms >= 0) {
      nw_mode.max_init_timeo = std::chrono::milliseconds{amf_cfg.sctp_max_init_timeo_ms};
    }
    if (amf_cfg.sctp_hb_interval_ms >= 0) {
      nw_mode.hb_interval = std::chrono::milliseconds{amf_cfg.sctp_hb_interval_ms};
    }
    if (amf_cfg.sctp_assoc_max_retx >= 0) {
      nw_mode.assoc_max_rxt = amf_cfg.sctp_assoc_max_retx;
    }
    nw_mode.nodelay = amf_cfg.sctp_nodelay;
  }

  return srs_cu_cp::n2_connection_client_config{pcap_writer, mode};
}

void srsran::fill_cu_cp_worker_manager_config(worker_manager_config& config, const cu_cp_unit_config& unit_cfg)
{
  // CU-CP executors are needed.
  config.cu_cp_cfg.emplace();

  // Enable PCAPs.
  auto& pcap_cfg = config.pcap_cfg;
  if (unit_cfg.pcap_cfg.e1ap.enabled) {
    pcap_cfg.is_e1ap_enabled = true;
  }
  if (unit_cfg.pcap_cfg.f1ap.enabled) {
    pcap_cfg.is_f1ap_enabled = true;
  }
  if (unit_cfg.pcap_cfg.ngap.enabled) {
    pcap_cfg.is_ngap_enabled = true;
  }
}
