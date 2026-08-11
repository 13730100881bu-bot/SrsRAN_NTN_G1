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

#include "fapi_to_mac_data_msg_fastpath_translator.h"
#include "srsran/fapi/messages/rach_indication.h"
#include <gtest/gtest.h>

using namespace srsran;
using namespace fapi_adaptor;

namespace {
class mac_cell_rach_handler_dummy : public mac_cell_rach_handler
{
  mac_rach_indication indication;

public:
  void handle_rach_indication(const mac_rach_indication& rach_ind) override { indication = rach_ind; }

  const mac_rach_indication& get_indication() const { return indication; }
};
} // namespace

class mac_rach_indication_fixture : public testing::TestWithParam<float>
{
  mac_cell_rach_handler_dummy              rach_handler;
  fapi_to_mac_data_msg_fastpath_translator translator;

protected:
  mac_rach_indication_fixture() : translator(subcarrier_spacing::kHz15, 0)
  {
    translator.set_cell_rach_handler(rach_handler);
  }

  void test_pdu()
  {
    const fapi::rach_indication_message& msg = build_message();
    translator.on_rach_indication(msg);
    check_pdu();
  }

  void test_power_values(float value)
  {
    rssi  = std::clamp(value, -140.F, 30.F);
    power = std::clamp(value, -140.F, 30.F);
    test_pdu();
  }

  void test_default_port_attribution()
  {
    include_port_attribution = false;
    test_pdu();
  }

  void test_ambiguous_port_attribution()
  {
    fapi_port_attribution_status = fapi::prach_rx_port_attribution_status::ambiguous;
    test_pdu();
  }

  void test_verified_ofh_context()
  {
    include_verified_context = true;
    test_pdu();
  }

  void test_ambiguous_verified_ofh_context()
  {
    include_verified_context     = true;
    fapi_port_attribution_status = fapi::prach_rx_port_attribution_status::ambiguous;
    test_pdu();
  }

  void test_default_calendar_position()
  {
    include_calendar_position = false;
    test_pdu();
  }

  std::weak_ptr<const verified_prach_rx_context> translate_verified_context_and_release_fapi_message()
  {
    include_verified_context = true;
    std::weak_ptr<const verified_prach_rx_context> source_context;
    {
      fapi::rach_indication_message msg = build_message();
      source_context                    = msg.pdus.front().preambles.front().verified_rx_context;
      translator.on_rach_indication(msg);
    }
    return source_context;
  }

  const mac_rach_indication& translated_indication() const { return rach_handler.get_indication(); }

private:
  unsigned                               slot                         = 0;
  unsigned                               sfn                          = 1;
  float                                  rssi                         = 14.F;
  unsigned                               slot_index                   = 3;
  unsigned                               start_symbol                 = 4;
  unsigned                               freq_index                   = 5;
  float                                  snr                          = 12.F;
  float                                  power                        = 13.F;
  unsigned                               time_advance_ns              = 8;
  uint32_t                               handle                       = 0x12345678U;
  uint8_t                                strongest_port               = 7;
  float                                  port_margin_dB               = 8.5F;
  bool                                   include_port_attribution     = true;
  bool                                   include_verified_context     = false;
  bool                                   include_calendar_position    = true;
  fapi::prach_rx_port_attribution_status fapi_port_attribution_status = fapi::prach_rx_port_attribution_status::unique;

  fapi::rach_indication_message build_message()
  {
    fapi::rach_indication_message fapi_msg;
    fapi_msg.sfn  = sfn;
    fapi_msg.slot = slot;

    fapi_msg.pdus.emplace_back();
    fapi_msg.num_pdu = fapi_msg.pdus.size();

    fapi::rach_indication_pdu& pdu = fapi_msg.pdus.back();
    pdu.handle                     = handle;
    pdu.avg_rssi                   = (rssi + 140) * 1000;
    pdu.symbol_index               = start_symbol;
    pdu.slot_index                 = slot_index;
    pdu.ra_index                   = freq_index;
    if (include_calendar_position) {
      pdu.calendar_position_valid = true;
      pdu.calendar_schedule_version = 41U;
      pdu.calendar_cycle_index    = 160U;
      pdu.occasion_offset_us      = 5000U;
    }
    pdu.preambles.emplace_back();
    fapi::rach_indication_pdu_preamble& preamble = pdu.preambles.back();
    preamble.preamble_snr                        = (snr + 64) * 2;
    preamble.preamble_pwr                        = (power + 140) * 1000;
    preamble.timing_advance_offset_ns            = time_advance_ns;
    if (include_port_attribution) {
      preamble.port_attribution_status       = fapi_port_attribution_status;
      preamble.strongest_rx_port             = strongest_port;
      preamble.strongest_to_second_margin_dB = port_margin_dB;
    }
    if (include_verified_context) {
      auto context                = std::make_shared<verified_prach_rx_context>();
      context->authority          = prach_rx_context_authority::ofh_beam_id_verified;
      context->buffer_port        = 0;
      context->logical_port_id    = 7;
      context->ofh_prach_eaxc     = 5;
      context->ofh_beam_id        = 0x1234;
      context->position_id        = "G000123";
      context->schedule_version   = 41;
      context->calendar_hash      = "calendar-sha256";
      context->mapping_generation = 9;
      context->mapping_hash       = "mapping-sha256";
      preamble.verified_rx_context = context;
    }

    return fapi_msg;
  }

