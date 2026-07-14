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

#include "ngap_test_helpers.h"
#include "srsran/asn1/ngap/ngap_pdu_contents.h"
#include "srsran/support/async/async_test_utils.h"
#include "srsran/support/test_utils.h"
#include <gtest/gtest.h>

using namespace srsran;
using namespace srs_cu_cp;

class ngap_ue_context_management_procedure_test : public ngap_test
{
protected:
  ue_index_t start_procedure()
  {
    ue_index_t ue_index = create_ue();

    // Inject DL NAS transport message from AMF
    run_dl_nas_transport(ue_index);

    // Inject UL NAS transport message from RRC
    run_ul_nas_transport(ue_index);

    return ue_index;
  }

  bool was_initial_context_setup_response_sent() const
  {
    return n2_gw.last_ngap_msgs.back().pdu.successful_outcome().value.type() ==
           asn1::ngap::ngap_elem_procs_o::successful_outcome_c::types_opts::init_context_setup_resp;
  }

  bool was_initial_context_setup_failure_sent() const
  {
    return n2_gw.last_ngap_msgs.back().pdu.unsuccessful_outcome().value.type() ==
           asn1::ngap::ngap_elem_procs_o::unsuccessful_outcome_c::types_opts::init_context_setup_fail;
  }

  bool was_pdu_session_resource_setup_successful() const
  {
    bool setup_present = n2_gw.last_ngap_msgs.back()
                             .pdu.successful_outcome()
                             .value.init_context_setup_resp()
                             ->pdu_session_res_setup_list_cxt_res_present == true;

    bool fail_present = n2_gw.last_ngap_msgs.back()
                            .pdu.successful_outcome()
                            .value.init_context_setup_resp()
                            ->pdu_session_res_failed_to_setup_list_cxt_res_present == false;

    return setup_present && fail_present;
  }

  bool was_pdu_session_resource_setup_unsuccessful() const
  {
    return n2_gw.last_ngap_msgs.back()
               .pdu.unsuccessful_outcome()
               .value.init_context_setup_fail()
               ->pdu_session_res_failed_to_setup_list_cxt_fail_present == true;
  }

  bool was_ue_context_release_request_sent() const
  {
    if (n2_gw.last_ngap_msgs.back().pdu.type() == asn1::ngap::ngap_pdu_c::types_opts::nulltype) {
      return false;
    }
    return n2_gw.last_ngap_msgs.back().pdu.init_msg().value.type() ==
           asn1::ngap::ngap_elem_procs_o::init_msg_c::types_opts::ue_context_release_request;
  }

  bool was_ue_context_release_complete_sent() const
  {
    return n2_gw.last_ngap_msgs.back().pdu.successful_outcome().value.type() ==
           asn1::ngap::ngap_elem_procs_o::successful_outcome_c::types_opts::ue_context_release_complete;
  }

  bool was_ue_added() const { return ngap->get_nof_ues() == 1; }

  bool was_ue_removed() const { return ngap->get_nof_ues() == 0; }

  void clear_last_received_msg() { n2_gw.last_ngap_msgs.back() = {}; }

  bool was_ue_release_requested(const test_ue& ue) const { return cu_cp_notifier.last_command.ue_index == ue.ue_index; }

  bool was_error_indication_sent() const
  {
    return n2_gw.last_ngap_msgs.back().pdu.init_msg().value.type() ==
           asn1::ngap::ngap_elem_procs_o::init_msg_c::types_opts::error_ind;
  }

  bool was_rrc_inactive_transition_report_sent() const
  {
    return n2_gw.last_ngap_msgs.back().pdu.init_msg().value.type() ==
           asn1::ngap::ngap_elem_procs_o::init_msg_c::types_opts::rrc_inactive_transition_report;
  }

  bool was_location_report_sent() const
  {
    return n2_gw.last_ngap_msgs.back().pdu.init_msg().value.type() ==
           asn1::ngap::ngap_elem_procs_o::init_msg_c::types_opts::location_report;
  }

  bool was_location_reporting_failure_sent() const
  {
    return n2_gw.last_ngap_msgs.back().pdu.init_msg().value.type() ==
           asn1::ngap::ngap_elem_procs_o::init_msg_c::types_opts::location_report_fail_ind;
  }
};

