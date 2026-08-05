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

#include "lib/cu_cp/ntn_mobility/ntn_plan_version_anchor.h"
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

std::filesystem::path make_anchor_path(const char* label)
{
  static std::atomic<uint64_t> suffix{0};
  return std::filesystem::temp_directory_path() /
         fmt::format("srsran-ntn-anchor-{}-{}.json", label, suffix.fetch_add(1, std::memory_order_relaxed));
}

class temporary_anchor_guard
{
public:
  explicit temporary_anchor_guard(std::filesystem::path path_) : path(std::move(path_)) {}

  ~temporary_anchor_guard()
  {
    std::error_code error;
    std::filesystem::remove(path, error);
    const std::filesystem::path parent = path.parent_path();
    const std::string           prefix = fmt::format(".{}.tmp-", path.filename().string());
    for (const auto& entry : std::filesystem::directory_iterator(parent, error)) {
      if (entry.path().filename().string().rfind(prefix, 0) == 0) {
        std::filesystem::remove(entry.path(), error);
      }
    }
  }

private:
  std::filesystem::path path;
};

void write_file(const std::filesystem::path& path, const std::string& contents)
{
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(output.is_open());
  output << contents;
  ASSERT_TRUE(output.good());
}

std::string read_file(const std::filesystem::path& path)
{
  std::ifstream      input(path, std::ios::binary);
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

std::string make_digest(char digit)
{
  return "sha256:" + std::string(64, digit);
}

ntn_plan_version_identity make_identity(uint64_t catalog_version,
                                        uint64_t schedule_version,
                                        char     digest_digit = 'a')
{
  return {catalog_version, schedule_version, make_digest(digest_digit), "ntn-planning-key-2026-01"};
}

ntn_plan_version_anchor_state make_anchor_state()
{
  ntn_plan_version_anchor_state state;
  state.generation       = 1;
  state.satellite_id     = "P01-S01";
  state.planning_context = {"global-land-l1-v1",
                            make_digest('b'),
                            "mc-ntn-onboard-cell-registry-v1",
                            make_digest('c'),
                            "ntn-access-16a-64d-v1",
                            make_digest('d')};
  state.onboard_cells    = {{{first_nci, 101}, {second_nci, 101}}};
  return state;
}

ntn_plan_version_anchor_state store_and_reload(const std::filesystem::path&       path,
                                               const ntn_plan_version_anchor_state& state)
{
  auto stored = store_ntn_plan_version_anchor_atomic(path.string(), state);
  EXPECT_TRUE(stored.has_value()) << stored.error();
  if (!stored.has_value()) {
    return {};
  }

  auto loaded = load_ntn_plan_version_anchor(path.string());
  EXPECT_TRUE(loaded.has_value()) << loaded.error();
  EXPECT_TRUE(loaded.has_value() && loaded->has_value());
  return loaded.has_value() && loaded->has_value() ? std::move(**loaded) : ntn_plan_version_anchor_state{};
}

} // namespace

TEST(ntn_plan_version_anchor, missing_file_is_an_empty_first_boot_anchor)
{
  const std::filesystem::path path = make_anchor_path("missing");
  temporary_anchor_guard      guard(path);

  auto loaded = load_ntn_plan_version_anchor(path.string());

  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  EXPECT_FALSE(loaded->has_value());
}

TEST(ntn_plan_version_anchor, first_reservation_round_trips_without_becoming_committed)
{
  const std::filesystem::path path = make_anchor_path("first-reservation");
  temporary_anchor_guard      guard(path);
  ntn_plan_version_anchor_state state    = make_anchor_state();
  const ntn_plan_version_identity identity = make_identity(10, 20);

  auto reserved = reserve_ntn_plan_version(state, identity, 7);
  ASSERT_TRUE(reserved.has_value()) << reserved.error();
  EXPECT_EQ(*reserved, ntn_plan_version_reserve_outcome::reserved);
  EXPECT_EQ(state.generation, 2U);

  const ntn_plan_version_anchor_state reloaded = store_and_reload(path, state);
  EXPECT_EQ(reloaded.generation, state.generation);
  EXPECT_FALSE(reloaded.committed.has_value());
  ASSERT_TRUE(reloaded.reserved.has_value());
  EXPECT_TRUE(ntn_plan_version_identity_equal(reloaded.reserved->identity, identity));
  EXPECT_EQ(reloaded.reserved->target_state_generation, 7U);
}

