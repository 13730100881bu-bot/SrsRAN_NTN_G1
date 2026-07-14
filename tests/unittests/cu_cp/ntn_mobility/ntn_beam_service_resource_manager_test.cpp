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

#include "lib/cu_cp/ntn_mobility/ntn_beam_service_resource_manager.h"
#include "srsran/ran/gnb_id.h"
#include <algorithm>
#include <gtest/gtest.h>

using namespace srsran;
using namespace srs_cu_cp;

namespace {

nr_cell_identity make_nci(unsigned sector_id)
{
  return nr_cell_identity::create(gnb_id_t{0x19b, 32}, sector_id).value();
}

ntn_beam_du_assignment make_loaded_assignment(const std::string& beam_id,
                                              unsigned           sector_id,
                                              du_index_t         du_index)
{
  ntn_beam_du_assignment assignment;
  assignment.beam_id         = beam_id;
  assignment.nci             = make_nci(sector_id);
  assignment.du_index        = du_index;
  assignment.state           = ntn_beam_assignment_state::active_loaded;
  assignment.nof_ues         = 1;
  assignment.nof_drbs        = 1;
  assignment.uplink_ready    = true;
  assignment.sr_slot_offset  = 3;
  assignment.sr_slot_period  = 40;
  assignment.srs_slot_offset = 7;
  assignment.srs_slot_period = 80;
  return assignment;
}

ntn_rnti_lease_pool_update make_target_handover_lease_pool()
{
  ntn_rnti_lease_pool_update lease_pool;
  lease_pool.du_index       = uint_to_du_index(1);
  lease_pool.cell_index     = to_du_cell_index(2);
  lease_pool.pci            = pci_t{17};
  lease_pool.analog_beam_id = "ANALOG-TARGET-001";
  lease_pool.generation_id  = 101;
  lease_pool.leases         = {to_rnti(0x4801), to_rnti(0x4802)};
  return lease_pool;
}

void apply_lease_pool(ntn_beam_service_resource_manager& manager, const ntn_rnti_lease_pool_update& lease_pool)
{
  ASSERT_TRUE(manager.reserve_rnti_leases(lease_pool).accepted);

  f1ap_ntn_rnti_lease_pool_result result;
  result.generation_id   = lease_pool.generation_id;
  result.accepted        = true;
  result.accepted_leases = lease_pool.leases;
  manager.mark_rnti_lease_pool_distribution_result(lease_pool, result);
}

} // namespace

TEST(ntn_beam_service_resource_manager, records_access_rnti_ownership_and_releases_after_ics)
{
  ntn_beam_service_resource_manager manager;

  ntn_access_rnti_ownership_update ownership;
  ownership.ue_index       = uint_to_ue_index(1);
  ownership.du_index       = uint_to_du_index(0);
  ownership.cell_index     = to_du_cell_index(1);
  ownership.pci            = pci_t{1};
  ownership.rnti           = to_rnti(0x4601);
  ownership.analog_beam_id = "ANALOG-ACCESS-001";
  ownership.access_nci     = make_nci(1);
  ownership.has_access_nci = true;

  const ntn_access_rnti_ownership_result result = manager.register_access_rnti_ownership(ownership);
  EXPECT_TRUE(result.accepted);
  EXPECT_EQ(result.state, "observed");
  EXPECT_EQ(result.reason, "access_active");

  ntn_beam_service_resource_snapshot snapshot = manager.get_snapshot();
  ASSERT_EQ(snapshot.access_rnti_ownerships.size(), 1U);
  EXPECT_EQ(snapshot.nof_access_rnti_owned, 1U);
  EXPECT_EQ(snapshot.nof_access_rnti_conflicts, 0U);
  EXPECT_EQ(snapshot.access_rnti_ownerships.front().analog_beam_id, "ANALOG-ACCESS-001");
  EXPECT_EQ(snapshot.access_rnti_ownerships.front().state, "observed");

  manager.release_analog_access_after_ics(uint_to_ue_index(1));

  snapshot = manager.get_snapshot();
  ASSERT_EQ(snapshot.access_rnti_ownerships.size(), 1U);
  EXPECT_EQ(snapshot.nof_access_rnti_owned, 0U);
  EXPECT_EQ(snapshot.access_rnti_ownerships.front().state, "released_after_ics");
  EXPECT_EQ(snapshot.access_rnti_ownerships.front().reason, "released_after_ics");
}

TEST(ntn_beam_service_resource_manager, validates_access_rnti_against_authoritative_lease_pool)
{
  ntn_beam_service_resource_manager manager;
  manager.set_authoritative_rnti_lease_validation_enabled(true);

  ntn_rnti_lease_pool_update lease_pool;
  lease_pool.du_index       = uint_to_du_index(0);
  lease_pool.cell_index     = to_du_cell_index(1);
  lease_pool.pci            = pci_t{1};
  lease_pool.analog_beam_id = "ANALOG-ACCESS-001";
  lease_pool.leases.push_back(to_rnti(0x4701));
  ASSERT_TRUE(manager.reserve_rnti_leases(lease_pool).accepted);
  f1ap_ntn_rnti_lease_pool_result lease_result;
  lease_result.accepted        = true;
  lease_result.accepted_leases = lease_pool.leases;
  manager.mark_rnti_lease_pool_distribution_result(lease_pool, lease_result);

  ntn_access_rnti_ownership_update ownership;
  ownership.ue_index       = uint_to_ue_index(1);
  ownership.du_index       = uint_to_du_index(0);
  ownership.cell_index     = to_du_cell_index(1);
  ownership.pci            = pci_t{1};
  ownership.rnti           = to_rnti(0x4701);
  ownership.analog_beam_id = "ANALOG-ACCESS-001";

  const ntn_access_rnti_ownership_result result = manager.register_access_rnti_ownership(ownership);
  EXPECT_TRUE(result.accepted);
  EXPECT_EQ(result.state, "initial_ul_seen");
  EXPECT_EQ(result.reason, "lease_validated");

  ntn_beam_service_resource_snapshot snapshot = manager.get_snapshot();
  ASSERT_EQ(snapshot.rnti_leases.size(), 1U);
  EXPECT_EQ(snapshot.rnti_leases.front().state, "initial_ul_seen");
  EXPECT_EQ(snapshot.nof_rnti_leases_initial_ul_seen, 1U);

  manager.release_analog_access_after_ics(uint_to_ue_index(1));

  snapshot = manager.get_snapshot();
  ASSERT_EQ(snapshot.rnti_leases.size(), 1U);
  EXPECT_EQ(snapshot.rnti_leases.front().state, "committed");
  EXPECT_EQ(snapshot.nof_rnti_leases_committed, 1U);
}

