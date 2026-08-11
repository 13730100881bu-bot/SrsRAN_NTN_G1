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

#include "lib/cu_cp/ntn_mobility/ntn_initial_ul_position_authorizer.h"
#include "fmt/format.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

using namespace srsran;
using namespace srsran::srs_cu_cp;

namespace {

constexpr const char* catalog_hash        = "sha256:b39fe9c3ee9a9355b3546036b7f16e0fb858c953f8558cc4295122f2169fbe7a";
constexpr const char* registry_hash       = "sha256:7475821350e104b57a70d979d630f4b29a6cecb89ca0eca7b16dddf2ffee6a4a";
constexpr const char* access_profile_hash = "sha256:195786f4161e3b0fad6faa0605144948a7401c067a014bde684c1b29a8087d63";

std::chrono::system_clock::time_point system_at_ms(int64_t milliseconds)
{
  return std::chrono::system_clock::time_point{std::chrono::milliseconds{milliseconds}};
}

std::chrono::steady_clock::time_point steady_at_ms(int64_t milliseconds)
{
  return std::chrono::steady_clock::time_point{std::chrono::milliseconds{milliseconds}};
}

ntn_onboard_cell_identity make_cell(uint64_t nci, pci_t pci)
{
  return {nr_cell_identity::create(nci).value(), pci};
}

ntn_onboard_position_plan_config make_config()
{
  ntn_onboard_position_plan_config config;
  config.enabled                            = true;
  config.require_external_apply             = true;
  config.satellite_id                       = "P01-S01";
  config.expected_catalog_id                = "global-land-l1-v1";
  config.expected_catalog_hash              = catalog_hash;
  config.expected_identity_registry_version = "mc-ntn-onboard-cell-registry-v1";
  config.expected_identity_registry_hash    = registry_hash;
  config.expected_access_profile_id         = "ntn-access-16a-64d-v1";
  config.expected_access_profile_hash       = access_profile_hash;
  config.onboard_cells[0]                   = make_cell(0x123450001ULL, 101);
  config.onboard_cells[1]                   = make_cell(0x123450002ULL, 202);
  return config;
}

ntn_versioned_position_plan make_plan()
{
  ntn_versioned_position_plan plan;
  plan.schema_version            = 2;
  plan.planning_run_id           = "planning-run-2026-08-08";
  plan.catalog_id                = "global-land-l1-v1";
  plan.catalog_hash              = catalog_hash;
  plan.identity_registry_version = "mc-ntn-onboard-cell-registry-v1";
  plan.identity_registry_hash    = registry_hash;
  plan.access_profile_id         = "ntn-access-16a-64d-v1";
  plan.access_profile_hash       = access_profile_hash;
  plan.satellite_id              = "P01-S01";
  plan.catalog_version           = 17;
  plan.schedule_version          = 29;
  plan.valid_from                = system_at_ms(640);
  plan.activation_epoch          = system_at_ms(1280);
  plan.valid_until               = system_at_ms(12800);
  plan.onboard_cells             = make_config().onboard_cells;
  for (unsigned i = 0; i != 8; ++i) {
    plan.visible_l1_positions.push_back(
        {fmt::format("G{:06}", i + 1), 10.0 + i * 0.01, 20.0 + i * 0.01, 0x7f});
  }
  plan.content_hash = compute_ntn_position_plan_content_hash(plan);
  return plan;
}

std::array<ntn_onboard_runtime_cell_route, 2> make_routes()
{
  const auto cells = make_config().onboard_cells;
  return {{{cells[0],
            {plmn_identity::test_value(), cells[0].nci},
            101,
            uint_to_du_index(4),
            uint_to_du_cell_index(0),
            9,
            ntn_onboard_tai_status::ready},
           {cells[1],
            {plmn_identity::test_value(), cells[1].nci},
            202,
            uint_to_du_index(4),
            uint_to_du_cell_index(1),
            9,
            ntn_onboard_tai_status::ready}}};
}

ntn_initial_ul_position_observation make_store_observation(uint64_t event_id = 1)
{
  ntn_initial_ul_position_observation observation;
  observation.observation_id           = event_id;
  observation.du_index                 = uint_to_du_index(4);
  observation.du_cell_index            = uint_to_du_cell_index(0);
  observation.c_rnti                   = to_rnti(0x4601);
  observation.du_connection_generation = 9;
  observation.satellite_id             = "P01-S01";
  observation.catalog_version          = 17;
  observation.schedule_version         = 29;
  observation.source_content_hash      = "sha256:" + std::string(64, 'a');
  observation.calendar_hash            = "sha256:" + std::string(64, 'b');
  observation.nci                      = make_cell(0x123450001ULL, 101).nci;
  observation.pci                      = 101;
  observation.position_id              = "G000001";
  observation.occasion_time            = system_at_ms(1281);
  observation.ul_beam_port_id           = 1;
  return observation;
}

class ntn_initial_ul_position_authorizer_test : public ::testing::Test
{
protected:
  ntn_initial_ul_position_authorizer_test() : controller(make_config()), authorizer(store, controller)
  {
    const ntn_versioned_position_plan plan = make_plan();
    EXPECT_TRUE(controller.submit(plan, system_at_ms(1000)).accepted);
    EXPECT_TRUE(controller.pending_plan().has_value());
    if (!controller.pending_plan().has_value()) {
      return;
    }
    const std::string calendar_hash = controller.pending_plan()->calendar_hash;
    EXPECT_TRUE(controller.mark_deployment_preparing(plan.schedule_version, calendar_hash));
    EXPECT_TRUE(controller.mark_deployment_applied(plan.schedule_version, calendar_hash));
    EXPECT_TRUE(controller.advance_time(plan.activation_epoch));
    EXPECT_TRUE(controller.active_plan().has_value());
    if (!controller.active_plan().has_value()) {
      return;
    }
    auto mapping_result = ntn_onboard_runtime_mapping_snapshot::create(*controller.active_plan(), make_routes());
    EXPECT_TRUE(mapping_result.has_value()) << (mapping_result.has_value() ? "" : mapping_result.error());
    if (mapping_result.has_value()) {
      mapping = mapping_result.value();
    }
  }

