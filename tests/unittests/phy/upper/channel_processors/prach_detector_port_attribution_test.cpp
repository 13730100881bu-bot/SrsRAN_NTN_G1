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

#include "../../../../../lib/phy/upper/channel_processors/prach_detector_generic_impl.h"
#include "srsran/phy/generic_functions/generic_functions_factories.h"
#include "srsran/phy/support/support_factories.h"
#include "srsran/phy/upper/channel_processors/channel_processor_factories.h"
#include "srsran/ran/prach/prach_preamble_information.h"
#include "srsran/srsvec/copy.h"
#include "srsran/support/math/math_utils.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <gtest/gtest.h>
#include <memory>
#include <vector>

using namespace srsran;

namespace {

class prach_detector_port_attribution_test : public ::testing::Test
{
protected:
  void SetUp() override
  {
    std::shared_ptr<dft_processor_factory> dft_factory = create_dft_processor_factory_generic();
    ASSERT_NE(dft_factory, nullptr);

    generator_factory = create_prach_generator_factory_sw();
    ASSERT_NE(generator_factory, nullptr);

    std::shared_ptr<prach_detector_factory> detector_factory =
        create_prach_detector_factory_sw(dft_factory, generator_factory);
    ASSERT_NE(detector_factory, nullptr);

    detector = detector_factory->create();
    ASSERT_NE(detector, nullptr);

    generator = generator_factory->create();
    ASSERT_NE(generator, nullptr);
  }

  static prach_detector::configuration make_config(unsigned nof_rx_ports, bool enable_attribution)
  {
    prach_detector::configuration config{};
    config.root_sequence_index               = 17;
    config.format                            = prach_format_type::zero;
    config.restricted_set                    = restricted_set_config::UNRESTRICTED;
    config.zero_correlation_zone             = 0;
    config.start_preamble_index              = 0;
    config.nof_preamble_indices              = 1;
    config.ra_scs                            = prach_subcarrier_spacing::kHz1_25;
    config.nof_rx_ports                      = nof_rx_ports;
    config.port_attribution.enabled          = enable_attribution;
    config.port_attribution.unique_margin_dB = 6.0F;
    return config;
  }

  std::unique_ptr<prach_buffer> make_buffer(span<const float> port_amplitudes)
  {
    prach_generator::configuration generator_config{};
    generator_config.format                = prach_format_type::zero;
    generator_config.root_sequence_index   = 17;
    generator_config.preamble_index        = 0;
    generator_config.restricted_set        = restricted_set_config::UNRESTRICTED;
    generator_config.zero_correlation_zone = 0;

    span<const cf_t> sequence = generator->generate(generator_config);
    auto             buffer   = create_prach_buffer_long(port_amplitudes.size(), 1);
    EXPECT_NE(buffer, nullptr);

    unsigned          nof_symbols = get_prach_preamble_long_info(prach_format_type::zero).nof_symbols;
    std::vector<cf_t> scaled_sequence(sequence.size());
    for (unsigned i_port = 0; i_port != port_amplitudes.size(); ++i_port) {
      std::transform(sequence.begin(),
                     sequence.end(),
                     scaled_sequence.begin(),
                     [amplitude = port_amplitudes[i_port]](cf_t sample) { return sample * amplitude; });
      for (unsigned i_symbol = 0; i_symbol != nof_symbols; ++i_symbol) {
        srsvec::copy(buffer->get_symbol(i_port, 0, 0, i_symbol), span<const cf_t>(scaled_sequence));
      }
    }

    return buffer;
  }

  prach_detection_result detect(span<const float> port_amplitudes, bool enable_attribution)
  {
    std::unique_ptr<prach_buffer> buffer = make_buffer(port_amplitudes);
    EXPECT_NE(buffer, nullptr);
    return detector->detect(*buffer, make_config(port_amplitudes.size(), enable_attribution));
  }

