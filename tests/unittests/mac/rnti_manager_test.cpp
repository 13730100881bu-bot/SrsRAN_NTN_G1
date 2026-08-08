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

#include "lib/mac/rnti_manager.h"
#include <chrono>
#include <gtest/gtest.h>

using namespace srsran;

TEST(rnti_manager_test, when_allocate_rnti_called_multiple_times_then_rntis_are_unique)
{
  rnti_manager rnti_db;

  unsigned         max_count = 100;
  std::set<rnti_t> prev_rntis;
  for (unsigned count = 0; count != max_count; ++count) {
    rnti_t rnti = rnti_db.allocate();
    ASSERT_EQ(prev_rntis.count(rnti), 0);
    prev_rntis.insert(rnti);
  }

  ASSERT_EQ(rnti_db.nof_ues(), 0) << "No UE should have been added";
}

TEST(rnti_manager_test, when_ue_added_then_allocate_rnti_does_not_repeat_rnti)
{
  rnti_manager rnti_db;

  rnti_t rnti1 = rnti_db.allocate();
  ASSERT_TRUE(rnti_db.add_ue(rnti1, to_du_ue_index(0)));

  rnti_t rnti2 = rnti_db.allocate();
  ASSERT_NE(rnti1, rnti2);

  ASSERT_EQ(rnti_db.nof_ues(), 1);
}

TEST(rnti_manager_test, when_ntn_lease_tracks_next_terrestrial_rnti_then_terrestrial_allocator_skips_it)
{
  rnti_manager          rnti_db;
  const du_cell_index_t cell = to_du_cell_index(0);

  ASSERT_TRUE(rnti_db.add_ntn_rnti_lease(cell, to_rnti(0x4601), 7, 30000));

  EXPECT_EQ(rnti_db.allocate(), to_rnti(0x4602));
  EXPECT_EQ(rnti_db.nof_ntn_rnti_leases(cell), 1U);
}

TEST(rnti_manager_test, when_terrestrial_rnti_is_outstanding_then_ntn_update_cannot_claim_it)
{
  rnti_manager          rnti_db;
  const du_cell_index_t cell             = to_du_cell_index(0);
  const auto            now              = rnti_manager::ntn_lease_time_point{};
  const rnti_t          terrestrial_rnti = rnti_db.allocate(now);
  ASSERT_EQ(terrestrial_rnti, to_rnti(0x4601));

  EXPECT_FALSE(rnti_db.add_ntn_rnti_lease(cell, terrestrial_rnti, 8, 30000, now));
  EXPECT_TRUE(rnti_db.add_ue(terrestrial_rnti, to_du_ue_index(0)));
  EXPECT_FALSE(rnti_db.add_ntn_rnti_lease(cell, terrestrial_rnti, 8, 30000, now));
}

TEST(rnti_manager_test, when_abandoned_terrestrial_rnti_guard_expires_then_ntn_update_can_claim_it)
{
  rnti_manager          rnti_db;
  const du_cell_index_t cell             = to_du_cell_index(0);
  const auto            now              = rnti_manager::ntn_lease_time_point{};
  const rnti_t          terrestrial_rnti = rnti_db.allocate(now);

  EXPECT_FALSE(rnti_db.add_ntn_rnti_lease(cell, terrestrial_rnti, 9, 30000, now));
  EXPECT_TRUE(rnti_db.add_ntn_rnti_lease(cell, terrestrial_rnti, 9, 30000, now + std::chrono::seconds{10}));
}

TEST(rnti_manager_test, when_ntn_lease_is_available_then_allocate_ntn_lease_returns_reserved_rnti)
{
  rnti_manager rnti_db;

  ASSERT_TRUE(rnti_db.add_ntn_rnti_lease(to_rnti(0x4701)));
  ASSERT_EQ(rnti_db.nof_ntn_rnti_leases(), 1U);

  ASSERT_EQ(rnti_db.allocate_ntn_lease(), to_rnti(0x4701));
  ASSERT_EQ(rnti_db.nof_ntn_rnti_leases(), 0U);
}

TEST(rnti_manager_test, when_ntn_lease_is_consumed_then_snapshot_retains_consumed_state)
{
  rnti_manager              rnti_db;
  const du_cell_index_t     cell          = to_du_cell_index(0);
  const auto                now           = rnti_manager::ntn_lease_time_point{} + std::chrono::seconds{10};
  static constexpr uint32_t generation_id = 17;

  ASSERT_TRUE(rnti_db.add_ntn_rnti_lease(cell, to_rnti(0x4701), generation_id, 5000, now));

  auto pending_snapshot = rnti_db.get_ntn_rnti_lease_pool_snapshot(cell, now);
  ASSERT_TRUE(pending_snapshot.complete);
  ASSERT_EQ(pending_snapshot.leases.size(), 1U);
  EXPECT_EQ(pending_snapshot.leases.front().generation_id, generation_id);
  EXPECT_EQ(pending_snapshot.leases.front().state, "pending");
  EXPECT_EQ(pending_snapshot.leases.front().distribution_state, "applied_by_du");

  ASSERT_EQ(rnti_db.allocate_ntn_lease(cell, now), to_rnti(0x4701));
  EXPECT_EQ(rnti_db.nof_ntn_rnti_leases(cell, now), 0U);

  auto consumed_snapshot = rnti_db.get_ntn_rnti_lease_pool_snapshot(cell, now);
  ASSERT_EQ(consumed_snapshot.leases.size(), 1U);
  EXPECT_EQ(consumed_snapshot.leases.front().state, "consumed_by_mac");
  EXPECT_EQ(consumed_snapshot.leases.front().distribution_state, "applied_by_du");
  EXPECT_FALSE(rnti_db.add_ntn_rnti_lease(cell, to_rnti(0x4701), generation_id + 1, 5000, now));

  auto expired_snapshot = rnti_db.get_ntn_rnti_lease_pool_snapshot(cell, now + std::chrono::milliseconds{5000});
  ASSERT_EQ(expired_snapshot.leases.size(), 1U);
  EXPECT_EQ(expired_snapshot.leases.front().state, "expired");
  EXPECT_EQ(expired_snapshot.leases.front().distribution_state, "expired_by_du");
}

