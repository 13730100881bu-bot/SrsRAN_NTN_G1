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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * A copy of the GNU Affero General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */

#include "lib/cu_cp/ntn_mobility/ntn_onboard_position_plan.h"
#include "lib/cu_cp/ntn_mobility/ntn_onboard_position_plan_state.h"
#include "nlohmann/json.hpp"
#include "fmt/format.h"
#include "gtest/gtest.h"
#include <algorithm>
#include <map>
#include <set>

using namespace srsran;
using namespace srsran::srs_cu_cp;

namespace {

constexpr const char* catalog_hash        = "sha256:b39fe9c3ee9a9355b3546036b7f16e0fb858c953f8558cc4295122f2169fbe7a";
constexpr const char* registry_hash       = "sha256:7475821350e104b57a70d979d630f4b29a6cecb89ca0eca7b16dddf2ffee6a4a";
constexpr const char* access_profile_hash = "sha256:195786f4161e3b0fad6faa0605144948a7401c067a014bde684c1b29a8087d63";

std::chrono::system_clock::time_point at_ms(int64_t milliseconds)
{
  return std::chrono::system_clock::time_point{std::chrono::milliseconds{milliseconds}};
}

ntn_onboard_cell_identity make_cell(uint64_t nci, pci_t pci)
{
  return {nr_cell_identity::create(nci).value(), pci};
}

ntn_onboard_position_plan_config make_config(bool enabled = true)
{
  ntn_onboard_position_plan_config config;
  config.enabled          = enabled;
  config.satellite_id                       = "P01-S01";
  config.expected_catalog_id                = "global-land-l1-v1";
  config.expected_catalog_hash              = catalog_hash;
  config.expected_identity_registry_version = "mc-ntn-onboard-cell-registry-v1";
  config.expected_identity_registry_hash    = registry_hash;
  config.expected_access_profile_id         = "ntn-access-16a-64d-v1";
  config.expected_access_profile_hash       = access_profile_hash;
  config.onboard_cells[0] = make_cell(0x123450001ULL, 101);
  config.onboard_cells[1] = make_cell(0x123450002ULL, 202);
  return config;
}

ntn_versioned_position_plan make_plan(unsigned count,
                                      uint64_t schedule_version = 1,
                                      int64_t activation_ms = 1920,
                                      uint64_t catalog_version = 1)
{
  ntn_versioned_position_plan plan;
  plan.schema_version            = 2;
  plan.planning_run_id           = "planning-run-2026-07-15";
  plan.catalog_id                = "global-land-l1-v1";
  plan.catalog_hash              = catalog_hash;
  plan.identity_registry_version = "mc-ntn-onboard-cell-registry-v1";
  plan.identity_registry_hash    = registry_hash;
  plan.access_profile_id         = "ntn-access-16a-64d-v1";
  plan.access_profile_hash       = access_profile_hash;
  plan.satellite_id              = "P01-S01";
  plan.catalog_version  = catalog_version;
  plan.schedule_version = schedule_version;
  plan.valid_from       = at_ms(640);
  plan.valid_until      = at_ms(64000);
  plan.activation_epoch = at_ms(activation_ms);
  plan.onboard_cells    = make_config().onboard_cells;
  for (unsigned i = 0; i != count; ++i) {
    plan.visible_l1_positions.push_back({fmt::format("G{:06}", i + 1),
                                         10.0 + static_cast<double>(i / 32) * 0.05,
                                         20.0 + static_cast<double>(i % 32) * 0.05,
                                         0x7f});
  }
  plan.content_hash = compute_ntn_position_plan_content_hash(plan);
  return plan;
}

std::map<std::string, unsigned> make_owner_map(const ntn_activated_position_plan& plan)
{
  std::map<std::string, unsigned> owners;
  for (unsigned cell_index = 0; cell_index != plan.cell_positions.size(); ++cell_index) {
    for (const std::string& position_id : plan.cell_positions[cell_index].assigned_l1_ids) {
      EXPECT_TRUE(owners.emplace(position_id, cell_index).second);
    }
  }
  return owners;
}

ntn_initial_access_plan_event make_initial_access_event(const ntn_activated_position_plan& plan)
{
  ntn_initial_access_plan_event event;
  const auto ro_it = std::find_if(plan.access_calendar.begin(), plan.access_calendar.end(), [](const auto& intent) {
    return intent.purpose == ntn_access_calendar_purpose::prach_ro;
  });
  if (ro_it == plan.access_calendar.end()) {
    return event;
  }
  const auto beam_it = std::find_if(plan.access_calendar.begin(), plan.access_calendar.end(), [&](const auto& intent) {
    return intent.purpose == ntn_access_calendar_purpose::prach_ul_beam && intent.nci == ro_it->nci &&
           intent.position_id == ro_it->position_id && intent.start_time == ro_it->start_time &&
           intent.duration == ro_it->duration;
  });
  if (beam_it == plan.access_calendar.end()) {
    return event;
  }
  const auto cell_it = std::find_if(plan.cell_positions.begin(), plan.cell_positions.end(), [&](const auto& cell) {
    return cell.identity.nci == ro_it->nci;
  });
  if (cell_it == plan.cell_positions.end()) {
    return event;
  }

  event.satellite_id        = plan.source.satellite_id;
  event.catalog_version     = plan.source.catalog_version;
  event.schedule_version    = plan.source.schedule_version;
  event.source_content_hash = plan.source.content_hash;
  event.calendar_hash       = plan.calendar_hash;
  event.cell                = cell_it->identity;
  event.position_id         = ro_it->position_id;
  event.occasion_time       = plan.source.activation_epoch + ro_it->start_time + std::chrono::microseconds{1};
  event.ul_beam_port_id     = beam_it->port_id;
  return event;
}

} // namespace

TEST(ntn_onboard_position_plan, valid_plan_is_loaded_checked_and_held_pending_until_activation)
{
  ntn_onboard_position_plan_controller controller(make_config());
  const ntn_versioned_position_plan     plan = make_plan(32);

  const auto result = controller.submit(plan, at_ms(1280));

  ASSERT_TRUE(result.accepted);
  EXPECT_EQ(result.stage, ntn_position_plan_stage::pending);
  EXPECT_FALSE(controller.active_plan().has_value());
  ASSERT_TRUE(controller.pending_plan().has_value());
  EXPECT_TRUE(controller.pending_plan()->calendar_audit.accepted);
  EXPECT_EQ(controller.pending_plan()->calendar_audit.max_ssb_interval, std::chrono::milliseconds{80});
  EXPECT_EQ(controller.pending_plan()->calendar_audit.max_prach_interval, std::chrono::milliseconds{640});
  EXPECT_EQ(controller.candidate_inventory().size(), 32U);
}

TEST(ntn_onboard_position_plan, empty_visible_inventory_produces_a_checked_explicit_deny_all_calendar)
{
  ntn_onboard_position_plan_controller controller(make_config());

  const auto result = controller.submit(make_plan(0), at_ms(1280));

  ASSERT_TRUE(result.accepted);
  ASSERT_TRUE(controller.pending_plan().has_value());
  EXPECT_TRUE(controller.candidate_inventory().empty());
  EXPECT_TRUE(controller.pending_plan()->access_calendar.empty());
  EXPECT_TRUE(controller.pending_plan()->calendar_audit.accepted);
  EXPECT_FALSE(controller.pending_plan()->calendar_hash.empty());
}

TEST(ntn_onboard_position_plan, management_center_json_parses_and_keeps_opaque_cell_identities)
{
  const ntn_versioned_position_plan plan = make_plan(2);
  ASSERT_EQ(plan.content_hash, "sha256:374593193a7a297734e583978263759e12209c491418eed55ee82af884ab599d");
  nlohmann::json root;
  root["schema_version"]    = plan.schema_version;
  root["planning_run_id"]   = plan.planning_run_id;
  root["catalog"]           = {{"id", plan.catalog_id}, {"sha256", plan.catalog_hash}};
  root["identity_registry"] = {{"version", plan.identity_registry_version}, {"sha256", plan.identity_registry_hash}};
  root["access_profile"]    = {{"id", plan.access_profile_id}, {"sha256", plan.access_profile_hash}};
  root["satellite_id"]             = plan.satellite_id;
  root["catalog_version"]          = plan.catalog_version;
  root["schedule_version"]         = plan.schedule_version;
  root["content_hash"]             = plan.content_hash;
  root["valid_from_unix_ms"]       = 640;
  root["valid_until_unix_ms"]      = 64000;
  root["activation_epoch_unix_ms"] = 1920;
  for (const auto& cell : plan.onboard_cells) {
    root["onboard_cells"].push_back({{"nci", cell.nci.value()}, {"pci", cell.pci}});
  }
  for (const auto& position : plan.visible_l1_positions) {
    root["visible_l1_positions"].push_back({{"position_id", position.position_id},
                                            {"latitude_deg", position.latitude_deg},
                                            {"longitude_deg", position.longitude_deg},
                                            {"child_mask", position.child_mask}});
  }

  auto parsed = parse_ntn_position_plan_json(root.dump());
  ASSERT_TRUE(parsed.has_value()) << parsed.error();
  EXPECT_EQ(parsed.value().onboard_cells[0].nci, plan.onboard_cells[0].nci);
  EXPECT_EQ(parsed.value().onboard_cells[1].pci, plan.onboard_cells[1].pci);
  EXPECT_EQ(compute_ntn_position_plan_content_hash(parsed.value()), plan.content_hash);

  ntn_onboard_position_plan_controller controller(make_config());
  EXPECT_TRUE(controller.submit(parsed.value(), at_ms(1280)).accepted);
}

TEST(ntn_onboard_position_plan, schema_v2_json_rejects_unknown_missing_and_invalid_child_fields)
{
  const ntn_versioned_position_plan plan = make_plan(1);
  nlohmann::json                    root = {
      {"schema_version", plan.schema_version},
      {"planning_run_id", plan.planning_run_id},
      {"catalog", {{"id", plan.catalog_id}, {"sha256", plan.catalog_hash}}},
      {"identity_registry", {{"version", plan.identity_registry_version}, {"sha256", plan.identity_registry_hash}}},
      {"access_profile", {{"id", plan.access_profile_id}, {"sha256", plan.access_profile_hash}}},
      {"satellite_id", plan.satellite_id},
      {"catalog_version", plan.catalog_version},
      {"schedule_version", plan.schedule_version},
      {"content_hash", plan.content_hash},
      {"valid_from_unix_ms", 640},
      {"valid_until_unix_ms", 64000},
      {"activation_epoch_unix_ms", 1920},
      {"onboard_cells",
       {{{"nci", plan.onboard_cells[0].nci.value()}, {"pci", plan.onboard_cells[0].pci}},
        {{"nci", plan.onboard_cells[1].nci.value()}, {"pci", plan.onboard_cells[1].pci}}}},
      {"visible_l1_positions",
       {{{"position_id", plan.visible_l1_positions[0].position_id},
         {"latitude_deg", plan.visible_l1_positions[0].latitude_deg},
         {"longitude_deg", plan.visible_l1_positions[0].longitude_deg},
         {"child_mask", plan.visible_l1_positions[0].child_mask}}}}};

  auto invalid                     = root;
  invalid["catalog"]["unexpected"] = true;
  auto parsed                      = parse_ntn_position_plan_json(invalid.dump());
  ASSERT_FALSE(parsed.has_value());
  EXPECT_NE(parsed.error().find("unknown field 'catalog.unexpected'"), std::string::npos);

  invalid                                   = root;
  invalid["onboard_cells"][0]["unexpected"] = true;
  parsed                                    = parse_ntn_position_plan_json(invalid.dump());
  ASSERT_FALSE(parsed.has_value());
  EXPECT_NE(parsed.error().find("unknown field 'onboard_cells[0].unexpected'"), std::string::npos);

  invalid = root;
  invalid["visible_l1_positions"][0].erase("child_mask");
  parsed = parse_ntn_position_plan_json(invalid.dump());
  ASSERT_FALSE(parsed.has_value());
  EXPECT_NE(parsed.error().find("missing field 'visible_l1_positions[0].child_mask'"), std::string::npos);

  invalid                                          = root;
  invalid["visible_l1_positions"][0]["child_mask"] = 128;
  parsed                                           = parse_ntn_position_plan_json(invalid.dump());
  ASSERT_FALSE(parsed.has_value());
  EXPECT_NE(parsed.error().find("child_mask must be in 1..127"), std::string::npos);

  invalid                                          = root;
  invalid["visible_l1_positions"][0]["child_mask"] = 1.5;
  parsed                                           = parse_ntn_position_plan_json(invalid.dump());
  ASSERT_FALSE(parsed.has_value());
  EXPECT_NE(parsed.error().find("child_mask must be an unsigned integer"), std::string::npos);

  invalid                    = root;
  invalid["catalog_version"] = 1.5;
  parsed                     = parse_ntn_position_plan_json(invalid.dump());
  ASSERT_FALSE(parsed.has_value());
  EXPECT_NE(parsed.error().find("catalog_version must be an unsigned integer"), std::string::npos);

  invalid                            = root;
  invalid["onboard_cells"][0]["pci"] = -1;
  parsed                             = parse_ntn_position_plan_json(invalid.dump());
  ASSERT_FALSE(parsed.has_value());
  EXPECT_NE(parsed.error().find("pci must be an unsigned integer"), std::string::npos);

  invalid                             = root;
  invalid["activation_epoch_unix_ms"] = 1920.5;
  parsed                              = parse_ntn_position_plan_json(invalid.dump());
  ASSERT_FALSE(parsed.has_value());
  EXPECT_NE(parsed.error().find("activation_epoch_unix_ms must be an integer"), std::string::npos);
}

