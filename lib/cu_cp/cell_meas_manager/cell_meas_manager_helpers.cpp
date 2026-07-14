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

#include "cell_meas_manager_helpers.h"
#include "srsran/srslog/srslog.h"
#include <cmath>
#include <set>
#include <unordered_set>

using namespace srsran;
using namespace srs_cu_cp;

#define LOG_CHAN ("CU-CP")

void srsran::srs_cu_cp::log_cells(const srslog::basic_logger& logger, const cell_meas_manager_cfg& cfg)
{
  if (!cfg.cells.empty()) {
    logger.debug("Configured cells:");
    for (const auto& cell : cfg.cells) {
      logger.debug(" - {}", cell.second);
    }
  }
}

bool srsran::srs_cu_cp::is_complete(const serving_cell_meas_config& cfg)
{
  // All mandatory values must be present.
  if (!cfg.pci.has_value() || !cfg.band.has_value() || !cfg.ssb_mtc.has_value() || !cfg.ssb_arfcn.has_value() ||
      !cfg.ssb_scs.has_value()) {
    return false;
  }
  // Call validators of individual params.
  if (!is_scs_valid(cfg.ssb_scs.value())) {
    return false;
  }

  // TODO: validate ssb arfcn
#ifdef SSB_ARFC_VALIDATOR
  error_type<std::string> ret =
      band_helper::is_dl_arfcn_valid_given_band(cfg.band.value(), cfg.ssb_arfcn.value(), cfg.ssb_scs.value());
  if (not ret.has_value()) {
    srslog::fetch_basic_logger(LOG_CHAN).error(
        "Invalid SSB ARFCN={} for band {}. Cause: {}", cfg.ssb_arfcn.value(), cfg.band.value(), ret.error());
    return false;
  }
#endif // SSB_ARFC_VALIDATOR

  return true;
}

