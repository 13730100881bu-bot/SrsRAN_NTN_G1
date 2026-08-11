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

#include "lib/mac/mac_ntn_initial_ul_position_manager.h"
#include <gtest/gtest.h>
#include <chrono>
#include <memory>
#include <utility>

using namespace srsran;

namespace {

const nr_cell_identity test_nci  = nr_cell_identity::create(0x123450001ULL).value();
const nr_cell_identity other_nci = nr_cell_identity::create(0x123450002ULL).value();

constexpr du_cell_index_t test_cell      = to_du_cell_index(0);
constexpr pci_t           test_pci       = 101;
constexpr uint16_t        logical_port   = 3;
constexpr uint16_t        physical_port  = 7;
constexpr uint64_t        mapping_version = 9;

mac_ntn_access_calendar_update make_calendar_update()
{
  mac_ntn_access_calendar_update update;
  update.operation           = mac_ntn_access_calendar_operation::prepare;
  update.satellite_id        = "P01-S01";
  update.catalog_version     = 17;
  update.schedule_version    = 41;
  update.source_content_hash = "source-sha256";
  update.calendar_hash       = "calendar-sha256";
  update.cycle_duration      = std::chrono::milliseconds{80};
  update.valid_until         = std::chrono::system_clock::now() + std::chrono::hours{1};
  update.cells[0].cell_index = test_cell;
  update.cells[0].nci        = test_nci;
  update.cells[0].pci        = test_pci;
  update.cells[0].intents.push_back({"G000123",
                                     std::chrono::milliseconds{5},
                                     std::chrono::milliseconds{1},
                                     mac_ntn_access_calendar_direction::uplink,
                                     mac_ntn_access_calendar_purpose::prach_ul_beam,
                                     logical_port});
  update.cells[1].cell_index = to_du_cell_index(1);
  update.cells[1].nci        = other_nci;
  update.cells[1].pci        = 102;
  return update;
}

void activate_calendar(mac_ntn_initial_ul_position_manager& manager, const mac_ntn_access_calendar_update& update)
{
  mac_ntn_access_calendar_result prepare_result;
  prepare_result.status                    = mac_ntn_access_calendar_status::ready;
  prepare_result.effective_activation_slot = slot_point{0, 100};
  manager.handle_calendar_result(update, prepare_result);

  mac_ntn_access_calendar_update query = update;
  query.operation                       = mac_ntn_access_calendar_operation::query;
  mac_ntn_access_calendar_result query_result;
  query_result.status = mac_ntn_access_calendar_status::applied;
  manager.handle_calendar_result(query, query_result);
}

mac_ntn_rx_mapping_config make_mapping(mac_ntn_rx_backend backend)
{
  mac_ntn_rx_mapping_config config;
  config.enabled          = true;
  config.version          = mapping_version;
  config.hash             = "mapping-sha256";
  config.unique_margin_db = 6.0F;

  mac_ntn_rx_port_mapping entry;
  entry.nci              = test_nci;
  entry.cell_local_port  = logical_port;
  entry.backend          = backend;
  entry.physical_rx_port = physical_port;
  if (backend == mac_ntn_rx_backend::ofh) {
    entry.prach_eaxc = 5;
    entry.beam_id    = 0x1234;
  }
  config.entries.push_back(entry);
  return config;
}

mac_ntn_access_calendar_update make_parallel_calendar_update()
{
  mac_ntn_access_calendar_update update = make_calendar_update();
  update.cells[0].intents.push_back({"G000124",
                                     std::chrono::milliseconds{5},
                                     std::chrono::milliseconds{1},
                                     mac_ntn_access_calendar_direction::uplink,
                                     mac_ntn_access_calendar_purpose::prach_ul_beam,
                                     static_cast<uint16_t>(logical_port + 1)});
  return update;
}

mac_ntn_rx_mapping_config make_parallel_mapping(mac_ntn_rx_backend backend)
{
  mac_ntn_rx_mapping_config config = make_mapping(backend);
  mac_ntn_rx_port_mapping   second = config.entries.front();
  second.cell_local_port           = logical_port + 1;
  second.physical_rx_port          = physical_port + 1;
  if (backend == mac_ntn_rx_backend::ofh) {
    second.prach_eaxc = 6;
    second.beam_id    = 0x1235;
  }
  config.entries.push_back(second);
  return config;
}

std::shared_ptr<verified_prach_rx_context> make_ofh_context()
{
  auto context                = std::make_shared<verified_prach_rx_context>();
  context->authority          = prach_rx_context_authority::ofh_beam_id_verified;
  context->buffer_port        = 1;
  context->logical_port_id    = logical_port;
  context->ofh_prach_eaxc     = 5;
  context->ofh_beam_id        = 0x1234;
  context->position_id        = "G000123";
  context->schedule_version   = 41;
  context->calendar_hash      = "calendar-sha256";
  context->mapping_generation = mapping_version;
  context->mapping_hash       = "mapping-sha256";
  return context;
}

void record_prach(mac_ntn_initial_ul_position_manager&                 manager,
                  rnti_t                                               rnti,
                  mac_rach_indication::rx_port_attribution_status      status,
                  std::optional<unsigned>                              strongest_port,
                  std::shared_ptr<const verified_prach_rx_context>     verified_context,
                  std::chrono::steady_clock::time_point                now)
{
  // The plain slot has wrapped before the activation slot. The authoritative metadata still identifies cycle 2 and
  // the 5 ms position within the 80 ms calendar.
  manager.record_initial_prach(test_cell,
                               slot_point{0, 5},
                               rnti,
                               1,
                               true,
                               41,
                               2,
                               5000,
                               status,
                               strongest_port,
                               8.5F,
                               std::move(verified_context),
                               now);
}

} // namespace

