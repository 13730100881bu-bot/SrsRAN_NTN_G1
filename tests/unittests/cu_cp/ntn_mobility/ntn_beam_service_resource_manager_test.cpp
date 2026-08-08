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
  lease_pool.generation_id  = 1;
  lease_pool.leases.push_back(to_rnti(0x4701));
  ASSERT_TRUE(manager.reserve_rnti_leases(lease_pool).accepted);
  f1ap_ntn_rnti_lease_pool_result lease_result;
  lease_result.generation_id   = lease_pool.generation_id;
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

TEST(ntn_beam_service_resource_manager, malformed_rnti_ack_is_rejected_atomically_and_can_be_repaired)
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

  EXPECT_FALSE(manager.mark_rnti_lease_pool_distribution_result(lease_pool, result));
  snapshot = manager.get_snapshot();
  EXPECT_EQ(snapshot.nof_rnti_leases_sent_to_du, 2U);
  EXPECT_EQ(snapshot.nof_rnti_leases_applied_by_du, 0U);
  EXPECT_EQ(snapshot.nof_rnti_leases_rejected_by_du, 0U);

  result.accepted        = true;
  result.accepted_leases = {to_rnti(0x4701), to_rnti(0x4701)};
  result.rejected_leases.clear();
  EXPECT_FALSE(manager.mark_rnti_lease_pool_distribution_result(lease_pool, result));
  manager.mark_rnti_lease_pool_ack_unknown(lease_pool, "invalid_du_result");

  ntn_resource_audit_report report;
  report.du_index               = lease_pool.du_index;
  report.cell_index             = lease_pool.cell_index;
  report.pci                    = lease_pool.pci;
  report.generation_id          = 88;
  report.rnti_snapshot_complete = true;
  const ntn_resource_audit_decision decision = manager.handle_resource_audit_report(report);
  ASSERT_EQ(decision.repairs.size(), 1U);
  EXPECT_EQ(decision.repairs.front().rnti_lease_generation_id, lease_pool.generation_id);
  EXPECT_EQ(decision.repairs.front().rnti_leases, lease_pool.leases);

  result.accepted = false;
  result.accepted_leases.clear();
  result.rejected_leases = lease_pool.leases;
  EXPECT_TRUE(manager.mark_rnti_lease_pool_distribution_result(lease_pool, result));
  snapshot = manager.get_snapshot();
  EXPECT_EQ(snapshot.nof_rnti_leases_sent_to_du, 0U);
  EXPECT_EQ(snapshot.nof_rnti_leases_rejected_by_du, 2U);
}

TEST(ntn_beam_service_resource_manager, stale_rnti_ack_generation_does_not_override_in_flight_pool)
{
  ntn_beam_service_resource_manager manager;
  const ntn_rnti_lease_pool_update  lease_pool = make_target_handover_lease_pool();
  ASSERT_TRUE(manager.reserve_rnti_leases(lease_pool).accepted);
  manager.mark_rnti_lease_pool_sent_to_du(lease_pool);

  f1ap_ntn_rnti_lease_pool_result stale_result;
  stale_result.generation_id   = lease_pool.generation_id + 1;
  stale_result.accepted        = true;
  stale_result.accepted_leases = lease_pool.leases;
  EXPECT_FALSE(manager.mark_rnti_lease_pool_distribution_result(lease_pool, stale_result));

  const auto snapshot = manager.get_snapshot();
  EXPECT_EQ(snapshot.nof_rnti_leases_sent_to_du, lease_pool.leases.size());
  EXPECT_EQ(snapshot.nof_rnti_leases_applied_by_du, 0U);
  for (const auto& lease : snapshot.rnti_leases) {
    EXPECT_EQ(lease.generation_id, lease_pool.generation_id);
  }
}

TEST(ntn_beam_service_resource_manager, in_flight_pool_waits_for_ack_but_ack_unknown_pool_uses_same_generation_repair)
{
  ntn_beam_service_resource_manager manager;
  const ntn_rnti_lease_pool_update  lease_pool = make_target_handover_lease_pool();
  ASSERT_TRUE(manager.reserve_rnti_leases(lease_pool).accepted);
  EXPECT_TRUE(manager.has_unresolved_rnti_lease_pool(
      lease_pool.du_index, lease_pool.cell_index, lease_pool.pci, lease_pool.analog_beam_id));
  manager.mark_rnti_lease_pool_sent_to_du(lease_pool);

  ntn_resource_audit_report report;
  report.du_index               = lease_pool.du_index;
  report.cell_index             = lease_pool.cell_index;
  report.pci                    = lease_pool.pci;
  report.generation_id          = 100;
  report.rnti_snapshot_complete = true;

  EXPECT_TRUE(manager.handle_resource_audit_report(report).repairs.empty());

  manager.mark_rnti_lease_pool_ack_unknown(lease_pool, "du_response_missing");
  EXPECT_TRUE(manager.has_unresolved_rnti_lease_pool(
      lease_pool.du_index, lease_pool.cell_index, lease_pool.pci, lease_pool.analog_beam_id));
  const ntn_resource_audit_decision repair_decision = manager.handle_resource_audit_report(report);
  ASSERT_EQ(repair_decision.repairs.size(), 1U);
  EXPECT_EQ(repair_decision.repairs.front().action, ntn_resource_repair_action::resend_rnti_lease_pool);
  EXPECT_EQ(repair_decision.repairs.front().reason, "du_missing_ack_unknown_rnti_pool");
  EXPECT_EQ(repair_decision.repairs.front().rnti_lease_generation_id, lease_pool.generation_id);
  EXPECT_EQ(repair_decision.repairs.front().rnti_leases, lease_pool.leases);

  for (rnti_t rnti : lease_pool.leases) {
    report.rnti_leases.push_back({rnti, "pending", "applied_by_du", lease_pool.generation_id});
  }
  EXPECT_TRUE(manager.handle_resource_audit_report(report).repairs.empty());
  const auto snapshot = manager.get_snapshot();
  EXPECT_EQ(snapshot.nof_rnti_leases_sent_to_du, 0U);
  EXPECT_EQ(snapshot.nof_rnti_leases_applied_by_du, lease_pool.leases.size());
  EXPECT_FALSE(manager.has_unresolved_rnti_lease_pool(
      lease_pool.du_index, lease_pool.cell_index, lease_pool.pci, lease_pool.analog_beam_id));
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

TEST(ntn_beam_service_resource_manager, same_du_cells_cannot_reuse_rnti_even_when_pci_is_shared)
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
  const ntn_rnti_lease_pool_update_result result = manager.reserve_rnti_leases(second_pool);
  EXPECT_FALSE(result.accepted);
  EXPECT_EQ(result.state, "conflict");
  EXPECT_EQ(result.reason, "duplicate_lease");
}

TEST(ntn_beam_service_resource_manager, different_dus_can_reuse_same_cell_scoped_rnti)
{
  ntn_beam_service_resource_manager manager;

  ntn_rnti_lease_pool_update first_pool;
  first_pool.du_index       = uint_to_du_index(0);
  first_pool.cell_index     = to_du_cell_index(1);
  first_pool.pci            = pci_t{101};
  first_pool.analog_beam_id = "ANALOG-ACCESS-001";
  first_pool.generation_id  = 101;
  first_pool.leases         = {to_rnti(0x4701)};
  ASSERT_TRUE(manager.reserve_rnti_leases(first_pool).accepted);

  ntn_rnti_lease_pool_update second_pool = first_pool;
  second_pool.du_index                   = uint_to_du_index(1);
  second_pool.generation_id              = 102;
  EXPECT_TRUE(manager.reserve_rnti_leases(second_pool).accepted);
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
  report.rnti_snapshot_complete = true;

  const ntn_resource_audit_decision decision = manager.handle_resource_audit_report(report);
  EXPECT_EQ(decision.nof_mismatches, 1U);
  ASSERT_EQ(decision.repairs.size(), 1U);
  EXPECT_EQ(decision.repairs.front().action, ntn_resource_repair_action::resend_rnti_lease_pool);
  EXPECT_EQ(decision.repairs.front().reason, "du_missing_applied_rnti_pool");
}

