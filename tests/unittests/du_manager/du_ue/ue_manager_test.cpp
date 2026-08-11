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

#include "lib/du/du_high/du_manager/du_ue/du_ue_manager.h"
#include "tests/unittests/du_manager/du_manager_test_helpers.h"
#include "srsran/du/du_cell_config_helpers.h"
#include "srsran/mac/mac_pdu_handler.h"
#include "srsran/support/executors/manual_task_worker.h"
#include "srsran/support/test_utils.h"
#include <gtest/gtest.h>

/// \file
/// \brief In this file, we unit test the interaction between DU UE procedures in the DU UE Manager. For unit tests
/// addressing the specific details of each DU UE manager procedure, please check procedures/ directory.

using namespace srsran;
using namespace srs_du;

static mac_ntn_initial_ul_position_record make_du_initial_ul_observation(unsigned index = 0)
{
  mac_ntn_initial_ul_position_record observation;
  observation.observation_id         = 1000 + index;
  observation.cell_index             = to_du_cell_index(0);
  observation.c_rnti                 = to_rnti(0x4601 + index);
  observation.rnti_generation        = 17;
  observation.nci                    = nr_cell_identity::create(0x12345).value();
  observation.pci                    = pci_t{17};
  observation.authority              = mac_ntn_initial_ul_position_authority::sdr_rx_port_verified;
  observation.schedule_version       = 21;
  observation.calendar_hash          = std::string(64, 'a');
  observation.mapping_version        = 3;
  observation.mapping_hash           = std::string(64, 'b');
  observation.position_id            = "G000123";
  observation.cell_local_port        = 4;
  observation.physical_rx_port       = 2;
  observation.receive_port_margin_db = 6.25F;
  observation.calendar_cycle_index   = 9;
  observation.occasion_offset_us     = 40000;
  return observation;
}

static f1ap_ntn_initial_ul_position_query
make_du_initial_ul_query(gnb_du_ue_f1ap_id_t f1ap_ue_id, const mac_ntn_initial_ul_position_record& observation)
{
  f1ap_ntn_initial_ul_position_query query;
  query.query_generation         = 7;
  query.nonce                    = 0x1020304050607080ULL;
  query.connection_token         = 0x8877665544332211ULL;
  query.gnb_du_id                = int_to_gnb_du_id(1);
  query.cell_cgi                 =
      nr_cell_global_id_t{plmn_identity::test_value(), observation.nci};
  query.cell_index               = observation.cell_index;
  query.pci                      = observation.pci;
  query.gnb_du_ue_f1ap_id        = f1ap_ue_id;
  query.c_rnti                   = observation.c_rnti;
  query.expected_rnti_generation = observation.rnti_generation;
  return query;
}

TEST(du_ntn_initial_ul_position_store_test, exact_query_consumes_once_and_same_nonce_is_idempotent)
{
  du_ntn_initial_ul_position_store store;
  const auto                       now         = du_ntn_initial_ul_position_store::clock::time_point{} +
                         std::chrono::seconds{10};
  const auto observation = make_du_initial_ul_observation();
  const auto f1ap_ue_id  = int_to_gnb_du_ue_f1ap_id(51);
  const auto query       = make_du_initial_ul_query(f1ap_ue_id, observation);

  ASSERT_TRUE(store.store(f1ap_ue_id, observation, now));
  const auto first = store.query(query, now + std::chrono::milliseconds{1});
  ASSERT_TRUE(first.accepted);
  EXPECT_EQ(first.observation_id, observation.observation_id);
  EXPECT_EQ(first.calendar_cycle_index, observation.calendar_cycle_index);
  EXPECT_EQ(first.occasion_offset_us, observation.occasion_offset_us);

  const auto retry = store.query(query, now + std::chrono::milliseconds{2});
  EXPECT_TRUE(retry.accepted);
  EXPECT_EQ(retry.position_id, first.position_id);

  auto reused_nonce = query;
  ++reused_nonce.query_generation;
  const auto nonce_mismatch = store.query(reused_nonce, now + std::chrono::milliseconds{3});
  EXPECT_FALSE(nonce_mismatch.accepted);
  EXPECT_EQ(nonce_mismatch.reason, "nonce_identity_mismatch");

  auto different_nonce = query;
  ++different_nonce.nonce;
  const auto replay = store.query(different_nonce, now + std::chrono::milliseconds{4});
  EXPECT_FALSE(replay.accepted);
  EXPECT_EQ(replay.reason, "observation_consumed");
}

