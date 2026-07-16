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

#include "f1ap_cu_test_helpers.h"
#include "srsran/asn1/f1ap/common.h"
#include "srsran/asn1/f1ap/f1ap_pdu_contents.h"
#include "srsran/f1ap/cu_cp/f1ap_cu_resource_coordination.h"
#include "srsran/f1ap/ntn_access_calendar.h"
#include "srsran/f1ap/ntn_rnti_lease_pool.h"
#include "srsran/support/async/async_test_utils.h"
#include "fmt/format.h"
#include <gtest/gtest.h>
#include <array>
#include <limits>

using namespace srsran;
using namespace srsran::srs_cu_cp;

static f1ap_ntn_rnti_lease_pool_update make_lease_update()
{
  f1ap_ntn_rnti_lease_pool_update update;
  update.gnb_du_id      = int_to_gnb_du_id(0x123);
  update.du_index       = uint_to_du_index(2);
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
  first.intents.push_back({"G000001",
                           1000,
                           1000,
                           f1ap_ntn_access_calendar_direction::uplink,
                           f1ap_ntn_access_calendar_purpose::prach_ro,
                           f1ap_ntn_access_calendar_intent::no_resource_port});

  f1ap_ntn_access_calendar_cell second;
  second.du_cell_index = to_du_cell_index(1);
  second.nci        = nr_cell_identity::create(0x200000002).value();
  second.pci        = pci_t{17};
  second.intents.push_back({"G000002",
                            2000,
                            1000,
                            f1ap_ntn_access_calendar_direction::uplink,
                            f1ap_ntn_access_calendar_purpose::prach_ul_beam,
                            0});
  update.cells = {std::move(first), std::move(second)};
  return update;
}

static f1ap_ntn_access_calendar_result make_access_calendar_result_with_preflight()
{
  f1ap_ntn_access_calendar_result result;
  result.catalog_version              = 10;
  result.schedule_version             = 21;
  result.source_content_hash          = std::string(64, 'a');
  result.calendar_hash                = std::string(64, 'b');
  result.status                       = f1ap_ntn_access_calendar_result_status::ready;
  result.activation_slot              = slot_point{1, 42};
  result.accepted_intents_per_cell[0] = 16;
  result.accepted_intents_per_cell[1] = 15;

  auto& first_report               = result.preflight_reports[0];
  first_report.performed           = true;
  first_report.passed              = true;
  first_report.numerology          = 1;
  first_report.expected_ssb        = 16;
  first_report.matched_ssb         = 16;
  first_report.expected_prach      = 8;
  first_report.matched_prach       = 8;
  first_report.max_ssb_gap_slots   = 8;
  first_report.max_prach_gap_slots = 64;

  auto& second_report               = result.preflight_reports[1];
  second_report.performed           = true;
  second_report.passed              = false;
  second_report.numerology          = 1;
  second_report.expected_ssb        = 16;
  second_report.matched_ssb         = 16;
  second_report.expected_prach      = 8;
  second_report.matched_prach       = 7;
  second_report.max_ssb_gap_slots   = 8;
  second_report.max_prach_gap_slots = 65;
  second_report.first_unmatched.emplace(f1ap_ntn_access_calendar_unmatched_intent{
      "G000002", f1ap_ntn_access_calendar_preflight_purpose::prach, 320, 2});
  return result;
}

static byte_buffer make_legacy_v1_access_calendar_result(const f1ap_ntn_access_calendar_result& result)
{
  using namespace f1ap_ntn_access_calendar_detail;
  writer out;
  out.bytes(result_magic.data(), result_magic.data() + result_magic.size());
  out.u8(legacy_result_wire_version);
  out.u64(result.catalog_version);
  out.u64(result.schedule_version);
  out.string(result.source_content_hash);
  out.string(result.calendar_hash);
  out.u8(static_cast<uint8_t>(result.status));
  out.string(result.reject_reason);
  out.u8(result.activation_slot.has_value() ? 1U : 0U);
  out.u8(result.activation_slot.has_value() ? static_cast<uint8_t>(result.activation_slot->numerology()) : 0xffU);
  out.u32(result.activation_slot.has_value() ? result.activation_slot->to_uint() : 0U);
  out.u16(result.accepted_intents_per_cell[0]);
  out.u16(result.accepted_intents_per_cell[1]);
  return out.finish();
}

static size_t access_calendar_v2_preflight_count_offset(const f1ap_ntn_access_calendar_result& result)
{
  return f1ap_ntn_access_calendar_detail::result_magic.size() + 1U + 2U * sizeof(uint64_t) + sizeof(uint16_t) +
         result.source_content_hash.size() + sizeof(uint16_t) + result.calendar_hash.size() + sizeof(uint8_t) +
         sizeof(uint16_t) + result.reject_reason.size() + 2U * sizeof(uint8_t) + sizeof(uint32_t) +
         2U * sizeof(uint16_t);
}

