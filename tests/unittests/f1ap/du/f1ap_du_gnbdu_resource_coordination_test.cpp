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
#include "srsran/f1ap/ntn_initial_ul_position_query.h"
#include "srsran/f1ap/ntn_rnti_lease_pool.h"
#include <array>
#include <gtest/gtest.h>
#include <limits>
#include <vector>

using namespace srsran;
using namespace srsran::srs_du;
using namespace srsran::srs_cu_cp;

static f1ap_ntn_initial_ul_position_query make_du_initial_ul_position_query()
{
  f1ap_ntn_initial_ul_position_query query;
  query.query_generation         = 7;
  query.nonce                    = 0x1020304050607080ULL;
  query.connection_token         = 0x8877665544332211ULL;
  query.gnb_du_id                = int_to_gnb_du_id(0x123);
  query.cell_cgi                 =
      nr_cell_global_id_t{plmn_identity::test_value(), nr_cell_identity::create(0x12345).value()};
  query.cell_index               = to_du_cell_index(1);
  query.pci                      = pci_t{17};
  query.gnb_du_ue_f1ap_id        = int_to_gnb_du_ue_f1ap_id(51);
  query.c_rnti                   = to_rnti(0x4701);
  query.expected_rnti_generation = 105;
  return query;
}

static f1ap_ntn_initial_ul_position_result make_initial_ul_position_result()
{
  const auto query = make_du_initial_ul_position_query();

  f1ap_ntn_initial_ul_position_result result;
  result.query_generation         = query.query_generation;
  result.nonce                    = query.nonce;
  result.connection_token         = query.connection_token;
  result.gnb_du_id                = query.gnb_du_id;
  result.cell_cgi                 = query.cell_cgi;
  result.cell_index               = query.cell_index;
  result.pci                      = query.pci;
  result.gnb_du_ue_f1ap_id        = query.gnb_du_ue_f1ap_id;
  result.c_rnti                   = query.c_rnti;
  result.expected_rnti_generation = query.expected_rnti_generation;
  result.accepted                 = true;
  result.observation_id           = 0x12345678ULL;
  result.authority                = f1ap_ntn_initial_ul_position_authority::ofh_beam_id_verified;
  result.reason                   = "accepted";
  result.schedule_version         = 21;
  result.calendar_hash            = std::string(64, 'a');
  result.mapping_version          = 3;
  result.mapping_hash             = std::string(64, 'b');
  result.position_id              = "G000123";
  result.logical_port             = 4;
  result.physical_port            = 2;
  result.eaxc                     = 3;
  result.beam_id                  = 0x1234;
  result.calendar_cycle_index     = 22;
  result.occasion_offset_us       = 40000;
  result.confidence_margin_db     = 6.25F;
  return result;
}

TEST(f1ap_ntn_initial_ul_position_container_test, valid_result_round_trips_identity_and_verified_port_context)
{
  const f1ap_ntn_initial_ul_position_result result  = make_initial_ul_position_result();
  const byte_buffer                        encoded = encode_f1ap_ntn_initial_ul_position_result(result);

  ASSERT_GT(encoded.length(), f1ap_ntn_initial_ul_position_detail::result_magic.size());
  ASSERT_LE(encoded.length(), f1ap_ntn_initial_ul_position_detail::max_container_size);
  for (size_t i = 0; i != f1ap_ntn_initial_ul_position_detail::result_magic.size(); ++i) {
    EXPECT_EQ(encoded[i], f1ap_ntn_initial_ul_position_detail::result_magic[i]);
  }
  const auto decoded = decode_f1ap_ntn_initial_ul_position_result(encoded);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->query_generation, result.query_generation);
  EXPECT_EQ(decoded->nonce, result.nonce);
  EXPECT_EQ(decoded->connection_token, result.connection_token);
  EXPECT_EQ(decoded->gnb_du_id, result.gnb_du_id);
  EXPECT_EQ(decoded->cell_cgi, result.cell_cgi);
  EXPECT_EQ(decoded->cell_index, result.cell_index);
  EXPECT_EQ(decoded->pci, result.pci);
  EXPECT_EQ(decoded->gnb_du_ue_f1ap_id, result.gnb_du_ue_f1ap_id);
  EXPECT_EQ(decoded->c_rnti, result.c_rnti);
  EXPECT_EQ(decoded->expected_rnti_generation, result.expected_rnti_generation);
  EXPECT_EQ(decoded->observation_id, result.observation_id);
  EXPECT_TRUE(decoded->accepted);
  EXPECT_EQ(decoded->authority, result.authority);
  EXPECT_EQ(decoded->reason, result.reason);
  EXPECT_EQ(decoded->schedule_version, result.schedule_version);
  EXPECT_EQ(decoded->calendar_hash, result.calendar_hash);
  EXPECT_EQ(decoded->mapping_version, result.mapping_version);
  EXPECT_EQ(decoded->mapping_hash, result.mapping_hash);
  EXPECT_EQ(decoded->position_id, result.position_id);
  EXPECT_EQ(decoded->logical_port, result.logical_port);
  EXPECT_EQ(decoded->physical_port, result.physical_port);
  EXPECT_EQ(decoded->eaxc, result.eaxc);
  EXPECT_EQ(decoded->beam_id, result.beam_id);
  EXPECT_EQ(decoded->calendar_cycle_index, result.calendar_cycle_index);
  EXPECT_EQ(decoded->occasion_offset_us, result.occasion_offset_us);
  EXPECT_FLOAT_EQ(decoded->confidence_margin_db, result.confidence_margin_db);
}