TEST(ntn_onboard_position_plan, schema_v1_is_dry_run_only_and_schema_v2_binds_planning_context)
{
  ntn_versioned_position_plan legacy = make_plan(2);
  legacy.schema_version              = 1;
  legacy.planning_run_id.clear();
  legacy.catalog_id.clear();
  legacy.catalog_hash.clear();
  legacy.identity_registry_version.clear();
  legacy.identity_registry_hash.clear();
  legacy.access_profile_id.clear();
  legacy.access_profile_hash.clear();
  for (ntn_l1_position& position : legacy.visible_l1_positions) {
    position.child_mask = 0;
  }
  legacy.content_hash = compute_ntn_position_plan_content_hash(legacy);

  ntn_onboard_position_plan_controller dry_run_controller(make_config());
  EXPECT_TRUE(dry_run_controller.submit(legacy, at_ms(1280)).accepted);
  EXPECT_TRUE(dry_run_controller.advance_time(legacy.activation_epoch));
  ASSERT_TRUE(dry_run_controller.active_plan().has_value());
  EXPECT_EQ(dry_run_controller.active_plan()->source.schema_version, 1U);
  EXPECT_FALSE(dry_run_controller.active_has_external_apply_evidence());

  ntn_onboard_position_plan_config execution_config = make_config();
  execution_config.require_external_apply           = true;
  ntn_onboard_position_plan_controller execution_controller(execution_config);
  const auto                           legacy_result = execution_controller.submit(legacy, at_ms(1280));
  EXPECT_FALSE(legacy_result.accepted);
  EXPECT_EQ(legacy_result.reason, ntn_position_plan_reject_reason::unbound_planning_context);

  ntn_versioned_position_plan mismatched = make_plan(2);
  mismatched.catalog_id                  = "different-catalog";
  mismatched.content_hash                = compute_ntn_position_plan_content_hash(mismatched);
  ntn_onboard_position_plan_controller mismatch_controller(make_config());
  const auto                           mismatch_result = mismatch_controller.submit(mismatched, at_ms(1280));
  EXPECT_FALSE(mismatch_result.accepted);
  EXPECT_EQ(mismatch_result.reason, ntn_position_plan_reject_reason::planning_context_mismatch);

  ntn_versioned_position_plan unsupported = make_plan(2);
  unsupported.schema_version              = 3;
  unsupported.content_hash                = compute_ntn_position_plan_content_hash(unsupported);
  ntn_onboard_position_plan_controller unsupported_controller(make_config());
  const auto                           unsupported_result = unsupported_controller.submit(unsupported, at_ms(1280));
  EXPECT_FALSE(unsupported_result.accepted);
  EXPECT_EQ(unsupported_result.reason, ntn_position_plan_reject_reason::unsupported_schema);
}

TEST(ntn_onboard_position_plan, access_profile_hash_binds_the_complete_local_resource_model)
{
  ntn_onboard_position_plan_config config = make_config();
  EXPECT_EQ(compute_ntn_access_profile_hash(config), access_profile_hash);

  ntn_onboard_position_plan_controller accepted_controller(config);
  EXPECT_TRUE(accepted_controller.submit(make_plan(2), at_ms(1280)).accepted);

  ++config.max_digital_ports_per_cell;
  ntn_onboard_position_plan_controller drifted_controller(config);
  const auto                           drifted = drifted_controller.submit(make_plan(2), at_ms(1280));
  EXPECT_FALSE(drifted.accepted);
  EXPECT_EQ(drifted.reason, ntn_position_plan_reject_reason::planning_context_mismatch);
}

TEST(ntn_onboard_position_plan, web_exporter_and_cpp_share_the_same_canonical_hash_golden_vector)
{
  ntn_versioned_position_plan plan;
  plan.satellite_id         = "P01-S01";
  plan.catalog_version   = 10;
  plan.schedule_version  = 20;
  plan.valid_from        = at_ms(1780000000000LL);
  plan.valid_until       = at_ms(1780003600000LL);
  plan.activation_epoch  = at_ms(1780000000640LL);
  plan.onboard_cells     = {make_cell(4886691841ULL, 101), make_cell(4886691842ULL, 202)};
  plan.visible_l1_positions = {{"G000002", 56.7654, -158.36066}, {"G000001", 10.0, 20.0}};

  EXPECT_EQ(compute_ntn_position_plan_content_hash(plan),
            "sha256:0e92970559dc95a87d3413e9300cf99ad257ea85e43e66898146a1af86d8c8b8");
}

TEST(ntn_onboard_position_plan, partition_is_deterministic_balanced_and_assigns_every_l1_exactly_once)
{
  ntn_onboard_position_plan_controller first(make_config());
  ntn_onboard_position_plan_controller second(make_config());
  const ntn_versioned_position_plan     plan = make_plan(127);
  ntn_versioned_position_plan           reordered = plan;
  std::reverse(reordered.visible_l1_positions.begin(), reordered.visible_l1_positions.end());
  ASSERT_EQ(compute_ntn_position_plan_content_hash(reordered), plan.content_hash);

  ASSERT_TRUE(first.submit(plan, at_ms(1280)).accepted);
  ASSERT_TRUE(second.submit(reordered, at_ms(1280)).accepted);
  ASSERT_TRUE(first.pending_plan().has_value());
  ASSERT_TRUE(second.pending_plan().has_value());

  const auto first_owners  = make_owner_map(*first.pending_plan());
  const auto second_owners = make_owner_map(*second.pending_plan());
  EXPECT_EQ(first_owners, second_owners);
  EXPECT_EQ(first_owners.size(), plan.visible_l1_positions.size());
  EXPECT_EQ(first.pending_plan()->cell_positions[0].assigned_l1_ids.size(), 64U);
  EXPECT_EQ(first.pending_plan()->cell_positions[1].assigned_l1_ids.size(), 63U);
}

TEST(ntn_onboard_position_plan, small_update_preserves_existing_assignments_and_cell_identity)
{
  ntn_onboard_position_plan_controller controller(make_config());
  ntn_versioned_position_plan           initial = make_plan(20, 1, 1280);
  ASSERT_TRUE(controller.submit(initial, at_ms(1280)).accepted);
  ASSERT_TRUE(controller.active_plan().has_value());
  const auto initial_owners = make_owner_map(*controller.active_plan());

  ntn_versioned_position_plan update = make_plan(22, 2, 2560, 1);
  ASSERT_TRUE(controller.submit(update, at_ms(1920)).accepted);
  ASSERT_TRUE(controller.pending_plan().has_value());
  const auto update_owners = make_owner_map(*controller.pending_plan());

  unsigned changed = 0;
  for (const auto& entry : initial_owners) {
    changed += update_owners.at(entry.first) != entry.second;
  }
  EXPECT_LE(changed, 1U);
  for (unsigned i = 0; i != 2; ++i) {
    EXPECT_EQ(controller.pending_plan()->cell_positions[i].identity.nci, make_config().onboard_cells[i].nci);
    EXPECT_EQ(controller.pending_plan()->cell_positions[i].identity.pci, make_config().onboard_cells[i].pci);
  }
}

TEST(ntn_onboard_position_plan, full_256_l1_plan_uses_two_128_position_cells_without_trimming)
{
  ntn_onboard_position_plan_controller controller(make_config());
  const auto result = controller.submit(make_plan(256), at_ms(1280));

  ASSERT_TRUE(result.accepted);
  ASSERT_TRUE(controller.pending_plan().has_value());
  EXPECT_EQ(controller.candidate_inventory().size(), 256U);
  EXPECT_EQ(controller.pending_plan()->cell_positions[0].assigned_l1_ids.size(), 128U);
  EXPECT_EQ(controller.pending_plan()->cell_positions[1].assigned_l1_ids.size(), 128U);
  EXPECT_LE(controller.pending_plan()->calendar_audit.max_used_analog_ports_per_cell, 16U);
  EXPECT_LE(controller.pending_plan()->calendar_audit.max_used_analog_ports_per_satellite, 32U);
}

TEST(ntn_onboard_position_plan, overflow_keeps_all_257_candidates_and_preserves_old_active_plan)
{
  ntn_onboard_position_plan_controller controller(make_config());
  ASSERT_TRUE(controller.submit(make_plan(8, 1, 1280), at_ms(1280)).accepted);
  ASSERT_TRUE(controller.active_plan().has_value());

  const ntn_versioned_position_plan overflow = make_plan(257, 2, 2560);
  const auto                        result   = controller.submit(overflow, at_ms(1920));

  EXPECT_FALSE(result.accepted);
  EXPECT_EQ(result.reason, ntn_position_plan_reject_reason::schedule_overflow);
  EXPECT_EQ(controller.candidate_inventory().size(), 257U);
  EXPECT_EQ(controller.last_received_catalog_version(), overflow.catalog_version);
  EXPECT_EQ(controller.last_received_schedule_version(), overflow.schedule_version);
  EXPECT_EQ(controller.last_received_content_hash(), overflow.content_hash);
  EXPECT_EQ(controller.last_received_activation_epoch(), overflow.activation_epoch);
  ASSERT_TRUE(controller.active_plan().has_value());
  EXPECT_EQ(controller.active_plan()->source.schedule_version, 1U);
}

TEST(ntn_onboard_position_plan, restart_preserves_all_257_received_candidates_without_promoting_them)
{
  ntn_onboard_position_plan_config config = make_config();
  config.require_external_apply           = true;
  ntn_onboard_position_plan_controller controller(config);

  ASSERT_TRUE(controller.submit(make_plan(8, 1, 1280), at_ms(640)).accepted);
  const std::string active_hash = controller.pending_plan()->calendar_hash;
  ASSERT_TRUE(controller.mark_deployment_preparing(1, active_hash));
  ASSERT_TRUE(controller.mark_deployment_applied(1, active_hash));
  ASSERT_TRUE(controller.advance_time(at_ms(1280)));

  const ntn_versioned_position_plan overflow = make_plan(257, 2, 2560, 2);
  const auto                        result   = controller.submit(overflow, at_ms(1920));
  ASSERT_FALSE(result.accepted);
  ASSERT_EQ(result.reason, ntn_position_plan_reject_reason::schedule_overflow);

  const ntn_onboard_position_plan_persistent_state state = controller.make_persistent_state(6);
  ASSERT_TRUE(state.active.has_value());
  EXPECT_FALSE(state.pending.has_value());
  ASSERT_TRUE(state.received_plan.has_value());
  EXPECT_EQ(state.received_plan->schedule_version, overflow.schedule_version);
  EXPECT_EQ(state.received_plan->candidate_inventory.size(), 257U);

  ntn_onboard_position_plan_controller restarted(config);
  ASSERT_TRUE(restarted.restore_persistent_state(state, at_ms(1920)).has_value());
  ASSERT_TRUE(restarted.recovery_plan().has_value());
  EXPECT_EQ(restarted.recovery_plan()->source.schedule_version, 1U);
  EXPECT_EQ(restarted.highest_schedule_version_seen(), 1U);
  EXPECT_EQ(restarted.candidate_inventory().size(), 257U);
  EXPECT_EQ(restarted.last_received_catalog_version(), overflow.catalog_version);
  EXPECT_EQ(restarted.last_received_schedule_version(), overflow.schedule_version);
  EXPECT_EQ(restarted.last_received_content_hash(), overflow.content_hash);
  EXPECT_EQ(restarted.last_received_activation_epoch(), overflow.activation_epoch);
}