TEST(f1ap_ntn_access_calendar_container_test, valid_prepare_and_query_without_cells_round_trip)
{
  const auto        update  = make_access_calendar_update();
  const byte_buffer encoded = encode_f1ap_ntn_access_calendar_update(update);
  ASSERT_GT(encoded.length(), f1ap_ntn_access_calendar_detail::update_magic.size());
  EXPECT_EQ(encoded[f1ap_ntn_access_calendar_detail::update_magic.size()], 1U);
  const auto decoded = decode_f1ap_ntn_access_calendar_update(encoded);

  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->satellite_id, update.satellite_id);
  EXPECT_EQ(decoded->catalog_version, update.catalog_version);
  EXPECT_EQ(decoded->schedule_version, update.schedule_version);
  EXPECT_EQ(decoded->source_content_hash, update.source_content_hash);
  EXPECT_EQ(decoded->calendar_hash, update.calendar_hash);
  ASSERT_EQ(decoded->cells.size(), 2U);
  EXPECT_EQ(decoded->cells[0].nci, update.cells[0].nci);
  EXPECT_EQ(decoded->cells[0].pci, update.cells[0].pci);
  EXPECT_EQ(decoded->cells[1].nci, update.cells[1].nci);
  EXPECT_EQ(decoded->cells[1].pci, update.cells[1].pci);
  ASSERT_EQ(decoded->cells[0].intents.size(), 2U);
  EXPECT_EQ(decoded->cells[0].intents[1].purpose, f1ap_ntn_access_calendar_purpose::prach_ro);
  EXPECT_EQ(decoded->cells[0].intents[1].port_id, f1ap_ntn_access_calendar_intent::no_resource_port);

  auto query      = update;
  query.operation = f1ap_ntn_access_calendar_operation::query;
  query.cells.clear();
  const auto decoded_query = decode_f1ap_ntn_access_calendar_update(encode_f1ap_ntn_access_calendar_update(query));
  ASSERT_TRUE(decoded_query.has_value());
  EXPECT_EQ(decoded_query->operation, f1ap_ntn_access_calendar_operation::query);
  EXPECT_TRUE(decoded_query->cells.empty());
}

TEST(f1ap_ntn_access_calendar_container_test, empty_two_cell_prepare_round_trips_as_explicit_deny_all)
{
  auto update = make_access_calendar_update();
  update.cells[0].intents.clear();
  update.cells[1].intents.clear();

  const auto decoded = decode_f1ap_ntn_access_calendar_update(encode_f1ap_ntn_access_calendar_update(update));

  ASSERT_TRUE(decoded.has_value());
  ASSERT_EQ(decoded->cells.size(), 2U);
  EXPECT_TRUE(decoded->cells[0].intents.empty());
  EXPECT_TRUE(decoded->cells[1].intents.empty());
  EXPECT_EQ(decoded->cycle_duration_us, update.cycle_duration_us);
}

TEST(f1ap_ntn_access_calendar_container_test, decoder_enforces_cell_position_and_intent_bounds)
{
  auto one_cell = make_access_calendar_update();
  one_cell.cells.pop_back();
  EXPECT_FALSE(decode_f1ap_ntn_access_calendar_update(encode_f1ap_ntn_access_calendar_update(one_cell)).has_value());

  auto too_many_positions = make_access_calendar_update();
  too_many_positions.cells[0].intents.clear();
  too_many_positions.cells[1].intents.clear();
  for (unsigned i = 0; i != 256; ++i) {
    auto& cell = too_many_positions.cells[i % 2];
    cell.intents.push_back({fmt::format("G{:06}", i),
                            0,
                            1000,
                            f1ap_ntn_access_calendar_direction::downlink,
                            f1ap_ntn_access_calendar_purpose::ssb_sib_paging,
                            0});
  }
  EXPECT_TRUE(
      decode_f1ap_ntn_access_calendar_update(encode_f1ap_ntn_access_calendar_update(too_many_positions)).has_value());
  too_many_positions.cells[0].intents.push_back({"G000256",
                                                  0,
                                                  1000,
                                                  f1ap_ntn_access_calendar_direction::downlink,
                                                  f1ap_ntn_access_calendar_purpose::ssb_sib_paging,
                                                  0});
  EXPECT_FALSE(
      decode_f1ap_ntn_access_calendar_update(encode_f1ap_ntn_access_calendar_update(too_many_positions)).has_value());

  auto too_many_intents = make_access_calendar_update();
  too_many_intents.cells[0].intents.assign(2560, too_many_intents.cells[0].intents.front());
  too_many_intents.cells[1].intents.clear();
  EXPECT_TRUE(
      decode_f1ap_ntn_access_calendar_update(encode_f1ap_ntn_access_calendar_update(too_many_intents)).has_value());
  too_many_intents.cells[0].intents.push_back(too_many_intents.cells[0].intents.front());
  EXPECT_FALSE(
      decode_f1ap_ntn_access_calendar_update(encode_f1ap_ntn_access_calendar_update(too_many_intents)).has_value());
}

