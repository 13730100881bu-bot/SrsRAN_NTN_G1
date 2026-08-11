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

#include "phy_to_fapi_results_event_fastpath_translator.h"
#include "srsran/fapi/messages/rach_indication.h"
#include "srsran/srslog/srslog.h"
#include <gtest/gtest.h>

using namespace srsran;
using namespace fapi_adaptor;

namespace {

class slot_data_message_notifier_spy : public fapi::slot_data_message_notifier
{
public:
  void on_rx_data_indication(const fapi::rx_data_indication_message&) override {}
  void on_crc_indication(const fapi::crc_indication_message&) override {}
  void on_uci_indication(const fapi::uci_indication_message&) override {}
  void on_srs_indication(const fapi::srs_indication_message&) override {}
  void on_rach_indication(const fapi::rach_indication_message& msg) override
  {
    rach_notified = true;
    rach_message  = msg;
  }

  bool                          rach_notified = false;
  fapi::rach_indication_message rach_message;
};

} // namespace

TEST(phy_to_fapi_results_event_fastpath_translator_test, prach_handle_and_physical_port_attribution_are_preserved)
{
  slot_data_message_notifier_spy                notifier;
  auto&                                         logger = srslog::fetch_basic_logger("TEST");
  phy_to_fapi_results_event_fastpath_translator translator(0, 1.0F, logger);
  translator.set_slot_data_message_notifier(notifier);

  ul_prach_results result{};
  result.context.slot         = slot_point(0, 1, 2);
  result.context.start_symbol = 4;
  result.context.handle       = 0x12345678U;
  result.context.calendar_position_valid = true;
  result.context.calendar_schedule_version = 41U;
  result.context.calendar_cycle_index    = 160U;
  result.context.occasion_offset_us      = 5000U;
  result.context.ports.push_back(3);
  result.context.ports.push_back(7);
  auto verified_contexts = std::make_shared<verified_prach_rx_context_list>();
  verified_prach_rx_context first_context;
  first_context.authority          = prach_rx_context_authority::ofh_beam_id_verified;
  first_context.buffer_port        = 0;
  first_context.logical_port_id    = 6;
  first_context.ofh_prach_eaxc     = 4;
  first_context.ofh_beam_id        = 0x1233;
  first_context.position_id        = "G000122";
  first_context.schedule_version   = 41;
  first_context.calendar_hash      = "calendar-sha256";
  first_context.mapping_generation = 9;
  first_context.mapping_hash       = "mapping-sha256";
  verified_contexts->push_back(first_context);
  verified_prach_rx_context second_context = first_context;
  second_context.buffer_port                = 1;
  second_context.logical_port_id            = 7;
  second_context.ofh_prach_eaxc             = 5;
  second_context.ofh_beam_id                = 0x1234;
  second_context.position_id                = "G000123";
  verified_contexts->push_back(second_context);
  result.context.verified_rx_contexts = verified_contexts;
  result.result.rssi_dB = -10.0F;

  auto& preamble                                          = result.result.preambles.emplace_back();
  preamble.preamble_index                                 = 5;
  preamble.time_advance                                   = phy_time_unit::from_seconds(0.0);
  preamble.detection_metric                               = 2.0F;
  preamble.preamble_power_dB                              = -20.0F;
  preamble.port_attribution.status                        = prach_detection_result::rx_port_attribution_status::unique;
  preamble.port_attribution.strongest_port_index          = 1;
  preamble.port_attribution.strongest_to_second_margin_dB = 8.5F;

  translator.on_new_prach_results(result);

  // The translated indication owns an aliasing shared pointer to the selected list entry. Drop both source owners to
  // verify that the selected metadata remains alive with the FAPI indication.
  verified_contexts.reset();
  result.context.verified_rx_contexts.reset();

  ASSERT_TRUE(notifier.rach_notified);
  ASSERT_EQ(1U, notifier.rach_message.pdus.size());
  const fapi::rach_indication_pdu& pdu = notifier.rach_message.pdus.front();
  EXPECT_EQ(result.context.handle, pdu.handle);
  EXPECT_TRUE(pdu.calendar_position_valid);
  EXPECT_EQ(pdu.calendar_schedule_version, 41U);
  EXPECT_EQ(pdu.calendar_cycle_index, 160U);
  EXPECT_EQ(pdu.occasion_offset_us, 5000U);
  ASSERT_EQ(1U, pdu.preambles.size());
  const fapi::rach_indication_pdu_preamble& fapi_preamble = pdu.preambles.front();
  EXPECT_EQ(fapi::prach_rx_port_attribution_status::unique, fapi_preamble.port_attribution_status);
  EXPECT_EQ(7U, fapi_preamble.strongest_rx_port);
  EXPECT_FLOAT_EQ(8.5F, fapi_preamble.strongest_to_second_margin_dB);
  ASSERT_NE(fapi_preamble.verified_rx_context, nullptr);
  EXPECT_EQ(prach_rx_context_authority::ofh_beam_id_verified, fapi_preamble.verified_rx_context->authority);
  // The detector selects buffer index 1, while the externally visible physical receive port is 7.
  EXPECT_EQ(1U, fapi_preamble.verified_rx_context->buffer_port);
  EXPECT_EQ(7, fapi_preamble.verified_rx_context->logical_port_id);
  ASSERT_TRUE(fapi_preamble.verified_rx_context->ofh_prach_eaxc.has_value());
  EXPECT_EQ(5, fapi_preamble.verified_rx_context->ofh_prach_eaxc.value());
  ASSERT_TRUE(fapi_preamble.verified_rx_context->ofh_beam_id.has_value());
  EXPECT_EQ(0x1234, fapi_preamble.verified_rx_context->ofh_beam_id.value());
  EXPECT_EQ("G000123", fapi_preamble.verified_rx_context->position_id);
  EXPECT_EQ(41, fapi_preamble.verified_rx_context->schedule_version);
  EXPECT_EQ("calendar-sha256", fapi_preamble.verified_rx_context->calendar_hash);
  EXPECT_EQ(9, fapi_preamble.verified_rx_context->mapping_generation);
  EXPECT_EQ("mapping-sha256", fapi_preamble.verified_rx_context->mapping_hash);
}

