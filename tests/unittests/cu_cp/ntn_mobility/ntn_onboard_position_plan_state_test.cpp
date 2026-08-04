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
 * the LICENSE file in the top-level directory of this distribution.
 *
 */

#include "lib/cu_cp/ntn_mobility/ntn_onboard_position_plan_state.h"
#include "nlohmann/json.hpp"
#include "fmt/format.h"
#include <atomic>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <sstream>

using namespace srsran;
using namespace srsran::srs_cu_cp;

namespace {

const nr_cell_identity first_nci  = nr_cell_identity::create(0x123450001ULL).value();
const nr_cell_identity second_nci = nr_cell_identity::create(0x123450002ULL).value();

std::filesystem::path make_state_path(const char* label)
{
  static std::atomic<uint64_t> suffix{0};
  return std::filesystem::temp_directory_path() /
         fmt::format("srsran-ntn-state-{}-{}.json", label, suffix.fetch_add(1, std::memory_order_relaxed));
}

class temporary_state_guard
{
public:
  explicit temporary_state_guard(std::filesystem::path path_) : path(std::move(path_)) {}
  ~temporary_state_guard()
  {
    std::error_code error;
    std::filesystem::remove(path, error);
    const std::filesystem::path parent = path.parent_path();
    const std::string           prefix = fmt::format(".{}.tmp.", path.filename().string());
    for (const auto& entry : std::filesystem::directory_iterator(parent, error)) {
      if (entry.path().filename().string().rfind(prefix, 0) == 0) {
        std::filesystem::remove(entry.path(), error);
      }
    }
  }

private:
  std::filesystem::path path;
};

std::string read_file(const std::filesystem::path& path)
{
  std::ifstream      input(path, std::ios::binary);
  std::ostringstream text;
  text << input.rdbuf();
  return text.str();
}

void write_file(const std::filesystem::path& path, const std::string& text)
{
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << text;
}

void grow_state_file_after_size_check(const std::string& path)
{
  std::ofstream output(path, std::ios::binary | std::ios::app);
  output << ' ';
}

ntn_versioned_position_plan
make_plan(uint64_t catalog_version, uint64_t schedule_version, std::vector<ntn_l1_position> positions)
{
  ntn_versioned_position_plan plan;
  plan.schema_version            = 2;
  plan.planning_run_id           = fmt::format("planning-run-{}", schedule_version);
  plan.catalog_id                = "global-land-l1-v1";
  plan.catalog_hash              = "sha256:b39fe9c3ee9a9355b3546036b7f16e0fb858c953f8558cc4295122f2169fbe7a";
  plan.identity_registry_version = "mc-ntn-onboard-cell-registry-v1";
  plan.identity_registry_hash    = "sha256:7475821350e104b57a70d979d630f4b29a6cecb89ca0eca7b16dddf2ffee6a4a";
  plan.access_profile_id         = "ntn-access-16a-64d-v1";
  plan.access_profile_hash       = "sha256:195786f4161e3b0fad6faa0605144948a7401c067a014bde684c1b29a8087d63";
  plan.satellite_id              = "P01-S01";
  plan.catalog_version           = catalog_version;
  plan.schedule_version          = schedule_version;
  plan.valid_from                = std::chrono::system_clock::time_point{std::chrono::milliseconds{0}};
  plan.activation_epoch          = std::chrono::system_clock::time_point{std::chrono::milliseconds{1280}};
  plan.valid_until               = std::chrono::system_clock::time_point{std::chrono::milliseconds{6400}};
  plan.onboard_cells             = {{{first_nci, 101}, {second_nci, 101}}};
  plan.visible_l1_positions      = std::move(positions);
  plan.content_hash              = compute_ntn_position_plan_content_hash(plan);
  return plan;
}

ntn_versioned_position_plan make_schema_v3_plan(uint64_t                     catalog_version,
                                                uint64_t                     schedule_version,
                                                std::vector<ntn_l1_position> positions,
                                                std::vector<std::string>     assigned_ids)
{
  ntn_versioned_position_plan plan = make_plan(catalog_version, schedule_version, std::move(positions));
  plan.schema_version              = 3;
  plan.assigned_l1_position_ids    = std::move(assigned_ids);
  plan.content_hash                = compute_ntn_position_plan_content_hash(plan);
  return plan;
}

std::vector<ntn_l1_position> make_positions(unsigned count)
{
  std::vector<ntn_l1_position> positions;
  positions.reserve(count);
  for (unsigned i = 0; i != count; ++i) {
    positions.push_back(
        {fmt::format("G{:06}", i + 1), 10.0 + static_cast<double>(i / 32) * 0.01, 20.0 + (i % 32) * 0.01, 0x7f});
  }
  return positions;
}

ntn_onboard_position_plan_state_snapshot make_snapshot(ntn_versioned_position_plan plan)
{
  ntn_onboard_position_plan_state_snapshot snapshot;
  snapshot.source                     = std::move(plan);
  snapshot.cell_positions[0].identity = {first_nci, 101};
  snapshot.cell_positions[1].identity = {second_nci, 101};
  std::vector<std::string> assigned_ids = snapshot.source.assigned_l1_position_ids;
  if (snapshot.source.schema_version < 3) {
    assigned_ids.clear();
    assigned_ids.reserve(snapshot.source.visible_l1_positions.size());
    for (const ntn_l1_position& position : snapshot.source.visible_l1_positions) {
      assigned_ids.push_back(position.position_id);
    }
  }
  for (size_t i = 0; i != assigned_ids.size(); ++i) {
    snapshot.cell_positions[i % 2].assigned_l1_ids.push_back(assigned_ids[i]);
  }
  snapshot.calendar_hash = compute_ntn_access_calendar_hash(snapshot.source.schedule_version, {});
  return snapshot;
}

ntn_onboard_position_plan_persistent_state make_state()
{
  ntn_onboard_position_plan_persistent_state state;
  state.generation       = 7;
  state.satellite_id     = "P01-S01";
  state.planning_context = {"global-land-l1-v1",
                            "sha256:b39fe9c3ee9a9355b3546036b7f16e0fb858c953f8558cc4295122f2169fbe7a",
                            "mc-ntn-onboard-cell-registry-v1",
                            "sha256:7475821350e104b57a70d979d630f4b29a6cecb89ca0eca7b16dddf2ffee6a4a",
                            "ntn-access-16a-64d-v1",
                            "sha256:195786f4161e3b0fad6faa0605144948a7401c067a014bde684c1b29a8087d63"};
  state.onboard_cells    = {{{first_nci, 101}, {second_nci, 101}}};
  state.active  = make_snapshot(make_plan(10, 20, {{"G000001", 10.0, 20.0, 0x7f}, {"G000002", 10.1, 20.1, 0x7f}}));
  state.pending = make_snapshot(
      make_plan(11, 21, {{"G000001", 10.0, 20.0, 0x7f}, {"G000002", 10.1, 20.1, 0x7f}, {"G000003", 10.2, 20.2, 0x7f}}));
  std::vector<ntn_l1_position> received_positions;
  received_positions.reserve(257);
  for (unsigned i = 0; i != 257; ++i) {
    received_positions.push_back(
        {fmt::format("G{:06}", i + 1), 10.0 + static_cast<double>(i) * 0.001, 20.0, 0x7f});
  }
  const ntn_versioned_position_plan received = make_plan(12, 22, std::move(received_positions));
  state.received_plan = ntn_onboard_position_plan_received_observation{received.catalog_version,
                                                                        received.schedule_version,
                                                                        received.content_hash,
                                                                        received.activation_epoch,
                                                                        received.visible_l1_positions};
  state.highest_catalog_version  = 11;
  state.highest_schedule_version = 21;
  state.sticky_partition         = state.active->cell_positions;
  state.outstanding_clears.push_back(
      {make_snapshot(make_plan(9, 19, {{"G000004", 10.3, 20.3, 0x7f}})), "superseded_pending"});
  state.recorded_deployment_stage            = ntn_position_plan_deployment_stage::applied;
  state.recorded_deployment_detail           = "historical_software_gate_applied";
  state.recorded_deployment_schedule_version = state.active->source.schedule_version;
  state.recorded_deployment_calendar_hash    = state.active->calendar_hash;
  state.du_reconciliation_required           = true;
  return state;
}

} // namespace