TEST(f1ap_ntn_access_calendar_container_test, decoder_rejects_invalid_time_enum_direction_and_port)
{
  auto invalid_length = make_access_calendar_update();
  invalid_length.satellite_id.assign(129, 'S');
  EXPECT_FALSE(
      decode_f1ap_ntn_access_calendar_update(encode_f1ap_ntn_access_calendar_update(invalid_length)).has_value());

  auto invalid_epoch = make_access_calendar_update();
  invalid_epoch.activation_epoch_unix_ms = static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1U;
  invalid_epoch.valid_until_unix_ms       = invalid_epoch.activation_epoch_unix_ms + 1U;
  EXPECT_FALSE(
      decode_f1ap_ntn_access_calendar_update(encode_f1ap_ntn_access_calendar_update(invalid_epoch)).has_value());

  auto invalid_time = make_access_calendar_update();
  invalid_time.cells[0].intents[0].start_time_us = invalid_time.cycle_duration_us;
  EXPECT_FALSE(
      decode_f1ap_ntn_access_calendar_update(encode_f1ap_ntn_access_calendar_update(invalid_time)).has_value());

  auto invalid_enum = make_access_calendar_update();
  invalid_enum.cells[0].intents[0].direction = f1ap_ntn_access_calendar_direction::invalid;
  EXPECT_FALSE(
      decode_f1ap_ntn_access_calendar_update(encode_f1ap_ntn_access_calendar_update(invalid_enum)).has_value());

  auto invalid_direction = make_access_calendar_update();
  invalid_direction.cells[0].intents[0].direction = f1ap_ntn_access_calendar_direction::uplink;
  EXPECT_FALSE(
      decode_f1ap_ntn_access_calendar_update(encode_f1ap_ntn_access_calendar_update(invalid_direction)).has_value());

  auto invalid_port = make_access_calendar_update();
  invalid_port.cells[0].intents[1].port_id = 0;
  EXPECT_FALSE(
      decode_f1ap_ntn_access_calendar_update(encode_f1ap_ntn_access_calendar_update(invalid_port)).has_value());
}

TEST(f1ap_ntn_access_calendar_container_test, rejected_result_keeps_echo_and_activation_fields)
{
  f1ap_ntn_access_calendar_result result;
  result.catalog_version              = 10;
  result.schedule_version             = 21;
  result.source_content_hash          = std::string(64, 'a');
  result.calendar_hash                = std::string(64, 'b');
  result.status                       = f1ap_ntn_access_calendar_result_status::rejected;
  result.reject_reason                = "resource_conflict";
  result.activation_slot             = slot_point{1, 42};
  result.accepted_intents_per_cell[0] = 17;
  result.accepted_intents_per_cell[1] = 19;

  const auto decoded = decode_f1ap_ntn_access_calendar_result(encode_f1ap_ntn_access_calendar_result(result));
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->catalog_version, result.catalog_version);
  EXPECT_EQ(decoded->schedule_version, result.schedule_version);
  EXPECT_EQ(decoded->calendar_hash, result.calendar_hash);
  EXPECT_EQ(decoded->status, f1ap_ntn_access_calendar_result_status::rejected);
  EXPECT_FALSE(decoded->accepted());
  EXPECT_EQ(decoded->reject_reason, result.reject_reason);
  EXPECT_EQ(decoded->activation_slot, result.activation_slot);
  EXPECT_EQ(decoded->accepted_intents_per_cell, result.accepted_intents_per_cell);
}

TEST(f1ap_ntn_access_calendar_container_test, preparing_result_is_accepted_and_round_trips)
{
  f1ap_ntn_access_calendar_result result;
  result.catalog_version     = 10;
  result.schedule_version    = 21;
  result.source_content_hash = std::string(64, 'a');
  result.calendar_hash       = std::string(64, 'b');
  result.status              = f1ap_ntn_access_calendar_result_status::preparing;
  result.reject_reason       = "waiting_for_both_cell_slot_threads_to_arm";
  for (auto& report : result.preflight_reports) {
    report.performed  = true;
    report.passed     = true;
    report.numerology = 1;
  }

  const auto decoded = decode_f1ap_ntn_access_calendar_result(encode_f1ap_ntn_access_calendar_result(result));

  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->status, f1ap_ntn_access_calendar_result_status::preparing);
  EXPECT_TRUE(decoded->accepted());
}

TEST(f1ap_ntn_access_calendar_container_test,
     when_v2_result_is_round_tripped_then_both_cell_preflight_reports_are_preserved)
{
  const auto        result  = make_access_calendar_result_with_preflight();
  const byte_buffer encoded = encode_f1ap_ntn_access_calendar_result(result);

  ASSERT_GT(encoded.length(), f1ap_ntn_access_calendar_detail::result_magic.size());
  EXPECT_EQ(encoded[f1ap_ntn_access_calendar_detail::result_magic.size()], 2U);
  const auto decoded = decode_f1ap_ntn_access_calendar_result(encoded);

  ASSERT_TRUE(decoded.has_value());
  ASSERT_TRUE(decoded->preflight_reports[0].performed);
  EXPECT_TRUE(decoded->preflight_reports[0].passed);
  EXPECT_EQ(decoded->preflight_reports[0].numerology, 1U);
  EXPECT_EQ(decoded->preflight_reports[0].expected_ssb, 16U);
  EXPECT_EQ(decoded->preflight_reports[0].matched_ssb, 16U);
  EXPECT_EQ(decoded->preflight_reports[0].expected_prach, 8U);
  EXPECT_EQ(decoded->preflight_reports[0].matched_prach, 8U);
  EXPECT_EQ(decoded->preflight_reports[0].max_ssb_gap_slots, 8U);
  EXPECT_EQ(decoded->preflight_reports[0].max_prach_gap_slots, 64U);
  ASSERT_TRUE(decoded->preflight_reports[1].performed);
  EXPECT_FALSE(decoded->preflight_reports[1].passed);
  EXPECT_EQ(decoded->preflight_reports[1].matched_prach, 7U);
  ASSERT_TRUE(decoded->preflight_reports[1].first_unmatched.has_value());
  EXPECT_EQ(decoded->preflight_reports[1].first_unmatched->position_id, "G000002");
  EXPECT_EQ(decoded->preflight_reports[1].first_unmatched->purpose,
            f1ap_ntn_access_calendar_preflight_purpose::prach);
  EXPECT_EQ(decoded->preflight_reports[1].first_unmatched->start_slot_offset, 320U);
  EXPECT_EQ(decoded->preflight_reports[1].first_unmatched->nof_slots, 2U);
}