TEST(ntn_beam_service_resource_manager, when_audit_domain_is_incomplete_then_empty_snapshot_does_not_trigger_repair)
{
  ntn_beam_service_resource_manager manager;
  const ntn_rnti_lease_pool_update  lease_pool = make_target_handover_lease_pool();
  apply_lease_pool(manager, lease_pool);

  f1ap_ntn_ul_slot_resource_request slot_request;
  slot_request.sr_slot_offset = 3U;
  slot_request.sr_slot_period = 40U;
  manager.set_digital_slot_intent_from_request(uint_to_ue_index(9), slot_request, "loaded_service_calendar");
  manager.mark_slot_update_applied(uint_to_ue_index(9), slot_request);

  ntn_resource_audit_report report;
  report.du_index      = lease_pool.du_index;
  report.cell_index    = lease_pool.cell_index;
  report.pci           = lease_pool.pci;
  report.generation_id = 11;

  const ntn_resource_audit_decision decision = manager.handle_resource_audit_report(report);
  EXPECT_EQ(decision.nof_mismatches, 0U);
  EXPECT_TRUE(decision.repairs.empty());
}

TEST(ntn_beam_service_resource_manager, when_du_reports_pending_and_consumed_leases_then_state_is_reconciled)
{
  ntn_beam_service_resource_manager manager;
  manager.set_authoritative_rnti_lease_validation_enabled(true);

  ntn_rnti_lease_pool_update lease_pool;
  lease_pool.du_index       = uint_to_du_index(0);
  lease_pool.cell_index     = to_du_cell_index(1);
  lease_pool.pci            = pci_t{101};
  lease_pool.analog_beam_id = "ANALOG-ACCESS-001";
  lease_pool.generation_id  = 77;
  lease_pool.leases         = {to_rnti(0x4701), to_rnti(0x4702)};
  apply_lease_pool(manager, lease_pool);

  ntn_resource_audit_report report;
  report.du_index               = lease_pool.du_index;
  report.cell_index             = lease_pool.cell_index;
  report.pci                    = lease_pool.pci;
  report.generation_id          = 12;
  report.rnti_snapshot_complete = true;
  report.rnti_leases.push_back({lease_pool.leases[0], "consumed_by_mac", "applied_by_du", lease_pool.generation_id});
  report.rnti_leases.push_back({lease_pool.leases[1], "pending", "applied_by_du", lease_pool.generation_id});

  const ntn_resource_audit_decision decision = manager.handle_resource_audit_report(report);
  EXPECT_TRUE(decision.repairs.empty());

  const ntn_beam_service_resource_snapshot snapshot = manager.get_snapshot();
  EXPECT_EQ(snapshot.nof_rnti_leases_consumed_by_du, 1U);
  EXPECT_EQ(snapshot.nof_rnti_leases_available, 1U);
  const auto consumed_it = std::find_if(snapshot.rnti_leases.begin(),
                                        snapshot.rnti_leases.end(),
                                        [](const auto& lease) { return lease.rnti == to_rnti(0x4701); });
  ASSERT_NE(consumed_it, snapshot.rnti_leases.end());
  EXPECT_EQ(consumed_it->state, "consumed_by_du");
  EXPECT_EQ(consumed_it->reason, "du_audit_consumed_by_mac");

  ntn_access_rnti_ownership_update ownership;
  ownership.ue_index       = uint_to_ue_index(9);
  ownership.du_index       = lease_pool.du_index;
  ownership.cell_index     = lease_pool.cell_index;
  ownership.pci            = lease_pool.pci;
  ownership.rnti           = lease_pool.leases[0];
  ownership.analog_beam_id = lease_pool.analog_beam_id;
  EXPECT_TRUE(manager.register_access_rnti_ownership(ownership).accepted);

  ntn_resource_audit_report after_initial_ul = report;
  after_initial_ul.rnti_leases.clear();
  after_initial_ul.rnti_leases.push_back({lease_pool.leases[1], "pending", "applied_by_du", lease_pool.generation_id});
  EXPECT_TRUE(manager.handle_resource_audit_report(after_initial_ul).repairs.empty());
}

TEST(ntn_beam_service_resource_manager, when_du_reports_expired_lease_then_same_rnti_is_not_reinserted)
{
  ntn_beam_service_resource_manager manager;
  manager.set_authoritative_rnti_lease_validation_enabled(true);
  const ntn_rnti_lease_pool_update lease_pool = make_target_handover_lease_pool();
  apply_lease_pool(manager, lease_pool);

  ntn_resource_audit_report report;
  report.du_index               = lease_pool.du_index;
  report.cell_index             = lease_pool.cell_index;
  report.pci                    = lease_pool.pci;
  report.generation_id          = 13;
  report.rnti_snapshot_complete = true;
  for (rnti_t rnti : lease_pool.leases) {
    report.rnti_leases.push_back({rnti, "expired", "expired_by_du", lease_pool.generation_id});
  }

  const ntn_resource_audit_decision decision = manager.handle_resource_audit_report(report);
  EXPECT_TRUE(decision.repairs.empty());
  const ntn_beam_service_resource_snapshot snapshot = manager.get_snapshot();
  EXPECT_EQ(snapshot.nof_rnti_leases_expired, lease_pool.leases.size());
  EXPECT_EQ(snapshot.nof_rnti_leases_available, 0U);

  ntn_access_rnti_ownership_update ownership;
  ownership.ue_index                            = uint_to_ue_index(9);
  ownership.du_index                            = lease_pool.du_index;
  ownership.cell_index                          = lease_pool.cell_index;
  ownership.pci                                 = lease_pool.pci;
  ownership.rnti                                = lease_pool.leases.front();
  ownership.analog_beam_id                      = lease_pool.analog_beam_id;
  const ntn_access_rnti_ownership_result access = manager.register_access_rnti_ownership(ownership);
  EXPECT_FALSE(access.accepted);
  EXPECT_EQ(access.reason, "expired_rnti");
}

TEST(ntn_beam_service_resource_manager, when_complete_snapshot_has_duplicate_rnti_then_resource_domain_is_blocked)
{
  ntn_beam_service_resource_manager manager;
  const ntn_rnti_lease_pool_update  lease_pool = make_target_handover_lease_pool();
  apply_lease_pool(manager, lease_pool);

  ntn_resource_audit_report report;
  report.du_index               = lease_pool.du_index;
  report.cell_index             = lease_pool.cell_index;
  report.pci                    = lease_pool.pci;
  report.generation_id          = 14;
  report.rnti_snapshot_complete = true;
  report.rnti_leases.push_back({lease_pool.leases.front(), "pending", "applied_by_du", lease_pool.generation_id});
  report.rnti_leases.push_back({lease_pool.leases.front(), "pending", "applied_by_du", lease_pool.generation_id});

  const ntn_resource_audit_decision decision = manager.handle_resource_audit_report(report);
  ASSERT_EQ(decision.repairs.size(), 1U);
  EXPECT_EQ(decision.repairs.front().action, ntn_resource_repair_action::mark_resource_conflict);
  EXPECT_EQ(decision.repairs.front().reason, "duplicate_du_rnti_snapshot");
}