TEST(du_ntn_initial_ul_position_store_test, mismatched_generation_does_not_consume_and_entry_expires_at_one_second)
{
  du_ntn_initial_ul_position_store store;
  const auto                       now         = du_ntn_initial_ul_position_store::clock::time_point{} +
                         std::chrono::seconds{10};
  const auto observation = make_du_initial_ul_observation();
  const auto f1ap_ue_id  = int_to_gnb_du_ue_f1ap_id(51);
  auto       query       = make_du_initial_ul_query(f1ap_ue_id, observation);

  ASSERT_TRUE(store.store(f1ap_ue_id, observation, now));
  ++query.expected_rnti_generation;
  const auto mismatch = store.query(query, now + std::chrono::milliseconds{1});
  EXPECT_FALSE(mismatch.accepted);
  EXPECT_EQ(mismatch.reason, "rnti_generation_mismatch");

  query.expected_rnti_generation = observation.rnti_generation;
  const auto expired = store.query(query, now + du_ntn_initial_ul_position_store::entry_ttl);
  EXPECT_FALSE(expired.accepted);
  EXPECT_EQ(expired.reason, "observation_expired");
}

TEST(du_ntn_initial_ul_position_store_test, non_authoritative_record_preserves_its_machine_readable_reason)
{
  du_ntn_initial_ul_position_store store;
  const auto now = du_ntn_initial_ul_position_store::clock::time_point{} + std::chrono::seconds{10};
  auto       observation = make_du_initial_ul_observation();
  observation.authority  = mac_ntn_initial_ul_position_authority::none;
  observation.reason     = "ambiguous_receive_position";
  const auto f1ap_ue_id  = int_to_gnb_du_ue_f1ap_id(51);

  ASSERT_TRUE(store.store(f1ap_ue_id, observation, now));
  const auto result = store.query(make_du_initial_ul_query(f1ap_ue_id, observation), now);
  EXPECT_FALSE(result.accepted);
  EXPECT_EQ(result.reason, "ambiguous_receive_position");
  EXPECT_EQ(result.observation_id, 0U);
}

TEST(du_ntn_initial_ul_position_store_test, software_observation_without_physical_port_remains_auditable)
{
  du_ntn_initial_ul_position_store store;
  const auto now = du_ntn_initial_ul_position_store::clock::time_point{} + std::chrono::seconds{10};
  auto       observation             = make_du_initial_ul_observation();
  observation.authority              = mac_ntn_initial_ul_position_authority::software_attributed;
  observation.physical_rx_port       = std::numeric_limits<uint16_t>::max();
  observation.mapping_version        = 0;
  observation.mapping_hash.clear();
  const auto f1ap_ue_id = int_to_gnb_du_ue_f1ap_id(51);

  ASSERT_TRUE(store.store(f1ap_ue_id, observation, now));
  const auto result = store.query(make_du_initial_ul_query(f1ap_ue_id, observation), now);
  ASSERT_TRUE(result.accepted);
  EXPECT_EQ(result.authority, f1ap_ntn_initial_ul_position_authority::software_attributed);
  EXPECT_EQ(result.physical_port, f1ap_ntn_initial_ul_position_detail::unavailable_physical_port_id);
}

TEST(du_ntn_initial_ul_position_store_test, capacity_is_bounded_without_overwriting_existing_records)
{
  du_ntn_initial_ul_position_store store;
  const auto now = du_ntn_initial_ul_position_store::clock::time_point{} + std::chrono::seconds{10};
  for (unsigned i = 0; i != du_ntn_initial_ul_position_store::max_entries; ++i) {
    ASSERT_TRUE(store.store(int_to_gnb_du_ue_f1ap_id(i + 1), make_du_initial_ul_observation(i), now));
  }
  EXPECT_EQ(store.size(), du_ntn_initial_ul_position_store::max_entries);
  EXPECT_FALSE(store.store(int_to_gnb_du_ue_f1ap_id(2000), make_du_initial_ul_observation(2000), now));
  EXPECT_EQ(store.size(), du_ntn_initial_ul_position_store::max_entries);
}