TEST(ntn_onboard_position_plan, schema_v2_null_received_plan_does_not_invent_an_observation_from_active_state)
{
  ntn_onboard_position_plan_config config = make_config();
  config.require_external_apply           = true;
  ntn_onboard_position_plan_controller controller(config);

  ASSERT_TRUE(controller.submit(make_plan(8, 1, 1280), at_ms(640)).accepted);
  const std::string calendar_hash = controller.pending_plan()->calendar_hash;
  ASSERT_TRUE(controller.mark_deployment_preparing(1, calendar_hash));
  ASSERT_TRUE(controller.mark_deployment_applied(1, calendar_hash));
  ASSERT_TRUE(controller.advance_time(at_ms(1280)));

  ntn_onboard_position_plan_persistent_state state = controller.make_persistent_state(7);
  ASSERT_EQ(state.schema_version, 2U);
  ASSERT_TRUE(state.active.has_value());
  state.received_plan.reset();

  ntn_onboard_position_plan_controller restarted(config);
  ASSERT_TRUE(restarted.restore_persistent_state(state, at_ms(1920)).has_value());
  ASSERT_TRUE(restarted.recovery_plan().has_value());
  EXPECT_FALSE(restarted.has_received_plan());
  EXPECT_TRUE(restarted.candidate_inventory().empty());
  EXPECT_FALSE(restarted.make_persistent_state(8).received_plan.has_value());
}

TEST(ntn_onboard_position_plan, rejects_satellite_hash_version_validity_activation_and_l1_errors)
{
  struct test_case {
    const char*                     name;
    ntn_position_plan_reject_reason expected;
    void (*mutate)(ntn_versioned_position_plan&);
    bool recompute_hash;
  };
  const std::vector<test_case> cases{
      {"satellite",
       ntn_position_plan_reject_reason::invalid_satellite_id,
       [](auto& plan) { plan.satellite_id = "P99-S999"; },
       true},
      {"legacy_satellite_spelling",
       ntn_position_plan_reject_reason::invalid_satellite_id,
       [](auto& plan) { plan.satellite_id = "P01-S001"; },
       true},
      {"hash",
       ntn_position_plan_reject_reason::invalid_hash,
       [](auto& plan) { plan.visible_l1_positions.front().latitude_deg += 1.0; },
       false},
      {"zero_version",
       ntn_position_plan_reject_reason::non_monotonic_version,
       [](auto& plan) { plan.schedule_version = 0; },
       true},
      {"expired",
       ntn_position_plan_reject_reason::expired,
       [](auto& plan) {
         plan.valid_from       = at_ms(0);
         plan.activation_epoch = at_ms(640);
         plan.valid_until      = at_ms(1280);
       },
       true},
      {"validity",
       ntn_position_plan_reject_reason::invalid_validity_window,
       [](auto& plan) {
         plan.valid_from  = at_ms(3200);
         plan.valid_until = at_ms(2560);
       },
       true},
      {"activation",
       ntn_position_plan_reject_reason::invalid_activation_epoch,
       [](auto& plan) { plan.activation_epoch = at_ms(2000); },
       true},
      {"activation_sub_ms",
       ntn_position_plan_reject_reason::invalid_activation_epoch,
       [](auto& plan) { plan.activation_epoch += std::chrono::microseconds{500}; },
       true},
      {"validity_sub_ms",
       ntn_position_plan_reject_reason::invalid_validity_window,
       [](auto& plan) { plan.valid_until += std::chrono::microseconds{500}; },
       true},
      {"l1_format",
       ntn_position_plan_reject_reason::invalid_l1_id,
       [](auto& plan) { plan.visible_l1_positions.front().position_id = "L000001"; },
       true},
      {"l1_duplicate",
       ntn_position_plan_reject_reason::duplicate_l1_id,
       [](auto& plan) { plan.visible_l1_positions[1].position_id = plan.visible_l1_positions[0].position_id; },
       true},
      {"child_mask",
       ntn_position_plan_reject_reason::invalid_child_mask,
       [](auto& plan) { plan.visible_l1_positions.front().child_mask = 0; },
       true}};

  for (const test_case& item : cases) {
    SCOPED_TRACE(item.name);
    ntn_onboard_position_plan_controller controller(make_config());
    ntn_versioned_position_plan           plan = make_plan(4);
    item.mutate(plan);
    if (item.recompute_hash) {
      plan.content_hash = compute_ntn_position_plan_content_hash(plan);
    }
    const auto result = controller.submit(plan, at_ms(1280));
    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.reason, item.expected);
    EXPECT_EQ(controller.last_rejection_reason(), item.expected);
    EXPECT_TRUE(controller.has_received_plan());
  }

  ntn_onboard_position_plan_controller version_controller(make_config());
  ASSERT_TRUE(version_controller.submit(make_plan(4, 2, 1280, 2), at_ms(1280)).accepted);
  ntn_versioned_position_plan old_version = make_plan(3, 1, 2560, 1);
  const auto version_result = version_controller.submit(old_version, at_ms(1920));
  EXPECT_FALSE(version_result.accepted);
  EXPECT_EQ(version_result.reason, ntn_position_plan_reject_reason::non_monotonic_version);
  // candidate_inventory is the complete most recently received management input, even when that input is rejected.
  // The accepted/active plan is reported separately and must not be changed by the replay.
  EXPECT_EQ(version_controller.last_received_schedule_version(), 1U);
  EXPECT_EQ(version_controller.candidate_inventory().size(), 3U);
  EXPECT_EQ(version_controller.highest_schedule_version_seen(), 2U);
  ASSERT_TRUE(version_controller.active_plan().has_value());
  EXPECT_EQ(version_controller.active_plan()->source.schedule_version, 2U);
  EXPECT_EQ(version_controller.active_plan()->source.visible_l1_positions.size(), 4U);
  const ntn_onboard_position_plan_persistent_state replay_state = version_controller.make_persistent_state(2);
  ASSERT_TRUE(replay_state.received_plan.has_value());
  EXPECT_EQ(replay_state.received_plan->schedule_version, 1U);
  EXPECT_EQ(replay_state.received_plan->candidate_inventory.size(), 3U);
}

TEST(ntn_onboard_position_plan, rejects_identity_change_without_changing_active_cell_identity)
{
  ntn_onboard_position_plan_controller controller(make_config());
  ASSERT_TRUE(controller.submit(make_plan(4, 1, 1280), at_ms(1280)).accepted);

  ntn_versioned_position_plan update = make_plan(4, 2, 2560);
  update.onboard_cells[1].pci         = 303;
  update.content_hash                = compute_ntn_position_plan_content_hash(update);
  const auto result                  = controller.submit(update, at_ms(1920));

  EXPECT_FALSE(result.accepted);
  EXPECT_EQ(result.reason, ntn_position_plan_reject_reason::identity_mismatch);
  ASSERT_TRUE(controller.active_plan().has_value());
  EXPECT_EQ(controller.active_plan()->cell_positions[1].identity.pci, 202);
}

TEST(ntn_onboard_position_plan, accepts_planned_pci_reuse_when_the_two_stable_cell_identities_match)
{
  ntn_onboard_position_plan_config config = make_config();
  config.onboard_cells[1].pci              = config.onboard_cells[0].pci;
  ntn_onboard_position_plan_controller controller(config);
  ntn_versioned_position_plan           plan = make_plan(4);
  plan.onboard_cells                          = config.onboard_cells;
  plan.content_hash                           = compute_ntn_position_plan_content_hash(plan);

  const auto result = controller.submit(plan, at_ms(1280));

  EXPECT_TRUE(result.accepted);
  ASSERT_TRUE(controller.pending_plan().has_value());
  EXPECT_EQ(controller.pending_plan()->cell_positions[0].identity.pci,
            controller.pending_plan()->cell_positions[1].identity.pci);
}

TEST(ntn_onboard_position_plan, failed_new_plan_does_not_replace_active_or_valid_pending_plan)
{
  ntn_onboard_position_plan_controller controller(make_config());
  ASSERT_TRUE(controller.submit(make_plan(4, 1, 1280), at_ms(1280)).accepted);
  ASSERT_TRUE(controller.submit(make_plan(6, 2, 3200), at_ms(1920)).accepted);
  ASSERT_TRUE(controller.pending_plan().has_value());

  ntn_versioned_position_plan invalid = make_plan(8, 3, 3840);
  invalid.content_hash                = "sha256:00";
  const auto result                  = controller.submit(invalid, at_ms(1920));

  EXPECT_FALSE(result.accepted);
  ASSERT_TRUE(controller.active_plan().has_value());
  ASSERT_TRUE(controller.pending_plan().has_value());
  EXPECT_EQ(controller.active_plan()->source.schedule_version, 1U);
  EXPECT_EQ(controller.pending_plan()->source.schedule_version, 2U);
}

TEST(ntn_onboard_position_plan, activation_epoch_switches_the_complete_plan_atomically)
{
  ntn_onboard_position_plan_controller controller(make_config());
  ASSERT_TRUE(controller.submit(make_plan(4, 1, 1280), at_ms(1280)).accepted);
  ASSERT_TRUE(controller.active_plan().has_value());

  ASSERT_TRUE(controller.submit(make_plan(12, 2, 3200), at_ms(1920)).accepted);
  EXPECT_FALSE(controller.advance_time(at_ms(3199)));
  ASSERT_TRUE(controller.active_plan().has_value());
  EXPECT_EQ(controller.active_plan()->source.schedule_version, 1U);
  ASSERT_TRUE(controller.pending_plan().has_value());
  EXPECT_EQ(controller.pending_plan()->source.schedule_version, 2U);

  EXPECT_TRUE(controller.advance_time(at_ms(3200)));
  ASSERT_TRUE(controller.active_plan().has_value());
  EXPECT_EQ(controller.active_plan()->source.schedule_version, 2U);
  EXPECT_FALSE(controller.pending_plan().has_value());
  EXPECT_EQ(make_owner_map(*controller.active_plan()).size(), 12U);
}

TEST(ntn_onboard_position_plan, active_plan_is_marked_expired_at_valid_until)
{
  ntn_onboard_position_plan_controller controller(make_config());
  ASSERT_TRUE(controller.submit(make_plan(4, 1, 1280), at_ms(1280)).accepted);
  ASSERT_TRUE(controller.active_plan().has_value());

  EXPECT_FALSE(controller.advance_time(at_ms(64000)));
  EXPECT_FALSE(controller.active_plan().has_value());
  EXPECT_EQ(controller.stage(), ntn_position_plan_stage::rejected);
  EXPECT_EQ(controller.last_rejection_reason(), ntn_position_plan_reject_reason::expired);

  const auto replay = controller.submit(make_plan(4, 1, 1280), at_ms(65000));
  EXPECT_FALSE(replay.accepted);
  EXPECT_EQ(replay.reason, ntn_position_plan_reject_reason::non_monotonic_version);
}

TEST(ntn_onboard_position_plan, externally_applied_active_expiry_removes_the_live_deployment_claim)
{
  ntn_onboard_position_plan_config config = make_config();
  config.require_external_apply           = true;
  ntn_onboard_position_plan_controller controller(config);

  ASSERT_TRUE(controller.submit(make_plan(4, 1, 1280), at_ms(1280)).accepted);
  ASSERT_TRUE(controller.pending_plan().has_value());
  const std::string calendar_hash = controller.pending_plan()->calendar_hash;
  ASSERT_TRUE(controller.mark_deployment_preparing(1, calendar_hash));
  ASSERT_TRUE(controller.mark_deployment_applied(1, calendar_hash));
  ASSERT_TRUE(controller.advance_time(at_ms(1280)));
  ASSERT_TRUE(controller.active_plan().has_value());
  ASSERT_TRUE(controller.active_has_external_apply_evidence());

  EXPECT_FALSE(controller.advance_time(at_ms(64000)));
  EXPECT_FALSE(controller.active_plan().has_value());
  EXPECT_FALSE(controller.active_has_external_apply_evidence());
  EXPECT_EQ(controller.deployment_stage(), ntn_position_plan_deployment_stage::rejected);
  EXPECT_EQ(controller.deployment_detail(), "active_plan_expired");

  const ntn_onboard_position_plan_persistent_state state = controller.make_persistent_state(2);
  EXPECT_FALSE(state.active.has_value());
  EXPECT_FALSE(state.pending.has_value());
  EXPECT_EQ(state.recorded_deployment_stage, ntn_position_plan_deployment_stage::rejected);
  EXPECT_EQ(state.recorded_deployment_schedule_version, 0U);
  EXPECT_TRUE(state.recorded_deployment_calendar_hash.empty());
  EXPECT_EQ(state.highest_schedule_version, 1U);
}

