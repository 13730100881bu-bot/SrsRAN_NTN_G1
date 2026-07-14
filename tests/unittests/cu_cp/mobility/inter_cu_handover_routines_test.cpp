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

#include "lib/cu_cp/cell_meas_manager/cell_meas_manager_impl.h"
#include "lib/cu_cp/routines/mobility/inter_cu_handover_target_routine.h"
#include "mobility_test_helpers.h"
#include "srsran/cu_cp/cu_cp_types.h"
#include "srsran/ngap/ngap_handover.h"
#include "srsran/support/async/async_test_utils.h"
#include <gtest/gtest.h>

using namespace srsran;
using namespace srs_cu_cp;

namespace {

/// Stub mobility notifier required by the cell_meas_manager constructor. The target routine doesn't
/// actually invoke the measurement manager, so this never fires.
struct stub_meas_mobility_notifier : public cell_meas_mobility_manager_notifier {
  void on_neighbor_better_than_spcell(ue_index_t       ue_index,
                                      gnb_id_t         neighbor_gnb_id,
                                      nr_cell_identity neighbor_nci,
                                      pci_t            neighbor_pci) override
  {
  }
};

/// Build a minimal but well-formed HandoverRequest common type that the target routine can consume.
/// One PDU session is included so that E1AP gets at least one item to set up.
ngap_handover_request make_minimal_handover_request(ue_index_t ue_index, const nr_cell_global_id_t& target_cgi)
{
  ngap_handover_request req;
  req.ue_index    = ue_index;
  req.handov_type = ngap_handov_type::intra5gs;
  req.cause       = ngap_cause_radio_network_t::ho_desirable_for_radio_reason;
  req.ue_aggr_max_bit_rate.ue_aggr_max_bit_rate_dl = 1'000'000;
  req.ue_aggr_max_bit_rate.ue_aggr_max_bit_rate_ul = 500'000;
  req.guami.plmn                                   = plmn_identity::test_value();
  req.guami.amf_set_id                             = 1;
  req.guami.amf_pointer                            = 0;
  req.guami.amf_region_id                          = 1;
  req.source_to_target_transparent_container.target_cell_id = target_cgi;
  req.source_to_target_transparent_container.rrc_container  = make_byte_buffer("deadbeef").value();

  cu_cp_pdu_session_res_setup_item ps_item;
  ps_item.pdu_session_id     = uint_to_pdu_session_id(1);
  ps_item.s_nssai.sst        = slice_service_type{1};
  ps_item.ul_ngu_up_tnl_info = up_transport_layer_info{transport_layer_address::create_from_string("127.0.0.1"),
                                                       int_to_gtpu_teid(0x1)};
  req.pdu_session_res_setup_list_ho_req.emplace(ps_item.pdu_session_id, ps_item);
  return req;
}

} // namespace

// =====================================================================================================
// Target routine tests — exercise the E1AP/F1AP coordination of incoming N2 handover at the target gNB.
// =====================================================================================================

class inter_cu_handover_target_test : public mobility_test
{
protected:
  inter_cu_handover_target_test() : meas_mng(meas_cfg, meas_notifier, ue_mng)
  {
    // Allocate a UE on the target side. The NGAP layer would normally call request_new_ue_index_allocation()
    // to provision this UE before invoking the routine; in the unit test we do it directly.
    ue_index = ue_mng.add_ue(uint_to_du_index(0), int_to_gnb_du_id(0), pci_t{2}, to_rnti(0x4602));
    ue_mng.set_plmn(ue_index, plmn_identity::test_value());
    ue = ue_mng.find_du_ue(ue_index);
    EXPECT_NE(ue, nullptr);

    // Default outcome: both layers succeed.
    e1ap_mng.set_first_message_outcome(bearer_context_outcome_t{true, {1}, {}, {}});
    f1ap_mng.set_ue_context_setup_outcome(true);
  }

  ngap_handover_request build_request() const
  {
    nr_cell_global_id_t cgi;
    cgi.plmn_id = plmn_identity::test_value();
    cgi.nci     = nr_cell_identity::create(0xabcde).value();
    return make_minimal_handover_request(ue_index, cgi);
  }

