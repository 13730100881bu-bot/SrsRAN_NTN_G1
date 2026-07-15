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

#include "f1ap_du_test_helpers.h"
#include "srsran/asn1/f1ap/common.h"
#include "srsran/asn1/f1ap/f1ap_pdu_contents.h"
#include "srsran/f1ap/ntn_access_calendar.h"
#include "srsran/f1ap/ntn_rnti_lease_pool.h"
#include <gtest/gtest.h>
#include <array>
#include <vector>

using namespace srsran;
using namespace srsran::srs_du;
using namespace srsran::srs_cu_cp;

static f1ap_ntn_rnti_lease_pool_update make_lease_update()
{
  f1ap_ntn_rnti_lease_pool_update update;
  update.gnb_du_id      = int_to_gnb_du_id(0x123);
  update.du_index       = uint_to_du_index(0);
  update.cell_index     = to_du_cell_index(1);
  update.cell_cgi       = nr_cell_global_id_t{plmn_identity::test_value(), nr_cell_identity::create(42).value()};
  update.pci            = 17;
  update.analog_beam_id = "AN-BEAM-0003";
  update.generation_id  = 11;
  update.expiry_ms      = 3000;
  update.operation      = f1ap_ntn_rnti_lease_pool_operation::replace;
  update.leases         = {to_rnti(0x4701), to_rnti(0x4702)};
  return update;
}

static f1ap_ntn_access_calendar_update make_access_calendar_update()
{
  f1ap_ntn_access_calendar_update update;
  update.satellite_id             = "P01-S001";
  update.catalog_version          = 10;
  update.schedule_version         = 21;
  update.source_content_hash      = std::string(64, 'a');
  update.calendar_hash            = std::string(64, 'b');
  update.activation_epoch_unix_ms = 10000;
  update.valid_until_unix_ms      = 20000;
  update.cycle_duration_us        = 10000;
  update.operation                = f1ap_ntn_access_calendar_operation::prepare;

  f1ap_ntn_access_calendar_cell first;
  first.du_cell_index = to_du_cell_index(0);
  first.nci        = nr_cell_identity::create(0x100000001).value();
  first.pci        = pci_t{17};
  first.intents.push_back({"G000001",
                           0,
                           1000,
                           f1ap_ntn_access_calendar_direction::downlink,
                           f1ap_ntn_access_calendar_purpose::ssb_sib_paging,
                           0});

  f1ap_ntn_access_calendar_cell second;
  second.du_cell_index = to_du_cell_index(1);
  second.nci        = nr_cell_identity::create(0x200000002).value();
  second.pci        = pci_t{17};
  second.intents.push_back({"G000002",
                            1000,
                            1000,
                            f1ap_ntn_access_calendar_direction::uplink,
                            f1ap_ntn_access_calendar_purpose::prach_ro,
                            f1ap_ntn_access_calendar_intent::no_resource_port});
  update.cells = {std::move(first), std::move(second)};
  return update;
}

static f1ap_ntn_resource_audit_result make_resource_audit_result()
{
  f1ap_ntn_resource_audit_result result;
  result.generation_id             = 91;
  result.accepted                  = true;
  result.rnti_snapshot_complete    = true;
  result.ue_slot_snapshot_complete = true;
  result.reject_reason             = "accepted";
  result.rnti_leases.push_back({to_rnti(0x4701), "pending", "applied_by_du", 17});
  return result;
}

