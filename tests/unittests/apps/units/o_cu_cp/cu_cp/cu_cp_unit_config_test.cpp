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

#include "apps/units/o_cu_cp/cu_cp/cu_cp_config_translators.h"
#include "apps/units/o_cu_cp/cu_cp/cu_cp_unit_config.h"
#include "apps/units/o_cu_cp/cu_cp/cu_cp_unit_config_validator.h"
#include "srsran/cu_cp/cu_cp_configuration.h"
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

using namespace srsran;

namespace {

std::filesystem::path get_project_source_root()
{
  return std::filesystem::path(__FILE__)
      .parent_path()
      .parent_path()
      .parent_path()
      .parent_path()
      .parent_path()
      .parent_path()
      .parent_path();
}

nr_cell_identity make_nci(unsigned sector_id)
{
  return nr_cell_identity::create(gnb_id_t{0x19b, 32}, sector_id).value();
}

void add_ntn_cell(cu_cp_unit_config& cfg, nr_cell_identity nci)
{
  cu_cp_unit_cell_config_item cell;
  cell.nr_cell_id        = nci.value();
  cell.gnb_id_bit_length = 32;
  cfg.mobility_config.cells.push_back(cell);
}

void add_ntn_cell(cu_cp_unit_config& cfg, uint64_t nci, unsigned gnb_id_bit_length)
{
  cu_cp_unit_cell_config_item cell;
  cell.nr_cell_id        = nci;
  cell.gnb_id_bit_length = gnb_id_bit_length;
  cfg.mobility_config.cells.push_back(cell);
}

cu_cp_unit_report_config make_a3_report_config(unsigned report_cfg_id)
{
  cu_cp_unit_report_config report_cfg;
  report_cfg.report_cfg_id                 = report_cfg_id;
  report_cfg.report_type                   = "event_triggered";
  report_cfg.report_interval_ms            = 120;
  report_cfg.event_triggered_report_type   = "a3";
  report_cfg.meas_trigger_quantity         = "rsrp";
  report_cfg.meas_trigger_quantity_offset_db = 6;
  report_cfg.hysteresis_db                 = 0;
  report_cfg.time_to_trigger_ms            = 100;
  return report_cfg;
}

std::filesystem::path write_single_ntn_beam_table(const char* filename, nr_cell_identity nci)
{
  const std::filesystem::path beam_table_path = std::filesystem::temp_directory_path() / filename;
  std::ofstream               json_file(beam_table_path);
  json_file << R"json(
{
  "version": 1,
  "region": "test",
  "satellite_height_m": 35786000,
  "beams": [
    {
      "beam_id": "GEO-BEAM-0001",
      "nci": )json"
            << nci.value() << R"json(,
      "center_latitude_deg": 0.0,
      "center_longitude_deg": 120.0,
      "coverage_radius_m": 250000,
      "enabled": true
    }
  ]
}
)json";
  return beam_table_path;
}

} // namespace

TEST(cu_cp_unit_config, default_terrestrial_config_keeps_ntn_disabled)
{
  cu_cp_unit_config cfg;

  ASSERT_TRUE(validate_cu_cp_unit_config(cfg));

  const srs_cu_cp::cu_cp_configuration cu_cp_cfg = generate_cu_cp_config(cfg);
  const auto&                          ntn_cfg = cu_cp_cfg.mobility.meas_manager_config.ntn_location_mobility;

  ASSERT_FALSE(ntn_cfg.enabled);
  ASSERT_TRUE(ntn_cfg.beams.empty());
  ASSERT_EQ(ntn_cfg.satellite_state_update.source, srs_cu_cp::ntn_satellite_state_source::manual);
  ASSERT_EQ(ntn_cfg.satellite_state_update.update_period, std::chrono::milliseconds(0));
}