TEST(mac_ntn_initial_ul_position_manager_test, no_ntn_calendar_keeps_terrestrial_prach_untracked)
{
  mac_ntn_initial_ul_position_manager manager;
  const auto now  = std::chrono::steady_clock::time_point{std::chrono::seconds{10}};
  const auto rnti = to_rnti(0x4600);
  manager.record_initial_prach(test_cell,
                               slot_point{0, 1},
                               rnti,
                               0,
                               false,
                               0,
                               0,
                               0,
                               mac_rach_indication::rx_port_attribution_status::unavailable,
                               std::nullopt,
                               std::nullopt,
                               nullptr,
                               now);
  EXPECT_FALSE(manager.take_for_ul_ccch(test_cell, rnti, now).has_value());
}

TEST(mac_ntn_initial_ul_position_manager_test, unique_sdr_port_is_verified_and_msg3_consumes_record_once)
{
  mac_ntn_initial_ul_position_manager manager(make_mapping(mac_ntn_rx_backend::sdr));
  activate_calendar(manager, make_calendar_update());
  const auto now  = std::chrono::steady_clock::time_point{std::chrono::seconds{10}};
  const auto rnti = to_rnti(0x4601);

  record_prach(manager,
               rnti,
               mac_rach_indication::rx_port_attribution_status::unique,
               physical_port,
               nullptr,
               now);

  const auto record = manager.take_for_ul_ccch(test_cell, rnti, now + std::chrono::milliseconds{1});
  ASSERT_TRUE(record.has_value());
  EXPECT_TRUE(record->usable());
  EXPECT_EQ(record->authority, mac_ntn_initial_ul_position_authority::sdr_rx_port_verified);
  EXPECT_EQ(record->position_id, "G000123");
  EXPECT_EQ(record->cell_local_port, logical_port);
  EXPECT_EQ(record->physical_rx_port, physical_port);
  EXPECT_EQ(record->mapping_version, mapping_version);
  EXPECT_EQ(record->mapping_hash, "mapping-sha256");
  EXPECT_EQ(record->calendar_cycle_index, 2U);
  EXPECT_EQ(record->cycle_slot_offset, 5U);
  EXPECT_EQ(record->occasion_offset_us, 5000U);
  EXPECT_FALSE(manager.take_for_ul_ccch(test_cell, rnti, now + std::chrono::milliseconds{2}).has_value());
}