TEST(ntn_onboard_position_plan_state, missing_file_is_a_valid_first_boot)
{
  const std::filesystem::path path = make_state_path("missing");
  temporary_state_guard       guard(path);

  auto loaded = load_ntn_onboard_position_plan_state(path.string());

  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  EXPECT_FALSE(loaded->has_value());
}

TEST(ntn_onboard_position_plan_state, atomic_store_round_trips_complete_recovery_context)
{
  const std::filesystem::path path = make_state_path("round-trip");
  temporary_state_guard       guard(path);
  const auto                  state = make_state();

  auto stored_hash = store_ntn_onboard_position_plan_state_atomic(path.string(), state);
  ASSERT_TRUE(stored_hash.has_value()) << stored_hash.error();
  ASSERT_TRUE(stored_hash->durable) << stored_hash->durability_error;
  ASSERT_TRUE(std::filesystem::exists(path));

  auto loaded = load_ntn_onboard_position_plan_state(path.string());
  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  ASSERT_TRUE(loaded->has_value());
  const auto& recovered = loaded->value();
  EXPECT_EQ(recovered.state_hash, stored_hash->state_hash);
  EXPECT_EQ(recovered.generation, state.generation);
  EXPECT_EQ(recovered.satellite_id, "P01-S01");
  EXPECT_EQ(recovered.highest_catalog_version, 11U);
  EXPECT_EQ(recovered.highest_schedule_version, 21U);
  ASSERT_TRUE(recovered.active.has_value());
  ASSERT_TRUE(recovered.pending.has_value());
  EXPECT_EQ(recovered.active->source.schedule_version, 20U);
  EXPECT_EQ(recovered.pending->source.schedule_version, 21U);
  EXPECT_EQ(recovered.pending->source.visible_l1_positions.size(), 3U);
  ASSERT_TRUE(recovered.received_plan.has_value());
  EXPECT_EQ(recovered.received_plan->catalog_version, 12U);
  EXPECT_EQ(recovered.received_plan->schedule_version, 22U);
  EXPECT_EQ(recovered.received_plan->candidate_inventory.size(), 257U);
  EXPECT_EQ(recovered.received_plan->candidate_inventory.back().position_id, "G000257");
  EXPECT_EQ(recovered.sticky_partition[0].identity.nci, state.sticky_partition[0].identity.nci);
  EXPECT_EQ(recovered.sticky_partition[0].identity.pci, state.sticky_partition[0].identity.pci);
  EXPECT_EQ(recovered.sticky_partition[0].assigned_l1_ids, state.sticky_partition[0].assigned_l1_ids);
  EXPECT_EQ(recovered.sticky_partition[1].identity.nci, state.sticky_partition[1].identity.nci);
  EXPECT_EQ(recovered.sticky_partition[1].identity.pci, state.sticky_partition[1].identity.pci);
  EXPECT_EQ(recovered.sticky_partition[1].assigned_l1_ids, state.sticky_partition[1].assigned_l1_ids);
  ASSERT_EQ(recovered.outstanding_clears.size(), 1U);
  EXPECT_EQ(recovered.outstanding_clears.front().snapshot.source.schedule_version, 19U);
  EXPECT_EQ(recovered.outstanding_clears.front().reason, "superseded_pending");
  EXPECT_EQ(recovered.recorded_deployment_stage, ntn_position_plan_deployment_stage::applied);
  EXPECT_EQ(recovered.recorded_deployment_schedule_version, 20U);
  EXPECT_TRUE(recovered.du_reconciliation_required);
}