static ngap_message generate_location_reporting_control_message(amf_ue_id_t amf_ue_id, ran_ue_id_t ran_ue_id)
{
  ngap_message msg = {};
  msg.pdu.set_init_msg();
  msg.pdu.init_msg().load_info_obj(ASN1_NGAP_ID_LOCATION_REPORT_CTRL);

  auto& ctrl           = msg.pdu.init_msg().value.location_report_ctrl();
  ctrl->amf_ue_ngap_id = amf_ue_id_to_uint(amf_ue_id);
  ctrl->ran_ue_ngap_id = ran_ue_id_to_uint(ran_ue_id);
  ctrl->location_report_request_type.event_type.value = asn1::ngap::event_type_opts::direct;
  ctrl->location_report_request_type.report_area.value = asn1::ngap::report_area_opts::cell;
  return msg;
}

/// Test Initial Context Setup Request
TEST_F(ngap_ue_context_management_procedure_test, when_valid_initial_context_setup_request_received_then_response_send)
{
  // Test preamble
  ue_index_t ue_index = this->start_procedure();

  auto& ue = test_ues.at(ue_index);

  // Inject Initial Context Setup Request
  ngap_message init_context_setup_request =
      generate_valid_initial_context_setup_request_message(ue.amf_ue_id.value(), ue.ran_ue_id.value());
  ngap->handle_message(init_context_setup_request);

  // Check that AMF notifier was called with right type
  ASSERT_TRUE(was_initial_context_setup_response_sent());

  ASSERT_TRUE(was_ue_added());
}

/// Test Initial Context Setup Request with PDUSessionResourceSetupListCxtReq
TEST_F(ngap_ue_context_management_procedure_test,
       when_initial_context_setup_request_with_pdu_session_received_then_response_send)
{
  // Test preamble
  ue_index_t ue_index = this->start_procedure();

  auto& ue = test_ues.at(ue_index);

  // Inject Initial Context Setup Request
  ngap_message init_context_setup_request =
      generate_valid_initial_context_setup_request_message_with_pdu_session(ue.amf_ue_id.value(), ue.ran_ue_id.value());
  ngap->handle_message(init_context_setup_request);

  // Check that AMF notifier was called with right type
  ASSERT_TRUE(was_initial_context_setup_response_sent());

  ASSERT_TRUE(was_ue_added());

  ASSERT_TRUE(was_pdu_session_resource_setup_successful());
}

/// Test Initial Context Setup Request with "updated" AMF UE ID
TEST_F(ngap_ue_context_management_procedure_test,
       when_new_amf_ue_id_is_sent_in_initial_context_setup_request_received_then_id_is_updated)
{
  // Test preamble
  ue_index_t ue_index = this->start_procedure();

  auto& ue = test_ues.at(ue_index);

  // Get "first" AMF UE ID received
  amf_ue_id_t old_id = ue.amf_ue_id.value();

  // randomly generate new ID assigned by core
  amf_ue_id_t new_id = old_id;
  while (new_id == old_id) {
    new_id = uint_to_amf_ue_id(
        test_rgen::uniform_int<uint64_t>(amf_ue_id_to_uint(amf_ue_id_t::min), amf_ue_id_to_uint(amf_ue_id_t::max)));
  }
  ASSERT_NE(old_id, new_id);

  // Inject Initial Context Setup Request with new ID
  ngap_message init_context_setup_request =
      generate_valid_initial_context_setup_request_message(new_id, ue.ran_ue_id.value());
  ngap->handle_message(init_context_setup_request);

  // Check that AMF notifier was called with right type
  ASSERT_TRUE(was_initial_context_setup_response_sent());

  ASSERT_TRUE(was_ue_added());
}

/// Test invalid Initial Context Setup Request
TEST_F(ngap_ue_context_management_procedure_test, when_invalid_initial_context_setup_request_received_then_failure_sent)
{
  // Test preamble
  ue_index_t ue_index = this->start_procedure();

  auto& ue = test_ues.at(ue_index);

  // Inject Initial Context Setup Request
  ngap_message init_context_setup_request =
      generate_invalid_initial_context_setup_request_message(ue.amf_ue_id.value(), ue.ran_ue_id.value());
  ngap->handle_message(init_context_setup_request);

  // Check that AMF notifier was called with right type
  ASSERT_TRUE(was_initial_context_setup_failure_sent());
}