static byte_buffer make_legacy_v1_resource_audit_result(const f1ap_ntn_resource_audit_result& result)
{
  // Encode the actual V1 layout: it has neither completeness flags nor per-lease generation IDs.
  std::vector<uint8_t> legacy    = {'N', 'T', 'A', 'U', 'D', 'R', '0', '1'};
  const auto           write_u8  = [&legacy](uint8_t value) { legacy.push_back(value); };
  const auto           write_u16 = [&legacy](uint16_t value) {
    legacy.push_back(static_cast<uint8_t>((value >> 8U) & 0xffU));
    legacy.push_back(static_cast<uint8_t>(value & 0xffU));
  };
  const auto write_u32 = [&legacy](uint32_t value) {
    legacy.push_back(static_cast<uint8_t>((value >> 24U) & 0xffU));
    legacy.push_back(static_cast<uint8_t>((value >> 16U) & 0xffU));
    legacy.push_back(static_cast<uint8_t>((value >> 8U) & 0xffU));
    legacy.push_back(static_cast<uint8_t>(value & 0xffU));
  };
  const auto write_string = [&legacy, &write_u16](const std::string& value) {
    write_u16(static_cast<uint16_t>(value.size()));
    legacy.insert(legacy.end(), value.begin(), value.end());
  };

  write_u32(result.generation_id);
  write_u8(result.accepted ? 1 : 0);
  write_string(result.reject_reason);
  write_u16(static_cast<uint16_t>(result.rnti_leases.size()));
  for (const auto& lease : result.rnti_leases) {
    write_u16(to_value(lease.rnti));
    write_string(lease.state);
    write_string(lease.distribution_state);
  }
  // The legacy fixture intentionally carries no UE-slot entries.
  write_u16(0);
  return byte_buffer::create(span<const uint8_t>(legacy.data(), legacy.size())).value();
}

TEST(f1ap_ntn_resource_audit_codec_test, when_v2_result_is_round_tripped_then_snapshot_completeness_is_preserved)
{
  const f1ap_ntn_resource_audit_result result = make_resource_audit_result();

  const auto decoded = decode_f1ap_ntn_resource_audit_result(encode_f1ap_ntn_resource_audit_result(result));

  ASSERT_TRUE(decoded.has_value());
  EXPECT_TRUE(decoded->accepted);
  EXPECT_TRUE(decoded->rnti_snapshot_complete);
  EXPECT_TRUE(decoded->ue_slot_snapshot_complete);
  ASSERT_EQ(decoded->rnti_leases.size(), 1U);
  EXPECT_EQ(decoded->rnti_leases.front().generation_id, 17U);
  EXPECT_EQ(decoded->rnti_leases.front().state, "pending");
  EXPECT_EQ(decoded->rnti_leases.front().distribution_state, "applied_by_du");
}

TEST(f1ap_ntn_resource_audit_codec_test, when_v1_result_is_decoded_then_both_snapshots_fail_safe_to_incomplete)
{
  const auto decoded =
      decode_f1ap_ntn_resource_audit_result(make_legacy_v1_resource_audit_result(make_resource_audit_result()));

  ASSERT_TRUE(decoded.has_value());
  EXPECT_TRUE(decoded->accepted);
  EXPECT_FALSE(decoded->rnti_snapshot_complete);
  EXPECT_FALSE(decoded->ue_slot_snapshot_complete);
  ASSERT_EQ(decoded->rnti_leases.size(), 1U);
  EXPECT_EQ(decoded->rnti_leases.front().generation_id, 0U);
}

TEST(f1ap_ntn_resource_audit_codec_test, when_v2_result_has_unknown_completeness_bits_then_decode_fails)
{
  byte_buffer malformed = encode_f1ap_ntn_resource_audit_result(make_resource_audit_result());
  malformed[13] |= 0x80U;

  EXPECT_FALSE(decode_f1ap_ntn_resource_audit_result(malformed).has_value());
}

static f1ap_message make_resource_coordination_request(uint16_t transaction_id,
                                                       const f1ap_ntn_rnti_lease_pool_update& update)
{
  f1ap_message msg;
  msg.pdu.set_init_msg().load_info_obj(ASN1_F1AP_ID_GNB_DU_RES_COORDINATION);
  auto& req = msg.pdu.init_msg().value.gnb_du_res_coordination_request();
  req->transaction_id = transaction_id;
  req->request_type.value = asn1::f1ap::request_type_opts::execution;
  req->eutra_nr_cell_res_coordination_req_container = encode_f1ap_ntn_rnti_lease_pool_update(update);
  return msg;
}