TEST(cu_cp_unit_config, geo_like_circular_orbit_ntn_config_is_accepted)
{
  const nr_cell_identity nci = make_nci(0);
  const std::filesystem::path beam_table_path =
      write_single_ntn_beam_table("srsran_ntn_unit_config_geo_beam_table.json", nci);

  cu_cp_unit_config cfg;
  cfg.gnb_id = gnb_id_t{0x19b, 32};
  add_ntn_cell(cfg, nci);
  cfg.mobility_config.ntn_location_mobility.enabled                          = true;
  cfg.mobility_config.ntn_location_mobility.beam_table_json_file             = beam_table_path.string();
  cfg.mobility_config.ntn_location_mobility.satellite_state_source           = "circular_orbit";
  cfg.mobility_config.ntn_location_mobility.satellite_state_update_period_ms = 60000;
  cfg.mobility_config.ntn_location_mobility.circular_orbit_altitude_m        = 35786000.0;
  cfg.mobility_config.ntn_location_mobility.circular_orbit_inclination_deg   = 0.0;
  cfg.mobility_config.ntn_location_mobility.served_beam_min_elevation_deg    = 50.0;

  ASSERT_TRUE(validate_cu_cp_unit_config(cfg));

  const srs_cu_cp::cu_cp_configuration cu_cp_cfg = generate_cu_cp_config(cfg);
  const auto&                          ntn_cfg = cu_cp_cfg.mobility.meas_manager_config.ntn_location_mobility;

  ASSERT_TRUE(ntn_cfg.enabled);
  ASSERT_EQ(ntn_cfg.beams.size(), 1);
  ASSERT_EQ(ntn_cfg.beams.front().beam_id, "GEO-BEAM-0001");
  ASSERT_EQ(ntn_cfg.satellite_state_update.source, srs_cu_cp::ntn_satellite_state_source::circular_orbit);
  ASSERT_EQ(ntn_cfg.satellite_state_update.update_period, std::chrono::milliseconds(60000));
  ASSERT_DOUBLE_EQ(ntn_cfg.satellite_state_update.circular_altitude_m, 35786000.0);
  ASSERT_DOUBLE_EQ(ntn_cfg.satellite_state_update.circular_inclination_deg, 0.0);
  ASSERT_DOUBLE_EQ(ntn_cfg.served_beam_min_elevation_deg, 50.0);

  std::filesystem::remove(beam_table_path);
}

TEST(cu_cp_unit_config, ntn_location_mobility_validation_rejects_enabled_config_without_beam_table)
{
  cu_cp_unit_config cfg;
  cfg.mobility_config.ntn_location_mobility.enabled = true;

  ::testing::internal::CaptureStdout();
  const bool        valid  = validate_cu_cp_unit_config(cfg);
  const std::string output = ::testing::internal::GetCapturedStdout();

  ASSERT_FALSE(valid);
  ASSERT_NE(output.find("NTN location mobility"), std::string::npos);
  ASSERT_NE(output.find("beam_table_json_file"), std::string::npos);
}

TEST(cu_cp_unit_config, ntn_location_mobility_validation_rejects_unknown_satellite_state_source)
{
  const nr_cell_identity nci = make_nci(0);
  const std::filesystem::path beam_table_path =
      write_single_ntn_beam_table("srsran_ntn_unit_config_unknown_source_beam_table.json", nci);

  cu_cp_unit_config cfg;
  cfg.gnb_id = gnb_id_t{0x19b, 32};
  add_ntn_cell(cfg, nci);
  cfg.mobility_config.ntn_location_mobility.enabled                = true;
  cfg.mobility_config.ntn_location_mobility.beam_table_json_file   = beam_table_path.string();
  cfg.mobility_config.ntn_location_mobility.satellite_state_source = "unknown_orbit";

  ::testing::internal::CaptureStdout();
  const bool        valid  = validate_cu_cp_unit_config(cfg);
  const std::string output = ::testing::internal::GetCapturedStdout();
  std::filesystem::remove(beam_table_path);

  ASSERT_FALSE(valid);
  ASSERT_NE(output.find("satellite_state_source"), std::string::npos);
  ASSERT_NE(output.find("unknown_orbit"), std::string::npos);
  ASSERT_NE(output.find("manual"), std::string::npos);
  ASSERT_NE(output.find("circular_orbit"), std::string::npos);
  ASSERT_NE(output.find("tle"), std::string::npos);
}

TEST(cu_cp_unit_config, ntn_location_mobility_validation_rejects_orbit_source_without_update_period)
{
  const nr_cell_identity nci = make_nci(0);
  const std::filesystem::path beam_table_path =
      write_single_ntn_beam_table("srsran_ntn_unit_config_missing_update_period_beam_table.json", nci);

  cu_cp_unit_config cfg;
  cfg.gnb_id = gnb_id_t{0x19b, 32};
  add_ntn_cell(cfg, nci);
  cfg.mobility_config.ntn_location_mobility.enabled                = true;
  cfg.mobility_config.ntn_location_mobility.beam_table_json_file   = beam_table_path.string();
  cfg.mobility_config.ntn_location_mobility.satellite_state_source = "circular_orbit";

  ::testing::internal::CaptureStdout();
  const bool        valid  = validate_cu_cp_unit_config(cfg);
  const std::string output = ::testing::internal::GetCapturedStdout();
  std::filesystem::remove(beam_table_path);

  ASSERT_FALSE(valid);
  ASSERT_NE(output.find("satellite_state_update_period_ms"), std::string::npos);
  ASSERT_NE(output.find("greater than zero"), std::string::npos);
}