  ntn_initial_ul_position_observation make_observation(uint64_t event_id = 1) const
  {
    ntn_initial_ul_position_observation observation;
    if (!controller.active_plan().has_value()) {
      return observation;
    }
    const ntn_activated_position_plan& plan = *controller.active_plan();
    const auto ro = std::find_if(plan.access_calendar.begin(), plan.access_calendar.end(), [](const auto& intent) {
      return intent.purpose == ntn_access_calendar_purpose::prach_ro;
    });
    if (ro == plan.access_calendar.end()) {
      return observation;
    }
    const auto beam = std::find_if(plan.access_calendar.begin(), plan.access_calendar.end(), [&](const auto& intent) {
      return intent.purpose == ntn_access_calendar_purpose::prach_ul_beam && intent.nci == ro->nci &&
             intent.position_id == ro->position_id && intent.start_time == ro->start_time;
    });
    const ntn_onboard_runtime_cell_route* route = mapping != nullptr ? mapping->resolve_cell_route(ro->nci) : nullptr;
    if (beam == plan.access_calendar.end() || route == nullptr) {
      return observation;
    }

    observation.observation_id           = event_id;
    observation.ue_index                 = uint_to_ue_index(7);
    observation.du_index                 = route->du_index;
    observation.du_cell_index            = route->du_cell_index;
    observation.c_rnti                   = to_rnti(0x4601);
    observation.du_connection_generation = route->du_connection_generation;
    observation.satellite_id             = plan.source.satellite_id;
    observation.catalog_version          = plan.source.catalog_version;
    observation.schedule_version         = plan.source.schedule_version;
    observation.source_content_hash      = plan.source.content_hash;
    observation.calendar_hash            = plan.calendar_hash;
    observation.nci                      = route->identity.nci;
    observation.pci                      = route->identity.pci;
    observation.position_id              = ro->position_id;
    observation.occasion_time            = plan.source.activation_epoch + ro->start_time + std::chrono::microseconds{1};
    observation.ul_beam_port_id           = beam->port_id;
    return observation;
  }

  ntn_initial_ul_position_authorization_request make_request() const
  {
    const ntn_initial_ul_position_observation observation = make_observation();
    ntn_initial_ul_position_authorization_request request;
    request.key = {observation.du_index,
                   observation.du_cell_index,
                   observation.c_rnti,
                   observation.rnti_lease_generation,
                   observation.du_connection_generation};
    request.ue_index                = observation.ue_index;
    request.du_cell_index           = observation.du_cell_index;
    request.du_connection_generation = observation.du_connection_generation;
    request.runtime_mapping         = mapping;
    request.now                     = system_at_ms(2000);
    request.observation_now         = steady_at_ms(10);
    return request;
  }

  ntn_onboard_position_plan_controller controller;
  std::shared_ptr<const ntn_onboard_runtime_mapping_snapshot> mapping;
  ntn_initial_ul_position_observation_store store;
  ntn_initial_ul_position_authorizer        authorizer;
};

class fixed_initial_ul_position_provider final : public ntn_initial_ul_position_observation_provider
{
public:
  explicit fixed_initial_ul_position_provider(ntn_initial_ul_position_take_result result_) :
    result(std::move(result_))
  {
  }

