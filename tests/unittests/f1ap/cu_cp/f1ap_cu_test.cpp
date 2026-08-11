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
#include "lib/f1ap/asn1_helpers.h"
#include "tests/test_doubles/f1ap/f1ap_test_messages.h"
#include "srsran/asn1/f1ap/common.h"
#include "srsran/asn1/f1ap/f1ap_pdu_contents.h"
#include "srsran/f1ap/cu_cp/f1ap_cu.h"
#include "srsran/f1ap/ntn_initial_ul_position_query.h"
#include "srsran/support/async/async_test_utils.h"
#include <algorithm>
#include <gtest/gtest.h>

using namespace srsran;
using namespace srs_cu_cp;
using namespace asn1::f1ap;

static du_setup_result create_du_setup_result_accept(const f1ap_message& f1_msg)
{
  du_setup_result resp;
  auto&           accepted    = resp.result.emplace<du_setup_result::accepted>();
  accepted.gnb_cu_name        = "dummy_gnb_cu_name";
  accepted.gnb_cu_rrc_version = 2;

  auto& cells = f1_msg.pdu.init_msg().value.f1_setup_request()->gnb_du_served_cells_list;
  accepted.cells_to_be_activ_list.resize(cells.size());
  for (unsigned i = 0; i != cells.size(); ++i) {
    auto& cell  = accepted.cells_to_be_activ_list[i];
    cell.nr_cgi = cgi_from_asn1(cells[i]->gnb_du_served_cells_item().served_cell_info.nr_cgi).value();
    cell.nr_pci = cells[i]->gnb_du_served_cells_item().served_cell_info.nr_pci;
  }
  return resp;
}

static du_setup_result create_du_setup_result_reject(const f1ap_message& f1_msg)
{
  du_setup_result resp;
  auto&           rejected = resp.result.emplace<du_setup_result::rejected>();
  rejected.cause           = cause_misc_t::unspecified;
  rejected.cause_str       = "dummy reason";
  return resp;
}

static f1ap_initial_ul_position_query_plan make_initial_ul_position_query_plan()
{
  f1ap_initial_ul_position_query_plan plan;
  plan.query.query_generation         = 7;
  plan.query.nonce                    = 0x1020304050607080ULL;
  plan.query.connection_token         = 0x8877665544332211ULL;
  plan.query.gnb_du_id                = int_to_gnb_du_id(0x123);
  plan.query.cell_cgi                 =
      nr_cell_global_id_t{plmn_identity::test_value(), nr_cell_identity::create(0x12345).value()};
  plan.query.cell_index               = to_du_cell_index(1);
  plan.query.pci                      = pci_t{17};
  plan.query.gnb_du_ue_f1ap_id        = int_to_gnb_du_ue_f1ap_id(51);
  plan.query.c_rnti                   = to_rnti(0x4701);
  plan.query.expected_rnti_generation = 105;
  return plan;
}

static f1ap_ntn_initial_ul_position_result
make_initial_ul_position_result(const f1ap_ntn_initial_ul_position_query& query)
{
  f1ap_ntn_initial_ul_position_result result;
  result.query_generation         = query.query_generation;
  result.nonce                    = query.nonce;
  result.connection_token         = query.connection_token;
  result.gnb_du_id                = query.gnb_du_id;
  result.cell_cgi                 = query.cell_cgi;
  result.cell_index               = query.cell_index;
  result.pci                      = query.pci;
  result.gnb_du_ue_f1ap_id        = query.gnb_du_ue_f1ap_id;
  result.c_rnti                   = query.c_rnti;
  result.expected_rnti_generation = query.expected_rnti_generation;
  result.observation_id           = 0xabcdef0123456789ULL;
  result.accepted                 = true;
  result.authority                = f1ap_ntn_initial_ul_position_authority::ofh_beam_id_verified;
  result.reason                   = "accepted";
  result.schedule_version         = 41;
  result.calendar_hash            = "calendar-sha256";
  result.mapping_version          = 9;
  result.mapping_hash             = "mapping-sha256";
  result.position_id              = "G000123";
  result.logical_port             = 3;
  result.physical_port            = 7;
  result.eaxc                     = 5;
  result.beam_id                  = 0x1234;
  result.calendar_cycle_index     = 2;
  result.occasion_offset_us       = 5000;
  result.confidence_margin_db     = 8.5F;
  return result;
}