/// Test invalid Initial Context Setup Request with PDUSessionResourceSetupListCxtReq
TEST_F(ngap_ue_context_management_procedure_test,
       when_invalid_initial_context_setup_request_with_pdu_session_received_then_failure_sent)
{
  // Test preamble
  ue_index_t ue_index = this->start_procedure();

  auto& ue = test_ues.at(ue_index);

  // Inject Initial Context Setup Request
  ngap_message init_context_setup_request = generate_invalid_initial_context_setup_request_message_with_pdu_session(
      ue.amf_ue_id.value(), ue.ran_ue_id.value());
  ngap->handle_message(init_context_setup_request);

  // Check that AMF notifier was called with right type
  ASSERT_TRUE(was_initial_context_setup_failure_sent());

  ASSERT_TRUE(was_pdu_session_resource_setup_unsuccessful());
}

/// Test successful UE context release
TEST_F(
    ngap_ue_context_management_procedure_test,
    when_ue_context_release_command_as_first_message_from_core_received_then_ue_is_released_and_release_complete_is_sent)
{
  // Test preamble
  ue_index_t ue_index = create_ue();
  auto&      ue       = test_ues.at(ue_index);

  ASSERT_TRUE(was_ue_added());

  // Inject UE Context Release Command
  ngap_message ue_context_release_cmd =
      generate_valid_ue_context_release_command_with_ue_ngap_id_pair(amf_ue_id_t(1), ue.ran_ue_id.value());
  ngap->handle_message(ue_context_release_cmd);

  ASSERT_TRUE(was_ue_context_release_complete_sent());
  ASSERT_TRUE(was_ue_removed());
}

/// Test successful UE context release
TEST_F(ngap_ue_context_management_procedure_test,
       when_ue_context_release_command_with_amf_ue_ngap_id_received_then_ue_is_released_and_release_complete_is_sent)
{
  // Test preamble
  ue_index_t ue_index = this->start_procedure();

  auto& ue = test_ues.at(ue_index);

  // Inject Initial Context Setup Request
  ngap_message init_context_setup_request =
      generate_valid_initial_context_setup_request_message(ue.amf_ue_id.value(), ue.ran_ue_id.value());
  ngap->handle_message(init_context_setup_request);

  ASSERT_TRUE(was_ue_added());

  // Inject UE Context Release Command
  ngap_message ue_context_release_cmd =
      generate_valid_ue_context_release_command_with_amf_ue_ngap_id(ue.amf_ue_id.value());
  ngap->handle_message(ue_context_release_cmd);

  ASSERT_TRUE(was_ue_context_release_complete_sent());
  ASSERT_TRUE(was_ue_removed());
}

/// Initial UE message tests
TEST_F(ngap_ue_context_management_procedure_test,
       when_release_command_after_initial_ue_message_is_received_then_ue_is_released)
{
  ASSERT_EQ(ngap->get_nof_ues(), 0);

  // Test preamble
  ue_index_t ue_index = create_ue();

  auto& ue = test_ues.at(ue_index);

  // Inject DL NAS transport message from AMF
  run_dl_nas_transport(ue_index);

  // Inject UE Context Release Command
  ngap_message ue_context_release_cmd =
      generate_valid_ue_context_release_command_with_amf_ue_ngap_id(ue.amf_ue_id.value());
  ngap->handle_message(ue_context_release_cmd);

  ASSERT_TRUE(was_ue_context_release_complete_sent());
  ASSERT_TRUE(was_ue_removed());
}

/// Test successful UE context release
TEST_F(ngap_ue_context_management_procedure_test,
       when_ue_context_release_command_with_ue_ngap_id_pair_received_then_ue_is_released_and_release_complete_is_sent)
{
  // Test preamble
  ue_index_t ue_index = this->start_procedure();

  auto& ue = test_ues.at(ue_index);

  // Inject Initial Context Setup Request
  ngap_message init_context_setup_request =
      generate_valid_initial_context_setup_request_message(ue.amf_ue_id.value(), ue.ran_ue_id.value());
  ngap->handle_message(init_context_setup_request);

  ASSERT_TRUE(was_ue_added());

  // Inject UE Context Release Command
  ngap_message ue_context_release_cmd =
      generate_valid_ue_context_release_command_with_ue_ngap_id_pair(ue.amf_ue_id.value(), ue.ran_ue_id.value());
  ngap->handle_message(ue_context_release_cmd);

  ASSERT_TRUE(was_ue_context_release_complete_sent());
  ASSERT_TRUE(was_ue_removed());
}