TEST(ntn_beam_service_resource_manager, tracks_rnti_lease_pool_distribution_to_du)
{
  ntn_beam_service_resource_manager manager;

  ntn_rnti_lease_pool_update lease_pool;
  lease_pool.du_index       = uint_to_du_index(0);
  lease_pool.cell_index     = to_du_cell_index(1);
  lease_pool.pci            = pci_t{1};
  lease_pool.analog_beam_id = "ANALOG-ACCESS-001";
  lease_pool.generation_id  = 77;
  lease_pool.leases         = {to_rnti(0x4701), to_rnti(0x4702)};
  ASSERT_TRUE(manager.reserve_rnti_leases(lease_pool).accepted);

  ntn_beam_service_resource_snapshot snapshot = manager.get_snapshot();
  ASSERT_EQ(snapshot.rnti_leases.size(), 2U);
  EXPECT_EQ(snapshot.rnti_leases.front().distribution_state, "desired");

  manager.mark_rnti_lease_pool_sent_to_du(lease_pool);
  snapshot = manager.get_snapshot();
  EXPECT_EQ(snapshot.nof_rnti_leases_sent_to_du, 2U);

  f1ap_ntn_rnti_lease_pool_result result;
  result.generation_id    = lease_pool.generation_id;
  result.accepted         = false;
  result.reject_reason    = "partial";
  result.accepted_leases  = {to_rnti(0x4701)};
  result.rejected_leases  = {to_rnti(0x4702)};

  manager.mark_rnti_lease_pool_distribution_result(lease_pool, result);
  snapshot = manager.get_snapshot();
  EXPECT_EQ(snapshot.nof_rnti_leases_applied_by_du, 1U);
  EXPECT_EQ(snapshot.nof_rnti_leases_rejected_by_du, 1U);
}

TEST(ntn_beam_service_resource_manager, access_pool_is_ready_only_after_du_applies_usable_lease)
{
  ntn_beam_service_resource_manager manager;

  ntn_rnti_lease_pool_update lease_pool;
  lease_pool.du_index       = uint_to_du_index(0);
  lease_pool.cell_index     = to_du_cell_index(1);
  lease_pool.pci            = pci_t{1};
  lease_pool.analog_beam_id = "ANALOG-ACCESS-001";
  lease_pool.generation_id  = 77;
  lease_pool.leases         = {to_rnti(0x4701), to_rnti(0x4702)};
  ASSERT_TRUE(manager.reserve_rnti_leases(lease_pool).accepted);

  EXPECT_FALSE(manager.is_access_rnti_pool_ready(uint_to_du_index(0), to_du_cell_index(1), pci_t{1}, "ANALOG-ACCESS-001"));

  manager.mark_rnti_lease_pool_sent_to_du(lease_pool);
  EXPECT_FALSE(manager.is_access_rnti_pool_ready(uint_to_du_index(0), to_du_cell_index(1), pci_t{1}, "ANALOG-ACCESS-001"));

  f1ap_ntn_rnti_lease_pool_result result;
  result.generation_id   = lease_pool.generation_id;
  result.accepted        = true;
  result.accepted_leases = lease_pool.leases;
  manager.mark_rnti_lease_pool_distribution_result(lease_pool, result);

  EXPECT_TRUE(manager.is_access_rnti_pool_ready(uint_to_du_index(0), to_du_cell_index(1), pci_t{1}, "ANALOG-ACCESS-001"));
}

TEST(ntn_beam_service_resource_manager, rar_offer_and_initial_ul_use_same_applied_lease)
{
  ntn_beam_service_resource_manager manager;
  manager.set_authoritative_rnti_lease_validation_enabled(true);

  ntn_rnti_lease_pool_update lease_pool;
  lease_pool.du_index       = uint_to_du_index(0);
  lease_pool.cell_index     = to_du_cell_index(1);
  lease_pool.pci            = pci_t{1};
  lease_pool.analog_beam_id = "ANALOG-ACCESS-001";
  lease_pool.generation_id  = 77;
  lease_pool.leases         = {to_rnti(0x4701)};
  ASSERT_TRUE(manager.reserve_rnti_leases(lease_pool).accepted);

  f1ap_ntn_rnti_lease_pool_result result;
  result.generation_id   = lease_pool.generation_id;
  result.accepted        = true;
  result.accepted_leases = lease_pool.leases;
  manager.mark_rnti_lease_pool_distribution_result(lease_pool, result);

  EXPECT_TRUE(
      manager.mark_rnti_offered_in_rar(uint_to_du_index(0), to_du_cell_index(1), pci_t{1}, to_rnti(0x4701)).accepted);

  ntn_access_rnti_ownership_update ownership;
  ownership.ue_index       = uint_to_ue_index(1);
  ownership.du_index       = uint_to_du_index(0);
  ownership.cell_index     = to_du_cell_index(1);
  ownership.pci            = pci_t{1};
  ownership.rnti           = to_rnti(0x4701);
  ownership.analog_beam_id = "ANALOG-ACCESS-001";

  const ntn_access_rnti_ownership_result initial_ul = manager.register_access_rnti_ownership(ownership);
  EXPECT_TRUE(initial_ul.accepted);
  EXPECT_EQ(initial_ul.state, "initial_ul_seen");
  EXPECT_EQ(initial_ul.reason, "lease_validated");

  const ntn_beam_service_resource_snapshot snapshot = manager.get_snapshot();
  ASSERT_EQ(snapshot.rnti_leases.size(), 1U);
  EXPECT_EQ(snapshot.rnti_leases.front().state, "initial_ul_seen");
  EXPECT_EQ(snapshot.rnti_leases.front().reason, "lease_validated");
}