static f1ap_message make_initial_ul_position_response(
    const f1ap_message& request, const f1ap_ntn_initial_ul_position_result& result)
{
  const auto& asn1_request = request.pdu.init_msg().value.gnb_du_res_coordination_request();
  f1ap_message response;
  response.pdu.set_successful_outcome().load_info_obj(ASN1_F1AP_ID_GNB_DU_RES_COORDINATION);
  auto& asn1_response           = response.pdu.successful_outcome().value.gnb_du_res_coordination_resp();
  asn1_response->transaction_id = asn1_request->transaction_id;
  asn1_response->eutra_nr_cell_res_coordination_req_ack_container =
      encode_f1ap_ntn_initial_ul_position_result(result);
  return response;
}

//////////////////////////////////////////////////////////////////////////////////////
/* Handling of unsupported messages                                                 */
//////////////////////////////////////////////////////////////////////////////////////

TEST_F(f1ap_cu_test, when_unsupported_f1ap_pdu_received_then_message_ignored)
{
  // Set last message of PDU notifier to init_msg
  f1ap_pdu_notifier.last_f1ap_msg.pdu.set_init_msg();

  // Generate unsupported F1AP PDU
  f1ap_message unsupported_msg = {};
  unsupported_msg.pdu.set_choice_ext();

  f1ap->handle_message(unsupported_msg);

  // Check that PDU has not been forwarded (last PDU is still init_msg)
  EXPECT_EQ(f1ap_pdu_notifier.last_f1ap_msg.pdu.type(), asn1::f1ap::f1ap_pdu_c::types_opts::options::init_msg);
}

TEST_F(f1ap_cu_test, when_unsupported_init_msg_received_then_message_ignored)
{
  // Set last message of PDU notifier to successful outcome
  f1ap_pdu_notifier.last_f1ap_msg.pdu.set_successful_outcome();

  // Generate unupported F1AP PDU
  f1ap_message unsupported_msg = {};
  unsupported_msg.pdu.set_init_msg();

  f1ap->handle_message(unsupported_msg);

  // Check that PDU has not been forwarded (last PDU is still successful_outcome)
  EXPECT_EQ(f1ap_pdu_notifier.last_f1ap_msg.pdu.type(),
            asn1::f1ap::f1ap_pdu_c::types_opts::options::successful_outcome);
}

TEST_F(f1ap_cu_test, when_unsupported_successful_outcome_received_then_message_ignored)
{
  // Set last message of PDU notifier to init_msg
  f1ap_pdu_notifier.last_f1ap_msg.pdu.set_init_msg();

  // Generate unupported F1AP PDU
  f1ap_message unsupported_msg = {};
  unsupported_msg.pdu.set_successful_outcome();

  f1ap->handle_message(unsupported_msg);

  // Check that PDU has not been forwarded (last PDU is still init_msg)
  EXPECT_EQ(f1ap_pdu_notifier.last_f1ap_msg.pdu.type(), asn1::f1ap::f1ap_pdu_c::types_opts::options::init_msg);
}

TEST_F(f1ap_cu_test, when_unsupported_unsuccessful_outcome_received_then_message_ignored)
{
  // Set last message of PDU notifier to init_msg
  f1ap_pdu_notifier.last_f1ap_msg.pdu.set_init_msg();

  // Generate unupported F1AP PDU
  f1ap_message unsupported_msg = {};
  unsupported_msg.pdu.set_unsuccessful_outcome();

  f1ap->handle_message(unsupported_msg);

  // Check that PDU has not been forwarded (last PDU is still init_msg)
  EXPECT_EQ(f1ap_pdu_notifier.last_f1ap_msg.pdu.type(), asn1::f1ap::f1ap_pdu_c::types_opts::options::init_msg);
}