bool srsran::srs_cu_cp::is_valid_configuration(
    const cell_meas_manager_cfg&                                cfg,
    const std::unordered_map<ssb_frequency_t, rrc_meas_obj_nr>& ssb_freq_to_meas_object)
{
  const auto& ntn_cfg = cfg.ntn_location_mobility;
  if (ntn_cfg.enabled) {
    if (ntn_cfg.beams.empty()) {
      srslog::fetch_basic_logger(LOG_CHAN).error("NTN location mobility requires at least one static beam");
      return false;
    }
    if (ntn_cfg.required_consecutive_location_reports == 0) {
      srslog::fetch_basic_logger(LOG_CHAN).error("NTN location mobility requires at least one consecutive report");
      return false;
    }
    if (!std::isfinite(ntn_cfg.served_beam_min_elevation_deg) || ntn_cfg.served_beam_min_elevation_deg < -90.0 ||
        ntn_cfg.served_beam_min_elevation_deg > 90.0) {
      srslog::fetch_basic_logger(LOG_CHAN).error("NTN served beam minimum elevation must be within [-90, 90] degrees");
      return false;
    }
    if (ntn_cfg.max_nof_served_beams == 0) {
      srslog::fetch_basic_logger(LOG_CHAN).error("NTN max number of served beams must be greater than zero");
      return false;
    }
    if (ntn_cfg.served_beam_hopping_dwell_updates == 0) {
      srslog::fetch_basic_logger(LOG_CHAN).error("NTN served beam hopping dwell updates must be greater than zero");
      return false;
    }
    const auto& sat_state_cfg = ntn_cfg.satellite_state_update;
    if (sat_state_cfg.source != ntn_satellite_state_source::manual) {
      if (sat_state_cfg.update_period.count() <= 0) {
        srslog::fetch_basic_logger(LOG_CHAN).error("NTN satellite state update period must be greater than zero");
        return false;
      }
      if (sat_state_cfg.source == ntn_satellite_state_source::circular_orbit) {
        if (!std::isfinite(sat_state_cfg.circular_altitude_m) || sat_state_cfg.circular_altitude_m <= 0.0 ||
            !std::isfinite(sat_state_cfg.circular_inclination_deg) ||
            !std::isfinite(sat_state_cfg.circular_raan_deg) ||
            !std::isfinite(sat_state_cfg.circular_argument_of_latitude_deg)) {
          srslog::fetch_basic_logger(LOG_CHAN).error("Invalid NTN circular orbit configuration");
          return false;
        }
      }
      if (sat_state_cfg.source == ntn_satellite_state_source::tle &&
          (sat_state_cfg.tle_line1.empty() || sat_state_cfg.tle_line2.empty())) {
        srslog::fetch_basic_logger(LOG_CHAN).error("NTN TLE orbit source requires both TLE lines");
        return false;
      }
    }
    if (ntn_cfg.boundary_hysteresis_m < 0.0) {
      srslog::fetch_basic_logger(LOG_CHAN).error("NTN beam boundary hysteresis must not be negative");
      return false;
    }
    if (ntn_cfg.max_horizontal_accuracy_m.has_value() && ntn_cfg.max_horizontal_accuracy_m.value() < 0.0) {
      srslog::fetch_basic_logger(LOG_CHAN).error("NTN maximum horizontal accuracy must not be negative");
      return false;
    }
    if (ntn_cfg.measurement_report_period.count() < 0 || ntn_cfg.time_to_trigger.count() < 0 ||
        ntn_cfg.max_report_gap.count() < 0 || ntn_cfg.location_max_age.count() < 0 ||
        ntn_cfg.handover_retry_timeout.count() < 0 ||
        ntn_cfg.core_network_reporting.min_report_interval.count() < 0) {
      srslog::fetch_basic_logger(LOG_CHAN).error("NTN location mobility timers must not be negative");
      return false;
    }

    bool                            has_enabled_beam = false;
    std::unordered_set<std::string> beam_ids;
    std::set<nr_cell_identity>      beam_ncis;
    for (const auto& beam : ntn_cfg.beams) {
      if (beam.beam_id.empty()) {
        srslog::fetch_basic_logger(LOG_CHAN).error("NTN beam id must not be empty");
        return false;
      }
      if (!beam_ids.emplace(beam.beam_id).second) {
        srslog::fetch_basic_logger(LOG_CHAN).error("Duplicate NTN beam id '{}'", beam.beam_id);
        return false;
      }
      if (!beam_ncis.emplace(beam.nci).second) {
        srslog::fetch_basic_logger(LOG_CHAN).error("Duplicate NTN beam nci={:#x}", beam.nci);
        return false;
      }
      has_enabled_beam |= beam.enabled;
      if (cfg.cells.find(beam.nci) == cfg.cells.end()) {
        srslog::fetch_basic_logger(LOG_CHAN).error("NTN beam '{}' nci={:#x} has no cell measurement config",
                                                   beam.beam_id,
                                                   beam.nci);
        return false;
      }
      if (beam.center_latitude_deg < -90.0 || beam.center_latitude_deg > 90.0 ||
          beam.center_longitude_deg < -180.0 || beam.center_longitude_deg > 180.0 ||
          beam.coverage_radius_m <= 0.0 ||
          !std::isfinite(beam.center_latitude_deg) || !std::isfinite(beam.center_longitude_deg) ||
          !std::isfinite(beam.coverage_radius_m)) {
        srslog::fetch_basic_logger(LOG_CHAN).error("Invalid NTN beam position '{}'", beam.beam_id);
        return false;
      }
    }
    if (!has_enabled_beam) {
      srslog::fetch_basic_logger(LOG_CHAN).error("NTN location mobility requires at least one enabled beam");
      return false;
    }
  }

  std::vector<nr_cell_identity> ncis;
  // Verify neighbor cell lists: cell id must not be included in neighbor cell list.
  for (const auto& cell : cfg.cells) {
    const auto& nci = cell.first;
    if (std::find(ncis.begin(), ncis.end(), nci) != ncis.end()) {
      srslog::fetch_basic_logger(LOG_CHAN).error("Cell {:#x} already present, but must be unique", nci);
      return false;
    }
    ncis.push_back(nci);

    if (!ssb_freq_to_meas_object.empty()) {
      const auto& serving_cell_cfg = cell.second.serving_cell_cfg;
      if (serving_cell_cfg.ssb_arfcn.has_value()) {
        ssb_frequency_t ssb_freq = serving_cell_cfg.ssb_arfcn.value();
        if (ssb_freq_to_meas_object.find(ssb_freq) != ssb_freq_to_meas_object.end()) {
          // Check if the measurement object is already present.
          rrc_meas_obj_nr meas_obj_nr = generate_measurement_object(serving_cell_cfg);
          if (!is_duplicate(meas_obj_nr, ssb_freq_to_meas_object.at(ssb_freq))) {
            // If a measurement object for this ssb_freq is already present but not an update, we reject the update.
            srslog::fetch_basic_logger(LOG_CHAN).error(
                "Measurement object for ssb_freq={} already exists, but has different ssb_scs, smtc1 and/or smtc2",
                ssb_freq);
            return false;
          }
        }
      }
    }

    for (const auto& ncell_nci : cell.second.ncells) {
      if (nci == ncell_nci.nci) {
        srslog::fetch_basic_logger(LOG_CHAN).error("Cell {:#x} must not be its own neighbor", nci);
        return false;
      }
    }
  }

  return true;
}

