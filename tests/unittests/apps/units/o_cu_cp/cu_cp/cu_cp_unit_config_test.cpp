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

#include "apps/units/o_cu_cp/cu_cp/cu_cp_cmdline_commands.h"
#include "apps/units/o_cu_cp/cu_cp/cu_cp_config_translators.h"
#include "apps/units/o_cu_cp/cu_cp/cu_cp_unit_config.h"
#include "apps/units/o_cu_cp/cu_cp/cu_cp_unit_config_validator.h"
#include "apps/units/o_cu_cp/cu_cp/cu_cp_unit_config_yaml_writer.h"
#include "srsran/cu_cp/cu_cp_configuration.h"
#include "srsran/support/async/async_task.h"
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <gtest/gtest.h>
#include <yaml-cpp/yaml.h>

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

void set_onboard_planning_context(cu_cp_unit_ntn_onboard_position_plan_config& plan)
{
  plan.expected_catalog_id                = "global-land-l1-v1";
  plan.expected_catalog_hash              = "sha256:b39fe9c3ee9a9355b3546036b7f16e0fb858c953f8558cc4295122f2169fbe7a";
  plan.expected_identity_registry_version = "mc-ntn-onboard-cell-registry-v1";
  plan.expected_identity_registry_hash    = "sha256:7475821350e104b57a70d979d630f4b29a6cecb89ca0eca7b16dddf2ffee6a4a";
  plan.expected_access_profile_id         = "ntn-access-16a-64d-v1";
  plan.expected_access_profile_hash       = "sha256:195786f4161e3b0fad6faa0605144948a7401c067a014bde684c1b29a8087d63";
}

void set_onboard_execution_context(cu_cp_unit_ntn_onboard_position_plan_config& plan)
{
  set_onboard_planning_context(plan);
  plan.state_file = "ntn-onboard-position-plan-state.json";
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

class noop_mobility_command_handler : public srs_cu_cp::cu_cp_mobility_command_handler
{
public:
  void trigger_handover(pci_t source_pci, rnti_t rnti, pci_t target_pci) override {}
};

class noop_ue_command_handler : public srs_cu_cp::cu_cp_ue_command_handler
{
public:
  async_task<srs_cu_cp::cu_cp_ue_context_release_batch_response>
  release_ues(const srs_cu_cp::cu_cp_ue_context_release_batch_command& command) override
  {
    return launch_async([](coro_context<async_task<srs_cu_cp::cu_cp_ue_context_release_batch_response>>& ctx) mutable {
      CORO_BEGIN(ctx);
      CORO_RETURN(srs_cu_cp::cu_cp_ue_context_release_batch_response{});
    });
  }
};

class noop_admission_command_handler : public srs_cu_cp::cu_cp_admission_command_handler
{
public:
  void set_ue_admission_enabled(bool enabled) override {}

  srs_cu_cp::cu_cp_admission_control_status get_admission_control_status() override { return {}; }
};

class fake_ntn_command_handler : public srs_cu_cp::cu_cp_ntn_command_handler
{
public:
  bool handle_ntn_satellite_state_update(const ecef_coordinates_t&           satellite,
                                         std::optional<ecef_coordinates_t> next_satellite = std::nullopt) override
  {
    return true;
  }

  std::vector<std::string> get_current_ntn_served_beam_ids() const override { return {}; }

  std::vector<srs_cu_cp::cu_cp_ntn_beam_status> get_current_ntn_beam_status() const override
  {
    return beam_status;
  }

  srs_cu_cp::ntn_assistance_snapshot get_current_ntn_assistance_snapshot() const override { return assistance; }

  srs_cu_cp::ntn_sib19_assistance_snapshot get_current_ntn_sib19_assistance_snapshot() const override
  {
    return sib19_assistance;
  }

  srs_cu_cp::cu_cp_ntn_runtime_status get_current_ntn_runtime_status() const override { return runtime; }

  std::vector<srs_cu_cp::cu_cp_ntn_ue_status> get_current_ntn_ue_status() const override { return ue_status; }

  srs_cu_cp::cu_cp_ntn_antenna_intent_snapshot get_current_ntn_antenna_intent_snapshot() const override
  {
    return antenna_intent;
  }

  srs_cu_cp::ntn_beam_service_resource_snapshot get_current_ntn_beam_service_resource_snapshot() const override
  {
    return resource_snapshot;
  }

  bool handle_ntn_service_switch_over_event(const srs_cu_cp::ntn_service_switch_over_event& event) override
  {
    return true;
  }

  bool clear_ntn_service_switch_over_event(uint64_t event_id) override { return true; }

  bool handle_ntn_manual_override(const srs_cu_cp::ntn_manual_override_command& command) override { return true; }

  srs_cu_cp::ntn_repair_response handle_ntn_repair_command(const srs_cu_cp::ntn_repair_command& command) override
  {
    last_repair_command = command;
    ++nof_repair_commands;
    return repair_response;
  }

  srs_cu_cp::ntn_service_switch_over_snapshot get_current_ntn_service_switch_over_snapshot() const override
  {
    return switch_over;
  }

  std::vector<srs_cu_cp::cu_cp_ntn_beam_status> beam_status;
  srs_cu_cp::ntn_assistance_snapshot            assistance;
  srs_cu_cp::ntn_sib19_assistance_snapshot      sib19_assistance;
  srs_cu_cp::cu_cp_ntn_runtime_status           runtime;
  std::vector<srs_cu_cp::cu_cp_ntn_ue_status>   ue_status;
  srs_cu_cp::cu_cp_ntn_antenna_intent_snapshot  antenna_intent;
  srs_cu_cp::ntn_beam_service_resource_snapshot resource_snapshot;
  srs_cu_cp::ntn_service_switch_over_snapshot   switch_over;
  srs_cu_cp::ntn_repair_response                repair_response;
  std::optional<srs_cu_cp::ntn_repair_command>  last_repair_command;
  unsigned                                      nof_repair_commands = 0;
};

class fake_cu_cp_command_handler : public srs_cu_cp::cu_cp_command_handler
{
public:
  srs_cu_cp::cu_cp_mobility_command_handler& get_mobility_command_handler() override { return mobility; }
  srs_cu_cp::cu_cp_ntn_command_handler&      get_ntn_command_handler() override { return ntn; }
  srs_cu_cp::cu_cp_ue_command_handler&       get_ue_command_handler() override { return ue; }
  srs_cu_cp::cu_cp_admission_command_handler& get_admission_command_handler() override { return admission; }

  fake_ntn_command_handler ntn;

private:
  noop_mobility_command_handler  mobility;
  noop_ue_command_handler        ue;
  noop_admission_command_handler admission;
};

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
  ASSERT_FALSE(cu_cp_cfg.mobility.onboard_position_plan.enabled);
  ASSERT_FALSE(cu_cp_cfg.mobility.onboard_position_plan.du_execution_enabled);
  ASSERT_EQ(cu_cp_cfg.mobility.onboard_position_plan.du_prepare_guard, std::chrono::milliseconds{1000});
  ASSERT_EQ(cu_cp_cfg.mobility.onboard_position_plan.du_prepare_horizon, std::chrono::milliseconds{4000});
  ASSERT_EQ(cu_cp_cfg.mobility.onboard_position_plan.du_apply_timeout, std::chrono::milliseconds{500});
  ASSERT_TRUE(cu_cp_cfg.mobility.onboard_position_plan.plan_json_file.empty());
  ASSERT_TRUE(cu_cp_cfg.mobility.onboard_position_plan.state_file.empty());
}

TEST(cu_cp_unit_config, onboard_position_plan_is_an_independent_opt_in_profile)
{
  cu_cp_unit_config cfg;
  cfg.mobility_config.ntn_onboard_position_plan.enabled          = true;
  cfg.mobility_config.ntn_onboard_position_plan.du_execution_enabled = true;
  cfg.mobility_config.ntn_onboard_position_plan.satellite_id                    = "P01-S01";
  cfg.mobility_config.ntn_onboard_position_plan.plan_json_file   = "management-center-plan.json";
  cfg.mobility_config.ntn_onboard_position_plan.reload_period_ms = 2000;
  cfg.mobility_config.ntn_onboard_position_plan.du_prepare_guard_ms = 250;
  cfg.mobility_config.ntn_onboard_position_plan.du_prepare_horizon_ms = 3000;
  cfg.mobility_config.ntn_onboard_position_plan.du_apply_timeout_ms = 200;
  cfg.mobility_config.ntn_onboard_position_plan.cell_ncis        = {0x123450001ULL, 0x123450002ULL};
  cfg.mobility_config.ntn_onboard_position_plan.cell_pcis        = {101, 101};
  cfg.mobility_config.ntn_onboard_position_plan.max_l1_positions_per_cell       = 128;
  cfg.mobility_config.ntn_onboard_position_plan.max_l1_positions_per_satellite  = 256;
  cfg.mobility_config.ntn_onboard_position_plan.max_analog_ports_per_cell       = 16;
  cfg.mobility_config.ntn_onboard_position_plan.max_analog_ports_per_satellite  = 32;
  cfg.mobility_config.ntn_onboard_position_plan.max_digital_ports_per_cell      = 64;
  cfg.mobility_config.ntn_onboard_position_plan.max_digital_ports_per_satellite = 128;
  set_onboard_execution_context(cfg.mobility_config.ntn_onboard_position_plan);

  ASSERT_TRUE(validate_cu_cp_unit_config(cfg));
  const srs_cu_cp::cu_cp_configuration cu_cp_cfg = generate_cu_cp_config(cfg);
  YAML::Node                           yaml_root;
  fill_cu_cp_config_in_yaml_schema(yaml_root, cfg);
  const YAML::Node yaml_plan = yaml_root["cu_cp"]["mobility"]["ntn_onboard_position_plan"];

  EXPECT_FALSE(cu_cp_cfg.mobility.meas_manager_config.ntn_location_mobility.enabled);
  EXPECT_TRUE(cu_cp_cfg.mobility.onboard_position_plan.enabled);
  EXPECT_TRUE(cu_cp_cfg.mobility.onboard_position_plan.du_execution_enabled);
  EXPECT_EQ(cu_cp_cfg.mobility.onboard_position_plan.satellite_id, "P01-S01");
  EXPECT_EQ(cu_cp_cfg.mobility.onboard_position_plan.plan_json_file, "management-center-plan.json");
  EXPECT_EQ(cu_cp_cfg.mobility.onboard_position_plan.state_file, "ntn-onboard-position-plan-state.json");
  EXPECT_EQ(cu_cp_cfg.mobility.onboard_position_plan.expected_catalog_id, "global-land-l1-v1");
  EXPECT_EQ(cu_cp_cfg.mobility.onboard_position_plan.expected_catalog_hash,
            "sha256:b39fe9c3ee9a9355b3546036b7f16e0fb858c953f8558cc4295122f2169fbe7a");
  EXPECT_EQ(cu_cp_cfg.mobility.onboard_position_plan.expected_identity_registry_version,
            "mc-ntn-onboard-cell-registry-v1");
  EXPECT_EQ(cu_cp_cfg.mobility.onboard_position_plan.expected_identity_registry_hash,
            "sha256:7475821350e104b57a70d979d630f4b29a6cecb89ca0eca7b16dddf2ffee6a4a");
  EXPECT_EQ(cu_cp_cfg.mobility.onboard_position_plan.expected_access_profile_id, "ntn-access-16a-64d-v1");
  EXPECT_EQ(cu_cp_cfg.mobility.onboard_position_plan.expected_access_profile_hash,
            "sha256:195786f4161e3b0fad6faa0605144948a7401c067a014bde684c1b29a8087d63");
  EXPECT_EQ(cu_cp_cfg.mobility.onboard_position_plan.reload_period, std::chrono::milliseconds{2000});
  EXPECT_EQ(cu_cp_cfg.mobility.onboard_position_plan.du_prepare_guard, std::chrono::milliseconds{250});
  EXPECT_EQ(cu_cp_cfg.mobility.onboard_position_plan.du_prepare_horizon, std::chrono::milliseconds{3000});
  EXPECT_EQ(cu_cp_cfg.mobility.onboard_position_plan.du_apply_timeout, std::chrono::milliseconds{200});
  EXPECT_EQ(cu_cp_cfg.mobility.onboard_position_plan.cell_ncis[0].value(), 0x123450001ULL);
  EXPECT_EQ(cu_cp_cfg.mobility.onboard_position_plan.cell_ncis[1].value(), 0x123450002ULL);
  EXPECT_EQ(cu_cp_cfg.mobility.onboard_position_plan.cell_pcis[0], 101);
  EXPECT_EQ(cu_cp_cfg.mobility.onboard_position_plan.cell_pcis[1], 101);
  EXPECT_EQ(cu_cp_cfg.mobility.onboard_position_plan.max_l1_positions_per_cell, 128U);
  EXPECT_EQ(cu_cp_cfg.mobility.onboard_position_plan.max_l1_positions_per_satellite, 256U);
  EXPECT_EQ(cu_cp_cfg.mobility.onboard_position_plan.max_analog_ports_per_cell, 16U);
  EXPECT_EQ(cu_cp_cfg.mobility.onboard_position_plan.max_analog_ports_per_satellite, 32U);
  EXPECT_EQ(cu_cp_cfg.mobility.onboard_position_plan.max_digital_ports_per_cell, 64U);
  EXPECT_EQ(cu_cp_cfg.mobility.onboard_position_plan.max_digital_ports_per_satellite, 128U);
  EXPECT_EQ(cu_cp_cfg.mobility.onboard_position_plan.subvisit_duration, std::chrono::microseconds{2500});
  EXPECT_EQ(cu_cp_cfg.mobility.onboard_position_plan.activation_alignment, std::chrono::milliseconds{640});
  EXPECT_EQ(yaml_plan["expected_catalog_id"].as<std::string>(), "global-land-l1-v1");
  EXPECT_EQ(yaml_plan["state_file"].as<std::string>(), "ntn-onboard-position-plan-state.json");
  EXPECT_EQ(yaml_plan["expected_catalog_hash"].as<std::string>(),
            "sha256:b39fe9c3ee9a9355b3546036b7f16e0fb858c953f8558cc4295122f2169fbe7a");
  EXPECT_EQ(yaml_plan["expected_identity_registry_version"].as<std::string>(), "mc-ntn-onboard-cell-registry-v1");
  EXPECT_EQ(yaml_plan["expected_identity_registry_hash"].as<std::string>(),
            "sha256:7475821350e104b57a70d979d630f4b29a6cecb89ca0eca7b16dddf2ffee6a4a");
  EXPECT_EQ(yaml_plan["expected_access_profile_id"].as<std::string>(), "ntn-access-16a-64d-v1");
  EXPECT_EQ(yaml_plan["expected_access_profile_hash"].as<std::string>(),
            "sha256:195786f4161e3b0fad6faa0605144948a7401c067a014bde684c1b29a8087d63");
  EXPECT_EQ(yaml_plan["max_digital_ports_per_cell"].as<unsigned>(), 64U);
  EXPECT_EQ(yaml_plan["max_digital_ports_per_satellite"].as<unsigned>(), 128U);
}

TEST(cu_cp_unit_config, enabled_onboard_position_plan_requires_exactly_two_stable_cell_identities)
{
  cu_cp_unit_config cfg;
  cfg.mobility_config.ntn_onboard_position_plan.enabled        = true;
  cfg.mobility_config.ntn_onboard_position_plan.satellite_id   = "P01-S01";
  cfg.mobility_config.ntn_onboard_position_plan.plan_json_file = "management-center-plan.json";

  EXPECT_FALSE(validate_cu_cp_unit_config(cfg));

  cfg.mobility_config.ntn_onboard_position_plan.satellite_id = "P01-S001";
  cfg.mobility_config.ntn_onboard_position_plan.cell_ncis    = {0x123450001ULL, 0x123450002ULL};
  cfg.mobility_config.ntn_onboard_position_plan.cell_pcis    = {101, 202};
  EXPECT_FALSE(validate_cu_cp_unit_config(cfg));

  cfg.mobility_config.ntn_onboard_position_plan.satellite_id = "P01-S01";

  cfg.mobility_config.ntn_onboard_position_plan.cell_ncis = {0x123450001ULL, 0x123450001ULL};
  cfg.mobility_config.ntn_onboard_position_plan.cell_pcis = {101, 202};
  EXPECT_FALSE(validate_cu_cp_unit_config(cfg));

  cfg.mobility_config.ntn_onboard_position_plan.cell_ncis = {0x123450001ULL, 0x123450002ULL};
  cfg.mobility_config.ntn_onboard_position_plan.cell_pcis = {101, 101};
  EXPECT_TRUE(validate_cu_cp_unit_config(cfg));
}