TEST(rnti_manager_test, when_pending_ntn_lease_expires_then_it_is_not_allocated_and_snapshot_reports_expiry)
{
  rnti_manager          rnti_db;
  const du_cell_index_t cell = to_du_cell_index(0);
  const auto            now  = rnti_manager::ntn_lease_time_point{} + std::chrono::seconds{10};

  ASSERT_TRUE(rnti_db.add_ntn_rnti_lease(cell, to_rnti(0x4701), 23, 100, now));
  EXPECT_EQ(rnti_db.allocate_ntn_lease(cell, now + std::chrono::milliseconds{99}), to_rnti(0x4701));

  rnti_db.clear_ntn_rnti_leases(cell);
  ASSERT_TRUE(rnti_db.add_ntn_rnti_lease(cell, to_rnti(0x4702), 24, 100, now));
  EXPECT_EQ(rnti_db.allocate_ntn_lease(cell, now + std::chrono::milliseconds{100}), rnti_t::INVALID_RNTI);

  auto snapshot = rnti_db.get_ntn_rnti_lease_pool_snapshot(cell, now + std::chrono::milliseconds{100});
  ASSERT_EQ(snapshot.leases.size(), 1U);
  EXPECT_EQ(snapshot.leases.front().rnti, to_rnti(0x4702));
  EXPECT_EQ(snapshot.leases.front().state, "expired");
  EXPECT_EQ(snapshot.leases.front().distribution_state, "expired_by_du");
}

TEST(rnti_manager_test, when_requested_crnti_enters_du_table_then_snapshot_reports_lease_as_consumed)
{
  rnti_manager          rnti_db;
  const du_cell_index_t cell  = to_du_cell_index(0);
  const rnti_t          lease = to_rnti(0x4701);
  const auto            now   = rnti_manager::ntn_lease_time_point{};

  ASSERT_TRUE(rnti_db.add_ntn_rnti_lease(cell, lease, 25, 1000, now));
  ASSERT_TRUE(rnti_db.add_ue(lease, to_du_ue_index(0)));

  auto snapshot = rnti_db.get_ntn_rnti_lease_pool_snapshot(cell, now);
  ASSERT_EQ(snapshot.leases.size(), 1U);
  EXPECT_EQ(snapshot.leases.front().state, "consumed_by_mac");
  EXPECT_EQ(rnti_db.nof_ntn_rnti_leases(cell, now), 0U);
}

TEST(rnti_manager_test, when_ntn_pool_is_cleared_then_pending_and_terminal_records_are_removed)
{
  rnti_manager          rnti_db;
  const du_cell_index_t cell = to_du_cell_index(0);
  const auto            now  = rnti_manager::ntn_lease_time_point{};

  ASSERT_TRUE(rnti_db.add_ntn_rnti_lease(cell, to_rnti(0x4701), 30, 1000, now));
  ASSERT_TRUE(rnti_db.add_ntn_rnti_lease(cell, to_rnti(0x4702), 30, 1000, now));
  ASSERT_EQ(rnti_db.allocate_ntn_lease(cell, now), to_rnti(0x4701));
  ASSERT_EQ(rnti_db.get_ntn_rnti_lease_pool_snapshot(cell, now).leases.size(), 2U);

  rnti_db.clear_ntn_rnti_leases(cell);
  EXPECT_TRUE(rnti_db.get_ntn_rnti_lease_pool_snapshot(cell, now).leases.empty());

  ASSERT_TRUE(rnti_db.add_ntn_rnti_lease(cell, to_rnti(0x4701), 31, 1000, now));
  auto replacement_snapshot = rnti_db.get_ntn_rnti_lease_pool_snapshot(cell, now);
  ASSERT_EQ(replacement_snapshot.leases.size(), 1U);
  EXPECT_EQ(replacement_snapshot.leases.front().generation_id, 31U);
  EXPECT_EQ(replacement_snapshot.leases.front().state, "pending");
}