TEST(du_ntn_initial_ul_position_store_test, plan_and_cell_invalidation_remove_records_without_reuse)
{
  du_ntn_initial_ul_position_store store;
  const auto now = du_ntn_initial_ul_position_store::clock::time_point{} + std::chrono::seconds{10};
  const auto observation = make_du_initial_ul_observation();
  const auto f1ap_ue_id  = int_to_gnb_du_ue_f1ap_id(51);
  const auto query       = make_du_initial_ul_query(f1ap_ue_id, observation);

  ASSERT_TRUE(store.store(f1ap_ue_id, observation, now));
  store.retain_plan(observation.schedule_version + 1, observation.calendar_hash);
  EXPECT_EQ(store.query(query, now).reason, "observation_missing");

  ASSERT_TRUE(store.store(f1ap_ue_id, observation, now));
  store.invalidate_cell(observation.cell_index);
  EXPECT_EQ(store.query(query, now).reason, "observation_missing");
}

TEST(du_ntn_initial_ul_position_store_test, lease_invalidation_is_scoped_to_the_retired_rnti)
{
  du_ntn_initial_ul_position_store store;
  const auto now = du_ntn_initial_ul_position_store::clock::time_point{} + std::chrono::seconds{10};
  const auto first_observation  = make_du_initial_ul_observation(0);
  const auto second_observation = make_du_initial_ul_observation(1);
  const auto first_f1ap_ue_id   = int_to_gnb_du_ue_f1ap_id(51);
  const auto second_f1ap_ue_id  = int_to_gnb_du_ue_f1ap_id(52);

  ASSERT_TRUE(store.store(first_f1ap_ue_id, first_observation, now));
  ASSERT_TRUE(store.store(second_f1ap_ue_id, second_observation, now));
  store.invalidate_rntis(first_observation.cell_index, {first_observation.c_rnti});

  EXPECT_EQ(store.query(make_du_initial_ul_query(first_f1ap_ue_id, first_observation), now).reason,
            "observation_missing");
  EXPECT_TRUE(store.query(make_du_initial_ul_query(second_f1ap_ue_id, second_observation), now).accepted);
}

TEST(du_ntn_initial_ul_position_store_test, clearing_a_replacement_plan_keeps_previous_plan_observations)
{
  du_ntn_initial_ul_position_store store;
  const auto now = du_ntn_initial_ul_position_store::clock::time_point{} + std::chrono::seconds{10};
  const auto previous_observation = make_du_initial_ul_observation(0);
  auto       replacement_observation = make_du_initial_ul_observation(1);
  replacement_observation.schedule_version++;
  replacement_observation.calendar_hash = std::string(64, 'c');
  const auto previous_f1ap_ue_id    = int_to_gnb_du_ue_f1ap_id(51);
  const auto replacement_f1ap_ue_id = int_to_gnb_du_ue_f1ap_id(52);

  ASSERT_TRUE(store.store(previous_f1ap_ue_id, previous_observation, now));
  ASSERT_TRUE(store.store(replacement_f1ap_ue_id, replacement_observation, now));
  store.erase_plan(replacement_observation.schedule_version, replacement_observation.calendar_hash);

  EXPECT_TRUE(store.query(make_du_initial_ul_query(previous_f1ap_ue_id, previous_observation), now).accepted);
  EXPECT_EQ(store.query(make_du_initial_ul_query(replacement_f1ap_ue_id, replacement_observation), now).reason,
            "observation_missing");
}

class du_ue_manager_tester : public ::testing::Test
{
protected:
  du_ue_manager_tester()
  {
    srslog::fetch_basic_logger("DU-MNG").set_level(srslog::basic_levels::debug);
    srslog::fetch_basic_logger("TEST").set_level(srslog::basic_levels::debug);
    srslog::init();

    // By default F1AP creates two F1-C bearers.
    f1ap_dummy.next_ue_create_response.result = true;
    f1ap_dummy.next_ue_create_response.f1c_bearers_added.resize(2);
  }
  ~du_ue_manager_tester() override { srslog::flush(); }

  ul_ccch_indication_message create_ul_ccch_message(rnti_t rnti)
  {
    ul_ccch_indication_message ccch_ind{};
    ccch_ind.cell_index = to_du_cell_index(0);
    ccch_ind.tc_rnti    = rnti;
    ccch_ind.subpdu     = byte_buffer::create({0, 1, 2, 3, 4, 5}).value();
    return ccch_ind;
  }