/// Test UE context release for unknown UE
TEST_F(ngap_ue_context_management_procedure_test,
       when_ue_context_release_command_for_unknown_ue_received_then_ue_is_not_released_and_release_complete_is_not_sent)
{
  // Test preamble
  ue_index_t ue_index = this->start_procedure();

  auto& ue = test_ues.at(ue_index);

  // Inject Initial Context Setup Request
  ngap_message init_context_setup_request =
      generate_valid_initial_context_setup_request_message(ue.amf_ue_id.value(), ue.ran_ue_id.value());
  ngap->handle_message(init_context_setup_request);

  ASSERT_TRUE(was_ue_added());

  // Inject UE Context Release Command for unknown UE
  amf_ue_id_t unknown_ue_id = uint_to_amf_ue_id(amf_ue_id_to_uint(ue.amf_ue_id.value()) + 1);

  ngap_message ue_context_release_cmd = generate_valid_ue_context_release_command_with_amf_ue_ngap_id(unknown_ue_id);
  ngap->handle_message(ue_context_release_cmd);

  ASSERT_FALSE(was_ue_context_release_complete_sent());
  ASSERT_FALSE(was_ue_removed());
}

/// Test UE context release request for UE that hasn't received an AMF UE ID yet.
TEST_F(ngap_ue_context_management_procedure_test,
       when_ue_context_release_request_is_received_but_no_amf_ue_ngap_id_is_set_then_request_is_ignored)
{
  // Test preamble - Only create UE but do not have DL traffic from the AMF.
  ue_index_t ue_index = create_ue();

  // Trigger UE context release request.
  cu_cp_ue_context_release_request release_request;
  release_request.ue_index = ue_index;

  async_task<bool>         t = ngap->handle_ue_context_release_request(release_request);
  lazy_task_launcher<bool> t_launcher(t);

  // Status: should have failed already, as there is no UE.
  ASSERT_TRUE(t.ready());

  // Procedure should have failed.
  ASSERT_FALSE(t.get());
  ASSERT_FALSE(was_ue_context_release_request_sent());
}

/// Test UE context release request is not sent multiple times for same UE.
TEST_F(ngap_ue_context_management_procedure_test,
       when_ue_context_release_request_is_received_multiple_times_ngap_message_is_not_sent_more_than_once)
{
  // Test preamble
  ue_index_t ue_index = this->start_procedure();

  auto& ue = test_ues.at(ue_index);

  // Inject Initial Context Setup Request
  ngap_message init_context_setup_request =
      generate_valid_initial_context_setup_request_message(ue.amf_ue_id.value(), ue.ran_ue_id.value());
  ngap->handle_message(init_context_setup_request);

  ASSERT_TRUE(was_ue_added());

  // Trigger UE context release request.
  cu_cp_ue_context_release_request release_request;
  release_request.ue_index = ue_index;

  async_task<bool>         t = ngap->handle_ue_context_release_request(release_request);
  lazy_task_launcher<bool> t_launcher(t);

  // Status: should have succeeded already
  ASSERT_TRUE(t.ready());

  // Procedure should have succeeded.
  ASSERT_TRUE(t.get());
  ASSERT_TRUE(was_ue_context_release_request_sent());

  // Trigger 2nd UE context release request.
  clear_last_received_msg();
  async_task<bool>         t2 = ngap->handle_ue_context_release_request(release_request);
  lazy_task_launcher<bool> t_launcher2(t2);

  // Status: should have succeeded already, as a release request is already pending.
  ASSERT_TRUE(t2.ready());

  // Procedure should have succeeded.
  ASSERT_TRUE(t2.get());
  ASSERT_FALSE(was_ue_context_release_request_sent());
}