TEST(rnti_manager_test, when_atomic_replace_is_invalid_then_previous_ntn_ledger_is_preserved)
{
  rnti_manager          rnti_db;
  const du_cell_index_t cell = to_du_cell_index(0);
  const auto            now  = rnti_manager::ntn_lease_time_point{};

  mac_ntn_rnti_lease_pool_update initial;
  initial.cell_index    = cell;
  initial.operation     = mac_ntn_rnti_lease_pool_operation::replace;
  initial.generation_id = 51;
  initial.expiry_ms     = 1000;
  initial.leases        = {to_rnti(0x4701), to_rnti(0x4702)};
  ASSERT_TRUE(rnti_db.apply_ntn_rnti_lease_pool_update(initial, now).accepted);

  mac_ntn_rnti_lease_pool_update invalid      = initial;
  invalid.generation_id                       = 52;
  invalid.leases                              = {to_rnti(0x4801), to_rnti(0x4801)};
  const mac_ntn_rnti_lease_pool_result result = rnti_db.apply_ntn_rnti_lease_pool_update(invalid, now);

  EXPECT_FALSE(result.accepted);
  EXPECT_EQ(result.reason, "duplicate_lease");
  EXPECT_EQ(result.rejected_leases, invalid.leases);
  const mac_ntn_rnti_lease_pool_snapshot snapshot = rnti_db.get_ntn_rnti_lease_pool_snapshot(cell, now);
  ASSERT_EQ(snapshot.leases.size(), initial.leases.size());
  EXPECT_EQ(snapshot.leases[0].rnti, initial.leases[0]);
  EXPECT_EQ(snapshot.leases[0].generation_id, initial.generation_id);
  EXPECT_EQ(snapshot.leases[1].rnti, initial.leases[1]);
}

TEST(rnti_manager_test, when_pool_is_replaced_then_consumed_history_is_retained_and_not_resurrected)
{
  rnti_manager          rnti_db;
  const du_cell_index_t cell = to_du_cell_index(0);
  const auto            now  = rnti_manager::ntn_lease_time_point{};

  mac_ntn_rnti_lease_pool_update initial;
  initial.cell_index    = cell;
  initial.operation     = mac_ntn_rnti_lease_pool_operation::replace;
  initial.generation_id = 61;
  initial.expiry_ms     = 1000;
  initial.leases        = {to_rnti(0x4701), to_rnti(0x4702)};
  ASSERT_TRUE(rnti_db.apply_ntn_rnti_lease_pool_update(initial, now).accepted);
  ASSERT_EQ(rnti_db.allocate_ntn_lease(cell, now), initial.leases.front());

  mac_ntn_rnti_lease_pool_update replacement = initial;
  replacement.generation_id                  = 62;
  replacement.leases                         = {to_rnti(0x4801)};
  ASSERT_TRUE(rnti_db.apply_ntn_rnti_lease_pool_update(replacement, now).accepted);

  const mac_ntn_rnti_lease_pool_snapshot snapshot = rnti_db.get_ntn_rnti_lease_pool_snapshot(cell, now);
  ASSERT_EQ(snapshot.leases.size(), 2U);
  const auto consumed = std::find_if(
      snapshot.leases.begin(), snapshot.leases.end(), [](const auto& lease) { return lease.rnti == to_rnti(0x4701); });
  ASSERT_NE(consumed, snapshot.leases.end());
  EXPECT_EQ(consumed->state, "consumed_by_mac");
  const auto pending = std::find_if(
      snapshot.leases.begin(), snapshot.leases.end(), [](const auto& lease) { return lease.rnti == to_rnti(0x4801); });
  ASSERT_NE(pending, snapshot.leases.end());
  EXPECT_EQ(pending->generation_id, replacement.generation_id);
  EXPECT_EQ(pending->state, "pending");
}

TEST(rnti_manager_test, when_replace_arrives_at_expiry_boundary_then_expired_tombstone_is_retained)
{
  rnti_manager          rnti_db;
  const du_cell_index_t cell = to_du_cell_index(0);
  const auto            now  = rnti_manager::ntn_lease_time_point{};

  mac_ntn_rnti_lease_pool_update initial;
  initial.cell_index    = cell;
  initial.operation     = mac_ntn_rnti_lease_pool_operation::replace;
  initial.generation_id = 71;
  initial.expiry_ms     = 100;
  initial.leases        = {to_rnti(0x4701)};
  ASSERT_TRUE(rnti_db.apply_ntn_rnti_lease_pool_update(initial, now).accepted);

  mac_ntn_rnti_lease_pool_update replacement = initial;
  replacement.generation_id                  = 72;
  replacement.expiry_ms                      = 1000;
  replacement.leases                         = {to_rnti(0x4801)};
  ASSERT_TRUE(rnti_db.apply_ntn_rnti_lease_pool_update(
                         replacement, now + std::chrono::milliseconds{initial.expiry_ms})
                  .accepted);

  const auto snapshot =
      rnti_db.get_ntn_rnti_lease_pool_snapshot(cell, now + std::chrono::milliseconds{initial.expiry_ms});
  ASSERT_EQ(snapshot.leases.size(), 2U);
  const auto expired = std::find_if(snapshot.leases.begin(), snapshot.leases.end(), [&](const auto& lease) {
    return lease.rnti == initial.leases.front();
  });
  ASSERT_NE(expired, snapshot.leases.end());
  EXPECT_EQ(expired->generation_id, initial.generation_id);
  EXPECT_EQ(expired->state, "expired");
  const auto pending = std::find_if(snapshot.leases.begin(), snapshot.leases.end(), [&](const auto& lease) {
    return lease.rnti == replacement.leases.front();
  });
  ASSERT_NE(pending, snapshot.leases.end());
  EXPECT_EQ(pending->generation_id, replacement.generation_id);
  EXPECT_EQ(pending->state, "pending");

  mac_ntn_rnti_lease_pool_update resurrection = replacement;
  resurrection.generation_id                  = 73;
  resurrection.leases                         = initial.leases;
  const auto rejected = rnti_db.apply_ntn_rnti_lease_pool_update(
      resurrection, now + std::chrono::milliseconds{initial.expiry_ms});
  EXPECT_FALSE(rejected.accepted);
  EXPECT_EQ(rejected.reason, "terminal_lease");
}