TEST(ntn_beam_service_resource_manager, expired_or_wrong_beam_lease_rejects_initial_ul)
{
  ntn_beam_service_resource_manager manager;
  manager.set_authoritative_rnti_lease_validation_enabled(true);

  ntn_rnti_lease_pool_update lease_pool;
  lease_pool.du_index       = uint_to_du_index(0);
  lease_pool.cell_index     = to_du_cell_index(1);
  lease_pool.pci            = pci_t{1};
  lease_pool.analog_beam_id = "ANALOG-ACCESS-001";
  lease_pool.generation_id  = 77;
  lease_pool.leases         = {to_rnti(0x4701), to_rnti(0x4702)};
  ASSERT_TRUE(manager.reserve_rnti_leases(lease_pool).accepted);

  f1ap_ntn_rnti_lease_pool_result result;
  result.generation_id   = lease_pool.generation_id;
  result.accepted        = true;
  result.accepted_leases = lease_pool.leases;
  manager.mark_rnti_lease_pool_distribution_result(lease_pool, result);
  manager.expire_rnti_leases_for_analog_beam("ANALOG-ACCESS-001", "window_closed");

  ntn_access_rnti_ownership_update expired;
  expired.ue_index       = uint_to_ue_index(1);
  expired.du_index       = uint_to_du_index(0);
  expired.cell_index     = to_du_cell_index(1);
  expired.pci            = pci_t{1};
  expired.rnti           = to_rnti(0x4701);
  expired.analog_beam_id = "ANALOG-ACCESS-001";

  const ntn_access_rnti_ownership_result expired_result = manager.register_access_rnti_ownership(expired);
  EXPECT_FALSE(expired_result.accepted);
  EXPECT_EQ(expired_result.reason, "expired_rnti");

  ntn_access_rnti_ownership_update wrong_beam = expired;
  wrong_beam.ue_index                         = uint_to_ue_index(2);
  wrong_beam.rnti                             = to_rnti(0x4702);
  wrong_beam.analog_beam_id                   = "ANALOG-ACCESS-002";

  const ntn_access_rnti_ownership_result wrong_beam_result = manager.register_access_rnti_ownership(wrong_beam);
  EXPECT_FALSE(wrong_beam_result.accepted);
  EXPECT_EQ(wrong_beam_result.reason, "wrong_cell_or_beam");
}

TEST(ntn_beam_service_resource_manager, low_watermark_counts_only_applied_unused_leases)
{
  ntn_beam_service_resource_manager manager;

  ntn_rnti_lease_pool_update lease_pool;
  lease_pool.du_index       = uint_to_du_index(0);
  lease_pool.cell_index     = to_du_cell_index(1);
  lease_pool.pci            = pci_t{1};
  lease_pool.analog_beam_id = "ANALOG-ACCESS-001";
  lease_pool.generation_id  = 77;
  lease_pool.leases         = {to_rnti(0x4701), to_rnti(0x4702)};
  ASSERT_TRUE(manager.reserve_rnti_leases(lease_pool).accepted);

  f1ap_ntn_rnti_lease_pool_result result;
  result.generation_id   = lease_pool.generation_id;
  result.accepted        = true;
  result.accepted_leases = lease_pool.leases;
  manager.mark_rnti_lease_pool_distribution_result(lease_pool, result);

  EXPECT_FALSE(manager.rnti_pool_below_low_watermark(uint_to_du_index(0), to_du_cell_index(1), pci_t{1}, "ANALOG-ACCESS-001", 1));

  EXPECT_TRUE(
      manager.mark_rnti_offered_in_rar(uint_to_du_index(0), to_du_cell_index(1), pci_t{1}, to_rnti(0x4701)).accepted);

  EXPECT_TRUE(manager.rnti_pool_below_low_watermark(uint_to_du_index(0), to_du_cell_index(1), pci_t{1}, "ANALOG-ACCESS-001", 1));
}

TEST(ntn_beam_service_resource_manager, shared_pci_cells_can_reuse_same_rnti_without_cross_cell_conflict)
{
  ntn_beam_service_resource_manager manager;
  manager.set_authoritative_rnti_lease_validation_enabled(true);

  ntn_rnti_lease_pool_update first_pool;
  first_pool.du_index       = uint_to_du_index(0);
  first_pool.cell_index     = to_du_cell_index(1);
  first_pool.pci            = pci_t{101};
  first_pool.analog_beam_id = "ANALOG-ACCESS-001";
  first_pool.generation_id  = 101;
  first_pool.leases         = {to_rnti(0x4701)};
  apply_lease_pool(manager, first_pool);

  ntn_rnti_lease_pool_update second_pool = first_pool;
  second_pool.cell_index                 = to_du_cell_index(2);
  second_pool.analog_beam_id             = "ANALOG-ACCESS-002";
  second_pool.generation_id              = 102;
  apply_lease_pool(manager, second_pool);

  EXPECT_TRUE(manager
                  .mark_rnti_offered_in_rar(
                      first_pool.du_index, first_pool.cell_index, first_pool.pci, first_pool.leases.front())
                  .accepted);
  EXPECT_TRUE(manager
                  .mark_rnti_offered_in_rar(
                      second_pool.du_index, second_pool.cell_index, second_pool.pci, second_pool.leases.front())
                  .accepted);

  ntn_access_rnti_ownership_update first_ownership;
  first_ownership.ue_index       = uint_to_ue_index(1);
  first_ownership.du_index       = first_pool.du_index;
  first_ownership.cell_index     = first_pool.cell_index;
  first_ownership.pci            = first_pool.pci;
  first_ownership.rnti           = first_pool.leases.front();
  first_ownership.analog_beam_id = first_pool.analog_beam_id;
  ASSERT_TRUE(manager.register_access_rnti_ownership(first_ownership).accepted);

  ntn_access_rnti_ownership_update second_ownership = first_ownership;
  second_ownership.ue_index                         = uint_to_ue_index(2);
  second_ownership.cell_index                       = second_pool.cell_index;
  second_ownership.analog_beam_id                   = second_pool.analog_beam_id;
  EXPECT_TRUE(manager.register_access_rnti_ownership(second_ownership).accepted);

  const ntn_beam_service_resource_snapshot snapshot = manager.get_snapshot();
  EXPECT_EQ(snapshot.nof_access_rnti_owned, 2U);
  EXPECT_EQ(snapshot.nof_access_rnti_conflicts, 0U);
  EXPECT_EQ(snapshot.nof_rnti_leases_initial_ul_seen, 2U);
}

TEST(ntn_beam_service_resource_manager, reserves_applied_rnti_lease_for_connected_handover_target)
{
  ntn_beam_service_resource_manager manager;
  const ntn_rnti_lease_pool_update  lease_pool = make_target_handover_lease_pool();
  apply_lease_pool(manager, lease_pool);

  const ntn_handover_target_rnti_reservation_result reservation = manager.reserve_handover_target_rnti(
      uint_to_ue_index(11), lease_pool.du_index, lease_pool.cell_index, lease_pool.pci, lease_pool.analog_beam_id);

  ASSERT_TRUE(reservation.accepted);
  EXPECT_EQ(reservation.rnti, to_rnti(0x4801));
  EXPECT_EQ(reservation.state, "handover_reserved");
  EXPECT_EQ(reservation.reason, "connected_handover_target");

  const ntn_beam_service_resource_snapshot snapshot = manager.get_snapshot();
  ASSERT_EQ(snapshot.rnti_leases.size(), 2U);
  const auto reserved_it = std::find_if(snapshot.rnti_leases.begin(), snapshot.rnti_leases.end(), [](const auto& lease) {
    return lease.rnti == to_rnti(0x4801);
  });
  ASSERT_NE(reserved_it, snapshot.rnti_leases.end());
  EXPECT_EQ(reserved_it->state, "handover_reserved");
  EXPECT_EQ(reserved_it->ue_index, uint_to_ue_index(11));
  EXPECT_EQ(snapshot.nof_rnti_leases_available, 1U);
}