bool srsran::srs_cu_cp::is_complete(const cell_meas_manager_cfg& cfg)
{
  if (!is_valid_configuration(cfg)) {
    return false;
  }

  // Verify each neighbor cell has a valid config.
  for (const auto& cell : cfg.cells) {
    for (const auto& ncell : cell.second.ncells) {
      // Verify NCI is present.
      if (cfg.cells.find(ncell.nci) == cfg.cells.end()) {
        srslog::fetch_basic_logger(LOG_CHAN).error("No config for cell id {} found", ncell.nci);
        return false;
      }

      // Verify the config for this cell is complete.
      if (!is_complete(cfg.cells.at(ncell.nci).serving_cell_cfg)) {
        srslog::fetch_basic_logger(LOG_CHAN).error("Measurement config for cell id {} is not complete", ncell.nci);
        return false;
      }
    }
  }

  return true;
}

void srsran::srs_cu_cp::add_old_meas_config_to_rem_list(const rrc_meas_cfg& old_cfg, rrc_meas_cfg& new_cfg)
{
  // Remove measurement objects.
  for (const auto& meas_obj : old_cfg.meas_obj_to_add_mod_list) {
    new_cfg.meas_obj_to_rem_list.push_back(meas_obj.meas_obj_id);
  }

  // Remove measurement IDs.
  for (const auto& meas_id : old_cfg.meas_id_to_add_mod_list) {
    new_cfg.meas_id_to_rem_list.push_back(meas_id.meas_id);
  }

  // Remove active reports.
  for (const auto& report : old_cfg.report_cfg_to_add_mod_list) {
    new_cfg.report_cfg_to_rem_list.push_back(report.report_cfg_id);
  }
}

std::vector<ssb_frequency_t> srsran::srs_cu_cp::generate_measurement_object_list(const cell_meas_manager_cfg& cfg,
                                                                                 nr_cell_identity serving_nci)
{
  srsran_assert(cfg.cells.find(serving_nci) != cfg.cells.end(), "No cell config for nci={:#x}", serving_nci);

  // Add cells to lookup if report is configured
  std::vector<ssb_frequency_t> ssb_freqs;
  // Add serving cell if periodic report is configured
  auto& serving_cell = cfg.cells.at(serving_nci);
  if (serving_cell.periodic_report_cfg_id.has_value() && is_complete(serving_cell.serving_cell_cfg)) {
    ssb_freqs.push_back(serving_cell.serving_cell_cfg.ssb_arfcn.value());
  }
  // Add neighbor cells if report is configured
  for (const auto& ncell : serving_cell.ncells) {
    srsran_assert(cfg.cells.find(ncell.nci) != cfg.cells.end(), "No cell config for nci={:#x}", ncell.nci);
    auto& cell_cfg = cfg.cells.at(ncell.nci);
    if (!ncell.report_cfg_ids.empty() && is_complete(cell_cfg.serving_cell_cfg)) {
      if (std::find(ssb_freqs.begin(), ssb_freqs.end(), cell_cfg.serving_cell_cfg.ssb_arfcn.value()) ==
          ssb_freqs.end()) {
        ssb_freqs.push_back(cell_cfg.serving_cell_cfg.ssb_arfcn.value());
      }
    }
  }

  return ssb_freqs;
}