TEST(f1ap_ntn_initial_ul_position_container_test, result_authorities_and_explicit_rejection_round_trip)
{
  auto software      = make_initial_ul_position_result();
  software.authority = f1ap_ntn_initial_ul_position_authority::software_attributed;
  software.physical_port = f1ap_ntn_initial_ul_position_detail::unavailable_physical_port_id;
  software.mapping_version = 0;
  software.mapping_hash.clear();
  software.eaxc.reset();
  software.beam_id.reset();
  const auto decoded_software =
      decode_f1ap_ntn_initial_ul_position_result(encode_f1ap_ntn_initial_ul_position_result(software));
  ASSERT_TRUE(decoded_software.has_value());
  EXPECT_EQ(decoded_software->authority, f1ap_ntn_initial_ul_position_authority::software_attributed);

  auto sdr      = software;
  sdr.authority = f1ap_ntn_initial_ul_position_authority::sdr_rx_port_verified;
  sdr.physical_port  = 2;
  sdr.mapping_version = 7;
  sdr.mapping_hash    = "mapping-hash";
  const auto decoded_sdr =
      decode_f1ap_ntn_initial_ul_position_result(encode_f1ap_ntn_initial_ul_position_result(sdr));
  ASSERT_TRUE(decoded_sdr.has_value());
  EXPECT_EQ(decoded_sdr->authority, f1ap_ntn_initial_ul_position_authority::sdr_rx_port_verified);

  EXPECT_EQ(decoded_software->physical_port,
            f1ap_ntn_initial_ul_position_detail::unavailable_physical_port_id);

  auto rejected              = make_initial_ul_position_result();
  rejected.accepted          = false;
  rejected.observation_id    = 0;
  rejected.authority         = f1ap_ntn_initial_ul_position_authority::none;
  rejected.reason            = "observation_missing";
  rejected.schedule_version = 0;
  rejected.calendar_hash.clear();
  rejected.mapping_version = 0;
  rejected.mapping_hash.clear();
  rejected.position_id.clear();
  rejected.logical_port = 0;
  rejected.physical_port = 0;
  rejected.eaxc.reset();
  rejected.beam_id.reset();
  rejected.calendar_cycle_index = 0;
  rejected.occasion_offset_us   = 0;
  rejected.confidence_margin_db = 0.0F;
  const auto decoded_rejection =
      decode_f1ap_ntn_initial_ul_position_result(encode_f1ap_ntn_initial_ul_position_result(rejected));
  ASSERT_TRUE(decoded_rejection.has_value());
  EXPECT_FALSE(decoded_rejection->accepted);
  EXPECT_EQ(decoded_rejection->authority, f1ap_ntn_initial_ul_position_authority::none);
  EXPECT_EQ(decoded_rejection->reason, rejected.reason);
}