static f1ap_message make_resource_coordination_request(uint16_t transaction_id,
                                                       const f1ap_ntn_resource_audit_request& request)
{
  f1ap_message msg;
  msg.pdu.set_init_msg().load_info_obj(ASN1_F1AP_ID_GNB_DU_RES_COORDINATION);
  auto& req = msg.pdu.init_msg().value.gnb_du_res_coordination_request();
  req->transaction_id = transaction_id;
  req->request_type.value = asn1::f1ap::request_type_opts::execution;
  req->eutra_nr_cell_res_coordination_req_container = encode_f1ap_ntn_resource_audit_request(request);
  return msg;
}

static f1ap_message make_resource_coordination_request(uint16_t transaction_id,
                                                       const f1ap_ntn_sib19_broadcast_update& update)
{
  f1ap_message msg;
  msg.pdu.set_init_msg().load_info_obj(ASN1_F1AP_ID_GNB_DU_RES_COORDINATION);
  auto& req = msg.pdu.init_msg().value.gnb_du_res_coordination_request();
  req->transaction_id = transaction_id;
  req->request_type.value = asn1::f1ap::request_type_opts::execution;
  req->eutra_nr_cell_res_coordination_req_container = encode_f1ap_ntn_sib19_broadcast_update(update);
  return msg;
}

static f1ap_message make_resource_coordination_request(uint16_t transaction_id,
                                                       const f1ap_ntn_access_calendar_update& update)
{
  f1ap_message msg;
  msg.pdu.set_init_msg().load_info_obj(ASN1_F1AP_ID_GNB_DU_RES_COORDINATION);
  auto& req = msg.pdu.init_msg().value.gnb_du_res_coordination_request();
  req->transaction_id = transaction_id;
  req->request_type.value = asn1::f1ap::request_type_opts::execution;
  req->eutra_nr_cell_res_coordination_req_container = encode_f1ap_ntn_access_calendar_update(update);
  return msg;
}

class f1ap_du_gnbdu_resource_coordination_test : public f1ap_du_test
{
public:
  f1ap_du_gnbdu_resource_coordination_test()
  {
    run_f1_setup_procedure();
    f1c_gw.clear_tx_pdus();
  }
};

TEST_F(f1ap_du_gnbdu_resource_coordination_test, valid_update_is_forwarded_to_du_configurator_and_acked)
{
  const auto update = make_lease_update();

  f1ap_ntn_rnti_lease_pool_result next_result;
  next_result.generation_id   = update.generation_id;
  next_result.accepted        = true;
  next_result.accepted_leases = update.leases;
  next_result.reject_reason   = "accepted";
  f1ap_du_cfg_handler.next_ntn_rnti_lease_pool_result = next_result;

  f1ap->handle_message(make_resource_coordination_request(7, update));

  ASSERT_TRUE(f1ap_du_cfg_handler.last_ntn_rnti_lease_pool_update.has_value());
  EXPECT_EQ(f1ap_du_cfg_handler.last_ntn_rnti_lease_pool_update->generation_id, update.generation_id);
  EXPECT_EQ(f1ap_du_cfg_handler.last_ntn_rnti_lease_pool_update->leases, update.leases);

  ASSERT_TRUE(f1c_gw.tx_pdus_sent());
  const f1ap_message& response = f1c_gw.last_tx_pdu();
  ASSERT_EQ(response.pdu.type().value, asn1::f1ap::f1ap_pdu_c::types_opts::successful_outcome);
  ASSERT_EQ(response.pdu.successful_outcome().value.type().value,
            asn1::f1ap::f1ap_elem_procs_o::successful_outcome_c::types_opts::gnb_du_res_coordination_resp);
  const auto& asn1_resp = response.pdu.successful_outcome().value.gnb_du_res_coordination_resp();
  EXPECT_EQ(asn1_resp->transaction_id, 7);

  auto decoded = decode_f1ap_ntn_rnti_lease_pool_result(asn1_resp->eutra_nr_cell_res_coordination_req_ack_container);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_TRUE(decoded->accepted);
  EXPECT_EQ(decoded->accepted_leases, update.leases);
}