//////////////////////////////////////////////////////////////////////////////////////
/* F1 Setup handling                                                                */
//////////////////////////////////////////////////////////////////////////////////////

/// Test the successful f1 setup procedure
TEST_F(f1ap_cu_test, when_f1_setup_request_valid_then_connect_du)
{
  // Create F1SetupRequest.
  f1ap_message f1setup_msg = test_helpers::generate_f1_setup_request();

  // Prepare the DU processor response.
  du_processor_notifier.next_du_setup_resp = create_du_setup_result_accept(f1setup_msg);

  test_logger.info("TEST: Receive F1SetupRequest message...");
  f1ap->handle_message(f1setup_msg);

  // Check if F1SetupRequest was forwarded to DU processor
  ASSERT_EQ(du_processor_notifier.last_f1_setup_request_msg.gnb_du_id, int_to_gnb_du_id(0x11U));

  // Check the F1 Tx PDU is indeed the F1 Setup response
  ASSERT_EQ(asn1::f1ap::f1ap_pdu_c::types_opts::options::successful_outcome,
            f1ap_pdu_notifier.last_f1ap_msg.pdu.type());
  ASSERT_EQ(asn1::f1ap::f1ap_elem_procs_o::successful_outcome_c::types_opts::options::f1_setup_resp,
            f1ap_pdu_notifier.last_f1ap_msg.pdu.successful_outcome().value.type());
}

/// Test the f1 setup failure
TEST_F(f1ap_cu_test, when_f1_setup_request_invalid_then_reject_du)
{
  // Generate Invalid F1SetupRequest
  f1ap_message f1setup_msg                    = test_helpers::generate_f1_setup_request();
  auto&        setup_req                      = f1setup_msg.pdu.init_msg().value.f1_setup_request();
  setup_req->gnb_du_served_cells_list_present = false;
  setup_req->gnb_du_served_cells_list.clear();

  // Prepare the DU processor response.
  du_processor_notifier.next_du_setup_resp = create_du_setup_result_reject(f1setup_msg);

  f1ap->handle_message(f1setup_msg);

  // Check the generated PDU is indeed the F1 Setup failure
  ASSERT_EQ(asn1::f1ap::f1ap_pdu_c::types_opts::options::unsuccessful_outcome,
            f1ap_pdu_notifier.last_f1ap_msg.pdu.type());
  ASSERT_EQ(asn1::f1ap::f1ap_elem_procs_o::unsuccessful_outcome_c::types_opts::f1_setup_fail,
            f1ap_pdu_notifier.last_f1ap_msg.pdu.unsuccessful_outcome().value.type());
}

//////////////////////////////////////////////////////////////////////////////////////
/* Initial UL RRC Message handling                                                  */
//////////////////////////////////////////////////////////////////////////////////////

TEST_F(f1ap_cu_test, when_init_ul_rrc_correct_then_ue_added)
{
  // Generate F1 Initial UL RRC Message
  f1ap_message init_ul_rrc_msg = test_helpers::generate_init_ul_rrc_message_transfer(int_to_gnb_du_ue_f1ap_id(41255));

  // Pass message to F1AP
  f1ap->handle_message(init_ul_rrc_msg);

  EXPECT_EQ(f1ap->get_nof_ues(), 1);
  EXPECT_EQ(du_processor_notifier.initial_ul_position_query_requests, 1U);
  EXPECT_EQ(du_processor_notifier.initial_ul_position_query_completions, 0U);
  ASSERT_EQ(du_processor_notifier.initial_ul_event_order.size(), 2U);
  EXPECT_EQ(du_processor_notifier.initial_ul_event_order[0], "position_query_required");
  EXPECT_EQ(du_processor_notifier.initial_ul_event_order[1], "rrc_context_creation");
  const bool private_query_was_sent =
      f1ap_pdu_notifier.last_f1ap_msg.pdu.type().value == f1ap_pdu_c::types_opts::init_msg &&
      f1ap_pdu_notifier.last_f1ap_msg.pdu.init_msg().value.type().value ==
          f1ap_elem_procs_o::init_msg_c::types_opts::gnb_du_res_coordination_request;
  EXPECT_FALSE(private_query_was_sent);
}