TEST(ntn_onboard_position_plan, expired_active_plan_does_not_hide_a_valid_future_pending_plan)
{
  ntn_onboard_position_plan_controller controller(make_config());
  ntn_versioned_position_plan           active = make_plan(4, 1, 1280);
  active.valid_until                           = at_ms(2000);
  active.content_hash                          = compute_ntn_position_plan_content_hash(active);
  ASSERT_TRUE(controller.submit(active, at_ms(1280)).accepted);

  const auto result = controller.submit(make_plan(6, 2, 3200), at_ms(2500));

  EXPECT_TRUE(result.accepted);
  EXPECT_EQ(result.stage, ntn_position_plan_stage::pending);
  EXPECT_FALSE(controller.active_plan().has_value());
  ASSERT_TRUE(controller.pending_plan().has_value());
  EXPECT_EQ(controller.pending_plan()->source.schedule_version, 2U);
  EXPECT_EQ(controller.last_rejection_reason(), ntn_position_plan_reject_reason::expired);
  EXPECT_EQ(controller.last_rejected_schedule_version(), 1U);
}

TEST(ntn_onboard_position_plan, long_runtime_deadlines_are_split_into_timer_safe_slices)
{
  EXPECT_EQ(limit_ntn_position_plan_timer_delay(std::chrono::milliseconds{1}), std::chrono::milliseconds{1});
  EXPECT_EQ(limit_ntn_position_plan_timer_delay(std::chrono::hours{24}),
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::hours{24}));
  EXPECT_EQ(limit_ntn_position_plan_timer_delay(std::chrono::hours{24 * 90}),
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::hours{24}));
}

TEST(ntn_onboard_position_plan, external_execution_requires_matching_applied_feedback_before_atomic_activation)
{
  ntn_onboard_position_plan_config config = make_config();
  config.require_external_apply           = true;
  ntn_onboard_position_plan_controller controller(config);

  ASSERT_TRUE(controller.submit(make_plan(8, 1, 1920), at_ms(1280)).accepted);
  ASSERT_TRUE(controller.pending_plan().has_value());
  const std::string calendar_hash = controller.pending_plan()->calendar_hash;
  EXPECT_EQ(controller.deployment_stage(), ntn_position_plan_deployment_stage::not_sent);
  EXPECT_TRUE(controller.mark_deployment_preparing(1, calendar_hash));
  EXPECT_TRUE(controller.mark_deployment_ready(1, calendar_hash));
  EXPECT_FALSE(controller.advance_time(at_ms(1920)));
  EXPECT_FALSE(controller.active_plan().has_value());

  EXPECT_FALSE(controller.mark_deployment_applied(2, calendar_hash));
  EXPECT_FALSE(controller.mark_deployment_applied(1, "sha256:stale"));
  EXPECT_TRUE(controller.mark_deployment_applied(1, calendar_hash));
  EXPECT_TRUE(controller.advance_time(at_ms(1920)));
  ASSERT_TRUE(controller.active_plan().has_value());
  EXPECT_EQ(controller.active_plan()->source.schedule_version, 1U);
  EXPECT_TRUE(controller.active_has_external_apply_evidence());
  EXPECT_EQ(controller.deployment_stage(), ntn_position_plan_deployment_stage::applied);
}

TEST(ntn_onboard_position_plan, lost_du_connection_hides_active_evidence_until_matching_query_reconfirms_it)
{
  ntn_onboard_position_plan_config config = make_config();
  config.require_external_apply           = true;
  ntn_onboard_position_plan_controller controller(config);

  ASSERT_TRUE(controller.submit(make_plan(4, 1, 1280), at_ms(640)).accepted);
  const std::string active_hash = controller.pending_plan()->calendar_hash;
  ASSERT_TRUE(controller.mark_deployment_preparing(1, active_hash));
  ASSERT_TRUE(controller.mark_deployment_applied(1, active_hash));
  ASSERT_TRUE(controller.advance_time(at_ms(1280)));
  ASSERT_TRUE(controller.submit(make_plan(6, 2, 3200, 2), at_ms(1920)).accepted);

  ASSERT_TRUE(controller.require_du_reconciliation_after_connection_loss(
      "du_connection_lost_awaiting_matching_query"));
  EXPECT_FALSE(controller.active_plan().has_value());
  EXPECT_FALSE(controller.pending_plan().has_value());
  EXPECT_FALSE(controller.active_has_external_apply_evidence());
  ASSERT_TRUE(controller.recovery_plan().has_value());
  EXPECT_EQ(controller.recovery_plan()->source.schedule_version, 1U);
  EXPECT_EQ(controller.recovery_stage(), ntn_position_plan_recovery_stage::reconciling);
  EXPECT_EQ(controller.deployment_stage(), ntn_position_plan_deployment_stage::preparing);
  EXPECT_EQ(controller.highest_schedule_version_seen(), 2U);
  EXPECT_EQ(controller.candidate_inventory().size(), 6U);
  EXPECT_FALSE(controller.require_du_reconciliation_after_connection_loss(
      "du_connection_lost_awaiting_matching_query"));

  const ntn_onboard_position_plan_persistent_state hidden_state = controller.make_persistent_state(3);
  ASSERT_TRUE(hidden_state.active.has_value());
  ASSERT_TRUE(hidden_state.pending.has_value());
  EXPECT_EQ(hidden_state.active->source.schedule_version, 1U);
  EXPECT_EQ(hidden_state.pending->source.schedule_version, 2U);

  ntn_onboard_position_plan_controller restored(config);
  ASSERT_TRUE(restored.restore_persistent_state(hidden_state, at_ms(2000)).has_value());
  EXPECT_EQ(restored.candidate_inventory().size(), 6U);

  ASSERT_TRUE(controller.confirm_recovery_applied(1, active_hash, at_ms(2000)));
  ASSERT_TRUE(controller.active_plan().has_value());
  EXPECT_EQ(controller.active_plan()->source.schedule_version, 1U);
  EXPECT_TRUE(controller.active_has_external_apply_evidence());
  ASSERT_TRUE(controller.pending_plan().has_value());
  EXPECT_EQ(controller.pending_plan()->source.schedule_version, 2U);
  EXPECT_EQ(controller.deployment_stage(), ntn_position_plan_deployment_stage::not_sent);
  EXPECT_EQ(controller.candidate_inventory().size(), 6U);
}

TEST(ntn_onboard_position_plan, lost_du_connection_reconciles_inflight_update_before_active_fallback)
{
  ntn_onboard_position_plan_config config = make_config();
  config.require_external_apply           = true;
  ntn_onboard_position_plan_controller controller(config);

  ASSERT_TRUE(controller.submit(make_plan(4, 1, 1280), at_ms(640)).accepted);
  const std::string active_hash = controller.pending_plan()->calendar_hash;
  ASSERT_TRUE(controller.mark_deployment_preparing(1, active_hash));
  ASSERT_TRUE(controller.mark_deployment_applied(1, active_hash));
  ASSERT_TRUE(controller.advance_time(at_ms(1280)));

  ASSERT_TRUE(controller.submit(make_plan(6, 2, 3200, 2), at_ms(1920)).accepted);
  const std::string pending_hash = controller.pending_plan()->calendar_hash;
  ASSERT_TRUE(controller.mark_deployment_preparing(2, pending_hash));
  ASSERT_TRUE(controller.require_du_reconciliation_after_connection_loss({}));
  ASSERT_TRUE(controller.recovery_plan().has_value());
  EXPECT_EQ(controller.recovery_plan()->source.schedule_version, 2U);
  ASSERT_TRUE(controller.recovery_fallback_plan().has_value());
  EXPECT_EQ(controller.recovery_fallback_plan()->source.schedule_version, 1U);

  ASSERT_TRUE(controller.reject_recovery(2,
                                         pending_hash,
                                         ntn_position_plan_reject_reason::du_reconciliation_failed,
                                         "calendar_not_found"));
  ASSERT_TRUE(controller.recovery_plan().has_value());
  EXPECT_EQ(controller.recovery_plan()->source.schedule_version, 1U);
  EXPECT_FALSE(controller.active_has_external_apply_evidence());
  EXPECT_EQ(controller.highest_schedule_version_seen(), 2U);
  EXPECT_EQ(controller.candidate_inventory().size(), 6U);
}

TEST(ntn_onboard_position_plan, future_applied_update_remains_pending_until_activation_epoch)
{
  ntn_onboard_position_plan_config config = make_config();
  config.require_external_apply           = true;
  ntn_onboard_position_plan_controller controller(config);

  ASSERT_TRUE(controller.submit(make_plan(4, 1, 1280), at_ms(640)).accepted);
  const std::string active_hash = controller.pending_plan()->calendar_hash;
  ASSERT_TRUE(controller.mark_deployment_preparing(1, active_hash));
  ASSERT_TRUE(controller.mark_deployment_applied(1, active_hash));
  ASSERT_TRUE(controller.advance_time(at_ms(1280)));

  ASSERT_TRUE(controller.submit(make_plan(6, 2, 3200, 2), at_ms(1920)).accepted);
  const std::string pending_hash = controller.pending_plan()->calendar_hash;
  ASSERT_TRUE(controller.mark_deployment_preparing(2, pending_hash));
  ASSERT_TRUE(controller.mark_deployment_applied(2, pending_hash));
  ASSERT_TRUE(controller.require_du_reconciliation_after_connection_loss({}));

  ASSERT_TRUE(controller.confirm_recovery_applied(2, pending_hash, at_ms(2000)));
  EXPECT_FALSE(controller.active_plan().has_value());
  ASSERT_TRUE(controller.pending_plan().has_value());
  EXPECT_EQ(controller.pending_plan()->source.schedule_version, 2U);
  EXPECT_FALSE(controller.recovery_plan().has_value());
  EXPECT_EQ(controller.recovery_stage(), ntn_position_plan_recovery_stage::reconciled);
  EXPECT_EQ(controller.deployment_stage(), ntn_position_plan_deployment_stage::applied);
  EXPECT_FALSE(controller.active_has_external_apply_evidence());
  EXPECT_EQ(controller.highest_schedule_version_seen(), 2U);
  EXPECT_EQ(controller.candidate_inventory().size(), 6U);

  const ntn_onboard_position_plan_persistent_state pending_state = controller.make_persistent_state(4);
  ASSERT_TRUE(pending_state.active.has_value());
  EXPECT_EQ(pending_state.active->source.schedule_version, 1U);
  ASSERT_TRUE(pending_state.pending.has_value());
  EXPECT_EQ(pending_state.pending->source.schedule_version, 2U);
  EXPECT_EQ(pending_state.recorded_deployment_stage, ntn_position_plan_deployment_stage::applied);
  EXPECT_EQ(pending_state.recorded_deployment_schedule_version, 2U);

  // A second connection loss before the future epoch must re-check the update without discarding the historical
  // fallback that is still needed if the update later expires or is rejected.
  ASSERT_TRUE(controller.require_du_reconciliation_after_connection_loss("second_du_connection_lost"));
  ASSERT_TRUE(controller.recovery_plan().has_value());
  EXPECT_EQ(controller.recovery_plan()->source.schedule_version, 2U);
  ASSERT_TRUE(controller.recovery_fallback_plan().has_value());
  EXPECT_EQ(controller.recovery_fallback_plan()->source.schedule_version, 1U);
  ASSERT_TRUE(controller.confirm_recovery_applied(2, pending_hash, at_ms(2100)));
  EXPECT_FALSE(controller.active_plan().has_value());
  ASSERT_TRUE(controller.pending_plan().has_value());
  ASSERT_TRUE(controller.recovery_fallback_plan().has_value());
  EXPECT_EQ(controller.recovery_fallback_plan()->source.schedule_version, 1U);

  EXPECT_FALSE(controller.advance_time(at_ms(3199)));
  EXPECT_FALSE(controller.active_plan().has_value());
  ASSERT_TRUE(controller.advance_time(at_ms(3200)));
  ASSERT_TRUE(controller.active_plan().has_value());
  EXPECT_EQ(controller.active_plan()->source.schedule_version, 2U);
  EXPECT_FALSE(controller.pending_plan().has_value());
  EXPECT_FALSE(controller.recovery_fallback_plan().has_value());
  EXPECT_TRUE(controller.active_has_external_apply_evidence());
  EXPECT_EQ(controller.deployment_stage(), ntn_position_plan_deployment_stage::applied);
  EXPECT_EQ(controller.candidate_inventory().size(), 6U);
}