TEST(f1ap_ntn_initial_ul_position_container_test, result_encoder_rejects_invalid_authority_fields_and_bounds)
{
  const auto expect_rejected = [](const f1ap_ntn_initial_ul_position_result& result) {
    EXPECT_EQ(encode_f1ap_ntn_initial_ul_position_result(result).length(), 0U);
  };

  auto result      = make_initial_ul_position_result();
  result.authority = f1ap_ntn_initial_ul_position_authority::invalid;
  expect_rejected(result);
  result           = make_initial_ul_position_result();
  result.beam_id   = f1ap_ntn_initial_ul_position_detail::max_beam_id + 1U;
  expect_rejected(result);
  result      = make_initial_ul_position_result();
  result.eaxc = f1ap_ntn_initial_ul_position_detail::max_eaxc_id + 1U;
  expect_rejected(result);
  result              = make_initial_ul_position_result();
  result.beam_id.reset();
  expect_rejected(result);
  result           = make_initial_ul_position_result();
  result.authority = f1ap_ntn_initial_ul_position_authority::sdr_rx_port_verified;
  expect_rejected(result);
  result        = make_initial_ul_position_result();
  result.reason = std::string(f1ap_ntn_initial_ul_position_detail::max_reason_length + 1U, 'x');
  expect_rejected(result);
  result               = make_initial_ul_position_result();
  result.calendar_hash = std::string(f1ap_ntn_initial_ul_position_detail::max_hash_length + 1U, 'x');
  expect_rejected(result);
  result             = make_initial_ul_position_result();
  result.position_id = "not-a-position";
  expect_rejected(result);
  result              = make_initial_ul_position_result();
  result.logical_port = f1ap_ntn_initial_ul_position_detail::max_port_id + 1U;
  expect_rejected(result);
  result               = make_initial_ul_position_result();
  result.physical_port = f1ap_ntn_initial_ul_position_detail::max_physical_port_id + 1U;
  expect_rejected(result);
  result               = make_initial_ul_position_result();
  result.physical_port = f1ap_ntn_initial_ul_position_detail::unavailable_physical_port_id;
  expect_rejected(result);
  result                  = make_initial_ul_position_result();
  result.authority        = f1ap_ntn_initial_ul_position_authority::software_attributed;
  result.eaxc.reset();
  result.beam_id.reset();
  result.mapping_version = 0;
  result.mapping_hash.clear();
  expect_rejected(result);
  result.physical_port = f1ap_ntn_initial_ul_position_detail::unavailable_physical_port_id;
  result.mapping_version = 7;
  result.mapping_hash    = "mapping-hash";
  expect_rejected(result);
  result                  = make_initial_ul_position_result();
  result.schedule_version = std::numeric_limits<uint64_t>::max();
  expect_rejected(result);
}

TEST(f1ap_ntn_initial_ul_position_container_test, result_decoder_rejects_invalid_wire_values_and_framing)
{
  const auto result = make_initial_ul_position_result();

  byte_buffer invalid_accepted = encode_f1ap_ntn_initial_ul_position_result(result);
  invalid_accepted[73]         = 2;
  EXPECT_FALSE(decode_f1ap_ntn_initial_ul_position_result(invalid_accepted).has_value());

  byte_buffer invalid_authority = encode_f1ap_ntn_initial_ul_position_result(result);
  invalid_authority[74]         = 4;
  EXPECT_FALSE(decode_f1ap_ntn_initial_ul_position_result(invalid_authority).has_value());

  byte_buffer zero_generation = encode_f1ap_ntn_initial_ul_position_result(result);
  for (size_t i = 8; i != 12; ++i) {
    zero_generation[i] = 0;
  }
  EXPECT_FALSE(decode_f1ap_ntn_initial_ul_position_result(zero_generation).has_value());

  byte_buffer invalid_cell = encode_f1ap_ntn_initial_ul_position_result(result);
  invalid_cell[47]         = 0xff;
  invalid_cell[48]         = 0xff;
  EXPECT_FALSE(decode_f1ap_ntn_initial_ul_position_result(invalid_cell).has_value());

  byte_buffer invalid_beam = encode_f1ap_ntn_initial_ul_position_result(result);
  const size_t beam_offset = invalid_beam.length() - 16U;
  invalid_beam[beam_offset] = 0x80;
  EXPECT_FALSE(decode_f1ap_ntn_initial_ul_position_result(invalid_beam).has_value());

  byte_buffer truncated = encode_f1ap_ntn_initial_ul_position_result(result);
  truncated.trim_tail(1);
  EXPECT_FALSE(decode_f1ap_ntn_initial_ul_position_result(truncated).has_value());

  byte_buffer trailing = encode_f1ap_ntn_initial_ul_position_result(result);
  ASSERT_TRUE(trailing.append(0));
  EXPECT_FALSE(decode_f1ap_ntn_initial_ul_position_result(trailing).has_value());
}

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
  update.satellite_id             = "P01-S01";
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