TEST_F(f1ap_du_gnbdu_resource_coordination_test, valid_access_calendar_is_forwarded_to_du_boundary_and_acked)
{
  const auto update = make_access_calendar_update();

  f1ap_ntn_access_calendar_result next_result;
  next_result.catalog_version              = update.catalog_version;
  next_result.schedule_version             = update.schedule_version;
  next_result.source_content_hash          = update.source_content_hash;
  next_result.calendar_hash                = update.calendar_hash;
  next_result.status                       = f1ap_ntn_access_calendar_result_status::ready;
  next_result.activation_slot              = slot_point{1, 42};
  next_result.accepted_intents_per_cell[0] = 1;
  next_result.accepted_intents_per_cell[1] = 1;
  f1ap_du_cfg_handler.next_ntn_access_calendar_result = next_result;

  f1ap->handle_message(make_resource_coordination_request(11, update));

  ASSERT_TRUE(f1ap_du_cfg_handler.last_ntn_access_calendar_update.has_value());
  EXPECT_EQ(f1ap_du_cfg_handler.last_ntn_access_calendar_update->schedule_version, update.schedule_version);
  ASSERT_EQ(f1ap_du_cfg_handler.last_ntn_access_calendar_update->cells.size(), 2U);
  EXPECT_EQ(f1ap_du_cfg_handler.last_ntn_access_calendar_update->cells[1].nci, update.cells[1].nci);

  ASSERT_TRUE(f1c_gw.tx_pdus_sent());
  const auto& asn1_resp = f1c_gw.last_tx_pdu().pdu.successful_outcome().value.gnb_du_res_coordination_resp();
  EXPECT_EQ(asn1_resp->transaction_id, 11);
  const auto decoded =
      decode_f1ap_ntn_access_calendar_result(asn1_resp->eutra_nr_cell_res_coordination_req_ack_container);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->status, f1ap_ntn_access_calendar_result_status::ready);
  EXPECT_TRUE(decoded->accepted());
  EXPECT_EQ(decoded->activation_slot, next_result.activation_slot);
  EXPECT_EQ(decoded->accepted_intents_per_cell, next_result.accepted_intents_per_cell);
}

TEST_F(f1ap_du_gnbdu_resource_coordination_test, malformed_access_calendar_is_rejected_without_falling_through)
{
  f1ap_message msg = make_resource_coordination_request(12, make_access_calendar_update());
  auto& container =
      msg.pdu.init_msg().value.gnb_du_res_coordination_request()->eutra_nr_cell_res_coordination_req_container;
  container[6] = 2;

  f1ap->handle_message(msg);

  EXPECT_FALSE(f1ap_du_cfg_handler.last_ntn_access_calendar_update.has_value());
  EXPECT_FALSE(f1ap_du_cfg_handler.last_ntn_rnti_lease_pool_update.has_value());
  ASSERT_TRUE(f1c_gw.tx_pdus_sent());
  const auto& asn1_resp = f1c_gw.last_tx_pdu().pdu.successful_outcome().value.gnb_du_res_coordination_resp();
  const auto decoded =
      decode_f1ap_ntn_access_calendar_result(asn1_resp->eutra_nr_cell_res_coordination_req_ack_container);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->status, f1ap_ntn_access_calendar_result_status::rejected);
  EXPECT_EQ(decoded->reject_reason, "malformed_ntn_access_calendar_update");
}

TEST_F(f1ap_du_gnbdu_resource_coordination_test, malformed_update_is_rejected_without_configurator_call)
{
  f1ap_message msg = make_resource_coordination_request(8, make_lease_update());
  msg.pdu.init_msg().value.gnb_du_res_coordination_request()->eutra_nr_cell_res_coordination_req_container[0] = 'X';

  f1ap->handle_message(msg);

  EXPECT_FALSE(f1ap_du_cfg_handler.last_ntn_rnti_lease_pool_update.has_value());
  ASSERT_TRUE(f1c_gw.tx_pdus_sent());
  const auto& asn1_resp = f1c_gw.last_tx_pdu().pdu.successful_outcome().value.gnb_du_res_coordination_resp();
  auto        decoded = decode_f1ap_ntn_rnti_lease_pool_result(asn1_resp->eutra_nr_cell_res_coordination_req_ack_container);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_FALSE(decoded->accepted);
}