TEST(ntn_beam_service_resource_manager, commits_handover_target_rnti_after_success)
{
  ntn_beam_service_resource_manager manager;
  const ntn_rnti_lease_pool_update  lease_pool = make_target_handover_lease_pool();
  apply_lease_pool(manager, lease_pool);

  const ntn_handover_target_rnti_reservation_result reservation = manager.reserve_handover_target_rnti(
      uint_to_ue_index(11), lease_pool.du_index, lease_pool.cell_index, lease_pool.pci, lease_pool.analog_beam_id);
  ASSERT_TRUE(reservation.accepted);

  const ntn_access_rnti_ownership_result commit =
      manager.commit_handover_target_rnti(uint_to_ue_index(11), uint_to_ue_index(12), reservation.rnti);
  EXPECT_TRUE(commit.accepted);
  EXPECT_EQ(commit.state, "committed");
  EXPECT_EQ(commit.reason, "connected_handover_success");

  const ntn_beam_service_resource_snapshot snapshot = manager.get_snapshot();
  const auto committed_it = std::find_if(snapshot.rnti_leases.begin(), snapshot.rnti_leases.end(), [](const auto& lease) {
    return lease.rnti == to_rnti(0x4801);
  });
  ASSERT_NE(committed_it, snapshot.rnti_leases.end());
  EXPECT_EQ(committed_it->state, "committed");
  EXPECT_EQ(committed_it->ue_index, uint_to_ue_index(12));
  EXPECT_EQ(snapshot.nof_rnti_leases_committed, 1U);
}

TEST(ntn_beam_service_resource_manager, rolls_back_handover_target_rnti_when_preparation_fails)
{
  ntn_beam_service_resource_manager manager;
  const ntn_rnti_lease_pool_update  lease_pool = make_target_handover_lease_pool();
  apply_lease_pool(manager, lease_pool);

  const ntn_handover_target_rnti_reservation_result reservation = manager.reserve_handover_target_rnti(
      uint_to_ue_index(11), lease_pool.du_index, lease_pool.cell_index, lease_pool.pci, lease_pool.analog_beam_id);
  ASSERT_TRUE(reservation.accepted);

  manager.rollback_handover_target_rnti(uint_to_ue_index(11), "target_sr_srs_rejected");

  const ntn_beam_service_resource_snapshot snapshot = manager.get_snapshot();
  const auto rolled_back_it = std::find_if(snapshot.rnti_leases.begin(), snapshot.rnti_leases.end(), [](const auto& lease) {
    return lease.rnti == to_rnti(0x4801);
  });
  ASSERT_NE(rolled_back_it, snapshot.rnti_leases.end());
  EXPECT_EQ(rolled_back_it->state, "reserved");
  EXPECT_EQ(rolled_back_it->reason, "target_sr_srs_rejected");
  EXPECT_EQ(rolled_back_it->ue_index, ue_index_t::invalid);
  EXPECT_EQ(snapshot.nof_rnti_leases_available, 2U);
}

TEST(ntn_beam_service_resource_manager, marks_handover_target_rnti_conflict_on_commit_mismatch)
{
  ntn_beam_service_resource_manager manager;
  const ntn_rnti_lease_pool_update  lease_pool = make_target_handover_lease_pool();
  apply_lease_pool(manager, lease_pool);

  ASSERT_TRUE(manager
                  .reserve_handover_target_rnti(
                      uint_to_ue_index(11), lease_pool.du_index, lease_pool.cell_index, lease_pool.pci, lease_pool.analog_beam_id)
                  .accepted);

  const ntn_access_rnti_ownership_result commit =
      manager.commit_handover_target_rnti(uint_to_ue_index(11), uint_to_ue_index(12), to_rnti(0x4802));
  EXPECT_FALSE(commit.accepted);
  EXPECT_EQ(commit.state, "conflict");
  EXPECT_EQ(commit.reason, "target_rnti_mismatch");

  const ntn_beam_service_resource_snapshot snapshot = manager.get_snapshot();
  EXPECT_EQ(snapshot.nof_rnti_leases_conflict, 1U);
}

TEST(ntn_beam_service_resource_manager, strict_authoritative_mode_rejects_unexpected_access_rnti)
{
  ntn_beam_service_resource_manager manager;
  manager.set_authoritative_rnti_lease_validation_enabled(true);

  ntn_access_rnti_ownership_update ownership;
  ownership.ue_index       = uint_to_ue_index(1);
  ownership.du_index       = uint_to_du_index(0);
  ownership.cell_index     = to_du_cell_index(1);
  ownership.pci            = pci_t{1};
  ownership.rnti           = to_rnti(0x4701);
  ownership.analog_beam_id = "ANALOG-ACCESS-001";

  const ntn_access_rnti_ownership_result result = manager.register_access_rnti_ownership(ownership);
  EXPECT_FALSE(result.accepted);
  EXPECT_EQ(result.state, "conflict");
  EXPECT_EQ(result.reason, "unexpected_rnti");

  const ntn_beam_service_resource_snapshot snapshot = manager.get_snapshot();
  EXPECT_EQ(snapshot.nof_access_rnti_conflicts, 1U);
  ASSERT_EQ(snapshot.access_rnti_ownerships.size(), 1U);
  EXPECT_EQ(snapshot.access_rnti_ownerships.front().reason, "unexpected_rnti");
}