TEST(mac_ntn_initial_ul_position_manager_test, missing_calendar_position_fails_closed_with_explicit_reason)
{
  mac_ntn_initial_ul_position_manager manager(make_mapping(mac_ntn_rx_backend::sdr));
  activate_calendar(manager, make_calendar_update());
  const auto now  = std::chrono::steady_clock::time_point{std::chrono::seconds{10}};
  const auto rnti = to_rnti(0x4610);

  manager.record_initial_prach(test_cell,
                               slot_point{0, 105},
                               rnti,
                               1,
                               false,
                               0,
                               0,
                               0,
                               mac_rach_indication::rx_port_attribution_status::unique,
                               physical_port,
                               8.5F,
                               nullptr,
                               now);

  const auto record = manager.take_for_ul_ccch(test_cell, rnti, now + std::chrono::milliseconds{1});
  ASSERT_TRUE(record.has_value());
  EXPECT_FALSE(record->usable());
  EXPECT_EQ(record->authority, mac_ntn_initial_ul_position_authority::none);
  EXPECT_EQ(record->reason, "calendar_position_unavailable");
}

TEST(mac_ntn_initial_ul_position_manager_test,
     verified_ofh_context_keeps_buffer_index_separate_from_physical_port)
{
  mac_ntn_initial_ul_position_manager manager(make_mapping(mac_ntn_rx_backend::ofh));
  activate_calendar(manager, make_calendar_update());
  const auto now  = std::chrono::steady_clock::time_point{std::chrono::seconds{10}};
  const auto rnti = to_rnti(0x4602);
  auto       context = make_ofh_context();

  ASSERT_EQ(context->buffer_port, 1U);
  ASSERT_NE(context->buffer_port, physical_port);
  record_prach(manager,
               rnti,
               mac_rach_indication::rx_port_attribution_status::unique,
               physical_port,
               context,
               now);

  const auto record = manager.take_for_ul_ccch(test_cell, rnti, now + std::chrono::milliseconds{1});
  ASSERT_TRUE(record.has_value());
  EXPECT_TRUE(record->usable());
  EXPECT_EQ(record->authority, mac_ntn_initial_ul_position_authority::ofh_beam_id_verified);
  EXPECT_EQ(record->physical_rx_port, physical_port);
  ASSERT_TRUE(record->prach_eaxc.has_value());
  EXPECT_EQ(record->prach_eaxc.value(), 5);
  ASSERT_TRUE(record->beam_id.has_value());
  EXPECT_EQ(record->beam_id.value(), 0x1234);
  EXPECT_EQ(record->mapping_version, mapping_version);
  EXPECT_EQ(record->mapping_hash, "mapping-sha256");
}

TEST(mac_ntn_initial_ul_position_manager_test,
     stale_ofh_mapping_identity_or_calendar_hash_is_rejected_with_explicit_mapping_reason)
{
  mac_ntn_initial_ul_position_manager manager(make_mapping(mac_ntn_rx_backend::ofh));
  activate_calendar(manager, make_calendar_update());
  const auto now = std::chrono::steady_clock::time_point{std::chrono::seconds{10}};

  auto stale_generation = make_ofh_context();
  stale_generation->mapping_generation++;
  record_prach(manager,
               to_rnti(0x4603),
               mac_rach_indication::rx_port_attribution_status::unique,
               physical_port,
               stale_generation,
               now);

  auto wrong_calendar_hash = make_ofh_context();
  wrong_calendar_hash->calendar_hash = "other-calendar";
  record_prach(manager,
               to_rnti(0x4604),
               mac_rach_indication::rx_port_attribution_status::unique,
               physical_port,
               wrong_calendar_hash,
               now);

  auto wrong_mapping_hash = make_ofh_context();
  wrong_mapping_hash->mapping_hash = "other-mapping";
  record_prach(manager,
               to_rnti(0x4605),
               mac_rach_indication::rx_port_attribution_status::unique,
               physical_port,
               wrong_mapping_hash,
               now);

  for (rnti_t rnti : {to_rnti(0x4603), to_rnti(0x4604), to_rnti(0x4605)}) {
    const auto record = manager.take_for_ul_ccch(test_cell, rnti, now + std::chrono::milliseconds{1});
    ASSERT_TRUE(record.has_value());
    EXPECT_FALSE(record->usable());
    EXPECT_EQ(record->authority, mac_ntn_initial_ul_position_authority::none);
    EXPECT_EQ(record->reason, "rx_mapping_mismatch");
    EXPECT_EQ(record->mapping_version, 0U);
    EXPECT_TRUE(record->mapping_hash.empty());
  }
}