TEST(ntn_beam_service_resource_manager, when_complete_snapshot_has_unknown_rnti_then_it_is_quarantined)
{
  ntn_beam_service_resource_manager manager;
  const ntn_rnti_lease_pool_update  lease_pool = make_target_handover_lease_pool();
  apply_lease_pool(manager, lease_pool);

  ntn_resource_audit_report report;
  report.du_index               = lease_pool.du_index;
  report.cell_index             = lease_pool.cell_index;
  report.pci                    = lease_pool.pci;
  report.generation_id          = 15;
  report.rnti_snapshot_complete = true;
  report.du_connection_generation         = 7;
  report.rnti_retirement_capability_known = true;
  report.rnti_retirement_supported        = true;
  report.rnti_generation_high_water       = lease_pool.generation_id;
  report.rnti_leases.push_back({lease_pool.leases.front(), "pending", "applied_by_du", lease_pool.generation_id});
  report.rnti_leases.push_back({lease_pool.leases.back(), "pending", "applied_by_du", lease_pool.generation_id});
  report.rnti_leases.push_back({to_rnti(0x5001), "pending", "applied_by_du", lease_pool.generation_id});

  const ntn_resource_audit_decision decision = manager.handle_resource_audit_report(report);
  EXPECT_TRUE(decision.repairs.empty());
  const auto snapshot = manager.get_snapshot();
  EXPECT_EQ(snapshot.nof_rnti_orphans_quarantined, 1U);
  EXPECT_EQ(snapshot.nof_rnti_orphans_observed, 1U);
  const auto orphan = std::find_if(snapshot.rnti_leases.begin(), snapshot.rnti_leases.end(), [](const auto& lease) {
    return lease.rnti == to_rnti(0x5001);
  });
  ASSERT_NE(orphan, snapshot.rnti_leases.end());
  EXPECT_EQ(orphan->state, "orphan_quarantined");
  EXPECT_TRUE(orphan->analog_beam_id.empty());
  EXPECT_TRUE(manager.is_rnti_excluded_for_du(report.du_index, orphan->rnti));
}

TEST(ntn_beam_service_resource_manager, expired_orphan_rnti_is_grouped_for_atomic_retirement)
{
  ntn_beam_service_resource_manager manager;

  ntn_resource_audit_report report;
  report.du_index                         = uint_to_du_index(3);
  report.cell_index                       = to_du_cell_index(4);
  report.pci                              = pci_t{44};
  report.generation_id                    = 91;
  report.rnti_snapshot_complete           = true;
  report.du_connection_generation         = 12;
  report.rnti_retirement_capability_known = true;
  report.rnti_retirement_supported        = true;
  report.rnti_generation_high_water       = 501;
  report.rnti_leases.push_back({to_rnti(0x5102), "expired", "expired_by_du", 501});
  report.rnti_leases.push_back({to_rnti(0x5101), "expired", "expired_by_du", 501});

  EXPECT_TRUE(manager.handle_resource_audit_report(report).repairs.empty());
  const auto snapshot = manager.get_snapshot();
  EXPECT_EQ(snapshot.nof_rnti_orphans_quarantined, 2U);
  EXPECT_EQ(snapshot.nof_rnti_leases_retire_pending, 2U);
  ASSERT_EQ(snapshot.rnti_retirement_du_statuses.size(), 1U);
  EXPECT_TRUE(snapshot.rnti_retirement_du_statuses.front().supported);
  EXPECT_EQ(snapshot.rnti_retirement_du_statuses.front().generation_high_water, 501U);

  const auto batches = manager.get_pending_rnti_retirement_batches();
  ASSERT_EQ(batches.size(), 1U);
  EXPECT_EQ(batches.front().du_index, report.du_index);
  EXPECT_EQ(batches.front().cell_index, report.cell_index);
  EXPECT_EQ(batches.front().pci, report.pci);
  EXPECT_EQ(batches.front().generation_id, 501U);
  EXPECT_EQ(batches.front().du_connection_generation, 12U);
  EXPECT_EQ(batches.front().leases, (std::vector<rnti_t>{to_rnti(0x5101), to_rnti(0x5102)}));
}

TEST(ntn_beam_service_resource_manager, rnti_retirement_is_disabled_when_du_does_not_advertise_support)
{
  ntn_beam_service_resource_manager manager;

  ntn_resource_audit_report report;
  report.du_index                         = uint_to_du_index(3);
  report.cell_index                       = to_du_cell_index(4);
  report.pci                              = pci_t{45};
  report.generation_id                    = 911;
  report.rnti_snapshot_complete           = true;
  report.du_connection_generation         = 12;
  report.rnti_retirement_capability_known = true;
  report.rnti_retirement_supported        = false;
  report.rnti_generation_high_water       = 502;
  report.rnti_leases.push_back({to_rnti(0x5111), "expired", "expired_by_du", 502});

  EXPECT_TRUE(manager.handle_resource_audit_report(report).repairs.empty());
  EXPECT_TRUE(manager.get_pending_rnti_retirement_batches().empty());
  const auto snapshot = manager.get_snapshot();
  ASSERT_EQ(snapshot.rnti_leases.size(), 1U);
  EXPECT_EQ(snapshot.rnti_leases.front().state, "orphan_quarantined");
  ASSERT_EQ(snapshot.rnti_retirement_du_statuses.size(), 1U);
  EXPECT_TRUE(snapshot.rnti_retirement_du_statuses.front().capability_known);
  EXPECT_FALSE(snapshot.rnti_retirement_du_statuses.front().supported);
}

TEST(ntn_beam_service_resource_manager, released_and_unused_rntis_retire_after_one_complete_du_audit)
{
  ntn_beam_service_resource_manager manager;
  manager.set_authoritative_rnti_lease_validation_enabled(true);

  ntn_rnti_lease_pool_update lease_pool;
  lease_pool.du_index       = uint_to_du_index(0);
  lease_pool.cell_index     = to_du_cell_index(1);
  lease_pool.pci            = pci_t{71};
  lease_pool.analog_beam_id = "ANALOG-ACCESS-071";
  lease_pool.generation_id  = 601;
  lease_pool.leases         = {to_rnti(0x5201), to_rnti(0x5202)};
  apply_lease_pool(manager, lease_pool);

  ntn_access_rnti_ownership_update ownership;
  ownership.ue_index       = uint_to_ue_index(31);
  ownership.du_index       = lease_pool.du_index;
  ownership.cell_index     = lease_pool.cell_index;
  ownership.pci            = lease_pool.pci;
  ownership.rnti           = lease_pool.leases.front();
  ownership.analog_beam_id = lease_pool.analog_beam_id;
  ASSERT_TRUE(manager.register_access_rnti_ownership(ownership).accepted);
  manager.remove_ue(ownership.ue_index);

  ntn_resource_audit_report report;
  report.du_index                         = lease_pool.du_index;
  report.cell_index                       = lease_pool.cell_index;
  report.pci                              = lease_pool.pci;
  report.generation_id                    = 92;
  report.rnti_snapshot_complete           = true;
  report.du_connection_generation         = 13;
  report.rnti_retirement_capability_known = true;
  report.rnti_retirement_supported        = true;
  report.rnti_generation_high_water       = lease_pool.generation_id;
  for (rnti_t rnti : lease_pool.leases) {
    report.rnti_leases.push_back({rnti, "expired", "expired_by_du", lease_pool.generation_id});
  }

  EXPECT_TRUE(manager.handle_resource_audit_report(report).repairs.empty());
  const auto batches = manager.get_pending_rnti_retirement_batches();
  ASSERT_EQ(batches.size(), 1U);
  EXPECT_EQ(batches.front().leases, lease_pool.leases);

  const auto snapshot = manager.get_snapshot();
  EXPECT_EQ(snapshot.nof_rnti_leases_retire_pending, 2U);
  EXPECT_EQ(snapshot.nof_rnti_leases_expired, 0U);
}