TEST_F(f1ap_du_gnbdu_resource_coordination_test, valid_audit_request_is_forwarded_to_du_configurator_and_acked)
{
  f1ap_ntn_resource_audit_request request;
  request.du_index      = uint_to_du_index(0);
  request.cell_index    = to_du_cell_index(1);
  request.pci           = pci_t{17};
  request.generation_id = 91;

  f1ap_ntn_resource_audit_result next_result;
  next_result.generation_id = request.generation_id;
  next_result.accepted      = true;
  next_result.rnti_snapshot_complete    = true;
  next_result.ue_slot_snapshot_complete = false;
  next_result.reject_reason             = "ue_slot_snapshot_incomplete";
  next_result.rnti_leases.push_back({to_rnti(0x4701), "pending", "applied_by_du", 23});
  f1ap_du_cfg_handler.next_ntn_resource_audit_result = next_result;

  f1ap->handle_message(make_resource_coordination_request(9, request));

  ASSERT_TRUE(f1ap_du_cfg_handler.last_ntn_resource_audit_request.has_value());
  EXPECT_EQ(f1ap_du_cfg_handler.last_ntn_resource_audit_request->generation_id, request.generation_id);

  ASSERT_TRUE(f1c_gw.tx_pdus_sent());
  const auto& asn1_resp = f1c_gw.last_tx_pdu().pdu.successful_outcome().value.gnb_du_res_coordination_resp();
  auto decoded = decode_f1ap_ntn_resource_audit_result(asn1_resp->eutra_nr_cell_res_coordination_req_ack_container);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_TRUE(decoded->accepted);
  EXPECT_TRUE(decoded->rnti_snapshot_complete);
  EXPECT_FALSE(decoded->ue_slot_snapshot_complete);
  EXPECT_EQ(decoded->reject_reason, "ue_slot_snapshot_incomplete");
  ASSERT_EQ(decoded->rnti_leases.size(), 1U);
  EXPECT_EQ(decoded->rnti_leases.front().rnti, to_rnti(0x4701));
  EXPECT_EQ(decoded->rnti_leases.front().generation_id, 23U);
}

TEST_F(f1ap_du_gnbdu_resource_coordination_test, valid_sib19_update_is_forwarded_to_du_configurator_and_acked)
{
  f1ap_ntn_sib19_broadcast_update update;
  update.du_index      = uint_to_du_index(0);
  update.cell_index    = to_du_cell_index(1);
  update.pci           = pci_t{17};
  update.beam_id       = "CN-BEAM-0007";
  update.nci           = nr_cell_identity::create(0x66c007).value();
  update.generation_id = 33;
  update.operation     = f1ap_ntn_sib19_broadcast_operation::update;
  update.si_msg_idx    = 0;
  update.sib_idx       = 19;
  update.valid_from    = slot_point{0, 44};
  const std::array<uint8_t, 2> pdu = {0xaa, 0xbb};
  update.packed_sib19 = byte_buffer::create(pdu).value();

  f1ap_ntn_sib19_broadcast_result next_result;
  next_result.generation_id = update.generation_id;
  next_result.status        = f1ap_ntn_sib19_broadcast_result_status::applied;
  f1ap_du_cfg_handler.next_ntn_sib19_broadcast_result = next_result;

  f1ap->handle_message(make_resource_coordination_request(10, update));

  ASSERT_TRUE(f1ap_du_cfg_handler.last_ntn_sib19_broadcast_update.has_value());
  EXPECT_EQ(f1ap_du_cfg_handler.last_ntn_sib19_broadcast_update->generation_id, update.generation_id);
  EXPECT_EQ(f1ap_du_cfg_handler.last_ntn_sib19_broadcast_update->beam_id, update.beam_id);

  ASSERT_TRUE(f1c_gw.tx_pdus_sent());
  const auto& asn1_resp = f1c_gw.last_tx_pdu().pdu.successful_outcome().value.gnb_du_res_coordination_resp();
  auto decoded = decode_f1ap_ntn_sib19_broadcast_result(asn1_resp->eutra_nr_cell_res_coordination_req_ack_container);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_TRUE(decoded->accepted());
  EXPECT_EQ(decoded->generation_id, update.generation_id);
}