TEST(mac_ntn_initial_ul_position_manager_test, invalid_local_mapping_hash_rejects_measured_port_explicitly)
{
  mac_ntn_rx_mapping_config config = make_mapping(mac_ntn_rx_backend::sdr);
  config.hash.clear();
  mac_ntn_initial_ul_position_manager manager(std::move(config));
  EXPECT_FALSE(manager.is_rx_mapping_valid());
  activate_calendar(manager, make_calendar_update());
  const auto now  = std::chrono::steady_clock::time_point{std::chrono::seconds{10}};
  const auto rnti = to_rnti(0x4605);

  record_prach(manager,
               rnti,
               mac_rach_indication::rx_port_attribution_status::unique,
               physical_port,
               nullptr,
               now);

  const auto record = manager.take_for_ul_ccch(test_cell, rnti, now + std::chrono::milliseconds{1});
  ASSERT_TRUE(record.has_value());
  EXPECT_FALSE(record->usable());
  EXPECT_EQ(record->authority, mac_ntn_initial_ul_position_authority::none);
  EXPECT_EQ(record->reason, "rx_mapping_mismatch");
  EXPECT_EQ(record->mapping_version, 0U);
  EXPECT_TRUE(record->mapping_hash.empty());
}

TEST(mac_ntn_initial_ul_position_manager_test, missing_ofh_beam_context_reports_capability_unavailable)
{
  mac_ntn_initial_ul_position_manager manager(make_mapping(mac_ntn_rx_backend::ofh));
  activate_calendar(manager, make_calendar_update());
  const auto now  = std::chrono::steady_clock::time_point{std::chrono::seconds{10}};
  const auto rnti = to_rnti(0x4613);

  record_prach(manager,
               rnti,
               mac_rach_indication::rx_port_attribution_status::unique,
               physical_port,
               nullptr,
               now);

  const auto record = manager.take_for_ul_ccch(test_cell, rnti, now + std::chrono::milliseconds{1});
  ASSERT_TRUE(record.has_value());
  EXPECT_FALSE(record->usable());
  EXPECT_EQ(record->authority, mac_ntn_initial_ul_position_authority::none);
  EXPECT_EQ(record->reason, "ofh_beam_capability_unavailable");
}

TEST(mac_ntn_initial_ul_position_manager_test, receive_mapping_enforces_physical_port_and_per_cell_bounds)
{
  mac_ntn_rx_mapping_config config = make_mapping(mac_ntn_rx_backend::sdr);
  config.entries.front().physical_rx_port = 254;
  mac_ntn_initial_ul_position_manager manager(config);
  EXPECT_TRUE(manager.is_rx_mapping_valid());

  config.entries.front().physical_rx_port = 255;
  manager.update_rx_mapping(config);
  EXPECT_FALSE(manager.is_rx_mapping_valid());

  config = make_mapping(mac_ntn_rx_backend::sdr);
  for (unsigned i = 1; i != 17; ++i) {
    mac_ntn_rx_port_mapping entry = config.entries.front();
    entry.cell_local_port          = static_cast<uint16_t>(logical_port + i);
    entry.physical_rx_port         = static_cast<uint16_t>(physical_port + i);
    config.entries.push_back(entry);
  }
  manager.update_rx_mapping(config);
  EXPECT_FALSE(manager.is_rx_mapping_valid());
}