TEST(cu_cp_unit_config, onboard_position_plan_state_file_is_execution_only_and_must_be_distinct)
{
  cu_cp_unit_config cfg;
  EXPECT_TRUE(validate_cu_cp_unit_config(cfg));

  auto& plan          = cfg.mobility_config.ntn_onboard_position_plan;
  plan.enabled        = true;
  plan.satellite_id   = "P01-S01";
  plan.plan_json_file = "management-center-plan.json";
  plan.cell_ncis      = {0x123450001ULL, 0x123450002ULL};
  plan.cell_pcis      = {101, 101};
  EXPECT_TRUE(validate_cu_cp_unit_config(cfg));

  plan.du_execution_enabled = true;
  set_onboard_planning_context(plan);
  EXPECT_FALSE(validate_cu_cp_unit_config(cfg));

  plan.state_file = plan.plan_json_file;
  EXPECT_FALSE(validate_cu_cp_unit_config(cfg));

  plan.state_file = "recovery/../management-center-plan.json";
  EXPECT_FALSE(validate_cu_cp_unit_config(cfg));

  plan.state_file = "ntn-onboard-position-plan-state.json";
  EXPECT_TRUE(validate_cu_cp_unit_config(cfg));
}

TEST(cu_cp_unit_config, du_calendar_execution_cannot_be_enabled_without_the_position_plan)
{
  cu_cp_unit_config cfg;
  cfg.mobility_config.ntn_onboard_position_plan.du_execution_enabled = true;

  EXPECT_FALSE(validate_cu_cp_unit_config(cfg));

  auto& plan          = cfg.mobility_config.ntn_onboard_position_plan;
  plan.enabled        = true;
  plan.satellite_id   = "P01-S01";
  plan.plan_json_file = "management-center-plan.json";
  plan.cell_ncis      = {0x123450001ULL, 0x123450002ULL};
  plan.cell_pcis      = {101, 101};
  set_onboard_execution_context(plan);
  plan.du_prepare_horizon_ms = plan.du_prepare_guard_ms;
  EXPECT_FALSE(validate_cu_cp_unit_config(cfg));

  plan.du_prepare_horizon_ms = 4000;
  plan.du_apply_timeout_ms   = 0;
  EXPECT_FALSE(validate_cu_cp_unit_config(cfg));

  plan.du_apply_timeout_ms = 500;
  EXPECT_TRUE(validate_cu_cp_unit_config(cfg));
}

TEST(cu_cp_unit_config, onboard_execution_rejects_legacy_identity_authority_and_incomplete_planning_context)
{
  cu_cp_unit_config cfg;
  auto&             plan = cfg.mobility_config.ntn_onboard_position_plan;
  plan.enabled           = true;
  plan.satellite_id      = "P01-S01";
  plan.plan_json_file    = "management-center-plan.json";
  plan.cell_ncis         = {0x123450001ULL, 0x123450002ULL};
  plan.cell_pcis         = {101, 101};

  plan.expected_catalog_id = "global-land-l1-v1";
  EXPECT_FALSE(validate_cu_cp_unit_config(cfg));
  plan.expected_catalog_id.clear();
  EXPECT_TRUE(validate_cu_cp_unit_config(cfg));

  plan.du_execution_enabled = true;
  set_onboard_execution_context(plan);
  cfg.mobility_config.ntn_location_mobility.enabled = true;
  EXPECT_FALSE(validate_cu_cp_unit_config(cfg));

  cfg.mobility_config.ntn_location_mobility.enabled = false;
  plan.expected_access_profile_hash = "Sha256:195786F4161E3B0FAD6FAA0605144948A7401C067A014BDE684C1B29A8087D63";
  EXPECT_TRUE(validate_cu_cp_unit_config(cfg));
}