TEST(ntn_beam_service_resource_manager, reports_duplicate_access_rnti_as_conflict_without_replacing_owner)
{
  ntn_beam_service_resource_manager manager;

  ntn_access_rnti_ownership_update first;
  first.ue_index       = uint_to_ue_index(1);
  first.du_index       = uint_to_du_index(0);
  first.cell_index     = to_du_cell_index(1);
  first.pci            = pci_t{1};
  first.rnti           = to_rnti(0x4601);
  first.analog_beam_id = "ANALOG-ACCESS-001";
  ASSERT_TRUE(manager.register_access_rnti_ownership(first).accepted);

  ntn_access_rnti_ownership_update duplicate = first;
  duplicate.ue_index                         = uint_to_ue_index(2);
  duplicate.analog_beam_id                   = "ANALOG-ACCESS-002";

  const ntn_access_rnti_ownership_result result = manager.register_access_rnti_ownership(duplicate);
  EXPECT_FALSE(result.accepted);
  EXPECT_EQ(result.state, "conflict");
  EXPECT_EQ(result.reason, "duplicate_crnti");

  const ntn_beam_service_resource_snapshot snapshot = manager.get_snapshot();
  EXPECT_EQ(snapshot.nof_access_rnti_owned, 1U);
  EXPECT_EQ(snapshot.nof_access_rnti_conflicts, 1U);
  ASSERT_EQ(snapshot.access_rnti_ownerships.size(), 2U);
  EXPECT_EQ(snapshot.access_rnti_ownerships.front().ue_index, uint_to_ue_index(1));
  EXPECT_EQ(snapshot.access_rnti_ownerships.back().ue_index, uint_to_ue_index(2));
  EXPECT_EQ(snapshot.access_rnti_ownerships.back().state, "conflict");
}

TEST(ntn_beam_service_resource_manager, missing_cell_identity_is_rejected_before_rnti_keying)
{
  ntn_beam_service_resource_manager manager;

  ntn_rnti_lease_pool_update lease_pool;
  lease_pool.du_index       = uint_to_du_index(0);
  lease_pool.pci            = pci_t{1};
  lease_pool.analog_beam_id = "ANALOG-ACCESS-001";
  lease_pool.leases         = {to_rnti(0x4701)};
  EXPECT_FALSE(manager.reserve_rnti_leases(lease_pool).accepted);

  ntn_access_rnti_ownership_update ownership;
  ownership.ue_index       = uint_to_ue_index(1);
  ownership.du_index       = uint_to_du_index(0);
  ownership.pci            = pci_t{1};
  ownership.rnti           = to_rnti(0x4701);
  ownership.analog_beam_id = "ANALOG-ACCESS-001";
  EXPECT_FALSE(manager.register_access_rnti_ownership(ownership).accepted);
  EXPECT_TRUE(manager.get_snapshot().access_rnti_ownerships.empty());
}

TEST(ntn_beam_service_resource_manager, duplicate_rnti_in_same_lease_batch_is_rejected_atomically)
{
  ntn_beam_service_resource_manager manager;

  ntn_rnti_lease_pool_update lease_pool;
  lease_pool.du_index       = uint_to_du_index(0);
  lease_pool.cell_index     = to_du_cell_index(0);
  lease_pool.pci            = pci_t{1};
  lease_pool.analog_beam_id = "ANALOG-ACCESS-001";
  lease_pool.leases         = {to_rnti(0x4701), to_rnti(0x4701)};

  const ntn_rnti_lease_pool_update_result result = manager.reserve_rnti_leases(lease_pool);
  EXPECT_FALSE(result.accepted);
  EXPECT_EQ(result.state, "conflict");
  EXPECT_EQ(result.reason, "duplicate_lease");
  EXPECT_EQ(result.nof_leases_reserved, 0U);
  EXPECT_TRUE(manager.get_snapshot().rnti_leases.empty());
}

TEST(ntn_beam_service_resource_manager, creates_and_clears_digital_service_slot_intent)
{
  ntn_beam_service_resource_manager manager;
  ntn_beam_placement_plan           plan;
  plan.assignments.push_back(make_loaded_assignment("CN-BEAM-0001", 1, uint_to_du_index(0)));

  ntn_digital_service_slot_intent_update update;
  update.ue_index        = uint_to_ue_index(1);
  update.digital_beam_id = "CN-BEAM-0001";
  update.service_du_index = uint_to_du_index(0);
  update.service_nci     = make_nci(1);
  update.has_service_nci = true;
  update.service_state   = "service_bound";

  const ntn_slot_resource_update_decision set_decision =
      manager.update_digital_service_slot_intent(update, plan);
  ASSERT_EQ(set_decision.action, ntn_slot_resource_update_action::set);
  ASSERT_TRUE(set_decision.request.has_value());
  EXPECT_EQ(set_decision.request->sr_slot_offset, std::optional<unsigned>{3});
  EXPECT_EQ(set_decision.request->sr_slot_period, std::optional<unsigned>{40});
  EXPECT_EQ(set_decision.request->srs_slot_offset, std::optional<unsigned>{7});
  EXPECT_EQ(set_decision.request->srs_slot_period, std::optional<unsigned>{80});

  ntn_beam_service_resource_snapshot snapshot = manager.get_snapshot();
  ASSERT_EQ(snapshot.digital_slot_intents.size(), 1U);
  EXPECT_EQ(snapshot.nof_digital_slot_active, 1U);
  EXPECT_EQ(snapshot.digital_slot_intents.front().state, "active");
  EXPECT_EQ(snapshot.digital_slot_intents.front().reason, "loaded_service_calendar");
  manager.mark_slot_update_applied(uint_to_ue_index(1), *set_decision.request);

  const ntn_slot_resource_update_decision clear_decision =
      manager.clear_digital_service_slot_intent(uint_to_ue_index(1));
  EXPECT_EQ(clear_decision.action, ntn_slot_resource_update_action::clear);
  ASSERT_TRUE(clear_decision.previous_request.has_value());
  EXPECT_EQ(clear_decision.previous_request->sr_slot_offset, std::optional<unsigned>{3});

  snapshot = manager.get_snapshot();
  EXPECT_EQ(snapshot.nof_digital_slot_active, 0U);
  ASSERT_EQ(snapshot.digital_slot_intents.size(), 1U);
  EXPECT_EQ(snapshot.digital_slot_intents.front().state, "cleared");
}

TEST(ntn_beam_service_resource_manager, restores_previous_slot_request_when_clear_update_fails)
{
  ntn_beam_service_resource_manager manager;
  ntn_beam_placement_plan           plan;
  plan.assignments.push_back(make_loaded_assignment("CN-BEAM-0001", 1, uint_to_du_index(0)));

  ntn_digital_service_slot_intent_update update;
  update.ue_index         = uint_to_ue_index(1);
  update.digital_beam_id  = "CN-BEAM-0001";
  update.service_du_index = uint_to_du_index(0);
  update.service_nci      = make_nci(1);
  update.has_service_nci  = true;
  update.service_state    = "service_bound";

  const ntn_slot_resource_update_decision set_decision =
      manager.update_digital_service_slot_intent(update, plan);
  ASSERT_EQ(set_decision.action, ntn_slot_resource_update_action::set);
  ASSERT_TRUE(set_decision.request.has_value());
  manager.mark_slot_update_applied(uint_to_ue_index(1), *set_decision.request);

  const ntn_slot_resource_update_decision clear_decision =
      manager.clear_digital_service_slot_intent(uint_to_ue_index(1));
  ASSERT_EQ(clear_decision.action, ntn_slot_resource_update_action::clear);
  ASSERT_TRUE(clear_decision.request.has_value());
  ASSERT_TRUE(clear_decision.previous_request.has_value());

  manager.restore_or_clear_failed_slot_update(
      uint_to_ue_index(1), *clear_decision.request, clear_decision.previous_request);

  const std::optional<f1ap_ntn_ul_slot_resource_request> cached_request =
      manager.get_cached_slot_request(uint_to_ue_index(1));
  ASSERT_TRUE(cached_request.has_value());
  EXPECT_EQ(cached_request->sr_slot_offset, std::optional<unsigned>{3});
  EXPECT_EQ(cached_request->srs_slot_offset, std::optional<unsigned>{7});
}