TEST(rnti_manager_test, when_same_generation_add_is_retried_then_update_is_idempotent)
{
  rnti_manager          rnti_db;
  const du_cell_index_t cell = to_du_cell_index(0);
  const auto            now  = rnti_manager::ntn_lease_time_point{};

  mac_ntn_rnti_lease_pool_update update;
  update.cell_index    = cell;
  update.operation     = mac_ntn_rnti_lease_pool_operation::add;
  update.generation_id = 81;
  update.expiry_ms     = 1000;
  update.leases        = {to_rnti(0x4701), to_rnti(0x4702)};

  ASSERT_TRUE(rnti_db.apply_ntn_rnti_lease_pool_update(update, now).accepted);
  const auto retry = rnti_db.apply_ntn_rnti_lease_pool_update(update, now + std::chrono::milliseconds{1});
  EXPECT_TRUE(retry.accepted);
  EXPECT_EQ(retry.accepted_leases, update.leases);
  const auto snapshot = rnti_db.get_ntn_rnti_lease_pool_snapshot(cell, now + std::chrono::milliseconds{1});
  ASSERT_EQ(snapshot.leases.size(), update.leases.size());
  EXPECT_EQ(snapshot.leases[0].generation_id, update.generation_id);
  EXPECT_EQ(snapshot.leases[1].generation_id, update.generation_id);
}

TEST(rnti_manager_test, when_same_generation_add_is_retried_after_consumption_then_terminal_state_is_not_resurrected)
{
  rnti_manager          rnti_db;
  const du_cell_index_t cell = to_du_cell_index(0);
  const auto            now  = rnti_manager::ntn_lease_time_point{};

  mac_ntn_rnti_lease_pool_update update;
  update.cell_index    = cell;
  update.operation     = mac_ntn_rnti_lease_pool_operation::add;
  update.generation_id = 82;
  update.expiry_ms     = 1000;
  update.leases        = {to_rnti(0x4701), to_rnti(0x4702)};

  ASSERT_TRUE(rnti_db.apply_ntn_rnti_lease_pool_update(update, now).accepted);
  ASSERT_EQ(rnti_db.allocate_ntn_lease(cell, now), update.leases.front());

  const auto retry = rnti_db.apply_ntn_rnti_lease_pool_update(update, now + std::chrono::milliseconds{1});
  ASSERT_TRUE(retry.accepted);
  EXPECT_EQ(retry.accepted_leases, update.leases);

  const auto snapshot = rnti_db.get_ntn_rnti_lease_pool_snapshot(cell, now + std::chrono::milliseconds{1});
  ASSERT_EQ(snapshot.leases.size(), update.leases.size());
  EXPECT_EQ(snapshot.leases[0].state, "consumed_by_mac");
  EXPECT_EQ(snapshot.leases[1].state, "pending");
  EXPECT_EQ(snapshot.leases[0].generation_id, update.generation_id);
  EXPECT_EQ(snapshot.leases[1].generation_id, update.generation_id);
}

TEST(rnti_manager_test, when_same_du_other_cell_tracks_rnti_then_second_pool_is_rejected)
{
  rnti_manager          rnti_db;
  const auto            now = rnti_manager::ntn_lease_time_point{};
  mac_ntn_rnti_lease_pool_update first;
  first.cell_index    = to_du_cell_index(0);
  first.operation     = mac_ntn_rnti_lease_pool_operation::add;
  first.generation_id = 91;
  first.expiry_ms     = 1000;
  first.leases        = {to_rnti(0x4701)};
  ASSERT_TRUE(rnti_db.apply_ntn_rnti_lease_pool_update(first, now).accepted);

  mac_ntn_rnti_lease_pool_update second = first;
  second.cell_index                    = to_du_cell_index(1);
  second.generation_id                 = 92;
  const auto result                    = rnti_db.apply_ntn_rnti_lease_pool_update(second, now);
  EXPECT_FALSE(result.accepted);
  EXPECT_EQ(result.reason, "rnti_tracked_in_other_cell");
  EXPECT_TRUE(rnti_db.get_ntn_rnti_lease_pool_snapshot(second.cell_index, now).leases.empty());
}