  void check_pdu()
  {
    const mac_rach_indication&                msg = rach_handler.get_indication();
    const mac_rach_indication::rach_occasion& occ = msg.occasions.front();
    EXPECT_EQ(handle, occ.handle);
    EXPECT_EQ(start_symbol, occ.start_symbol);
    EXPECT_EQ(slot_index, occ.slot_index);
    EXPECT_EQ(freq_index, occ.frequency_index);
    EXPECT_FLOAT_EQ(rssi, occ.rssi_dBFS.value());
    EXPECT_EQ(include_calendar_position, occ.calendar_position_valid);
    EXPECT_EQ(include_calendar_position ? 41U : 0U, occ.calendar_schedule_version);
    EXPECT_EQ(include_calendar_position ? 160U : 0U, occ.calendar_cycle_index);
    EXPECT_EQ(include_calendar_position ? 5000U : 0U, occ.occasion_offset_us);

    const mac_rach_indication::rach_preamble& pream = occ.preambles.front();
    EXPECT_FLOAT_EQ(power, pream.pwr_dBFS.value());
    EXPECT_EQ(time_advance_ns, std::round(pream.time_advance.to_seconds() * 1e9));
    if (include_port_attribution) {
      const mac_rach_indication::rx_port_attribution_status expected_status =
          fapi_port_attribution_status == fapi::prach_rx_port_attribution_status::unique
              ? mac_rach_indication::rx_port_attribution_status::unique
              : mac_rach_indication::rx_port_attribution_status::ambiguous;
      EXPECT_EQ(expected_status, pream.port_attribution_status);
      ASSERT_TRUE(pream.strongest_rx_port.has_value());
      EXPECT_EQ(strongest_port, pream.strongest_rx_port.value());
      ASSERT_TRUE(pream.strongest_to_second_margin_dB.has_value());
      EXPECT_FLOAT_EQ(port_margin_dB, pream.strongest_to_second_margin_dB.value());
    } else {
      EXPECT_EQ(mac_rach_indication::rx_port_attribution_status::unavailable, pream.port_attribution_status);
      EXPECT_FALSE(pream.strongest_rx_port.has_value());
      EXPECT_FALSE(pream.strongest_to_second_margin_dB.has_value());
    }

    if (include_verified_context &&
        fapi_port_attribution_status == fapi::prach_rx_port_attribution_status::unique) {
      ASSERT_NE(pream.verified_context, nullptr);
      EXPECT_EQ(prach_rx_context_authority::ofh_beam_id_verified, pream.verified_context->authority);
      EXPECT_EQ(7, pream.verified_context->logical_port_id);
      ASSERT_TRUE(pream.verified_context->ofh_prach_eaxc.has_value());
      EXPECT_EQ(5, pream.verified_context->ofh_prach_eaxc.value());
      ASSERT_TRUE(pream.verified_context->ofh_beam_id.has_value());
      EXPECT_EQ(0x1234, pream.verified_context->ofh_beam_id.value());
      EXPECT_EQ("G000123", pream.verified_context->position_id);
      EXPECT_EQ(41, pream.verified_context->schedule_version);
      EXPECT_EQ("calendar-sha256", pream.verified_context->calendar_hash);
      EXPECT_EQ(9, pream.verified_context->mapping_generation);
      EXPECT_EQ("mapping-sha256", pream.verified_context->mapping_hash);
    } else {
      EXPECT_EQ(pream.verified_context, nullptr);
    }
  }
};

TEST_P(mac_rach_indication_fixture, PowerRelatedValuesWorks)
{
  test_power_values(GetParam());
}

INSTANTIATE_TEST_SUITE_P(PowerValues, mac_rach_indication_fixture, testing::Values(-140.F, -20.F, 0.F, 30.F, 63.F));

TEST_F(mac_rach_indication_fixture, CorrectMessageConvertsCorrectly)
{
  test_pdu();
}

TEST_F(mac_rach_indication_fixture, DefaultPortAttributionRemainsUnavailable)
{
  test_default_port_attribution();
}

TEST_F(mac_rach_indication_fixture, DefaultCalendarPositionRemainsUnavailable)
{
  test_default_calendar_position();
}

TEST_F(mac_rach_indication_fixture, AmbiguousPortAttributionIsPreserved)
{
  test_ambiguous_port_attribution();
}

TEST_F(mac_rach_indication_fixture, VerifiedOfhContextIsPreservedForUniquePort)
{
  test_verified_ofh_context();
}

TEST_F(mac_rach_indication_fixture, VerifiedOfhContextIsNotPreservedForAmbiguousPort)
{
  test_ambiguous_verified_ofh_context();
}

TEST_F(mac_rach_indication_fixture, VerifiedOfhContextLifetimeExtendsPastFapiMessage)
{
  const std::weak_ptr<const verified_prach_rx_context> source_context =
      translate_verified_context_and_release_fapi_message();

  ASSERT_FALSE(source_context.expired());
  const auto& mac_preamble = translated_indication().occasions.front().preambles.front();
  ASSERT_NE(mac_preamble.verified_context, nullptr);
  EXPECT_EQ(source_context.lock().get(), mac_preamble.verified_context.get());
  EXPECT_EQ(0U, mac_preamble.verified_context->buffer_port);
  ASSERT_TRUE(mac_preamble.strongest_rx_port.has_value());
  EXPECT_EQ(7U, mac_preamble.strongest_rx_port.value());
}