TEST_F(f1ap_cu_test, when_initial_ul_position_query_is_enabled_then_rrc_creation_waits_for_exact_response)
{
  f1ap_initial_ul_position_query_plan plan = make_initial_ul_position_query_plan();
  du_processor_notifier.next_initial_ul_position_query_plan.emplace(plan);
  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(41255);
  f1ap_message init_ul_rrc_msg = test_helpers::generate_init_ul_rrc_message_transfer(du_ue_id);

  f1ap->handle_message(init_ul_rrc_msg);

  EXPECT_EQ(f1ap->get_nof_ues(), 0U);
  ASSERT_EQ(du_processor_notifier.initial_ul_event_order.size(), 1U);
  EXPECT_EQ(du_processor_notifier.initial_ul_event_order.front(), "position_query_required");
  ASSERT_EQ(f1ap_pdu_notifier.last_f1ap_msg.pdu.type().value, f1ap_pdu_c::types_opts::init_msg);
  ASSERT_EQ(f1ap_pdu_notifier.last_f1ap_msg.pdu.init_msg().value.type().value,
            f1ap_elem_procs_o::init_msg_c::types_opts::gnb_du_res_coordination_request);

  f1ap_message response = make_initial_ul_position_response(
      f1ap_pdu_notifier.last_f1ap_msg, make_initial_ul_position_result(plan.query));
  f1ap->handle_message(response);

  EXPECT_EQ(f1ap->get_nof_ues(), 1U);
  EXPECT_EQ(du_processor_notifier.initial_ul_position_query_completions, 1U);
  EXPECT_TRUE(du_processor_notifier.last_initial_ul_position_query_had_result);
  EXPECT_TRUE(du_processor_notifier.last_initial_ul_position_query_failure.empty());
  ASSERT_EQ(du_processor_notifier.initial_ul_event_order.size(), 3U);
  EXPECT_EQ(du_processor_notifier.initial_ul_event_order[1], "position_query_complete");
  EXPECT_EQ(du_processor_notifier.initial_ul_event_order[2], "rrc_context_creation");
}

TEST_F(f1ap_cu_test, when_initial_ul_position_query_times_out_then_rrc_creation_resumes_after_timeout)
{
  f1ap_initial_ul_position_query_plan plan = make_initial_ul_position_query_plan();
  plan.response_timeout = std::chrono::milliseconds{3};
  du_processor_notifier.next_initial_ul_position_query_plan.emplace(plan);
  f1ap_message init_ul_rrc_msg =
      test_helpers::generate_init_ul_rrc_message_transfer(int_to_gnb_du_ue_f1ap_id(41255));

  f1ap->handle_message(init_ul_rrc_msg);
  for (unsigned elapsed_ms = 0; elapsed_ms != 3; ++elapsed_ms) {
    EXPECT_EQ(f1ap->get_nof_ues(), 0U);
    tick();
  }

  EXPECT_EQ(f1ap->get_nof_ues(), 1U);
  EXPECT_EQ(du_processor_notifier.initial_ul_position_query_completions, 1U);
  EXPECT_FALSE(du_processor_notifier.last_initial_ul_position_query_had_result);
  EXPECT_EQ(du_processor_notifier.last_initial_ul_position_query_failure, "response_timeout");
  ASSERT_EQ(du_processor_notifier.initial_ul_event_order.size(), 3U);
  EXPECT_EQ(du_processor_notifier.initial_ul_event_order[1], "position_query_complete");
  EXPECT_EQ(du_processor_notifier.initial_ul_event_order[2], "rrc_context_creation");
}