TEST(rnti_manager_test, allocate_for_cell_atomically_selects_enabled_ntn_pool_or_terrestrial_path)
{
  rnti_manager          rnti_db;
  const du_cell_index_t cell = to_du_cell_index(0);
  const auto            now  = rnti_manager::ntn_lease_time_point{};

  EXPECT_EQ(rnti_db.allocate_for_cell(cell, now), to_rnti(0x4601));

  mac_ntn_rnti_lease_pool_update update;
  update.cell_index    = cell;
  update.operation     = mac_ntn_rnti_lease_pool_operation::add;
  update.generation_id = 101;
  update.expiry_ms     = 1000;
  update.leases        = {to_rnti(0x4701)};
  ASSERT_TRUE(rnti_db.apply_ntn_rnti_lease_pool_update(update, now).accepted);
  EXPECT_EQ(rnti_db.allocate_for_cell(cell, now), update.leases.front());

  mac_ntn_rnti_lease_pool_update clear;
  clear.cell_index = cell;
  clear.operation  = mac_ntn_rnti_lease_pool_operation::clear;
  clear.generation_id = update.generation_id;
  ASSERT_TRUE(rnti_db.apply_ntn_rnti_lease_pool_update(clear, now).accepted);
  EXPECT_EQ(rnti_db.allocate_for_cell(cell, now), to_rnti(0x4602));
}

TEST(rnti_manager_test, when_ntn_lease_pool_is_empty_then_allocate_ntn_lease_does_not_fallback_to_local_allocator)
{
  rnti_manager rnti_db;

  ASSERT_EQ(rnti_db.allocate_ntn_lease(), rnti_t::INVALID_RNTI);
  ASSERT_EQ(rnti_db.nof_ues(), 0);
}

TEST(rnti_manager_test, when_ntn_lease_matches_existing_ue_then_it_is_rejected)
{
  rnti_manager rnti_db;
  ASSERT_TRUE(rnti_db.add_ue(to_rnti(0x4701), to_du_ue_index(0)));

  ASSERT_FALSE(rnti_db.add_ntn_rnti_lease(to_rnti(0x4701)));
  ASSERT_EQ(rnti_db.nof_ntn_rnti_leases(), 0U);
}

TEST(rnti_manager_test, ntn_lease_pool_is_cell_aware)
{
  rnti_manager rnti_db;

  const du_cell_index_t cell0 = to_du_cell_index(0);
  const du_cell_index_t cell1 = to_du_cell_index(1);

  ASSERT_TRUE(rnti_db.add_ntn_rnti_lease(cell0, to_rnti(0x4701)));
  ASSERT_TRUE(rnti_db.add_ntn_rnti_lease(cell1, to_rnti(0x4801)));

  EXPECT_EQ(rnti_db.nof_ntn_rnti_leases(cell0), 1U);
  EXPECT_EQ(rnti_db.nof_ntn_rnti_leases(cell1), 1U);

  EXPECT_EQ(rnti_db.allocate_ntn_lease(cell1), to_rnti(0x4801));
  EXPECT_EQ(rnti_db.allocate_ntn_lease(cell0), to_rnti(0x4701));
  EXPECT_EQ(rnti_db.nof_ntn_rnti_leases(cell0), 0U);
  EXPECT_EQ(rnti_db.nof_ntn_rnti_leases(cell1), 0U);
}

TEST(rnti_manager_test, when_cells_reuse_identity_context_then_cell_index_keeps_lease_records_independent)
{
  rnti_manager          rnti_db;
  const du_cell_index_t cell0 = to_du_cell_index(0);
  const du_cell_index_t cell1 = to_du_cell_index(1);
  const auto            now   = rnti_manager::ntn_lease_time_point{};

  // PCI is intentionally not part of the MAC pool key. Two stable cells that reuse a PCI remain isolated by index.
  ASSERT_TRUE(rnti_db.add_ntn_rnti_lease(cell0, to_rnti(0x4701), 40, 1000, now));
  ASSERT_TRUE(rnti_db.add_ntn_rnti_lease(cell1, to_rnti(0x4801), 41, 1000, now));

  EXPECT_EQ(rnti_db.allocate_ntn_lease(cell0, now), to_rnti(0x4701));
  auto cell0_snapshot = rnti_db.get_ntn_rnti_lease_pool_snapshot(cell0, now);
  auto cell1_snapshot = rnti_db.get_ntn_rnti_lease_pool_snapshot(cell1, now);
  ASSERT_EQ(cell0_snapshot.leases.size(), 1U);
  ASSERT_EQ(cell1_snapshot.leases.size(), 1U);
  EXPECT_EQ(cell0_snapshot.leases.front().state, "consumed_by_mac");
  EXPECT_EQ(cell1_snapshot.leases.front().state, "pending");
}

TEST(rnti_manager_test, ntn_lease_mode_can_be_enabled_per_cell)
{
  rnti_manager rnti_db;

  rnti_db.set_ntn_rnti_lease_mode(to_du_cell_index(1), true);

  EXPECT_FALSE(rnti_db.is_ntn_rnti_lease_mode_enabled(to_du_cell_index(0)));
  EXPECT_TRUE(rnti_db.is_ntn_rnti_lease_mode_enabled(to_du_cell_index(1)));
}

TEST(rnti_manager_test, when_ntn_lease_mode_is_disabled_then_terrestrial_allocator_is_unchanged)
{
  rnti_manager          rnti_db;
  const du_cell_index_t cell = to_du_cell_index(0);

  rnti_db.set_ntn_rnti_lease_mode(cell, false);
  ASSERT_TRUE(rnti_db.add_ntn_rnti_lease(cell, to_rnti(0x4701)));

  EXPECT_FALSE(rnti_db.is_ntn_rnti_lease_mode_enabled(cell));
  EXPECT_EQ(rnti_db.allocate(), to_rnti(0x4601));
  EXPECT_EQ(rnti_db.nof_ntn_rnti_leases(cell), 1U);
}