TEST(cu_cp_unit_config, du_calendar_execution_rejects_profiles_outside_f1_and_scheduler_envelopes)
{
  cu_cp_unit_config cfg;
  auto&             plan          = cfg.mobility_config.ntn_onboard_position_plan;
  plan.enabled                    = true;
  plan.du_execution_enabled       = true;
  plan.satellite_id               = "P01-S01";
  plan.plan_json_file             = "management-center-plan.json";
  plan.cell_ncis                  = {0x123450001ULL, 0x123450002ULL};
  plan.cell_pcis                  = {101, 101};
  set_onboard_execution_context(plan);

  // The non-execution inventory can retain 257 positions, but the private F1 execution payload is bounded to 256.
  plan.max_l1_positions_per_cell      = 129;
  plan.max_l1_positions_per_satellite = 257;
  plan.max_analog_ports_per_cell      = 17;
  plan.max_analog_ports_per_satellite = 34;
  EXPECT_FALSE(validate_cu_cp_unit_config(cfg));
  plan.du_execution_enabled = false;
  EXPECT_TRUE(validate_cu_cp_unit_config(cfg));
  plan.du_execution_enabled = true;

  // 192 positions with a 40 ms SSB period generate 192 * (16 + 2) intents, beyond the 2560-intent payload limit.
  plan.max_l1_positions_per_cell      = 96;
  plan.max_l1_positions_per_satellite = 192;
  plan.max_analog_ports_per_cell      = 24;
  plan.max_analog_ports_per_satellite = 48;
  plan.max_ssb_interval_ms            = 40;
  EXPECT_FALSE(validate_cu_cp_unit_config(cfg));

  // A 1280 ms cycle fits the intent envelope below, but would require 20480 slots at NR numerology mu=4.
  plan.max_l1_positions_per_cell      = 64;
  plan.max_l1_positions_per_satellite = 128;
  plan.max_analog_ports_per_cell      = 16;
  plan.max_analog_ports_per_satellite = 32;
  plan.max_ssb_interval_ms            = 80;
  plan.max_prach_interval_ms          = 1280;
  EXPECT_FALSE(validate_cu_cp_unit_config(cfg));

  plan.max_l1_positions_per_cell      = 128;
  plan.max_l1_positions_per_satellite = 256;
  plan.max_prach_interval_ms          = 640;
  EXPECT_TRUE(validate_cu_cp_unit_config(cfg));
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

TEST(cu_cp_unit_config, multi_satellite_circular_orbit_ntn_config_is_accepted)
{
  const nr_cell_identity nci = make_nci(0);
  const std::filesystem::path beam_table_path =
      write_single_ntn_beam_table("srsran_ntn_unit_config_multi_sat_beam_table.json", nci);

  cu_cp_unit_config cfg;
  cfg.gnb_id = gnb_id_t{0x19b, 32};
  add_ntn_cell(cfg, nci);
  cfg.mobility_config.ntn_location_mobility.enabled                          = true;
  cfg.mobility_config.ntn_location_mobility.beam_table_json_file             = beam_table_path.string();
  cfg.mobility_config.ntn_location_mobility.satellite_state_source           = "circular_orbit";
  cfg.mobility_config.ntn_location_mobility.satellite_state_update_period_ms = 1000;
  cfg.mobility_config.ntn_location_mobility.predictive_service_window_horizon_ms = 10000;
  cfg.mobility_config.ntn_location_mobility.predictive_handover_lead_time_ms     = 3000;

  cu_cp_unit_ntn_circular_orbit_satellite_config sat_a;
  sat_a.satellite_id             = "sat-A";
  sat_a.altitude_m               = 500000.0;
  sat_a.inclination_deg          = 53.0;
  sat_a.raan_deg                 = 10.0;
  sat_a.argument_of_latitude_deg = 20.0;
  sat_a.epoch_unix_s             = 30.0;
  cu_cp_unit_ntn_circular_orbit_satellite_config sat_b = sat_a;
  sat_b.satellite_id                                 = "sat-B";
  sat_b.raan_deg                                     = 40.0;
  cfg.mobility_config.ntn_location_mobility.circular_orbit_satellites = {sat_a, sat_b};

  ASSERT_TRUE(validate_cu_cp_unit_config(cfg));

  YAML::Node yaml_root;
  fill_cu_cp_config_in_yaml_schema(yaml_root, cfg);
  const YAML::Node yaml_ntn = yaml_root["cu_cp"]["mobility"]["ntn_location_mobility"];
  ASSERT_EQ(yaml_ntn["circular_orbit_satellites"].size(), 2);
  ASSERT_EQ(yaml_ntn["circular_orbit_satellites"][0]["satellite_id"].as<std::string>(), "sat-A");

  const srs_cu_cp::cu_cp_configuration cu_cp_cfg = generate_cu_cp_config(cfg);
  const auto&                          sat_cfg =
      cu_cp_cfg.mobility.meas_manager_config.ntn_location_mobility.satellite_state_update;
  ASSERT_EQ(sat_cfg.source, srs_cu_cp::ntn_satellite_state_source::circular_orbit);
  ASSERT_EQ(sat_cfg.predictive_service_window_horizon, std::chrono::milliseconds(10000));
  ASSERT_EQ(sat_cfg.predictive_handover_lead_time, std::chrono::milliseconds(3000));
  ASSERT_EQ(sat_cfg.circular_orbit_satellites.size(), 2);
  ASSERT_EQ(sat_cfg.circular_orbit_satellites[0].satellite_id, "sat-A");
  ASSERT_DOUBLE_EQ(sat_cfg.circular_orbit_satellites[0].altitude_m, 500000.0);
  ASSERT_DOUBLE_EQ(sat_cfg.circular_orbit_satellites[0].raan_deg, 10.0);
  ASSERT_EQ(std::chrono::duration_cast<std::chrono::seconds>(
                sat_cfg.circular_orbit_satellites[0].epoch.time_since_epoch())
                .count(),
            30);
  ASSERT_EQ(sat_cfg.circular_orbit_satellites[1].satellite_id, "sat-B");
  ASSERT_DOUBLE_EQ(sat_cfg.circular_orbit_satellites[1].raan_deg, 40.0);
  ASSERT_EQ(yaml_ntn["predictive_service_window_horizon_ms"].as<unsigned>(), 10000U);
  ASSERT_EQ(yaml_ntn["predictive_handover_lead_time_ms"].as<unsigned>(), 3000U);

  std::filesystem::remove(beam_table_path);
}

TEST(cu_cp_unit_config, multi_satellite_circular_orbit_rejects_duplicate_satellite_id)
{
  const nr_cell_identity nci = make_nci(0);
  const std::filesystem::path beam_table_path =
      write_single_ntn_beam_table("srsran_ntn_unit_config_multi_sat_duplicate_beam_table.json", nci);

  cu_cp_unit_config cfg;
  cfg.gnb_id = gnb_id_t{0x19b, 32};
  add_ntn_cell(cfg, nci);
  cfg.mobility_config.ntn_location_mobility.enabled                          = true;
  cfg.mobility_config.ntn_location_mobility.beam_table_json_file             = beam_table_path.string();
  cfg.mobility_config.ntn_location_mobility.satellite_state_source           = "circular_orbit";
  cfg.mobility_config.ntn_location_mobility.satellite_state_update_period_ms = 1000;

  cu_cp_unit_ntn_circular_orbit_satellite_config sat;
  sat.satellite_id = "sat-A";
  cfg.mobility_config.ntn_location_mobility.circular_orbit_satellites = {sat, sat};

  ::testing::internal::CaptureStdout();
  const bool        valid  = validate_cu_cp_unit_config(cfg);
  const std::string output = ::testing::internal::GetCapturedStdout();
  std::filesystem::remove(beam_table_path);

  ASSERT_FALSE(valid);
  ASSERT_NE(output.find("circular orbit satellite list"), std::string::npos);
}

TEST(cu_cp_unit_config, ntn_location_mobility_accepts_zero_served_beam_limit_as_unlimited)
{
  const nr_cell_identity nci = make_nci(0);
  const std::filesystem::path beam_table_path =
      write_single_ntn_beam_table("srsran_ntn_unit_config_unlimited_beam_table.json", nci);

  cu_cp_unit_config cfg;
  cfg.gnb_id = gnb_id_t{0x19b, 32};
  add_ntn_cell(cfg, nci);
  cfg.mobility_config.ntn_location_mobility.enabled                = true;
  cfg.mobility_config.ntn_location_mobility.beam_table_json_file   = beam_table_path.string();
  cfg.mobility_config.ntn_location_mobility.max_nof_served_beams   = 0;
  cfg.mobility_config.ntn_location_mobility.satellite_state_source = "manual";

  ASSERT_TRUE(validate_cu_cp_unit_config(cfg));

  const srs_cu_cp::cu_cp_configuration cu_cp_cfg = generate_cu_cp_config(cfg);
  const auto&                          ntn_cfg = cu_cp_cfg.mobility.meas_manager_config.ntn_location_mobility;
  ASSERT_EQ(ntn_cfg.max_nof_served_beams, 0U);

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
  cfg.mobility_config.ntn_location_mobility.max_nof_served_beams             = 0;
  cfg.mobility_config.ntn_location_mobility.max_nof_active_analog_access_beams   = 16;
  cfg.mobility_config.ntn_location_mobility.max_nof_loaded_digital_service_beams = 256;
  cfg.mobility_config.ntn_location_mobility.preheated_beam_hold_time_ms          = 7000;
  cfg.mobility_config.ntn_location_mobility.preheated_beam_min_ready_time_ms     = 1100;
  cfg.mobility_config.ntn_location_mobility.analog_rebalance_pair_cooldown_ms    = 12000;
  cfg.mobility_config.ntn_location_mobility.digital_target_reservation_hold_time_ms = 8000;
  cfg.mobility_config.ntn_location_mobility.served_beam_hopping_enabled      = true;
  cfg.mobility_config.ntn_location_mobility.served_beam_hopping_dwell_updates = 3;
  cfg.mobility_config.ntn_location_mobility.multi_beam_load_balancing_enabled = true;
  cfg.mobility_config.ntn_location_mobility.demand_aware_beam_scheduling_enabled = true;
  cfg.mobility_config.ntn_location_mobility.multi_beam_headroom_admission_enabled = true;
  cfg.mobility_config.ntn_location_mobility.multi_beam_load_balancing_min_ue_delta = 2;
  cfg.mobility_config.ntn_location_mobility.multi_beam_load_balancing_max_handovers_per_eval = 1;
  cfg.mobility_config.ntn_location_mobility.multi_beam_load_balancing_handover_cooldown_ms = 30000;
  cfg.mobility_config.ntn_location_mobility.required_consecutive_location_reports = 3;
  cfg.mobility_config.ntn_location_mobility.time_to_trigger_ms                    = 1000;
  cfg.mobility_config.ntn_location_mobility.max_report_gap_ms                     = 750;
  cfg.mobility_config.ntn_location_mobility.boundary_hysteresis_m                 = 5000.0;
  cfg.mobility_config.ntn_location_mobility.core_network_reporting_local_forwarding_enabled = false;
  cfg.mobility_config.ntn_location_mobility.core_network_reporting_amf_control_enabled      = true;
  cfg.mobility_config.ntn_location_mobility.core_network_reporting_min_report_interval_ms   = 0;
  for (unsigned sector_id = 1; sector_id != 844; ++sector_id) {
    const nr_cell_identity nci = nr_cell_identity::create(gnb_id_t{0x19b, 22}, sector_id).value();
    add_ntn_cell(cfg, nci.value(), 22);
  }

  const srs_cu_cp::cu_cp_configuration cu_cp_cfg = generate_cu_cp_config(cfg);
  const auto&                          ntn_cfg = cu_cp_cfg.mobility.meas_manager_config.ntn_location_mobility;

  ASSERT_TRUE(ntn_cfg.enabled);
  ASSERT_EQ(ntn_cfg.analog_beams.size(), 137);
  ASSERT_EQ(std::count_if(ntn_cfg.analog_beams.begin(),
                          ntn_cfg.analog_beams.end(),
                          [](const srs_cu_cp::ntn_analog_beam_position& beam) {
                            return !beam.is_edge_partial;
                          }),
            109);
  ASSERT_EQ(std::count_if(ntn_cfg.analog_beams.begin(),
                          ntn_cfg.analog_beams.end(),
                          [](const srs_cu_cp::ntn_analog_beam_position& beam) {
                            return beam.is_edge_partial;
                          }),
            28);
  ASSERT_EQ(ntn_cfg.beams.size(), 843);

  std::map<std::string, const srs_cu_cp::ntn_beam_position*> beams_by_id;
  for (const auto& beam : ntn_cfg.beams) {
    beams_by_id.emplace(beam.beam_id, &beam);
  }
  std::set<std::string> child_digital_beam_ids;
  for (const auto& analog : ntn_cfg.analog_beams) {
    ASSERT_NE(beams_by_id.find(analog.center_digital_beam_id), beams_by_id.end());
    for (const auto& child_id : analog.child_digital_beam_ids) {
      auto child_it = beams_by_id.find(child_id);
      ASSERT_NE(child_it, beams_by_id.end());
      ASSERT_TRUE(child_digital_beam_ids.emplace(child_id).second);
      ASSERT_EQ(child_it->second->analog_beam_id, analog.analog_beam_id);
    }
  }
  ASSERT_EQ(child_digital_beam_ids.size(), ntn_cfg.beams.size());

  const auto full_analog_it =
      std::find_if(ntn_cfg.analog_beams.begin(),
                   ntn_cfg.analog_beams.end(),
                   [](const srs_cu_cp::ntn_analog_beam_position& analog) { return !analog.is_edge_partial; });
  ASSERT_NE(full_analog_it, ntn_cfg.analog_beams.end());
  const auto* full_center = beams_by_id.at(full_analog_it->center_digital_beam_id);
  ASSERT_TRUE(full_center->hex_q.has_value());
  ASSERT_TRUE(full_center->hex_r.has_value());
  std::set<std::pair<int, int>> child_offsets;
  for (const auto& child_id : full_analog_it->child_digital_beam_ids) {
    const auto* child = beams_by_id.at(child_id);
    ASSERT_TRUE(child->hex_q.has_value());
    ASSERT_TRUE(child->hex_r.has_value());
    child_offsets.emplace(child->hex_q.value() - full_center->hex_q.value(),
                          child->hex_r.value() - full_center->hex_r.value());
  }
  const std::set<std::pair<int, int>> expected_child_offsets = {
      {0, 0}, {1, 0}, {1, -1}, {0, -1}, {-1, 0}, {-1, 1}, {0, 1}};
  ASSERT_EQ(child_offsets, expected_child_offsets);

  ASSERT_EQ(ntn_cfg.beams.front().beam_id, "LEO500-DIGI-00001");
  ASSERT_EQ(ntn_cfg.beams.front().analog_beam_id, "LEO500-ANALOG-00001");
  ASSERT_TRUE(ntn_cfg.beams.front().hex_q.has_value());
  ASSERT_TRUE(ntn_cfg.beams.front().hex_r.has_value());
  ASSERT_DOUBLE_EQ(ntn_cfg.beams.front().coverage_radius_m, 15000.0);
  ASSERT_EQ(ntn_cfg.beams.back().beam_id, "LEO500-DIGI-00843");
  ASSERT_DOUBLE_EQ(ntn_cfg.beams.back().coverage_radius_m, 15000.0);
  ASSERT_DOUBLE_EQ(ntn_cfg.satellite_state_update.circular_altitude_m, 500000.0);
  ASSERT_EQ(ntn_cfg.satellite_state_update.update_period, std::chrono::milliseconds(1000));
  ASSERT_EQ(ntn_cfg.satellite_state_update.source, srs_cu_cp::ntn_satellite_state_source::circular_orbit);
  ASSERT_DOUBLE_EQ(ntn_cfg.served_beam_min_elevation_deg, 50.0);
  ASSERT_EQ(ntn_cfg.max_nof_served_beams, 0);
  ASSERT_EQ(ntn_cfg.max_nof_active_analog_access_beams, 16);
  ASSERT_EQ(ntn_cfg.max_nof_loaded_digital_service_beams, 256);
  ASSERT_EQ(ntn_cfg.preheated_beam_hold_time, std::chrono::milliseconds(7000));
  ASSERT_EQ(ntn_cfg.preheated_beam_min_ready_time, std::chrono::milliseconds(1100));
  ASSERT_EQ(ntn_cfg.analog_rebalance_pair_cooldown, std::chrono::milliseconds(12000));
  ASSERT_EQ(ntn_cfg.digital_target_reservation_hold_time, std::chrono::milliseconds(8000));
  ASSERT_TRUE(ntn_cfg.served_beam_hopping_enabled);
  ASSERT_EQ(ntn_cfg.served_beam_hopping_dwell_updates, 3);
  ASSERT_TRUE(ntn_cfg.multi_beam_load_balancing_enabled);
  ASSERT_TRUE(ntn_cfg.demand_aware_beam_scheduling_enabled);
  ASSERT_TRUE(ntn_cfg.multi_beam_headroom_admission_enabled);
  ASSERT_EQ(ntn_cfg.multi_beam_load_balancing_min_ue_delta, 2);
  ASSERT_EQ(ntn_cfg.multi_beam_load_balancing_max_handovers_per_eval, 1);
  ASSERT_EQ(ntn_cfg.multi_beam_load_balancing_handover_cooldown, std::chrono::milliseconds(30000));
  ASSERT_EQ(ntn_cfg.required_consecutive_location_reports, 3);
  ASSERT_EQ(ntn_cfg.time_to_trigger, std::chrono::milliseconds(1000));
  ASSERT_EQ(ntn_cfg.max_report_gap, std::chrono::milliseconds(750));
  ASSERT_DOUBLE_EQ(ntn_cfg.boundary_hysteresis_m, 5000.0);
  ASSERT_FALSE(ntn_cfg.core_network_reporting.local_forwarding_enabled);
  ASSERT_TRUE(ntn_cfg.core_network_reporting.amf_control_enabled);
  ASSERT_EQ(ntn_cfg.core_network_reporting.min_report_interval, std::chrono::milliseconds(0));
}

TEST(cu_cp_unit_config, leo_500km_example_yaml_uses_full_candidate_inventory_semantics)
{
  const YAML::Node root =
      YAML::LoadFile((get_project_source_root() / "configs/leo_500km_cucp_ntn.yml").string());
  const YAML::Node ntn = root["cu_cp"]["mobility"]["ntn_location_mobility"];

  ASSERT_TRUE(ntn["enabled"].as<bool>());
  ASSERT_DOUBLE_EQ(ntn["served_beam_min_elevation_deg"].as<double>(), 50.0);
  ASSERT_EQ(ntn["max_nof_served_beams"].as<unsigned>(), 0U);
  ASSERT_EQ(ntn["max_nof_active_analog_access_beams"].as<unsigned>(), 16U);
  ASSERT_EQ(ntn["max_nof_loaded_digital_service_beams"].as<unsigned>(), 256U);
  ASSERT_EQ(ntn["preheated_beam_hold_time_ms"].as<unsigned>(), 5000U);
  ASSERT_EQ(ntn["preheated_beam_min_ready_time_ms"].as<unsigned>(), 1000U);
  ASSERT_EQ(ntn["analog_rebalance_pair_cooldown_ms"].as<unsigned>(), 10000U);
  ASSERT_EQ(ntn["digital_target_reservation_hold_time_ms"].as<unsigned>(), 5000U);
  ASSERT_TRUE(ntn["served_beam_hopping_enabled"].as<bool>());
  ASSERT_TRUE(ntn["multi_beam_load_balancing_enabled"].as<bool>());
  ASSERT_TRUE(ntn["demand_aware_beam_scheduling_enabled"].as<bool>());
  ASSERT_TRUE(ntn["multi_beam_headroom_admission_enabled"].as<bool>());
  ASSERT_EQ(ntn["multi_beam_load_balancing_min_ue_delta"].as<unsigned>(), 2U);
  ASSERT_EQ(ntn["multi_beam_load_balancing_max_handovers_per_eval"].as<unsigned>(), 1U);
  ASSERT_EQ(ntn["multi_beam_load_balancing_handover_cooldown_ms"].as<unsigned>(), 30000U);
  ASSERT_EQ(ntn["satellite_state_source"].as<std::string>(), "circular_orbit");
  ASSERT_FALSE(ntn["core_network_reporting_local_forwarding_enabled"].as<bool>());
  ASSERT_TRUE(ntn["core_network_reporting_amf_control_enabled"].as<bool>());
}

TEST(cu_cp_unit_config, ntn_beams_command_uses_active_loaded_label_for_loaded_service_calendar)
{
  fake_cu_cp_command_handler command_handler;
  srs_cu_cp::cu_cp_ntn_beam_status candidate;
  candidate.beam_id      = "LEO500-BEAM-0001";
  candidate.analog_beam_id = "LEO500-ANALOG-0001";
  candidate.serving_satellite_id = "sat-A";
  candidate.analog_access_eligible = true;
  candidate.analog_access_reason   = "eligible";
  candidate.analog_edge_partial    = false;
  candidate.nci          = nr_cell_identity::create(gnb_id_t{0x19b, 22}, 0).value();
  candidate.state        = srs_cu_cp::cu_cp_ntn_beam_assignment_state::candidate;
  candidate.state_reason = "visible_without_loaded_demand";
  candidate.downlink_ready = true;
  candidate.downlink_visible = true;
  candidate.uplink_access_ready = false;
  candidate.access_roundtrip_ready = false;
  candidate.paired_uplink_access_ready = true;
  candidate.paired_uplink_beam_id      = "LEO500-BEAM-0002";
  candidate.paired_uplink_nci          = nr_cell_identity::create(gnb_id_t{0x19b, 22}, 1).value();
  candidate.paired_uplink_du_index     = srs_cu_cp::uint_to_du_index(1);
  candidate.access_pair_reason         = "paired_same_analog_tac";
  candidate.du_index              = srs_cu_cp::uint_to_du_index(0);
  candidate.access_du_index       = srs_cu_cp::uint_to_du_index(0);
  candidate.service_du_index      = srs_cu_cp::uint_to_du_index(0);
  candidate.du_assignment_reason  = "eligible";
  candidate.resource_domain_eligible = true;
  candidate.resource_domain_reason   = "eligible";
  candidate.reuse_group_id           = "reuse-a";
  candidate.conflict_group_ids       = {"conflict-a"};
  candidate.derived_tac  = 1;
  candidate.derived_tac_reason             = "valid";
  candidate.paging_recommendable           = true;
  candidate.paging_recommendation_reason   = "eligible";
  candidate.sib19_broadcast_state          = "desired";
  candidate.sib19_broadcast_reason         = "broadcastable";
  candidate.sib19_broadcast_generation     = 7;
  candidate.predictive_entry_offset         = std::chrono::milliseconds{2000};
  candidate.scheduling_score                = 100;
  candidate.window_rank                     = 1;
  candidate.scheduling_reason               = "legacy";

  srs_cu_cp::cu_cp_ntn_beam_status loaded;
  loaded.beam_id        = "LEO500-BEAM-0002";
  loaded.analog_beam_id = "LEO500-ANALOG-0001";
  loaded.serving_satellite_id = "sat-B";
  loaded.analog_access_eligible = true;
  loaded.analog_access_reason   = "eligible";
  loaded.analog_edge_partial    = false;
  loaded.nci            = nr_cell_identity::create(gnb_id_t{0x19b, 22}, 1).value();
  loaded.du_index                = srs_cu_cp::uint_to_du_index(1);
  loaded.access_du_index         = srs_cu_cp::uint_to_du_index(1);
  loaded.service_du_index        = srs_cu_cp::uint_to_du_index(1);
  loaded.du_assignment_reason  = "eligible";
  loaded.resource_domain_eligible = true;
  loaded.resource_domain_reason   = "eligible";
  loaded.reuse_group_id           = "reuse-a";
  loaded.conflict_group_ids       = {"conflict-b"};
  loaded.state          = srs_cu_cp::cu_cp_ntn_beam_assignment_state::active_loaded;
  loaded.state_reason   = "loaded_service_calendar";
  loaded.downlink_ready = true;
  loaded.uplink_ready   = true;
  loaded.downlink_visible = true;
  loaded.uplink_access_ready = true;
  loaded.access_roundtrip_ready = true;
  loaded.bidirectional_service_ready = true;
  loaded.nof_ues        = 1;
  loaded.nof_drbs       = 1;
  loaded.sr_slot_period = 1;
  loaded.srs_slot_period = 1;
  loaded.derived_tac     = 2;
  loaded.derived_tac_reason             = "valid";
  loaded.paging_recommendable           = true;
  loaded.paging_recommendation_reason   = "eligible";
  loaded.sib19_broadcast_state          = "applied_by_du";
  loaded.sib19_broadcast_reason         = "applied";
  loaded.sib19_broadcast_generation     = 8;
  loaded.predictive_exit_offset          = std::chrono::milliseconds{3000};
  loaded.scheduling_score                = 201000;
  loaded.window_rank                     = 0;
  loaded.scheduling_reason               = "demand";
  loaded.resource_weight                 = 7;
  loaded.resource_share                  = 0.75;
  loaded.resource_weight_reason          = "qos_weighted";
  loaded.headroom_reserved               = true;
  loaded.headroom_reason                 = "load_balancing";

  srs_cu_cp::cu_cp_ntn_beam_status invalid;
  invalid.beam_id                        = "LEO500-BEAM-A";
  invalid.analog_beam_id                 = "LEO500-ANALOG-EDGE";
  invalid.serving_satellite_id           = "sat-C";
  invalid.analog_access_reason           = "inactive";
  invalid.analog_edge_partial            = true;
  invalid.downlink_enabled               = false;
  invalid.uplink_enabled                 = true;
  invalid.downlink_visible               = false;
  invalid.uplink_access_ready            = true;
  invalid.access_roundtrip_ready         = false;
  invalid.nci                            = nr_cell_identity::create(gnb_id_t{0x19b, 22}, 2).value();
  invalid.access_du_index                = srs_cu_cp::uint_to_du_index(0);
  invalid.du_assignment_reason           = "service_du_differs_from_access_du";
  invalid.resource_domain_eligible       = false;
  invalid.resource_domain_reason         = "conflict_group_blocked";
  invalid.reuse_group_id                 = "reuse-b";
  invalid.conflict_group_ids             = {"conflict-a"};
  invalid.state                          = srs_cu_cp::cu_cp_ntn_beam_assignment_state::inactive;
  invalid.state_reason                   = "not_visible";
  invalid.derived_tac_reason             = "missing_decimal_suffix";
  invalid.paging_recommendation_reason   = "invalid_tac";
  invalid.sib19_broadcast_state          = "stale_blocked";
  invalid.sib19_broadcast_reason         = "stale_satellite_state";
  invalid.sib19_broadcast_generation     = 9;

  command_handler.ntn.beam_status = {candidate, loaded, invalid};

  ntn_beams_app_command command(command_handler);
  ::testing::internal::CaptureStdout();
  const std::array<std::string, 1> args = {"all"};
  command.execute(span<const std::string>{args});
  const std::string output = ::testing::internal::GetCapturedStdout();

  ASSERT_NE(output.find("active_loaded=1"), std::string::npos);
  ASSERT_NE(output.find("candidate=1"), std::string::npos);
  ASSERT_NE(output.find("NTN beam state guide: active_loaded=serving_or_ready candidate=visible_not_serving "
                        "draining=leaving_service inactive=not_currently_usable"),
            std::string::npos);
  ASSERT_NE(output.find("NTN link guide: link=both|downlink_only|uplink_only "
                        "downlink_ready=SIB19_or_paging_possible uplink_ready=UL_control_resources_possible "
                        "access_roundtrip_ready=UE_can_be_paged_and_answer "
                        "paired_access_ready=DL_page_with_separate_UL_response "
                        "bidirectional_service_ready=PDU_or_handover_target_possible"),
            std::string::npos);
  ASSERT_NE(output.find("NTN beam table guide: resource=capacity_or_conflict_check "
                        "headroom=capacity_reserved_for_moves sched_reason=why_selected"),
            std::string::npos);
  ASSERT_NE(output.find("downlink_ready=2 uplink_ready=1 downlink_visible=2 uplink_access_ready=2 "
                        "access_roundtrip_ready=1 paired_access_ready=1 downlink_only_without_ul_pair=0 "
                        "bidirectional_service_ready=1"),
            std::string::npos);
  ASSERT_NE(output.find("link"), std::string::npos);
  ASSERT_NE(output.find("dl_ready"), std::string::npos);
  ASSERT_NE(output.find("ul_ready"), std::string::npos);
  ASSERT_NE(output.find("svc_ready"), std::string::npos);
  ASSERT_NE(output.find("dl_visible"), std::string::npos);
  ASSERT_NE(output.find("ul_access"), std::string::npos);
  ASSERT_NE(output.find("roundtrip"), std::string::npos);
  ASSERT_NE(output.find("paired_ul"), std::string::npos);
  ASSERT_NE(output.find("pair_reason"), std::string::npos);
  ASSERT_NE(output.find("uplink_only"), std::string::npos);
  ASSERT_NE(output.find("tac"), std::string::npos);
  ASSERT_NE(output.find("paging"), std::string::npos);
  ASSERT_NE(output.find("sib19"), std::string::npos);
  ASSERT_NE(output.find("analog"), std::string::npos);
  ASSERT_NE(output.find("satellite"), std::string::npos);
  ASSERT_NE(output.find("entry_ms"), std::string::npos);
  ASSERT_NE(output.find("exit_ms"), std::string::npos);
  ASSERT_NE(output.find("score"), std::string::npos);
  ASSERT_NE(output.find("rank"), std::string::npos);
  ASSERT_NE(output.find("sched_reason"), std::string::npos);
  ASSERT_NE(output.find("res_w"), std::string::npos);
  ASSERT_NE(output.find("res_pct"), std::string::npos);
  ASSERT_NE(output.find("res_reason"), std::string::npos);
  ASSERT_NE(output.find("headroom"), std::string::npos);
  ASSERT_NE(output.find("headroom_reason"), std::string::npos);
  ASSERT_NE(output.find("load_balancing"), std::string::npos);
  ASSERT_NE(output.find("access_du"), std::string::npos);
  ASSERT_NE(output.find("service_du"), std::string::npos);
  ASSERT_NE(output.find("du_policy"), std::string::npos);
  ASSERT_NE(output.find("resource"), std::string::npos);
  ASSERT_NE(output.find("reuse"), std::string::npos);
  ASSERT_NE(output.find("conflict"), std::string::npos);
  ASSERT_NE(output.find("LEO500-ANALOG-0001"), std::string::npos);
  ASSERT_NE(output.find("LEO500-BEAM-0001"), std::string::npos);
  ASSERT_NE(output.find("sat-A"), std::string::npos);
  ASSERT_NE(output.find("2000"), std::string::npos);
  ASSERT_NE(output.find("candidate"), std::string::npos);
  ASSERT_NE(output.find("yes"), std::string::npos);
  ASSERT_NE(output.find("LEO500-BEAM-0002"), std::string::npos);
  ASSERT_NE(output.find("paired_same_analog_tac"), std::string::npos);
  ASSERT_NE(output.find("201000"), std::string::npos);
  ASSERT_NE(output.find("demand"), std::string::npos);
  ASSERT_NE(output.find("qos_weighted"), std::string::npos);
  ASSERT_NE(output.find("75.0"), std::string::npos);
  ASSERT_NE(output.find("sat-B"), std::string::npos);
  ASSERT_NE(output.find("3000"), std::string::npos);
  ASSERT_NE(output.find("active_loaded"), std::string::npos);
  ASSERT_NE(output.find("LEO500-BEAM-A"), std::string::npos);
  ASSERT_NE(output.find("missing_decimal_suffix"), std::string::npos);
  ASSERT_NE(output.find("invalid_tac"), std::string::npos);
  ASSERT_NE(output.find("applied_by_du"), std::string::npos);
  ASSERT_NE(output.find("stale_satellite_state"), std::string::npos);
  ASSERT_NE(output.find("service_du_differs_from_access_du"), std::string::npos);
  ASSERT_NE(output.find("reuse-a"), std::string::npos);
  ASSERT_NE(output.find("conflict-a"), std::string::npos);
  ASSERT_NE(output.find("conflict_group_blocked"), std::string::npos);
  ASSERT_EQ(output.find(" active=1"), std::string::npos);
}

TEST(cu_cp_unit_config, ntn_state_command_prints_service_area_paging_counters)
{
  fake_cu_cp_command_handler command_handler;
  command_handler.ntn.runtime.enabled                         = true;
  command_handler.ntn.runtime.satellite_state_available       = true;
  command_handler.ntn.runtime.assistance_valid                = true;
  command_handler.ntn.runtime.nof_total_beams                 = 3;
  command_handler.ntn.runtime.nof_candidate_beams             = 1;
  command_handler.ntn.runtime.nof_active_loaded_beams         = 1;
  command_handler.ntn.runtime.nof_inactive_beams              = 1;
  command_handler.ntn.runtime.nof_mobility_eligible_beams     = 1;
  command_handler.ntn.runtime.nof_downlink_ready_beams        = 2;
  command_handler.ntn.runtime.nof_uplink_ready_beams          = 1;
  command_handler.ntn.runtime.nof_downlink_visible_beams      = 2;
  command_handler.ntn.runtime.nof_uplink_access_ready_beams   = 2;
  command_handler.ntn.runtime.nof_access_roundtrip_ready_beams = 1;
  command_handler.ntn.runtime.nof_paired_uplink_access_ready_beams = 1;
  command_handler.ntn.runtime.nof_downlink_only_without_ul_pair_beams = 0;
  command_handler.ntn.runtime.nof_bidirectional_service_ready_beams = 1;
  command_handler.ntn.runtime.nof_valid_service_area_beams    = 2;
  command_handler.ntn.runtime.nof_invalid_service_area_beams  = 1;
  command_handler.ntn.runtime.nof_paging_recommendable_beams  = 1;
  command_handler.ntn.runtime.nof_total_analog_access_beams   = 137;
  command_handler.ntn.runtime.nof_full_analog_access_beams    = 109;
  command_handler.ntn.runtime.nof_partial_analog_access_beams = 28;
  command_handler.ntn.runtime.nof_active_analog_access_beams  = 16;
  command_handler.ntn.runtime.nof_loaded_service_beams         = 64;
  command_handler.ntn.runtime.nof_loaded_digital_service_beams = 64;
  command_handler.ntn.runtime.nof_access_du_assigned_analog_beams   = 12;
  command_handler.ntn.runtime.nof_access_du_unassigned_analog_beams = 4;
  command_handler.ntn.runtime.nof_same_du_service_beams             = 60;
  command_handler.ntn.runtime.nof_split_du_service_beams            = 4;
  command_handler.ntn.runtime.nof_pre_service_relocations_pending   = 2;
  command_handler.ntn.runtime.nof_pre_service_relocations_active    = 1;
  command_handler.ntn.runtime.nof_pre_service_relocations_blocked   = 3;
  command_handler.ntn.runtime.nof_connected_handovers_candidate     = 4;
  command_handler.ntn.runtime.nof_connected_handovers_preloaded     = 5;
  command_handler.ntn.runtime.nof_connected_handovers_resource_preparing = 42;
  command_handler.ntn.runtime.nof_connected_handovers_resource_applied   = 43;
  command_handler.ntn.runtime.nof_connected_handovers_blocked       = 6;
  command_handler.ntn.runtime.nof_connected_handovers_active        = 7;
  command_handler.ntn.runtime.nof_connected_handovers_rollback      = 44;
  command_handler.ntn.runtime.nof_ntn_access_only_ues                = 8;
  command_handler.ntn.runtime.nof_ntn_service_binding_pending_ues    = 9;
  command_handler.ntn.runtime.nof_ntn_service_binding_blocked_ues    = 10;
  command_handler.ntn.runtime.nof_ntn_service_bound_ues              = 11;
  command_handler.ntn.runtime.nof_ntn_location_bound_service_ues     = 12;
  command_handler.ntn.runtime.nof_ntn_access_cell_fallback_service_ues = 13;
  command_handler.ntn.runtime.nof_ntn_access_active_ues                = 18;
  command_handler.ntn.runtime.nof_ntn_control_only_ues                 = 19;
  command_handler.ntn.runtime.nof_ntn_analog_released_ues              = 20;
  command_handler.ntn.runtime.nof_ntn_digital_service_bound_ues        = 21;
  command_handler.ntn.runtime.nof_ntn_release_allowed_ues_requested    = 66;
  command_handler.ntn.runtime.nof_ntn_release_allowed_ues_scheduled    = 67;
  command_handler.ntn.runtime.nof_ntn_release_allowed_ues_skipped      = 68;
  command_handler.ntn.runtime.nof_ntn_load_balancing_evaluations       = 142;
  command_handler.ntn.runtime.nof_ntn_load_balancing_admission_steered = 143;
  command_handler.ntn.runtime.nof_ntn_load_balancing_handover_requested = 144;
  command_handler.ntn.runtime.nof_ntn_load_balancing_handover_scheduled = 145;
  command_handler.ntn.runtime.nof_ntn_load_balancing_handover_skipped   = 146;
  command_handler.ntn.runtime.nof_ntn_load_balancing_same_analog_scheduled = 147;
  command_handler.ntn.runtime.nof_ntn_load_balancing_cross_analog_scheduled = 148;
  command_handler.ntn.runtime.nof_ntn_load_balancing_skipped_projected_capacity = 149;
  command_handler.ntn.runtime.nof_ntn_load_balancing_skipped_cold_analog = 150;
  command_handler.ntn.runtime.last_ntn_load_balancing_source_beam_id    = "LEO500-DIGI-00001";
  command_handler.ntn.runtime.last_ntn_load_balancing_target_beam_id    = "LEO500-DIGI-00002";
  command_handler.ntn.runtime.last_ntn_load_balancing_source_analog_id  = "LEO500-ANALOG-00001";
  command_handler.ntn.runtime.last_ntn_load_balancing_target_analog_id  = "LEO500-ANALOG-00002";
  command_handler.ntn.runtime.last_ntn_load_balancing_reason            = "cross_analog_scheduled";
  command_handler.ntn.runtime.nof_ntn_preheat_requested                 = 160;
  command_handler.ntn.runtime.nof_ntn_preheat_sent                      = 161;
  command_handler.ntn.runtime.nof_ntn_preheat_applied                   = 162;
  command_handler.ntn.runtime.nof_ntn_preheat_skipped                   = 163;
  command_handler.ntn.runtime.nof_ntn_preheat_demoted                   = 164;
  command_handler.ntn.runtime.nof_ntn_preheat_skipped_by_capacity       = 165;
  command_handler.ntn.runtime.nof_ntn_preheat_skipped_by_policy         = 166;
  command_handler.ntn.runtime.nof_ntn_cold_analog_preheat_requested     = 167;
  command_handler.ntn.runtime.last_ntn_preheat_reason                   = "idle_demoted";
  command_handler.ntn.runtime.last_ntn_preheat_source_analog_id         = "LEO500-ANALOG-00001";
  command_handler.ntn.runtime.last_ntn_preheat_target_analog_id         = "LEO500-ANALOG-00002";
  command_handler.ntn.runtime.nof_ntn_target_reservation_created        = 168;
  command_handler.ntn.runtime.nof_ntn_target_reservation_held           = 169;
  command_handler.ntn.runtime.nof_ntn_target_reservation_consumed       = 170;
  command_handler.ntn.runtime.nof_ntn_target_reservation_expired        = 171;
  command_handler.ntn.runtime.nof_ntn_admission_blocked_by_target_reservation = 172;
  command_handler.ntn.runtime.nof_ntn_handover_skipped_by_pair_cooldown        = 173;
  command_handler.ntn.runtime.nof_ntn_handover_skipped_by_preheat_ready_guard   = 174;
  command_handler.ntn.runtime.nof_ntn_preheat_demote_deferred_by_reservation   = 175;
  command_handler.ntn.runtime.last_ntn_scheduling_guard_source_beam_id         = "LEO500-DIGI-00003";
  command_handler.ntn.runtime.last_ntn_scheduling_guard_target_beam_id         = "LEO500-DIGI-00004";
  command_handler.ntn.runtime.last_ntn_scheduling_guard_source_analog_id       = "LEO500-ANALOG-00003";
  command_handler.ntn.runtime.last_ntn_scheduling_guard_target_analog_id       = "LEO500-ANALOG-00004";
  command_handler.ntn.runtime.last_ntn_scheduling_guard_reason                 = "preheat_ready_guard";
  command_handler.ntn.runtime.nof_ntn_beam_scheduling_evaluations       = 151;
  command_handler.ntn.runtime.nof_ntn_beam_scheduling_demand_prioritized_windows = 152;
  command_handler.ntn.runtime.nof_ntn_beam_scheduling_legacy_fallback   = 153;
  command_handler.ntn.runtime.nof_ntn_beam_scheduling_sticky_kept       = 154;
  command_handler.ntn.runtime.last_ntn_beam_scheduling_reason           = "demand_prioritized";
  command_handler.ntn.runtime.nof_ntn_resource_weighting_evaluations    = 151;
  command_handler.ntn.runtime.nof_ntn_resource_weighting_weighted_beams  = 152;
  command_handler.ntn.runtime.nof_ntn_resource_weighting_qos_boosted_beams = 153;
  command_handler.ntn.runtime.nof_ntn_resource_weighting_legacy_fallback = 154;
  command_handler.ntn.runtime.last_ntn_resource_weighting_reason         = "weighted";
  command_handler.ntn.runtime.nof_ntn_headroom_evaluations        = 155;
  command_handler.ntn.runtime.nof_ntn_headroom_reserved_beams     = 156;
  command_handler.ntn.runtime.nof_ntn_headroom_admission_allowed  = 157;
  command_handler.ntn.runtime.nof_ntn_headroom_admission_blocked  = 158;
  command_handler.ntn.runtime.nof_ntn_headroom_handover_protected = 159;
  command_handler.ntn.runtime.last_ntn_headroom_reason            = "reserved_for_handover_target";
  command_handler.ntn.runtime.multi_satellite_window_valid              = true;
  command_handler.ntn.runtime.nof_ntn_current_window_satellites         = 2;
  command_handler.ntn.runtime.nof_ntn_next_window_satellites            = 3;
  command_handler.ntn.runtime.nof_ntn_multi_satellite_visible_beams     = 4;
  command_handler.ntn.runtime.nof_ntn_satellite_owner_changes           = 5;
  command_handler.ntn.runtime.ntn_predictive_service_window_horizon      = std::chrono::milliseconds{10000};
  command_handler.ntn.runtime.ntn_predictive_handover_lead_time          = std::chrono::milliseconds{3000};
  command_handler.ntn.runtime.nof_ntn_predictive_timeline_steps          = 10;
  command_handler.ntn.runtime.nof_ntn_predictive_timeline_entry_beams    = 6;
  command_handler.ntn.runtime.nof_ntn_predictive_timeline_exit_beams     = 7;
  command_handler.ntn.runtime.earliest_ntn_predictive_upcoming_offset    = std::chrono::milliseconds{2000};
  command_handler.ntn.runtime.earliest_ntn_predictive_drain_offset       = std::chrono::milliseconds{3000};
  command_handler.ntn.runtime.nof_ntn_location_fresh_ues               = 81;
  command_handler.ntn.runtime.nof_ntn_location_missing_ues             = 82;
  command_handler.ntn.runtime.nof_ntn_location_stale_ues               = 83;
  command_handler.ntn.runtime.nof_ntn_location_release_pending_ues     = 84;
  command_handler.ntn.runtime.nof_ntn_location_watchdog_evaluations    = 85;
  command_handler.ntn.runtime.nof_ntn_location_watchdog_refresh_requested = 86;
  command_handler.ntn.runtime.nof_ntn_location_watchdog_release_requested = 87;
  command_handler.ntn.runtime.nof_ntn_location_watchdog_release_scheduled = 88;
  command_handler.ntn.runtime.nof_ntn_location_watchdog_release_skipped   = 89;
  command_handler.ntn.runtime.last_ntn_location_watchdog_release_reason   = "last_location_expired";
  command_handler.ntn.runtime.nof_ntn_capability_supported_ues         = 57;
  command_handler.ntn.runtime.nof_ntn_capability_unsupported_ues       = 58;
  command_handler.ntn.runtime.nof_ntn_capability_unknown_ues           = 59;
  command_handler.ntn.runtime.nof_ntn_capability_parse_failed_ues      = 60;
  command_handler.ntn.runtime.nof_ntn_capability_ngso_ues              = 61;
  command_handler.ntn.runtime.nof_ntn_capability_gso_ues               = 62;
  command_handler.ntn.runtime.nof_ntn_capability_both_ues              = 63;
  command_handler.ntn.runtime.nof_ntn_capability_implicit_both_ues     = 64;
  command_handler.ntn.runtime.nof_ntn_capability_profile_blocked_ues   = 65;
  command_handler.ntn.runtime.nof_ntn_nrppa_dl_ue_received             = 69;
  command_handler.ntn.runtime.nof_ntn_nrppa_dl_ue_forwarded            = 70;
  command_handler.ntn.runtime.nof_ntn_nrppa_dl_ue_dropped              = 71;
  command_handler.ntn.runtime.nof_ntn_nrppa_dl_non_ue_received         = 72;
  command_handler.ntn.runtime.nof_ntn_nrppa_dl_non_ue_forwarded        = 73;
  command_handler.ntn.runtime.nof_ntn_nrppa_dl_non_ue_dropped          = 74;
  command_handler.ntn.runtime.nof_ntn_nrppa_ul_ue_received             = 75;
  command_handler.ntn.runtime.nof_ntn_nrppa_ul_ue_sent                 = 76;
  command_handler.ntn.runtime.nof_ntn_nrppa_ul_ue_dropped              = 77;
  command_handler.ntn.runtime.nof_ntn_nrppa_ul_non_ue_received         = 78;
  command_handler.ntn.runtime.nof_ntn_nrppa_ul_non_ue_sent             = 79;
  command_handler.ntn.runtime.nof_ntn_nrppa_ul_non_ue_dropped          = 80;
  command_handler.ntn.runtime.last_ntn_nrppa_dropped_reason            = "unknown_amf";
  command_handler.ntn.runtime.nof_ntn_nrppa_trp_requests_received      = 111;
  command_handler.ntn.runtime.nof_ntn_nrppa_trp_requests_decoded       = 112;
  command_handler.ntn.runtime.nof_ntn_nrppa_trp_responses_sent         = 113;
  command_handler.ntn.runtime.nof_ntn_nrppa_trp_failures_sent          = 114;
  command_handler.ntn.runtime.nof_ntn_nrppa_unsupported_procedures     = 115;
  command_handler.ntn.runtime.nof_ntn_nrppa_trp_unsupported_info_items = 116;
  command_handler.ntn.runtime.nof_ntn_nrppa_trp_empty_results          = 117;
  command_handler.ntn.runtime.last_ntn_nrppa_trp_reason                = "empty_result";
  command_handler.ntn.runtime.nof_ntn_nrppa_positioning_info_requests_received  = 118;
  command_handler.ntn.runtime.nof_ntn_nrppa_positioning_info_requests_decoded   = 119;
  command_handler.ntn.runtime.nof_ntn_nrppa_positioning_info_requests_forwarded = 120;
  command_handler.ntn.runtime.nof_ntn_nrppa_positioning_info_responses_sent     = 121;
  command_handler.ntn.runtime.nof_ntn_nrppa_positioning_info_failures_sent      = 122;
  command_handler.ntn.runtime.nof_ntn_nrppa_positioning_info_dropped            = 123;
  command_handler.ntn.runtime.last_ntn_nrppa_positioning_info_reason            = "f1ap_failure";
  command_handler.ntn.runtime.nof_ntn_nrppa_measurement_requests_received       = 124;
  command_handler.ntn.runtime.nof_ntn_nrppa_measurement_requests_decoded        = 125;
  command_handler.ntn.runtime.nof_ntn_nrppa_measurement_requests_forwarded      = 126;
  command_handler.ntn.runtime.nof_ntn_nrppa_measurement_responses_sent          = 127;
  command_handler.ntn.runtime.nof_ntn_nrppa_measurement_failures_sent           = 128;
  command_handler.ntn.runtime.nof_ntn_nrppa_measurement_dropped                 = 129;
  command_handler.ntn.runtime.last_ntn_nrppa_measurement_reason                 = "responded";
  command_handler.ntn.runtime.nof_ntn_nrppa_activation_requests_received        = 130;
  command_handler.ntn.runtime.nof_ntn_nrppa_activation_requests_decoded         = 131;
  command_handler.ntn.runtime.nof_ntn_nrppa_activation_requests_forwarded       = 132;
  command_handler.ntn.runtime.nof_ntn_nrppa_activation_responses_sent           = 133;
  command_handler.ntn.runtime.nof_ntn_nrppa_activation_failures_sent            = 134;
  command_handler.ntn.runtime.nof_ntn_nrppa_activation_dropped                  = 135;
  command_handler.ntn.runtime.last_ntn_nrppa_activation_reason                  = "responded";
  command_handler.ntn.runtime.nof_ntn_nrppa_deactivation_requests_received      = 136;
  command_handler.ntn.runtime.nof_ntn_nrppa_deactivation_requests_decoded       = 137;
  command_handler.ntn.runtime.nof_ntn_nrppa_deactivation_requests_forwarded     = 138;
  command_handler.ntn.runtime.nof_ntn_nrppa_deactivation_acks_sent              = 139;
  command_handler.ntn.runtime.nof_ntn_nrppa_deactivation_failures_sent          = 140;
  command_handler.ntn.runtime.nof_ntn_nrppa_deactivation_dropped                = 141;
  command_handler.ntn.runtime.last_ntn_nrppa_deactivation_reason                = "acked";
  command_handler.ntn.runtime.nof_ntn_idle_paging_contexts             = 90;
  command_handler.ntn.runtime.nof_ntn_idle_paging_contexts_with_5g_s_tmsi = 91;
  command_handler.ntn.runtime.nof_ntn_idle_paging_contexts_expired      = 92;
  command_handler.ntn.runtime.nof_ntn_idle_paging_ue_hits               = 93;
  command_handler.ntn.runtime.nof_ntn_idle_paging_tac_fallbacks         = 94;
  command_handler.ntn.runtime.nof_ntn_idle_paging_recommendations       = 95;
  command_handler.ntn.runtime.nof_ntn_idle_paging_skipped               = 96;
  command_handler.ntn.runtime.last_ntn_idle_paging_reason               = "last_beam_unpageable";
  command_handler.ntn.runtime.nof_ntn_paired_access_contexts            = 176;
  command_handler.ntn.runtime.nof_ntn_paired_access_responses           = 177;
  command_handler.ntn.runtime.nof_ntn_service_bindings_from_paired_access = 178;
  command_handler.ntn.runtime.nof_ntn_paired_access_blocked             = 179;
  command_handler.ntn.runtime.last_ntn_paired_access_reason             = "service_bound";
  command_handler.ntn.runtime.nof_ntn_service_pair_bound_ues            = 180;
  command_handler.ntn.runtime.nof_ntn_service_pair_blocked_ues          = 181;
  command_handler.ntn.runtime.last_ntn_service_pair_reason              = "service_pair_capacity_blocked";
  command_handler.ntn.runtime.nof_ntn_service_pair_handover_targets     = 182;
  command_handler.ntn.runtime.nof_ntn_service_pair_handover_scheduled   = 183;
  command_handler.ntn.runtime.nof_ntn_service_pair_handover_skipped     = 184;
  command_handler.ntn.runtime.nof_ntn_service_pair_handover_committed   = 185;
  command_handler.ntn.runtime.nof_ntn_service_pair_handover_rolled_back = 186;
  command_handler.ntn.runtime.nof_ntn_service_pair_handover_context_cleared = 187;
  command_handler.ntn.runtime.last_ntn_service_pair_handover_reason     = "same_analog_tac_uplink_resource";
  command_handler.ntn.runtime.last_ntn_service_pair_handover_completion_reason = "committed";
  command_handler.ntn.runtime.nof_ntn_service_pair_resource_audit_targets    = 188;
  command_handler.ntn.runtime.nof_ntn_service_pair_resource_audit_mismatches = 189;
  command_handler.ntn.runtime.nof_ntn_service_pair_resource_audit_repairs    = 190;
  command_handler.ntn.runtime.nof_ntn_service_pair_resource_audit_skipped    = 191;
  command_handler.ntn.runtime.last_ntn_service_pair_resource_audit_reason =
      "du_missing_service_pair_ul_sr_srs_assignment";
  command_handler.ntn.runtime.ntn_resource_audit_generation             = 194;
  command_handler.ntn.runtime.nof_ntn_resource_audit_queries_sent       = 195;
  command_handler.ntn.runtime.nof_ntn_resource_audit_responses_accepted = 196;
  command_handler.ntn.runtime.nof_ntn_resource_audit_mismatches         = 197;
  command_handler.ntn.runtime.nof_ntn_resource_audit_repair_actions     = 198;
  command_handler.ntn.runtime.nof_ntn_resource_audit_failures           = 199;
  command_handler.ntn.runtime.nof_ntn_resource_audit_rnti_incomplete    = 200;
  command_handler.ntn.runtime.nof_ntn_resource_audit_ue_slot_incomplete = 201;
  command_handler.ntn.runtime.last_ntn_resource_audit_reason             = "ue_slot_snapshot_incomplete";
  command_handler.ntn.runtime.nof_ntn_inactive_contexts                 = 97;
  command_handler.ntn.runtime.nof_ntn_inactive_contexts_expired         = 98;
  command_handler.ntn.runtime.nof_ntn_inactive_suspend_requested        = 99;
  command_handler.ntn.runtime.nof_ntn_inactive_suspend_succeeded        = 100;
  command_handler.ntn.runtime.nof_ntn_inactive_suspend_failed           = 101;
  command_handler.ntn.runtime.nof_ntn_inactive_resume_requested         = 102;
  command_handler.ntn.runtime.nof_ntn_inactive_resume_succeeded         = 103;
  command_handler.ntn.runtime.nof_ntn_inactive_resume_failed            = 104;
  command_handler.ntn.runtime.nof_ntn_inactive_ngap_suspend_responses   = 105;
  command_handler.ntn.runtime.nof_ntn_inactive_ngap_suspend_failures    = 106;
  command_handler.ntn.runtime.nof_ntn_inactive_ngap_resume_responses    = 107;
  command_handler.ntn.runtime.nof_ntn_inactive_ngap_resume_failures     = 108;
  command_handler.ntn.runtime.nof_ntn_inactive_paging_hits              = 109;
  command_handler.ntn.runtime.nof_ntn_inactive_fallback_releases        = 110;
  command_handler.ntn.runtime.last_ntn_inactive_reason                  = "native_resume_complete";
  command_handler.ntn.runtime.nof_resource_domain_analog_cap_blocked   = 14;
  command_handler.ntn.runtime.nof_resource_domain_digital_cap_blocked  = 15;
  command_handler.ntn.runtime.nof_resource_domain_conflict_blocked     = 16;
  command_handler.ntn.runtime.nof_active_reuse_groups                  = 17;
  command_handler.ntn.runtime.nof_sib19_broadcast_desired              = 51;
  command_handler.ntn.runtime.nof_sib19_broadcast_sent_to_du           = 52;
  command_handler.ntn.runtime.nof_sib19_broadcast_applied_by_du        = 53;
  command_handler.ntn.runtime.nof_sib19_broadcast_rejected_by_du       = 54;
  command_handler.ntn.runtime.nof_sib19_broadcast_cleared_by_du        = 55;
  command_handler.ntn.runtime.nof_sib19_broadcast_stale_blocked        = 56;
  command_handler.ntn.resource_snapshot.nof_access_rnti_owned          = 22;
  command_handler.ntn.resource_snapshot.nof_access_rnti_conflicts      = 23;
  command_handler.ntn.resource_snapshot.nof_digital_slot_active        = 24;
  command_handler.ntn.resource_snapshot.nof_digital_slot_cleared       = 25;
  command_handler.ntn.resource_snapshot.nof_digital_slot_sent_to_du     = 37;
  command_handler.ntn.resource_snapshot.nof_digital_slot_applied_by_du  = 38;
  command_handler.ntn.resource_snapshot.nof_digital_slot_rejected_by_du = 39;
  command_handler.ntn.resource_snapshot.nof_digital_slot_cleared_by_du  = 40;
  command_handler.ntn.resource_snapshot.nof_digital_slot_rollback       = 41;
  command_handler.ntn.resource_snapshot.nof_rnti_leases_reserved       = 26;
  command_handler.ntn.resource_snapshot.nof_rnti_leases_available      = 27;
  command_handler.ntn.resource_snapshot.nof_rnti_leases_sent_to_du     = 28;
  command_handler.ntn.resource_snapshot.nof_rnti_leases_applied_by_du  = 29;
  command_handler.ntn.resource_snapshot.nof_rnti_leases_rejected_by_du = 30;
  command_handler.ntn.resource_snapshot.nof_rnti_leases_offered_in_rar = 31;
  command_handler.ntn.resource_snapshot.nof_rnti_leases_consumed_by_du = 42;
  command_handler.ntn.resource_snapshot.nof_rnti_leases_initial_ul_seen = 32;
  command_handler.ntn.resource_snapshot.nof_rnti_leases_committed      = 33;
  command_handler.ntn.resource_snapshot.nof_rnti_leases_released       = 34;
  command_handler.ntn.resource_snapshot.nof_rnti_leases_expired        = 35;
  command_handler.ntn.resource_snapshot.nof_rnti_leases_conflict       = 36;
  command_handler.ntn.resource_snapshot.nof_resource_repairs_queued    = 45;
  command_handler.ntn.resource_snapshot.nof_resource_repairs_sent      = 46;
  command_handler.ntn.resource_snapshot.nof_resource_repairs_applied   = 47;
  command_handler.ntn.resource_snapshot.nof_resource_repairs_failed    = 48;
  command_handler.ntn.resource_snapshot.nof_resource_repairs_retry_exhausted  = 49;
  command_handler.ntn.resource_snapshot.nof_resource_repairs_blocked_conflict = 50;
  command_handler.ntn.resource_snapshot.nof_service_pair_digital_slot_intents = 192;
  command_handler.ntn.resource_snapshot.nof_service_pair_resource_repairs     = 193;

  ntn_state_app_command command(command_handler);
  ::testing::internal::CaptureStdout();
  const std::array<std::string, 0> args = {};
  command.execute(span<const std::string>{args});
  const std::string output = ::testing::internal::GetCapturedStdout();

  ASSERT_NE(output.find("NTN readable summary: access_ready=yes service_ready=yes move_waiting=53 "
                        "move_active=8 reserved_capacity=yes safety_guard_active=yes"),
            std::string::npos);
  ASSERT_NE(output.find("NTN term guide: active_loaded=serving_or_ready candidate=visible_not_serving "
                        "draining=leaving_service inactive=not_currently_usable"),
            std::string::npos);
  ASSERT_NE(output.find("NTN operation guide: preheat=prepare_target_beams "
                        "headroom=capacity_reserved_for_moves reservation=reserved_move_capacity "
                        "scheduling_guard=anti_ping_pong_safety resource_domain=capacity_or_conflict_limits"),
            std::string::npos);
  ASSERT_NE(output.find("NTN beams: total=3 candidate=1 active_loaded=1 loaded_service=64 draining=0 inactive=1 "
                        "mobility_eligible=1 downlink_ready=2 uplink_ready=1 downlink_visible=2 "
                        "uplink_access_ready=2 access_roundtrip_ready=1 paired_access_ready=1 "
                        "downlink_only_without_ul_pair=0 bidirectional_service_ready=1"),
            std::string::npos);
  ASSERT_NE(output.find("NTN link guide: downlink_ready=SIB19_or_paging_possible "
                        "uplink_ready=UL_control_resources_possible "
                        "access_roundtrip_ready=UE_can_be_paged_and_answer "
                        "paired_access_ready=DL_page_with_separate_UL_response "
                        "bidirectional_service_ready=PDU_or_handover_target_possible"),
            std::string::npos);
  ASSERT_NE(output.find("NTN service area: valid_tac=2 invalid_tac=1 paging_recommendable=1"), std::string::npos);
  ASSERT_NE(output.find("NTN analog access: total=137 full=109 partial=28 active=16"), std::string::npos);
  ASSERT_NE(output.find("loaded_digital=64"), std::string::npos);
  ASSERT_NE(output.find("NTN access DU: assigned_analog=12 unassigned_analog=4 same_du_service=60 split_du_service=4"),
            std::string::npos);
  ASSERT_NE(output.find("NTN pre-service relocation: pending=2 active=1 blocked=3"), std::string::npos);
  ASSERT_NE(output.find("NTN connected handover: candidate=4 preloaded=5 resource_preparing=42 resource_applied=43 active=7 blocked=6 rollback=44"),
            std::string::npos);
  ASSERT_NE(output.find("NTN UE access/service: access_only=8 binding_pending=9 binding_blocked=10 service_bound=11"),
            std::string::npos);
  ASSERT_NE(output.find("NTN UE ownership: access_active=18 control_only=19 analog_released=20 digital_service_bound=21"),
            std::string::npos);
  ASSERT_NE(output.find("binding_source: location=12 access_cell_fallback=13"), std::string::npos);
  ASSERT_NE(output.find("NTN load_balancing: evaluations=142 admission_steered=143 handover_requested=144 "
                        "scheduled=145 skipped=146 same_analog_scheduled=147 cross_analog_scheduled=148 "
                        "skipped_projected_capacity=149 skipped_cold_analog=150 source_beam=LEO500-DIGI-00001 "
                        "target_beam=LEO500-DIGI-00002 source_analog=LEO500-ANALOG-00001 "
                        "target_analog=LEO500-ANALOG-00002 last_reason=cross_analog_scheduled"),
            std::string::npos);
  ASSERT_NE(output.find("NTN preheat: requested=160 sent=161 applied=162 skipped=163 demoted=164 "
                        "skipped_by_capacity=165 skipped_by_policy=166 cold_analog_requested=167 "
                        "source_analog=LEO500-ANALOG-00001 target_analog=LEO500-ANALOG-00002 "
                        "last_reason=idle_demoted"),
            std::string::npos);
  ASSERT_NE(output.find("NTN scheduling_guard: reservation_created=168 held=169 consumed=170 expired=171 "
                        "admission_blocked_by_reservation=172 skipped_pair_cooldown=173 "
                        "skipped_preheat_ready_guard=174 preheat_demote_deferred=175 "
                        "source_beam=LEO500-DIGI-00003 target_beam=LEO500-DIGI-00004 "
                        "source_analog=LEO500-ANALOG-00003 target_analog=LEO500-ANALOG-00004 "
                        "last_reason=preheat_ready_guard"),
            std::string::npos);
  ASSERT_NE(output.find("NTN beam_scheduling: evaluations=151 demand_prioritized=152 legacy_fallback=153 "
                        "sticky_kept=154 last_reason=demand_prioritized"),
            std::string::npos);
  ASSERT_NE(output.find("NTN resource_weighting: evaluations=151 weighted_beams=152 qos_boosted=153 "
                        "legacy_fallback=154 last_reason=weighted"),
            std::string::npos);
  ASSERT_NE(output.find("NTN headroom: evaluations=155 reserved_beams=156 admission_allowed=157 "
                        "admission_blocked=158 handover_protected=159 "
                        "last_reason=reserved_for_handover_target"),
            std::string::npos);
  ASSERT_NE(output.find("NTN release_allowed: requested=66 scheduled=67 skipped=68"), std::string::npos);
  ASSERT_NE(output.find("NTN UE location_watchdog: fresh=81 missing=82 stale=83 release_pending=84 "
                        "evaluations=85 refresh_requested=86 release_requested=87 scheduled=88 skipped=89 "
                        "last_reason=last_location_expired"),
            std::string::npos);
  ASSERT_NE(output.find("NTN UE capability: supported=57 unsupported=58 unknown=59 parse_failed=60"),
            std::string::npos);
  ASSERT_NE(output.find("NTN UE capability profile: ngso=61 gso=62 both=63 implicit_both=64 blocked=65"),
            std::string::npos);
  ASSERT_NE(output.find("NTN NRPPa transport: dl_ue_received=69 dl_ue_forwarded=70 dl_ue_dropped=71 "
                        "dl_non_ue_received=72 dl_non_ue_forwarded=73 dl_non_ue_dropped=74 "
                        "ul_ue_received=75 ul_ue_sent=76 ul_ue_dropped=77 "
                        "ul_non_ue_received=78 ul_non_ue_sent=79 ul_non_ue_dropped=80 last_drop=unknown_amf "
	                        "trp_received=111 trp_decoded=112 trp_responded=113 trp_failed=114 "
	                        "unsupported_proc=115 unsupported_info=116 trp_empty=117 trp_last=empty_result "
	                        "standard_decode_ok=0 standard_decode_fail=0 standard_encode_resp=0 "
	                        "standard_encode_fail=0 minimal_fallback_decode=0 standard_last=none "
	                        "pos_info_received=118 pos_info_decoded=119 pos_info_forwarded=120 "
	                        "pos_info_responded=121 pos_info_failed=122 pos_info_dropped=123 "
	                        "pos_info_last=f1ap_failure measurement_received=124 measurement_decoded=125 "
                        "measurement_forwarded=126 measurement_responded=127 measurement_failed=128 "
                        "measurement_dropped=129 measurement_last=responded activation_received=130 "
                        "activation_decoded=131 activation_forwarded=132 activation_responded=133 "
	                        "activation_failed=134 activation_dropped=135 activation_last=responded "
	                        "deactivation_received=136 deactivation_decoded=137 deactivation_forwarded=138 "
	                        "deactivation_acked=139 deactivation_failed=140 deactivation_dropped=141 "
	                        "deactivation_last=acked assist_ctrl_received=0 assist_ctrl_decoded=0 "
	                        "assist_ctrl_forwarded=0 assist_ctrl_feedback=0 assist_ctrl_failed=0 "
	                        "assist_ctrl_dropped=0 assist_ctrl_unsupported_fields=0 assist_ctrl_last=none"),
	            std::string::npos);
  ASSERT_NE(output.find("NTN idle paging: contexts=90 with_5g_s_tmsi=91 expired=92 ue_hits=93 "
                        "tac_fallbacks=94 recommendations=95 skipped=96 last_reason=last_beam_unpageable"),
            std::string::npos);
  ASSERT_NE(output.find("NTN paired access: contexts=176 responses=177 service_bindings=178 blocked=179 "
                        "last_reason=service_bound"),
            std::string::npos);
  ASSERT_NE(output.find("NTN service pair: bound=180 blocked=181 last_reason=service_pair_capacity_blocked"),
            std::string::npos);
  ASSERT_NE(output.find("NTN service pair handover: targets=182 scheduled=183 skipped=184 committed=185 "
                        "rolled_back=186 context_cleared=187 last_reason=same_analog_tac_uplink_resource "
                        "completion_reason=committed"),
            std::string::npos);
  ASSERT_NE(output.find("NTN inactive: contexts=97 expired=98 suspend_requested=99 suspend_succeeded=100 "
                        "suspend_failed=101 resume_requested=102 resume_succeeded=103 resume_failed=104 "
                        "ngap_suspend_resp=105 ngap_suspend_fail=106 ngap_resume_resp=107 "
                        "ngap_resume_fail=108 paging_hits=109 fallback_releases=110 "
                        "last_reason=native_resume_complete"),
            std::string::npos);
  ASSERT_NE(output.find("NTN multi-satellite window: valid=yes current_satellites=2 next_satellites=3 "
                        "visible_beams=4 owner_changes=5"),
            std::string::npos);
  ASSERT_NE(output.find("timeline_steps=10 horizon_ms=10000 lead_ms=3000 entries=6 exits=7 "
                        "earliest_upcoming_ms=2000 earliest_drain_ms=3000"),
            std::string::npos);
  ASSERT_NE(output.find("NTN resource manager: rnti_owned=22 rnti_conflicts=23 digital_slot_active=24 sent=37 applied=38 rejected=39 cleared=25 cleared_by_du=40 rollback=41"),
            std::string::npos);
  ASSERT_NE(output.find("NTN RNTI leases: reserved=26 available=27 sent=28 applied=29 rejected=30 offered=31 consumed=42 initial_ul=32 committed=33 released=34 expired=35 conflicts=36"),
            std::string::npos);
  ASSERT_NE(output.find("NTN resource audit: generation=194 queries=195 accepted=196 mismatches=197 repairs=198 "
                        "failures=199 rnti_incomplete=200 ue_slot_incomplete=201 reason=ue_slot_snapshot_incomplete"),
            std::string::npos);
  ASSERT_NE(output.find("NTN resource repairs: queued=45 sent=46 applied=47 failed=48 retry_exhausted=49 conflicts=50"),
            std::string::npos);
  ASSERT_NE(output.find("NTN service pair resource audit: targets=188 mismatches=189 repairs=190 skipped=191 "
                        "slot_intents=192 repair_records=193 "
                        "last_reason=du_missing_service_pair_ul_sr_srs_assignment"),
            std::string::npos);
  ASSERT_NE(output.find("NTN resource domain: analog_cap_blocked=14 digital_cap_blocked=15 conflict_blocked=16 active_reuse_groups=17"),
            std::string::npos);
  ASSERT_NE(output.find("NTN SIB19 broadcast: desired=51 sent=52 applied=53 rejected=54 cleared=55 stale=56"),
            std::string::npos);
}

TEST(cu_cp_unit_config, ntn_state_command_prints_versioned_onboard_position_plan)
{
  fake_cu_cp_command_handler command_handler;
  auto& plan                    = command_handler.ntn.runtime.onboard_position_plan;
  plan.enabled                  = true;
  plan.du_execution_enabled     = true;
  plan.schema_version           = 2;
  plan.planning_run_id          = "planning-run-2026-07-15";
  plan.access_profile_id        = "ntn-access-16a-64d-v1";
  plan.access_profile_hash      = "sha256:profile";
  plan.identity_authority       = "onboard_position_plan";
  plan.state_file_configured                         = true;
  plan.state_file_required                           = true;
  plan.state_schema_version                          = 2;
  plan.state_generation                              = 17;
  plan.state_hash                                    = "sha256:state";
  plan.state_store_status                            = "stored";
  plan.state_store_error                             = "none";
  plan.state_write_blocked                           = false;
  plan.last_state_save_unix_ms                       = 639900;
  plan.recovery_stage                                = "reconciling";
  plan.recovery_detail                               = "awaiting_du_query";
  plan.recovery_schedule_version                     = 19;
  plan.catalog_version_high_water                    = 12;
  plan.schedule_version_high_water                   = 22;
  plan.stage                    = "pending";
  plan.deployment_stage         = "ready";
  plan.deployment_detail        = "du_ready";
  plan.execution_evidence       = "intent_or_control_plane_only";
  plan.satellite_id                                  = "P01-S01";
  plan.active_catalog_version   = 10;
  plan.active_schedule_version  = 20;
  plan.active_content_hash      = "sha256:active";
  plan.active_calendar_hash     = "sha256:active-calendar";
  plan.active_activation_epoch_unix_ms = 320000;
  plan.pending_catalog_version  = 11;
  plan.pending_schedule_version = 21;
  plan.pending_content_hash     = "sha256:pending";
  plan.pending_calendar_hash    = "sha256:calendar";
  plan.pending_activation_epoch_unix_ms = 640000;
  plan.received_plan_present     = true;
  plan.received_catalog_version  = 12;
  plan.received_schedule_version = 22;
  plan.received_content_hash     = "sha256:rejected";
  plan.received_activation_epoch_unix_ms = 960000;
  plan.candidate_l1_positions   = 256;
  plan.audited_schedule_version = 21;
  plan.calendar_intents         = 2560;
  plan.ssb_intents              = 2048;
  plan.prach_ro_intents         = 256;
  plan.prach_ul_beam_intents    = 256;
  plan.max_ssb_interval_ms      = 80;
  plan.max_prach_interval_ms    = 640;
  plan.last_rejection           = "schedule_overflow";
  plan.last_rejected_schedule_version = 22;
  plan.clear_queue_depth                  = 2;
  plan.clear_in_flight                    = true;
  plan.clear_queue_head_schedule_version = 18;
  plan.clear_queue_head_calendar_hash     = "sha256:clear-calendar";
  plan.clear_queue_head_reason            = "expired_deployment_recovered_after_restart";
  plan.cells[0] = {nr_cell_identity::create(0x123450001ULL).value(), 101, 128, 128, 128, 16, 16, 64};
  plan.cells[1] = {nr_cell_identity::create(0x123450002ULL).value(), 202, 128, 128, 128, 16, 16, 64};
  plan.static_preflight_schedule_version             = 21;
  plan.static_opportunities[0].performed             = true;
  plan.static_opportunities[0].passed                = true;
  plan.static_opportunities[0].numerology            = 1;
  plan.static_opportunities[0].expected_ssb          = 1024;
  plan.static_opportunities[0].matched_ssb           = 1024;
  plan.static_opportunities[0].expected_prach        = 128;
  plan.static_opportunities[0].matched_prach         = 128;
  plan.static_opportunities[0].max_ssb_gap_slots     = 160;
  plan.static_opportunities[0].max_prach_gap_slots   = 1280;

  ntn_state_app_command command(command_handler);
  ::testing::internal::CaptureStdout();
  const std::array<std::string, 0> args = {};
  command.execute(span<const std::string>{args});
  const std::string output = ::testing::internal::GetCapturedStdout();

  EXPECT_NE(output.find("NTN onboard position plan: enabled=yes du_execution=yes stage=pending deployment=ready "
                        "satellite_id=P01-S01 last_rejection=schedule_overflow rejected_schedule_version=22"),
            std::string::npos);
  EXPECT_NE(output.find("NTN onboard planning context: schema_version=2 planning_run_id=planning-run-2026-07-15 "
                        "access_profile_id=ntn-access-16a-64d-v1 access_profile_hash=sha256:profile "
                        "identity_authority=onboard_position_plan"),
            std::string::npos);
  EXPECT_NE(output.find("NTN onboard state store: file_configured=yes file_required=yes schema_version=2 "
                        "generation=17 state_hash=sha256:state status=stored error=none write_blocked=no "
                        "last_save_unix_ms=639900"),
            std::string::npos);
  EXPECT_NE(output.find("NTN onboard recovery: stage=reconciling detail=awaiting_du_query schedule_version=19 "
                        "catalog_version_high_water=12 schedule_version_high_water=22 "
                        "evidence=persisted_state_is_not_du_or_rf_evidence"),
            std::string::npos);
  EXPECT_NE(output.find("NTN onboard position plan received: present=yes catalog_version=12 schedule_version=22 "
                        "content_hash=sha256:rejected candidate_l1=256 activation_epoch_unix_ms=960000"),
            std::string::npos);
  EXPECT_NE(output.find("NTN onboard position plan active: catalog_version=10 schedule_version=20 "
                        "content_hash=sha256:active calendar_hash=sha256:active-calendar "
                        "activation_epoch_unix_ms=320000"),
            std::string::npos);
  EXPECT_NE(output.find("NTN onboard position plan pending: catalog_version=11 schedule_version=21 "
                        "content_hash=sha256:pending calendar_hash=sha256:calendar activation_epoch_unix_ms=640000"),
            std::string::npos);
  EXPECT_NE(output.find("NTN access calendar intent: schedule_version=21 intents=2560 ssb=2048 prach_ro=256 "
                        "prach_ul_beam=256 max_ssb_interval_ms=80 "
                         "max_prach_interval_ms=640 "
                         "prach_ro_without_beam=0 resource_conflicts=0 deployment_detail=du_ready "
                         "evidence=intent_or_control_plane_only"),
            std::string::npos);
  EXPECT_NE(output.find("NTN calendar clear queue: depth=2 in_flight=yes head_schedule_version=18 "
                        "head_calendar_hash=sha256:clear-calendar "
                        "head_reason=expired_deployment_recovered_after_restart"),
            std::string::npos);
  EXPECT_NE(output.find("NTN static opportunity preflight: schedule_version=21 nci=0x123450001 pci=101 "
                        "performed=yes passed=yes numerology=1 ssb=1024/1024 prach=128/128 "
                        "max_ssb_gap_slots=160 max_prach_gap_slots=1280"),
            std::string::npos);
  EXPECT_NE(output.find("NTN onboard cell: nci=0x123450001 pci=101 active_l1=128 pending_l1=128 capacity=128"),
            std::string::npos);
  EXPECT_NE(output.find("NTN onboard cell: nci=0x123450002 pci=202 active_l1=128 pending_l1=128 capacity=128"),
            std::string::npos);
}

TEST(cu_cp_unit_config, ntn_state_command_prints_disabled_onboard_state_defaults)
{
  fake_cu_cp_command_handler command_handler;

  ntn_state_app_command command(command_handler);
  ::testing::internal::CaptureStdout();
  const std::array<std::string, 0> args = {};
  command.execute(span<const std::string>{args});
  const std::string output = ::testing::internal::GetCapturedStdout();

  EXPECT_NE(output.find("NTN onboard state store: file_configured=no file_required=no schema_version=0 generation=0 "
                        "state_hash=none status=disabled error=none write_blocked=no last_save_unix_ms=-1"),
            std::string::npos);
  EXPECT_NE(output.find("NTN onboard recovery: stage=disabled detail=state_recovery_disabled schedule_version=0 "
                        "catalog_version_high_water=0 schedule_version_high_water=0 "
                        "evidence=persisted_state_is_not_du_or_rf_evidence"),
            std::string::npos);
  EXPECT_NE(output.find("NTN onboard position plan active: catalog_version=0 schedule_version=0 content_hash=none "
                        "calendar_hash=none activation_epoch_unix_ms=-1"),
            std::string::npos);
  EXPECT_NE(output.find("NTN calendar clear queue: depth=0 in_flight=no head_schedule_version=0 "
                        "head_calendar_hash=none head_reason=none"),
            std::string::npos);
}

TEST(cu_cp_unit_config, ntn_state_command_prints_antenna_intent_summary)
{
  fake_cu_cp_command_handler command_handler;

  srs_cu_cp::cu_cp_ntn_analog_access_intent analog;
  analog.analog_beam_id         = "ANALOG-ACCESS-001";
  analog.selected_du_index      = srs_cu_cp::uint_to_du_index(0);
  analog.nof_access_active_ues  = 2;
  analog.reason                 = "eligible";

  srs_cu_cp::cu_cp_ntn_digital_service_intent digital;
  digital.digital_beam_id     = "CN-BEAM-0001";
  digital.service_du_index    = srs_cu_cp::uint_to_du_index(0);
  digital.nof_ues             = 1;
  digital.nof_drbs            = 1;
  digital.nof_antenna_slots   = 1;
  digital.antenna_slot_period = 1;
  digital.reason              = "loaded_service_calendar";

  command_handler.ntn.antenna_intent.analog_access_intents  = {analog};
  command_handler.ntn.antenna_intent.digital_service_intents = {digital};

  ntn_state_app_command command(command_handler);
  ::testing::internal::CaptureStdout();
  const std::array<std::string, 0> args = {};
  command.execute(span<const std::string>{args});
  const std::string output = ::testing::internal::GetCapturedStdout();

  ASSERT_NE(output.find("NTN antenna intent: analog_access=1 access_active_ues=2 digital_service=1 digital_ues=1 digital_drbs=1"),
            std::string::npos);
}

TEST(cu_cp_unit_config, ntn_diagnose_summary_reports_ready_system_as_ok)
{
  fake_cu_cp_command_handler command_handler;
  command_handler.ntn.runtime.enabled                         = true;
  command_handler.ntn.runtime.satellite_state_available       = true;
  command_handler.ntn.runtime.assistance_valid                = true;
  command_handler.ntn.runtime.nof_active_analog_access_beams  = 2;
  command_handler.ntn.runtime.nof_access_roundtrip_ready_beams = 2;
  command_handler.ntn.runtime.nof_loaded_digital_service_beams = 3;
  command_handler.ntn.runtime.nof_paging_recommendable_beams  = 1;
  command_handler.ntn.runtime.nof_sib19_broadcast_applied_by_du = 1;
  command_handler.ntn.resource_snapshot.nof_rnti_leases_applied_by_du = 4;
  command_handler.ntn.resource_snapshot.nof_digital_slot_applied_by_du = 3;

  ntn_diagnose_app_command command(command_handler);
  ::testing::internal::CaptureStdout();
  const std::array<std::string, 0> args = {};
  command.execute(span<const std::string>{args});
  const std::string output = ::testing::internal::GetCapturedStdout();

  ASSERT_NE(output.find("NTN diagnose: filter=summary"), std::string::npos);
  ASSERT_NE(output.find("area=access status=ok reason=access_ready next_step=none"), std::string::npos);
  ASSERT_NE(output.find("area=service status=ok reason=service_ready next_step=none"), std::string::npos);
  ASSERT_NE(output.find("area=mobility status=ok reason=no_blocked_moves next_step=none"), std::string::npos);
  ASSERT_NE(output.find("area=paging status=ok reason=paging_ready next_step=none"), std::string::npos);
  ASSERT_NE(output.find("area=resources status=ok reason=resources_applied next_step=none"), std::string::npos);
}

TEST(cu_cp_unit_config, ntn_diagnose_reports_downlink_only_paging_without_uplink_response)
{
  fake_cu_cp_command_handler command_handler;
  command_handler.ntn.runtime.enabled                         = true;
  command_handler.ntn.runtime.satellite_state_available       = true;
  command_handler.ntn.runtime.assistance_valid                = true;
  command_handler.ntn.runtime.nof_downlink_visible_beams      = 1;
  command_handler.ntn.runtime.nof_uplink_access_ready_beams   = 0;
  command_handler.ntn.runtime.nof_access_roundtrip_ready_beams = 0;
  command_handler.ntn.runtime.nof_active_analog_access_beams  = 0;
  command_handler.ntn.runtime.nof_paging_recommendable_beams  = 0;

  ntn_diagnose_app_command command(command_handler);
  ::testing::internal::CaptureStdout();
  const std::array<std::string, 1> args = {"all"};
  command.execute(span<const std::string>{args});
  const std::string output = ::testing::internal::GetCapturedStdout();

  ASSERT_NE(output.find("area=access status=blocker reason=downlink_only_without_ul_pair "
                        "next_step=check_uplink_access_resources_for_visible_beams"),
            std::string::npos);
  ASSERT_NE(output.find("area=paging status=blocker reason=downlink_only_without_ul_pair "
                        "next_step=enable_uplink_access_or_select_paired_access_beam"),
            std::string::npos);
}

TEST(cu_cp_unit_config, ntn_diagnose_reports_paired_access_ready_for_downlink_only_beam)
{
  fake_cu_cp_command_handler command_handler;
  command_handler.ntn.runtime.enabled                              = true;
  command_handler.ntn.runtime.satellite_state_available            = true;
  command_handler.ntn.runtime.assistance_valid                     = true;
  command_handler.ntn.runtime.nof_downlink_visible_beams           = 1;
  command_handler.ntn.runtime.nof_uplink_access_ready_beams        = 1;
  command_handler.ntn.runtime.nof_access_roundtrip_ready_beams     = 0;
  command_handler.ntn.runtime.nof_paired_uplink_access_ready_beams = 1;
  command_handler.ntn.runtime.nof_active_analog_access_beams       = 1;
  command_handler.ntn.runtime.nof_paging_recommendable_beams       = 1;
  command_handler.ntn.runtime.nof_loaded_digital_service_beams     = 1;

  ntn_diagnose_app_command command(command_handler);
  ::testing::internal::CaptureStdout();
  const std::array<std::string, 1> args = {"all"};
  command.execute(span<const std::string>{args});
  const std::string output = ::testing::internal::GetCapturedStdout();

  ASSERT_NE(output.find("area=access status=ok reason=access_ready next_step=none"), std::string::npos);
  ASSERT_NE(output.find("area=paging status=ok reason=paging_ready next_step=none"), std::string::npos);
  ASSERT_EQ(output.find("reason=access_roundtrip_unavailable"), std::string::npos);
  ASSERT_EQ(output.find("reason=downlink_only_without_ul_pair"), std::string::npos);
}

TEST(cu_cp_unit_config, ntn_diagnose_reports_paired_access_waiting_for_ul_response)
{
  fake_cu_cp_command_handler command_handler;
  command_handler.ntn.runtime.enabled                              = true;
  command_handler.ntn.runtime.satellite_state_available            = true;
  command_handler.ntn.runtime.assistance_valid                     = true;
  command_handler.ntn.runtime.nof_downlink_visible_beams           = 1;
  command_handler.ntn.runtime.nof_uplink_access_ready_beams        = 1;
  command_handler.ntn.runtime.nof_paired_uplink_access_ready_beams = 1;
  command_handler.ntn.runtime.nof_active_analog_access_beams       = 1;
  command_handler.ntn.runtime.nof_paging_recommendable_beams       = 1;
  command_handler.ntn.runtime.nof_loaded_digital_service_beams     = 1;
  command_handler.ntn.runtime.nof_ntn_paired_access_contexts       = 1;
  command_handler.ntn.runtime.nof_ntn_paired_access_responses      = 0;

  ntn_diagnose_app_command command(command_handler);
  ::testing::internal::CaptureStdout();
  const std::array<std::string, 1> args = {"all"};
  command.execute(span<const std::string>{args});
  const std::string output = ::testing::internal::GetCapturedStdout();

  ASSERT_NE(output.find("area=access status=warn reason=paired_access_waiting_for_ul_response "
                        "next_step=check_paired_uplink_access_beam_and_rnti_lease"),
            std::string::npos);
  ASSERT_NE(output.find("area=paging status=warn reason=paired_access_context_active "
                        "next_step=check_ntn_ues_for_dl_wake_and_ul_response_beams"),
            std::string::npos);
}

TEST(cu_cp_unit_config, ntn_diagnose_reports_paired_access_service_binding_blocked)
{
  fake_cu_cp_command_handler command_handler;
  command_handler.ntn.runtime.enabled                              = true;
  command_handler.ntn.runtime.satellite_state_available            = true;
  command_handler.ntn.runtime.assistance_valid                     = true;
  command_handler.ntn.runtime.nof_access_roundtrip_ready_beams     = 1;
  command_handler.ntn.runtime.nof_active_analog_access_beams       = 1;
  command_handler.ntn.runtime.nof_paging_recommendable_beams       = 1;
  command_handler.ntn.runtime.nof_loaded_digital_service_beams     = 1;
  command_handler.ntn.runtime.nof_ntn_paired_access_contexts       = 1;
  command_handler.ntn.runtime.nof_ntn_paired_access_responses      = 1;
  command_handler.ntn.runtime.nof_ntn_service_bindings_from_paired_access = 0;
  command_handler.ntn.runtime.nof_ntn_paired_access_blocked        = 1;
  command_handler.ntn.runtime.last_ntn_paired_access_reason        = "no_bidirectional_sibling_for_paired_access";

  ntn_diagnose_app_command command(command_handler);
  ::testing::internal::CaptureStdout();
  const std::array<std::string, 1> args = {"service"};
  command.execute(span<const std::string>{args});
  const std::string output = ::testing::internal::GetCapturedStdout();

  ASSERT_NE(output.find("area=service status=warn "
                        "reason=paired_access_service_blocked:no_bidirectional_sibling_for_paired_access "
                        "next_step=provide_bidirectional_service_or_dl_ul_service_pair"),
            std::string::npos);
}

TEST(cu_cp_unit_config, ntn_diagnose_reports_missing_satellite_and_invalid_assistance_as_access_blocker)
{
  fake_cu_cp_command_handler command_handler;
  command_handler.ntn.runtime.enabled                   = true;
  command_handler.ntn.runtime.satellite_state_available = false;
  command_handler.ntn.runtime.assistance_valid          = false;
  command_handler.ntn.runtime.assistance_invalid_reason =
      srs_cu_cp::ntn_assistance_invalid_reason::stale_satellite_state;

  ntn_diagnose_app_command command(command_handler);
  ::testing::internal::CaptureStdout();
  const std::array<std::string, 1> args = {"access"};
  command.execute(span<const std::string>{args});
  const std::string output = ::testing::internal::GetCapturedStdout();

  ASSERT_NE(output.find("NTN diagnose: filter=access"), std::string::npos);
  ASSERT_NE(output.find("area=access status=blocker reason=no_satellite_state next_step=inject_or_enable_satellite_state"),
            std::string::npos);
  ASSERT_NE(output.find("area=access status=blocker reason=assistance_invalid:stale_satellite_state "
                        "next_step=refresh_satellite_state_or_check_beam_table"),
            std::string::npos);
}

TEST(cu_cp_unit_config, ntn_diagnose_reports_no_access_or_service_ready_beams)
{
  fake_cu_cp_command_handler command_handler;
  command_handler.ntn.runtime.enabled                         = true;
  command_handler.ntn.runtime.satellite_state_available       = true;
  command_handler.ntn.runtime.assistance_valid                = true;
  command_handler.ntn.runtime.nof_active_analog_access_beams  = 0;
  command_handler.ntn.runtime.nof_loaded_digital_service_beams = 0;
  command_handler.ntn.runtime.nof_active_loaded_beams         = 0;
  command_handler.ntn.runtime.nof_draining_beams              = 2;
  command_handler.ntn.runtime.nof_ntn_service_binding_pending_ues = 1;
  command_handler.ntn.runtime.nof_ntn_service_binding_blocked_ues = 1;
  command_handler.ntn.runtime.nof_ntn_admission_blocked_by_target_reservation = 1;

  ntn_diagnose_app_command command(command_handler);
  ::testing::internal::CaptureStdout();
  const std::array<std::string, 1> args = {"service"};
  command.execute(span<const std::string>{args});
  const std::string output = ::testing::internal::GetCapturedStdout();

  ASSERT_NE(output.find("area=service status=blocker reason=no_service_ready_beam "
                        "next_step=check_loaded_service_capacity_and_beam_visibility"),
            std::string::npos);
  ASSERT_NE(output.find("area=service status=warn reason=draining_service_beams "
                        "next_step=wait_for_target_beam_or_check_switch_over_policy"),
            std::string::npos);
  ASSERT_NE(output.find("area=service status=warn reason=service_binding_not_complete "
                        "next_step=inspect_ntn_ues_for_binding_reason"),
            std::string::npos);
  ASSERT_NE(output.find("area=service status=warn reason=reserved_capacity_blocks_low_priority_demand "
                        "next_step=wait_for_move_or_raise_demand_priority"),
            std::string::npos);
}

TEST(cu_cp_unit_config, ntn_diagnose_reports_mobility_guard_capacity_and_preheat_issues)
{
  fake_cu_cp_command_handler command_handler;
  command_handler.ntn.runtime.enabled = true;
  command_handler.ntn.runtime.nof_pre_service_relocations_pending = 1;
  command_handler.ntn.runtime.nof_connected_handovers_blocked     = 1;
  command_handler.ntn.runtime.nof_ntn_beam_hopping_ues_skipped    = 1;
  command_handler.ntn.runtime.nof_ntn_load_balancing_handover_skipped = 1;
  command_handler.ntn.runtime.nof_ntn_load_balancing_skipped_projected_capacity = 1;
  command_handler.ntn.runtime.nof_ntn_load_balancing_skipped_cold_analog = 1;
  command_handler.ntn.runtime.nof_ntn_preheat_skipped_by_capacity = 1;
  command_handler.ntn.runtime.nof_ntn_handover_skipped_by_pair_cooldown = 1;
  command_handler.ntn.runtime.nof_ntn_handover_skipped_by_preheat_ready_guard = 1;
  command_handler.ntn.runtime.nof_ntn_service_pair_handover_rolled_back = 1;

  ntn_diagnose_app_command command(command_handler);
  ::testing::internal::CaptureStdout();
  const std::array<std::string, 1> args = {"mobility"};
  command.execute(span<const std::string>{args});
  const std::string output = ::testing::internal::GetCapturedStdout();

  ASSERT_NE(output.find("area=mobility status=warn reason=pre_service_move_pending "
                        "next_step=wait_for_target_du_preparation"),
            std::string::npos);
  ASSERT_NE(output.find("area=mobility status=warn reason=handover_blocked_or_skipped "
                        "next_step=inspect_target_beam_capacity_and_resource_state"),
            std::string::npos);
  ASSERT_NE(output.find("area=mobility status=warn reason=service_pair_handover_completion_issue "
                        "next_step=check_target_downlink_and_uplink_resource_completion"),
            std::string::npos);
  ASSERT_NE(output.find("area=mobility status=warn reason=target_capacity_or_cold_beam "
                        "next_step=check_preheat_capacity_and_target_reservation"),
            std::string::npos);
  ASSERT_NE(output.find("area=mobility status=warn reason=safety_guard_active "
                        "next_step=wait_for_cooldown_or_ready_guard"),
            std::string::npos);
}

TEST(cu_cp_unit_config, ntn_diagnose_reports_paging_and_resource_apply_issues)
{
  fake_cu_cp_command_handler command_handler;
  command_handler.ntn.runtime.enabled = true;
  command_handler.ntn.runtime.nof_paging_recommendable_beams       = 0;
  command_handler.ntn.runtime.nof_ntn_idle_paging_contexts_expired = 1;
  command_handler.ntn.runtime.nof_ntn_idle_paging_tac_fallbacks    = 1;
  command_handler.ntn.runtime.last_ntn_idle_paging_reason          = "last_beam_unpageable";
  command_handler.ntn.runtime.nof_sib19_broadcast_desired          = 2;
  command_handler.ntn.runtime.nof_sib19_broadcast_applied_by_du    = 0;
  command_handler.ntn.runtime.nof_sib19_broadcast_rejected_by_du   = 1;
  command_handler.ntn.resource_snapshot.nof_rnti_leases_sent_to_du     = 2;
  command_handler.ntn.resource_snapshot.nof_rnti_leases_applied_by_du  = 0;
  command_handler.ntn.resource_snapshot.nof_rnti_leases_rejected_by_du = 1;
  command_handler.ntn.resource_snapshot.nof_digital_slot_sent_to_du    = 2;
  command_handler.ntn.resource_snapshot.nof_digital_slot_applied_by_du = 0;
  command_handler.ntn.resource_snapshot.nof_digital_slot_rejected_by_du = 1;
  command_handler.ntn.resource_snapshot.nof_resource_repairs_failed = 1;
  command_handler.ntn.runtime.nof_ntn_service_pair_resource_audit_skipped = 1;
  command_handler.ntn.runtime.last_ntn_service_pair_resource_audit_reason = "connected_handover_pending";

  ntn_diagnose_app_command command(command_handler);
  ::testing::internal::CaptureStdout();
  const std::array<std::string, 1> args = {"all"};
  command.execute(span<const std::string>{args});
  const std::string output = ::testing::internal::GetCapturedStdout();

  ASSERT_NE(output.find("area=paging status=blocker reason=no_pageable_beam "
                        "next_step=check_beam_tac_and_current_service_area"),
            std::string::npos);
  ASSERT_NE(output.find("area=paging status=warn reason=idle_context_or_tac_fallback "
                        "next_step=inspect_idle_context_age_and_last_beam"),
            std::string::npos);
  ASSERT_NE(output.find("area=resources status=warn reason=sib19_not_applied "
                        "next_step=ntn_repair apply sib19"),
            std::string::npos);
  ASSERT_NE(output.find("area=resources status=warn reason=rnti_or_slot_apply_issue "
                        "next_step=ntn_repair apply resources"),
            std::string::npos);
  ASSERT_NE(output.find("area=resources status=warn reason=resource_repair_failed "
                        "next_step=ntn_repair apply resources"),
            std::string::npos);
  ASSERT_NE(output.find("area=resources status=warn "
                        "reason=service_pair_ul_resource_repair_skipped:connected_handover_pending "
                        "next_step=ntn_repair apply resources"),
            std::string::npos);
}

TEST(cu_cp_unit_config, ntn_repair_default_is_dry_run_and_does_not_apply)
{
  fake_cu_cp_command_handler command_handler;
  command_handler.ntn.repair_response.accepted          = true;
  command_handler.ntn.repair_response.reason            = "dry_run";
  command_handler.ntn.repair_response.next_step         = "rerun_with_apply_to_trigger_refresh";
  command_handler.ntn.repair_response.audit_targets     = 2;
  command_handler.ntn.repair_response.sib19_candidates  = 1;
  command_handler.ntn.repair_response.existing_failed   = 3;
  command_handler.ntn.repair_response.existing_blockers = 4;

  ntn_repair_app_command command(command_handler);
  ::testing::internal::CaptureStdout();
  const std::array<std::string, 0> args = {};
  command.execute(span<const std::string>{args});
  const std::string output = ::testing::internal::GetCapturedStdout();

  ASSERT_EQ(command_handler.ntn.nof_repair_commands, 1U);
  ASSERT_TRUE(command_handler.ntn.last_repair_command.has_value());
  EXPECT_EQ(command_handler.ntn.last_repair_command->mode, srs_cu_cp::ntn_repair_mode::dry_run);
  EXPECT_EQ(command_handler.ntn.last_repair_command->scope, srs_cu_cp::ntn_repair_scope::all);
  EXPECT_EQ(command_handler.ntn.last_repair_command->limit, 16U);
  ASSERT_NE(output.find("NTN repair: mode=dry-run scope=all accepted=yes reason=dry_run "
                        "next_step=rerun_with_apply_to_trigger_refresh audit_targets=2 "
                        "sib19_candidates=1 existing_failed=3 existing_blockers=4"),
            std::string::npos);
}

TEST(cu_cp_unit_config, ntn_repair_apply_resources_calls_handler_with_apply_mode)
{
  fake_cu_cp_command_handler command_handler;
  command_handler.ntn.repair_response.accepted          = true;
  command_handler.ntn.repair_response.reason            = "resources_repair_requested";
  command_handler.ntn.repair_response.next_step         = "check_ntn_state_for_async_results";
  command_handler.ntn.repair_response.audit_targets     = 1;
  command_handler.ntn.repair_response.sib19_candidates  = 0;
  command_handler.ntn.repair_response.existing_failed   = 0;
  command_handler.ntn.repair_response.existing_blockers = 0;

  ntn_repair_app_command command(command_handler);
  ::testing::internal::CaptureStdout();
  const std::array<std::string, 2> args = {"apply", "resources"};
  command.execute(span<const std::string>{args});
  const std::string output = ::testing::internal::GetCapturedStdout();

  ASSERT_EQ(command_handler.ntn.nof_repair_commands, 1U);
  ASSERT_TRUE(command_handler.ntn.last_repair_command.has_value());
  EXPECT_EQ(command_handler.ntn.last_repair_command->mode, srs_cu_cp::ntn_repair_mode::apply);
  EXPECT_EQ(command_handler.ntn.last_repair_command->scope, srs_cu_cp::ntn_repair_scope::resources);
  ASSERT_NE(output.find("NTN repair: mode=apply scope=resources accepted=yes "
                        "reason=resources_repair_requested next_step=check_ntn_state_for_async_results"),
            std::string::npos);
}

TEST(cu_cp_unit_config, ntn_repair_apply_all_prints_async_followup_next_step)
{
  fake_cu_cp_command_handler command_handler;
  command_handler.ntn.repair_response.accepted          = true;
  command_handler.ntn.repair_response.reason            = "repair_requested";
  command_handler.ntn.repair_response.next_step         = "check_ntn_state_for_async_results";
  command_handler.ntn.repair_response.audit_targets     = 3;
  command_handler.ntn.repair_response.sib19_candidates  = 2;
  command_handler.ntn.repair_response.existing_failed   = 1;
  command_handler.ntn.repair_response.existing_blockers = 0;

  ntn_repair_app_command command(command_handler);
  ::testing::internal::CaptureStdout();
  const std::array<std::string, 3> args = {"apply", "all", "7"};
  command.execute(span<const std::string>{args});
  const std::string output = ::testing::internal::GetCapturedStdout();

  ASSERT_TRUE(command_handler.ntn.last_repair_command.has_value());
  EXPECT_EQ(command_handler.ntn.last_repair_command->mode, srs_cu_cp::ntn_repair_mode::apply);
  EXPECT_EQ(command_handler.ntn.last_repair_command->scope, srs_cu_cp::ntn_repair_scope::all);
  EXPECT_EQ(command_handler.ntn.last_repair_command->limit, 7U);
  ASSERT_NE(output.find("next_step=check_ntn_state_for_async_results"), std::string::npos);
  ASSERT_NE(output.find("audit_targets=3 sib19_candidates=2 existing_failed=1 existing_blockers=0"),
            std::string::npos);
}

TEST(cu_cp_unit_config, ntn_repair_rejects_invalid_mode_scope_or_limit)
{
  fake_cu_cp_command_handler command_handler;
  ntn_repair_app_command     command(command_handler);

  ::testing::internal::CaptureStdout();
  const std::array<std::string, 1> invalid_mode = {"force"};
  command.execute(span<const std::string>{invalid_mode});
  std::string output = ::testing::internal::GetCapturedStdout();
  ASSERT_NE(output.find("Invalid NTN repair argument."), std::string::npos);
  EXPECT_EQ(command_handler.ntn.nof_repair_commands, 0U);

  ::testing::internal::CaptureStdout();
  const std::array<std::string, 2> invalid_scope = {"apply", "paging"};
  command.execute(span<const std::string>{invalid_scope});
  output = ::testing::internal::GetCapturedStdout();
  ASSERT_NE(output.find("Invalid NTN repair argument."), std::string::npos);
  EXPECT_EQ(command_handler.ntn.nof_repair_commands, 0U);

  ::testing::internal::CaptureStdout();
  const std::array<std::string, 3> invalid_limit = {"dry-run", "all", "NaN"};
  command.execute(span<const std::string>{invalid_limit});
  output = ::testing::internal::GetCapturedStdout();
  ASSERT_NE(output.find("Invalid NTN repair row limit."), std::string::npos);
  EXPECT_EQ(command_handler.ntn.nof_repair_commands, 0U);
}

TEST(cu_cp_unit_config, ntn_diagnose_command_is_registered_by_ocucp_builder)
{
  const std::filesystem::path builder_path = get_project_source_root() / "apps/units/o_cu_cp/o_cu_cp_builder.cpp";
  std::ifstream               builder_file(builder_path);
  ASSERT_TRUE(builder_file.is_open());
  const std::string builder_source((std::istreambuf_iterator<char>(builder_file)), std::istreambuf_iterator<char>());

  ASSERT_NE(builder_source.find("ntn_diagnose_app_command"), std::string::npos);
  ASSERT_NE(builder_source.find("ntn_repair_app_command"), std::string::npos);
}

TEST(cu_cp_unit_config, ntn_ues_command_prints_pre_service_relocation_state)
{
  fake_cu_cp_command_handler command_handler;
  srs_cu_cp::cu_cp_ntn_ue_status ue;
  ue.ue_index                                      = srs_cu_cp::uint_to_ue_index(7);
  ue.du_index                                      = srs_cu_cp::uint_to_du_index(0);
  ue.rnti                                          = to_rnti(0x4601);
  ue.serving_beam_id                               = "CN-BEAM-0001";
  ue.pre_service_relocation_state                  = "preparing";
  ue.pre_service_relocation_reason                 = "access_du_mismatch";
  ue.pre_service_relocation_target_du_index        = srs_cu_cp::uint_to_du_index(1);
  ue.pre_service_relocation_target_beam_id         = "CN-BEAM-0001";
  ue.pre_service_relocation_target_nci             = make_nci(0);
  command_handler.ntn.ue_status                    = {ue};

  ntn_ues_app_command command(command_handler);
  ::testing::internal::CaptureStdout();
  const std::array<std::string, 0> args = {};
  command.execute(span<const std::string>{args});
  const std::string output = ::testing::internal::GetCapturedStdout();

  ASSERT_NE(output.find("NTN UE state guide: access_only=signaling_only control_only=connected_without_service "
                        "binding_pending=waiting_for_service_beam service_bound=using_service_beam"),
            std::string::npos);
  ASSERT_NE(output.find("NTN UE movement guide: reloc=move_before_service conn_ho=connected_move "
                        "loc_req=location_reporting_request core=core_location_requests"),
            std::string::npos);
  ASSERT_NE(output.find("reloc"), std::string::npos);
  ASSERT_NE(output.find("preparing"), std::string::npos);
  ASSERT_NE(output.find("access_du_mismatch"), std::string::npos);
  ASSERT_NE(output.find("CN-BEAM-0001"), std::string::npos);
  ASSERT_NE(output.find("0x"), std::string::npos);
}

TEST(cu_cp_unit_config, ntn_ues_command_prints_connected_handover_state)
{
  fake_cu_cp_command_handler command_handler;
  srs_cu_cp::cu_cp_ntn_ue_status ue;
  ue.ue_index                                  = srs_cu_cp::uint_to_ue_index(8);
  ue.du_index                                  = srs_cu_cp::uint_to_du_index(0);
  ue.rnti                                      = to_rnti(0x4602);
  ue.serving_beam_id                           = "CN-BEAM-0001";
  ue.connected_handover_state                  = "handover_preparing";
  ue.connected_handover_reason                 = "location_boundary";
  ue.connected_handover_target_resource_state  = "applied_by_du";
  ue.connected_handover_source_beam_id         = "CN-BEAM-0001";
  ue.connected_handover_target_beam_id         = "CN-BEAM-0002";
  ue.connected_handover_target_nci             = make_nci(1);
  ue.connected_handover_target_uplink_resource_beam_id = "CN-BEAM-UL-0002";
  ue.connected_handover_target_uplink_resource_nci     = make_nci(2);
  ue.connected_handover_target_uplink_resource_du_index = srs_cu_cp::uint_to_du_index(0);
  ue.connected_handover_target_service_pair_reason      = "same_analog_tac_uplink_resource";
  command_handler.ntn.ue_status                = {ue};

  ntn_ues_app_command command(command_handler);
  ::testing::internal::CaptureStdout();
  const std::array<std::string, 0> args = {};
  command.execute(span<const std::string>{args});
  const std::string output = ::testing::internal::GetCapturedStdout();

  ASSERT_NE(output.find("NTN UE state guide: access_only=signaling_only control_only=connected_without_service "
                        "binding_pending=waiting_for_service_beam service_bound=using_service_beam"),
            std::string::npos);
  ASSERT_NE(output.find("NTN UE movement guide: reloc=move_before_service conn_ho=connected_move "
                        "loc_req=location_reporting_request core=core_location_requests"),
            std::string::npos);
  ASSERT_NE(output.find("conn_ho"), std::string::npos);
  ASSERT_NE(output.find("handover_preparing"), std::string::npos);
  ASSERT_NE(output.find("location_boundary"), std::string::npos);
  ASSERT_NE(output.find("applied_by_du"), std::string::npos);
  ASSERT_NE(output.find("CN-BEAM-0001"), std::string::npos);
  ASSERT_NE(output.find("CN-BEAM-0002"), std::string::npos);
  ASSERT_NE(output.find("service_pair_handover ue=8 dl_target=CN-BEAM-0002 ul_resource=CN-BEAM-UL-0002"),
            std::string::npos);
  ASSERT_NE(output.find("ul_du=0 reason=same_analog_tac_uplink_resource handover_reason=location_boundary"),
            std::string::npos);
}

TEST(cu_cp_unit_config, ntn_ues_command_prints_access_service_layer_state)
{
  fake_cu_cp_command_handler command_handler;
  srs_cu_cp::cu_cp_ntn_ue_status ue;
  ue.ue_index                       = srs_cu_cp::uint_to_ue_index(9);
  ue.du_index                       = srs_cu_cp::uint_to_du_index(0);
  ue.rnti                           = to_rnti(0x4603);
  ue.ntn_runtime_state              = "service_bound";
  ue.analog_access_released         = true;
  ue.access_layer_state             = "released_after_ics";
  ue.access_layer_reason            = "valid";
  ue.access_analog_beam_id          = "ANALOG-ACCESS-001";
  ue.access_du_index                = srs_cu_cp::uint_to_du_index(0);
  ue.service_layer_state            = "service_bound";
  ue.service_layer_reason           = "service_pair_bound";
  ue.service_digital_beam_id        = "CN-BEAM-0002";
  ue.service_downlink_beam_id       = "CN-BEAM-0002";
  ue.service_du_index               = srs_cu_cp::uint_to_du_index(0);
  ue.service_binding_source         = "paired_access_service_pair";
  ue.service_uplink_resource_beam_id = "CN-BEAM-UL-SVC-0007";
  ue.service_uplink_resource_nci     = make_nci(8);
  ue.service_uplink_resource_du_index = srs_cu_cp::uint_to_du_index(2);
  ue.service_pair_reason             = "same_analog_tac_uplink_resource";
  ue.last_downlink_wake_beam_id     = "CN-BEAM-DL-0007";
  ue.paired_uplink_access_beam_id   = "CN-BEAM-UL-0007";
  ue.paired_uplink_access_nci       = make_nci(7);
  ue.paired_uplink_access_du_index  = srs_cu_cp::uint_to_du_index(1);
  ue.paired_access_reason           = "same_analog_uplink_sibling";
  ue.access_rnti_ownership_state    = "released_after_ics";
  ue.access_rnti_ownership_reason   = "released_after_ics";
  ue.digital_slot_intent_state      = "active";
  ue.digital_slot_intent_reason     = "loaded_service_calendar";
  ue.ntn_capability_state           = srs_cu_cp::ntn_ue_capability_state::supported;
  ue.ntn_capability_reason          = "supported";
  ue.ntn_capability_scenario_support = srs_cu_cp::ntn_ue_capability_scenario_support::gso;
  ue.ntn_capability_scenario         = "gso";
  ue.ntn_capability_deployment_profile = "leo_ngso";
  ue.ntn_capability_profile_match      = false;
  ue.ntn_capability_profile_reason     = "scenario_mismatch";
  command_handler.ntn.ue_status     = {ue};

  ntn_ues_app_command command(command_handler);
  ::testing::internal::CaptureStdout();
  const std::array<std::string, 0> args = {};
  command.execute(span<const std::string>{args});
  const std::string output = ::testing::internal::GetCapturedStdout();

  ASSERT_NE(output.find("NTN UE state guide: access_only=signaling_only control_only=connected_without_service "
                        "binding_pending=waiting_for_service_beam service_bound=using_service_beam"),
            std::string::npos);
  ASSERT_NE(output.find("NTN UE movement guide: reloc=move_before_service conn_ho=connected_move "
                        "loc_req=location_reporting_request core=core_location_requests"),
            std::string::npos);
  ASSERT_NE(output.find("NTN UE paired access guide: dl_wake=downlink paging beam "
                        "ul_response=paired uplink access beam service can use bidirectional beam or paired DL service "
                        "plus UL resource beam"),
            std::string::npos);
  ASSERT_NE(output.find("access"), std::string::npos);
  ASSERT_NE(output.find("service"), std::string::npos);
  ASSERT_NE(output.find("binding"), std::string::npos);
  ASSERT_NE(output.find("rnti_owner"), std::string::npos);
  ASSERT_NE(output.find("slot_intent"), std::string::npos);
  ASSERT_NE(output.find("capability"), std::string::npos);
  ASSERT_NE(output.find("scenario"), std::string::npos);
  ASSERT_NE(output.find("deploy"), std::string::npos);
  ASSERT_NE(output.find("prof"), std::string::npos);
  ASSERT_NE(output.find("cap_reason"), std::string::npos);
  ASSERT_NE(output.find("profile_reason"), std::string::npos);
  ASSERT_NE(output.find("supported"), std::string::npos);
  ASSERT_NE(output.find("gso"), std::string::npos);
  ASSERT_NE(output.find("leo_ngso"), std::string::npos);
  ASSERT_NE(output.find("blocked"), std::string::npos);
  ASSERT_NE(output.find("scenario_mismatch"), std::string::npos);
  ASSERT_NE(output.find("runtime"), std::string::npos);
  ASSERT_NE(output.find("released_after_ics"), std::string::npos);
  ASSERT_NE(output.find("loaded_service_calendar"), std::string::npos);
  ASSERT_NE(output.find("released"), std::string::npos);
  ASSERT_NE(output.find("ANALOG-ACCESS-001"), std::string::npos);
  ASSERT_NE(output.find("CN-BEAM-0002"), std::string::npos);
  ASSERT_NE(output.find("paired_access_service_pair"), std::string::npos);
  ASSERT_NE(output.find("paired_access ue=9 dl_wake=CN-BEAM-DL-0007 ul_response=CN-BEAM-UL-0007"),
            std::string::npos);
  ASSERT_NE(output.find("ul_du=1 reason=same_analog_uplink_sibling"), std::string::npos);
  ASSERT_NE(output.find("service_pair ue=9 dl_service=CN-BEAM-0002 ul_resource=CN-BEAM-UL-SVC-0007"),
            std::string::npos);
  ASSERT_NE(output.find("ul_du=2 reason=same_analog_tac_uplink_resource service_reason=service_pair_bound"),
            std::string::npos);
  ASSERT_NE(output.find("slot_audit_target=paired_ul_resource"), std::string::npos);
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
  cfg.mobility_config.ntn_location_mobility.location_lost_release_grace_period_ms = 45000;
  cfg.mobility_config.ntn_location_mobility.idle_paging_context_max_age_ms        = 120000;
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
  ASSERT_EQ(ntn_cfg.location_lost_release_grace_period, std::chrono::milliseconds(45000));
  ASSERT_EQ(ntn_cfg.idle_paging_context_max_age, std::chrono::milliseconds(120000));
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