TEST(ntn_plan_version_anchor, reserve_commit_and_reload_preserve_the_exact_plan_identity)
{
  const std::filesystem::path path = make_anchor_path("commit");
  temporary_anchor_guard      guard(path);
  ntn_plan_version_anchor_state state    = make_anchor_state();
  const ntn_plan_version_identity identity = make_identity(11, 21);

  ASSERT_EQ(reserve_ntn_plan_version(state, identity, 8).value(),
            ntn_plan_version_reserve_outcome::reserved);
  auto committed = commit_ntn_plan_version(state, identity, 8);
  ASSERT_TRUE(committed.has_value()) << committed.error();
  EXPECT_EQ(state.generation, 3U);
  ASSERT_TRUE(state.committed.has_value());
  EXPECT_TRUE(ntn_plan_version_identity_equal(*state.committed, identity));
  EXPECT_FALSE(state.reserved.has_value());

  auto repeated_commit = commit_ntn_plan_version(state, identity, 999);
  EXPECT_TRUE(repeated_commit.has_value()) << repeated_commit.error();
  EXPECT_EQ(state.generation, 3U);

  const ntn_plan_version_anchor_state reloaded = store_and_reload(path, state);
  ASSERT_TRUE(reloaded.committed.has_value());
  EXPECT_TRUE(ntn_plan_version_identity_equal(*reloaded.committed, identity));
  EXPECT_FALSE(reloaded.reserved.has_value());
}

TEST(ntn_plan_version_anchor, cancel_requires_the_exact_reservation_and_survives_reload)
{
  const std::filesystem::path path = make_anchor_path("cancel");
  temporary_anchor_guard      guard(path);
  ntn_plan_version_anchor_state state    = make_anchor_state();
  const ntn_plan_version_identity identity = make_identity(12, 22);
  ASSERT_TRUE(reserve_ntn_plan_version(state, identity, 9).has_value());

  auto wrong_identity = cancel_ntn_plan_version_reservation(state, make_identity(12, 23), 9);
  EXPECT_FALSE(wrong_identity.has_value());
  EXPECT_TRUE(state.reserved.has_value());
  auto wrong_generation = cancel_ntn_plan_version_reservation(state, identity, 10);
  EXPECT_FALSE(wrong_generation.has_value());
  EXPECT_TRUE(state.reserved.has_value());

  auto cancelled = cancel_ntn_plan_version_reservation(state, identity, 9);
  ASSERT_TRUE(cancelled.has_value()) << cancelled.error();
  EXPECT_FALSE(state.reserved.has_value());
  EXPECT_FALSE(state.committed.has_value());
  EXPECT_EQ(state.generation, 3U);

  const ntn_plan_version_anchor_state reloaded = store_and_reload(path, state);
  EXPECT_FALSE(reloaded.reserved.has_value());
  EXPECT_FALSE(reloaded.committed.has_value());
}

TEST(ntn_plan_version_anchor, reload_retains_the_context_needed_for_exact_cu_cp_matching)
{
  const std::filesystem::path path = make_anchor_path("context");
  temporary_anchor_guard      guard(path);
  const ntn_plan_version_anchor_state state    = make_anchor_state();
  const ntn_plan_version_anchor_state reloaded = store_and_reload(path, state);

  EXPECT_EQ(reloaded.satellite_id, "P01-S01");
  EXPECT_EQ(reloaded.planning_context.catalog_id, state.planning_context.catalog_id);
  EXPECT_EQ(reloaded.planning_context.catalog_hash, state.planning_context.catalog_hash);
  EXPECT_EQ(reloaded.planning_context.identity_registry_version,
            state.planning_context.identity_registry_version);
  EXPECT_EQ(reloaded.planning_context.identity_registry_hash, state.planning_context.identity_registry_hash);
  EXPECT_EQ(reloaded.planning_context.access_profile_id, state.planning_context.access_profile_id);
  EXPECT_EQ(reloaded.planning_context.access_profile_hash, state.planning_context.access_profile_hash);
  EXPECT_EQ(reloaded.onboard_cells[0].nci, first_nci);
  EXPECT_EQ(reloaded.onboard_cells[1].nci, second_nci);

  // The file contains the full binding, so CU-CP can reject a valid anchor copied from another planning context.
  EXPECT_NE(reloaded.satellite_id, "P02-S01");
  EXPECT_NE(reloaded.planning_context.catalog_id, "different-catalog");
}