  ntn_initial_ul_position_source_snapshot source_snapshot() const override { return {true, "test_provider", 0}; }
  ntn_initial_ul_position_take_result
  take(const ntn_initial_ul_position_observation_key&, std::chrono::steady_clock::time_point) override
  {
    return result;
  }
  void invalidate_du(du_index_t) override {}
  void invalidate_all() override {}

private:
  ntn_initial_ul_position_take_result result;
};

} // namespace

TEST(ntn_initial_ul_position_store, reports_missing_expired_ambiguous_replayed_and_source_unavailable)
{
  ntn_initial_ul_position_observation_store store;
  const ntn_initial_ul_position_observation_key key{
      uint_to_du_index(4), uint_to_du_cell_index(0), to_rnti(0x4601), 0, 9};

  EXPECT_EQ(store.take(key, steady_at_ms(0)).status,
            ntn_initial_ul_position_take_status::missing);

  ASSERT_EQ(store.record(make_store_observation(1), steady_at_ms(0)),
            ntn_initial_ul_position_record_status::stored);
  EXPECT_EQ(store.take(key, steady_at_ms(1000)).status,
            ntn_initial_ul_position_take_status::expired);

  ASSERT_EQ(store.record(make_store_observation(2), steady_at_ms(2000)),
            ntn_initial_ul_position_record_status::stored);
  EXPECT_EQ(store.take(key, steady_at_ms(2010)).status, ntn_initial_ul_position_take_status::found);
  EXPECT_EQ(store.take(key, steady_at_ms(2020)).status,
            ntn_initial_ul_position_take_status::replayed);

  ASSERT_EQ(store.record(make_store_observation(3), steady_at_ms(4000)),
            ntn_initial_ul_position_record_status::stored);
  ASSERT_EQ(store.record(make_store_observation(4), steady_at_ms(4000)),
            ntn_initial_ul_position_record_status::stored);
  EXPECT_EQ(store.take(key, steady_at_ms(4010)).status,
            ntn_initial_ul_position_take_status::ambiguous);

  store.set_source_ready(false);
  EXPECT_FALSE(store.is_ready());
  EXPECT_EQ(store.take(key, steady_at_ms(4020)).status,
            ntn_initial_ul_position_take_status::source_unavailable);
}

TEST(ntn_initial_ul_position_store, rejects_invalid_observations_and_keys_without_consuming_a_valid_record)
{
  ntn_initial_ul_position_observation_store store;
  auto expect_invalid = [&store](auto mutate) {
    ntn_initial_ul_position_observation observation = make_store_observation();
    mutate(observation);
    EXPECT_EQ(store.record(std::move(observation), steady_at_ms(0)),
              ntn_initial_ul_position_record_status::invalid);
  };

  expect_invalid([](auto& observation) { observation.observation_id = 0; });
  expect_invalid([](auto& observation) { observation.du_index = du_index_t::invalid; });
  expect_invalid([](auto& observation) { observation.du_cell_index = du_cell_index_t::invalid; });
  expect_invalid([](auto& observation) { observation.c_rnti = rnti_t::INVALID_RNTI; });
  expect_invalid([](auto& observation) { observation.satellite_id.clear(); });
  expect_invalid([](auto& observation) { observation.satellite_id = std::string(257, 's'); });
  expect_invalid([](auto& observation) { observation.catalog_version = 0; });
  expect_invalid([](auto& observation) { observation.schedule_version = 0; });
  expect_invalid([](auto& observation) { observation.source_content_hash = std::string(257, 'h'); });
  expect_invalid([](auto& observation) { observation.calendar_hash.clear(); });
  expect_invalid([](auto& observation) { observation.pci = INVALID_PCI; });
  expect_invalid([](auto& observation) { observation.position_id = std::string(257, 'p'); });
  expect_invalid([](auto& observation) { observation.occasion_time = {}; });
  expect_invalid([](auto& observation) {
    observation.ul_beam_port_id = ntn_access_calendar_intent::no_resource_port;
  });

  const ntn_initial_ul_position_observation valid = make_store_observation(2);
  ASSERT_EQ(store.record(valid, steady_at_ms(0)), ntn_initial_ul_position_record_status::stored);
  EXPECT_EQ(store.take({du_index_t::invalid,
                        valid.du_cell_index,
                        valid.c_rnti,
                        valid.rnti_lease_generation,
                        valid.du_connection_generation},
                       steady_at_ms(10))
                .status,
            ntn_initial_ul_position_take_status::missing);
  EXPECT_EQ(store.take({valid.du_index,
                        du_cell_index_t::invalid,
                        valid.c_rnti,
                        valid.rnti_lease_generation,
                        valid.du_connection_generation},
                       steady_at_ms(10))
                .status,
            ntn_initial_ul_position_take_status::missing);
  EXPECT_EQ(store.take({valid.du_index,
                        valid.du_cell_index,
                        rnti_t::INVALID_RNTI,
                        valid.rnti_lease_generation,
                        valid.du_connection_generation},
                       steady_at_ms(10))
                .status,
            ntn_initial_ul_position_take_status::missing);
  EXPECT_EQ(store.take({valid.du_index,
                        valid.du_cell_index,
                        valid.c_rnti,
                        valid.rnti_lease_generation,
                        valid.du_connection_generation},
                       steady_at_ms(10))
                .status,
            ntn_initial_ul_position_take_status::found);
}

