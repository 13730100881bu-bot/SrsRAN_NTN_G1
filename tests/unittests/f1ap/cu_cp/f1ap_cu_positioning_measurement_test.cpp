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
#include "srsran/support/async/async_test_utils.h"
#include <gtest/gtest.h>

using namespace srsran;
using namespace srs_cu_cp;

namespace {

f1ap_message generate_positioning_assistance_information_feedback(unsigned transaction_id,
                                                                  const nr_cell_global_id_t& cgi)
{
  f1ap_message feedback;
  feedback.pdu.set_init_msg().load_info_obj(ASN1_F1AP_ID_POSITIONING_ASSIST_INFO_FEEDBACK);
  auto& asn1_feedback = feedback.pdu.init_msg().value.positioning_assist_info_feedback();
  asn1_feedback->transaction_id = transaction_id;
  asn1_feedback->positioning_broadcast_cells_present = true;
  asn1_feedback->positioning_broadcast_cells.push_back(cgi_to_asn1(cgi));
  asn1_feedback->routing_id_present = true;
  asn1_feedback->routing_id.from_string("ab");
  return feedback;
}

} // namespace

class f1ap_cu_positioning_measurement_test : public f1ap_cu_test
{
protected:
  void start_procedure(ue_index_t ue_index)
  {
    measurement_request_t request;
    request.ue_index    = ue_index;
    request.lmf_meas_id = uint_to_lmf_meas_id(41);
    request.ran_meas_id = uint_to_ran_meas_id(42);
    request.trp_meas_request_list.push_back({uint_to_trp_id(0x101)});
    request.report_characteristics = report_characteristics_t::on_demand;
    request.trp_meas_quantities.push_back({trp_meas_quantities_item_t::ul_rtoa});

    task = f1ap->get_f1ap_nrppa_message_handler().handle_positioning_measurement_request(request);
    task_launcher.emplace(task);
  }

  async_task<expected<measurement_response_t, measurement_failure_t>> task;
  std::optional<lazy_task_launcher<expected<measurement_response_t, measurement_failure_t>>> task_launcher;
};

TEST_F(f1ap_cu_positioning_measurement_test, request_is_sent_to_du)
{
  test_ue& ue = run_ue_context_setup();

  start_procedure(ue.ue_index);

  ASSERT_EQ(f1ap_pdu_notifier.last_f1ap_msg.pdu.type().value, asn1::f1ap::f1ap_pdu_c::types_opts::init_msg);
  ASSERT_EQ(f1ap_pdu_notifier.last_f1ap_msg.pdu.init_msg().value.type().value,
            asn1::f1ap::f1ap_elem_procs_o::init_msg_c::types_opts::positioning_meas_request);
  const auto& request = f1ap_pdu_notifier.last_f1ap_msg.pdu.init_msg().value.positioning_meas_request();
  ASSERT_EQ(request->lmf_meas_id, 41U);
  ASSERT_EQ(request->ran_meas_id, 42U);
  ASSERT_EQ(request->trp_meas_request_list.size(), 1U);
  ASSERT_EQ(request->pos_meas_quantities.size(), 1U);
  ASSERT_FALSE(task.ready());
}

TEST_F(f1ap_cu_positioning_measurement_test, response_completes_request)
{
  test_ue& ue = run_ue_context_setup();
  start_procedure(ue.ue_index);
  const auto& request = f1ap_pdu_notifier.last_f1ap_msg.pdu.init_msg().value.positioning_meas_request();

  f1ap->handle_message(test_helpers::generate_positioning_measurement_response(
      uint_to_lmf_meas_id(request->lmf_meas_id),
      uint_to_ran_meas_id(request->ran_meas_id),
      {uint_to_trp_id(0x101)},
      request->transaction_id));

  ASSERT_TRUE(task.ready());
  ASSERT_TRUE(task.get().has_value());
  ASSERT_EQ(task.get().value().lmf_meas_id, uint_to_lmf_meas_id(41));
  ASSERT_EQ(task.get().value().ran_meas_id, uint_to_ran_meas_id(42));
}

TEST_F(f1ap_cu_positioning_measurement_test, failure_returns_nrppa_failure)
{
  test_ue& ue = run_ue_context_setup();
  start_procedure(ue.ue_index);
  const auto& request = f1ap_pdu_notifier.last_f1ap_msg.pdu.init_msg().value.positioning_meas_request();

  f1ap->handle_message(test_helpers::generate_positioning_measurement_failure(uint_to_lmf_meas_id(request->lmf_meas_id),
                                                                              uint_to_ran_meas_id(request->ran_meas_id),
                                                                              request->transaction_id));

  ASSERT_TRUE(task.ready());
  ASSERT_FALSE(task.get().has_value());
  ASSERT_EQ(task.get().error().lmf_meas_id, uint_to_lmf_meas_id(41));
}