TEST(ntn_onboard_position_plan_state, schema_v3_round_trips_complete_visibility_and_assigned_subset)
{
  const std::filesystem::path path = make_state_path("schema-v3-dual-set");
  temporary_state_guard       guard(path);
  auto                        state = make_state();

  std::vector<ntn_l1_position> positions = make_positions(300);
  std::vector<std::string>     assigned_ids;
  assigned_ids.reserve(60);
  for (unsigned i = 0; i != 60; ++i) {
    assigned_ids.push_back(positions[i * 4].position_id);
  }
  const ntn_versioned_position_plan plan =
      make_schema_v3_plan(10, 20, std::move(positions), std::move(assigned_ids));
  state.active           = make_snapshot(plan);
  state.pending.reset();
  state.sticky_partition = state.active->cell_positions;
  state.received_plan    = ntn_onboard_position_plan_received_observation{plan.catalog_version,
                                                                           plan.schedule_version,
                                                                           plan.content_hash,
                                                                           plan.activation_epoch,
                                                                           plan.visible_l1_positions,
                                                                           plan.assigned_l1_position_ids};
  state.recorded_deployment_schedule_version = plan.schedule_version;
  state.recorded_deployment_calendar_hash    = state.active->calendar_hash;

  auto stored = store_ntn_onboard_position_plan_state_atomic(path.string(), state);
  ASSERT_TRUE(stored.has_value()) << stored.error();
  auto loaded = load_ntn_onboard_position_plan_state(path.string());
  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  ASSERT_TRUE(loaded->has_value());
  const auto& recovered = loaded->value();

  EXPECT_EQ(recovered.schema_version, 3U);
  ASSERT_TRUE(recovered.active.has_value());
  EXPECT_EQ(recovered.active->source.visible_l1_positions.size(), 300U);
  EXPECT_EQ(recovered.active->source.assigned_l1_position_ids, plan.assigned_l1_position_ids);
  const size_t recovered_partition_size = recovered.active->cell_positions[0].assigned_l1_ids.size() +
                                          recovered.active->cell_positions[1].assigned_l1_ids.size();
  EXPECT_EQ(recovered_partition_size, plan.assigned_l1_position_ids.size());
  ASSERT_TRUE(recovered.received_plan.has_value());
  EXPECT_EQ(recovered.received_plan->candidate_inventory.size(), 300U);
  EXPECT_EQ(recovered.received_plan->assigned_l1_position_ids, plan.assigned_l1_position_ids);
}