TEST(ntn_initial_ul_position_store, source_snapshot_excludes_expired_pending_observations)
{
  ntn_initial_ul_position_observation_store store("trusted_du_observer");
  const auto recorded_at =
      std::chrono::steady_clock::now() - ntn_initial_ul_position_observation_ttl - std::chrono::milliseconds{1};
  ASSERT_EQ(store.record(make_store_observation(), recorded_at), ntn_initial_ul_position_record_status::stored);
  ASSERT_EQ(store.pending_count(), 1U);

  const ntn_initial_ul_position_source_snapshot snapshot = store.source_snapshot();
  EXPECT_TRUE(snapshot.ready);
  EXPECT_EQ(snapshot.authority, "trusted_du_observer");
  EXPECT_EQ(snapshot.pending, 0U);
  EXPECT_FALSE(snapshot.live_device_backend_ready);
  EXPECT_EQ(snapshot.live_device_backend, "none");
}

TEST(ntn_initial_ul_position_store, reports_live_device_readiness_separately_from_provider_availability)
{
  ntn_initial_ul_position_observation_store store("device_provider", true, true);
  ntn_initial_ul_position_observation       observation = make_store_observation();
  observation.authority = ntn_initial_ul_position_observation_authority::sdr_rx_port_verified;
  observation.rnti_lease_generation = 7;
  observation.physical_rx_port_id   = 3;
  observation.mapping_version       = 9;
  observation.mapping_hash          = "rx-mapping-sha256";
  ASSERT_EQ(store.record(observation), ntn_initial_ul_position_record_status::stored);

  const ntn_initial_ul_position_source_snapshot ready = store.source_snapshot();
  EXPECT_TRUE(ready.ready);
  EXPECT_TRUE(ready.rnti_generation_authoritative);
  EXPECT_TRUE(ready.device_verification_capable);
  EXPECT_TRUE(ready.live_device_backend_ready);
  EXPECT_EQ(ready.live_device_backend, "sdr_zmq");

  store.invalidate_all();
  const ntn_initial_ul_position_source_snapshot no_backend = store.source_snapshot();
  EXPECT_TRUE(no_backend.ready);
  EXPECT_FALSE(no_backend.live_device_backend_ready);
  EXPECT_EQ(no_backend.live_device_backend, "none");
}

TEST(ntn_initial_ul_position_store, enforces_the_1024_record_limit_and_rejects_duplicate_event_ids)
{
  ntn_initial_ul_position_observation_store store;
  for (uint64_t event_id = 1; event_id <= max_ntn_initial_ul_position_observations; ++event_id) {
    ASSERT_EQ(store.record(make_store_observation(event_id), steady_at_ms(0)),
              ntn_initial_ul_position_record_status::stored);
  }
  EXPECT_EQ(store.pending_count(), max_ntn_initial_ul_position_observations);
  EXPECT_EQ(store.record(make_store_observation(1), steady_at_ms(0)),
            ntn_initial_ul_position_record_status::replayed);
  EXPECT_EQ(store.record(make_store_observation(max_ntn_initial_ul_position_observations + 1), steady_at_ms(0)),
            ntn_initial_ul_position_record_status::full);
}

TEST(ntn_initial_ul_position_store, supports_generation_zero_and_provider_invalidation)
{
  ntn_initial_ul_position_observation_store store("trusted_du_observer");
  ntn_initial_ul_position_observation       first = make_store_observation(1);
  first.du_connection_generation                  = 0;
  ASSERT_EQ(store.record(first, steady_at_ms(0)), ntn_initial_ul_position_record_status::stored);
  EXPECT_TRUE(store.is_ready());
  EXPECT_EQ(store.authority(), "trusted_du_observer");
  EXPECT_EQ(store.take({first.du_index,
                        first.du_cell_index,
                        first.c_rnti,
                        first.rnti_lease_generation,
                        0},
                       steady_at_ms(10))
                .status,
            ntn_initial_ul_position_take_status::found);

  ntn_initial_ul_position_observation second = make_store_observation(2);
  ntn_initial_ul_position_observation third  = make_store_observation(3);
  third.du_index                            = uint_to_du_index(5);
  ASSERT_EQ(store.record(second, steady_at_ms(100)), ntn_initial_ul_position_record_status::stored);
  ASSERT_EQ(store.record(third, steady_at_ms(100)), ntn_initial_ul_position_record_status::stored);
  EXPECT_EQ(store.pending_count(), 2U);

  store.invalidate_du(uint_to_du_index(4));
  EXPECT_EQ(store.pending_count(), 1U);
  store.invalidate_all();
  EXPECT_EQ(store.pending_count(), 0U);
}