TEST(ntn_beam_service_resource_manager, active_access_and_handover_rntis_are_never_retired_from_an_expired_du_record)
{
  ntn_beam_service_resource_manager manager;
  manager.set_authoritative_rnti_lease_validation_enabled(true);

  ntn_rnti_lease_pool_update lease_pool;
  lease_pool.du_index       = uint_to_du_index(0);
  lease_pool.cell_index     = to_du_cell_index(1);
  lease_pool.pci            = pci_t{74};
  lease_pool.analog_beam_id = "ANALOG-ACCESS-074";
  lease_pool.generation_id  = 650;
  lease_pool.leases         = {to_rnti(0x5251), to_rnti(0x5252), to_rnti(0x5253), to_rnti(0x5254), to_rnti(0x5255)};
  apply_lease_pool(manager, lease_pool);

  ASSERT_TRUE(
      manager.mark_rnti_offered_in_rar(lease_pool.du_index, lease_pool.cell_index, lease_pool.pci, lease_pool.leases[0])
          .accepted);

  const auto register_access = [&](ue_index_t ue_index, rnti_t rnti) {
    ntn_access_rnti_ownership_update ownership;
    ownership.ue_index       = ue_index;
    ownership.du_index       = lease_pool.du_index;
    ownership.cell_index     = lease_pool.cell_index;
    ownership.pci            = lease_pool.pci;
    ownership.rnti           = rnti;
    ownership.analog_beam_id = lease_pool.analog_beam_id;
    return manager.register_access_rnti_ownership(ownership);
  };
  ASSERT_TRUE(register_access(uint_to_ue_index(41), lease_pool.leases[1]).accepted);
  ASSERT_TRUE(register_access(uint_to_ue_index(42), lease_pool.leases[2]).accepted);
  manager.release_analog_access_after_ics(uint_to_ue_index(42));
  ASSERT_TRUE(manager
                  .reserve_handover_target_rnti(uint_to_ue_index(43),
                                                lease_pool.du_index,
                                                lease_pool.cell_index,
                                                lease_pool.pci,
                                                lease_pool.analog_beam_id)
                  .accepted);
  EXPECT_FALSE(
      manager.commit_handover_target_rnti(uint_to_ue_index(43), uint_to_ue_index(44), lease_pool.leases[4]).accepted);
  ASSERT_TRUE(manager
                  .reserve_handover_target_rnti(uint_to_ue_index(45),
                                                lease_pool.du_index,
                                                lease_pool.cell_index,
                                                lease_pool.pci,
                                                lease_pool.analog_beam_id)
                  .accepted);
  manager.remove_ue(uint_to_ue_index(43));
  manager.remove_ue(uint_to_ue_index(45));

  ntn_resource_audit_report report;
  report.du_index                         = lease_pool.du_index;
  report.cell_index                       = lease_pool.cell_index;
  report.pci                              = lease_pool.pci;
  report.generation_id                    = 96;
  report.rnti_snapshot_complete           = true;
  report.du_connection_generation         = 15;
  report.rnti_retirement_capability_known = true;
  report.rnti_retirement_supported        = true;
  report.rnti_generation_high_water       = lease_pool.generation_id;
  for (rnti_t rnti : lease_pool.leases) {
    report.rnti_leases.push_back({rnti, "expired", "expired_by_du", lease_pool.generation_id});
  }

  EXPECT_TRUE(manager.handle_resource_audit_report(report).repairs.empty());
  EXPECT_TRUE(manager.get_pending_rnti_retirement_batches().empty());
  const auto snapshot = manager.get_snapshot();
  EXPECT_EQ(snapshot.nof_rnti_leases_offered_in_rar, 1U);
  EXPECT_EQ(snapshot.nof_rnti_leases_initial_ul_seen, 1U);
  EXPECT_EQ(snapshot.nof_rnti_leases_committed, 1U);
  EXPECT_EQ(snapshot.nof_rnti_leases_conflict, 1U);
  EXPECT_EQ(std::count_if(snapshot.rnti_leases.begin(),
                          snapshot.rnti_leases.end(),
                          [](const auto& lease) { return lease.state == "handover_reserved"; }),
            1);
}

TEST(ntn_beam_service_resource_manager,
     rnti_retirement_waits_for_new_audit_after_rejection_then_allows_higher_generation_reuse)
{
  ntn_beam_service_resource_manager manager;
  manager.set_authoritative_rnti_lease_validation_enabled(true);

  ntn_rnti_lease_pool_update lease_pool;
  lease_pool.du_index       = uint_to_du_index(0);
  lease_pool.cell_index     = to_du_cell_index(1);
  lease_pool.pci            = pci_t{72};
  lease_pool.analog_beam_id = "ANALOG-ACCESS-072";
  lease_pool.generation_id  = 701;
  lease_pool.leases         = {to_rnti(0x5301)};
  apply_lease_pool(manager, lease_pool);

  ntn_access_rnti_ownership_update ownership;
  ownership.ue_index       = uint_to_ue_index(32);
  ownership.du_index       = lease_pool.du_index;
  ownership.cell_index     = lease_pool.cell_index;
  ownership.pci            = lease_pool.pci;
  ownership.rnti           = lease_pool.leases.front();
  ownership.analog_beam_id = lease_pool.analog_beam_id;
  ASSERT_TRUE(manager.register_access_rnti_ownership(ownership).accepted);
  manager.remove_ue(ownership.ue_index);

  ntn_resource_audit_report report;
  report.du_index                         = lease_pool.du_index;
  report.cell_index                       = lease_pool.cell_index;
  report.pci                              = lease_pool.pci;
  report.generation_id                    = 93;
  report.rnti_snapshot_complete           = true;
  report.du_connection_generation         = 14;
  report.rnti_retirement_capability_known = true;
  report.rnti_retirement_supported        = true;
  report.rnti_generation_high_water       = lease_pool.generation_id;
  report.rnti_leases.push_back({lease_pool.leases.front(), "expired", "expired_by_du", lease_pool.generation_id});
  ASSERT_TRUE(manager.handle_resource_audit_report(report).repairs.empty());

  auto batches = manager.get_pending_rnti_retirement_batches();
  ASSERT_EQ(batches.size(), 1U);
  ASSERT_TRUE(manager.mark_rnti_retirement_sent(batches.front()));

  ntn_rnti_retirement_batch partial = batches.front();
  partial.leases.push_back(to_rnti(0x5302));
  EXPECT_FALSE(manager.mark_rnti_retirement_result(partial, ntn_rnti_retirement_outcome::accepted, "malformed_ack"));
  EXPECT_TRUE(manager.mark_rnti_retirement_result(
      batches.front(), ntn_rnti_retirement_outcome::outcome_unknown, "du_response_missing"));
  EXPECT_TRUE(manager.get_pending_rnti_retirement_batches().empty());
  EXPECT_EQ(manager.get_snapshot().nof_rnti_leases_retire_waiting_audit, 1U);

  ++report.generation_id;
  ASSERT_TRUE(manager.handle_resource_audit_report(report).repairs.empty());
  batches = manager.get_pending_rnti_retirement_batches();
  ASSERT_EQ(batches.size(), 1U);
  ASSERT_TRUE(manager.mark_rnti_retirement_sent(batches.front()));
  ASSERT_TRUE(manager.mark_rnti_retirement_result(
      batches.front(), ntn_rnti_retirement_outcome::definitively_rejected, "du_retire_rejected"));
  EXPECT_EQ(manager.get_snapshot().nof_rnti_retirement_rejected, 1U);

  ++report.generation_id;
  ASSERT_TRUE(manager.handle_resource_audit_report(report).repairs.empty());
  batches = manager.get_pending_rnti_retirement_batches();
  ASSERT_EQ(batches.size(), 1U);
  ASSERT_TRUE(manager.mark_rnti_retirement_sent(batches.front()));
  ASSERT_TRUE(
      manager.mark_rnti_retirement_result(batches.front(), ntn_rnti_retirement_outcome::accepted, "du_retire_ack"));
  EXPECT_FALSE(manager.is_rnti_excluded_for_du(lease_pool.du_index, lease_pool.leases.front()));

  ntn_rnti_lease_pool_update stale_reuse  = lease_pool;
  const auto                 stale_result = manager.reserve_rnti_leases(stale_reuse);
  EXPECT_FALSE(stale_result.accepted);
  EXPECT_EQ(stale_result.reason, "stale_generation");

  ntn_rnti_lease_pool_update safe_reuse = lease_pool;
  ++safe_reuse.generation_id;
  EXPECT_TRUE(manager.reserve_rnti_leases(safe_reuse).accepted);
  const auto snapshot = manager.get_snapshot();
  EXPECT_EQ(snapshot.nof_rnti_leases_retired, 1U);
  EXPECT_EQ(snapshot.nof_rnti_leases_reused, 1U);
  EXPECT_EQ(snapshot.nof_rnti_retirement_rejected, 1U);
}

