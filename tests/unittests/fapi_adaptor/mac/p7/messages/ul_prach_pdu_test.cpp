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

#include "helpers.h"
#include "prach.h"
#include <gtest/gtest.h>

using namespace srsran;
using namespace fapi_adaptor;
using namespace unittests;

TEST(mac_fapi_ul_prach_pdu_conversor_test, valid_prach_pdu_should_pass)
{
  prach_occasion_info mac_pdu                  = build_valid_prach_occassion();
  mac_pdu.handle                               = 0x12345678U;
  mac_pdu.enable_rx_port_attribution           = true;
  mac_pdu.rx_port_attribution_unique_margin_dB = 8.5F;
  mac_pdu.calendar_position_valid              = true;
  mac_pdu.calendar_schedule_version            = 41U;
  mac_pdu.calendar_cycle_index                 = 160U;
  mac_pdu.occasion_offset_us                   = 5000U;
  fapi::ul_prach_pdu fapi_pdu;

  convert_prach_mac_to_fapi(fapi_pdu, mac_pdu);

  ASSERT_EQ(static_cast<unsigned>(prach_format_type::one), static_cast<unsigned>(fapi_pdu.prach_format));
  ASSERT_EQ(is_long_preamble(mac_pdu.format) ? 1 : mac_pdu.nof_prach_occasions, fapi_pdu.num_prach_ocas);
  ASSERT_EQ(mac_pdu.index_fd_ra, fapi_pdu.index_fd_ra);
  ASSERT_EQ(mac_pdu.pci, fapi_pdu.phys_cell_id);
  ASSERT_EQ(mac_pdu.nof_fd_ra, fapi_pdu.maintenance_v3.num_fd_ra);
  ASSERT_EQ(mac_pdu.start_symbol, fapi_pdu.prach_start_symbol);
  ASSERT_EQ(mac_pdu.nof_cs, fapi_pdu.num_cs);
  ASSERT_EQ(0, fapi_pdu.maintenance_v3.prach_res_config_index);
  ASSERT_EQ(mac_pdu.start_preamble_index, fapi_pdu.maintenance_v3.start_preamble_index);
  ASSERT_EQ(mac_pdu.nof_preamble_indexes, fapi_pdu.maintenance_v3.num_preamble_indices);
  ASSERT_EQ(mac_pdu.handle, fapi_pdu.maintenance_v3.handle);
  ASSERT_EQ(mac_pdu.enable_rx_port_attribution, fapi_pdu.enable_rx_port_attribution);
  ASSERT_FLOAT_EQ(mac_pdu.rx_port_attribution_unique_margin_dB, fapi_pdu.rx_port_attribution_unique_margin_dB);
  ASSERT_EQ(mac_pdu.calendar_position_valid, fapi_pdu.calendar_position_valid);
  ASSERT_EQ(mac_pdu.calendar_schedule_version, fapi_pdu.calendar_schedule_version);
  ASSERT_EQ(mac_pdu.calendar_cycle_index, fapi_pdu.calendar_cycle_index);
  ASSERT_EQ(mac_pdu.occasion_offset_us, fapi_pdu.occasion_offset_us);
  ASSERT_EQ(static_cast<unsigned>(fapi::prach_config_scope_type::phy_context),
            static_cast<unsigned>(fapi_pdu.maintenance_v3.prach_config_scope));
}

TEST(mac_fapi_ul_prach_pdu_conversor_test, default_port_attribution_request_remains_disabled)
{
  const prach_occasion_info mac_pdu = build_valid_prach_occassion();
  fapi::ul_prach_pdu        fapi_pdu;

  convert_prach_mac_to_fapi(fapi_pdu, mac_pdu);

  EXPECT_EQ(0U, fapi_pdu.maintenance_v3.handle);
  EXPECT_FALSE(fapi_pdu.enable_rx_port_attribution);
  EXPECT_FLOAT_EQ(6.0F, fapi_pdu.rx_port_attribution_unique_margin_dB);
  EXPECT_FALSE(fapi_pdu.calendar_position_valid);
  EXPECT_EQ(fapi_pdu.calendar_schedule_version, 0U);
  EXPECT_EQ(fapi_pdu.calendar_cycle_index, 0U);
  EXPECT_EQ(fapi_pdu.occasion_offset_us, 0U);
}