TEST(ntn_initial_ul_position_store, requires_the_complete_du_cell_rnti_and_generation_key)
{
  ntn_initial_ul_position_observation_store store;
  const auto                                 observation = make_store_observation();
  ASSERT_EQ(store.record(observation, steady_at_ms(0)), ntn_initial_ul_position_record_status::stored);

  EXPECT_EQ(store.take({uint_to_du_index(5),
                        observation.du_cell_index,
                        observation.c_rnti,
                        observation.rnti_lease_generation,
                        observation.du_connection_generation},
                       steady_at_ms(10))
                .status,
            ntn_initial_ul_position_take_status::missing);
  EXPECT_EQ(store.take({observation.du_index,
                        uint_to_du_cell_index(1),
                        observation.c_rnti,
                        observation.rnti_lease_generation,
                        observation.du_connection_generation},
                       steady_at_ms(10))
                .status,
            ntn_initial_ul_position_take_status::missing);
  EXPECT_EQ(store.take({observation.du_index,
                        observation.du_cell_index,
                        to_rnti(0x4602),
                        observation.rnti_lease_generation,
                        observation.du_connection_generation},
                       steady_at_ms(10))
                .status,
            ntn_initial_ul_position_take_status::missing);
  EXPECT_EQ(store.take({observation.du_index,
                        observation.du_cell_index,
                        observation.c_rnti,
                        observation.rnti_lease_generation,
                        observation.du_connection_generation + 1},
                       steady_at_ms(10))
                .status,
            ntn_initial_ul_position_take_status::du_generation_mismatch);
  EXPECT_EQ(store.take({observation.du_index,
                        observation.du_cell_index,
                        observation.c_rnti,
                        observation.rnti_lease_generation,
                        observation.du_connection_generation},
                       steady_at_ms(10))
                .status,
            ntn_initial_ul_position_take_status::found);
}

