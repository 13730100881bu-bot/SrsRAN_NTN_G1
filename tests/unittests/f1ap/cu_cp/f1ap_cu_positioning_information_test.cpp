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
#include "tests/test_doubles/f1ap/f1ap_test_messages.h"
#include "srsran/asn1/f1ap/common.h"
#include "srsran/asn1/f1ap/f1ap_pdu_contents.h"
#include "srsran/support/async/async_test_utils.h"
#include <gtest/gtest.h>

using namespace srsran;
using namespace srs_cu_cp;

class f1ap_cu_positioning_information_test : public f1ap_cu_test
{
protected:
  void start_procedure(ue_index_t ue_index)
  {
    positioning_information_request_t request;
    request.ue_index = ue_index;
    task             = f1ap->get_f1ap_nrppa_message_handler().handle_positioning_information_request(request);
    task_launcher.emplace(task);
  }

  async_task<expected<positioning_information_response_t, positioning_information_failure_t>> task;
  std::optional<lazy_task_launcher<expected<positioning_information_response_t, positioning_information_failure_t>>>
      task_launcher;
};

TEST_F(f1ap_cu_positioning_information_test, request_is_sent_to_du)
{
  test_ue& ue = run_ue_context_setup();

  start_procedure(ue.ue_index);

  ASSERT_EQ(f1ap_pdu_notifier.last_f1ap_msg.pdu.type().value, asn1::f1ap::f1ap_pdu_c::types_opts::init_msg);
  ASSERT_EQ(f1ap_pdu_notifier.last_f1ap_msg.pdu.init_msg().value.type().value,
            asn1::f1ap::f1ap_elem_procs_o::init_msg_c::types_opts::positioning_info_request);
  const auto& request = f1ap_pdu_notifier.last_f1ap_msg.pdu.init_msg().value.positioning_info_request();
  ASSERT_EQ(request->gnb_cu_ue_f1ap_id, gnb_cu_ue_f1ap_id_to_uint(ue.cu_ue_id.value()));
  ASSERT_EQ(request->gnb_du_ue_f1ap_id, gnb_du_ue_f1ap_id_to_uint(ue.du_ue_id.value()));
  ASSERT_FALSE(task.ready());
}

TEST_F(f1ap_cu_positioning_information_test, response_completes_request)
{
  test_ue& ue = run_ue_context_setup();
  start_procedure(ue.ue_index);

  f1ap->handle_message(test_helpers::generate_positioning_information_response(ue.du_ue_id.value(), ue.cu_ue_id.value()));

  ASSERT_TRUE(task.ready());
  ASSERT_TRUE(task.get().has_value());
}

TEST_F(f1ap_cu_positioning_information_test, failure_returns_nrppa_failure)
{
  test_ue& ue = run_ue_context_setup();
  start_procedure(ue.ue_index);

  f1ap->handle_message(test_helpers::generate_positioning_information_failure(ue.du_ue_id.value(), ue.cu_ue_id.value()));

  ASSERT_TRUE(task.ready());
  ASSERT_FALSE(task.get().has_value());
}