  void push_ul_ccch_message(ul_ccch_indication_message ccch_ind)
  {
    test_logger.info("TEST: Pushing UL CCCH indication for rnti={}...", ccch_ind.tc_rnti);
    ue_mng.handle_ue_create_request(ccch_ind);
  }

  void push_f1ap_ue_delete_request(du_ue_index_t ue_index)
  {
    f1ap_ue_delete_request ue_del_req{};
    ue_del_req.ue_index = ue_index;
    test_logger.info("TEST: Starting UE deletion with UE index={}...", fmt::underlying(ue_del_req.ue_index));
    ue_mng.schedule_async_task(ue_del_req.ue_index, ue_mng.handle_ue_delete_request(ue_del_req));
  }

  void mac_completes_ue_creation(bool result)
  {
    mac_dummy.wait_ue_create.result.ue_index   = get_last_ue_index();
    mac_dummy.wait_ue_create.result.cell_index = to_du_cell_index(0);
    mac_dummy.wait_ue_create.result.allocated_crnti =
        result ? mac_dummy.last_ue_create_msg->crnti : rnti_t::INVALID_RNTI;
    mac_dummy.wait_ue_create.ready_ev.set();
    worker.run_pending_tasks();
  }

  void mac_completes_ue_deletion()
  {
    mac_dummy.wait_ue_delete.result.result = true;
    mac_dummy.wait_ue_delete.ready_ev.set();
  }

  bool is_ue_creation_complete() const { return not mac_dummy.last_pushed_ul_ccch_msg.empty(); }

  du_ue_index_t get_last_ue_index() const
  {
    srsran_assert(f1ap_dummy.last_ue_create.has_value(), "No UE creation request was provided");
    return f1ap_dummy.last_ue_create.value().ue_index;
  }

  srslog::basic_logger&      test_logger = srslog::fetch_basic_logger("TEST");
  timer_manager              timers;
  manual_task_worker         worker{128};
  dummy_ue_executor_mapper   ue_execs{worker};
  dummy_cell_executor_mapper cell_execs{worker};

  std::vector<du_cell_config>            cells = {config_helpers::make_default_du_cell_config()};
  f1ap_test_dummy                        f1ap_dummy;
  f1u_gateway_dummy                      f1u_dummy;
  mac_test_dummy                         mac_dummy;
  null_rlc_pcap                          rlc_pcap;
  dummy_ue_resource_configurator_factory cell_res_alloc;

  du_manager_params params{{"srsgnb", (gnb_du_id_t)1, 1, cells},
                           {timers, worker, ue_execs, cell_execs},
                           {f1ap_dummy, f1ap_dummy, f1ap_dummy, f1ap_dummy},
                           {f1u_dummy},
                           {mac_dummy, f1ap_dummy, f1ap_dummy, rlc_pcap},
                           {mac_dummy}};

  du_ue_manager ue_mng{params, cell_res_alloc};
};

TEST_F(du_ue_manager_tester, when_ue_create_request_is_received_du_manager_requests_f1ap_and_mac_to_create_ue)
{
  // Action: UL CCCH Message received.
  ul_ccch_indication_message ccch_ind = create_ul_ccch_message(to_rnti(0x4601));
  push_ul_ccch_message(ccch_ind);

  // TEST: F1AP received request to create UE.
  TESTASSERT(f1ap_dummy.last_ue_create.has_value());
  du_ue_index_t ue_index = f1ap_dummy.last_ue_create.value().ue_index;
  TESTASSERT(ue_index < MAX_NOF_DU_UES);

  // TEST: MAC received UE creation request.
  TESTASSERT(mac_dummy.last_ue_create_msg.has_value());
  TESTASSERT_EQ(ccch_ind.tc_rnti, mac_dummy.last_ue_create_msg->crnti);

  // TEST: DU UE manager registers UE being created.
  ASSERT_TRUE(ue_mng.find_ue(ue_index) != nullptr);
  ASSERT_EQ(to_value(ue_mng.find_ue(ue_index)->rnti), 0x4601);
}