TEST(f1ap_ntn_access_calendar_container_test,
     when_v1_result_is_decoded_then_preflight_reports_fail_closed_to_not_performed)
{
  const auto decoded = decode_f1ap_ntn_access_calendar_result(
      make_legacy_v1_access_calendar_result(make_access_calendar_result_with_preflight()));

  ASSERT_TRUE(decoded.has_value());
  EXPECT_FALSE(decoded->accepted());
  for (const auto& report : decoded->preflight_reports) {
    EXPECT_FALSE(report.performed);
    EXPECT_FALSE(report.passed);
    EXPECT_EQ(report.numerology, 0xffU);
    EXPECT_EQ(report.expected_ssb, 0U);
    EXPECT_EQ(report.matched_prach, 0U);
    EXPECT_FALSE(report.first_unmatched.has_value());
  }
}

TEST(f1ap_ntn_access_calendar_container_test,
     when_v2_result_has_invalid_length_enum_or_count_then_decode_fails_closed)
{
  const auto   result                 = make_access_calendar_result_with_preflight();
  const size_t preflight_count_offset = access_calendar_v2_preflight_count_offset(result);

  byte_buffer wrong_report_count = encode_f1ap_ntn_access_calendar_result(result);
  ASSERT_LT(preflight_count_offset, wrong_report_count.length());
  wrong_report_count[preflight_count_offset] = 1;
  EXPECT_FALSE(decode_f1ap_ntn_access_calendar_result(wrong_report_count).has_value());

  byte_buffer invalid_boolean = encode_f1ap_ntn_access_calendar_result(result);
  invalid_boolean[preflight_count_offset + 1U] = 2;
  EXPECT_FALSE(decode_f1ap_ntn_access_calendar_result(invalid_boolean).has_value());

  byte_buffer invalid_numerology = encode_f1ap_ntn_access_calendar_result(result);
  invalid_numerology[preflight_count_offset + 3U] = 5;
  EXPECT_FALSE(decode_f1ap_ntn_access_calendar_result(invalid_numerology).has_value());

  byte_buffer invalid_purpose = encode_f1ap_ntn_access_calendar_result(result);
  constexpr size_t fixed_report_size_without_unmatched = 20;
  constexpr size_t unmatched_purpose_offset            = 29;
  const size_t invalid_purpose_offset =
      preflight_count_offset + 1U + fixed_report_size_without_unmatched + unmatched_purpose_offset;
  ASSERT_LT(invalid_purpose_offset, invalid_purpose.length());
  invalid_purpose[invalid_purpose_offset] = 2;
  EXPECT_FALSE(decode_f1ap_ntn_access_calendar_result(invalid_purpose).has_value());

  auto invalid_matched_count = result;
  invalid_matched_count.preflight_reports[0].matched_prach =
      invalid_matched_count.preflight_reports[0].expected_prach + 1U;
  EXPECT_FALSE(decode_f1ap_ntn_access_calendar_result(
                   encode_f1ap_ntn_access_calendar_result(invalid_matched_count))
                   .has_value());

  byte_buffer truncated = encode_f1ap_ntn_access_calendar_result(result);
  truncated.trim_tail(1);
  EXPECT_FALSE(decode_f1ap_ntn_access_calendar_result(truncated).has_value());

  byte_buffer trailing = encode_f1ap_ntn_access_calendar_result(result);
  ASSERT_TRUE(trailing.append(0));
  EXPECT_FALSE(decode_f1ap_ntn_access_calendar_result(trailing).has_value());
}

TEST(f1ap_ntn_rnti_lease_pool_container_test, valid_update_round_trips)
{
  const auto update  = make_lease_update();
  byte_buffer buffer = encode_f1ap_ntn_rnti_lease_pool_update(update);

  std::optional<f1ap_ntn_rnti_lease_pool_update> decoded = decode_f1ap_ntn_rnti_lease_pool_update(buffer);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->gnb_du_id, update.gnb_du_id);
  EXPECT_EQ(decoded->du_index, update.du_index);
  EXPECT_EQ(decoded->cell_index, update.cell_index);
  EXPECT_EQ(decoded->cell_cgi, update.cell_cgi);
  EXPECT_EQ(decoded->pci, update.pci);
  EXPECT_EQ(decoded->analog_beam_id, update.analog_beam_id);
  EXPECT_EQ(decoded->generation_id, update.generation_id);
  EXPECT_EQ(decoded->expiry_ms, update.expiry_ms);
  EXPECT_EQ(decoded->operation, update.operation);
  ASSERT_EQ(decoded->leases.size(), update.leases.size());
  EXPECT_EQ(decoded->leases[0], update.leases[0]);
  EXPECT_EQ(decoded->leases[1], update.leases[1]);
}