/// Test DL NAS transport after transfering UE IDs/context.
TEST_F(ngap_ue_context_management_procedure_test, when_ue_context_is_tranfered_amf_ue_id_is_updated)
{
  // Normal test preamble to get UE created.
  ue_index_t ue_index = this->start_procedure();
  auto&      ue       = test_ues.at(ue_index);
  ASSERT_NE(ue.ue_index, ue_index_t::invalid);

  // Get AMF UE ID
  amf_ue_id_t amf_id = ue.amf_ue_id.value();
  ASSERT_NE(amf_id, amf_ue_id_t::invalid);

  ran_ue_id_t ran_id = ue.ran_ue_id.value();
  ASSERT_NE(ran_id, ran_ue_id_t::invalid);

  // Clear NAS PDU.
  ue.rrc_ue_handler.last_nas_pdu.clear();
  ASSERT_TRUE(ue.rrc_ue_handler.last_nas_pdu.empty());

  // Inject new DL NAS transport from core.
  ngap_message dl_nas_transport = generate_downlink_nas_transport_message(amf_id, ue.ran_ue_id.value());
  ngap->handle_message(dl_nas_transport);

  // Check NAS PDU has been passed to RRC.
  ASSERT_FALSE(ue.rrc_ue_handler.last_nas_pdu.empty());

  // Clear PDU again.
  ue.rrc_ue_handler.last_nas_pdu.clear();

  // Create new UE object (with own RRC UE notifier).
  ue_index_t target_ue_index = create_ue_without_init_ue_message(rnti_t::MAX_CRNTI);
  ASSERT_NE(target_ue_index, ue_index_t::invalid);
  ASSERT_NE(target_ue_index, ue_index);
  auto& target_ue = test_ues.at(target_ue_index);
  ASSERT_TRUE(target_ue.rrc_ue_handler.last_nas_pdu.empty());

  // Transfer NGAP UE context to new target UE.
  ngap->update_ue_index(target_ue_index, ue_index, ue_mng.find_ue(target_ue_index)->get_ngap_cu_cp_ue_notifier());

  // Inject NAS message again.
  ngap->handle_message(dl_nas_transport);

  // Check that RRC notifier of initial UE has not been called.
  ASSERT_TRUE(ue.rrc_ue_handler.last_nas_pdu.empty());

  // Verify that RRC notifier of target UE has indeed benn called.
  ASSERT_FALSE(target_ue.rrc_ue_handler.last_nas_pdu.empty());
}

/// Test when Initial Context Setup Request with inconsistent NGAP ID pair is received,
/// an error indication is sent.
TEST_F(ngap_ue_context_management_procedure_test, when_ue_context_setup_has_inconsistent_id_pair_err_indication_is_sent)
{
  // Test preamble
  ue_index_t ue_index1 = this->start_procedure();
  ue_index_t ue_index2 = this->start_procedure();

  auto& ue1 = test_ues.at(ue_index1);
  auto& ue2 = test_ues.at(ue_index2);

  // Inject Initial Context Setup Request with inconsistent NGAP ID pair.
  ngap_message init_context_setup_request =
      generate_valid_initial_context_setup_request_message(ue1.amf_ue_id.value(), ue2.ran_ue_id.value());
  ngap->handle_message(init_context_setup_request);

  // Check that release of old UE has been requested.
  ASSERT_TRUE(was_ue_release_requested(ue1));

  // Check that error indication has been sent to AMF.
  ASSERT_TRUE(was_error_indication_sent());
  ASSERT_EQ(n2_gw.last_ngap_msgs.back().pdu.init_msg().value.error_ind()->cause.radio_network(),
            asn1::ngap::cause_radio_network_e::options::inconsistent_remote_ue_ngap_id);
}

/// Test when UE Context Release Command with inconsistent NGAP ID pair is received,
/// an error indication is sent.
TEST_F(ngap_ue_context_management_procedure_test,
       when_ue_context_release_command_has_inconsistent_id_pair_err_indication_is_sent)
{
  // Test preamble
  ue_index_t ue_index1 = this->start_procedure();
  ue_index_t ue_index2 = this->start_procedure();

  auto& ue1 = test_ues.at(ue_index1);
  auto& ue2 = test_ues.at(ue_index2);

  // Inject UE Context Release Command with inconsistent NGAP ID pair.
  ngap_message ue_context_release_cmd =
      generate_valid_ue_context_release_command_with_ue_ngap_id_pair(ue1.amf_ue_id.value(), ue2.ran_ue_id.value());
  ngap->handle_message(ue_context_release_cmd);

  // Check that release of old UE has been requested.
  ASSERT_TRUE(was_ue_release_requested(ue1));

  // Check that error indication has been sent to AMF.
  ASSERT_TRUE(was_error_indication_sent());
  ASSERT_EQ(n2_gw.last_ngap_msgs.back().pdu.init_msg().value.error_ind()->cause.radio_network(),
            asn1::ngap::cause_radio_network_e::options::inconsistent_remote_ue_ngap_id);
}