TEST(cu_cp_unit_config, leo_500km_example_beam_table_loads_with_circular_orbit_defaults)
{
  cu_cp_unit_config cfg;
  cfg.mobility_config.ntn_location_mobility.enabled                          = true;
  cfg.mobility_config.ntn_location_mobility.beam_table_json_file =
      (get_project_source_root() / "configs/leo_500km_beam_table.json").string();
  cfg.mobility_config.ntn_location_mobility.satellite_state_source           = "circular_orbit";
  cfg.mobility_config.ntn_location_mobility.satellite_state_update_period_ms = 1000;
  cfg.mobility_config.ntn_location_mobility.circular_orbit_altitude_m        = 500000.0;
  cfg.mobility_config.ntn_location_mobility.served_beam_min_elevation_deg    = 50.0;
  cfg.mobility_config.ntn_location_mobility.max_nof_served_beams             = 3;
  cfg.mobility_config.ntn_location_mobility.served_beam_hopping_enabled      = true;
  cfg.mobility_config.ntn_location_mobility.served_beam_hopping_dwell_updates = 3;
  cfg.mobility_config.ntn_location_mobility.required_consecutive_location_reports = 3;
  cfg.mobility_config.ntn_location_mobility.time_to_trigger_ms                    = 1000;
  cfg.mobility_config.ntn_location_mobility.max_report_gap_ms                     = 750;
  cfg.mobility_config.ntn_location_mobility.boundary_hysteresis_m                 = 5000.0;
  cfg.mobility_config.ntn_location_mobility.core_network_reporting_local_forwarding_enabled = true;
  cfg.mobility_config.ntn_location_mobility.core_network_reporting_amf_control_enabled      = false;
  cfg.mobility_config.ntn_location_mobility.core_network_reporting_min_report_interval_ms   = 2000;
  for (unsigned sector_id = 0; sector_id != 5; ++sector_id) {
    add_ntn_cell(cfg, 0x66c000 + sector_id, 22);
  }

  const srs_cu_cp::cu_cp_configuration cu_cp_cfg = generate_cu_cp_config(cfg);
  const auto&                          ntn_cfg = cu_cp_cfg.mobility.meas_manager_config.ntn_location_mobility;

  ASSERT_TRUE(ntn_cfg.enabled);
  ASSERT_EQ(ntn_cfg.beams.size(), 5);
  ASSERT_EQ(ntn_cfg.beams.front().beam_id, "LEO500-BEAM-0001");
  ASSERT_DOUBLE_EQ(ntn_cfg.beams.front().coverage_radius_m, 15000.0);
  ASSERT_EQ(ntn_cfg.beams.back().beam_id, "LEO500-BEAM-0005");
  ASSERT_DOUBLE_EQ(ntn_cfg.beams.back().coverage_radius_m, 15000.0);
  ASSERT_DOUBLE_EQ(ntn_cfg.satellite_state_update.circular_altitude_m, 500000.0);
  ASSERT_EQ(ntn_cfg.satellite_state_update.update_period, std::chrono::milliseconds(1000));
  ASSERT_EQ(ntn_cfg.satellite_state_update.source, srs_cu_cp::ntn_satellite_state_source::circular_orbit);
  ASSERT_DOUBLE_EQ(ntn_cfg.served_beam_min_elevation_deg, 50.0);
  ASSERT_EQ(ntn_cfg.max_nof_served_beams, 3);
  ASSERT_TRUE(ntn_cfg.served_beam_hopping_enabled);
  ASSERT_EQ(ntn_cfg.served_beam_hopping_dwell_updates, 3);
  ASSERT_EQ(ntn_cfg.required_consecutive_location_reports, 3);
  ASSERT_EQ(ntn_cfg.time_to_trigger, std::chrono::milliseconds(1000));
  ASSERT_EQ(ntn_cfg.max_report_gap, std::chrono::milliseconds(750));
  ASSERT_DOUBLE_EQ(ntn_cfg.boundary_hysteresis_m, 5000.0);
  ASSERT_TRUE(ntn_cfg.core_network_reporting.local_forwarding_enabled);
  ASSERT_FALSE(ntn_cfg.core_network_reporting.amf_control_enabled);
  ASSERT_EQ(ntn_cfg.core_network_reporting.min_report_interval, std::chrono::milliseconds(2000));
}