TEST(f1ap_ntn_rnti_lease_pool_container_test, malformed_magic_is_rejected)
{
  byte_buffer buffer = encode_f1ap_ntn_rnti_lease_pool_update(make_lease_update());
  buffer[0]          = 'X';

  EXPECT_FALSE(decode_f1ap_ntn_rnti_lease_pool_update(buffer).has_value());
}

TEST(f1ap_ntn_resource_audit_container_test, valid_audit_request_and_result_round_trip)
{
  f1ap_ntn_resource_audit_request request;
  request.du_index      = uint_to_du_index(2);
  request.cell_index    = to_du_cell_index(1);
  request.pci           = pci_t{17};
  request.generation_id = 99;

  const byte_buffer request_buffer = encode_f1ap_ntn_resource_audit_request(request);
  const auto        decoded_request = decode_f1ap_ntn_resource_audit_request(request_buffer);
  ASSERT_TRUE(decoded_request.has_value());
  EXPECT_EQ(decoded_request->du_index, request.du_index);
  EXPECT_EQ(decoded_request->cell_index, request.cell_index);
  EXPECT_EQ(decoded_request->pci, request.pci);
  EXPECT_EQ(decoded_request->generation_id, request.generation_id);

  f1ap_ntn_resource_audit_result result;
  result.generation_id = request.generation_id;
  result.accepted      = true;
  result.rnti_leases.push_back({to_rnti(0x4701), "reserved", "applied_by_du"});
  f1ap_ntn_resource_audit_ue_slot slot;
  slot.ue_index = uint_to_ue_index(7);
  slot.state    = "applied_by_du";
  slot.request.sr_slot_offset = 3U;
  result.ue_slots.push_back(slot);

  const byte_buffer result_buffer = encode_f1ap_ntn_resource_audit_result(result);
  const auto        decoded_result = decode_f1ap_ntn_resource_audit_result(result_buffer);
  ASSERT_TRUE(decoded_result.has_value());
  EXPECT_TRUE(decoded_result->accepted);
  EXPECT_EQ(decoded_result->generation_id, result.generation_id);
  ASSERT_EQ(decoded_result->rnti_leases.size(), 1U);
  EXPECT_EQ(decoded_result->rnti_leases.front().rnti, to_rnti(0x4701));
  ASSERT_EQ(decoded_result->ue_slots.size(), 1U);
  EXPECT_EQ(decoded_result->ue_slots.front().ue_index, uint_to_ue_index(7));
  EXPECT_EQ(decoded_result->ue_slots.front().request.sr_slot_offset, std::optional<unsigned>{3U});
}

TEST(f1ap_ntn_sib19_broadcast_container_test, valid_update_and_result_round_trip)
{
  f1ap_ntn_sib19_broadcast_update update;
  update.du_index      = uint_to_du_index(1);
  update.cell_index    = to_du_cell_index(0);
  update.pci           = pci_t{42};
  update.beam_id       = "CN-BEAM-0001";
  update.nci           = nr_cell_identity::create(0x66c001).value();
  update.generation_id = 17;
  update.operation     = f1ap_ntn_sib19_broadcast_operation::update;
  update.si_msg_idx    = 0;
  update.sib_idx       = 19;
  update.valid_from    = slot_point{0, 12};
  const std::array<uint8_t, 3> sib_bytes = {0x11, 0x22, 0x33};
  update.packed_sib19 = byte_buffer::create(sib_bytes).value();

  const byte_buffer buffer = encode_f1ap_ntn_sib19_broadcast_update(update);
  const auto        decoded = decode_f1ap_ntn_sib19_broadcast_update(buffer);

  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->du_index, update.du_index);
  EXPECT_EQ(decoded->cell_index, update.cell_index);
  EXPECT_EQ(decoded->pci, update.pci);
  EXPECT_EQ(decoded->beam_id, update.beam_id);
  EXPECT_EQ(decoded->nci, update.nci);
  EXPECT_EQ(decoded->generation_id, update.generation_id);
  EXPECT_EQ(decoded->operation, update.operation);
  EXPECT_EQ(decoded->si_msg_idx, update.si_msg_idx);
  EXPECT_EQ(decoded->sib_idx, update.sib_idx);
  EXPECT_EQ(decoded->valid_from, update.valid_from);
  EXPECT_EQ(decoded->packed_sib19, update.packed_sib19);

  f1ap_ntn_sib19_broadcast_result result;
  result.generation_id = update.generation_id;
  result.status        = f1ap_ntn_sib19_broadcast_result_status::applied;
  result.reject_reason = "";

  const byte_buffer result_buffer = encode_f1ap_ntn_sib19_broadcast_result(result);
  const auto        decoded_result = decode_f1ap_ntn_sib19_broadcast_result(result_buffer);

  ASSERT_TRUE(decoded_result.has_value());
  EXPECT_EQ(decoded_result->generation_id, result.generation_id);
  EXPECT_EQ(decoded_result->status, result.status);
  EXPECT_TRUE(decoded_result->accepted());
}

