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

#include "../../../lib/phy/upper/upper_phy_rx_results_notifier_wrapper.h"
#include "../../../lib/phy/upper/upper_phy_rx_symbol_handler_impl.h"
#include "../support/prach_buffer_test_doubles.h"
#include "../support/resource_grid_test_doubles.h"
#include "uplink_processor_test_doubles.h"
#include "srsran/phy/support/prach_buffer_context.h"
#include "srsran/phy/upper/upper_phy_factories.h"
#include "srsran/ru/ru_adapters.h"
#include <gtest/gtest.h>

using namespace srsran;

namespace {

class UpperPhyRxSymbolHandlerFixture : public ::testing::Test
{
protected:
  static constexpr slot_point test_slot = {0, 0, 0};

  std::unique_ptr<rx_buffer_pool_controller> rm_buffer_pool;
  uplink_processor_spy*                      ul_proc_spy;
  uplink_processor_spy*                      default_ul_proc_spy;
  std::unique_ptr<uplink_processor_pool>     ul_processor_pool;
  upper_phy_rx_symbol_handler_impl           rx_handler;
  std::unique_ptr<prach_buffer_pool>         prach_pool;
  resource_grid_dummy                        rg;
  shared_resource_grid_spy                   shared_rg;

  void handle_prach_symbol()
  {
    prach_buffer_context prach_context;
    prach_context.sector = 0;
    prach_context.slot   = test_slot;

    rx_handler.handle_rx_prach_window(prach_context, prach_pool->get());
  }

  void handle_grid_symbol()
  {
    static constexpr unsigned start_symbol_index = 1;
    static constexpr unsigned nof_symbols        = 2;

    // Uplink processor gets called on the last symbol allocated in this PDU.
    for (unsigned i = start_symbol_index, e = start_symbol_index + nof_symbols; i != e; ++i) {
      upper_phy_rx_symbol_context ctx = {};
      ctx.symbol                      = i;
      ctx.slot                        = test_slot;
      rx_handler.handle_rx_symbol(ctx, shared_rg.get_grid(), true);
    }
  }

  UpperPhyRxSymbolHandlerFixture() :
    rm_buffer_pool(create_rx_buffer_pool(rx_buffer_pool_config{16, 2, 2, 16})),
    ul_processor_pool(create_ul_processor_pool()),
    rx_handler(ul_processor_pool->get_slot_processor_pool()),
    prach_pool(create_spy_prach_buffer_pool()),
    shared_rg(rg)
  {
    srslog::fetch_basic_logger("TEST").set_level(srslog::basic_levels::warning);
    srslog::init();
  }

  std::unique_ptr<uplink_processor_pool> create_ul_processor_pool()
  {
    auto default_ul_proc = std::make_unique<uplink_processor_spy>();
    auto ul_proc         = std::make_unique<uplink_processor_spy>();
    ul_proc_spy          = ul_proc.get();
    default_ul_proc_spy  = default_ul_proc.get();

    uplink_processor_pool_config config_pool;
    config_pool.ul_processors.emplace_back();
    uplink_processor_pool_config::uplink_processor_set& info = config_pool.ul_processors.back();
    info.scs                                                 = subcarrier_spacing::kHz15;
    config_pool.default_processor                            = std::move(default_ul_proc);
    info.procs.push_back(std::move(ul_proc));

    return create_uplink_processor_pool(std::move(config_pool));
  }
};

} // namespace

TEST_F(UpperPhyRxSymbolHandlerFixture, handling_valid_prach_calls_default_uplink_processor)
{
  ASSERT_FALSE(ul_proc_spy->is_process_prach_method_called());
  ASSERT_FALSE(default_ul_proc_spy->is_process_prach_method_called());

  handle_prach_symbol();

  ASSERT_FALSE(ul_proc_spy->is_process_prach_method_called());
  ASSERT_TRUE(default_ul_proc_spy->is_process_prach_method_called());
}

TEST_F(UpperPhyRxSymbolHandlerFixture, handling_valid_pusch_pdu_calls_default_uplink_processor)
{
  ASSERT_EQ(ul_proc_spy->get_on_rx_symbol_count(), 0);
  ASSERT_EQ(ul_proc_spy->get_last_end_symbol_index(), std::numeric_limits<unsigned>::max());
  ASSERT_EQ(default_ul_proc_spy->get_on_rx_symbol_count(), 0);
  ASSERT_EQ(default_ul_proc_spy->get_last_end_symbol_index(), std::numeric_limits<unsigned>::max());

  handle_grid_symbol();

  ASSERT_EQ(ul_proc_spy->get_on_rx_symbol_count(), 0);
  ASSERT_EQ(ul_proc_spy->get_last_end_symbol_index(), std::numeric_limits<unsigned>::max());
  ASSERT_EQ(default_ul_proc_spy->get_on_rx_symbol_count(), 2);
  ASSERT_EQ(default_ul_proc_spy->get_last_end_symbol_index(), 2);
}