  void run(const ngap_handover_request& req)
  {
    auto t = start_inter_cu_handover_target_routine(
        req, e1ap_mng, f1ap_mng, ue_removal_handler, ue_mng, meas_mng, sec_ind, test_logger);
    lazy_task_launcher<ngap_handover_resource_allocation_response> launcher(t);
    ASSERT_TRUE(t.ready());
    response.emplace(t.get());
  }

  bool succeeded() const { return response.has_value() and response->success; }
  bool failed() const { return response.has_value() and not response->success; }

  cell_meas_manager_cfg                                     meas_cfg{};
  stub_meas_mobility_notifier                               meas_notifier;
  cell_meas_manager                                         meas_mng;
  security_indication_t                                     sec_ind{};
  dummy_e1ap_bearer_context_manager                         e1ap_mng;
  dummy_f1ap_ue_context_manager                             f1ap_mng;
  dummy_cu_cp_ue_removal_handler                            ue_removal_handler{&ue_mng};
  ue_index_t                                                ue_index = ue_index_t::invalid;
  cu_cp_ue*                                                 ue       = nullptr;
  std::optional<ngap_handover_resource_allocation_response> response;
};

TEST_F(inter_cu_handover_target_test, when_e1ap_and_f1ap_succeed_then_response_is_admitted)
{
  ngap_handover_request req = build_request();
  run(req);

  ASSERT_TRUE(response.has_value());
  EXPECT_TRUE(response->success);
  EXPECT_EQ(response->ue_index, ue_index);
  EXPECT_FALSE(response->pdu_session_res_admitted_list.empty());
  // The target-to-source container must contain the F1AP-supplied RRC Reconfiguration so the source
  // gNB can forward it to the UE inside the HandoverCommand.
  EXPECT_FALSE(response->target_to_source_transparent_container.rrc_container.empty());
}

TEST_F(inter_cu_handover_target_test, when_e1ap_setup_fails_then_response_is_failure_and_ue_is_removed)
{
  // Simulate CU-UP rejecting the bearer context setup (e.g. resource exhaustion).
  e1ap_mng.set_first_message_outcome(bearer_context_outcome_t{false, {}, {1}, {}});

  ngap_handover_request req = build_request();
  run(req);

  ASSERT_TRUE(response.has_value());
  EXPECT_FALSE(response->success);
  EXPECT_TRUE(response->pdu_session_res_admitted_list.empty());
  EXPECT_EQ(ue_mng.find_du_ue(ue_index), nullptr) << "UE must be reaped after E1AP failure";
}

TEST_F(inter_cu_handover_target_test, when_f1ap_setup_fails_then_response_is_failure_and_ue_is_removed)
{
  // E1AP succeeds but the target DU rejects the UE context (e.g. PCell unavailable).
  e1ap_mng.set_first_message_outcome(bearer_context_outcome_t{true, {1}, {}, {}});
  f1ap_mng.set_ue_context_setup_outcome(false);

  ngap_handover_request req = build_request();
  run(req);

  ASSERT_TRUE(response.has_value());
  EXPECT_FALSE(response->success);
  EXPECT_EQ(ue_mng.find_du_ue(ue_index), nullptr) << "UE must be reaped after F1AP failure";
}

TEST_F(inter_cu_handover_target_test, response_carries_correct_admitted_list)
{
  ngap_handover_request req          = build_request();
  const auto            expected_nci = req.source_to_target_transparent_container.target_cell_id.nci;
  run(req);

  ASSERT_TRUE(response.has_value());
  EXPECT_TRUE(response->success);
  // PDU session admitted list must mirror the input list size (1 here).
  EXPECT_EQ(response->pdu_session_res_admitted_list.size(), 1U);
  EXPECT_TRUE(response->pdu_session_res_admitted_list.contains(uint_to_pdu_session_id(1)));
  EXPECT_EQ(expected_nci, nr_cell_identity::create(0xabcde).value());
}