  std::shared_ptr<prach_generator_factory> generator_factory;
  std::unique_ptr<prach_generator>         generator;
  std::unique_ptr<prach_detector>          detector;
};

TEST_F(prach_detector_port_attribution_test, when_single_port_is_enabled_then_only_port_is_unique)
{
  const std::array<float, 1> amplitudes = {1.0F};

  prach_detection_result result = detect(amplitudes, true);
  ASSERT_EQ(result.preambles.size(), 1U);
  const auto& port = result.preambles.front().port_attribution;

  EXPECT_EQ(port.status, prach_detection_result::rx_port_attribution_status::unique);
  EXPECT_EQ(port.strongest_port_index, 0U);
  EXPECT_FLOAT_EQ(port.strongest_to_second_margin_dB, 120.0F);
}

TEST_F(prach_detector_port_attribution_test, when_two_ports_have_clear_margin_then_strongest_port_is_unique)
{
  const std::array<float, 2> amplitudes = {1.0F, convert_dB_to_amplitude(10.0F)};

  prach_detection_result result = detect(amplitudes, true);
  ASSERT_EQ(result.preambles.size(), 1U);
  const auto& port = result.preambles.front().port_attribution;

  EXPECT_EQ(port.status, prach_detection_result::rx_port_attribution_status::unique);
  EXPECT_EQ(port.strongest_port_index, 1U);
  EXPECT_NEAR(port.strongest_to_second_margin_dB, 10.0F, 0.2F);
}

TEST_F(prach_detector_port_attribution_test, when_two_ports_have_equal_power_then_attribution_is_ambiguous)
{
  const std::array<float, 2> amplitudes = {1.0F, 1.0F};

  prach_detection_result result = detect(amplitudes, true);
  ASSERT_EQ(result.preambles.size(), 1U);
  const auto& port = result.preambles.front().port_attribution;

  EXPECT_EQ(port.status, prach_detection_result::rx_port_attribution_status::ambiguous);
  EXPECT_EQ(port.strongest_port_index, 0U);
  EXPECT_NEAR(port.strongest_to_second_margin_dB, 0.0F, 0.01F);
}

TEST_F(prach_detector_port_attribution_test, when_margin_is_exactly_six_db_then_attribution_is_unique)
{
  prach_detection_result::rx_port_attribution at_boundary =
      detail::classify_prach_rx_port_attribution(true, 2, 1, convert_dB_to_power(6.0F), 1.0F, 6.0F);
  prach_detection_result::rx_port_attribution below_boundary =
      detail::classify_prach_rx_port_attribution(true, 2, 1, convert_dB_to_power(5.99F), 1.0F, 6.0F);

  EXPECT_NEAR(at_boundary.strongest_to_second_margin_dB, 6.0F, 1e-5F);
  EXPECT_EQ(at_boundary.status, prach_detection_result::rx_port_attribution_status::unique);
  EXPECT_EQ(below_boundary.status, prach_detection_result::rx_port_attribution_status::ambiguous);
}

TEST_F(prach_detector_port_attribution_test, when_attribution_is_disabled_then_legacy_result_is_unchanged)
{
  const std::array<float, 2> amplitudes = {1.0F, convert_dB_to_amplitude(8.0F)};

  prach_detection_result enabled_result  = detect(amplitudes, true);
  prach_detection_result disabled_result = detect(amplitudes, false);

  ASSERT_EQ(enabled_result.preambles.size(), 1U);
  ASSERT_EQ(disabled_result.preambles.size(), 1U);
  const auto& enabled_preamble  = enabled_result.preambles.front();
  const auto& disabled_preamble = disabled_result.preambles.front();

  EXPECT_EQ(disabled_preamble.port_attribution.status, prach_detection_result::rx_port_attribution_status::unavailable);
  EXPECT_EQ(disabled_preamble.port_attribution.strongest_port_index,
            prach_detection_result::rx_port_attribution::invalid_port_index);
  EXPECT_FLOAT_EQ(disabled_result.rssi_dB, enabled_result.rssi_dB);
  EXPECT_EQ(disabled_preamble.preamble_index, enabled_preamble.preamble_index);
  EXPECT_EQ(disabled_preamble.time_advance, enabled_preamble.time_advance);
  EXPECT_FLOAT_EQ(disabled_preamble.detection_metric, enabled_preamble.detection_metric);
  EXPECT_FLOAT_EQ(disabled_preamble.preamble_power_dB, enabled_preamble.preamble_power_dB);
}

} // namespace