TEST_F(UpperPhyRxSymbolHandlerFixture, handling_valid_prach_calls_uplink_processor)
{
  ASSERT_FALSE(ul_proc_spy->is_process_prach_method_called());
  ASSERT_FALSE(default_ul_proc_spy->is_process_prach_method_called());

  ul_processor_pool->get_slot_pdu_repository_pool().get_pdu_slot_repository(test_slot);
  handle_prach_symbol();

  ASSERT_TRUE(ul_proc_spy->is_process_prach_method_called());
  ASSERT_FALSE(default_ul_proc_spy->is_process_prach_method_called());
}

TEST_F(UpperPhyRxSymbolHandlerFixture, handling_valid_pusch_pdu_calls_uplink_processor)
{
  ASSERT_EQ(ul_proc_spy->get_on_rx_symbol_count(), 0);
  ASSERT_EQ(ul_proc_spy->get_last_end_symbol_index(), std::numeric_limits<unsigned>::max());
  ASSERT_EQ(default_ul_proc_spy->get_on_rx_symbol_count(), 0);
  ASSERT_EQ(default_ul_proc_spy->get_last_end_symbol_index(), std::numeric_limits<unsigned>::max());

  ul_processor_pool->get_slot_pdu_repository_pool().get_pdu_slot_repository(test_slot);
  handle_grid_symbol();

  ASSERT_EQ(ul_proc_spy->get_on_rx_symbol_count(), 2);
  ASSERT_EQ(ul_proc_spy->get_last_end_symbol_index(), 2);
  ASSERT_EQ(default_ul_proc_spy->get_on_rx_symbol_count(), 0);
  ASSERT_EQ(default_ul_proc_spy->get_last_end_symbol_index(), std::numeric_limits<unsigned>::max());
}

TEST_F(UpperPhyRxSymbolHandlerFixture, ru_adapter_attaches_complete_verified_prach_receive_context)
{
  upper_phy_ru_ul_adapter adapter(1);
  adapter.map_handler(0, rx_handler);

  prach_buffer_context context;
  context.sector = 0;
  context.slot   = test_slot;
  context.ports  = {3};

  verified_prach_rx_context verified;
  verified.authority          = prach_rx_context_authority::ofh_beam_id_verified;
  verified.buffer_port        = 0;
  verified.logical_port_id    = 7;
  verified.ofh_prach_eaxc     = 5;
  verified.ofh_beam_id        = 0x1234;
  verified.position_id        = "G000123";
  verified.schedule_version   = 41;
  verified.calendar_hash      = "calendar-sha256";
  verified.mapping_generation = 9;
  verified.mapping_hash       = "mapping-sha256";

  verified_prach_rx_context_list contexts;
  contexts.push_back(verified);
  adapter.on_new_prach_window_data(context, prach_pool->get(), contexts);

  ASSERT_TRUE(default_ul_proc_spy->is_process_prach_method_called());
  const auto& forwarded = default_ul_proc_spy->get_last_prach_context();
  ASSERT_NE(forwarded.verified_rx_contexts, nullptr);
  ASSERT_EQ(forwarded.verified_rx_contexts->size(), 1);
  EXPECT_EQ(forwarded.verified_rx_contexts->front().buffer_port, 0);
  ASSERT_TRUE(forwarded.verified_rx_contexts->front().ofh_prach_eaxc.has_value());
  EXPECT_EQ(forwarded.verified_rx_contexts->front().ofh_prach_eaxc.value(), 5);
  EXPECT_EQ(forwarded.verified_rx_contexts->front().position_id, "G000123");
  EXPECT_EQ(forwarded.verified_rx_contexts->front().mapping_hash, "mapping-sha256");
}

TEST_F(UpperPhyRxSymbolHandlerFixture, ru_adapter_does_not_attach_duplicate_verified_buffer_port)
{
  upper_phy_ru_ul_adapter adapter(1);
  adapter.map_handler(0, rx_handler);

  prach_buffer_context context;
  context.sector = 0;
  context.slot   = test_slot;
  context.ports  = {3, 4};

  verified_prach_rx_context verified;
  verified.authority          = prach_rx_context_authority::ofh_beam_id_verified;
  verified.buffer_port        = 0;
  verified.logical_port_id    = 7;
  verified.ofh_prach_eaxc     = 5;
  verified.ofh_beam_id        = 0x1234;
  verified.position_id        = "G000123";
  verified.schedule_version   = 41;
  verified.calendar_hash      = "calendar-sha256";
  verified.mapping_generation = 9;
  verified.mapping_hash       = "mapping-sha256";

  verified_prach_rx_context_list contexts;
  contexts.push_back(verified);
  verified.ofh_prach_eaxc = 6;
  contexts.push_back(verified);
  adapter.on_new_prach_window_data(context, prach_pool->get(), contexts);

  ASSERT_TRUE(default_ul_proc_spy->is_process_prach_method_called());
  EXPECT_EQ(default_ul_proc_spy->get_last_prach_context().verified_rx_contexts, nullptr);
}