TEST(cu_cp_unit_config, ntn_location_mobility_loads_static_beams_from_json_file)
{
  const nr_cell_identity nci1 = make_nci(0);
  const nr_cell_identity nci2 = make_nci(1);

  const std::filesystem::path beam_table_path =
      std::filesystem::temp_directory_path() / "srsran_ntn_unit_config_beam_table.json";
  {
    std::ofstream json_file(beam_table_path);
    json_file << R"json(
{
  "version": 1,
  "region": "china",
  "satellite_height_m": 500000,
  "beams": [
    {
      "beam_id": "CN-BEAM-0001",
      "nci": )json"
              << nci1.value() << R"json(,
      "center_latitude_deg": 39.9,
      "center_longitude_deg": 116.4,
      "coverage_radius_m": 230000,
      "enabled": true
    },
    {
      "beam_id": "CN-BEAM-0002",
      "nci": )json"
              << nci2.value() << R"json(,
      "center_latitude_deg": 31.2,
      "center_longitude_deg": 121.5,
      "coverage_radius_m": 230000,
      "enabled": true
    }
  ]
}
)json";
  }

  cu_cp_unit_config cfg;
  cfg.gnb_id = gnb_id_t{0x19b, 32};
  add_ntn_cell(cfg, nci1);
  add_ntn_cell(cfg, nci2);
  cfg.mobility_config.ntn_location_mobility.enabled                               = true;
  cfg.mobility_config.ntn_location_mobility.beam_table_json_file                  = beam_table_path.string();
  cfg.mobility_config.ntn_location_mobility.served_beam_min_elevation_deg         = 25.0;
  cfg.mobility_config.ntn_location_mobility.max_nof_served_beams                  = 4;
  cfg.mobility_config.ntn_location_mobility.served_beam_hopping_enabled           = true;
  cfg.mobility_config.ntn_location_mobility.served_beam_hopping_dwell_updates     = 5;
  cfg.mobility_config.ntn_location_mobility.satellite_state_source                = "circular_orbit";
  cfg.mobility_config.ntn_location_mobility.satellite_state_update_period_ms      = 200;
  cfg.mobility_config.ntn_location_mobility.circular_orbit_altitude_m             = 600000.0;
  cfg.mobility_config.ntn_location_mobility.circular_orbit_inclination_deg        = 55.0;
  cfg.mobility_config.ntn_location_mobility.circular_orbit_raan_deg               = 20.0;
  cfg.mobility_config.ntn_location_mobility.circular_orbit_argument_of_latitude_deg = 10.0;
  cfg.mobility_config.ntn_location_mobility.circular_orbit_epoch_unix_s           = 30.0;
  cfg.mobility_config.ntn_location_mobility.measurement_report_period_ms          = 500;
  cfg.mobility_config.ntn_location_mobility.time_to_trigger_ms                    = 1000;
  cfg.mobility_config.ntn_location_mobility.required_consecutive_location_reports = 2;
  cfg.mobility_config.ntn_location_mobility.boundary_hysteresis_m                 = 5000.0;
  cfg.mobility_config.ntn_location_mobility.max_horizontal_accuracy_m             = 250.0;
  cfg.mobility_config.ntn_location_mobility.core_network_reporting_local_forwarding_enabled = true;
  cfg.mobility_config.ntn_location_mobility.core_network_reporting_min_report_interval_ms   = 1500;

  const srs_cu_cp::cu_cp_configuration cu_cp_cfg = generate_cu_cp_config(cfg);
  const auto&                          ntn_cfg = cu_cp_cfg.mobility.meas_manager_config.ntn_location_mobility;

  ASSERT_TRUE(ntn_cfg.enabled);
  ASSERT_EQ(ntn_cfg.beams.size(), 2);
  ASSERT_EQ(ntn_cfg.beams[0].beam_id, "CN-BEAM-0001");
  ASSERT_EQ(ntn_cfg.beams[0].nci, nci1);
  ASSERT_EQ(ntn_cfg.beams[1].beam_id, "CN-BEAM-0002");
  ASSERT_EQ(ntn_cfg.beams[1].nci, nci2);
  ASSERT_DOUBLE_EQ(ntn_cfg.served_beam_min_elevation_deg, 25.0);
  ASSERT_EQ(ntn_cfg.max_nof_served_beams, 4);
  ASSERT_TRUE(ntn_cfg.served_beam_hopping_enabled);
  ASSERT_EQ(ntn_cfg.served_beam_hopping_dwell_updates, 5);
  ASSERT_EQ(ntn_cfg.satellite_state_update.source, srs_cu_cp::ntn_satellite_state_source::circular_orbit);
  ASSERT_EQ(ntn_cfg.satellite_state_update.update_period, std::chrono::milliseconds(200));
  ASSERT_DOUBLE_EQ(ntn_cfg.satellite_state_update.circular_altitude_m, 600000.0);
  ASSERT_DOUBLE_EQ(ntn_cfg.satellite_state_update.circular_inclination_deg, 55.0);
  ASSERT_DOUBLE_EQ(ntn_cfg.satellite_state_update.circular_raan_deg, 20.0);
  ASSERT_DOUBLE_EQ(ntn_cfg.satellite_state_update.circular_argument_of_latitude_deg, 10.0);
  ASSERT_EQ(std::chrono::duration_cast<std::chrono::seconds>(
                ntn_cfg.satellite_state_update.circular_epoch.time_since_epoch())
                .count(),
            30);
  ASSERT_EQ(ntn_cfg.measurement_report_period, std::chrono::milliseconds(500));
  ASSERT_EQ(ntn_cfg.time_to_trigger, std::chrono::milliseconds(1000));
  ASSERT_EQ(ntn_cfg.required_consecutive_location_reports, 2);
  ASSERT_DOUBLE_EQ(ntn_cfg.boundary_hysteresis_m, 5000.0);
  ASSERT_TRUE(ntn_cfg.max_horizontal_accuracy_m.has_value());
  ASSERT_DOUBLE_EQ(ntn_cfg.max_horizontal_accuracy_m.value(), 250.0);
  ASSERT_TRUE(ntn_cfg.core_network_reporting.local_forwarding_enabled);
  ASSERT_TRUE(ntn_cfg.core_network_reporting.amf_control_enabled);
  ASSERT_EQ(ntn_cfg.core_network_reporting.min_report_interval, std::chrono::milliseconds(1500));

  std::filesystem::remove(beam_table_path);
}