TEST(ntn_beam_service_resource_manager, du_disconnect_invalidates_retirement_and_rejects_old_connection_audit)
{
  ntn_beam_service_resource_manager manager;

  ntn_resource_audit_report report;
  report.du_index                         = uint_to_du_index(4);
  report.cell_index                       = to_du_cell_index(5);
  report.pci                              = pci_t{73};
  report.generation_id                    = 95;
  report.rnti_snapshot_complete           = true;
  report.du_connection_generation         = 20;
  report.rnti_retirement_capability_known = true;
  report.rnti_retirement_supported        = true;
  report.rnti_generation_high_water       = 801;
  report.rnti_leases.push_back({to_rnti(0x5401), "expired", "expired_by_du", 801});
  ASSERT_TRUE(manager.handle_resource_audit_report(report).repairs.empty());
  const auto old_batch = manager.get_pending_rnti_retirement_batches().front();
  ASSERT_TRUE(manager.mark_rnti_retirement_sent(old_batch));

  manager.invalidate_rnti_retirement_for_du(report.du_index);
  EXPECT_FALSE(manager.mark_rnti_retirement_result(old_batch, ntn_rnti_retirement_outcome::accepted, "late_ack"));
  EXPECT_TRUE(manager.get_pending_rnti_retirement_batches().empty());

  const auto stale = manager.handle_resource_audit_report(report);
  ASSERT_EQ(stale.repairs.size(), 1U);
  EXPECT_EQ(stale.repairs.front().reason, "stale_du_connection_generation");

  report.du_connection_generation = 21;
  ++report.generation_id;
  --report.rnti_generation_high_water;
  const auto reconnect_regression = manager.handle_resource_audit_report(report);
  ASSERT_EQ(reconnect_regression.repairs.size(), 1U);
  EXPECT_EQ(reconnect_regression.repairs.front().reason, "rnti_generation_high_water_regressed");

  ++report.rnti_generation_high_water;
  EXPECT_TRUE(manager.handle_resource_audit_report(report).repairs.empty());
  ASSERT_EQ(manager.get_pending_rnti_retirement_batches().size(), 1U);
  EXPECT_EQ(manager.get_pending_rnti_retirement_batches().front().du_connection_generation, 21U);

  ntn_resource_audit_report regressed = report;
  --regressed.rnti_generation_high_water;
  const auto regression = manager.handle_resource_audit_report(regressed);
  ASSERT_EQ(regression.repairs.size(), 1U);
  EXPECT_EQ(regression.repairs.front().reason, "rnti_generation_high_water_regressed");
}

TEST(ntn_beam_service_resource_manager,
     rnti_retirement_incomplete_audit_connection_is_invalidated_before_late_complete_response)
{
  ntn_beam_service_resource_manager manager;

  ntn_resource_audit_report report;
  report.du_index                         = uint_to_du_index(4);
  report.cell_index                       = to_du_cell_index(6);
  report.pci                              = pci_t{74};
  report.generation_id                    = 96;
  report.rnti_snapshot_complete           = false;
  report.du_connection_generation         = 25;
  report.rnti_retirement_capability_known = true;
  report.rnti_retirement_supported        = true;
  report.rnti_generation_high_water       = 850;
  EXPECT_TRUE(manager.handle_resource_audit_report(report).repairs.empty());

  auto snapshot = manager.get_snapshot();
  ASSERT_EQ(snapshot.rnti_retirement_du_statuses.size(), 1U);
  EXPECT_EQ(snapshot.rnti_retirement_du_statuses.front().du_connection_generation, 25U);
  EXPECT_TRUE(snapshot.rnti_retirement_du_statuses.front().capability_known);
  EXPECT_FALSE(snapshot.rnti_retirement_du_statuses.front().complete_audit_seen);

  manager.invalidate_rnti_retirement_for_du(report.du_index);
  report.rnti_snapshot_complete = true;
  ++report.generation_id;
  const auto stale = manager.handle_resource_audit_report(report);
  ASSERT_EQ(stale.repairs.size(), 1U);
  EXPECT_EQ(stale.repairs.front().reason, "stale_du_connection_generation");

  ++report.du_connection_generation;
  ++report.generation_id;
  EXPECT_TRUE(manager.handle_resource_audit_report(report).repairs.empty());
  snapshot = manager.get_snapshot();
  ASSERT_EQ(snapshot.rnti_retirement_du_statuses.size(), 1U);
  EXPECT_EQ(snapshot.rnti_retirement_du_statuses.front().du_connection_generation, 26U);
  EXPECT_TRUE(snapshot.rnti_retirement_du_statuses.front().complete_audit_seen);
  EXPECT_EQ(snapshot.rnti_retirement_du_statuses.front().generation_high_water, 850U);
}