TEST(rnti_manager_test, expired_rnti_can_be_retired_retried_and_reused_only_with_a_newer_generation)
{
  rnti_manager          rnti_db;
  const du_cell_index_t cell  = to_du_cell_index(0);
  const auto            now   = rnti_manager::ntn_lease_time_point{};
  const rnti_t          lease = to_rnti(0x4901);

  mac_ntn_rnti_lease_pool_update add;
  add.cell_index    = cell;
  add.operation     = mac_ntn_rnti_lease_pool_operation::add;
  add.generation_id = 110;
  add.expiry_ms     = 100;
  add.leases        = {lease};
  ASSERT_TRUE(rnti_db.apply_ntn_rnti_lease_pool_update(add, now).accepted);

  mac_ntn_rnti_lease_pool_update retire = add;
  retire.operation                      = mac_ntn_rnti_lease_pool_operation::retire;
  retire.expiry_ms                      = 0;
  ASSERT_TRUE(rnti_db.apply_ntn_rnti_lease_pool_update(retire, now + std::chrono::milliseconds{100}).accepted);
  ASSERT_TRUE(rnti_db.get_ntn_rnti_lease_pool_snapshot(cell, now + std::chrono::milliseconds{100}).leases.empty());

  // Retrying the exact retirement is idempotent, while the old add can no longer resurrect the lease.
  EXPECT_TRUE(rnti_db.apply_ntn_rnti_lease_pool_update(retire, now + std::chrono::milliseconds{101}).accepted);
  const auto stale_add = rnti_db.apply_ntn_rnti_lease_pool_update(add, now + std::chrono::milliseconds{101});
  EXPECT_FALSE(stale_add.accepted);
  EXPECT_EQ(stale_add.reason, "stale_generation");

  mac_ntn_rnti_lease_pool_update reuse = add;
  reuse.generation_id                  = 111;
  ASSERT_TRUE(rnti_db.apply_ntn_rnti_lease_pool_update(reuse, now + std::chrono::milliseconds{101}).accepted);

  const auto delayed_retire = rnti_db.apply_ntn_rnti_lease_pool_update(retire, now + std::chrono::milliseconds{102});
  EXPECT_FALSE(delayed_retire.accepted);
  EXPECT_EQ(delayed_retire.reason, "stale_generation");
  const auto snapshot = rnti_db.get_ntn_rnti_lease_pool_snapshot(cell, now + std::chrono::milliseconds{102});
  ASSERT_EQ(snapshot.leases.size(), 1U);
  EXPECT_EQ(snapshot.leases.front().generation_id, 111U);
  EXPECT_TRUE(snapshot.retirement_supported);
  EXPECT_EQ(snapshot.rnti_generation_high_water, 111U);
}

TEST(rnti_manager_test, rnti_retirement_batch_is_atomic_when_one_rnti_is_still_active)
{
  rnti_manager          rnti_db;
  const du_cell_index_t cell = to_du_cell_index(0);
  const auto            now  = rnti_manager::ntn_lease_time_point{};

  mac_ntn_rnti_lease_pool_update add;
  add.cell_index    = cell;
  add.operation     = mac_ntn_rnti_lease_pool_operation::add;
  add.generation_id = 120;
  add.expiry_ms     = 100;
  add.leases        = {to_rnti(0x4901), to_rnti(0x4902)};
  ASSERT_TRUE(rnti_db.apply_ntn_rnti_lease_pool_update(add, now).accepted);
  ASSERT_TRUE(rnti_db.add_ue(add.leases[1], to_du_ue_index(0)));

  mac_ntn_rnti_lease_pool_update retire = add;
  retire.operation                      = mac_ntn_rnti_lease_pool_operation::retire;
  retire.expiry_ms                      = 0;
  const auto rejected = rnti_db.apply_ntn_rnti_lease_pool_update(retire, now + std::chrono::milliseconds{100});
  EXPECT_FALSE(rejected.accepted);
  EXPECT_EQ(rejected.reason, "active_rnti");
  EXPECT_EQ(rejected.rejected_leases, retire.leases);
  EXPECT_EQ(rnti_db.get_ntn_rnti_lease_pool_snapshot(cell, now + std::chrono::milliseconds{100}).leases.size(), 2U);

  rnti_db.rem_ue(add.leases[1]);
  EXPECT_TRUE(rnti_db.apply_ntn_rnti_lease_pool_update(retire, now + std::chrono::milliseconds{100}).accepted);
  EXPECT_TRUE(rnti_db.get_ntn_rnti_lease_pool_snapshot(cell, now + std::chrono::milliseconds{100}).leases.empty());
}