TEST(ntn_initial_ul_position_store, accepts_concurrent_records_without_exceeding_the_bound)
{
  ntn_initial_ul_position_observation_store store;
  std::vector<ntn_initial_ul_position_record_status> results(256, ntn_initial_ul_position_record_status::invalid);
  std::vector<std::thread> workers;
  for (uint64_t worker = 0; worker != 8; ++worker) {
    workers.emplace_back([&store, &results, worker]() {
      for (uint64_t item = 0; item != 32; ++item) {
        const uint64_t event_id = worker * 32 + item + 1;
        results[event_id - 1]   = store.record(make_store_observation(event_id), steady_at_ms(0));
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  EXPECT_TRUE(std::all_of(results.begin(), results.end(), [](ntn_initial_ul_position_record_status result) {
    return result == ntn_initial_ul_position_record_status::stored;
  }));
  EXPECT_EQ(store.pending_count(), 256U);
}

TEST(ntn_initial_ul_position_store, scopes_replay_identifiers_to_one_du_connection)
{
  ntn_initial_ul_position_observation_store store;
  ntn_initial_ul_position_observation       first = make_store_observation(1);
  ntn_initial_ul_position_observation       second = first;
  second.du_index                                  = uint_to_du_index(5);
  second.du_connection_generation                  = 3;

  ASSERT_EQ(store.record(first, steady_at_ms(0)), ntn_initial_ul_position_record_status::stored);
  ASSERT_EQ(store.record(second, steady_at_ms(0)), ntn_initial_ul_position_record_status::stored);
  EXPECT_EQ(store.pending_count(), 2U);

  const ntn_initial_ul_position_observation_key first_key{
      first.du_index, first.du_cell_index, first.c_rnti, first.rnti_lease_generation, first.du_connection_generation};
  const ntn_initial_ul_position_observation_key second_key{second.du_index,
                                                            second.du_cell_index,
                                                            second.c_rnti,
                                                            second.rnti_lease_generation,
                                                            second.du_connection_generation};
  EXPECT_EQ(store.take(first_key, steady_at_ms(10)).status, ntn_initial_ul_position_take_status::found);
  EXPECT_EQ(store.take(second_key, steady_at_ms(10)).status, ntn_initial_ul_position_take_status::found);
}

TEST_F(ntn_initial_ul_position_authorizer_test, authorizes_one_exact_observation_only_once)
{
  ASSERT_EQ(store.record(make_observation(), steady_at_ms(0)), ntn_initial_ul_position_record_status::stored);

  const ntn_initial_ul_position_authorization_request request = make_request();
  const auto first = authorizer.authorize(request);
  EXPECT_EQ(first.status, ntn_initial_ul_position_authorization_status::authorized);
  EXPECT_EQ(first.observation_id, 1U);
  EXPECT_FALSE(first.position_id.empty());
  EXPECT_EQ(first.plan_audit.decision, ntn_initial_access_plan_decision::accept);

  EXPECT_EQ(authorizer.authorize(request).status,
             ntn_initial_ul_position_authorization_status::replayed_observation);
}

TEST_F(ntn_initial_ul_position_authorizer_test, strict_admission_requires_a_device_verified_receive_source)
{
  ntn_initial_ul_position_observation injected = make_observation(1);
  ASSERT_EQ(store.record(injected, steady_at_ms(0)), ntn_initial_ul_position_record_status::stored);
  auto strict_request                    = make_request();
  strict_request.require_device_verified = true;
  EXPECT_EQ(authorizer.authorize(strict_request).status,
            ntn_initial_ul_position_authorization_status::receive_port_unavailable);

  ntn_initial_ul_position_observation device = make_observation(2);
  device.authority                           = ntn_initial_ul_position_observation_authority::sdr_rx_port_verified;
  device.rnti_lease_generation               = 1;
  device.physical_rx_port_id                 = 3;
  device.mapping_version                     = 7;
  device.mapping_hash                        = "rx-mapping-sha256";
  ASSERT_EQ(store.record(device, steady_at_ms(20)), ntn_initial_ul_position_record_status::stored);
  strict_request.key.rnti_lease_generation = device.rnti_lease_generation;
  strict_request.observation_now           = steady_at_ms(30);
  EXPECT_EQ(authorizer.authorize(strict_request).status,
            ntn_initial_ul_position_authorization_status::authorized);
}

TEST_F(ntn_initial_ul_position_authorizer_test, enforces_receive_authority_identity_boundaries)
{
  auto expect_source_unavailable = [&](ntn_initial_ul_position_observation observation) {
    fixed_initial_ul_position_provider provider{{ntn_initial_ul_position_take_status::found, std::move(observation)}};
    ntn_initial_ul_position_authorizer provider_authorizer{provider, controller};
    auto                               request = make_request();
    request.key.rnti_lease_generation         = 1;
    request.require_device_verified            = true;
    EXPECT_EQ(provider_authorizer.authorize(request).status,
              ntn_initial_ul_position_authorization_status::source_unavailable);
  };

  ntn_initial_ul_position_observation invalid_sdr = make_observation(10);
  invalid_sdr.authority                           = ntn_initial_ul_position_observation_authority::sdr_rx_port_verified;
  invalid_sdr.rnti_lease_generation               = 1;
  invalid_sdr.physical_rx_port_id                 = 255;
  invalid_sdr.mapping_version                     = 7;
  invalid_sdr.mapping_hash                        = "rx-mapping-sha256";
  expect_source_unavailable(invalid_sdr);

  ntn_initial_ul_position_observation invalid_ofh = make_observation(11);
  invalid_ofh.authority                           = ntn_initial_ul_position_observation_authority::ofh_beam_id_verified;
  invalid_ofh.rnti_lease_generation               = 1;
  invalid_ofh.physical_rx_port_id                 = 254;
  invalid_ofh.ofh_prach_eaxc                      = 32;
  invalid_ofh.ofh_beam_id                         = 1;
  invalid_ofh.mapping_version                     = 7;
  invalid_ofh.mapping_hash                        = "rx-mapping-sha256";
  expect_source_unavailable(invalid_ofh);

  ntn_initial_ul_position_observation invalid_software = make_observation(12);
  invalid_software.authority              = ntn_initial_ul_position_observation_authority::software_attributed;
  invalid_software.rnti_lease_generation = 1;
  invalid_software.mapping_version        = 7;
  invalid_software.mapping_hash           = "self-asserted-mapping";
  expect_source_unavailable(invalid_software);

  ntn_initial_ul_position_observation valid_ofh = make_observation(13);
  valid_ofh.authority                           = ntn_initial_ul_position_observation_authority::ofh_beam_id_verified;
  valid_ofh.rnti_lease_generation               = 1;
  valid_ofh.physical_rx_port_id                 = 254;
  valid_ofh.ofh_prach_eaxc                      = 31;
  valid_ofh.ofh_beam_id                         = 0x7fff;
  valid_ofh.mapping_version                     = 7;
  valid_ofh.mapping_hash                        = "rx-mapping-sha256";
  fixed_initial_ul_position_provider valid_provider{{ntn_initial_ul_position_take_status::found, valid_ofh}};
  ntn_initial_ul_position_authorizer valid_authorizer{valid_provider, controller};
  auto                               strict_request = make_request();
  strict_request.key.rnti_lease_generation         = 1;
  strict_request.require_device_verified            = true;
  EXPECT_EQ(valid_authorizer.authorize(strict_request).status,
            ntn_initial_ul_position_authorization_status::authorized);
}

TEST_F(ntn_initial_ul_position_authorizer_test, audit_admission_can_record_an_injected_observation)
{
  ASSERT_EQ(store.record(make_observation(), steady_at_ms(0)), ntn_initial_ul_position_record_status::stored);
  ntn_initial_ul_position_authorization_request request = make_request();
  request.require_device_verified                        = false;
  EXPECT_EQ(authorizer.authorize(request).status, ntn_initial_ul_position_authorization_status::authorized);
}

TEST_F(ntn_initial_ul_position_authorizer_test, rejects_provider_results_that_violate_the_take_contract)
{
  fixed_initial_ul_position_provider missing_payload{{ntn_initial_ul_position_take_status::found, std::nullopt}};
  ntn_initial_ul_position_authorizer  missing_payload_authorizer{missing_payload, controller};
  EXPECT_EQ(missing_payload_authorizer.authorize(make_request()).status,
            ntn_initial_ul_position_authorization_status::source_unavailable);

  fixed_initial_ul_position_provider unexpected_payload{
      {ntn_initial_ul_position_take_status::missing, make_observation(50)}};
  ntn_initial_ul_position_authorizer unexpected_payload_authorizer{unexpected_payload, controller};
  EXPECT_EQ(unexpected_payload_authorizer.authorize(make_request()).status,
            ntn_initial_ul_position_authorization_status::observation_missing);

  ntn_initial_ul_position_observation invalid_observation = make_observation(51);
  invalid_observation.observation_id                       = 0;
  fixed_initial_ul_position_provider invalid_payload{
      {ntn_initial_ul_position_take_status::found, invalid_observation}};
  ntn_initial_ul_position_authorizer invalid_payload_authorizer{invalid_payload, controller};
  EXPECT_EQ(invalid_payload_authorizer.authorize(make_request()).status,
            ntn_initial_ul_position_authorization_status::source_unavailable);
}

TEST_F(ntn_initial_ul_position_authorizer_test, rejects_stale_or_future_prach_observation_times)
{
  ntn_initial_ul_position_observation stale = make_observation(10);
  ASSERT_EQ(store.record(stale, steady_at_ms(0)), ntn_initial_ul_position_record_status::stored);
  auto stale_request = make_request();
  stale_request.now  = stale.occasion_time + ntn_initial_ul_position_observation_ttl + std::chrono::milliseconds{1};
  EXPECT_EQ(authorizer.authorize(stale_request).status,
            ntn_initial_ul_position_authorization_status::active_plan_mismatch);

  store.invalidate_all();
  ntn_initial_ul_position_observation future = make_observation(11);
  ASSERT_EQ(store.record(future, steady_at_ms(100)), ntn_initial_ul_position_record_status::stored);
  auto future_request            = make_request();
  future_request.observation_now = steady_at_ms(110);
  future_request.now             = future.occasion_time - std::chrono::microseconds{1};
  EXPECT_EQ(authorizer.authorize(future_request).status,
            ntn_initial_ul_position_authorization_status::active_plan_mismatch);
}

TEST_F(ntn_initial_ul_position_authorizer_test, consumes_and_rejects_ue_and_du_generation_mismatches)
{
  ntn_initial_ul_position_observation wrong_ue = make_observation(1);
  wrong_ue.ue_index                            = uint_to_ue_index(8);
  ASSERT_EQ(store.record(wrong_ue, steady_at_ms(0)), ntn_initial_ul_position_record_status::stored);
  EXPECT_EQ(authorizer.authorize(make_request()).status,
            ntn_initial_ul_position_authorization_status::ue_identity_mismatch);

  ntn_initial_ul_position_observation wrong_du = make_observation(2);
  wrong_du.du_connection_generation           = 10;
  ASSERT_EQ(store.record(wrong_du, steady_at_ms(2000)), ntn_initial_ul_position_record_status::stored);
  auto du_request             = make_request();
  du_request.observation_now  = steady_at_ms(2010);
  EXPECT_EQ(authorizer.authorize(du_request).status,
            ntn_initial_ul_position_authorization_status::du_generation_mismatch);
}

TEST_F(ntn_initial_ul_position_authorizer_test, rejects_missing_or_mismatched_runtime_mapping)
{
  ASSERT_EQ(store.record(make_observation(1), steady_at_ms(0)), ntn_initial_ul_position_record_status::stored);
  auto unavailable            = make_request();
  unavailable.runtime_mapping = nullptr;
  EXPECT_EQ(authorizer.authorize(unavailable).status,
            ntn_initial_ul_position_authorization_status::active_plan_unavailable);

  ntn_initial_ul_position_observation wrong_hash = make_observation(2);
  wrong_hash.calendar_hash                       = "sha256:" + std::string(64, 'f');
  ASSERT_EQ(store.record(wrong_hash, steady_at_ms(2000)), ntn_initial_ul_position_record_status::stored);
  auto mismatch            = make_request();
  mismatch.observation_now = steady_at_ms(2010);
  EXPECT_EQ(authorizer.authorize(mismatch).status,
            ntn_initial_ul_position_authorization_status::active_plan_mismatch);
}

TEST_F(ntn_initial_ul_position_authorizer_test, fails_closed_when_the_observation_source_is_unavailable)
{
  ASSERT_EQ(store.record(make_observation(), steady_at_ms(0)), ntn_initial_ul_position_record_status::stored);
  store.set_source_ready(false);

  EXPECT_EQ(authorizer.authorize(make_request()).status,
            ntn_initial_ul_position_authorization_status::source_unavailable);
  EXPECT_EQ(store.pending_count(), 0U);
}

TEST_F(ntn_initial_ul_position_authorizer_test, returns_plan_rejected_for_wrong_scheduled_uplink_port)
{
  ntn_initial_ul_position_observation wrong_port = make_observation();
  ++wrong_port.ul_beam_port_id;
  ASSERT_EQ(store.record(wrong_port, steady_at_ms(0)), ntn_initial_ul_position_record_status::stored);

  const auto result = authorizer.authorize(make_request());
  EXPECT_EQ(result.status, ntn_initial_ul_position_authorization_status::active_plan_mismatch);
  EXPECT_EQ(result.plan_audit.reason, ntn_initial_access_plan_reason::resource_port_mismatch);
}

TEST_F(ntn_initial_ul_position_authorizer_test, preserves_precise_plan_rejection_reasons)
{
  uint64_t event_id = 100;
  auto expect_rejected = [this, &event_id](auto mutate, ntn_initial_access_plan_reason expected_reason) {
    store.invalidate_all();
    ntn_initial_ul_position_observation observation = make_observation(event_id++);
    mutate(observation);
    ASSERT_EQ(store.record(observation, steady_at_ms(static_cast<int64_t>(event_id * 10))),
              ntn_initial_ul_position_record_status::stored);
    ntn_initial_ul_position_authorization_request request = make_request();
    request.observation_now = steady_at_ms(static_cast<int64_t>(event_id * 10 + 1));
    const auto result = authorizer.authorize(request);
    EXPECT_EQ(result.status, ntn_initial_ul_position_authorization_status::active_plan_mismatch);
    EXPECT_EQ(result.plan_audit.reason, expected_reason);
  };

  expect_rejected([](auto& observation) { observation.satellite_id = "P01-S02"; },
                  ntn_initial_access_plan_reason::satellite_mismatch);
  expect_rejected([](auto& observation) { ++observation.catalog_version; },
                  ntn_initial_access_plan_reason::catalog_version_mismatch);
  expect_rejected([](auto& observation) { ++observation.schedule_version; },
                  ntn_initial_access_plan_reason::schedule_version_mismatch);
  expect_rejected([](auto& observation) { observation.source_content_hash = "sha256:wrong"; },
                  ntn_initial_access_plan_reason::source_hash_mismatch);
  expect_rejected([](auto& observation) { observation.calendar_hash = "sha256:wrong"; },
                  ntn_initial_access_plan_reason::calendar_hash_mismatch);
  expect_rejected(
      [](auto& observation) {
        observation.nci = make_cell(0x123450003ULL, 303).nci;
        observation.pci = 303;
      },
      ntn_initial_access_plan_reason::cell_identity_mismatch);
  expect_rejected([](auto& observation) { observation.position_id = "G999999"; },
                  ntn_initial_access_plan_reason::position_not_assigned_to_cell);
  expect_rejected([](auto& observation) { observation.occasion_time += std::chrono::milliseconds{10}; },
                  ntn_initial_access_plan_reason::prach_occasion_not_scheduled);
}

TEST(ntn_initial_ul_position_status, exposes_stable_machine_readable_strings)
{
  EXPECT_STREQ(to_string(ntn_initial_ul_position_record_status::full), "full");
  EXPECT_STREQ(to_string(ntn_initial_ul_position_authorization_status::source_unavailable), "source_unavailable");
  EXPECT_STREQ(to_string(ntn_initial_ul_position_authorization_status::active_plan_mismatch),
               "active_plan_mismatch");
}