static f1ap_ntn_resource_audit_request make_authoritative_resource_audit_request()
{
  f1ap_ntn_resource_audit_request request;
  request.du_index                    = uint_to_du_index(0);
  request.cell_index                  = to_du_cell_index(1);
  request.pci                         = pci_t{17};
  request.generation_id               = 93;
  request.request_retirement_metadata = true;
  request.request_ue_slot_identity    = true;
  request.gnb_du_id                   = int_to_gnb_du_id(0x123);
  request.cell_cgi = nr_cell_global_id_t{plmn_identity::test_value(), nr_cell_identity::create(0x12345).value()};
  request.connection_token = 0x1122334455667788ULL;
  return request;
}

static f1ap_ntn_resource_audit_result
make_authoritative_resource_audit_result(const f1ap_ntn_resource_audit_request& request)
{
  f1ap_ntn_resource_audit_result result;
  result.generation_id                     = request.generation_id;
  result.accepted                          = true;
  result.rnti_snapshot_complete            = true;
  result.ue_slot_snapshot_complete         = true;
  result.retirement_metadata_present       = true;
  result.retire_supported                  = true;
  result.rnti_generation_high_water        = 82;
  result.ue_slot_assignment_generation_high_water = 9;
  result.ue_slot_identity_metadata_present = true;
  result.ue_slot_identity_supported        = true;
  result.gnb_du_id                         = request.gnb_du_id;
  result.du_index                          = request.du_index;
  result.cell_index                        = request.cell_index;
  result.cell_cgi                          = request.cell_cgi;
  result.pci                               = request.pci;
  result.connection_token                  = request.connection_token;
  result.rnti_leases.push_back({to_rnti(0x4701), "expired", "retired_by_du", 23});

  f1ap_ntn_resource_audit_ue_slot slot;
  slot.identity_present       = true;
  slot.gnb_cu_ue_f1ap_id      = int_to_gnb_cu_ue_f1ap_id(41);
  slot.gnb_du_ue_f1ap_id      = int_to_gnb_du_ue_f1ap_id(51);
  slot.cell_index             = request.cell_index;
  slot.cell_cgi               = request.cell_cgi;
  slot.pci                    = request.pci;
  slot.c_rnti                 = to_rnti(0x4701);
  slot.assignment_generation = 9;
  slot.state                  = "applied_by_du";
  slot.request.sr_slot_offset        = 3;
  slot.request.sr_slot_period        = 80;
  slot.request.assignment_generation = slot.assignment_generation;
  slot.request.operation             = f1ap_ntn_ul_slot_resource_operation::set;
  result.ue_slots.push_back(std::move(slot));
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
                                                       const f1ap_ntn_initial_ul_position_query& query)
{
  f1ap_message msg;
  msg.pdu.set_init_msg().load_info_obj(ASN1_F1AP_ID_GNB_DU_RES_COORDINATION);
  auto& req = msg.pdu.init_msg().value.gnb_du_res_coordination_request();
  req->transaction_id = transaction_id;
  req->request_type.value = asn1::f1ap::request_type_opts::execution;
  req->eutra_nr_cell_res_coordination_req_container = encode_f1ap_ntn_initial_ul_position_query(query);
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

TEST_F(f1ap_du_gnbdu_resource_coordination_test, initial_ul_position_query_is_forwarded_and_exact_result_is_acked)
{
  const auto query = make_du_initial_ul_position_query();
  f1ap_du_cfg_handler.next_ntn_initial_ul_position_result = make_initial_ul_position_result();

  f1ap->handle_message(make_resource_coordination_request(21, query));

  ASSERT_TRUE(f1ap_du_cfg_handler.last_ntn_initial_ul_position_query.has_value());
  const auto& forwarded = *f1ap_du_cfg_handler.last_ntn_initial_ul_position_query;
  EXPECT_EQ(forwarded.query_generation, query.query_generation);
  EXPECT_EQ(forwarded.nonce, query.nonce);
  EXPECT_EQ(forwarded.connection_token, query.connection_token);
  EXPECT_EQ(forwarded.gnb_du_id, query.gnb_du_id);
  EXPECT_EQ(forwarded.cell_cgi, query.cell_cgi);
  EXPECT_EQ(forwarded.gnb_du_ue_f1ap_id, query.gnb_du_ue_f1ap_id);
  EXPECT_EQ(forwarded.expected_rnti_generation, query.expected_rnti_generation);

  ASSERT_TRUE(f1c_gw.tx_pdus_sent());
  const auto& asn1_resp = f1c_gw.last_tx_pdu().pdu.successful_outcome().value.gnb_du_res_coordination_resp();
  EXPECT_EQ(asn1_resp->transaction_id, 21);
  const auto decoded =
      decode_f1ap_ntn_initial_ul_position_result(asn1_resp->eutra_nr_cell_res_coordination_req_ack_container);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_TRUE(decoded->accepted);
  EXPECT_EQ(decoded->observation_id, make_initial_ul_position_result().observation_id);
  EXPECT_EQ(decoded->authority, f1ap_ntn_initial_ul_position_authority::ofh_beam_id_verified);
  EXPECT_EQ(decoded->calendar_cycle_index, 22U);
  EXPECT_EQ(decoded->occasion_offset_us, 40000U);
}

TEST_F(f1ap_du_gnbdu_resource_coordination_test, malformed_initial_ul_query_does_not_fall_through_to_other_handlers)
{
  f1ap_message msg = make_resource_coordination_request(22, make_du_initial_ul_position_query());
  auto& container =
      msg.pdu.init_msg().value.gnb_du_res_coordination_request()->eutra_nr_cell_res_coordination_req_container;
  container.trim_tail(1);

  f1ap->handle_message(msg);

  EXPECT_FALSE(f1ap_du_cfg_handler.last_ntn_initial_ul_position_query.has_value());
  EXPECT_FALSE(f1ap_du_cfg_handler.last_ntn_access_calendar_update.has_value());
  EXPECT_FALSE(f1ap_du_cfg_handler.last_ntn_resource_audit_request.has_value());
  EXPECT_FALSE(f1ap_du_cfg_handler.last_ntn_rnti_lease_pool_update.has_value());
  ASSERT_TRUE(f1c_gw.tx_pdus_sent());
  const auto& asn1_resp = f1c_gw.last_tx_pdu().pdu.successful_outcome().value.gnb_du_res_coordination_resp();
  EXPECT_TRUE(asn1_resp->eutra_nr_cell_res_coordination_req_ack_container.empty());
}

TEST_F(f1ap_du_gnbdu_resource_coordination_test, retire_v2_is_forwarded_to_du_configurator_and_acked)
{
  auto update      = make_lease_update();
  update.operation = f1ap_ntn_rnti_lease_pool_operation::retire;
  update.expiry_ms = 0;

  f1ap_ntn_rnti_lease_pool_result next_result;
  next_result.generation_id                           = update.generation_id;
  next_result.accepted                                = true;
  next_result.accepted_leases                         = update.leases;
  next_result.reject_reason                           = "accepted";
  f1ap_du_cfg_handler.next_ntn_rnti_lease_pool_result = next_result;

  f1ap->handle_message(make_resource_coordination_request(17, update));

  ASSERT_TRUE(f1ap_du_cfg_handler.last_ntn_rnti_lease_pool_update.has_value());
  EXPECT_EQ(f1ap_du_cfg_handler.last_ntn_rnti_lease_pool_update->operation, f1ap_ntn_rnti_lease_pool_operation::retire);
  EXPECT_EQ(f1ap_du_cfg_handler.last_ntn_rnti_lease_pool_update->expiry_ms, 0U);

  const auto& asn1_resp = f1c_gw.last_tx_pdu().pdu.successful_outcome().value.gnb_du_res_coordination_resp();
  const auto  decoded =
      decode_f1ap_ntn_rnti_lease_pool_result(asn1_resp->eutra_nr_cell_res_coordination_req_ack_container);
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
  for (auto& report : next_result.preflight_reports) {
    report.performed           = true;
    report.passed              = true;
    report.numerology          = 1;
    report.expected_ssb        = 8;
    report.matched_ssb         = 8;
    report.expected_prach      = 1;
    report.matched_prach       = 1;
    report.max_ssb_gap_slots   = 8;
    report.max_prach_gap_slots = 64;
  }
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
  EXPECT_TRUE(decoded->preflight_reports[0].performed);
  EXPECT_TRUE(decoded->preflight_reports[0].passed);
  EXPECT_EQ(decoded->preflight_reports[0].expected_ssb, 8U);
  EXPECT_EQ(decoded->preflight_reports[1].max_prach_gap_slots, 64U);
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

TEST_F(f1ap_du_gnbdu_resource_coordination_test, v2_audit_request_returns_v3_retirement_metadata)
{
  f1ap_ntn_resource_audit_request request;
  request.du_index                    = uint_to_du_index(0);
  request.cell_index                  = to_du_cell_index(1);
  request.pci                         = pci_t{17};
  request.generation_id               = 92;
  request.request_retirement_metadata = true;

  f1ap_ntn_resource_audit_result next_result;
  next_result.generation_id                          = request.generation_id;
  next_result.accepted                               = true;
  next_result.rnti_snapshot_complete                 = true;
  next_result.retirement_metadata_present            = true;
  next_result.retire_supported                       = true;
  next_result.rnti_generation_high_water             = 81;
  next_result.reject_reason                          = "ue_slot_snapshot_incomplete";
  f1ap_du_cfg_handler.next_ntn_resource_audit_result = next_result;

  f1ap->handle_message(make_resource_coordination_request(19, request));

  ASSERT_TRUE(f1ap_du_cfg_handler.last_ntn_resource_audit_request.has_value());
  EXPECT_TRUE(f1ap_du_cfg_handler.last_ntn_resource_audit_request->request_retirement_metadata);
  const auto& asn1_resp = f1c_gw.last_tx_pdu().pdu.successful_outcome().value.gnb_du_res_coordination_resp();
  const auto  decoded =
      decode_f1ap_ntn_resource_audit_result(asn1_resp->eutra_nr_cell_res_coordination_req_ack_container);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_TRUE(decoded->retirement_metadata_present);
  EXPECT_TRUE(decoded->retire_supported);
  EXPECT_EQ(decoded->rnti_generation_high_water, 81U);
}

TEST_F(f1ap_du_gnbdu_resource_coordination_test, q3_audit_request_forwards_identity_and_returns_r4_snapshot)
{
  const auto request = make_authoritative_resource_audit_request();
  f1ap_du_cfg_handler.next_ntn_resource_audit_result = make_authoritative_resource_audit_result(request);

  f1ap->handle_message(make_resource_coordination_request(20, request));

  ASSERT_TRUE(f1ap_du_cfg_handler.last_ntn_resource_audit_request.has_value());
  const auto& forwarded = *f1ap_du_cfg_handler.last_ntn_resource_audit_request;
  EXPECT_TRUE(forwarded.request_ue_slot_identity);
  EXPECT_TRUE(forwarded.request_retirement_metadata);
  EXPECT_EQ(forwarded.gnb_du_id, request.gnb_du_id);
  EXPECT_EQ(forwarded.cell_cgi, request.cell_cgi);
  EXPECT_EQ(forwarded.connection_token, request.connection_token);

  ASSERT_TRUE(f1c_gw.tx_pdus_sent());
  const auto& asn1_resp = f1c_gw.last_tx_pdu().pdu.successful_outcome().value.gnb_du_res_coordination_resp();
  ASSERT_EQ(asn1_resp->eutra_nr_cell_res_coordination_req_ack_container[7], '4');
  const auto decoded =
      decode_f1ap_ntn_resource_audit_result(asn1_resp->eutra_nr_cell_res_coordination_req_ack_container);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_TRUE(decoded->rnti_snapshot_complete);
  EXPECT_TRUE(decoded->ue_slot_snapshot_complete);
  EXPECT_EQ(decoded->connection_token, request.connection_token);
  EXPECT_EQ(decoded->ue_slot_assignment_generation_high_water, 9U);
  ASSERT_EQ(decoded->ue_slots.size(), 1U);
  EXPECT_EQ(decoded->ue_slots.front().gnb_cu_ue_f1ap_id, int_to_gnb_cu_ue_f1ap_id(41));
  EXPECT_EQ(decoded->ue_slots.front().gnb_du_ue_f1ap_id, int_to_gnb_du_ue_f1ap_id(51));
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