TEST(cu_cp_unit_config, ntn_location_mobility_validation_rejects_beam_without_cell_config)
{
  const nr_cell_identity nci1 = make_nci(0);
  const nr_cell_identity nci2 = make_nci(1);

  const std::filesystem::path beam_table_path =
      std::filesystem::temp_directory_path() / "srsran_ntn_unit_config_unmatched_beam_table.json";
  {
    std::ofstream json_file(beam_table_path);
    json_file << R"json(
{
  "version": 1,
  "region": "china",
  "satellite_height_m": 500000,
  "beams": [
    {
      "beam_id": "CN-BEAM-0001",
      "nci": )json"
              << nci1.value() << R"json(,
      "center_latitude_deg": 39.9,
      "center_longitude_deg": 116.4,
      "coverage_radius_m": 230000,
      "enabled": true
    },
    {
      "beam_id": "CN-BEAM-0002",
      "nci": )json"
              << nci2.value() << R"json(,
      "center_latitude_deg": 31.2,
      "center_longitude_deg": 121.5,
      "coverage_radius_m": 230000,
      "enabled": true
    }
  ]
}
)json";
  }

  cu_cp_unit_config cfg;
  cfg.gnb_id = gnb_id_t{0x19b, 32};
  cfg.mobility_config.ntn_location_mobility.enabled                          = true;
  cfg.mobility_config.ntn_location_mobility.beam_table_json_file             = beam_table_path.string();
  cfg.mobility_config.ntn_location_mobility.satellite_state_source           = "manual";
  cfg.mobility_config.ntn_location_mobility.required_consecutive_location_reports = 1;
  add_ntn_cell(cfg, nci1);

  ASSERT_FALSE(validate_cu_cp_unit_config(cfg));
  add_ntn_cell(cfg, nci2);
  ASSERT_TRUE(validate_cu_cp_unit_config(cfg));

  std::filesystem::remove(beam_table_path);
}