TEST(ntn_beam_service_resource_manager, slot_resource_result_tracks_du_applied_and_rejected_states)
{
  ntn_beam_service_resource_manager manager;

  f1ap_ntn_ul_slot_resource_request request;
  request.sr_slot_offset  = 3U;
  request.sr_slot_period  = 40U;
  request.srs_slot_offset = 7U;
  request.srs_slot_period = 80U;

  const ue_index_t ue_index = uint_to_ue_index(1);
  const ntn_slot_resource_update_decision set_decision =
      manager.set_digital_slot_intent_from_request(ue_index, request, "loaded_service_calendar");
  ASSERT_EQ(set_decision.action, ntn_slot_resource_update_action::set);

  manager.mark_slot_update_sent_to_du(ue_index, request);
  ntn_beam_service_resource_snapshot snapshot = manager.get_snapshot();
  ASSERT_EQ(snapshot.digital_slot_intents.size(), 1U);
  EXPECT_EQ(snapshot.digital_slot_intents.front().state, "sent_to_du");
  EXPECT_EQ(snapshot.nof_digital_slot_sent_to_du, 1U);

  f1ap_ntn_ul_slot_resource_result applied;
  applied.accepted = true;
  applied.reason   = f1ap_ntn_ul_slot_resource_result_reason::applied;
  applied.applied_request = request;
  manager.mark_slot_update_result(ue_index, request, std::nullopt, applied);

  snapshot = manager.get_snapshot();
  ASSERT_EQ(snapshot.digital_slot_intents.size(), 1U);
  EXPECT_EQ(snapshot.digital_slot_intents.front().state, "applied_by_du");
  EXPECT_EQ(snapshot.digital_slot_intents.front().reason, "applied");
  EXPECT_EQ(snapshot.nof_digital_slot_applied_by_du, 1U);

  f1ap_ntn_ul_slot_resource_request next_request = request;
  next_request.sr_slot_offset = 4U;
  manager.set_digital_slot_intent_from_request(ue_index, next_request, "loaded_service_calendar");
  manager.mark_slot_update_sent_to_du(ue_index, next_request);

  f1ap_ntn_ul_slot_resource_result rejected;
  rejected.accepted = false;
  rejected.reason   = f1ap_ntn_ul_slot_resource_result_reason::sr_offset_unavailable;
  manager.mark_slot_update_result(ue_index, next_request, request, rejected);

  snapshot = manager.get_snapshot();
  ASSERT_EQ(snapshot.digital_slot_intents.size(), 1U);
  EXPECT_EQ(snapshot.digital_slot_intents.front().state, "rejected_by_du");
  EXPECT_EQ(snapshot.digital_slot_intents.front().reason, "sr_offset_unavailable");
  EXPECT_EQ(snapshot.nof_digital_slot_rejected_by_du, 1U);

  const std::optional<f1ap_ntn_ul_slot_resource_request> cached_request = manager.get_cached_slot_request(ue_index);
  ASSERT_TRUE(cached_request.has_value());
  EXPECT_EQ(cached_request->sr_slot_offset, std::optional<unsigned>{3U});
}

TEST(ntn_beam_service_resource_manager, audit_missing_applied_rnti_pool_requests_authoritative_resend)
{
  ntn_beam_service_resource_manager manager;

  ntn_rnti_lease_pool_update lease_pool;
  lease_pool.du_index       = uint_to_du_index(0);
  lease_pool.cell_index     = to_du_cell_index(1);
  lease_pool.pci            = pci_t{1};
  lease_pool.analog_beam_id = "ANALOG-ACCESS-001";
  lease_pool.generation_id  = 77;
  lease_pool.leases         = {to_rnti(0x4701), to_rnti(0x4702)};
  ASSERT_TRUE(manager.reserve_rnti_leases(lease_pool).accepted);

  f1ap_ntn_rnti_lease_pool_result apply_result;
  apply_result.generation_id   = lease_pool.generation_id;
  apply_result.accepted        = true;
  apply_result.accepted_leases = lease_pool.leases;
  manager.mark_rnti_lease_pool_distribution_result(lease_pool, apply_result);

  ntn_resource_audit_report report;
  report.du_index   = lease_pool.du_index;
  report.cell_index = lease_pool.cell_index;
  report.pci        = lease_pool.pci;
  report.generation_id = 10;

  const ntn_resource_audit_decision decision = manager.handle_resource_audit_report(report);
  EXPECT_EQ(decision.nof_mismatches, 1U);
  ASSERT_EQ(decision.repairs.size(), 1U);
  EXPECT_EQ(decision.repairs.front().action, ntn_resource_repair_action::resend_rnti_lease_pool);
  EXPECT_EQ(decision.repairs.front().reason, "du_missing_applied_rnti_pool");
}

TEST(ntn_beam_service_resource_manager, audit_unknown_du_slot_assignment_requests_clear)
{
  ntn_beam_service_resource_manager manager;

  ntn_resource_audit_report report;
  report.du_index      = uint_to_du_index(0);
  report.cell_index    = to_du_cell_index(1);
  report.pci           = pci_t{1};
  report.generation_id = 11;

  ntn_resource_audit_ue_slot du_slot;
  du_slot.ue_index = uint_to_ue_index(7);
  du_slot.state    = "applied_by_du";
  du_slot.request.sr_slot_offset  = 5U;
  du_slot.request.sr_slot_period  = 40U;
  du_slot.request.srs_slot_offset = 9U;
  du_slot.request.srs_slot_period = 80U;
  report.ue_slots.push_back(du_slot);

  const ntn_resource_audit_decision decision = manager.handle_resource_audit_report(report);
  EXPECT_EQ(decision.nof_mismatches, 1U);
  ASSERT_EQ(decision.repairs.size(), 1U);
  EXPECT_EQ(decision.repairs.front().action, ntn_resource_repair_action::clear_unknown_sr_srs_assignment);
  EXPECT_EQ(decision.repairs.front().ue_index, uint_to_ue_index(7));
}