TEST(rnti_manager_test, missing_rnti_cannot_be_retired_and_clear_preserves_existing_replay_tombstone)
{
  rnti_manager          rnti_db;
  const du_cell_index_t cell  = to_du_cell_index(0);
  const auto            now   = rnti_manager::ntn_lease_time_point{};
  const rnti_t          lease = to_rnti(0x4901);

  mac_ntn_rnti_lease_pool_update retire;
  retire.cell_index    = cell;
  retire.operation     = mac_ntn_rnti_lease_pool_operation::retire;
  retire.generation_id = 130;
  retire.expiry_ms     = 0;
  retire.leases        = {lease};
  const auto missing   = rnti_db.apply_ntn_rnti_lease_pool_update(retire, now);
  EXPECT_FALSE(missing.accepted);
  EXPECT_EQ(missing.reason, "lease_not_found");

  mac_ntn_rnti_lease_pool_update add = retire;
  add.operation                      = mac_ntn_rnti_lease_pool_operation::add;
  add.expiry_ms                      = 100;
  ASSERT_TRUE(rnti_db.apply_ntn_rnti_lease_pool_update(add, now).accepted);
  ASSERT_TRUE(rnti_db.apply_ntn_rnti_lease_pool_update(retire, now + std::chrono::milliseconds{100}).accepted);
  EXPECT_TRUE(rnti_db.apply_ntn_rnti_lease_pool_update(retire, now + std::chrono::milliseconds{101}).accepted);
  mac_ntn_rnti_lease_pool_update future_retire = retire;
  future_retire.generation_id                  = 131;
  EXPECT_EQ(rnti_db.apply_ntn_rnti_lease_pool_update(future_retire, now + std::chrono::milliseconds{101}).reason,
            "generation_mismatch");
  EXPECT_EQ(rnti_db.apply_ntn_rnti_lease_pool_update(add, now + std::chrono::milliseconds{101}).reason,
            "stale_generation");

  add.generation_id = 131;
  add.expiry_ms     = 1000;
  ASSERT_TRUE(rnti_db.apply_ntn_rnti_lease_pool_update(add, now + std::chrono::milliseconds{101}).accepted);
  mac_ntn_rnti_lease_pool_update clear;
  clear.cell_index    = cell;
  clear.operation     = mac_ntn_rnti_lease_pool_operation::clear;
  clear.generation_id = add.generation_id;
  ASSERT_TRUE(rnti_db.apply_ntn_rnti_lease_pool_update(clear, now + std::chrono::milliseconds{101}).accepted);

  EXPECT_EQ(rnti_db.apply_ntn_rnti_lease_pool_update(add, now + std::chrono::milliseconds{101}).reason,
            "stale_generation");
  add.generation_id = 132;
  EXPECT_TRUE(rnti_db.apply_ntn_rnti_lease_pool_update(add, now + std::chrono::milliseconds{101}).accepted);
}

TEST(rnti_manager_test, clear_generation_is_idempotent_and_cannot_delete_newer_rnti_reuse)
{
  rnti_manager          rnti_db;
  const du_cell_index_t cell  = to_du_cell_index(0);
  const auto            now   = rnti_manager::ntn_lease_time_point{};
  const rnti_t          lease = to_rnti(0x4901);

  mac_ntn_rnti_lease_pool_update add;
  add.cell_index    = cell;
  add.operation     = mac_ntn_rnti_lease_pool_operation::add;
  add.generation_id = 150;
  add.expiry_ms     = 1000;
  add.leases        = {lease};
  ASSERT_TRUE(rnti_db.apply_ntn_rnti_lease_pool_update(add, now).accepted);

  mac_ntn_rnti_lease_pool_update clear;
  clear.cell_index    = cell;
  clear.operation     = mac_ntn_rnti_lease_pool_operation::clear;
  clear.generation_id = add.generation_id;
  ASSERT_TRUE(rnti_db.apply_ntn_rnti_lease_pool_update(clear, now).accepted);
  EXPECT_TRUE(rnti_db.apply_ntn_rnti_lease_pool_update(clear, now).accepted);

  mac_ntn_rnti_lease_pool_update reuse = add;
  reuse.generation_id                  = 151;
  ASSERT_TRUE(rnti_db.apply_ntn_rnti_lease_pool_update(reuse, now).accepted);

  const auto stale_clear = rnti_db.apply_ntn_rnti_lease_pool_update(clear, now);
  EXPECT_FALSE(stale_clear.accepted);
  EXPECT_EQ(stale_clear.reason, "stale_generation");

  mac_ntn_rnti_lease_pool_update future_clear = clear;
  future_clear.generation_id                  = 152;
  const auto mismatched_clear                 = rnti_db.apply_ntn_rnti_lease_pool_update(future_clear, now);
  EXPECT_FALSE(mismatched_clear.accepted);
  EXPECT_EQ(mismatched_clear.reason, "generation_mismatch");

  auto snapshot = rnti_db.get_ntn_rnti_lease_pool_snapshot(cell, now);
  ASSERT_EQ(snapshot.leases.size(), 1U);
  EXPECT_EQ(snapshot.leases.front().generation_id, reuse.generation_id);

  clear.generation_id = reuse.generation_id;
  EXPECT_TRUE(rnti_db.apply_ntn_rnti_lease_pool_update(clear, now).accepted);
  EXPECT_TRUE(rnti_db.get_ntn_rnti_lease_pool_snapshot(cell, now).leases.empty());
}