TEST(ntn_onboard_position_plan_state, schema_v2_load_migrates_implicit_assignment_to_the_visible_inventory)
{
  const std::filesystem::path path = make_state_path("schema-v2-migration");
  temporary_state_guard       guard(path);
  auto                        state = make_state();
  state.schema_version              = 2;
  ASSERT_TRUE(state.received_plan.has_value());
  state.received_plan->assigned_l1_position_ids.clear();

  auto stored = store_ntn_onboard_position_plan_state_atomic(path.string(), state);
  ASSERT_TRUE(stored.has_value()) << stored.error();
  const nlohmann::json persisted = nlohmann::json::parse(read_file(path));
  EXPECT_EQ(persisted.at("state_schema_version"), 2U);
  EXPECT_FALSE(persisted.at("received_plan").contains("assigned_l1_position_ids"));

  auto loaded = load_ntn_onboard_position_plan_state(path.string());
  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  ASSERT_TRUE(loaded->has_value());
  const auto& recovered = loaded->value();
  EXPECT_EQ(recovered.schema_version, 2U);
  ASSERT_TRUE(recovered.active.has_value());
  ASSERT_TRUE(recovered.pending.has_value());
  EXPECT_EQ(recovered.active->source.assigned_l1_position_ids.size(),
            recovered.active->source.visible_l1_positions.size());
  EXPECT_EQ(recovered.pending->source.assigned_l1_position_ids.size(),
            recovered.pending->source.visible_l1_positions.size());
  ASSERT_TRUE(recovered.received_plan.has_value());
  EXPECT_EQ(recovered.received_plan->assigned_l1_position_ids.size(),
            recovered.received_plan->candidate_inventory.size());
  for (size_t i = 0; i != recovered.received_plan->candidate_inventory.size(); ++i) {
    EXPECT_EQ(recovered.received_plan->assigned_l1_position_ids[i],
              recovered.received_plan->candidate_inventory[i].position_id);
  }
}