/// Test RRC Inactive Transition Report.
TEST_F(ngap_ue_context_management_procedure_test,
       when_rrc_inactive_transition_report_transmission_is_requested_then_report_is_sent)
{
  // Test preamble
  ue_index_t ue_index = this->start_procedure();

  // Trigger RRC Inactive Transition Report transmission.
  ngap_rrc_inactive_transition_report report;
  report.ue_index                          = ue_index;
  report.rrc_state                         = ngap_rrc_inactive_transition_report::ngap_rrc_state::inactive;
  report.user_location_info.nr_cgi.plmn_id = plmn_identity::test_value();
  report.user_location_info.nr_cgi.nci     = nr_cell_identity::create(gnb_id_t{411, 22}, 0).value();
  report.user_location_info.tai.plmn_id    = plmn_identity::test_value();
  report.user_location_info.tai.tac        = 7;

  async_task<bool>         t = ngap->handle_rrc_inactive_transition_report_required(report);
  lazy_task_launcher<bool> t_launcher(t);

  // Status: should have succeeded already
  ASSERT_TRUE(t.ready());

  // Procedure should have succeeded.
  ASSERT_TRUE(t.get());
  ASSERT_TRUE(was_rrc_inactive_transition_report_sent());
}

TEST_F(ngap_ue_context_management_procedure_test, when_location_report_is_requested_then_report_is_sent)
{
  ue_index_t ue_index = this->start_procedure();

  ngap_location_report report;
  report.ue_index                          = ue_index;
  report.user_location_info.nr_cgi.plmn_id = plmn_identity::test_value();
  report.user_location_info.nr_cgi.nci     = nr_cell_identity::create(gnb_id_t{411, 22}, 0).value();
  report.user_location_info.tai.plmn_id    = plmn_identity::test_value();
  report.user_location_info.tai.tac        = 7;
  report.user_location_info.time_stamp     = 1234;
  report.request_type.event_type           = ngap_location_reporting_event_type::direct;

  ASSERT_TRUE(ngap->get_ngap_control_message_handler().handle_location_report_required(report));
  ASSERT_TRUE(was_location_report_sent());

  const auto& asn1_report = n2_gw.last_ngap_msgs.back().pdu.init_msg().value.location_report();
  ASSERT_EQ(asn1_report->ran_ue_ngap_id, ran_ue_id_to_uint(test_ues.at(ue_index).ran_ue_id.value()));
  ASSERT_EQ(asn1_report->amf_ue_ngap_id, amf_ue_id_to_uint(test_ues.at(ue_index).amf_ue_id.value()));
  ASSERT_EQ(asn1_report->user_location_info.type().value,
            asn1::ngap::user_location_info_c::types_opts::user_location_info_nr);
  ASSERT_EQ(asn1_report->location_report_request_type.event_type.value, asn1::ngap::event_type_opts::direct);
  ASSERT_EQ(asn1_report->location_report_request_type.report_area.value, asn1::ngap::report_area_opts::cell);
}