TEST_F(f1ap_cu_test, duplicate_initial_ul_while_position_query_is_in_flight_starts_one_query_and_creates_one_ue)
{
  f1ap_initial_ul_position_query_plan plan = make_initial_ul_position_query_plan();
  du_processor_notifier.next_initial_ul_position_query_plan.emplace(plan);
  f1ap_message init_ul_rrc_msg =
      test_helpers::generate_init_ul_rrc_message_transfer(int_to_gnb_du_ue_f1ap_id(41255));

  f1ap->handle_message(init_ul_rrc_msg);
  const f1ap_message query_request = f1ap_pdu_notifier.last_f1ap_msg;
  f1ap->handle_message(init_ul_rrc_msg);

  EXPECT_EQ(du_processor_notifier.initial_ul_position_query_requests, 1U);
  EXPECT_EQ(du_processor_notifier.initial_ul_position_query_completions, 0U);
  EXPECT_EQ(f1ap->get_nof_ues(), 0U);

  f1ap_message response =
      make_initial_ul_position_response(query_request, make_initial_ul_position_result(plan.query));
  f1ap->handle_message(response);

  EXPECT_EQ(du_processor_notifier.initial_ul_position_query_requests, 1U);
  EXPECT_EQ(du_processor_notifier.initial_ul_position_query_completions, 1U);
  EXPECT_EQ(f1ap->get_nof_ues(), 1U);
  EXPECT_EQ(std::count(du_processor_notifier.initial_ul_event_order.begin(),
                       du_processor_notifier.initial_ul_event_order.end(),
                       "rrc_context_creation"),
            1);

  // A retransmission after the F1 UE exists is also idempotent.
  f1ap->handle_message(init_ul_rrc_msg);
  EXPECT_EQ(f1ap->get_nof_ues(), 1U);
  EXPECT_EQ(std::count(du_processor_notifier.initial_ul_event_order.begin(),
                       du_processor_notifier.initial_ul_event_order.end(),
                       "rrc_context_creation"),
            1);
}

TEST_F(f1ap_cu_test, same_du_ue_with_changed_rnti_cannot_start_a_second_initial_ul_position_query)
{
  f1ap_initial_ul_position_query_plan plan = make_initial_ul_position_query_plan();
  du_processor_notifier.next_initial_ul_position_query_plan.emplace(plan);
  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(41255);
  f1ap_message first = test_helpers::generate_init_ul_rrc_message_transfer(du_ue_id, to_rnti(0x4601));
  f1ap_message changed_rnti = test_helpers::generate_init_ul_rrc_message_transfer(du_ue_id, to_rnti(0x4602));

  f1ap->handle_message(first);
  const f1ap_message query_request = f1ap_pdu_notifier.last_f1ap_msg;
  f1ap->handle_message(changed_rnti);

  EXPECT_EQ(du_processor_notifier.initial_ul_position_query_requests, 1U);
  EXPECT_EQ(du_processor_notifier.initial_ul_position_query_completions, 0U);
  EXPECT_EQ(f1ap->get_nof_ues(), 0U);

  f1ap_message response =
      make_initial_ul_position_response(query_request, make_initial_ul_position_result(plan.query));
  f1ap->handle_message(response);

  EXPECT_EQ(du_processor_notifier.initial_ul_position_query_completions, 1U);
  EXPECT_EQ(f1ap->get_nof_ues(), 1U);
  EXPECT_EQ(std::count(du_processor_notifier.initial_ul_event_order.begin(),
                       du_processor_notifier.initial_ul_event_order.end(),
                       "rrc_context_creation"),
            1);
}