TEST(ntn_beam_service_resource_manager, audit_missing_du_slot_assignment_requests_apply)
{
  ntn_beam_service_resource_manager manager;

  f1ap_ntn_ul_slot_resource_request request;
  request.sr_slot_offset  = 3U;
  request.sr_slot_period  = 40U;
  request.srs_slot_offset = 7U;
  request.srs_slot_period = 80U;

  const ue_index_t ue_index = uint_to_ue_index(9);
  manager.set_digital_slot_intent_from_request(ue_index, request, "loaded_service_calendar");
  manager.mark_slot_update_applied(ue_index, request);

  ntn_resource_audit_report report;
  report.du_index      = uint_to_du_index(0);
  report.cell_index    = to_du_cell_index(1);
  report.pci           = pci_t{1};
  report.generation_id = 12;

  const ntn_resource_audit_decision decision = manager.handle_resource_audit_report(report);
  EXPECT_EQ(decision.nof_mismatches, 1U);
  ASSERT_EQ(decision.repairs.size(), 1U);
  EXPECT_EQ(decision.repairs.front().action, ntn_resource_repair_action::apply_sr_srs_assignment);
  EXPECT_EQ(decision.repairs.front().ue_index, ue_index);
  ASSERT_TRUE(decision.repairs.front().slot_request.has_value());
  EXPECT_EQ(decision.repairs.front().slot_request->sr_slot_offset, std::optional<unsigned>{3U});
}

TEST(ntn_beam_service_resource_manager, service_pair_audit_missing_ul_slot_requests_apply_on_paired_ul_resource)
{
  ntn_beam_service_resource_manager manager;
  ntn_beam_placement_plan           plan;
  plan.assignments.push_back(make_loaded_assignment("CN-BEAM-DL-0001", 1, uint_to_du_index(0)));
  plan.assignments.push_back(make_loaded_assignment("CN-BEAM-UL-0007", 7, uint_to_du_index(0)));

  ntn_digital_service_slot_intent_update update;
  update.ue_index                  = uint_to_ue_index(9);
  update.digital_beam_id           = "CN-BEAM-DL-0001";
  update.service_du_index          = uint_to_du_index(0);
  update.service_cell_index        = to_du_cell_index(1);
  update.service_pci               = pci_t{1};
  update.service_nci               = make_nci(1);
  update.has_service_nci           = true;
  update.uplink_resource_beam_id   = "CN-BEAM-UL-0007";
  update.uplink_resource_du_index  = uint_to_du_index(0);
  update.uplink_resource_cell_index = to_du_cell_index(7);
  update.uplink_resource_pci       = pci_t{7};
  update.uplink_resource_nci       = make_nci(7);
  update.has_uplink_resource_nci   = true;
  update.service_state             = "service_bound";

  const ntn_slot_resource_update_decision set_decision =
      manager.update_digital_service_slot_intent(update, plan);
  ASSERT_EQ(set_decision.action, ntn_slot_resource_update_action::set);
  ASSERT_TRUE(set_decision.request.has_value());
  manager.mark_slot_update_applied(update.ue_index, *set_decision.request);

  ntn_resource_audit_report report;
  report.du_index      = update.uplink_resource_du_index;
  report.cell_index    = to_du_cell_index(7);
  report.pci           = pci_t{7};
  report.generation_id = 42;

  const ntn_resource_audit_decision decision = manager.handle_resource_audit_report(report);
  EXPECT_EQ(decision.nof_mismatches, 1U);
  ASSERT_EQ(decision.repairs.size(), 1U);
  const ntn_resource_repair& repair = decision.repairs.front();
  EXPECT_EQ(repair.action, ntn_resource_repair_action::apply_sr_srs_assignment);
  EXPECT_EQ(repair.ue_index, update.ue_index);
  EXPECT_EQ(repair.du_index, update.uplink_resource_du_index);
  EXPECT_EQ(repair.cell_index, report.cell_index);
  EXPECT_EQ(repair.pci, report.pci);
  EXPECT_EQ(repair.reason, "du_missing_service_pair_ul_sr_srs_assignment");
  EXPECT_EQ(repair.service_beam_id, "CN-BEAM-DL-0001");
  EXPECT_EQ(repair.uplink_resource_beam_id, "CN-BEAM-UL-0007");
  EXPECT_EQ(repair.uplink_resource_nci, make_nci(7));
  EXPECT_EQ(repair.uplink_resource_du_index, uint_to_du_index(0));
  ASSERT_TRUE(repair.slot_request.has_value());
  EXPECT_EQ(repair.slot_request->sr_slot_offset, std::optional<unsigned>{3U});
}

TEST(ntn_beam_service_resource_manager, service_pair_audit_does_not_misrepair_from_downlink_anchor_cell_report)
{
  ntn_beam_service_resource_manager manager;
  ntn_beam_placement_plan           plan;
  plan.assignments.push_back(make_loaded_assignment("CN-BEAM-DL-0001", 1, uint_to_du_index(0)));
  plan.assignments.push_back(make_loaded_assignment("CN-BEAM-UL-0007", 7, uint_to_du_index(0)));

  ntn_digital_service_slot_intent_update update;
  update.ue_index                  = uint_to_ue_index(9);
  update.digital_beam_id           = "CN-BEAM-DL-0001";
  update.service_du_index          = uint_to_du_index(0);
  update.service_cell_index        = to_du_cell_index(1);
  update.service_pci               = pci_t{1};
  update.service_nci               = make_nci(1);
  update.has_service_nci           = true;
  update.uplink_resource_beam_id   = "CN-BEAM-UL-0007";
  update.uplink_resource_du_index  = uint_to_du_index(0);
  update.uplink_resource_cell_index = to_du_cell_index(7);
  update.uplink_resource_pci       = pci_t{7};
  update.uplink_resource_nci       = make_nci(7);
  update.has_uplink_resource_nci   = true;
  update.service_state             = "service_bound";

  const ntn_slot_resource_update_decision set_decision =
      manager.update_digital_service_slot_intent(update, plan);
  ASSERT_EQ(set_decision.action, ntn_slot_resource_update_action::set);
  ASSERT_TRUE(set_decision.request.has_value());
  manager.mark_slot_update_applied(update.ue_index, *set_decision.request);

  ntn_resource_audit_report downlink_report;
  downlink_report.du_index      = update.service_du_index;
  downlink_report.cell_index    = to_du_cell_index(1);
  downlink_report.pci           = pci_t{1};
  downlink_report.generation_id = 43;

  const ntn_resource_audit_decision decision = manager.handle_resource_audit_report(downlink_report);
  EXPECT_EQ(decision.nof_mismatches, 0U);
  EXPECT_TRUE(decision.repairs.empty());
}