TEST_F(ngap_ue_context_management_procedure_test, when_ntn_derived_tac_is_available_then_location_report_has_ntn_tai)
{
  ue_index_t ue_index = this->start_procedure();

  ngap_location_report report;
  report.ue_index                              = ue_index;
  report.user_location_info.nr_cgi.plmn_id     = plmn_identity::test_value();
  report.user_location_info.nr_cgi.nci         = nr_cell_identity::create(gnb_id_t{411, 22}, 0).value();
  report.user_location_info.tai.plmn_id        = plmn_identity::test_value();
  report.user_location_info.tai.tac            = 7;
  report.user_location_info.ntn_derived_tac    = 9;
  report.request_type.event_type               = ngap_location_reporting_event_type::direct;

  ASSERT_TRUE(ngap->get_ngap_control_message_handler().handle_location_report_required(report));
  ASSERT_TRUE(was_location_report_sent());

  const auto& asn1_report = n2_gw.last_ngap_msgs.back().pdu.init_msg().value.location_report();
  const auto& nr_info     = asn1_report->user_location_info.user_location_info_nr();
  ASSERT_TRUE(nr_info.ie_exts_present);
  ASSERT_TRUE(nr_info.ie_exts.nr_ntn_tai_info_present);
  ASSERT_EQ(nr_info.ie_exts.nr_ntn_tai_info.tac_list_in_nr_ntn.size(), 1);
  ASSERT_EQ(nr_info.ie_exts.nr_ntn_tai_info.tac_list_in_nr_ntn[0].to_number(), 7);
  ASSERT_TRUE(nr_info.ie_exts.nr_ntn_tai_info.ue_location_derived_tac_in_nr_ntn_present);
  ASSERT_EQ(nr_info.ie_exts.nr_ntn_tai_info.ue_location_derived_tac_in_nr_ntn.to_number(), 9);
}

TEST_F(ngap_ue_context_management_procedure_test, when_location_reporting_control_is_received_then_cu_cp_is_notified)
{
  ue_index_t ue_index = this->start_procedure();
  const auto& ue      = test_ues.at(ue_index);

  ngap->handle_message(generate_location_reporting_control_message(ue.amf_ue_id.value(), ue.ran_ue_id.value()));

  ASSERT_EQ(cu_cp_notifier.last_location_reporting_control.ue_index, ue_index);
  ASSERT_EQ(cu_cp_notifier.last_location_reporting_control.request_type.event_type,
            ngap_location_reporting_event_type::direct);
}

TEST_F(ngap_ue_context_management_procedure_test, when_location_reporting_control_has_unknown_ue_then_failure_is_sent)
{
  ngap->handle_message(generate_location_reporting_control_message(uint_to_amf_ue_id(11), uint_to_ran_ue_id(10)));

  ASSERT_TRUE(was_location_reporting_failure_sent());
  ASSERT_EQ(n2_gw.last_ngap_msgs.back().pdu.init_msg().value.location_report_fail_ind()->cause.radio_network(),
            asn1::ngap::cause_radio_network_e::options::unknown_local_ue_ngap_id);
}

TEST_F(ngap_ue_context_management_procedure_test,
       when_location_reporting_control_is_rejected_by_cu_cp_then_failure_is_sent)
{
  ue_index_t ue_index = this->start_procedure();
  const auto& ue      = test_ues.at(ue_index);
  cu_cp_notifier.location_reporting_control_response =
      ngap_location_reporting_control_response{false, ngap_cause_radio_network_t::unspecified};

  ngap->handle_message(generate_location_reporting_control_message(ue.amf_ue_id.value(), ue.ran_ue_id.value()));

  ASSERT_TRUE(was_location_reporting_failure_sent());
  ASSERT_EQ(n2_gw.last_ngap_msgs.back().pdu.init_msg().value.location_report_fail_ind()->cause.radio_network(),
            asn1::ngap::cause_radio_network_e::options::unspecified);
}

TEST_F(ngap_ue_context_management_procedure_test,
       when_location_reporting_control_has_duplicate_ref_ids_then_failure_is_sent)
{
  ue_index_t ue_index = this->start_procedure();
  const auto& ue      = test_ues.at(ue_index);

  ngap_message msg = generate_location_reporting_control_message(ue.amf_ue_id.value(), ue.ran_ue_id.value());
  auto& request_type = msg.pdu.init_msg().value.location_report_ctrl()->location_report_request_type;
  request_type.event_type.value = asn1::ngap::event_type_opts::ue_presence_in_area_of_interest;
  asn1::ngap::area_of_interest_item_s first_item;
  first_item.location_report_ref_id = 3;
  request_type.area_of_interest_list.push_back(first_item);
  asn1::ngap::area_of_interest_item_s second_item;
  second_item.location_report_ref_id = 3;
  request_type.area_of_interest_list.push_back(second_item);

  ngap->handle_message(msg);

  ASSERT_TRUE(was_location_reporting_failure_sent());
  ASSERT_EQ(n2_gw.last_ngap_msgs.back().pdu.init_msg().value.location_report_fail_ind()->cause.radio_network(),
            asn1::ngap::cause_radio_network_e::options::multiple_location_report_ref_id_instances);
}