TEST_F(du_ue_manager_tester,
       when_ue_create_request_is_received_du_manager_requests_mac_to_create_ue_and_awaits_response)
{
  // Action 1: UL CCCH Message received.
  ul_ccch_indication_message ccch_ind = create_ul_ccch_message(to_rnti(0x4601));
  push_ul_ccch_message(ccch_ind);

  // TEST: While MAC does not respond, UE creation is not complete.
  ASSERT_FALSE(is_ue_creation_complete());

  // Action 2: MAC UE creation completed.
  mac_completes_ue_creation(true);

  // TEST: DU manager completes DU UE creation procedure with success.
  ASSERT_TRUE(is_ue_creation_complete());
}

TEST_F(du_ue_manager_tester, successful_ue_creation_preserves_initial_ul_position_under_actual_f1_identity)
{
  auto observation = make_du_initial_ul_observation();
  observation.nci   = cells[0].nr_cgi.nci;
  observation.pci   = cells[0].pci;
  auto ccch_ind     = create_ul_ccch_message(observation.c_rnti);
  ccch_ind.ntn_initial_ul_position = observation;
  f1ap_dummy.next_ue_create_response.f1ap_ue_id = int_to_gnb_du_ue_f1ap_id(51);

  push_ul_ccch_message(ccch_ind);
  mac_completes_ue_creation(true);

  const auto result = ue_mng.handle_ntn_initial_ul_position_query(
      make_du_initial_ul_query(f1ap_dummy.next_ue_create_response.f1ap_ue_id, observation));
  ASSERT_TRUE(result.accepted);
  EXPECT_EQ(result.observation_id, observation.observation_id);
  EXPECT_EQ(result.position_id, observation.position_id);
  EXPECT_EQ(result.calendar_cycle_index, observation.calendar_cycle_index);
}

TEST_F(du_ue_manager_tester, when_mac_fails_to_create_ue_then_no_ue_is_created_in_du)
{
  // Action: UL CCCH Message received and MAC UE creation fails.
  ul_ccch_indication_message ccch_ind = create_ul_ccch_message(to_rnti(0x4601));
  push_ul_ccch_message(ccch_ind);
  mac_completes_ue_creation(false);

  // TEST: DU manager completes DU UE creation procedure with failure.
  ASSERT_EQ(ue_mng.nof_ues(), 0);
  ASSERT_FALSE(is_ue_creation_complete());
}

TEST_F(du_ue_manager_tester, inexistent_ue_index_removal_is_handled)
{
  // Action: Request UE deletion for inexistent UE Index.
  push_f1ap_ue_delete_request(to_du_ue_index(test_rgen::uniform_int<unsigned>(0, MAX_NOF_DU_UES - 1)));

  // There should not be any reply from MAC and F1AP should receive failure signal
  ASSERT_EQ(ue_mng.nof_ues(), 0);
  ASSERT_FALSE(mac_dummy.last_ue_delete_msg.has_value());
  // TODO: F1AP check
}

TEST_F(du_ue_manager_tester,
       when_request_for_ue_creation_and_removal_are_received_concurrently_then_the_procedures_run_in_sequence)
{
  // Action 1: UL CCCH Message and UE deletion request received concurrently.
  push_ul_ccch_message(create_ul_ccch_message(to_rnti(0x4601)));
  push_f1ap_ue_delete_request(get_last_ue_index());
  worker.run_pending_tasks();

  // MAC and F1AP receive request to create UE.
  ASSERT_TRUE(mac_dummy.last_ue_create_msg.has_value());
  ASSERT_TRUE(f1ap_dummy.last_ue_create.has_value());

  // Until MAC completes UE creation, F1AP and MAC should not receive request to delete UE.
  ASSERT_FALSE(mac_dummy.last_ue_delete_msg.has_value());
  ASSERT_FALSE(f1ap_dummy.last_ue_release_req.has_value());
  mac_completes_ue_creation(true);
  worker.run_pending_tasks();
  ASSERT_TRUE(mac_dummy.last_ue_delete_msg.has_value());
  ASSERT_EQ(get_last_ue_index(), mac_dummy.last_ue_delete_msg->ue_index);

  // Action 2: MAC finishes UE deletion.
  ASSERT_NE(ue_mng.nof_ues(), 0);
  mac_completes_ue_deletion();

  // UE deleted from the DU.
  ASSERT_EQ(ue_mng.nof_ues(), 0);
}