TEST(ntn_beam_service_resource_manager, lost_retirement_ack_is_confirmed_by_absence_after_reconnect)
{
  ntn_beam_service_resource_manager manager;

  ntn_resource_audit_report report;
  report.du_index                         = uint_to_du_index(5);
  report.cell_index                       = to_du_cell_index(6);
  report.pci                              = pci_t{75};
  report.generation_id                    = 101;
  report.rnti_snapshot_complete           = true;
  report.du_connection_generation         = 30;
  report.rnti_retirement_capability_known = true;
  report.rnti_retirement_supported        = true;
  report.rnti_generation_high_water       = 901;
  report.rnti_leases.push_back({to_rnti(0x5501), "expired", "expired_by_du", 901});
  ASSERT_TRUE(manager.handle_resource_audit_report(report).repairs.empty());

  const auto pending_batches = manager.get_pending_rnti_retirement_batches();
  ASSERT_EQ(pending_batches.size(), 1U);
  const ntn_rnti_retirement_batch sent_batch = pending_batches.front();
  ASSERT_TRUE(manager.mark_rnti_retirement_sent(sent_batch));
  ASSERT_TRUE(manager.mark_rnti_retirement_result(
      sent_batch, ntn_rnti_retirement_outcome::outcome_unknown, "retirement_result_timeout"));
  manager.invalidate_rnti_retirement_for_du(report.du_index);

  auto snapshot = manager.get_snapshot();
  ASSERT_EQ(snapshot.rnti_leases.size(), 1U);
  EXPECT_EQ(snapshot.rnti_leases.front().state, "retire_waiting_audit");
  EXPECT_TRUE(manager.is_rnti_excluded_for_du(report.du_index, to_rnti(0x5501)));

  report.rnti_leases.clear();
  const auto stale = manager.handle_resource_audit_report(report);
  ASSERT_EQ(stale.repairs.size(), 1U);
  EXPECT_EQ(stale.repairs.front().reason, "stale_du_connection_generation");
  EXPECT_EQ(manager.get_snapshot().nof_rnti_leases_retired, 0U);

  report.du_connection_generation = 31;
  ++report.generation_id;
  EXPECT_TRUE(manager.handle_resource_audit_report(report).repairs.empty());
  snapshot = manager.get_snapshot();
  EXPECT_TRUE(snapshot.rnti_leases.empty());
  EXPECT_EQ(snapshot.nof_rnti_leases_retired, 1U);
  EXPECT_EQ(snapshot.nof_rnti_orphans_quarantined, 0U);
  EXPECT_EQ(snapshot.rnti_retirement_last_reason, "retirement_confirmed_absent_by_complete_audit");
  EXPECT_FALSE(manager.is_rnti_excluded_for_du(report.du_index, to_rnti(0x5501)));

  ntn_rnti_lease_pool_update reuse;
  reuse.du_index         = report.du_index;
  reuse.cell_index       = report.cell_index;
  reuse.pci              = report.pci;
  reuse.analog_beam_id   = "ANALOG-ACCESS-075";
  reuse.generation_id    = 901;
  reuse.leases           = {to_rnti(0x5501)};
  const auto stale_reuse = manager.reserve_rnti_leases(reuse);
  EXPECT_FALSE(stale_reuse.accepted);
  EXPECT_EQ(stale_reuse.reason, "stale_generation");

  ++reuse.generation_id;
  EXPECT_TRUE(manager.reserve_rnti_leases(reuse).accepted);
  EXPECT_EQ(manager.get_snapshot().nof_rnti_leases_reused, 1U);

  report.du_connection_generation = 30;
  const auto old_connection       = manager.handle_resource_audit_report(report);
  ASSERT_EQ(old_connection.repairs.size(), 1U);
  EXPECT_EQ(old_connection.repairs.front().reason, "stale_du_connection_generation");
}

TEST(ntn_beam_service_resource_manager, partially_absent_retirement_batch_remains_atomically_quarantined)
{
  ntn_beam_service_resource_manager manager;

  ntn_resource_audit_report report;
  report.du_index                         = uint_to_du_index(5);
  report.cell_index                       = to_du_cell_index(7);
  report.pci                              = pci_t{76};
  report.generation_id                    = 102;
  report.rnti_snapshot_complete           = true;
  report.du_connection_generation         = 40;
  report.rnti_retirement_capability_known = true;
  report.rnti_retirement_supported        = true;
  report.rnti_generation_high_water       = 902;
  report.rnti_leases.push_back({to_rnti(0x5511), "expired", "expired_by_du", 902});
  report.rnti_leases.push_back({to_rnti(0x5512), "expired", "expired_by_du", 902});
  ASSERT_TRUE(manager.handle_resource_audit_report(report).repairs.empty());

  const auto pending_batches = manager.get_pending_rnti_retirement_batches();
  ASSERT_EQ(pending_batches.size(), 1U);
  const ntn_rnti_retirement_batch sent_batch = pending_batches.front();
  ASSERT_EQ(sent_batch.leases.size(), 2U);
  ASSERT_TRUE(manager.mark_rnti_retirement_sent(sent_batch));
  manager.invalidate_rnti_retirement_for_du(report.du_index);

  report.du_connection_generation = 41;
  ++report.generation_id;
  report.rnti_leases.erase(report.rnti_leases.begin());
  EXPECT_TRUE(manager.handle_resource_audit_report(report).repairs.empty());

  const auto snapshot = manager.get_snapshot();
  ASSERT_EQ(snapshot.rnti_leases.size(), 2U);
  EXPECT_EQ(snapshot.nof_rnti_leases_retired, 0U);
  EXPECT_EQ(snapshot.nof_rnti_leases_retire_waiting_audit, 2U);
  EXPECT_TRUE(manager.get_pending_rnti_retirement_batches().empty());
  EXPECT_TRUE(manager.is_rnti_excluded_for_du(report.du_index, to_rnti(0x5511)));
  EXPECT_TRUE(manager.is_rnti_excluded_for_du(report.du_index, to_rnti(0x5512)));
  EXPECT_EQ(snapshot.rnti_retirement_last_reason, "partial_retirement_visibility_in_complete_audit");

  report.rnti_leases.clear();
  report.rnti_leases.push_back({to_rnti(0x5511), "expired", "expired_by_du", 902});
  report.rnti_leases.push_back({to_rnti(0x5512), "expired", "expired_by_du", 902});
  ++report.generation_id;
  ASSERT_TRUE(manager.handle_resource_audit_report(report).repairs.empty());
  const auto retry_batches = manager.get_pending_rnti_retirement_batches();
  ASSERT_EQ(retry_batches.size(), 1U);
  ASSERT_EQ(retry_batches.front().leases.size(), 2U);
  ASSERT_TRUE(manager.mark_rnti_retirement_sent(retry_batches.front()));
  ASSERT_TRUE(manager.mark_rnti_retirement_result(
      retry_batches.front(), ntn_rnti_retirement_outcome::accepted, "retry_confirmed"));
  EXPECT_EQ(manager.get_snapshot().nof_rnti_leases_retired, 2U);

  ntn_rnti_lease_pool_update reuse;
  reuse.du_index       = report.du_index;
  reuse.cell_index     = report.cell_index;
  reuse.pci            = report.pci;
  reuse.analog_beam_id = "ANALOG-ACCESS-076";
  reuse.generation_id  = 903;
  reuse.leases         = {to_rnti(0x5511), to_rnti(0x5512)};
  ASSERT_TRUE(manager.reserve_rnti_leases(reuse).accepted);
  manager.mark_rnti_lease_pool_sent_to_du(reuse);

  report.rnti_generation_high_water = reuse.generation_id;
  report.rnti_leases.clear();
  for (rnti_t rnti : reuse.leases) {
    report.rnti_leases.push_back({rnti, "pending", "applied_by_du", reuse.generation_id});
  }
  ++report.generation_id;
  EXPECT_TRUE(manager.handle_resource_audit_report(report).repairs.empty());
  const auto reused_snapshot = manager.get_snapshot();
  EXPECT_EQ(reused_snapshot.nof_rnti_leases_retired, 2U);
  EXPECT_EQ(reused_snapshot.nof_rnti_leases_reused, 2U);
  EXPECT_EQ(reused_snapshot.nof_rnti_leases_retire_waiting_audit, 0U);
  EXPECT_EQ(reused_snapshot.nof_rnti_leases_available, 2U);
}

TEST(ntn_beam_service_resource_manager, unknown_retirement_outcome_is_confirmed_by_same_connection_audit)
{
  ntn_beam_service_resource_manager manager;

  ntn_resource_audit_report report;
  report.du_index                         = uint_to_du_index(5);
  report.cell_index                       = to_du_cell_index(8);
  report.pci                              = pci_t{79};
  report.generation_id                    = 105;
  report.rnti_snapshot_complete           = true;
  report.du_connection_generation         = 70;
  report.rnti_retirement_capability_known = true;
  report.rnti_retirement_supported        = true;
  report.rnti_generation_high_water       = 905;
  report.rnti_leases.push_back({to_rnti(0x5513), "expired", "expired_by_du", 905});
  ASSERT_TRUE(manager.handle_resource_audit_report(report).repairs.empty());

  const auto batches = manager.get_pending_rnti_retirement_batches();
  ASSERT_EQ(batches.size(), 1U);
  ASSERT_TRUE(manager.mark_rnti_retirement_sent(batches.front()));
  ASSERT_TRUE(manager.mark_rnti_retirement_result(
      batches.front(), ntn_rnti_retirement_outcome::outcome_unknown, "retirement_ack_missing"));

  report.rnti_leases.clear();
  ++report.generation_id;
  EXPECT_TRUE(manager.handle_resource_audit_report(report).repairs.empty());
  const auto snapshot = manager.get_snapshot();
  EXPECT_TRUE(snapshot.rnti_leases.empty());
  EXPECT_EQ(snapshot.nof_rnti_leases_retired, 1U);
  EXPECT_EQ(snapshot.rnti_retirement_last_reason, "retirement_confirmed_absent_by_complete_audit");
}