TEST(ntn_onboard_position_plan_state, schema_v1_without_received_observation_remains_readable)
{
  const std::filesystem::path path = make_state_path("schema-v1");
  temporary_state_guard       guard(path);
  auto                        state = make_state();
  state.schema_version              = 1;
  state.received_plan.reset();

  auto stored = store_ntn_onboard_position_plan_state_atomic(path.string(), state);
  ASSERT_TRUE(stored.has_value()) << stored.error();
  auto loaded = load_ntn_onboard_position_plan_state(path.string());
  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  ASSERT_TRUE(loaded->has_value());
  EXPECT_EQ((*loaded)->schema_version, 1U);
  EXPECT_FALSE((*loaded)->received_plan.has_value());
  ASSERT_TRUE((*loaded)->active.has_value());
  EXPECT_EQ((*loaded)->active->source.schedule_version, 20U);
}

TEST(ntn_onboard_position_plan_state, received_observation_is_exact_keyed_and_integrity_protected)
{
  const std::filesystem::path path = make_state_path("received-observation");
  temporary_state_guard       guard(path);
  ASSERT_TRUE(store_ntn_onboard_position_plan_state_atomic(path.string(), make_state()).has_value());

  nlohmann::json root                       = nlohmann::json::parse(read_file(path));
  root["received_plan"]["unexpected"]       = true;
  write_file(path, root.dump());
  auto unknown = load_ntn_onboard_position_plan_state(path.string());
  ASSERT_FALSE(unknown.has_value());
  EXPECT_NE(unknown.error().find("unknown field 'received_plan.unexpected'"), std::string::npos);

  ASSERT_TRUE(store_ntn_onboard_position_plan_state_atomic(path.string(), make_state()).has_value());
  root = nlohmann::json::parse(read_file(path));
  root.erase("received_plan");
  write_file(path, root.dump());
  auto missing = load_ntn_onboard_position_plan_state(path.string());
  ASSERT_FALSE(missing.has_value());
  EXPECT_NE(missing.error().find("missing field 'root.received_plan'"), std::string::npos);

  ASSERT_TRUE(store_ntn_onboard_position_plan_state_atomic(path.string(), make_state()).has_value());
  root = nlohmann::json::parse(read_file(path));
  root["received_plan"]["candidate_inventory"][0]["position_id"] = "G999999";
  write_file(path, root.dump());
  auto tampered = load_ntn_onboard_position_plan_state(path.string());
  ASSERT_FALSE(tampered.has_value());
  EXPECT_EQ(tampered.error(), "state_hash_mismatch");
}

TEST(ntn_onboard_position_plan_state, rejects_snapshot_versions_above_the_persisted_high_water)
{
  const std::filesystem::path path = make_state_path("invalid-high-water");
  temporary_state_guard       guard(path);

  auto state                     = make_state();
  state.highest_schedule_version = state.pending->source.schedule_version - 1;
  auto schedule_rejected         = store_ntn_onboard_position_plan_state_atomic(path.string(), state);
  ASSERT_FALSE(schedule_rejected.has_value());
  EXPECT_EQ(schedule_rejected.error(), "pending_exceeds_version_high_water");

  state                         = make_state();
  state.highest_catalog_version = state.pending->source.catalog_version - 1;
  auto catalog_rejected         = store_ntn_onboard_position_plan_state_atomic(path.string(), state);
  ASSERT_FALSE(catalog_rejected.has_value());
  EXPECT_EQ(catalog_rejected.error(), "pending_exceeds_version_high_water");
}