TEST(ntn_onboard_position_plan,
     expired_hidden_fallback_is_taken_exactly_at_deadline_without_disturbing_pending_state)
{
  ntn_onboard_position_plan_config config = make_config();
  config.require_external_apply           = true;
  ntn_onboard_position_plan_controller controller(config);

  ntn_versioned_position_plan fallback = make_plan(4, 1, 1280);
  fallback.valid_until                    = at_ms(3200);
  fallback.content_hash                   = compute_ntn_position_plan_content_hash(fallback);
  const std::string fallback_content_hash = fallback.content_hash;
  ASSERT_TRUE(controller.submit(fallback, at_ms(640)).accepted);
  const std::string fallback_hash = controller.pending_plan()->calendar_hash;
  ASSERT_TRUE(controller.mark_deployment_preparing(1, fallback_hash));
  ASSERT_TRUE(controller.mark_deployment_applied(1, fallback_hash));
  ASSERT_TRUE(controller.advance_time(at_ms(1280)));

  ASSERT_TRUE(controller.submit(make_plan(6, 2, 3840, 2), at_ms(1920)).accepted);
  const std::string pending_hash = controller.pending_plan()->calendar_hash;
  ASSERT_TRUE(controller.mark_deployment_preparing(2, pending_hash));
  ASSERT_TRUE(controller.mark_deployment_applied(2, pending_hash));
  ASSERT_TRUE(controller.require_du_reconciliation_after_connection_loss({}));
  ASSERT_TRUE(controller.confirm_recovery_applied(2, pending_hash, at_ms(2000)));
  ASSERT_TRUE(controller.recovery_fallback_plan().has_value());

  EXPECT_FALSE(controller.take_expired_recovery_fallback(at_ms(3199)).has_value());
  ASSERT_TRUE(controller.recovery_fallback_plan().has_value());
  EXPECT_EQ(controller.recovery_fallback_plan()->source.schedule_version, 1U);

  const auto expired = controller.take_expired_recovery_fallback(at_ms(3200));
  ASSERT_TRUE(expired.has_value());
  EXPECT_EQ(expired->source.schedule_version, 1U);
  EXPECT_EQ(expired->source.content_hash, fallback_content_hash);
  EXPECT_EQ(expired->calendar_hash, fallback_hash);
  EXPECT_FALSE(controller.recovery_fallback_plan().has_value());
  EXPECT_FALSE(controller.active_plan().has_value());
  ASSERT_TRUE(controller.pending_plan().has_value());
  EXPECT_EQ(controller.pending_plan()->source.schedule_version, 2U);
  EXPECT_EQ(controller.pending_plan()->calendar_hash, pending_hash);
  EXPECT_EQ(controller.highest_schedule_version_seen(), 2U);
  EXPECT_EQ(controller.candidate_inventory().size(), 6U);

  const ntn_onboard_position_plan_persistent_state state = controller.make_persistent_state(7);
  EXPECT_FALSE(state.active.has_value());
  ASSERT_TRUE(state.pending.has_value());
  EXPECT_EQ(state.pending->source.schedule_version, 2U);
  EXPECT_EQ(state.pending->calendar_hash, pending_hash);
  EXPECT_EQ(state.highest_schedule_version, 2U);
  ASSERT_TRUE(state.received_plan.has_value());
  EXPECT_EQ(state.received_plan->schedule_version, 2U);
  EXPECT_EQ(state.received_plan->candidate_inventory.size(), 6U);
}

TEST(ntn_onboard_position_plan, replacement_failure_after_hidden_fallback_expiry_does_not_restore_the_old_plan)
{
  ntn_onboard_position_plan_config config = make_config();
  config.require_external_apply           = true;
  ntn_onboard_position_plan_controller controller(config);
  ntn_versioned_position_plan           fallback = make_plan(4, 1, 1280);
  fallback.valid_until                           = at_ms(3200);
  fallback.content_hash                          = compute_ntn_position_plan_content_hash(fallback);
  ASSERT_TRUE(controller.submit(fallback, at_ms(640)).accepted);
  const std::string fallback_hash = controller.pending_plan()->calendar_hash;
  ASSERT_TRUE(controller.mark_deployment_preparing(1, fallback_hash));
  ASSERT_TRUE(controller.mark_deployment_applied(1, fallback_hash));
  ASSERT_TRUE(controller.advance_time(at_ms(1280)));

  ASSERT_TRUE(controller.submit(make_plan(6, 2, 3840, 2), at_ms(1920)).accepted);
  const std::string early_hash = controller.pending_plan()->calendar_hash;
  ASSERT_TRUE(controller.mark_deployment_preparing(2, early_hash));
  ASSERT_TRUE(controller.mark_deployment_applied(2, early_hash));
  ASSERT_TRUE(controller.require_du_reconciliation_after_connection_loss({}));
  ASSERT_TRUE(controller.confirm_recovery_applied(2, early_hash, at_ms(2000)));
  ASSERT_TRUE(controller.take_expired_recovery_fallback(at_ms(3200)).has_value());

  ASSERT_TRUE(controller.submit(make_plan(8, 3, 4480, 3), at_ms(3200)).accepted);
  ASSERT_TRUE(controller.pending_plan().has_value());
  const std::string replacement_hash = controller.pending_plan()->calendar_hash;
  ASSERT_TRUE(controller.reject_pending_deployment(3,
                                                    replacement_hash,
                                                    ntn_position_plan_reject_reason::du_prepare_rejected,
                                                    "replacement_prepare_failed_after_fallback_expiry"));
  EXPECT_FALSE(controller.active_plan().has_value());
  EXPECT_FALSE(controller.pending_plan().has_value());
  EXPECT_FALSE(controller.recovery_plan().has_value());
  EXPECT_FALSE(controller.recovery_fallback_plan().has_value());
  EXPECT_EQ(controller.highest_schedule_version_seen(), 3U);

  const ntn_onboard_position_plan_persistent_state state = controller.make_persistent_state(8);
  EXPECT_FALSE(state.active.has_value());
  EXPECT_FALSE(state.pending.has_value());
  ASSERT_TRUE(state.received_plan.has_value());
  EXPECT_EQ(state.received_plan->schedule_version, 3U);
  EXPECT_EQ(state.received_plan->candidate_inventory.size(), 8U);
}

TEST(ntn_onboard_position_plan, pending_plan_can_activate_at_the_same_instant_its_hidden_fallback_expires)
{
  ntn_onboard_position_plan_config config = make_config();
  config.require_external_apply           = true;
  ntn_onboard_position_plan_controller controller(config);
  ntn_versioned_position_plan           fallback = make_plan(4, 1, 1280);
  fallback.valid_until                           = at_ms(3200);
  fallback.content_hash                          = compute_ntn_position_plan_content_hash(fallback);
  ASSERT_TRUE(controller.submit(fallback, at_ms(640)).accepted);
  const std::string fallback_hash = controller.pending_plan()->calendar_hash;
  ASSERT_TRUE(controller.mark_deployment_preparing(1, fallback_hash));
  ASSERT_TRUE(controller.mark_deployment_applied(1, fallback_hash));
  ASSERT_TRUE(controller.advance_time(at_ms(1280)));

  ASSERT_TRUE(controller.submit(make_plan(6, 2, 3200, 2), at_ms(1920)).accepted);
  const std::string pending_hash = controller.pending_plan()->calendar_hash;
  ASSERT_TRUE(controller.mark_deployment_preparing(2, pending_hash));
  ASSERT_TRUE(controller.mark_deployment_applied(2, pending_hash));
  ASSERT_TRUE(controller.require_du_reconciliation_after_connection_loss({}));
  ASSERT_TRUE(controller.confirm_recovery_applied(2, pending_hash, at_ms(2000)));

  const auto expired = controller.take_expired_recovery_fallback(at_ms(3200));
  ASSERT_TRUE(expired.has_value());
  EXPECT_EQ(expired->source.schedule_version, 1U);
  EXPECT_EQ(expired->calendar_hash, fallback_hash);
  ASSERT_TRUE(controller.advance_time(at_ms(3200)));
  ASSERT_TRUE(controller.active_plan().has_value());
  EXPECT_EQ(controller.active_plan()->source.schedule_version, 2U);
  EXPECT_EQ(controller.active_plan()->calendar_hash, pending_hash);
  EXPECT_FALSE(controller.pending_plan().has_value());
  EXPECT_FALSE(controller.recovery_fallback_plan().has_value());
  EXPECT_TRUE(controller.active_has_external_apply_evidence());
  EXPECT_EQ(controller.highest_schedule_version_seen(), 2U);
  EXPECT_EQ(controller.candidate_inventory().size(), 6U);
}

TEST(ntn_onboard_position_plan, expired_early_applied_update_falls_back_to_live_du_reconciliation)
{
  ntn_onboard_position_plan_config config = make_config();
  config.require_external_apply           = true;
  ntn_onboard_position_plan_controller controller(config);

  ASSERT_TRUE(controller.submit(make_plan(4, 1, 1280), at_ms(640)).accepted);
  const std::string active_hash = controller.pending_plan()->calendar_hash;
  ASSERT_TRUE(controller.mark_deployment_preparing(1, active_hash));
  ASSERT_TRUE(controller.mark_deployment_applied(1, active_hash));
  ASSERT_TRUE(controller.advance_time(at_ms(1280)));

  ntn_versioned_position_plan update = make_plan(6, 2, 3200, 2);
  update.valid_until                 = at_ms(3300);
  update.content_hash                = compute_ntn_position_plan_content_hash(update);
  ASSERT_TRUE(controller.submit(update, at_ms(1920)).accepted);
  const std::string pending_hash = controller.pending_plan()->calendar_hash;
  ASSERT_TRUE(controller.mark_deployment_preparing(2, pending_hash));
  ASSERT_TRUE(controller.mark_deployment_applied(2, pending_hash));
  ASSERT_TRUE(controller.require_du_reconciliation_after_connection_loss({}));
  ASSERT_TRUE(controller.confirm_recovery_applied(2, pending_hash, at_ms(2000)));

  EXPECT_FALSE(controller.advance_time(at_ms(3400)));
  EXPECT_FALSE(controller.active_plan().has_value());
  EXPECT_FALSE(controller.pending_plan().has_value());
  ASSERT_TRUE(controller.recovery_plan().has_value());
  EXPECT_EQ(controller.recovery_plan()->source.schedule_version, 1U);
  EXPECT_FALSE(controller.recovery_fallback_plan().has_value());
  EXPECT_EQ(controller.recovery_stage(), ntn_position_plan_recovery_stage::reconciling);
  EXPECT_EQ(controller.deployment_stage(), ntn_position_plan_deployment_stage::preparing);
  EXPECT_EQ(controller.last_rejection_reason(), ntn_position_plan_reject_reason::expired);
  EXPECT_EQ(controller.last_rejected_schedule_version(), 2U);
  EXPECT_EQ(controller.highest_schedule_version_seen(), 2U);
  EXPECT_EQ(controller.candidate_inventory().size(), 6U);

  const ntn_onboard_position_plan_persistent_state fallback_state = controller.make_persistent_state(5);
  ASSERT_TRUE(fallback_state.active.has_value());
  EXPECT_EQ(fallback_state.active->source.schedule_version, 1U);
  EXPECT_FALSE(fallback_state.pending.has_value());
  EXPECT_EQ(fallback_state.recorded_deployment_schedule_version, 1U);
  EXPECT_EQ(fallback_state.recorded_deployment_calendar_hash, active_hash);

  ntn_onboard_position_plan_controller restarted(config);
  ASSERT_TRUE(restarted.restore_persistent_state(fallback_state, at_ms(3400)).has_value());
  ASSERT_TRUE(restarted.recovery_plan().has_value());
  EXPECT_EQ(restarted.recovery_plan()->source.schedule_version, 1U);
  EXPECT_EQ(restarted.recovery_stage(), ntn_position_plan_recovery_stage::reconciling);
  EXPECT_EQ(restarted.highest_schedule_version_seen(), 2U);
  EXPECT_EQ(restarted.candidate_inventory().size(), 6U);
  EXPECT_EQ(restarted.last_received_schedule_version(), 2U);
}