TEST(cu_cp_unit_config, admission_watermarks_are_translated_to_cu_cp_config)
{
  cu_cp_unit_config cfg;
  cfg.initial_access_admission.max_ue_usage   = 70;
  cfg.initial_access_admission.max_drb_usage  = 65;
  cfg.reestablishment_admission.max_ue_usage  = 90;
  cfg.reestablishment_admission.max_drb_usage = 85;
  cfg.handover_admission.max_ue_usage         = 95;
  cfg.handover_admission.max_drb_usage        = 92;

  const srs_cu_cp::cu_cp_configuration cu_cp_cfg = generate_cu_cp_config(cfg);

  ASSERT_EQ(cu_cp_cfg.admission.initial_access_watermark.max_ue_usage, 70);
  ASSERT_EQ(cu_cp_cfg.admission.initial_access_watermark.max_drb_usage, 65);
  ASSERT_EQ(cu_cp_cfg.admission.reestablishment_watermark.max_ue_usage, 90);
  ASSERT_EQ(cu_cp_cfg.admission.reestablishment_watermark.max_drb_usage, 85);
  ASSERT_EQ(cu_cp_cfg.admission.handover_watermark.max_ue_usage, 95);
  ASSERT_EQ(cu_cp_cfg.admission.handover_watermark.max_drb_usage, 92);
}

TEST(cu_cp_unit_config, neighbor_cell_info_loads_static_cells_and_relations_from_json_file)
{
  const nr_cell_identity serving_nci = make_nci(0);
  const nr_cell_identity neighbor_nci = make_nci(1);

  const std::filesystem::path neighbor_info_path =
      std::filesystem::temp_directory_path() / "srsran_cu_cp_unit_config_neighbor_cells.json";
  {
    std::ofstream json_file(neighbor_info_path);
    json_file << R"json(
{
  "version": 1,
  "region": "test",
  "cells": [
    {
      "nci": )json"
              << serving_nci.value() << R"json(,
      "gnb_id_bit_length": 32,
      "pci": 10,
      "band": 78,
      "ssb_arfcn": 632628,
      "ssb_scs": 30,
      "ssb_period": 20,
      "ssb_offset": 0,
      "ssb_duration": 1
    },
    {
      "nci": )json"
              << neighbor_nci.value() << R"json(,
      "gnb_id_bit_length": 32,
      "pci": 11,
      "band": 78,
      "ssb_arfcn": 632628,
      "ssb_scs": 30,
      "ssb_period": 20,
      "ssb_offset": 0,
      "ssb_duration": 1
    }
  ],
  "relations": [
    {
      "serving_nci": )json"
              << serving_nci.value() << R"json(,
      "neighbor_nci": )json"
              << neighbor_nci.value() << R"json(,
      "report_cfg_ids": [2],
      "handover_allowed": true
    }
  ]
}
)json";
  }

  cu_cp_unit_config cfg;
  cfg.mobility_config.neighbor_cell_info_json_file = neighbor_info_path.string();
  cfg.mobility_config.report_configs.push_back(make_a3_report_config(2));

  const srs_cu_cp::cu_cp_configuration cu_cp_cfg = generate_cu_cp_config(cfg);
  const auto&                          meas_cfg  = cu_cp_cfg.mobility.meas_manager_config;

  ASSERT_EQ(meas_cfg.cells.size(), 2);
  ASSERT_EQ(meas_cfg.cells.at(serving_nci).ncells.size(), 1);
  ASSERT_EQ(meas_cfg.cells.at(serving_nci).ncells.front().nci, neighbor_nci);
  ASSERT_EQ(meas_cfg.cells.at(serving_nci).ncells.front().report_cfg_ids.size(), 1);
  ASSERT_EQ(meas_cfg.cells.at(serving_nci).ncells.front().report_cfg_ids.front(), srs_cu_cp::uint_to_report_cfg_id(2));
  ASSERT_TRUE(meas_cfg.cells.at(neighbor_nci).serving_cell_cfg.pci.has_value());
  ASSERT_EQ(meas_cfg.cells.at(neighbor_nci).serving_cell_cfg.pci.value(), 11);
  ASSERT_TRUE(meas_cfg.cells.at(neighbor_nci).serving_cell_cfg.band.has_value());
  ASSERT_EQ(nr_band_to_uint(meas_cfg.cells.at(neighbor_nci).serving_cell_cfg.band.value()), 78);

  std::filesystem::remove(neighbor_info_path);
}