TEST(ntn_onboard_position_plan_state, exact_root_rejects_unknown_fields_before_integrity_acceptance)
{
  const std::filesystem::path path = make_state_path("unknown-root");
  temporary_state_guard       guard(path);
  ASSERT_TRUE(store_ntn_onboard_position_plan_state_atomic(path.string(), make_state()).has_value());
  nlohmann::json root = nlohmann::json::parse(read_file(path));
  root["unexpected"]  = true;
  write_file(path, root.dump());

  auto loaded = load_ntn_onboard_position_plan_state(path.string());

  ASSERT_FALSE(loaded.has_value());
  EXPECT_NE(loaded.error().find("unknown field 'root.unexpected'"), std::string::npos);
}

TEST(ntn_onboard_position_plan_state, hash_tamper_and_corrupt_json_fail_closed)
{
  const std::filesystem::path path = make_state_path("tamper");
  temporary_state_guard       guard(path);
  ASSERT_TRUE(store_ntn_onboard_position_plan_state_atomic(path.string(), make_state()).has_value());
  nlohmann::json root = nlohmann::json::parse(read_file(path));
  root["generation"]  = root.at("generation").get<uint64_t>() + 1;
  write_file(path, root.dump());

  auto tampered = load_ntn_onboard_position_plan_state(path.string());
  ASSERT_FALSE(tampered.has_value());
  EXPECT_EQ(tampered.error(), "state_hash_mismatch");

  write_file(path, R"({"state_schema_version":1)");
  auto corrupt = load_ntn_onboard_position_plan_state(path.string());
  ASSERT_FALSE(corrupt.has_value());
  EXPECT_NE(corrupt.error().find("invalid NTN position-plan state JSON"), std::string::npos);
}

TEST(ntn_onboard_position_plan_state, bounded_file_loader_accepts_exact_limit_and_rejects_oversized_or_growing_files)
{
  const std::filesystem::path path = make_state_path("oversized");
  temporary_state_guard       guard(path);
  EXPECT_EQ(max_ntn_onboard_position_plan_state_file_size, 16U * 1024U * 1024U);

  ASSERT_TRUE(store_ntn_onboard_position_plan_state_atomic(path.string(), make_state()).has_value());
  std::string exact_limit = read_file(path);
  ASSERT_LT(exact_limit.size(), max_ntn_onboard_position_plan_state_file_size);
  exact_limit.resize(max_ntn_onboard_position_plan_state_file_size, ' ');
  write_file(path, exact_limit);

  auto loaded = load_ntn_onboard_position_plan_state(path.string());
  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  ASSERT_TRUE(loaded->has_value());
  EXPECT_EQ((*loaded)->generation, 7U);

  write_file(path, std::string(max_ntn_onboard_position_plan_state_file_size + 1, 'x'));

  loaded = load_ntn_onboard_position_plan_state(path.string());

  ASSERT_FALSE(loaded.has_value());
  EXPECT_NE(loaded.error().find("exceeds"), std::string::npos);

  ASSERT_TRUE(store_ntn_onboard_position_plan_state_atomic(path.string(), make_state()).has_value());
  set_ntn_onboard_position_plan_state_file_read_test_hook_once_for_test(&grow_state_file_after_size_check);
  loaded = load_ntn_onboard_position_plan_state(path.string());

  ASSERT_FALSE(loaded.has_value());
  EXPECT_EQ(loaded.error().find("input_too_large:"), 0U);
  EXPECT_NE(loaded.error().find("grew while being read"), std::string::npos);
}