TEST(ntn_onboard_position_plan, replacement_failure_reconciles_historical_plan_hidden_by_early_applied_update)
{
  ntn_onboard_position_plan_config config = make_config();
  config.require_external_apply           = true;
  ntn_onboard_position_plan_controller controller(config);

  ASSERT_TRUE(controller.submit(make_plan(4, 1, 1280), at_ms(640)).accepted);
  const std::string active_hash = controller.pending_plan()->calendar_hash;
  ASSERT_TRUE(controller.mark_deployment_preparing(1, active_hash));
  ASSERT_TRUE(controller.mark_deployment_applied(1, active_hash));
  ASSERT_TRUE(controller.advance_time(at_ms(1280)));

  ASSERT_TRUE(controller.submit(make_plan(6, 2, 3200, 2), at_ms(1920)).accepted);
  const std::string early_hash = controller.pending_plan()->calendar_hash;
  ASSERT_TRUE(controller.mark_deployment_preparing(2, early_hash));
  ASSERT_TRUE(controller.mark_deployment_applied(2, early_hash));
  ASSERT_TRUE(controller.require_du_reconciliation_after_connection_loss({}));
  ASSERT_TRUE(controller.confirm_recovery_applied(2, early_hash, at_ms(2000)));
  ASSERT_TRUE(controller.recovery_fallback_plan().has_value());
  EXPECT_EQ(controller.recovery_fallback_plan()->source.schedule_version, 1U);

  ASSERT_TRUE(controller.submit(make_plan(8, 3, 3840, 3), at_ms(2100)).accepted);
  ASSERT_TRUE(controller.pending_plan().has_value());
  const std::string replacement_hash = controller.pending_plan()->calendar_hash;
  EXPECT_EQ(controller.pending_plan()->source.schedule_version, 3U);
  EXPECT_EQ(controller.candidate_inventory().size(), 8U);

  ASSERT_TRUE(controller.reject_pending_deployment(3,
                                                    replacement_hash,
                                                    ntn_position_plan_reject_reason::du_prepare_rejected,
                                                    "replacement_prepare_failed"));
  EXPECT_FALSE(controller.active_plan().has_value());
  EXPECT_FALSE(controller.pending_plan().has_value());
  ASSERT_TRUE(controller.recovery_plan().has_value());
  EXPECT_EQ(controller.recovery_plan()->source.schedule_version, 1U);
  EXPECT_EQ(controller.recovery_stage(), ntn_position_plan_recovery_stage::reconciling);
  EXPECT_EQ(controller.deployment_stage(), ntn_position_plan_deployment_stage::preparing);
  EXPECT_EQ(controller.last_rejection_reason(), ntn_position_plan_reject_reason::du_prepare_rejected);
  EXPECT_EQ(controller.last_rejected_schedule_version(), 3U);
  EXPECT_EQ(controller.highest_schedule_version_seen(), 3U);
  EXPECT_EQ(controller.candidate_inventory().size(), 8U);

  const ntn_onboard_position_plan_persistent_state fallback_state = controller.make_persistent_state(6);
  ASSERT_TRUE(fallback_state.active.has_value());
  EXPECT_EQ(fallback_state.active->source.schedule_version, 1U);
  EXPECT_FALSE(fallback_state.pending.has_value());
  ASSERT_TRUE(fallback_state.received_plan.has_value());
  EXPECT_EQ(fallback_state.received_plan->schedule_version, 3U);
  EXPECT_EQ(fallback_state.received_plan->candidate_inventory.size(), 8U);
  EXPECT_EQ(fallback_state.recorded_deployment_stage, ntn_position_plan_deployment_stage::preparing);
  EXPECT_EQ(fallback_state.recorded_deployment_schedule_version, 1U);
  EXPECT_EQ(fallback_state.recorded_deployment_calendar_hash, active_hash);

  ntn_onboard_position_plan_controller restarted(config);
  ASSERT_TRUE(restarted.restore_persistent_state(fallback_state, at_ms(2200)).has_value());
  ASSERT_TRUE(restarted.recovery_plan().has_value());
  EXPECT_EQ(restarted.recovery_plan()->source.schedule_version, 1U);
  EXPECT_EQ(restarted.highest_schedule_version_seen(), 3U);
  EXPECT_EQ(restarted.candidate_inventory().size(), 8U);
  ASSERT_TRUE(restarted.confirm_recovery_applied(1, active_hash, at_ms(2200)));
  ASSERT_TRUE(restarted.active_plan().has_value());
  EXPECT_EQ(restarted.active_plan()->source.schedule_version, 1U);
  EXPECT_TRUE(restarted.active_has_external_apply_evidence());
  EXPECT_FALSE(restarted.recovery_plan().has_value());
}

TEST(ntn_onboard_position_plan, deployment_rejection_removes_only_matching_pending_and_preserves_old_active)
{
  ntn_onboard_position_plan_config config = make_config();
  config.require_external_apply           = true;
  ntn_onboard_position_plan_controller controller(config);

  ASSERT_TRUE(controller.submit(make_plan(4, 1, 1280), at_ms(640)).accepted);
  ASSERT_TRUE(controller.pending_plan().has_value());
  const std::string first_hash = controller.pending_plan()->calendar_hash;
  ASSERT_TRUE(controller.mark_deployment_preparing(1, first_hash));
  ASSERT_TRUE(controller.mark_deployment_applied(1, first_hash));
  ASSERT_TRUE(controller.advance_time(at_ms(1280)));

  ASSERT_TRUE(controller.submit(make_plan(6, 2, 3200), at_ms(1920)).accepted);
  ASSERT_TRUE(controller.pending_plan().has_value());
  const std::string second_hash = controller.pending_plan()->calendar_hash;
  EXPECT_FALSE(controller.reject_pending_deployment(
      1, first_hash, ntn_position_plan_reject_reason::du_prepare_rejected, "stale_response"));
  EXPECT_TRUE(controller.reject_pending_deployment(
      2,
      second_hash,
      ntn_position_plan_reject_reason::static_opportunity_mismatch,
      "static_prach_opportunity_missing"));

  ASSERT_TRUE(controller.active_plan().has_value());
  EXPECT_EQ(controller.active_plan()->source.schedule_version, 1U);
  EXPECT_FALSE(controller.pending_plan().has_value());
  EXPECT_TRUE(controller.active_has_external_apply_evidence());
  EXPECT_EQ(controller.deployment_stage(), ntn_position_plan_deployment_stage::rejected);
  EXPECT_EQ(controller.last_rejection_reason(), ntn_position_plan_reject_reason::static_opportunity_mismatch);
  EXPECT_EQ(controller.deployment_detail(), "static_prach_opportunity_missing");
}

TEST(ntn_onboard_position_plan, deployment_feedback_is_monotonic_and_idempotent)
{
  ntn_onboard_position_plan_config config = make_config();
  config.require_external_apply           = true;
  ntn_onboard_position_plan_controller controller(config);

  ASSERT_TRUE(controller.submit(make_plan(4, 1, 1920), at_ms(1280)).accepted);
  ASSERT_TRUE(controller.pending_plan().has_value());
  const std::string calendar_hash = controller.pending_plan()->calendar_hash;

  EXPECT_FALSE(controller.mark_deployment_ready(1, calendar_hash));
  EXPECT_FALSE(controller.mark_deployment_applied(1, calendar_hash));
  EXPECT_TRUE(controller.mark_deployment_retryable(1, calendar_hash, "du_not_connected"));
  EXPECT_TRUE(controller.mark_deployment_preparing(1, calendar_hash));
  EXPECT_TRUE(controller.mark_deployment_preparing(1, calendar_hash));
  EXPECT_TRUE(controller.mark_deployment_ready(1, calendar_hash));
  EXPECT_TRUE(controller.mark_deployment_ready(1, calendar_hash));
  EXPECT_TRUE(controller.mark_deployment_retryable(1, calendar_hash, "late_retry"));
  EXPECT_EQ(controller.deployment_stage(), ntn_position_plan_deployment_stage::ready);
  EXPECT_TRUE(controller.mark_deployment_applied(1, calendar_hash));
  EXPECT_TRUE(controller.mark_deployment_applied(1, calendar_hash));
  EXPECT_TRUE(controller.mark_deployment_ready(1, calendar_hash));
  EXPECT_TRUE(controller.mark_deployment_preparing(1, calendar_hash));
  EXPECT_EQ(controller.deployment_stage(), ntn_position_plan_deployment_stage::applied);
  EXPECT_EQ(controller.deployment_detail(), "ssb_prach_software_gate_applied_no_position_or_rf_evidence");
}

TEST(ntn_onboard_position_plan, matching_initial_access_event_is_accepted_against_active_software_gate_snapshot)
{
  ntn_onboard_position_plan_config config = make_config();
  config.require_external_apply           = true;
  ntn_onboard_position_plan_controller controller(config);

  ASSERT_TRUE(controller.submit(make_plan(8, 1, 1920), at_ms(1280)).accepted);
  ASSERT_TRUE(controller.pending_plan().has_value());
  const std::string calendar_hash = controller.pending_plan()->calendar_hash;
  ASSERT_TRUE(controller.mark_deployment_preparing(1, calendar_hash));
  ASSERT_TRUE(controller.mark_deployment_applied(1, calendar_hash));
  ASSERT_TRUE(controller.advance_time(at_ms(1920)));
  ASSERT_TRUE(controller.active_plan().has_value());

  const ntn_initial_access_plan_event event = make_initial_access_event(*controller.active_plan());
  ASSERT_FALSE(event.position_id.empty());
  const ntn_initial_access_plan_audit audit = controller.audit_initial_access_event(event);

  EXPECT_EQ(audit.decision, ntn_initial_access_plan_decision::accept);
  EXPECT_EQ(audit.reason, ntn_initial_access_plan_reason::none);
  EXPECT_EQ(audit.active_schedule_version, 1U);
  EXPECT_EQ(audit.active_calendar_hash, calendar_hash);
  EXPECT_EQ(audit.evidence, "cu_cp_active_plan_event_match_software_gate_snapshot_no_rf_evidence");

  const auto ro_it = std::find_if(controller.active_plan()->access_calendar.begin(),
                                  controller.active_plan()->access_calendar.end(),
                                  [&](const auto& intent) {
                                    return intent.position_id == event.position_id &&
                                           intent.purpose == ntn_access_calendar_purpose::prach_ro;
                                  });
  ASSERT_NE(ro_it, controller.active_plan()->access_calendar.end());
  ntn_initial_access_plan_event exact_start = event;
  exact_start.occasion_time                 = controller.active_plan()->source.activation_epoch + ro_it->start_time;
  EXPECT_EQ(controller.audit_initial_access_event(exact_start).decision, ntn_initial_access_plan_decision::accept);

  ntn_initial_access_plan_event next_cycle = event;
  next_cycle.occasion_time += config.max_prach_interval;
  EXPECT_EQ(controller.audit_initial_access_event(next_cycle).decision, ntn_initial_access_plan_decision::accept);

  ntn_initial_access_plan_event exact_end = event;
  exact_end.occasion_time = controller.active_plan()->source.activation_epoch + ro_it->start_time + ro_it->duration;
  EXPECT_EQ(controller.audit_initial_access_event(exact_end).reason,
            ntn_initial_access_plan_reason::prach_occasion_not_scheduled);
}