class f1ap_cu_gnbdu_resource_coordination_test : public f1ap_cu_test
{
protected:
  void start_procedure(const f1ap_gnb_du_resource_coordination_request& req)
  {
    task = f1ap->handle_gnb_du_resource_coordination_request(req);
    task_launcher.emplace(task);
  }

  async_task<f1ap_gnb_du_resource_coordination_response>                        task;
  std::optional<lazy_task_launcher<f1ap_gnb_du_resource_coordination_response>> task_launcher;
};

TEST_F(f1ap_cu_gnbdu_resource_coordination_test, request_is_sent_in_gnbdu_resource_coordination_container)
{
  f1ap_gnb_du_resource_coordination_request request;
  request.ntn_rnti_lease_update = make_lease_update();

  start_procedure(request);

  ASSERT_EQ(f1ap_pdu_notifier.last_f1ap_msg.pdu.type().value, asn1::f1ap::f1ap_pdu_c::types_opts::init_msg);
  ASSERT_EQ(f1ap_pdu_notifier.last_f1ap_msg.pdu.init_msg().value.type().value,
            asn1::f1ap::f1ap_elem_procs_o::init_msg_c::types_opts::gnb_du_res_coordination_request);
  const auto& asn1_req =
      f1ap_pdu_notifier.last_f1ap_msg.pdu.init_msg().value.gnb_du_res_coordination_request();
  ASSERT_EQ(asn1_req->request_type.value, asn1::f1ap::request_type_opts::execution);

  const auto decoded = decode_f1ap_ntn_rnti_lease_pool_update(asn1_req->eutra_nr_cell_res_coordination_req_container);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->generation_id, request.ntn_rnti_lease_update.generation_id);
  EXPECT_EQ(decoded->leases, request.ntn_rnti_lease_update.leases);
  ASSERT_FALSE(task.ready());
}

TEST_F(f1ap_cu_gnbdu_resource_coordination_test, access_calendar_request_uses_private_container)
{
  f1ap_gnb_du_resource_coordination_request request;
  request.ntn_access_calendar_update = make_access_calendar_update();

  start_procedure(request);

  const auto& asn1_req =
      f1ap_pdu_notifier.last_f1ap_msg.pdu.init_msg().value.gnb_du_res_coordination_request();
  const auto decoded = decode_f1ap_ntn_access_calendar_update(asn1_req->eutra_nr_cell_res_coordination_req_container);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->schedule_version, request.ntn_access_calendar_update->schedule_version);
  ASSERT_EQ(decoded->cells.size(), 2U);
  EXPECT_EQ(decoded->cells[0].nci, request.ntn_access_calendar_update->cells[0].nci);
  EXPECT_FALSE(task.ready());
}

TEST_F(f1ap_cu_gnbdu_resource_coordination_test, rejected_access_calendar_ack_is_preserved_as_optional_result)
{
  f1ap_gnb_du_resource_coordination_request request;
  request.ntn_access_calendar_update = make_access_calendar_update();
  start_procedure(request);

  f1ap_ntn_access_calendar_result result;
  result.catalog_version     = request.ntn_access_calendar_update->catalog_version;
  result.schedule_version    = request.ntn_access_calendar_update->schedule_version;
  result.source_content_hash = request.ntn_access_calendar_update->source_content_hash;
  result.calendar_hash       = request.ntn_access_calendar_update->calendar_hash;
  result.status              = f1ap_ntn_access_calendar_result_status::rejected;
  result.reject_reason       = "port_conflict";

  f1ap_message response;
  response.pdu.set_successful_outcome().load_info_obj(ASN1_F1AP_ID_GNB_DU_RES_COORDINATION);
  auto& asn1_resp = response.pdu.successful_outcome().value.gnb_du_res_coordination_resp();
  asn1_resp->transaction_id = f1ap_pdu_notifier.last_f1ap_msg.pdu.init_msg()
                                  .value.gnb_du_res_coordination_request()
                                  ->transaction_id;
  asn1_resp->eutra_nr_cell_res_coordination_req_ack_container = encode_f1ap_ntn_access_calendar_result(result);

  f1ap->handle_message(response);

  ASSERT_TRUE(task.ready());
  ASSERT_TRUE(task.get().calendar_result.has_value());
  EXPECT_FALSE(task.get().success);
  EXPECT_EQ(task.get().calendar_result->status, f1ap_ntn_access_calendar_result_status::rejected);
  EXPECT_EQ(task.get().calendar_result->reject_reason, result.reject_reason);
}