TEST(ntn_onboard_position_plan_state, decoder_bounds_arrays_before_reserving_storage)
{
  const std::filesystem::path path = make_state_path("bounded-arrays");
  temporary_state_guard       guard(path);
  ASSERT_TRUE(store_ntn_onboard_position_plan_state_atomic(path.string(), make_state()).has_value());
  const nlohmann::json valid_root = nlohmann::json::parse(read_file(path));

  nlohmann::json too_many_cell_ids = valid_root;
  too_many_cell_ids["sticky_partition"][0]["assigned_l1_ids"] = nlohmann::json::array();
  for (size_t i = 0; i != max_ntn_onboard_position_plan_observed_positions + 1; ++i) {
    too_many_cell_ids["sticky_partition"][0]["assigned_l1_ids"].push_back(nullptr);
  }
  write_file(path, too_many_cell_ids.dump());

  auto loaded = load_ntn_onboard_position_plan_state(path.string());
  ASSERT_FALSE(loaded.has_value());
  EXPECT_NE(loaded.error().find("sticky_partition[0].assigned_l1_ids is too large"), std::string::npos);

  nlohmann::json too_many_clears = valid_root;
  too_many_clears["outstanding_clears"] = nlohmann::json::array();
  for (size_t i = 0; i != max_ntn_onboard_position_plan_cleanup_claims + 1; ++i) {
    too_many_clears["outstanding_clears"].push_back(nullptr);
  }
  write_file(path, too_many_clears.dump());

  loaded = load_ntn_onboard_position_plan_state(path.string());
  ASSERT_FALSE(loaded.has_value());
  EXPECT_EQ(loaded.error(), "outstanding_clears is too large");
}

TEST(ntn_onboard_position_plan_state, pre_rename_failure_preserves_previous_valid_state_and_cleans_temp)
{
  const std::filesystem::path path = make_state_path("preserve");
  temporary_state_guard       guard(path);
  auto                        state = make_state();
  ASSERT_TRUE(store_ntn_onboard_position_plan_state_atomic(path.string(), state).has_value());
  const std::string before = read_file(path);

  state.generation = 8;
  auto rejected    = store_ntn_onboard_position_plan_state_atomic(
      path.string(), state, ntn_onboard_position_plan_state_store_failpoint::before_rename);

  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(rejected.error(), "injected failure before state-file rename");
  EXPECT_EQ(read_file(path), before);
  const std::string prefix = fmt::format(".{}.tmp.", path.filename().string());
  std::error_code   error;
  for (const auto& entry : std::filesystem::directory_iterator(path.parent_path(), error)) {
    EXPECT_NE(entry.path().filename().string().rfind(prefix, 0), 0U);
  }
}

TEST(ntn_onboard_position_plan_state, post_rename_failure_reports_committed_bytes_without_rollback_semantics)
{
  const std::filesystem::path path = make_state_path("committed-not-durable");
  temporary_state_guard       guard(path);
  auto                        state = make_state();
  ASSERT_TRUE(store_ntn_onboard_position_plan_state_atomic(path.string(), state).has_value());

  state.generation = 8;
  auto committed   = store_ntn_onboard_position_plan_state_atomic(
      path.string(), state, ntn_onboard_position_plan_state_store_failpoint::after_rename);

  ASSERT_TRUE(committed.has_value()) << committed.error();
  EXPECT_FALSE(committed->durable);
  EXPECT_EQ(committed->durability_error, "injected failure after committed state-file rename");
  auto loaded = load_ntn_onboard_position_plan_state(path.string());
  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  ASSERT_TRUE(loaded->has_value());
  EXPECT_EQ((*loaded)->generation, 8U);
  EXPECT_EQ((*loaded)->state_hash, committed->state_hash);
}