void srsran::srs_cu_cp::generate_report_config(const cell_meas_manager_cfg&  cfg,
                                               const nr_cell_identity        meas_obj_nci,
                                               const nr_cell_identity        serving_nci,
                                               const report_cfg_id_t         report_cfg_id,
                                               rrc_meas_cfg&                 meas_cfg,
                                               cell_meas_manager_ue_context& ue_meas_context)
{
  // add report cfg to add mod
  rrc_report_cfg_to_add_mod report_cfg_to_add_mod;
  report_cfg_to_add_mod.report_cfg_id = report_cfg_id;
  report_cfg_to_add_mod.report_cfg    = cfg.report_config_ids.at(report_cfg_id);
  meas_cfg.report_cfg_to_add_mod_list.push_back(report_cfg_to_add_mod);

  // Add meas id to link the cell and the report together.
  rrc_meas_id_to_add_mod meas_id_to_add_mod;
  meas_id_to_add_mod.meas_id       = ue_meas_context.allocate_meas_id();
  meas_id_to_add_mod.meas_obj_id   = ue_meas_context.nci_to_meas_obj_id.at(meas_obj_nci);
  meas_id_to_add_mod.report_cfg_id = report_cfg_id;
  meas_cfg.meas_id_to_add_mod_list.push_back(meas_id_to_add_mod);

  // add meas id to lookup
  auto& serving_cell_cfg = cfg.cells.at(serving_nci).serving_cell_cfg;
  ue_meas_context.meas_id_to_meas_context.emplace(meas_id_to_add_mod.meas_id,
                                                  meas_context_t{meas_id_to_add_mod.meas_obj_id,
                                                                 meas_id_to_add_mod.report_cfg_id,
                                                                 serving_cell_cfg.gnb_id_bit_length,
                                                                 serving_cell_cfg.nci,
                                                                 serving_cell_cfg.pci.value()});
}

rrc_meas_obj_nr srsran::srs_cu_cp::generate_measurement_object(const serving_cell_meas_config& cfg)
{
  rrc_meas_obj_nr meas_obj_nr;

  meas_obj_nr.ssb_freq               = cfg.ssb_arfcn;
  meas_obj_nr.ssb_subcarrier_spacing = cfg.ssb_scs;
  meas_obj_nr.smtc1                  = cfg.ssb_mtc;

  // Mandatory fields.
  meas_obj_nr.ref_sig_cfg.ssb_cfg_mob.emplace().derive_ssb_idx_from_cell = true;
  meas_obj_nr.nrof_ss_blocks_to_average.emplace()                        = 8; // TODO: remove hardcoded values
  meas_obj_nr.quant_cfg_idx                                              = 1; // TODO: remove hardcoded values
  meas_obj_nr.freq_band_ind_nr.emplace()                                 = nr_band_to_uint(cfg.band.value());

  // TODO: Add optional fields.

  return meas_obj_nr;
}

bool srsran::srs_cu_cp::is_duplicate(const rrc_meas_obj_nr& obj_1, const rrc_meas_obj_nr& obj_2)
{
  // TS 38.331 section 5.5.2.1:
  // For all SSB based measurements there is at most one measurement object with
  // the same ssbFrequency;
  // - an smtc1 included in any measurement object with the same ssbFrequency has the
  //   same value and that an smtc2 included in any measurement object with the
  //   same ssbFrequency has the same value;
  // - to ensure that all measurement objects configured in this specification and in
  //   TS 36.331 [10] with the same ssbFrequency have the same ssbSubcarrierSpacing;
  return obj_1.ssb_freq == obj_2.ssb_freq && obj_1.ssb_subcarrier_spacing == obj_2.ssb_subcarrier_spacing &&
         obj_1.smtc1 == obj_2.smtc1 && obj_1.smtc2 == obj_2.smtc2;
}

void srsran::srs_cu_cp::log_meas_objects(const srslog::basic_logger&                                 logger,
                                         const std::unordered_map<ssb_frequency_t, rrc_meas_obj_nr>& meas_objects)
{
  if (!meas_objects.empty()) {
    logger.debug("Measurement objects:");
    for (const auto& meas_obj : meas_objects) {
      logger.debug(" - ssb_freq={}: {}", meas_obj.first, meas_obj.second);
    }
  }
}