TEST_F(f1ap_cu_gnbdu_resource_coordination_test, response_ack_container_completes_procedure)
{
  f1ap_gnb_du_resource_coordination_request request;
  request.ntn_rnti_lease_update = make_lease_update();
  start_procedure(request);

  f1ap_ntn_rnti_lease_pool_result result;
  result.generation_id    = request.ntn_rnti_lease_update.generation_id;
  result.accepted         = true;
  result.accepted_leases  = request.ntn_rnti_lease_update.leases;
  result.rejected_leases  = {};
  result.reject_reason    = "accepted";

  f1ap_message response;
  response.pdu.set_successful_outcome().load_info_obj(ASN1_F1AP_ID_GNB_DU_RES_COORDINATION);
  auto& asn1_resp = response.pdu.successful_outcome().value.gnb_du_res_coordination_resp();
  asn1_resp->transaction_id = f1ap_pdu_notifier.last_f1ap_msg.pdu.init_msg()
                                  .value.gnb_du_res_coordination_request()
                                  ->transaction_id;
  asn1_resp->eutra_nr_cell_res_coordination_req_ack_container =
      encode_f1ap_ntn_rnti_lease_pool_result(result);

  f1ap->handle_message(response);

  ASSERT_TRUE(task.ready());
  ASSERT_TRUE(task.get().result.has_value());
  EXPECT_TRUE(task.get().result->accepted);
  EXPECT_EQ(task.get().result->generation_id, result.generation_id);
}

TEST_F(f1ap_cu_gnbdu_resource_coordination_test, rnti_lease_result_with_wrong_generation_is_rejected)
{
  f1ap_gnb_du_resource_coordination_request request;
  request.ntn_rnti_lease_update = make_lease_update();
  start_procedure(request);

  f1ap_ntn_rnti_lease_pool_result result;
  result.generation_id   = request.ntn_rnti_lease_update.generation_id + 1;
  result.accepted        = true;
  result.accepted_leases = request.ntn_rnti_lease_update.leases;

  f1ap_message response;
  response.pdu.set_successful_outcome().load_info_obj(ASN1_F1AP_ID_GNB_DU_RES_COORDINATION);
  auto& asn1_resp = response.pdu.successful_outcome().value.gnb_du_res_coordination_resp();
  asn1_resp->transaction_id = f1ap_pdu_notifier.last_f1ap_msg.pdu.init_msg()
                                  .value.gnb_du_res_coordination_request()
                                  ->transaction_id;
  asn1_resp->eutra_nr_cell_res_coordination_req_ack_container = encode_f1ap_ntn_rnti_lease_pool_result(result);

  f1ap->handle_message(response);

  ASSERT_TRUE(task.ready());
  EXPECT_FALSE(task.get().success);
  EXPECT_FALSE(task.get().result.has_value());
}

TEST_F(f1ap_cu_gnbdu_resource_coordination_test, audit_request_is_sent_and_audit_result_completes_procedure)
{
  f1ap_gnb_du_resource_coordination_request request;
  request.ntn_resource_audit_request.du_index      = uint_to_du_index(2);
  request.ntn_resource_audit_request.cell_index    = to_du_cell_index(1);
  request.ntn_resource_audit_request.pci           = pci_t{17};
  request.ntn_resource_audit_request.generation_id = 42;

  start_procedure(request);

  const auto& asn1_req =
      f1ap_pdu_notifier.last_f1ap_msg.pdu.init_msg().value.gnb_du_res_coordination_request();
  const auto decoded = decode_f1ap_ntn_resource_audit_request(asn1_req->eutra_nr_cell_res_coordination_req_container);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->generation_id, request.ntn_resource_audit_request.generation_id);

  f1ap_ntn_resource_audit_result result;
  result.generation_id = request.ntn_resource_audit_request.generation_id;
  result.accepted      = true;

  f1ap_message response;
  response.pdu.set_successful_outcome().load_info_obj(ASN1_F1AP_ID_GNB_DU_RES_COORDINATION);
  auto& asn1_resp = response.pdu.successful_outcome().value.gnb_du_res_coordination_resp();
  asn1_resp->transaction_id = asn1_req->transaction_id;
  asn1_resp->eutra_nr_cell_res_coordination_req_ack_container = encode_f1ap_ntn_resource_audit_result(result);

  f1ap->handle_message(response);

  ASSERT_TRUE(task.ready());
  ASSERT_TRUE(task.get().audit_result.has_value());
  EXPECT_TRUE(task.get().audit_result->accepted);
  EXPECT_EQ(task.get().audit_result->generation_id, result.generation_id);
}

TEST_F(f1ap_cu_gnbdu_resource_coordination_test, audit_result_with_wrong_generation_is_rejected)
{
  f1ap_gnb_du_resource_coordination_request request;
  request.ntn_resource_audit_request.du_index      = uint_to_du_index(2);
  request.ntn_resource_audit_request.cell_index    = to_du_cell_index(1);
  request.ntn_resource_audit_request.pci           = pci_t{17};
  request.ntn_resource_audit_request.generation_id = 42;
  start_procedure(request);

  const auto& asn1_req = f1ap_pdu_notifier.last_f1ap_msg.pdu.init_msg().value.gnb_du_res_coordination_request();
  f1ap_ntn_resource_audit_result result;
  result.generation_id = request.ntn_resource_audit_request.generation_id + 1;
  result.accepted      = true;

  f1ap_message response;
  response.pdu.set_successful_outcome().load_info_obj(ASN1_F1AP_ID_GNB_DU_RES_COORDINATION);
  auto& asn1_resp           = response.pdu.successful_outcome().value.gnb_du_res_coordination_resp();
  asn1_resp->transaction_id = asn1_req->transaction_id;
  asn1_resp->eutra_nr_cell_res_coordination_req_ack_container = encode_f1ap_ntn_resource_audit_result(result);

  f1ap->handle_message(response);

  ASSERT_TRUE(task.ready());
  EXPECT_FALSE(task.get().success);
  EXPECT_FALSE(task.get().audit_result.has_value());
}

