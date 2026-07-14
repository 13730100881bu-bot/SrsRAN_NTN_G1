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

#include "lib/f1ap/asn1_helpers.h"
#include "srsran/asn1/f1ap/f1ap.h"
#include "srsran/f1ap/ntn_ul_slot_resource_request.h"
#include "srsran/ran/nr_cgi.h"
#include "srsran/ran/up_transport_layer_info.h"
#include "srsran/support/test_utils.h"
#include <gtest/gtest.h>

using namespace srsran;

static byte_buffer make_ntn_ul_slot_request_container(std::initializer_list<uint8_t> bytes)
{
  return byte_buffer::create(bytes).value();
}

TEST(f1ap_ntn_ul_slot_resource_request_test, when_offsets_and_periods_are_encoded_then_v2_payload_round_trips)
{
  f1ap_ntn_ul_slot_resource_request request;
  request.sr_slot_offset  = 3U;
  request.srs_slot_offset = 5U;
  request.sr_slot_period  = 10U;
  request.srs_slot_period = 20U;

  const byte_buffer payload = encode_f1ap_ntn_ul_slot_resource_request(request);

  ASSERT_EQ(payload.length(), 25U);
  ASSERT_EQ(payload[0], 'S');
  ASSERT_EQ(payload[7], '2');
  ASSERT_EQ(payload[8], 0x0f);

  const std::optional<f1ap_ntn_ul_slot_resource_request> decoded_request =
      decode_f1ap_ntn_ul_slot_resource_request(payload);
  ASSERT_TRUE(decoded_request.has_value());
  ASSERT_EQ(decoded_request->sr_slot_offset, 3U);
  ASSERT_EQ(decoded_request->srs_slot_offset, 5U);
  ASSERT_EQ(decoded_request->sr_slot_period, 10U);
  ASSERT_EQ(decoded_request->srs_slot_period, 20U);
}

TEST(f1ap_ntn_ul_slot_resource_request_test, when_legacy_v1_payload_is_decoded_then_offsets_are_preserved)
{
  const byte_buffer payload = make_ntn_ul_slot_request_container(
      {'S', 'R', 'S', 'N', 'T', 'N', '0', '1', 0x03, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x0b});

  const std::optional<f1ap_ntn_ul_slot_resource_request> decoded_request =
      decode_f1ap_ntn_ul_slot_resource_request(payload);

  ASSERT_TRUE(decoded_request.has_value());
  ASSERT_EQ(decoded_request->sr_slot_offset, 7U);
  ASSERT_EQ(decoded_request->srs_slot_offset, 11U);
  ASSERT_FALSE(decoded_request->sr_slot_period.has_value());
  ASSERT_FALSE(decoded_request->srs_slot_period.has_value());
}

TEST(f1ap_ntn_ul_slot_resource_request_test, when_v2_period_flag_has_no_matching_offset_then_payload_is_rejected)
{
  const byte_buffer payload = make_ntn_ul_slot_request_container({'S',
                                                                 'R',
                                                                 'S',
                                                                 'N',
                                                                 'T',
                                                                 'N',
                                                                 '0',
                                                                 '2',
                                                                 0x04,
                                                                 0x00,
                                                                 0x00,
                                                                 0x00,
                                                                 0x00,
                                                                 0x00,
                                                                 0x00,
                                                                 0x00,
                                                                 0x00,
                                                                 0x00,
                                                                 0x00,
                                                                 0x00,
                                                                 0x0a,
                                                                 0x00,
                                                                 0x00,
                                                                 0x00,
                                                                 0x00});

  ASSERT_FALSE(decode_f1ap_ntn_ul_slot_resource_request(payload).has_value());
}

TEST(f1ap_ntn_ul_slot_resource_request_test, when_empty_v2_payload_is_decoded_then_it_clears_slot_request)
{
  f1ap_ntn_ul_slot_resource_request request;

  const byte_buffer payload = encode_f1ap_ntn_ul_slot_resource_request(request);

  ASSERT_EQ(payload.length(), 25U);
  ASSERT_EQ(payload[8], 0x00);
  const std::optional<f1ap_ntn_ul_slot_resource_request> decoded_request =
      decode_f1ap_ntn_ul_slot_resource_request(payload);
  ASSERT_TRUE(decoded_request.has_value());
  ASSERT_TRUE(is_empty(*decoded_request));
}

TEST(f1ap_ntn_ul_slot_resource_request_test, when_zero_period_is_requested_then_encoder_omits_period_flag)
{
  f1ap_ntn_ul_slot_resource_request request;
  request.sr_slot_offset = 3U;
  request.sr_slot_period = 0U;

  const byte_buffer payload = encode_f1ap_ntn_ul_slot_resource_request(request);

  ASSERT_EQ(payload[8], 0x01);
  const std::optional<f1ap_ntn_ul_slot_resource_request> decoded_request =
      decode_f1ap_ntn_ul_slot_resource_request(payload);
  ASSERT_TRUE(decoded_request.has_value());
  ASSERT_EQ(decoded_request->sr_slot_offset, 3U);
  ASSERT_FALSE(decoded_request->sr_slot_period.has_value());
}

/// Test PLMN decoding
TEST(f1ap_asn1_helpers_test, test_ngi_converter_for_valid_plmn)
{
  // use known a PLMN
  asn1::f1ap::nr_cgi_s asn1_cgi;
  asn1_cgi.plmn_id.from_string("00f110"); // 001.01
  asn1_cgi.nr_cell_id.from_number(6576);

  // convert to internal NGI representation
  nr_cell_global_id_t ngi = cgi_from_asn1(asn1_cgi).value();
  ASSERT_EQ("00101", ngi.plmn_id.to_string()); // human-readable PLMN
}