TEST_F(du_ue_manager_tester,
       when_requests_for_ue_creation_are_received_sequentially_then_the_created_ues_have_different_indexes)
{
  // Action 1: UL CCCH Message received and UE creation completes.
  push_ul_ccch_message(create_ul_ccch_message(to_rnti(0x4601)));
  du_ue_index_t ue_index1 = get_last_ue_index();
  ASSERT_TRUE(mac_dummy.last_ue_create_msg.has_value());
  ASSERT_EQ(mac_dummy.last_ue_create_msg.value().ue_index, ue_index1);
  ASSERT_EQ(to_value(mac_dummy.last_ue_create_msg.value().crnti), 0x4601);
  mac_completes_ue_creation(true);

  // Action 2: UL CCCH Message received concurrently.
  push_ul_ccch_message(create_ul_ccch_message(to_rnti(0x4602)));
  du_ue_index_t ue_index2 = get_last_ue_index();
  ASSERT_TRUE(mac_dummy.last_ue_create_msg.has_value());
  ASSERT_EQ(mac_dummy.last_ue_create_msg.value().ue_index, ue_index2);
  ASSERT_EQ(to_value(mac_dummy.last_ue_create_msg.value().crnti), 0x4602);
  mac_completes_ue_creation(true);

  // TEST: UEs should have different UE indexes.
  ASSERT_NE(ue_index1, ue_index2);
  ASSERT_EQ(ue_mng.nof_ues(), 2);
}

TEST_F(du_ue_manager_tester,
       when_requests_for_ue_creation_are_received_concurrently_then_the_created_ues_have_different_indexes)
{
  // Action 1: UL CCCH Message received.
  push_ul_ccch_message(create_ul_ccch_message(to_rnti(0x4601)));
  du_ue_index_t ue_index1 = get_last_ue_index();
  ASSERT_TRUE(mac_dummy.last_ue_create_msg.has_value());
  ASSERT_EQ(mac_dummy.last_ue_create_msg.value().ue_index, ue_index1);
  ASSERT_EQ(to_value(mac_dummy.last_ue_create_msg.value().crnti), 0x4601);

  // Action 2: UL CCCH Message received concurrently.
  push_ul_ccch_message(create_ul_ccch_message(to_rnti(0x4602)));
  du_ue_index_t ue_index2 = get_last_ue_index();
  ASSERT_TRUE(mac_dummy.last_ue_create_msg.has_value());
  ASSERT_EQ(mac_dummy.last_ue_create_msg.value().ue_index, ue_index2);
  ASSERT_EQ(to_value(mac_dummy.last_ue_create_msg.value().crnti), 0x4602);

  // TEST: UEs should have different UE indexes.
  ASSERT_NE(ue_index1, ue_index2);
}

TEST_F(du_ue_manager_tester,
       when_requests_for_ue_creation_are_received_with_duplicate_crnti_then_only_one_request_is_handled)
{
  // Action: Two UL CCCH Messages with the same TC-RNTI received.
  push_ul_ccch_message(create_ul_ccch_message(to_rnti(0x4601)));
  du_ue_index_t ue_index1 = get_last_ue_index();
  push_ul_ccch_message(create_ul_ccch_message(to_rnti(0x4601)));
  mac_completes_ue_creation(true);

  // TEST: MAC only processes the first request.
  ASSERT_TRUE(mac_dummy.last_ue_create_msg.has_value());
  ASSERT_EQ(mac_dummy.last_ue_create_msg.value().ue_index, ue_index1);
  ASSERT_EQ(to_value(mac_dummy.last_ue_create_msg.value().crnti), 0x4601);
  ASSERT_TRUE(is_ue_creation_complete());
  ASSERT_EQ(ue_mng.nof_ues(), 1);
}