TEST(ntn_plan_version_anchor, damaged_or_oversized_files_are_rejected)
{
  const std::filesystem::path path = make_anchor_path("damaged");
  temporary_anchor_guard      guard(path);

  ntn_plan_version_anchor_state state = make_anchor_state();
  ASSERT_TRUE(store_ntn_plan_version_anchor_atomic(path.string(), state).has_value());
  nlohmann::json document = nlohmann::json::parse(read_file(path));
  document["satellite_id"] = "P02-S01";
  write_file(path, document.dump(2));

  auto tampered = load_ntn_plan_version_anchor(path.string());
  ASSERT_FALSE(tampered.has_value());
  EXPECT_NE(tampered.error().find("hash mismatch"), std::string::npos);

  write_file(path, "{not-json");
  auto corrupt = load_ntn_plan_version_anchor(path.string());
  ASSERT_FALSE(corrupt.has_value());
  EXPECT_NE(corrupt.error().find("invalid anchor JSON"), std::string::npos);

  write_file(path, std::string(max_ntn_plan_version_anchor_file_size + 1U, 'x'));
  auto oversized = load_ntn_plan_version_anchor(path.string());
  ASSERT_FALSE(oversized.has_value());
  EXPECT_NE(oversized.error().find("input_too_large"), std::string::npos);
}

TEST(ntn_plan_version_anchor, replay_and_conflicting_reservations_do_not_mutate_committed_state)
{
  ntn_plan_version_anchor_state state = make_anchor_state();
  const ntn_plan_version_identity committed_identity = make_identity(20, 30, 'e');
  ASSERT_TRUE(reserve_ntn_plan_version(state, committed_identity, 10).has_value());
  ASSERT_TRUE(commit_ntn_plan_version(state, committed_identity, 10).has_value());
  const uint64_t committed_generation = state.generation;

  auto same = reserve_ntn_plan_version(state, committed_identity, 11);
  ASSERT_TRUE(same.has_value()) << same.error();
  EXPECT_EQ(*same, ntn_plan_version_reserve_outcome::already_committed);
  EXPECT_EQ(state.generation, committed_generation);

  auto same_version_different_hash = reserve_ntn_plan_version(state, make_identity(20, 30, 'f'), 11);
  EXPECT_FALSE(same_version_different_hash.has_value());
  EXPECT_EQ(same_version_different_hash.error(), "version_replay");
  auto lower_schedule = reserve_ntn_plan_version(state, make_identity(21, 29, 'f'), 11);
  EXPECT_FALSE(lower_schedule.has_value());
  EXPECT_EQ(lower_schedule.error(), "version_replay");
  auto lower_catalog = reserve_ntn_plan_version(state, make_identity(19, 31, 'f'), 11);
  EXPECT_FALSE(lower_catalog.has_value());
  EXPECT_EQ(lower_catalog.error(), "version_replay");
  EXPECT_EQ(state.generation, committed_generation);
  ASSERT_TRUE(state.committed.has_value());
  EXPECT_TRUE(ntn_plan_version_identity_equal(*state.committed, committed_identity));

  const ntn_plan_version_identity next_identity = make_identity(21, 31, 'f');
  ASSERT_EQ(reserve_ntn_plan_version(state, next_identity, 11).value(),
            ntn_plan_version_reserve_outcome::reserved);
  const uint64_t reserved_generation = state.generation;
  auto conflicting = reserve_ntn_plan_version(state, make_identity(22, 32, '1'), 12);
  EXPECT_FALSE(conflicting.has_value());
  EXPECT_NE(conflicting.error().find("another reservation"), std::string::npos);
  EXPECT_EQ(state.generation, reserved_generation);
  ASSERT_TRUE(state.reserved.has_value());
  EXPECT_TRUE(ntn_plan_version_identity_equal(state.reserved->identity, next_identity));
}

TEST(ntn_plan_version_anchor, status_snapshot_identifies_software_only_protection)
{
  const ntn_plan_version_identity identity = make_identity(23, 33, '2');

  const ntn_position_plan_version_anchor_snapshot snapshot = make_ntn_plan_version_anchor_snapshot(identity);

  EXPECT_EQ(snapshot.mode, "software_only");
  EXPECT_EQ(snapshot.catalog_version, identity.catalog_version);
  EXPECT_EQ(snapshot.schedule_version, identity.schedule_version);
  EXPECT_EQ(snapshot.content_hash, identity.content_hash);
}