TEST(ntn_onboard_position_plan, initial_access_event_rejects_wrong_version_owner_window_and_port)
{
  ntn_onboard_position_plan_config config = make_config();
  config.require_external_apply           = true;
  ntn_onboard_position_plan_controller controller(config);

  ASSERT_TRUE(controller.submit(make_plan(8, 1, 1920), at_ms(1280)).accepted);
  const std::string calendar_hash = controller.pending_plan()->calendar_hash;
  ASSERT_TRUE(controller.mark_deployment_preparing(1, calendar_hash));
  ASSERT_TRUE(controller.mark_deployment_applied(1, calendar_hash));
  ASSERT_TRUE(controller.advance_time(at_ms(1920)));
  ASSERT_TRUE(controller.active_plan().has_value());
  const ntn_initial_access_plan_event valid_event = make_initial_access_event(*controller.active_plan());
  ASSERT_FALSE(valid_event.position_id.empty());

  ntn_initial_access_plan_event wrong_satellite = valid_event;
  wrong_satellite.satellite_id                  = "P01-S02";
  EXPECT_EQ(controller.audit_initial_access_event(wrong_satellite).reason,
            ntn_initial_access_plan_reason::satellite_mismatch);

  ntn_initial_access_plan_event wrong_catalog = valid_event;
  ++wrong_catalog.catalog_version;
  EXPECT_EQ(controller.audit_initial_access_event(wrong_catalog).reason,
            ntn_initial_access_plan_reason::catalog_version_mismatch);

  ntn_initial_access_plan_event wrong_version = valid_event;
  ++wrong_version.schedule_version;
  EXPECT_EQ(controller.audit_initial_access_event(wrong_version).reason,
            ntn_initial_access_plan_reason::schedule_version_mismatch);

  ntn_initial_access_plan_event wrong_source_hash = valid_event;
  wrong_source_hash.source_content_hash            = "sha256:00";
  EXPECT_EQ(controller.audit_initial_access_event(wrong_source_hash).reason,
            ntn_initial_access_plan_reason::source_hash_mismatch);

  ntn_initial_access_plan_event wrong_calendar_hash = valid_event;
  wrong_calendar_hash.calendar_hash                  = "sha256:00";
  EXPECT_EQ(controller.audit_initial_access_event(wrong_calendar_hash).reason,
            ntn_initial_access_plan_reason::calendar_hash_mismatch);

  ntn_initial_access_plan_event unknown_cell = valid_event;
  unknown_cell.cell                           = make_cell(0x123450003ULL, 303);
  EXPECT_EQ(controller.audit_initial_access_event(unknown_cell).reason,
            ntn_initial_access_plan_reason::cell_identity_mismatch);

  ntn_initial_access_plan_event wrong_owner    = valid_event;
  const auto&                   first_identity = controller.active_plan()->cell_positions[0].identity;
  wrong_owner.cell = first_identity.nci == valid_event.cell.nci && first_identity.pci == valid_event.cell.pci
                         ? controller.active_plan()->cell_positions[1].identity
                         : first_identity;
  EXPECT_EQ(controller.audit_initial_access_event(wrong_owner).reason,
            ntn_initial_access_plan_reason::position_not_assigned_to_cell);

  ntn_initial_access_plan_event outside_window = valid_event;
  outside_window.occasion_time += std::chrono::milliseconds{10};
  EXPECT_EQ(controller.audit_initial_access_event(outside_window).reason,
            ntn_initial_access_plan_reason::prach_occasion_not_scheduled);

  ntn_initial_access_plan_event wrong_port = valid_event;
  ++wrong_port.ul_beam_port_id;
  EXPECT_EQ(controller.audit_initial_access_event(wrong_port).reason,
            ntn_initial_access_plan_reason::resource_port_mismatch);

  ntn_initial_access_plan_event expired = valid_event;
  expired.occasion_time                 = controller.active_plan()->source.valid_until;
  EXPECT_EQ(controller.audit_initial_access_event(expired).reason,
            ntn_initial_access_plan_reason::active_plan_not_valid);
}

TEST(ntn_onboard_position_plan, pending_or_intent_only_plan_never_claims_runtime_initial_access_evidence)
{
  ntn_onboard_position_plan_config external_config = make_config();
  external_config.require_external_apply           = true;
  ntn_onboard_position_plan_controller pending_controller(external_config);
  ASSERT_TRUE(pending_controller.submit(make_plan(4), at_ms(1280)).accepted);
  ASSERT_TRUE(pending_controller.pending_plan().has_value());
  const ntn_initial_access_plan_event pending_event = make_initial_access_event(*pending_controller.pending_plan());
  EXPECT_EQ(pending_controller.audit_initial_access_event(pending_event).reason,
            ntn_initial_access_plan_reason::no_active_plan);

  ntn_onboard_position_plan_controller intent_controller(make_config());
  ASSERT_TRUE(intent_controller.submit(make_plan(4), at_ms(1920)).accepted);
  ASSERT_TRUE(intent_controller.active_plan().has_value());
  const ntn_initial_access_plan_audit intent_audit =
      intent_controller.audit_initial_access_event(make_initial_access_event(*intent_controller.active_plan()));
  EXPECT_EQ(intent_audit.decision, ntn_initial_access_plan_decision::audit_only);
  EXPECT_EQ(intent_audit.reason, ntn_initial_access_plan_reason::intent_only_plan);
  EXPECT_EQ(intent_audit.evidence, "cu_cp_intent_calendar_event_match_no_du_or_rf_evidence");

  ntn_onboard_position_plan_controller disabled_controller(make_config(false));
  const ntn_initial_access_plan_audit  disabled_audit = disabled_controller.audit_initial_access_event({});
  EXPECT_EQ(disabled_audit.decision, ntn_initial_access_plan_decision::audit_only);
  EXPECT_EQ(disabled_audit.reason, ntn_initial_access_plan_reason::feature_disabled);
}

TEST(ntn_onboard_position_plan, initial_access_audit_switches_atomically_with_active_plan_version)
{
  ntn_onboard_position_plan_config config = make_config();
  config.require_external_apply           = true;
  ntn_onboard_position_plan_controller controller(config);

  ASSERT_TRUE(controller.submit(make_plan(4, 1, 1280, 1), at_ms(640)).accepted);
  std::string calendar_hash = controller.pending_plan()->calendar_hash;
  ASSERT_TRUE(controller.mark_deployment_preparing(1, calendar_hash));
  ASSERT_TRUE(controller.mark_deployment_applied(1, calendar_hash));
  ASSERT_TRUE(controller.advance_time(at_ms(1280)));
  const ntn_initial_access_plan_event version_one_event = make_initial_access_event(*controller.active_plan());
  EXPECT_EQ(controller.audit_initial_access_event(version_one_event).decision,
            ntn_initial_access_plan_decision::accept);

  ASSERT_TRUE(controller.submit(make_plan(4, 2, 3200, 1), at_ms(1920)).accepted);
  const ntn_initial_access_plan_event version_two_pending_event = make_initial_access_event(*controller.pending_plan());
  EXPECT_EQ(controller.audit_initial_access_event(version_two_pending_event).reason,
            ntn_initial_access_plan_reason::schedule_version_mismatch);
  EXPECT_EQ(controller.audit_initial_access_event(version_one_event).decision,
            ntn_initial_access_plan_decision::accept);

  calendar_hash = controller.pending_plan()->calendar_hash;
  ASSERT_TRUE(controller.mark_deployment_preparing(2, calendar_hash));
  ASSERT_TRUE(controller.mark_deployment_applied(2, calendar_hash));
  ASSERT_TRUE(controller.advance_time(at_ms(3200)));
  const ntn_initial_access_plan_event version_two_event = make_initial_access_event(*controller.active_plan());
  EXPECT_EQ(controller.audit_initial_access_event(version_two_event).decision,
            ntn_initial_access_plan_decision::accept);
  ntn_initial_access_plan_event version_one_after_switch = version_one_event;
  version_one_after_switch.occasion_time                 = version_two_event.occasion_time;
  EXPECT_EQ(controller.audit_initial_access_event(version_one_after_switch).reason,
            ntn_initial_access_plan_reason::schedule_version_mismatch);
}

TEST(ntn_onboard_position_plan, access_calendar_hash_is_canonical_and_changes_with_executable_content)
{
  ntn_onboard_position_plan_controller controller(make_config());
  ASSERT_TRUE(controller.submit(make_plan(8), at_ms(1280)).accepted);
  ASSERT_TRUE(controller.pending_plan().has_value());

  std::vector<ntn_access_calendar_intent> reordered = controller.pending_plan()->access_calendar;
  std::reverse(reordered.begin(), reordered.end());
  EXPECT_EQ(compute_ntn_access_calendar_hash(1, reordered), controller.pending_plan()->calendar_hash);

  reordered.front().start_time += std::chrono::microseconds{1};
  EXPECT_NE(compute_ntn_access_calendar_hash(1, reordered), controller.pending_plan()->calendar_hash);
}

TEST(ntn_onboard_position_plan, restart_prioritizes_recorded_inflight_pending_deployment_and_preserves_active_fallback)
{
  for (ntn_position_plan_deployment_stage recorded_stage : {ntn_position_plan_deployment_stage::preparing,
                                                            ntn_position_plan_deployment_stage::ready,
                                                            ntn_position_plan_deployment_stage::applied}) {
    SCOPED_TRACE(to_string(recorded_stage));
    ntn_onboard_position_plan_config config = make_config();
    config.require_external_apply           = true;
    ntn_onboard_position_plan_controller original(config);

    ASSERT_TRUE(original.submit(make_plan(4, 1, 1280), at_ms(640)).accepted);
    const std::string active_hash = original.pending_plan()->calendar_hash;
    ASSERT_TRUE(original.mark_deployment_preparing(1, active_hash));
    ASSERT_TRUE(original.mark_deployment_applied(1, active_hash));
    ASSERT_TRUE(original.advance_time(at_ms(1280)));

    ASSERT_TRUE(original.submit(make_plan(6, 2, 3200, 2), at_ms(1920)).accepted);
    const std::string pending_hash = original.pending_plan()->calendar_hash;
    ASSERT_TRUE(original.mark_deployment_preparing(2, pending_hash));
    if (recorded_stage == ntn_position_plan_deployment_stage::ready ||
        recorded_stage == ntn_position_plan_deployment_stage::applied) {
      ASSERT_TRUE(original.mark_deployment_ready(2, pending_hash));
    }
    if (recorded_stage == ntn_position_plan_deployment_stage::applied) {
      ASSERT_TRUE(original.mark_deployment_applied(2, pending_hash));
    }

    const ntn_onboard_position_plan_persistent_state crash_state = original.make_persistent_state(7);
    ASSERT_TRUE(crash_state.active.has_value());
    ASSERT_TRUE(crash_state.pending.has_value());
    EXPECT_EQ(crash_state.active->source.schedule_version, 1U);
    EXPECT_EQ(crash_state.pending->source.schedule_version, 2U);
    EXPECT_EQ(crash_state.recorded_deployment_stage, recorded_stage);
    EXPECT_EQ(crash_state.recorded_deployment_schedule_version, 2U);
    EXPECT_EQ(crash_state.recorded_deployment_calendar_hash, pending_hash);

    ntn_onboard_position_plan_controller restored(config);
    ASSERT_TRUE(restored.restore_persistent_state(crash_state, at_ms(2000)).has_value());
    EXPECT_FALSE(restored.active_plan().has_value());
    EXPECT_FALSE(restored.pending_plan().has_value());
    ASSERT_TRUE(restored.recovery_plan().has_value());
    EXPECT_EQ(restored.recovery_plan()->source.schedule_version, 2U);
    EXPECT_EQ(restored.deployment_stage(), ntn_position_plan_deployment_stage::preparing);
    EXPECT_EQ(restored.recovery_stage(), ntn_position_plan_recovery_stage::reconciling);
    EXPECT_EQ(restored.highest_schedule_version_seen(), 2U);

    const ntn_onboard_position_plan_persistent_state second_crash_state = restored.make_persistent_state(8);
    ASSERT_TRUE(second_crash_state.active.has_value());
    ASSERT_TRUE(second_crash_state.pending.has_value());
    EXPECT_EQ(second_crash_state.active->source.schedule_version, 1U);
    EXPECT_EQ(second_crash_state.pending->source.schedule_version, 2U);
    EXPECT_EQ(second_crash_state.recorded_deployment_stage, ntn_position_plan_deployment_stage::preparing);
    EXPECT_EQ(second_crash_state.recorded_deployment_schedule_version, 2U);
    EXPECT_EQ(second_crash_state.recorded_deployment_calendar_hash, pending_hash);

    ntn_onboard_position_plan_controller restarted_again(config);
    ASSERT_TRUE(restarted_again.restore_persistent_state(second_crash_state, at_ms(2100)).has_value());
    ASSERT_TRUE(restarted_again.recovery_plan().has_value());
    EXPECT_EQ(restarted_again.recovery_plan()->source.schedule_version, 2U);
    EXPECT_EQ(restarted_again.deployment_stage(), ntn_position_plan_deployment_stage::preparing);
    EXPECT_EQ(restarted_again.highest_schedule_version_seen(), 2U);
  }
}