TEST(phy_to_fapi_results_event_fastpath_translator_test, duplicate_verified_buffer_port_is_not_forwarded)
{
  slot_data_message_notifier_spy                notifier;
  auto&                                         logger = srslog::fetch_basic_logger("TEST");
  phy_to_fapi_results_event_fastpath_translator translator(0, 1.0F, logger);
  translator.set_slot_data_message_notifier(notifier);

  ul_prach_results result{};
  result.context.slot         = slot_point(0, 1, 2);
  result.context.start_symbol = 4;
  result.context.handle       = 0x12345678U;
  result.context.ports        = {3, 7};
  auto contexts              = std::make_shared<verified_prach_rx_context_list>();
  verified_prach_rx_context context;
  context.authority          = prach_rx_context_authority::ofh_beam_id_verified;
  context.buffer_port        = 1;
  context.logical_port_id    = 7;
  context.ofh_prach_eaxc     = 5;
  context.ofh_beam_id        = 0x1234;
  context.position_id        = "G000123";
  context.schedule_version   = 41;
  context.calendar_hash      = "calendar-sha256";
  context.mapping_generation = 9;
  context.mapping_hash       = "mapping-sha256";
  contexts->push_back(context);
  context.ofh_prach_eaxc = 6;
  contexts->push_back(context);
  result.context.verified_rx_contexts = contexts;
  result.result.rssi_dB               = -10.0F;

  auto& preamble                                          = result.result.preambles.emplace_back();
  preamble.preamble_index                                 = 5;
  preamble.time_advance                                   = phy_time_unit::from_seconds(0.0);
  preamble.preamble_power_dB                              = -20.0F;
  preamble.port_attribution.status                        = prach_detection_result::rx_port_attribution_status::unique;
  preamble.port_attribution.strongest_port_index          = 1;
  preamble.port_attribution.strongest_to_second_margin_dB = 8.5F;

  translator.on_new_prach_results(result);

  ASSERT_TRUE(notifier.rach_notified);
  ASSERT_EQ(notifier.rach_message.pdus.size(), 1);
  ASSERT_EQ(notifier.rach_message.pdus.front().preambles.size(), 1);
  EXPECT_EQ(notifier.rach_message.pdus.front().preambles.front().verified_rx_context, nullptr);
}