TEST_F(f1ap_cu_test, late_initial_ul_position_response_after_timeout_does_not_create_a_second_ue)
{
  f1ap_initial_ul_position_query_plan plan = make_initial_ul_position_query_plan();
  plan.response_timeout = std::chrono::milliseconds{1};
  du_processor_notifier.next_initial_ul_position_query_plan.emplace(plan);
  f1ap_message init_ul_rrc_msg =
      test_helpers::generate_init_ul_rrc_message_transfer(int_to_gnb_du_ue_f1ap_id(41255));

  f1ap->handle_message(init_ul_rrc_msg);
  const f1ap_message query_request = f1ap_pdu_notifier.last_f1ap_msg;
  tick();

  ASSERT_EQ(f1ap->get_nof_ues(), 1U);
  ASSERT_EQ(du_processor_notifier.initial_ul_position_query_completions, 1U);
  EXPECT_EQ(du_processor_notifier.last_initial_ul_position_query_failure, "response_timeout");

  f1ap_message late_response =
      make_initial_ul_position_response(query_request, make_initial_ul_position_result(plan.query));
  f1ap->handle_message(late_response);

  EXPECT_EQ(f1ap->get_nof_ues(), 1U);
  EXPECT_EQ(du_processor_notifier.initial_ul_position_query_completions, 1U);
  EXPECT_EQ(std::count(du_processor_notifier.initial_ul_event_order.begin(),
                       du_processor_notifier.initial_ul_event_order.end(),
                       "rrc_context_creation"),
            1);
}

TEST_F(f1ap_cu_test, connection_loss_cancels_inflight_initial_ul_query_before_rrc_creation)
{
  f1ap_initial_ul_position_query_plan plan = make_initial_ul_position_query_plan();
  plan.response_timeout = std::chrono::milliseconds{200};
  du_processor_notifier.next_initial_ul_position_query_plan.emplace(plan);
  f1ap_message init_ul_rrc_msg =
      test_helpers::generate_init_ul_rrc_message_transfer(int_to_gnb_du_ue_f1ap_id(41255));

  f1ap->handle_message(init_ul_rrc_msg);
  const f1ap_message query_request = f1ap_pdu_notifier.last_f1ap_msg;
  ASSERT_EQ(f1ap->get_nof_ues(), 0U);

  f1ap->handle_connection_loss();
  EXPECT_EQ(du_processor_notifier.initial_ul_position_query_completions, 0U);
  EXPECT_EQ(f1ap->get_nof_ues(), 0U);
  EXPECT_EQ(std::count(du_processor_notifier.initial_ul_event_order.begin(),
                       du_processor_notifier.initial_ul_event_order.end(),
                       "rrc_context_creation"),
            0);

  // stop() remains idempotent after the synchronous transport-loss notification.
  async_task<void>         stop_task = f1ap->stop();
  lazy_task_launcher<void> stop_launcher(stop_task);
  ASSERT_TRUE(stop_task.ready());
  EXPECT_EQ(du_processor_notifier.initial_ul_position_query_completions, 0U);
  EXPECT_EQ(f1ap->get_nof_ues(), 0U);

  f1ap_message stale_response =
      make_initial_ul_position_response(query_request, make_initial_ul_position_result(plan.query));
  f1ap->handle_message(stale_response);
  f1ap->handle_message(init_ul_rrc_msg);

  EXPECT_EQ(du_processor_notifier.initial_ul_position_query_completions, 0U);
  EXPECT_EQ(f1ap->get_nof_ues(), 0U);
  EXPECT_EQ(std::count(du_processor_notifier.initial_ul_event_order.begin(),
                       du_processor_notifier.initial_ul_event_order.end(),
                       "rrc_context_creation"),
            0);
}