TEST(ntn_beam_service_resource_manager, empty_du_ledger_reset_requires_pool_reinstall_before_retirement_recovery)
{
  ntn_beam_service_resource_manager manager;

  ntn_rnti_lease_pool_update active_pool;
  active_pool.du_index       = uint_to_du_index(8);
  active_pool.cell_index     = to_du_cell_index(10);
  active_pool.pci            = pci_t{80};
  active_pool.analog_beam_id = "ANALOG-ACCESS-080";
  active_pool.generation_id  = 1000;
  active_pool.leases         = {to_rnti(0x5541)};
  apply_lease_pool(manager, active_pool);

  ntn_resource_audit_report report;
  report.du_index                         = active_pool.du_index;
  report.cell_index                       = active_pool.cell_index;
  report.pci                              = active_pool.pci;
  report.generation_id                    = 106;
  report.rnti_snapshot_complete           = true;
  report.du_connection_generation         = 80;
  report.rnti_retirement_capability_known = true;
  report.rnti_retirement_supported        = true;
  report.rnti_generation_high_water       = active_pool.generation_id;
  report.rnti_leases.push_back({active_pool.leases.front(), "pending", "applied_by_du", active_pool.generation_id});
  report.rnti_leases.push_back({to_rnti(0x5542), "expired", "expired_by_du", 999});
  ASSERT_TRUE(manager.handle_resource_audit_report(report).repairs.empty());

  const auto retirement_batches = manager.get_pending_rnti_retirement_batches();
  ASSERT_EQ(retirement_batches.size(), 1U);
  ASSERT_TRUE(manager.mark_rnti_retirement_sent(retirement_batches.front()));
  ASSERT_TRUE(manager.mark_rnti_retirement_result(
      retirement_batches.front(), ntn_rnti_retirement_outcome::outcome_unknown, "retirement_ack_missing"));

  ntn_resource_repair old_repair;
  old_repair.action                   = ntn_resource_repair_action::resend_rnti_lease_pool;
  old_repair.du_index                 = active_pool.du_index;
  old_repair.cell_index               = active_pool.cell_index;
  old_repair.pci                      = active_pool.pci;
  old_repair.analog_beam_id           = active_pool.analog_beam_id;
  old_repair.reason                   = "old_connection_repair";
  old_repair.rnti_lease_generation_id = active_pool.generation_id;
  old_repair.rnti_leases              = active_pool.leases;
  EXPECT_EQ(manager.queue_resource_repair(old_repair, report.generation_id).state, "queued");
  manager.mark_resource_repair_sent(old_repair);

  manager.invalidate_rnti_retirement_for_du(report.du_index);
  auto       snapshot     = manager.get_snapshot();
  const auto active_lease = std::find_if(snapshot.rnti_leases.begin(),
                                         snapshot.rnti_leases.end(),
                                         [&](const auto& lease) { return lease.rnti == active_pool.leases.front(); });
  ASSERT_NE(active_lease, snapshot.rnti_leases.end());
  EXPECT_EQ(active_lease->distribution_state, "sent_to_du");
  EXPECT_EQ(active_lease->distribution_reason, "ack_unknown:du_disconnected");
  EXPECT_FALSE(manager.is_access_rnti_pool_ready(
      active_pool.du_index, active_pool.cell_index, active_pool.pci, active_pool.analog_beam_id));
  ASSERT_EQ(snapshot.resource_repairs.size(), 1U);
  EXPECT_EQ(snapshot.resource_repairs.front().state, "invalidated");

  ntn_resource_audit_report invalid_reset = report;
  invalid_reset.du_connection_generation  = 81;
  ++invalid_reset.generation_id;
  invalid_reset.rnti_generation_high_water = 0;
  const auto nonempty_reset                = manager.handle_resource_audit_report(invalid_reset);
  ASSERT_EQ(nonempty_reset.repairs.size(), 1U);
  EXPECT_EQ(nonempty_reset.repairs.front().reason, "rnti_generation_high_water_regressed");

  invalid_reset.rnti_leases.clear();
  invalid_reset.rnti_generation_high_water = active_pool.generation_id - 1;
  ++invalid_reset.generation_id;
  const auto lower_nonzero = manager.handle_resource_audit_report(invalid_reset);
  ASSERT_EQ(lower_nonzero.repairs.size(), 1U);
  EXPECT_EQ(lower_nonzero.repairs.front().reason, "rnti_generation_high_water_regressed");

  invalid_reset.rnti_generation_high_water = 0;
  ++invalid_reset.generation_id;
  const auto ledger_reset = manager.handle_resource_audit_report(invalid_reset);
  ASSERT_EQ(ledger_reset.repairs.size(), 1U);
  EXPECT_EQ(ledger_reset.repairs.front().action, ntn_resource_repair_action::resend_rnti_lease_pool);
  EXPECT_EQ(ledger_reset.repairs.front().rnti_leases, active_pool.leases);

  snapshot = manager.get_snapshot();
  EXPECT_EQ(snapshot.nof_rnti_leases_retired, 0U);
  ASSERT_EQ(snapshot.rnti_retirement_du_statuses.size(), 1U);
  EXPECT_EQ(snapshot.rnti_retirement_du_statuses.front().generation_high_water, active_pool.generation_id);
  EXPECT_EQ(manager.queue_resource_repair(ledger_reset.repairs.front(), invalid_reset.generation_id).state, "queued");

  manager.mark_rnti_lease_pool_sent_to_du(active_pool);
  f1ap_ntn_rnti_lease_pool_result reinstall_result;
  reinstall_result.generation_id   = active_pool.generation_id;
  reinstall_result.accepted        = true;
  reinstall_result.accepted_leases = active_pool.leases;
  ASSERT_TRUE(manager.mark_rnti_lease_pool_distribution_result(active_pool, reinstall_result));

  invalid_reset.rnti_generation_high_water = active_pool.generation_id;
  invalid_reset.rnti_leases.push_back(
      {active_pool.leases.front(), "pending", "applied_by_du", active_pool.generation_id});
  ++invalid_reset.generation_id;
  EXPECT_TRUE(manager.handle_resource_audit_report(invalid_reset).repairs.empty());
  snapshot = manager.get_snapshot();
  EXPECT_EQ(snapshot.nof_rnti_leases_retired, 1U);
  EXPECT_EQ(snapshot.nof_rnti_leases_available, 1U);
  EXPECT_FALSE(manager.is_rnti_excluded_for_du(report.du_index, to_rnti(0x5542)));
}