TEST(rnti_manager_test, zero_generation_clear_is_allowed_only_before_cell_has_seen_a_lease)
{
  rnti_manager          rnti_db;
  const du_cell_index_t cell = to_du_cell_index(0);
  const auto            now  = rnti_manager::ntn_lease_time_point{};

  mac_ntn_rnti_lease_pool_update empty_clear;
  empty_clear.cell_index = cell;
  empty_clear.operation  = mac_ntn_rnti_lease_pool_operation::clear;
  EXPECT_TRUE(rnti_db.apply_ntn_rnti_lease_pool_update(empty_clear, now).accepted);
  EXPECT_TRUE(rnti_db.apply_ntn_rnti_lease_pool_update(empty_clear, now).accepted);

  mac_ntn_rnti_lease_pool_update add;
  add.cell_index    = cell;
  add.operation     = mac_ntn_rnti_lease_pool_operation::add;
  add.generation_id = 160;
  add.expiry_ms     = 1000;
  add.leases        = {to_rnti(0x4901)};
  ASSERT_TRUE(rnti_db.apply_ntn_rnti_lease_pool_update(add, now).accepted);

  const auto rejected = rnti_db.apply_ntn_rnti_lease_pool_update(empty_clear, now);
  EXPECT_FALSE(rejected.accepted);
  EXPECT_EQ(rejected.reason, "invalid_generation");
  EXPECT_EQ(rnti_db.get_ntn_rnti_lease_pool_snapshot(cell, now).leases.size(), 1U);
}

TEST(rnti_manager_test, cell_generation_guards_are_independent_and_old_clear_cannot_delete_new_reuse)
{
  rnti_manager          rnti_db;
  const du_cell_index_t cell_a = to_du_cell_index(0);
  const du_cell_index_t cell_b = to_du_cell_index(1);
  const auto            now    = rnti_manager::ntn_lease_time_point{};

  mac_ntn_rnti_lease_pool_update add_a;
  add_a.cell_index    = cell_a;
  add_a.operation     = mac_ntn_rnti_lease_pool_operation::add;
  add_a.generation_id = 42;
  add_a.expiry_ms     = 1000;
  add_a.leases        = {to_rnti(0x4901)};
  ASSERT_TRUE(rnti_db.apply_ntn_rnti_lease_pool_update(add_a, now).accepted);

  mac_ntn_rnti_lease_pool_update add_b = add_a;
  add_b.cell_index                     = cell_b;
  add_b.generation_id                  = 43;
  add_b.leases                         = {to_rnti(0x4a01)};
  ASSERT_TRUE(rnti_db.apply_ntn_rnti_lease_pool_update(add_b, now).accepted);

  // An exact retry for cell A remains idempotent even though cell B advanced the DU-wide high-water.
  EXPECT_TRUE(rnti_db.apply_ntn_rnti_lease_pool_update(add_a, now).accepted);

  mac_ntn_rnti_lease_pool_update clear_a;
  clear_a.cell_index    = cell_a;
  clear_a.operation     = mac_ntn_rnti_lease_pool_operation::clear;
  clear_a.generation_id = add_a.generation_id;
  ASSERT_TRUE(rnti_db.apply_ntn_rnti_lease_pool_update(clear_a, now).accepted);

  mac_ntn_rnti_lease_pool_update reuse_a = add_a;
  reuse_a.generation_id                  = 44;
  ASSERT_TRUE(rnti_db.apply_ntn_rnti_lease_pool_update(reuse_a, now).accepted);

  const auto delayed_clear = rnti_db.apply_ntn_rnti_lease_pool_update(clear_a, now);
  EXPECT_FALSE(delayed_clear.accepted);
  EXPECT_EQ(delayed_clear.reason, "stale_generation");

  const auto snapshot_a = rnti_db.get_ntn_rnti_lease_pool_snapshot(cell_a, now);
  const auto snapshot_b = rnti_db.get_ntn_rnti_lease_pool_snapshot(cell_b, now);
  ASSERT_EQ(snapshot_a.leases.size(), 1U);
  EXPECT_EQ(snapshot_a.leases.front().generation_id, 44U);
  ASSERT_EQ(snapshot_b.leases.size(), 1U);
  EXPECT_EQ(snapshot_b.leases.front().generation_id, 43U);
}

TEST(rnti_manager_test, rnti_retirement_rejects_pending_and_other_cell_records)
{
  rnti_manager                   rnti_db;
  const auto                     now = rnti_manager::ntn_lease_time_point{};
  mac_ntn_rnti_lease_pool_update add;
  add.cell_index    = to_du_cell_index(0);
  add.operation     = mac_ntn_rnti_lease_pool_operation::add;
  add.generation_id = 140;
  add.expiry_ms     = 1000;
  add.leases        = {to_rnti(0x4901)};
  ASSERT_TRUE(rnti_db.apply_ntn_rnti_lease_pool_update(add, now).accepted);

  mac_ntn_rnti_lease_pool_update retire = add;
  retire.operation                      = mac_ntn_rnti_lease_pool_operation::retire;
  retire.expiry_ms                      = 0;
  EXPECT_EQ(rnti_db.apply_ntn_rnti_lease_pool_update(retire, now).reason, "lease_not_expired");

  retire.cell_index = to_du_cell_index(1);
  EXPECT_EQ(rnti_db.apply_ntn_rnti_lease_pool_update(retire, now).reason, "rnti_tracked_in_other_cell");
}