TEST(ntn_beam_service_resource_manager, service_pair_audit_unknown_ul_slot_requests_clear_with_pair_context)
{
  ntn_beam_service_resource_manager manager;

  ntn_resource_audit_report report;
  report.du_index      = uint_to_du_index(0);
  report.cell_index    = to_du_cell_index(7);
  report.pci           = pci_t{7};
  report.generation_id = 44;

  ntn_resource_audit_ue_slot du_slot;
  du_slot.ue_index = uint_to_ue_index(7);
  du_slot.state    = "applied_by_du";
  du_slot.request.sr_slot_offset  = 5U;
  du_slot.request.sr_slot_period  = 40U;
  du_slot.request.srs_slot_offset = 9U;
  du_slot.request.srs_slot_period = 80U;
  report.ue_slots.push_back(du_slot);

  const ntn_resource_audit_decision decision = manager.handle_resource_audit_report(report);
  EXPECT_EQ(decision.nof_mismatches, 1U);
  ASSERT_EQ(decision.repairs.size(), 1U);
  const ntn_resource_repair& repair = decision.repairs.front();
  EXPECT_EQ(repair.action, ntn_resource_repair_action::clear_unknown_sr_srs_assignment);
  EXPECT_EQ(repair.ue_index, du_slot.ue_index);
  EXPECT_EQ(repair.reason, "du_unknown_sr_srs_assignment");
  EXPECT_EQ(repair.uplink_resource_du_index, report.du_index);
}

TEST(ntn_beam_service_resource_manager, repair_lifecycle_tracks_queued_sent_and_applied_states)
{
  ntn_beam_service_resource_manager manager;

  ntn_resource_repair repair;
  repair.action         = ntn_resource_repair_action::resend_rnti_lease_pool;
  repair.du_index       = uint_to_du_index(0);
  repair.cell_index     = to_du_cell_index(1);
  repair.pci            = pci_t{1};
  repair.analog_beam_id = "ANALOG-ACCESS-001";
  repair.reason         = "du_missing_applied_rnti_pool";
  repair.rnti_leases    = {to_rnti(0x4701), to_rnti(0x4702)};

  const ntn_resource_repair_record queued = manager.queue_resource_repair(repair, 31);
  EXPECT_EQ(queued.state, "queued");
  EXPECT_EQ(queued.retry_count, 0U);

  manager.mark_resource_repair_sent(repair);
  ntn_beam_service_resource_snapshot snapshot = manager.get_snapshot();
  ASSERT_EQ(snapshot.resource_repairs.size(), 1U);
  EXPECT_EQ(snapshot.resource_repairs.front().state, "sent");
  EXPECT_EQ(snapshot.nof_resource_repairs_sent, 1U);

  manager.mark_resource_repair_result(repair, true, "du_ack");
  snapshot = manager.get_snapshot();
  ASSERT_EQ(snapshot.resource_repairs.size(), 1U);
  EXPECT_EQ(snapshot.resource_repairs.front().state, "applied");
  EXPECT_EQ(snapshot.resource_repairs.front().reason, "du_ack");
  EXPECT_EQ(snapshot.nof_resource_repairs_applied, 1U);
}

TEST(ntn_beam_service_resource_manager, repair_lifecycle_allows_only_one_retry_before_exhaustion)
{
  ntn_beam_service_resource_manager manager;

  ntn_resource_repair repair;
  repair.action       = ntn_resource_repair_action::clear_unknown_sr_srs_assignment;
  repair.ue_index     = uint_to_ue_index(7);
  repair.du_index     = uint_to_du_index(0);
  repair.cell_index   = to_du_cell_index(1);
  repair.pci          = pci_t{1};
  repair.reason       = "du_unknown_sr_srs_assignment";
  repair.slot_request = f1ap_ntn_ul_slot_resource_request{};

  EXPECT_EQ(manager.queue_resource_repair(repair, 32).state, "queued");
  manager.mark_resource_repair_sent(repair);
  manager.mark_resource_repair_result(repair, false, "du_reject");
  EXPECT_EQ(manager.queue_resource_repair(repair, 33).retry_count, 1U);
  manager.mark_resource_repair_sent(repair);
  manager.mark_resource_repair_result(repair, false, "du_reject_again");

  const ntn_beam_service_resource_snapshot snapshot = manager.get_snapshot();
  ASSERT_EQ(snapshot.resource_repairs.size(), 1U);
  EXPECT_EQ(snapshot.resource_repairs.front().state, "retry_exhausted");
  EXPECT_EQ(snapshot.resource_repairs.front().retry_count, 1U);
  EXPECT_EQ(snapshot.resource_repairs.front().reason, "du_reject_again");
  EXPECT_EQ(snapshot.nof_resource_repairs_retry_exhausted, 1U);
}

TEST(ntn_beam_service_resource_manager, exhausted_or_conflict_repair_blocks_new_ntn_demand)
{
  ntn_beam_service_resource_manager manager;
  EXPECT_FALSE(manager.has_blocking_resource_repair());

  ntn_resource_repair repair;
  repair.action       = ntn_resource_repair_action::clear_unknown_sr_srs_assignment;
  repair.ue_index     = uint_to_ue_index(7);
  repair.du_index     = uint_to_du_index(0);
  repair.cell_index   = to_du_cell_index(1);
  repair.pci          = pci_t{1};
  repair.reason       = "du_unknown_sr_srs_assignment";
  repair.slot_request = f1ap_ntn_ul_slot_resource_request{};

  manager.queue_resource_repair(repair, 40);
  manager.mark_resource_repair_sent(repair);
  manager.mark_resource_repair_result(repair, false, "du_reject");
  EXPECT_FALSE(manager.has_blocking_resource_repair());

  manager.queue_resource_repair(repair, 41);
  manager.mark_resource_repair_sent(repair);
  manager.mark_resource_repair_result(repair, false, "du_reject_again");
  EXPECT_TRUE(manager.has_blocking_resource_repair());

  ntn_beam_service_resource_manager conflict_manager;
  ntn_resource_repair conflict;
  conflict.action = ntn_resource_repair_action::mark_resource_conflict;
  conflict.reason = "committed_rnti_mismatch";
  conflict_manager.queue_resource_repair(conflict, 42);
  EXPECT_TRUE(conflict_manager.has_blocking_resource_repair());
}