TEST_F(f1ap_cu_gnbdu_resource_coordination_test, sib19_update_request_is_sent_and_result_completes_procedure)
{
  f1ap_gnb_du_resource_coordination_request request;
  request.ntn_sib19_broadcast_update.du_index      = uint_to_du_index(1);
  request.ntn_sib19_broadcast_update.cell_index    = to_du_cell_index(0);
  request.ntn_sib19_broadcast_update.pci           = pci_t{42};
  request.ntn_sib19_broadcast_update.beam_id       = "CN-BEAM-0001";
  request.ntn_sib19_broadcast_update.nci           = nr_cell_identity::create(0x66c001).value();
  request.ntn_sib19_broadcast_update.generation_id = 18;
  request.ntn_sib19_broadcast_update.operation     = f1ap_ntn_sib19_broadcast_operation::update;
  request.ntn_sib19_broadcast_update.si_msg_idx    = 0;
  request.ntn_sib19_broadcast_update.sib_idx       = 19;
  request.ntn_sib19_broadcast_update.valid_from    = slot_point{0, 20};
  const std::array<uint8_t, 1> pdu                 = {0x7e};
  request.ntn_sib19_broadcast_update.packed_sib19  = byte_buffer::create(pdu).value();

  start_procedure(request);

  ASSERT_TRUE(this->f1ap_pdu_notifier.last_f1ap_msg.pdu.type().value ==
              asn1::f1ap::f1ap_pdu_c::types_opts::init_msg);
  const auto& asn1_req =
      this->f1ap_pdu_notifier.last_f1ap_msg.pdu.init_msg().value.gnb_du_res_coordination_request();
  const auto decoded = decode_f1ap_ntn_sib19_broadcast_update(asn1_req->eutra_nr_cell_res_coordination_req_container);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->generation_id, request.ntn_sib19_broadcast_update.generation_id);
  EXPECT_EQ(decoded->beam_id, request.ntn_sib19_broadcast_update.beam_id);

  f1ap_ntn_sib19_broadcast_result result;
  result.generation_id = request.ntn_sib19_broadcast_update.generation_id;
  result.status        = f1ap_ntn_sib19_broadcast_result_status::applied;
  f1ap_message response;
  response.pdu.set_successful_outcome().load_info_obj(ASN1_F1AP_ID_GNB_DU_RES_COORDINATION);
  auto& asn1_resp = response.pdu.successful_outcome().value.gnb_du_res_coordination_resp();
  asn1_resp->transaction_id = f1ap_pdu_notifier.last_f1ap_msg.pdu.init_msg()
                                  .value.gnb_du_res_coordination_request()
                                  ->transaction_id;
  asn1_resp->eutra_nr_cell_res_coordination_req_ack_container = encode_f1ap_ntn_sib19_broadcast_result(result);

  this->f1ap->handle_message(response);
  ASSERT_TRUE(task.ready());
  ASSERT_TRUE(task.get().sib19_result.has_value());
  EXPECT_TRUE(task.get().sib19_result->accepted());
}

TEST_F(f1ap_cu_gnbdu_resource_coordination_test, sib19_result_with_wrong_generation_is_rejected)
{
  f1ap_gnb_du_resource_coordination_request request;
  request.ntn_sib19_broadcast_update.du_index      = uint_to_du_index(1);
  request.ntn_sib19_broadcast_update.cell_index    = to_du_cell_index(0);
  request.ntn_sib19_broadcast_update.pci           = pci_t{42};
  request.ntn_sib19_broadcast_update.beam_id       = "CN-BEAM-0001";
  request.ntn_sib19_broadcast_update.nci           = nr_cell_identity::create(0x66c001).value();
  request.ntn_sib19_broadcast_update.generation_id = 18;
  request.ntn_sib19_broadcast_update.operation     = f1ap_ntn_sib19_broadcast_operation::clear;
  request.ntn_sib19_broadcast_update.si_msg_idx    = 0;
  request.ntn_sib19_broadcast_update.sib_idx       = 19;
  start_procedure(request);

  const auto& asn1_req = f1ap_pdu_notifier.last_f1ap_msg.pdu.init_msg().value.gnb_du_res_coordination_request();
  f1ap_ntn_sib19_broadcast_result result;
  result.generation_id = request.ntn_sib19_broadcast_update.generation_id + 1;
  result.status        = f1ap_ntn_sib19_broadcast_result_status::clear_applied;

  f1ap_message response;
  response.pdu.set_successful_outcome().load_info_obj(ASN1_F1AP_ID_GNB_DU_RES_COORDINATION);
  auto& asn1_resp           = response.pdu.successful_outcome().value.gnb_du_res_coordination_resp();
  asn1_resp->transaction_id = asn1_req->transaction_id;
  asn1_resp->eutra_nr_cell_res_coordination_req_ack_container = encode_f1ap_ntn_sib19_broadcast_result(result);

  f1ap->handle_message(response);

  ASSERT_TRUE(task.ready());
  EXPECT_FALSE(task.get().success);
  EXPECT_FALSE(task.get().sib19_result.has_value());
}
