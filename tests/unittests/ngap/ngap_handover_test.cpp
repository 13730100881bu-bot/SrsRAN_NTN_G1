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
#include "tests/unittests/ngap/ngap_test_messages.h"
#include "srsran/asn1/ngap/common.h"
#include "srsran/asn1/ngap/ngap_pdu_contents.h"
#include "srsran/ngap/ngap_handover.h"
#include "srsran/ran/cu_types.h"
#include "srsran/ran/rb_id.h"
#include "srsran/support/async/async_test_utils.h"
#include <gtest/gtest.h>

using namespace srsran;
using namespace srs_cu_cp;

/// Test successful handover preparation procedure
TEST_F(ngap_test, when_ue_missing_then_handover_preparation_procedure_fails)
{
  ngap_handover_preparation_request request = {};
  request.ue_index                          = uint_to_ue_index(0);

  // Action 1: Launch HO preparation procedure
  test_logger.info("Launch source NGAP handover preparation procedure");
  async_task<ngap_handover_preparation_response>         t = ngap->handle_handover_preparation_request(request);
  lazy_task_launcher<ngap_handover_preparation_response> t_launcher(t);

  // Status: should have failed already, as there is no UE.
  ASSERT_TRUE(t.ready());

  // Procedure should have failed.
  ASSERT_FALSE(t.get().success);

  // Make sure no NGAP pdu was sent
  ASSERT_TRUE(n2_gw.last_ngap_msgs.empty());
}

// =====================================================================================================
// Target-side NGAP HandoverResourceAllocation tests for the new implementation.
// =====================================================================================================

TEST_F(ngap_test, when_handover_request_received_then_handover_failure_is_sent_when_no_ue_can_be_allocated)
{
  ASSERT_TRUE(run_ng_setup());
  n2_gw.last_ngap_msgs.clear();

  // The default dummy_ngap_cu_cp_notifier::request_new_ue_index_allocation returns invalid, so the
  // NGAP layer must reply with HandoverFailure immediately.
  ngap_message ho_request = generate_valid_handover_request(uint_to_amf_ue_id(0x1000));
  ngap->handle_message(ho_request);

  ASSERT_FALSE(n2_gw.last_ngap_msgs.empty());
  const auto& sent = n2_gw.last_ngap_msgs.back();
  ASSERT_EQ(sent.pdu.type().value, asn1::ngap::ngap_pdu_c::types_opts::unsuccessful_outcome);
  EXPECT_EQ(sent.pdu.unsuccessful_outcome().value.type().value,
            asn1::ngap::ngap_elem_procs_o::unsuccessful_outcome_c::types_opts::ho_fail);
}