TEST(ntn_onboard_position_plan_state, post_rename_exception_is_still_reported_as_a_committed_replacement)
{
  const std::filesystem::path path = make_state_path("committed-exception");
  temporary_state_guard       guard(path);
  auto                        state = make_state();
  ASSERT_TRUE(store_ntn_onboard_position_plan_state_atomic(path.string(), state).has_value());

  state.generation = 8;
  auto committed   = store_ntn_onboard_position_plan_state_atomic(
      path.string(), state, ntn_onboard_position_plan_state_store_failpoint::after_rename_exception);

  ASSERT_TRUE(committed.has_value()) << committed.error();
  EXPECT_FALSE(committed->durable);
  EXPECT_EQ(committed->durability_error,
            "state file replaced but completion failed: injected exception after committed state-file rename");
  auto loaded = load_ntn_onboard_position_plan_state(path.string());
  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  ASSERT_TRUE(loaded->has_value());
  EXPECT_EQ((*loaded)->generation, 8U);
  EXPECT_EQ((*loaded)->state_hash, committed->state_hash);
}

TEST(ntn_onboard_position_plan_state, persisted_applied_stage_cannot_disable_du_reconciliation)
{
  const std::filesystem::path path = make_state_path("reconcile");
  temporary_state_guard       guard(path);
  auto                        state = make_state();
  state.du_reconciliation_required  = false;

  auto stored = store_ntn_onboard_position_plan_state_atomic(path.string(), state);

  ASSERT_FALSE(stored.has_value());
  EXPECT_EQ(stored.error(), "du_reconciliation_must_be_required");
}

TEST(ntn_onboard_position_plan_state, outstanding_clear_cannot_target_a_live_snapshot)
{
  const std::filesystem::path path = make_state_path("clear-live-snapshot");
  temporary_state_guard       guard(path);
  auto                        state = make_state();
  ASSERT_TRUE(state.active.has_value());
  state.outstanding_clears = {{*state.active, "must_not_clear_live_active"}};

  auto stored = store_ntn_onboard_position_plan_state_atomic(path.string(), state);

  ASSERT_FALSE(stored.has_value());
  EXPECT_EQ(stored.error(), "outstanding_clear_targets_live_snapshot");
}

TEST(ntn_onboard_position_plan_state, cleanup_capacity_retains_sixty_four_historical_tasks_and_two_live_snapshots)
{
  const std::filesystem::path path = make_state_path("cleanup-capacity");
  temporary_state_guard       guard(path);
  auto                        state = make_state();
  state.highest_catalog_version  = 200;
  state.highest_schedule_version = 200;
  state.outstanding_clears.clear();
  for (unsigned i = 0; i != 64; ++i) {
    state.outstanding_clears.push_back(
        {make_snapshot(make_plan(100 + i,
                                 100 + i,
                                 {{fmt::format("G{:06}", 100 + i), 10.0, 20.0, 0x7f}})),
         "historical_cleanup"});
  }

  auto stored = store_ntn_onboard_position_plan_state_atomic(path.string(), state);

  ASSERT_TRUE(stored.has_value()) << stored.error();
  EXPECT_TRUE(stored->durable) << stored->durability_error;
}

TEST(ntn_onboard_position_plan_state, cleanup_capacity_rejects_more_than_sixty_six_total_claims)
{
  const std::filesystem::path path = make_state_path("cleanup-capacity-overflow");
  temporary_state_guard       guard(path);
  auto                        state = make_state();
  state.highest_catalog_version  = 200;
  state.highest_schedule_version = 200;
  state.outstanding_clears.clear();
  for (unsigned i = 0; i != 65; ++i) {
    state.outstanding_clears.push_back(
        {make_snapshot(make_plan(100 + i,
                                 100 + i,
                                 {{fmt::format("G{:06}", 100 + i), 10.0, 20.0, 0x7f}})),
         "historical_cleanup"});
  }

  auto stored = store_ntn_onboard_position_plan_state_atomic(path.string(), state);

  ASSERT_FALSE(stored.has_value());
  EXPECT_EQ(stored.error(), "too_many_cleanup_claims");
}