TEST(ntn_onboard_position_plan, matching_applied_recovery_of_inflight_pending_plan_atomically_replaces_fallback)
{
  ntn_onboard_position_plan_config config = make_config();
  config.require_external_apply           = true;
  ntn_onboard_position_plan_controller original(config);

  ASSERT_TRUE(original.submit(make_plan(4, 1, 1280), at_ms(640)).accepted);
  const std::string active_hash = original.pending_plan()->calendar_hash;
  ASSERT_TRUE(original.mark_deployment_preparing(1, active_hash));
  ASSERT_TRUE(original.mark_deployment_applied(1, active_hash));
  ASSERT_TRUE(original.advance_time(at_ms(1280)));

  ASSERT_TRUE(original.submit(make_plan(6, 2, 1920, 2), at_ms(1280)).accepted);
  const std::string pending_hash = original.pending_plan()->calendar_hash;
  ASSERT_TRUE(original.mark_deployment_preparing(2, pending_hash));
  ASSERT_TRUE(original.mark_deployment_applied(2, pending_hash));

  ntn_onboard_position_plan_controller restored(config);
  ASSERT_TRUE(restored.restore_persistent_state(original.make_persistent_state(9), at_ms(2000)).has_value());
  ASSERT_TRUE(restored.recovery_plan().has_value());
  EXPECT_EQ(restored.recovery_plan()->source.schedule_version, 2U);

  ASSERT_TRUE(restored.confirm_recovery_applied(2, pending_hash, at_ms(2000)));
  ASSERT_TRUE(restored.active_plan().has_value());
  EXPECT_EQ(restored.active_plan()->source.schedule_version, 2U);
  EXPECT_TRUE(restored.active_has_external_apply_evidence());
  EXPECT_FALSE(restored.pending_plan().has_value());
  EXPECT_FALSE(restored.recovery_plan().has_value());
  EXPECT_EQ(restored.deployment_stage(), ntn_position_plan_deployment_stage::applied);
  EXPECT_EQ(restored.recovery_stage(), ntn_position_plan_recovery_stage::reconciled);
  EXPECT_EQ(restored.highest_schedule_version_seen(), 2U);

  const ntn_onboard_position_plan_persistent_state recovered_state = restored.make_persistent_state(10);
  ASSERT_TRUE(recovered_state.active.has_value());
  EXPECT_EQ(recovered_state.active->source.schedule_version, 2U);
  EXPECT_FALSE(recovered_state.pending.has_value());
  EXPECT_EQ(recovered_state.recorded_deployment_schedule_version, 2U);
  EXPECT_EQ(recovered_state.recorded_deployment_calendar_hash, pending_hash);
}

TEST(ntn_onboard_position_plan, failed_inflight_pending_recovery_queries_active_fallback_without_version_regression)
{
  ntn_onboard_position_plan_config config = make_config();
  config.require_external_apply           = true;
  ntn_onboard_position_plan_controller original(config);

  ASSERT_TRUE(original.submit(make_plan(4, 1, 1280), at_ms(640)).accepted);
  const std::string active_hash = original.pending_plan()->calendar_hash;
  ASSERT_TRUE(original.mark_deployment_preparing(1, active_hash));
  ASSERT_TRUE(original.mark_deployment_applied(1, active_hash));
  ASSERT_TRUE(original.advance_time(at_ms(1280)));
  ASSERT_TRUE(original.submit(make_plan(6, 2, 3200, 2), at_ms(1920)).accepted);
  const std::string pending_hash = original.pending_plan()->calendar_hash;
  ASSERT_TRUE(original.mark_deployment_preparing(2, pending_hash));
  ASSERT_TRUE(original.mark_deployment_ready(2, pending_hash));

  ntn_onboard_position_plan_controller restored(config);
  ASSERT_TRUE(restored.restore_persistent_state(original.make_persistent_state(11), at_ms(2000)).has_value());
  ASSERT_TRUE(restored.recovery_plan().has_value());
  EXPECT_EQ(restored.recovery_plan()->source.schedule_version, 2U);

  ASSERT_TRUE(restored.reject_recovery(
      2, pending_hash, ntn_position_plan_reject_reason::du_reconciliation_failed, "pending_du_state_rejected"));
  EXPECT_FALSE(restored.active_plan().has_value());
  EXPECT_FALSE(restored.pending_plan().has_value());
  ASSERT_TRUE(restored.recovery_plan().has_value());
  EXPECT_EQ(restored.recovery_plan()->source.schedule_version, 1U);
  EXPECT_EQ(restored.deployment_stage(), ntn_position_plan_deployment_stage::preparing);
  EXPECT_EQ(restored.deployment_detail(), "historical_active_fallback_requires_du_reconciliation");
  EXPECT_EQ(restored.recovery_stage(), ntn_position_plan_recovery_stage::reconciling);
  EXPECT_EQ(restored.recovery_schedule_version(), 1U);
  EXPECT_EQ(restored.last_rejection_reason(), ntn_position_plan_reject_reason::du_reconciliation_failed);
  EXPECT_EQ(restored.last_rejected_schedule_version(), 2U);
  EXPECT_EQ(restored.highest_schedule_version_seen(), 2U);

  const ntn_onboard_position_plan_persistent_state fallback_crash_state = restored.make_persistent_state(12);
  ASSERT_TRUE(fallback_crash_state.active.has_value());
  EXPECT_EQ(fallback_crash_state.active->source.schedule_version, 1U);
  EXPECT_FALSE(fallback_crash_state.pending.has_value());
  EXPECT_EQ(fallback_crash_state.recorded_deployment_stage, ntn_position_plan_deployment_stage::preparing);
  EXPECT_EQ(fallback_crash_state.recorded_deployment_schedule_version, 1U);
  EXPECT_EQ(fallback_crash_state.recorded_deployment_calendar_hash, active_hash);
  EXPECT_EQ(fallback_crash_state.highest_schedule_version, 2U);

  ntn_onboard_position_plan_controller restarted_again(config);
  ASSERT_TRUE(restarted_again.restore_persistent_state(fallback_crash_state, at_ms(2100)).has_value());
  ASSERT_TRUE(restarted_again.recovery_plan().has_value());
  EXPECT_EQ(restarted_again.recovery_plan()->source.schedule_version, 1U);
  EXPECT_EQ(restarted_again.highest_schedule_version_seen(), 2U);
  ASSERT_TRUE(restarted_again.confirm_recovery_applied(1, active_hash, at_ms(2100)));
  ASSERT_TRUE(restarted_again.active_plan().has_value());
  EXPECT_EQ(restarted_again.active_plan()->source.schedule_version, 1U);
  EXPECT_TRUE(restarted_again.active_has_external_apply_evidence());
  EXPECT_EQ(restarted_again.highest_schedule_version_seen(), 2U);
}

TEST(ntn_onboard_position_plan, restart_restore_keeps_high_water_but_requires_fresh_du_applied_evidence)
{
  ntn_onboard_position_plan_config config = make_config();
  config.require_external_apply           = true;
  ntn_onboard_position_plan_controller original(config);

  ASSERT_TRUE(original.submit(make_plan(8, 1, 1280), at_ms(640)).accepted);
  const std::string active_hash = original.pending_plan()->calendar_hash;
  ASSERT_TRUE(original.mark_deployment_preparing(1, active_hash));
  ASSERT_TRUE(original.mark_deployment_applied(1, active_hash));
  ASSERT_TRUE(original.advance_time(at_ms(1280)));
  ASSERT_TRUE(original.submit(make_plan(10, 2, 3200, 2), at_ms(1920)).accepted);

  const ntn_onboard_position_plan_persistent_state state = original.make_persistent_state(7);
  ntn_onboard_position_plan_controller             restored(config);
  ASSERT_TRUE(restored.restore_persistent_state(state, at_ms(2000)).has_value());

  EXPECT_FALSE(restored.active_plan().has_value());
  EXPECT_FALSE(restored.active_has_external_apply_evidence());
  ASSERT_TRUE(restored.recovery_plan().has_value());
  EXPECT_EQ(restored.recovery_plan()->source.schedule_version, 1U);
  EXPECT_EQ(restored.deployment_stage(), ntn_position_plan_deployment_stage::preparing);
  EXPECT_EQ(restored.recovery_stage(), ntn_position_plan_recovery_stage::reconciling);
  EXPECT_EQ(restored.highest_catalog_version_seen(), 2U);
  EXPECT_EQ(restored.highest_schedule_version_seen(), 2U);

  EXPECT_FALSE(restored.confirm_recovery_applied(1, "sha256:stale", at_ms(2000)));
  EXPECT_TRUE(restored.confirm_recovery_applied(1, active_hash, at_ms(2000)));
  ASSERT_TRUE(restored.active_plan().has_value());
  EXPECT_EQ(restored.active_plan()->source.schedule_version, 1U);
  EXPECT_TRUE(restored.active_has_external_apply_evidence());
  ASSERT_TRUE(restored.pending_plan().has_value());
  EXPECT_EQ(restored.pending_plan()->source.schedule_version, 2U);
  EXPECT_EQ(restored.deployment_stage(), ntn_position_plan_deployment_stage::not_sent);
  EXPECT_EQ(restored.recovery_stage(), ntn_position_plan_recovery_stage::reconciled);
  EXPECT_EQ(restored.recovery_schedule_version(), 1U);

  const auto replay = restored.submit(make_plan(10, 2, 3200, 2), at_ms(2100));
  EXPECT_FALSE(replay.accepted);
  EXPECT_EQ(replay.reason, ntn_position_plan_reject_reason::non_monotonic_version);
  ASSERT_TRUE(restored.active_plan().has_value());
  const auto active_owners = make_owner_map(*restored.active_plan());

  ASSERT_TRUE(restored.submit(make_plan(9, 3, 3840, 3), at_ms(2100)).accepted);
  ASSERT_TRUE(restored.pending_plan().has_value());
  const auto updated_owners = make_owner_map(*restored.pending_plan());
  for (const auto& [position_id, owner] : active_owners) {
    EXPECT_EQ(updated_owners.at(position_id), owner) << position_id;
  }
}

TEST(ntn_onboard_position_plan, failed_restart_reconciliation_preserves_deferred_plan_and_version_high_water)
{
  ntn_onboard_position_plan_config config = make_config();
  config.require_external_apply           = true;
  ntn_onboard_position_plan_controller original(config);

  ASSERT_TRUE(original.submit(make_plan(4, 1, 1280), at_ms(640)).accepted);
  const std::string active_hash = original.pending_plan()->calendar_hash;
  ASSERT_TRUE(original.mark_deployment_preparing(1, active_hash));
  ASSERT_TRUE(original.mark_deployment_applied(1, active_hash));
  ASSERT_TRUE(original.advance_time(at_ms(1280)));
  ASSERT_TRUE(original.submit(make_plan(6, 2, 3200, 2), at_ms(1920)).accepted);

  ntn_onboard_position_plan_controller restored(config);
  ASSERT_TRUE(restored.restore_persistent_state(original.make_persistent_state(3), at_ms(2000)).has_value());
  ASSERT_TRUE(restored.recovery_plan().has_value());
  EXPECT_TRUE(restored.reject_recovery(
      1, active_hash, ntn_position_plan_reject_reason::du_reconciliation_failed, "du_state_not_applied"));

  EXPECT_FALSE(restored.active_plan().has_value());
  EXPECT_FALSE(restored.active_has_external_apply_evidence());
  ASSERT_TRUE(restored.pending_plan().has_value());
  EXPECT_EQ(restored.pending_plan()->source.schedule_version, 2U);
  EXPECT_EQ(restored.highest_schedule_version_seen(), 2U);
  EXPECT_EQ(restored.recovery_stage(), ntn_position_plan_recovery_stage::failed);
  EXPECT_EQ(restored.last_rejection_reason(), ntn_position_plan_reject_reason::du_reconciliation_failed);
}

TEST(ntn_onboard_position_plan, restore_rejects_context_drift_without_installing_persisted_active_state)
{
  ntn_onboard_position_plan_config config = make_config();
  config.require_external_apply           = true;
  ntn_onboard_position_plan_controller original(config);
  ASSERT_TRUE(original.submit(make_plan(4, 1, 1280), at_ms(640)).accepted);

  ntn_onboard_position_plan_persistent_state state = original.make_persistent_state(1);
  state.planning_context.identity_registry_hash =
      "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  ntn_onboard_position_plan_controller restored(config);
  auto                                 result = restored.restore_persistent_state(state, at_ms(640));
  ASSERT_FALSE(result.has_value());
  EXPECT_FALSE(restored.active_plan().has_value());
  EXPECT_FALSE(restored.pending_plan().has_value());
  EXPECT_FALSE(restored.recovery_plan().has_value());
}

TEST(ntn_onboard_position_plan, disabled_profile_has_no_active_or_pending_plan)
{
  ntn_onboard_position_plan_controller controller(make_config(false));
  const auto result = controller.submit(make_plan(4), at_ms(1280));

  EXPECT_FALSE(result.accepted);
  EXPECT_EQ(result.stage, ntn_position_plan_stage::disabled);
  EXPECT_EQ(result.reason, ntn_position_plan_reject_reason::feature_disabled);
  EXPECT_FALSE(controller.active_plan().has_value());
  EXPECT_FALSE(controller.pending_plan().has_value());
}