TEST(mac_ntn_initial_ul_position_manager_test, software_only_is_recorded_but_ambiguous_port_fails_closed)
{
  mac_ntn_initial_ul_position_manager manager;
  activate_calendar(manager, make_calendar_update());
  const auto now = std::chrono::steady_clock::time_point{std::chrono::seconds{10}};

  record_prach(manager,
               to_rnti(0x4606),
               mac_rach_indication::rx_port_attribution_status::unavailable,
               std::nullopt,
               nullptr,
               now);
  record_prach(manager,
               to_rnti(0x4607),
               mac_rach_indication::rx_port_attribution_status::ambiguous,
               physical_port,
               nullptr,
               now);

  const auto software =
      manager.take_for_ul_ccch(test_cell, to_rnti(0x4606), now + std::chrono::milliseconds{1});
  ASSERT_TRUE(software.has_value());
  EXPECT_TRUE(software->usable());
  EXPECT_EQ(software->authority, mac_ntn_initial_ul_position_authority::software_attributed);

  const auto ambiguous =
      manager.take_for_ul_ccch(test_cell, to_rnti(0x4607), now + std::chrono::milliseconds{1});
  ASSERT_TRUE(ambiguous.has_value());
  EXPECT_FALSE(ambiguous->usable());
  EXPECT_EQ(ambiguous->authority, mac_ntn_initial_ul_position_authority::none);
  EXPECT_EQ(ambiguous->reason, "ambiguous_receive_position");
}

TEST(mac_ntn_initial_ul_position_manager_test, physical_receive_port_disambiguates_parallel_calendar_positions)
{
  mac_ntn_initial_ul_position_manager manager(make_parallel_mapping(mac_ntn_rx_backend::sdr));
  activate_calendar(manager, make_parallel_calendar_update());
  const auto now  = std::chrono::steady_clock::time_point{std::chrono::seconds{10}};
  const auto rnti = to_rnti(0x4608);

  manager.record_initial_prach(test_cell,
                               slot_point{0, 265},
                               rnti,
                               3,
                               true,
                               41,
                               2,
                               5000,
                               mac_rach_indication::rx_port_attribution_status::unique,
                               physical_port + 1,
                               9.0F,
                               nullptr,
                               now);

  const auto record = manager.take_for_ul_ccch(test_cell, rnti, now + std::chrono::milliseconds{1});
  ASSERT_TRUE(record.has_value());
  EXPECT_TRUE(record->usable());
  EXPECT_EQ(record->authority, mac_ntn_initial_ul_position_authority::sdr_rx_port_verified);
  EXPECT_EQ(record->position_id, "G000124");
  EXPECT_EQ(record->cell_local_port, logical_port + 1);
  EXPECT_EQ(record->physical_rx_port, physical_port + 1);
}

TEST(mac_ntn_initial_ul_position_manager_test, ofh_provider_exposes_each_active_eaxc_with_its_own_beam_context)
{
  mac_ntn_initial_ul_position_manager manager(make_parallel_mapping(mac_ntn_rx_backend::ofh));
  activate_calendar(manager, make_parallel_calendar_update());

  prach_buffer_context context{};
  context.sector                  = 0;
  context.slot                    = slot_point{0, 5};
  context.calendar_position_valid = true;
  context.calendar_schedule_version = 41;
  context.calendar_cycle_index    = 2;
  context.occasion_offset_us      = 5000;
  const auto mappings = manager.get_prach_beam_context(context);

  ASSERT_TRUE(mappings.has_value());
  ASSERT_EQ(mappings->size(), 2U);
  EXPECT_EQ((*mappings)[0].eaxc, 5U);
  EXPECT_EQ((*mappings)[0].context.beam_id, 0x1234);
  EXPECT_EQ((*mappings)[0].context.position_id, "G000123");
  EXPECT_EQ((*mappings)[1].eaxc, 6U);
  EXPECT_EQ((*mappings)[1].context.beam_id, 0x1235);
  EXPECT_EQ((*mappings)[1].context.position_id, "G000124");
  EXPECT_EQ((*mappings)[1].context.schedule_version, 41U);
  EXPECT_EQ((*mappings)[1].context.calendar_hash, "calendar-sha256");
  EXPECT_EQ((*mappings)[1].context.mapping_generation, mapping_version);
  EXPECT_EQ((*mappings)[1].context.mapping_hash, "mapping-sha256");
}