TEST_F(du_ue_manager_tester, when_ue_is_being_removed_then_ue_notifiers_get_disconnected)
{
  // Action: Create UE.
  ul_ccch_indication_message ccch_ind = create_ul_ccch_message(to_rnti(0x4601));
  push_ul_ccch_message(ccch_ind);
  mac_completes_ue_creation(true);

  // Test: Buffer State updates are forwarded to MAC.
  auto* test_ue = ue_mng.find_ue(get_last_ue_index());
  auto& srb1    = test_ue->bearers.srbs()[srb_id_t::srb1].connector.rlc_tx_buffer_state_notif;
  {
    rlc_buffer_state rlc_bs = {};
    rlc_bs.pending_bytes    = 10;
    srb1.on_buffer_state_update(rlc_bs);
  }
  ASSERT_TRUE(mac_dummy.last_dl_bs.has_value());
  ASSERT_EQ(mac_dummy.last_dl_bs->ue_index, test_ue->ue_index);
  ASSERT_EQ(mac_dummy.last_dl_bs->lcid, lcid_t::LCID_SRB1);
  ASSERT_EQ(mac_dummy.last_dl_bs->bs, 10);

  // Action: Start UE removal.
  push_f1ap_ue_delete_request(get_last_ue_index());

  // TEST: UE notifiers are disconnected.
  mac_dummy.last_dl_bs.reset();
  {
    rlc_buffer_state rlc_bs = {};
    rlc_bs.pending_bytes    = 10;
    srb1.on_buffer_state_update(rlc_bs);
  }
  worker.run_pending_tasks();
  ASSERT_TRUE(not mac_dummy.last_dl_bs.has_value() or mac_dummy.last_dl_bs.value().bs == 0);
}

class du_ue_manager_rlf_tester : public du_ue_manager_tester
{
public:
  du_ue_manager_rlf_tester() : du_ue_manager_tester()
  {
    // Creates UE.
    ul_ccch_indication_message ccch_ind = create_ul_ccch_message(to_rnti(0x4601));
    push_ul_ccch_message(ccch_ind);
    mac_completes_ue_creation(true);

    test_ue_index = get_last_ue_index();
  }

  void tick_until_rlf_timeout()
  {
    const auto&    ue_timers       = params.ran.cells[0].ue_timers_and_constants;
    const unsigned release_timeout = (ue_timers.t310 + ue_timers.t311).count();
    for (unsigned i = 0; i != release_timeout; ++i) {
      timers.tick();
      worker.run_pending_tasks();
    }
  }

  void rlf_detected(rlf_cause cause)
  {
    auto& mac_rlf_notifier = *mac_dummy.last_ue_create_msg->rlf_notifier;
    auto& rlc_rlf_notifier = ue_mng.find_ue(test_ue_index)->get_rlc_rlf_notifier();

    if (cause == rlf_cause::max_mac_kos_reached) {
      mac_rlf_notifier.on_rlf_detected();
    } else if (cause == rlf_cause::max_rlc_retxs_reached) {
      rlc_rlf_notifier.on_max_retx();
    } else {
      rlc_rlf_notifier.on_protocol_failure();
    }
  }

  void crnti_ce_detected() { mac_dummy.last_ue_create_msg->rlf_notifier->on_crnti_ce_received(); }

  du_ue_index_t test_ue_index;
};

static f1ap_ue_context_release_request::cause_type rlf_cause_to_f1ap_cause(rlf_cause rlf_cause)
{
  using cause_type = f1ap_ue_context_release_request::cause_type;
  cause_type cause = rlf_cause == rlf_cause::max_mac_kos_reached ? cause_type::rlf_mac : cause_type::rlf_rlc;
  return cause;
}

TEST_F(du_ue_manager_rlf_tester,
       when_rlf_is_triggered_then_timer_starts_and_on_timeout_f1ap_is_notified_of_ue_context_removal_request)
{
  // Action: RLF is triggered.
  const rlf_cause cause = static_cast<rlf_cause>(test_rgen::uniform_int<unsigned>(0, 2));
  this->rlf_detected(cause);

  // TEST: On RLF timer timeout, F1AP is notified of UE context removal request.
  ASSERT_FALSE(f1ap_dummy.last_ue_release_req.has_value());
  tick_until_rlf_timeout();
  ASSERT_TRUE(f1ap_dummy.last_ue_release_req.has_value());
  ASSERT_EQ(f1ap_dummy.last_ue_release_req->ue_index, get_last_ue_index());
  ASSERT_EQ(f1ap_dummy.last_ue_release_req->cause, rlf_cause_to_f1ap_cause(cause));
}