TEST_F(f1ap_cu_positioning_measurement_test, assistance_control_request_is_sent_to_du)
{
  positioning_assistance_information_control_request_t request;
  request.transaction_id = 37;
  request.pos_assist_info = byte_buffer::create({0x10, 0x20}).value();
  request.pos_broadcast = positioning_assistance_broadcast_action::start;
  request.positioning_broadcast_cells.push_back(
      nr_cell_global_id_t{plmn_identity::test_value(), nr_cell_identity::create(0x66c000).value()});
  request.routing_id = byte_buffer::create({0xab}).value();

  auto assistance_task =
      f1ap->get_f1ap_nrppa_message_handler().handle_positioning_assistance_information_control(request);
  lazy_task_launcher<expected<positioning_assistance_information_feedback_t,
                              positioning_assistance_information_failure_t>>
      launcher(assistance_task);

  ASSERT_EQ(f1ap_pdu_notifier.last_f1ap_msg.pdu.type().value, asn1::f1ap::f1ap_pdu_c::types_opts::init_msg);
  ASSERT_EQ(f1ap_pdu_notifier.last_f1ap_msg.pdu.init_msg().value.type().value,
            asn1::f1ap::f1ap_elem_procs_o::init_msg_c::types_opts::positioning_assist_info_ctrl);
  const auto& asn1_request = f1ap_pdu_notifier.last_f1ap_msg.pdu.init_msg().value.positioning_assist_info_ctrl();
  ASSERT_EQ(asn1_request->transaction_id, 37U);
  ASSERT_TRUE(asn1_request->pos_assist_info_present);
  ASSERT_TRUE(asn1_request->pos_broadcast_present);
  ASSERT_EQ(asn1_request->pos_broadcast.value, asn1::f1ap::pos_broadcast_opts::start);
  ASSERT_TRUE(asn1_request->positioning_broadcast_cells_present);
  ASSERT_EQ(asn1_request->positioning_broadcast_cells.size(), 1U);
  ASSERT_TRUE(asn1_request->routing_id_present);
  ASSERT_FALSE(assistance_task.ready());
}

TEST_F(f1ap_cu_positioning_measurement_test, assistance_feedback_completes_control_request)
{
  positioning_assistance_information_control_request_t request;
  request.transaction_id = 37;
  request.pos_broadcast = positioning_assistance_broadcast_action::start;
  request.positioning_broadcast_cells.push_back(
      nr_cell_global_id_t{plmn_identity::test_value(), nr_cell_identity::create(0x66c000).value()});
  request.routing_id = byte_buffer::create({0xab}).value();

  auto assistance_task =
      f1ap->get_f1ap_nrppa_message_handler().handle_positioning_assistance_information_control(request);
  lazy_task_launcher<expected<positioning_assistance_information_feedback_t,
                              positioning_assistance_information_failure_t>>
      launcher(assistance_task);

  f1ap->handle_message(generate_positioning_assistance_information_feedback(37, request.positioning_broadcast_cells[0]));

  ASSERT_TRUE(assistance_task.ready());
  ASSERT_TRUE(assistance_task.get().has_value());
  ASSERT_EQ(assistance_task.get().value().transaction_id, 37U);
  ASSERT_EQ(assistance_task.get().value().positioning_broadcast_cells.size(), 1U);
  ASSERT_TRUE(assistance_task.get().value().routing_id.has_value());
  ASSERT_EQ(assistance_task.get().value().routing_id.value(), byte_buffer::create({0xab}).value());
}

TEST_F(f1ap_cu_positioning_measurement_test, unsupported_empty_assistance_control_request_returns_failure)
{
  positioning_assistance_information_control_request_t request;
  request.transaction_id = 41;

  auto assistance_task =
      f1ap->get_f1ap_nrppa_message_handler().handle_positioning_assistance_information_control(request);
  lazy_task_launcher<expected<positioning_assistance_information_feedback_t,
                              positioning_assistance_information_failure_t>>
      launcher(assistance_task);

  ASSERT_TRUE(assistance_task.ready());
  ASSERT_FALSE(assistance_task.get().has_value());
  ASSERT_EQ(assistance_task.get().error().transaction_id, 41U);
}
