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
#include "fmt/format.h"
#include "gtest/gtest.h"
#include "nlohmann/json.hpp"
#include <algorithm>
#include <map>
#include <set>

using namespace srsran;
using namespace srsran::srs_cu_cp;

namespace {

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
  config.satellite_id     = "P01-S001";
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
  plan.satellite_id     = "P01-S001";
  plan.catalog_version  = catalog_version;
  plan.schedule_version = schedule_version;
  plan.valid_from       = at_ms(640);
  plan.valid_until      = at_ms(64000);
  plan.activation_epoch = at_ms(activation_ms);
  plan.onboard_cells    = make_config().onboard_cells;
  for (unsigned i = 0; i != count; ++i) {
    plan.visible_l1_positions.push_back(
        {fmt::format("G{:06}", i + 1), 10.0 + static_cast<double>(i / 32) * 0.05,
         20.0 + static_cast<double>(i % 32) * 0.05});
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
  nlohmann::json root;
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
                                              {"longitude_deg", position.longitude_deg}});
  }

  auto parsed = parse_ntn_position_plan_json(root.dump());
  ASSERT_TRUE(parsed.has_value()) << parsed.error();
  EXPECT_EQ(parsed.value().onboard_cells[0].nci, plan.onboard_cells[0].nci);
  EXPECT_EQ(parsed.value().onboard_cells[1].pci, plan.onboard_cells[1].pci);
  EXPECT_EQ(compute_ntn_position_plan_content_hash(parsed.value()), plan.content_hash);

  ntn_onboard_position_plan_controller controller(make_config());
  EXPECT_TRUE(controller.submit(parsed.value(), at_ms(1280)).accepted);
}

TEST(ntn_onboard_position_plan, web_exporter_and_cpp_share_the_same_canonical_hash_golden_vector)
{
  ntn_versioned_position_plan plan;
  plan.satellite_id      = "P01-S001";
  plan.catalog_version   = 10;
  plan.schedule_version  = 20;
  plan.valid_from        = at_ms(1780000000000LL);
  plan.valid_until       = at_ms(1780003600000LL);
  plan.activation_epoch  = at_ms(1780000000640LL);
  plan.onboard_cells     = {make_cell(4886691841ULL, 101), make_cell(4886691842ULL, 202)};
  plan.visible_l1_positions = {{"G000002", 56.7654, -158.36066}, {"G000001", 10.0, 20.0}};

  EXPECT_EQ(compute_ntn_position_plan_content_hash(plan),
            "sha256:9fc109aa6a2ec0029667603257afaf05f2dcac46e8fbb228121b5fa1bd27c720");
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

TEST(ntn_onboard_position_plan, rejects_satellite_hash_version_validity_activation_and_l1_errors)
{
  struct test_case {
    const char*                     name;
    ntn_position_plan_reject_reason expected;
    void (*mutate)(ntn_versioned_position_plan&);
    bool recompute_hash;
  };
  const std::vector<test_case> cases{
      {"satellite", ntn_position_plan_reject_reason::invalid_satellite_id,
       [](auto& plan) { plan.satellite_id = "P99-S999"; }, true},
      {"hash", ntn_position_plan_reject_reason::invalid_hash,
       [](auto& plan) { plan.visible_l1_positions.front().latitude_deg += 1.0; }, false},
      {"zero_version", ntn_position_plan_reject_reason::non_monotonic_version,
       [](auto& plan) { plan.schedule_version = 0; }, true},
      {"expired", ntn_position_plan_reject_reason::expired,
       [](auto& plan) {
         plan.valid_from       = at_ms(0);
         plan.activation_epoch = at_ms(640);
         plan.valid_until      = at_ms(1280);
       }, true},
      {"validity", ntn_position_plan_reject_reason::invalid_validity_window,
       [](auto& plan) {
         plan.valid_from  = at_ms(3200);
         plan.valid_until = at_ms(2560);
       }, true},
      {"activation", ntn_position_plan_reject_reason::invalid_activation_epoch,
       [](auto& plan) { plan.activation_epoch = at_ms(2000); }, true},
      {"l1_format", ntn_position_plan_reject_reason::invalid_l1_id,
       [](auto& plan) { plan.visible_l1_positions.front().position_id = "L000001"; }, true},
      {"l1_duplicate", ntn_position_plan_reject_reason::duplicate_l1_id,
       [](auto& plan) { plan.visible_l1_positions[1].position_id = plan.visible_l1_positions[0].position_id; }, true}};

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
  ntn_versioned_position_plan old_version = make_plan(4, 2, 2560, 1);
  const auto version_result = version_controller.submit(old_version, at_ms(1920));
  EXPECT_FALSE(version_result.accepted);
  EXPECT_EQ(version_result.reason, ntn_position_plan_reject_reason::non_monotonic_version);
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

TEST(ntn_onboard_position_plan, deployment_rejection_removes_only_matching_pending_and_preserves_old_active)
{
  ntn_onboard_position_plan_config config = make_config();
  config.require_external_apply           = true;
  ntn_onboard_position_plan_controller controller(config);

  ASSERT_TRUE(controller.submit(make_plan(4, 1, 1280), at_ms(640)).accepted);
  ASSERT_TRUE(controller.pending_plan().has_value());
  const std::string first_hash = controller.pending_plan()->calendar_hash;
  ASSERT_TRUE(controller.mark_deployment_applied(1, first_hash));
  ASSERT_TRUE(controller.advance_time(at_ms(1280)));

  ASSERT_TRUE(controller.submit(make_plan(6, 2, 3200), at_ms(1920)).accepted);
  ASSERT_TRUE(controller.pending_plan().has_value());
  const std::string second_hash = controller.pending_plan()->calendar_hash;
  EXPECT_FALSE(controller.reject_pending_deployment(
      1, first_hash, ntn_position_plan_reject_reason::du_prepare_rejected, "stale_response"));
  EXPECT_TRUE(controller.reject_pending_deployment(
      2, second_hash, ntn_position_plan_reject_reason::du_prepare_rejected, "static_prach_misaligned"));

  ASSERT_TRUE(controller.active_plan().has_value());
  EXPECT_EQ(controller.active_plan()->source.schedule_version, 1U);
  EXPECT_FALSE(controller.pending_plan().has_value());
  EXPECT_TRUE(controller.active_has_external_apply_evidence());
  EXPECT_EQ(controller.deployment_stage(), ntn_position_plan_deployment_stage::rejected);
  EXPECT_EQ(controller.deployment_detail(), "static_prach_misaligned");
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