TEST(ntn_beam_service_resource_manager, missing_never_sent_orphan_is_not_confirmed_as_retired_after_reconnect)
{
  ntn_beam_service_resource_manager manager;

  ntn_resource_audit_report report;
  report.du_index                         = uint_to_du_index(6);
  report.cell_index                       = to_du_cell_index(8);
  report.pci                              = pci_t{77};
  report.generation_id                    = 103;
  report.rnti_snapshot_complete           = true;
  report.du_connection_generation         = 50;
  report.rnti_retirement_capability_known = true;
  report.rnti_retirement_supported        = true;
  report.rnti_generation_high_water       = 903;
  report.rnti_leases.push_back({to_rnti(0x5521), "expired", "expired_by_du", 903});
  ASSERT_TRUE(manager.handle_resource_audit_report(report).repairs.empty());
  ASSERT_EQ(manager.get_pending_rnti_retirement_batches().size(), 1U);

  manager.invalidate_rnti_retirement_for_du(report.du_index);
  report.rnti_leases.clear();
  report.du_connection_generation = 51;
  ++report.generation_id;
  EXPECT_TRUE(manager.handle_resource_audit_report(report).repairs.empty());

  const auto snapshot = manager.get_snapshot();
  ASSERT_EQ(snapshot.rnti_leases.size(), 1U);
  EXPECT_EQ(snapshot.rnti_leases.front().state, "orphan_quarantined");
  EXPECT_EQ(snapshot.nof_rnti_leases_retired, 0U);
  EXPECT_TRUE(manager.is_rnti_excluded_for_du(report.du_index, to_rnti(0x5521)));
}

TEST(ntn_beam_service_resource_manager, known_non_successful_retirement_outcomes_cannot_use_absence_recovery)
{
  const std::vector<ntn_rnti_retirement_outcome> outcomes = {ntn_rnti_retirement_outcome::definitively_rejected,
                                                             ntn_rnti_retirement_outcome::not_sent};

  for (const ntn_rnti_retirement_outcome outcome : outcomes) {
    SCOPED_TRACE(outcome == ntn_rnti_retirement_outcome::not_sent ? "not_sent" : "definitively_rejected");
    ntn_beam_service_resource_manager manager;

    ntn_resource_audit_report report;
    report.du_index                         = uint_to_du_index(7);
    report.cell_index                       = to_du_cell_index(9);
    report.pci                              = pci_t{78};
    report.generation_id                    = 104;
    report.rnti_snapshot_complete           = true;
    report.du_connection_generation         = 60;
    report.rnti_retirement_capability_known = true;
    report.rnti_retirement_supported        = true;
    report.rnti_generation_high_water       = 904;
    report.rnti_leases.push_back({to_rnti(0x5531), "expired", "expired_by_du", 904});
    ASSERT_TRUE(manager.handle_resource_audit_report(report).repairs.empty());

    const auto batches = manager.get_pending_rnti_retirement_batches();
    ASSERT_EQ(batches.size(), 1U);
    ASSERT_TRUE(manager.mark_rnti_retirement_sent(batches.front()));
    ASSERT_TRUE(manager.mark_rnti_retirement_result(batches.front(), outcome, "known_non_success"));
    manager.invalidate_rnti_retirement_for_du(report.du_index);

    report.rnti_leases.clear();
    report.du_connection_generation = 61;
    ++report.generation_id;
    EXPECT_TRUE(manager.handle_resource_audit_report(report).repairs.empty());

    const auto snapshot = manager.get_snapshot();
    ASSERT_EQ(snapshot.rnti_leases.size(), 1U);
    EXPECT_EQ(snapshot.nof_rnti_leases_retired, 0U);
    EXPECT_TRUE(manager.is_rnti_excluded_for_du(report.du_index, to_rnti(0x5531)));
  }
}

TEST(ntn_beam_service_resource_manager, accepted_complete_audit_resolves_prior_generic_audit_rejection)
{
  ntn_beam_service_resource_manager manager;

  ntn_resource_audit_report rejected;
  rejected.du_index       = uint_to_du_index(0);
  rejected.cell_index     = to_du_cell_index(1);
  rejected.pci            = pci_t{1};
  rejected.generation_id  = 20;
  rejected.accepted       = false;
  rejected.reject_reason  = "unknown_cell";
  const auto reject_decision = manager.handle_resource_audit_report(rejected);
  ASSERT_EQ(reject_decision.repairs.size(), 1U);
  EXPECT_EQ(manager.queue_resource_repair(reject_decision.repairs.front(), rejected.generation_id).state,
            "blocked_conflict");
  EXPECT_TRUE(manager.has_blocking_resource_repair());

  ntn_resource_audit_report recovered = rejected;
  recovered.generation_id             = 21;
  recovered.accepted                  = true;
  recovered.rnti_snapshot_complete    = true;
  recovered.reject_reason.clear();
  EXPECT_TRUE(manager.handle_resource_audit_report(recovered).repairs.empty());
  EXPECT_FALSE(manager.has_blocking_resource_repair());

  const auto snapshot = manager.get_snapshot();
  ASSERT_EQ(snapshot.resource_repairs.size(), 1U);
  EXPECT_EQ(snapshot.resource_repairs.front().state, "resolved");
  EXPECT_EQ(snapshot.resource_repairs.front().reason, "du_audit_recovered");
}

TEST(ntn_beam_service_resource_manager, when_sent_generation_is_pending_in_complete_snapshot_then_it_is_applied)
{
  ntn_beam_service_resource_manager manager;
  const ntn_rnti_lease_pool_update  lease_pool = make_target_handover_lease_pool();
  ASSERT_TRUE(manager.reserve_rnti_leases(lease_pool).accepted);
  manager.mark_rnti_lease_pool_sent_to_du(lease_pool);

  ntn_resource_audit_report report;
  report.du_index               = lease_pool.du_index;
  report.cell_index             = lease_pool.cell_index;
  report.pci                    = lease_pool.pci;
  report.generation_id          = 16;
  report.rnti_snapshot_complete = true;
  for (rnti_t rnti : lease_pool.leases) {
    report.rnti_leases.push_back({rnti, "pending", "applied_by_du", lease_pool.generation_id});
  }

  const ntn_resource_audit_decision decision = manager.handle_resource_audit_report(report);
  EXPECT_TRUE(decision.repairs.empty());
  const ntn_beam_service_resource_snapshot snapshot = manager.get_snapshot();
  EXPECT_EQ(snapshot.nof_rnti_leases_sent_to_du, 0U);
  EXPECT_EQ(snapshot.nof_rnti_leases_applied_by_du, lease_pool.leases.size());
  EXPECT_EQ(snapshot.nof_rnti_leases_available, lease_pool.leases.size());
}

TEST(ntn_beam_service_resource_manager, when_complete_snapshot_generation_is_stale_then_resource_domain_is_blocked)
{
  ntn_beam_service_resource_manager manager;
  const ntn_rnti_lease_pool_update  lease_pool = make_target_handover_lease_pool();
  apply_lease_pool(manager, lease_pool);

  ntn_resource_audit_report report;
  report.du_index               = lease_pool.du_index;
  report.cell_index             = lease_pool.cell_index;
  report.pci                    = lease_pool.pci;
  report.generation_id          = 17;
  report.rnti_snapshot_complete = true;
  report.rnti_leases.push_back({lease_pool.leases.front(), "pending", "applied_by_du", lease_pool.generation_id + 1});

  const ntn_resource_audit_decision decision = manager.handle_resource_audit_report(report);
  ASSERT_EQ(decision.repairs.size(), 1U);
  EXPECT_EQ(decision.repairs.front().action, ntn_resource_repair_action::mark_resource_conflict);
  EXPECT_EQ(decision.repairs.front().reason, "stale_du_rnti_snapshot_generation");
}

TEST(ntn_beam_service_resource_manager, audit_unknown_du_slot_assignment_requests_clear)
{
  ntn_beam_service_resource_manager manager;

  ntn_resource_audit_report report;
  report.du_index      = uint_to_du_index(0);
  report.cell_index    = to_du_cell_index(1);
  report.pci           = pci_t{1};
  report.generation_id = 11;
  report.ue_slot_snapshot_complete = true;

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
  report.ue_slot_snapshot_complete = true;

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
  report.ue_slot_snapshot_complete = true;

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
  downlink_report.ue_slot_snapshot_complete = true;

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
  report.ue_slot_snapshot_complete = true;

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