TEST_F(f1ap_cu_test, when_initial_ul_position_response_uses_old_connection_token_then_result_is_rejected_before_rrc)
{
  f1ap_initial_ul_position_query_plan plan = make_initial_ul_position_query_plan();
  du_processor_notifier.next_initial_ul_position_query_plan.emplace(plan);
  f1ap_message init_ul_rrc_msg =
      test_helpers::generate_init_ul_rrc_message_transfer(int_to_gnb_du_ue_f1ap_id(41255));
  f1ap->handle_message(init_ul_rrc_msg);

  f1ap_ntn_initial_ul_position_result stale_result = make_initial_ul_position_result(plan.query);
  ++stale_result.connection_token;
  f1ap_message response = make_initial_ul_position_response(f1ap_pdu_notifier.last_f1ap_msg, stale_result);
  f1ap->handle_message(response);

  EXPECT_EQ(f1ap->get_nof_ues(), 1U);
  EXPECT_EQ(du_processor_notifier.initial_ul_position_query_completions, 1U);
  EXPECT_FALSE(du_processor_notifier.last_initial_ul_position_query_had_result);
  EXPECT_EQ(du_processor_notifier.last_initial_ul_position_query_failure, "response_identity_mismatch");
  ASSERT_EQ(du_processor_notifier.initial_ul_event_order.size(), 3U);
  EXPECT_EQ(du_processor_notifier.initial_ul_event_order[1], "position_query_complete");
  EXPECT_EQ(du_processor_notifier.initial_ul_event_order[2], "rrc_context_creation");
}

TEST_F(f1ap_cu_test, exact_f1_ue_identity_can_be_looked_up_in_both_directions)
{
  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(41255);
  const test_ue&             ue       = create_ue(du_ue_id);

  const std::optional<f1ap_ue_identity> identity = f1ap->get_ue_identity(ue.ue_index);
  ASSERT_TRUE(identity.has_value());
  EXPECT_EQ(identity->ue_index, ue.ue_index);
  EXPECT_EQ(identity->du_ue_f1ap_id, du_ue_id);

  const std::optional<ue_index_t> resolved_ue =
      f1ap->resolve_ue_identity(identity->cu_ue_f1ap_id, identity->du_ue_f1ap_id);
  ASSERT_TRUE(resolved_ue.has_value());
  EXPECT_EQ(*resolved_ue, ue.ue_index);
}

TEST_F(f1ap_cu_test, f1_ue_identity_lookup_rejects_invalid_and_unpaired_ids)
{
  const ue_index_t first_ue_index = create_ue(int_to_gnb_du_ue_f1ap_id(41255)).ue_index;
  const ue_index_t second_ue_index = create_ue(int_to_gnb_du_ue_f1ap_id(41256)).ue_index;

  const std::optional<f1ap_ue_identity> first_identity  = f1ap->get_ue_identity(first_ue_index);
  const std::optional<f1ap_ue_identity> second_identity = f1ap->get_ue_identity(second_ue_index);
  ASSERT_TRUE(first_identity.has_value());
  ASSERT_TRUE(second_identity.has_value());

  EXPECT_FALSE(f1ap->resolve_ue_identity(gnb_cu_ue_f1ap_id_t::invalid, first_identity->du_ue_f1ap_id).has_value());
  EXPECT_FALSE(f1ap->resolve_ue_identity(first_identity->cu_ue_f1ap_id, gnb_du_ue_f1ap_id_t::invalid).has_value());
  EXPECT_FALSE(
      f1ap->resolve_ue_identity(first_identity->cu_ue_f1ap_id, second_identity->du_ue_f1ap_id).has_value());
  EXPECT_FALSE(f1ap->get_ue_identity(ue_index_t::invalid).has_value());
}

TEST_F(f1ap_cu_test, when_cgi_invalid_then_ue_not_added)
{
  // Generate F1 Initial UL RRC Message
  f1ap_message init_ul_rrc_msg = test_helpers::generate_init_ul_rrc_message_transfer(int_to_gnb_du_ue_f1ap_id(41255));
  // Set PLMN to invalid value
  init_ul_rrc_msg.pdu.init_msg().value.init_ul_rrc_msg_transfer()->nr_cgi.plmn_id.from_number(0xa);

  // Pass message to F1AP
  f1ap->handle_message(init_ul_rrc_msg);

  EXPECT_EQ(f1ap->get_nof_ues(), 0);
}

