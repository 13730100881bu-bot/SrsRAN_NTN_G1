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

TEST(rnti_manager_test, when_ntn_lease_is_available_then_allocate_ntn_lease_returns_reserved_rnti)
{
  rnti_manager rnti_db;

  ASSERT_TRUE(rnti_db.add_ntn_rnti_lease(to_rnti(0x4701)));
  ASSERT_EQ(rnti_db.nof_ntn_rnti_leases(), 1U);

  ASSERT_EQ(rnti_db.allocate_ntn_lease(), to_rnti(0x4701));
  ASSERT_EQ(rnti_db.nof_ntn_rnti_leases(), 0U);
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

TEST(rnti_manager_test, ntn_lease_mode_can_be_enabled_per_cell)
{
  rnti_manager rnti_db;

  rnti_db.set_ntn_rnti_lease_mode(to_du_cell_index(1), true);

  EXPECT_FALSE(rnti_db.is_ntn_rnti_lease_mode_enabled(to_du_cell_index(0)));
  EXPECT_TRUE(rnti_db.is_ntn_rnti_lease_mode_enabled(to_du_cell_index(1)));
}