TEST(f1ap_asn1_helpers_test, test_ngi_converter_for_invalid_plmn)
{
  // use known a PLMN
  asn1::f1ap::nr_cgi_s asn1_cgi;
  asn1_cgi.plmn_id.from_string("00f00a"); // 000.0a
  asn1_cgi.nr_cell_id.from_number(6576);

  // convert to internal NGI representation
  auto ngi = cgi_from_asn1(asn1_cgi);
  ASSERT_FALSE(ngi.has_value());
}

static std::string create_random_ipv4_string()
{
  std::vector<uint8_t> nums = test_rgen::random_vector<uint8_t>(4);
  return fmt::format("{}.{}.{}.{}", nums[0], nums[1], nums[2], nums[3]);
}

static std::string create_random_ipv6_string()
{
  std::vector<uint16_t> nums = test_rgen::random_vector<uint16_t>(8);
  return fmt::format("{:x}:{:x}:{:x}:{:x}:{:x}:{:x}:{:x}:{:x}",
                     nums[0],
                     nums[1],
                     nums[2],
                     nums[3],
                     nums[4],
                     nums[5],
                     nums[6],
                     nums[7]);
}

static std::string generate_random_ipv4_bitstring()
{
  uint32_t    random_number = test_rgen::uniform_int<uint32_t>();
  std::string bitstr        = fmt::format("{:032b}", random_number);

  return bitstr;
}

static std::string generate_random_ipv6_bitstring()
{
  std::string bitstr;

  for (int i = 0; i < 2; i++) { // we need 128 bits for ipv6
    uint64_t random_number = test_rgen::uniform_int<uint64_t>();
    bitstr                 = bitstr + fmt::format("{:064b}", random_number);
  }

  return bitstr;
}

static uint32_t generate_gtp_teid()
{
  return test_rgen::uniform_int<uint32_t>();
}

TEST(f1ap_asn1_helpers_test, test_up_transport_layer_converter)
{
  up_transport_layer_info up_tp_layer_info = {transport_layer_address::create_from_string(create_random_ipv4_string()),
                                              int_to_gtpu_teid(0x1)};

  asn1::f1ap::up_transport_layer_info_c asn1_transport_layer_info;

  up_transport_layer_info_to_asn1(asn1_transport_layer_info, up_tp_layer_info);

  ASSERT_EQ(up_tp_layer_info.gtp_teid, int_to_gtpu_teid(asn1_transport_layer_info.gtp_tunnel().gtp_teid.to_number()));
  ASSERT_EQ(up_tp_layer_info.tp_address.to_bitstring(),
            asn1_transport_layer_info.gtp_tunnel().transport_layer_address.to_string());
}

TEST(transport_layer_address_test, ipv6_transport_layer_address_to_asn1)
{
  up_transport_layer_info up_tp_layer_info = {transport_layer_address::create_from_string(create_random_ipv6_string()),
                                              int_to_gtpu_teid(0x1)};

  asn1::f1ap::up_transport_layer_info_c asn1_transport_layer_info;

  up_transport_layer_info_to_asn1(asn1_transport_layer_info, up_tp_layer_info);

  ASSERT_EQ(up_tp_layer_info.gtp_teid, int_to_gtpu_teid(asn1_transport_layer_info.gtp_tunnel().gtp_teid.to_number()));
  ASSERT_EQ(up_tp_layer_info.tp_address.to_bitstring(),
            asn1_transport_layer_info.gtp_tunnel().transport_layer_address.to_string());
}

TEST(transport_layer_address_test, asn1_to_ipv4_transport_layer_address)
{
  asn1::f1ap::up_transport_layer_info_c asn1_transport_layer_info;
  asn1_transport_layer_info.set_gtp_tunnel().gtp_teid.from_number(generate_gtp_teid());
  asn1_transport_layer_info.set_gtp_tunnel().transport_layer_address.from_string(generate_random_ipv4_bitstring());

  // ASN1 -> internal representation.
  up_transport_layer_info up_tp_layer_info = asn1_to_up_transport_layer_info(asn1_transport_layer_info);

  ASSERT_EQ(up_tp_layer_info.gtp_teid, int_to_gtpu_teid(asn1_transport_layer_info.gtp_tunnel().gtp_teid.to_number()));
  ASSERT_EQ(up_tp_layer_info.tp_address.to_bitstring(),
            asn1_transport_layer_info.gtp_tunnel().transport_layer_address.to_string());
}

TEST(transport_layer_address_test, asn1_to_ipv6_transport_layer_address)
{
  asn1::f1ap::up_transport_layer_info_c asn1_transport_layer_info;
  asn1_transport_layer_info.set_gtp_tunnel().gtp_teid.from_number(0x1);
  asn1_transport_layer_info.set_gtp_tunnel().transport_layer_address.from_string(generate_random_ipv6_bitstring());

  up_transport_layer_info up_tp_layer_info = asn1_to_up_transport_layer_info(asn1_transport_layer_info);

  ASSERT_EQ(up_tp_layer_info.gtp_teid, int_to_gtpu_teid(asn1_transport_layer_info.gtp_tunnel().gtp_teid.to_number()));
  ASSERT_EQ(up_tp_layer_info.tp_address.to_bitstring(),
            asn1_transport_layer_info.gtp_tunnel().transport_layer_address.to_string());
}