TEST(mac_ntn_initial_ul_position_manager_test, armed_staged_calendar_is_selected_by_scheduler_version)
{
  mac_ntn_initial_ul_position_manager manager(make_mapping(mac_ntn_rx_backend::ofh));
  const mac_ntn_access_calendar_update update = make_calendar_update();
  mac_ntn_access_calendar_result       preparing;
  preparing.status                    = mac_ntn_access_calendar_status::preparing;
  preparing.effective_activation_slot = slot_point{0, 100};
  manager.handle_calendar_result(update, preparing);

  prach_buffer_context context{};
  context.sector                    = 0;
  context.slot                      = slot_point{0, 105};
  context.calendar_position_valid   = true;
  context.calendar_schedule_version = update.schedule_version;
  context.calendar_cycle_index      = 0;
  context.occasion_offset_us        = 5000;
  EXPECT_FALSE(manager.get_prach_beam_context(context).has_value());

  mac_ntn_access_calendar_result ready = preparing;
  ready.status                         = mac_ntn_access_calendar_status::ready;
  manager.handle_calendar_result(update, ready);
  ASSERT_TRUE(manager.get_prach_beam_context(context).has_value());

  context.calendar_schedule_version++;
  EXPECT_FALSE(manager.get_prach_beam_context(context).has_value());
}

TEST(mac_ntn_initial_ul_position_manager_test, active_and_staged_calendars_are_resolved_without_guessing)
{
  mac_ntn_initial_ul_position_manager manager(make_mapping(mac_ntn_rx_backend::sdr));
  const mac_ntn_access_calendar_update active_update = make_calendar_update();
  activate_calendar(manager, active_update);

  mac_ntn_access_calendar_update staged_update = active_update;
  staged_update.schedule_version               = 42;
  staged_update.calendar_hash                  = "next-calendar-sha256";
  staged_update.cells[0].intents.front().position_id = "G000124";
  mac_ntn_access_calendar_result ready;
  ready.status                    = mac_ntn_access_calendar_status::ready;
  ready.effective_activation_slot = slot_point{0, 180};
  manager.handle_calendar_result(staged_update, ready);

  const auto now = std::chrono::steady_clock::time_point{std::chrono::seconds{10}};
  manager.record_initial_prach(test_cell,
                               slot_point{0, 105},
                               to_rnti(0x4611),
                               1,
                               true,
                               41,
                               0,
                               5000,
                               mac_rach_indication::rx_port_attribution_status::unavailable,
                               std::nullopt,
                               std::nullopt,
                               nullptr,
                               now);
  manager.record_initial_prach(test_cell,
                               slot_point{0, 185},
                               to_rnti(0x4612),
                               1,
                               true,
                               42,
                               0,
                               5000,
                               mac_rach_indication::rx_port_attribution_status::unavailable,
                               std::nullopt,
                               std::nullopt,
                               nullptr,
                               now);

  const auto old_record = manager.take_for_ul_ccch(test_cell, to_rnti(0x4611), now);
  const auto new_record = manager.take_for_ul_ccch(test_cell, to_rnti(0x4612), now);
  ASSERT_TRUE(old_record.has_value());
  ASSERT_TRUE(new_record.has_value());
  EXPECT_EQ(old_record->schedule_version, 41U);
  EXPECT_EQ(old_record->position_id, "G000123");
  EXPECT_EQ(new_record->schedule_version, 42U);
  EXPECT_EQ(new_record->position_id, "G000124");
}