TEST_F(f1ap_cu_test, when_rnti_invalid_then_ue_not_added)
{
  // Generate F1 Initial UL RRC Message
  f1ap_message init_ul_rrc_msg = test_helpers::generate_init_ul_rrc_message_transfer(int_to_gnb_du_ue_f1ap_id(41255));
  // Set RNTI to invalid value
  init_ul_rrc_msg.pdu.init_msg().value.init_ul_rrc_msg_transfer()->c_rnti = 0;

  // Pass message to F1AP
  f1ap->handle_message(init_ul_rrc_msg);

  EXPECT_EQ(f1ap->get_nof_ues(), 0);
}

TEST_F(f1ap_cu_test, when_max_nof_ues_exceeded_then_ue_not_added)
{
  // Reduce F1AP and TEST logger loglevel to warning to reduce console output
  srslog::fetch_basic_logger("CU-CP-F1").set_level(srslog::basic_levels::warning);
  srslog::fetch_basic_logger("TEST").set_level(srslog::basic_levels::warning);

  // Add the maximum number of UEs
  for (unsigned du_ue_id = 0; du_ue_id < max_nof_ues; du_ue_id++) {
    // Generate ue_creation message
    f1ap_message init_ul_rrc_msg =
        test_helpers::generate_init_ul_rrc_message_transfer(int_to_gnb_du_ue_f1ap_id(du_ue_id));

    // Pass message to F1AP
    f1ap->handle_message(init_ul_rrc_msg);
  }

  // Reset F1AP and TEST logger loglevel
  srslog::fetch_basic_logger("CU-CP-F1").set_level(srslog::basic_levels::debug);
  srslog::fetch_basic_logger("TEST").set_level(srslog::basic_levels::debug);

  EXPECT_EQ(f1ap->get_nof_ues(), max_nof_ues);

  // Add one more UE to F1AP
  // Generate ue_creation message
  f1ap_message init_ul_rrc_msg =
      test_helpers::generate_init_ul_rrc_message_transfer(int_to_gnb_du_ue_f1ap_id(max_nof_ues + 1));

  // Pass message to F1AP
  f1ap->handle_message(init_ul_rrc_msg);

  EXPECT_EQ(f1ap->get_nof_ues(), max_nof_ues);
}

TEST_F(f1ap_cu_test, when_ue_creation_fails_then_ue_not_added)
{
  // Add maximum number of UEs to dummy DU processor
  du_processor_notifier.set_ue_id(max_nof_ues);

  // Add one more UE to F1AP
  // Generate F1 Initial UL RRC Message
  f1ap_message init_ul_rrc_msg = test_helpers::generate_init_ul_rrc_message_transfer(int_to_gnb_du_ue_f1ap_id(41255));

  // Pass message to F1AP
  f1ap->handle_message(init_ul_rrc_msg);

  EXPECT_TRUE(was_rrc_reject_sent());
  EXPECT_EQ(f1ap->get_nof_ues(), 0);
}

//////////////////////////////////////////////////////////////////////////////////////
/* F1 Removal Request handling                                                      */
//////////////////////////////////////////////////////////////////////////////////////

TEST_F(f1ap_cu_test, when_f1_removal_request_received_then_f1_removal_response_is_sent)
{
  // Generate F1 Removal Request Message
  f1ap_message removal_request = {};
  removal_request.pdu.set_init_msg();
  removal_request.pdu.init_msg().load_info_obj(ASN1_F1AP_ID_F1_REMOVAL);
  removal_request.pdu.init_msg().value.f1_removal_request()->resize(1);
  (*removal_request.pdu.init_msg().value.f1_removal_request())[0]->transaction_id() = 0;

  // Pass message to F1AP
  f1ap->handle_message(removal_request);

  ASSERT_EQ(f1ap_pdu_notifier.last_f1ap_msg.pdu.type().value, asn1::f1ap::f1ap_pdu_c::types::successful_outcome);
  ASSERT_EQ(f1ap_pdu_notifier.last_f1ap_msg.pdu.successful_outcome().value.type().value,
            asn1::f1ap::f1ap_elem_procs_o::successful_outcome_c::types::f1_removal_resp);
}