TEST_F(du_ue_manager_rlf_tester, when_mac_rlf_is_triggered_and_then_crnti_ce_is_detected_then_rlf_is_aborted)
{
  // Action: MAC RLF is triggered.
  rlf_cause cause = rlf_cause::max_mac_kos_reached;
  this->rlf_detected(cause);

  timers.tick();
  worker.run_pending_tasks();

  // Action: C-RNTI CE is detected.
  this->crnti_ce_detected();

  // Action: RLC RLF is triggered.
  tick_until_rlf_timeout();

  // TEST: RLF is NOT reported.
  ASSERT_FALSE(f1ap_dummy.last_ue_release_req.has_value());
}

TEST_F(du_ue_manager_rlf_tester,
       when_rlc_rlf_is_triggered_after_mac_rlf_then_rlc_rlf_is_reported_and_crnti_ce_detection_has_no_effect)
{
  // Action: MAC RLF is triggered.
  rlf_cause cause = rlf_cause::max_mac_kos_reached;
  this->rlf_detected(cause);

  timers.tick();
  worker.run_pending_tasks();
  ASSERT_FALSE(f1ap_dummy.last_ue_release_req.has_value());

  // Action: RLC RLF is triggered.
  cause = rlf_cause::max_rlc_retxs_reached;
  this->rlf_detected(cause);

  // Action: C-RNTI CE is detected.
  this->crnti_ce_detected();

  tick_until_rlf_timeout();

  // TEST: RLC RLF is reported.
  ASSERT_TRUE(f1ap_dummy.last_ue_release_req.has_value());
  ASSERT_EQ(f1ap_dummy.last_ue_release_req->ue_index, get_last_ue_index());
  ASSERT_EQ(f1ap_dummy.last_ue_release_req->cause, rlf_cause_to_f1ap_cause(cause));
}

TEST_F(du_ue_manager_rlf_tester, when_rlf_is_triggered_then_following_rlfs_have_no_effect)
{
  // Action: RLF is triggered.
  rlf_cause cause = static_cast<rlf_cause>(test_rgen::uniform_int<unsigned>(0, 2));
  this->rlf_detected(cause);

  // TEST: First RLF is reported.
  tick_until_rlf_timeout();
  ASSERT_TRUE(f1ap_dummy.last_ue_release_req.has_value());
  f1ap_dummy.last_ue_release_req.reset();

  // Action: RLF is triggered again.
  cause = static_cast<rlf_cause>(test_rgen::uniform_int<unsigned>(0, 2));
  this->rlf_detected(cause);

  // TEST: Second RLF is not reported.
  tick_until_rlf_timeout();
  ASSERT_FALSE(f1ap_dummy.last_ue_release_req.has_value());
}

TEST_F(du_ue_manager_rlf_tester, when_ue_is_being_deleted_then_rlf_should_have_no_effect)
{
  // Action: Initiate UE removal.
  push_f1ap_ue_delete_request(get_last_ue_index());
  worker.run_pending_tasks();

  // Test: UE removal is under way.
  ASSERT_TRUE(mac_dummy.last_ue_delete_msg.has_value());
  ASSERT_EQ(get_last_ue_index(), mac_dummy.last_ue_delete_msg->ue_index);

  // Action: RLF is triggered.
  rlf_cause cause = static_cast<rlf_cause>(test_rgen::uniform_int<unsigned>(0, 2));
  this->rlf_detected(cause);

  // Test: No RLF is reported.
  tick_until_rlf_timeout();
  ASSERT_FALSE(f1ap_dummy.last_ue_release_req.has_value());
}

TEST_F(du_ue_manager_rlf_tester, when_rlf_is_triggered_but_ue_removal_starts_then_rlf_should_have_no_effect)
{
  // Action: RLF is triggered.
  rlf_cause cause = static_cast<rlf_cause>(test_rgen::uniform_int<unsigned>(0, 2));
  this->rlf_detected(cause);

  // Action: Initiate UE removal.
  push_f1ap_ue_delete_request(get_last_ue_index());
  worker.run_pending_tasks();

  // Test: UE removal is under way.
  ASSERT_TRUE(mac_dummy.last_ue_delete_msg.has_value());
  ASSERT_EQ(get_last_ue_index(), mac_dummy.last_ue_delete_msg->ue_index);

  // Test: No RLF is reported.
  tick_until_rlf_timeout();
  ASSERT_FALSE(f1ap_dummy.last_ue_release_req.has_value());
}