TEST(mac_ntn_initial_ul_position_manager_test, applied_query_keeps_new_calendar_observation_waiting_for_msg3)
{
  mac_ntn_initial_ul_position_manager manager;
  const mac_ntn_access_calendar_update active_update = make_calendar_update();
  activate_calendar(manager, active_update);

  mac_ntn_access_calendar_update staged_update = active_update;
  staged_update.schedule_version               = 42;
  staged_update.calendar_hash                  = "next-calendar-sha256";
  staged_update.cells[0].intents.front().position_id = "G000124";
  mac_ntn_access_calendar_result ready;
  ready.status                    = mac_ntn_access_calendar_status::ready;
  ready.effective_activation_slot = slot_point{0, 180};
  manager.handle_calendar_result(staged_update, ready);

  const auto now = std::chrono::steady_clock::time_point{std::chrono::seconds{10}};
  manager.record_initial_prach(test_cell,
                               slot_point{0, 185},
                               to_rnti(0x4614),
                               1,
                               true,
                               42,
                               0,
                               5000,
                               mac_rach_indication::rx_port_attribution_status::unavailable,
                               std::nullopt,
                               std::nullopt,
                               nullptr,
                               now);

  mac_ntn_access_calendar_update applied_query = staged_update;
  applied_query.operation                       = mac_ntn_access_calendar_operation::query;
  mac_ntn_access_calendar_result applied;
  applied.status = mac_ntn_access_calendar_status::applied;
  manager.handle_calendar_result(applied_query, applied);

  const auto record = manager.take_for_ul_ccch(test_cell, to_rnti(0x4614), now + std::chrono::milliseconds{1});
  ASSERT_TRUE(record.has_value());
  EXPECT_EQ(record->schedule_version, 42U);
  EXPECT_EQ(record->position_id, "G000124");
}

TEST(mac_ntn_initial_ul_position_manager_test, clear_restores_previous_calendar_and_keeps_its_new_observation)
{
  mac_ntn_initial_ul_position_manager manager;
  const mac_ntn_access_calendar_update previous_update = make_calendar_update();
  activate_calendar(manager, previous_update);

  mac_ntn_access_calendar_update replacement = previous_update;
  replacement.schedule_version               = 42;
  replacement.calendar_hash                  = "next-calendar-sha256";
  replacement.cells[0].intents.front().position_id = "G000124";
  activate_calendar(manager, replacement);

  const auto now = std::chrono::steady_clock::time_point{std::chrono::seconds{10}};
  manager.record_initial_prach(test_cell,
                               slot_point{0, 105},
                               to_rnti(0x4615),
                               1,
                               true,
                               previous_update.schedule_version,
                               0,
                               5000,
                               mac_rach_indication::rx_port_attribution_status::unavailable,
                               std::nullopt,
                               std::nullopt,
                               nullptr,
                               now);
  manager.record_initial_prach(test_cell,
                               slot_point{0, 185},
                               to_rnti(0x4616),
                               1,
                               true,
                               replacement.schedule_version,
                               0,
                               5000,
                               mac_rach_indication::rx_port_attribution_status::unavailable,
                               std::nullopt,
                               std::nullopt,
                               nullptr,
                               now);

  mac_ntn_access_calendar_update clear = replacement;
  clear.operation                       = mac_ntn_access_calendar_operation::clear;
  mac_ntn_access_calendar_result cleared;
  cleared.status = mac_ntn_access_calendar_status::cleared;
  manager.handle_calendar_result(clear, cleared);

  const auto restored = manager.take_for_ul_ccch(test_cell, to_rnti(0x4615), now + std::chrono::milliseconds{1});
  ASSERT_TRUE(restored.has_value());
  EXPECT_EQ(restored->schedule_version, previous_update.schedule_version);
  EXPECT_EQ(restored->position_id, "G000123");
  EXPECT_FALSE(manager.take_for_ul_ccch(test_cell,
                                        to_rnti(0x4616),
                                        now + std::chrono::milliseconds{1})
                   .has_value());
}
