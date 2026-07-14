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

#include "inter_cu_handover_target_routine.h"
#include "srsran/e1ap/cu_cp/e1ap_cu_cp.h"
#include "srsran/e1ap/cu_cp/e1ap_cu_cp_bearer_context_update.h"
#include "srsran/f1ap/cu_cp/f1ap_cu.h"

using namespace srsran;
using namespace srs_cu_cp;

namespace {

class inter_cu_handover_target_routine_impl
{
public:
  inter_cu_handover_target_routine_impl(const ngap_handover_request& request_,
                                        e1ap_bearer_context_manager& e1ap_bearer_ctxt_mng_,
                                        f1ap_ue_context_manager&     f1ap_ue_ctxt_mng_,
                                        cu_cp_ue_removal_handler&    ue_removal_handler_,
                                        ue_manager&                  ue_mng_,
                                        cell_meas_manager&           cell_meas_mngr_,
                                        const security_indication_t& default_security_indication_,
                                        srslog::basic_logger&        logger_) :
    request(request_),
    e1ap(e1ap_bearer_ctxt_mng_),
    f1ap(f1ap_ue_ctxt_mng_),
    ue_removal_handler(ue_removal_handler_),
    ue_mng(ue_mng_),
    cell_meas_mngr(cell_meas_mngr_),
    default_security_indication(default_security_indication_),
    logger(logger_)
  {
    (void)cell_meas_mngr;
    (void)default_security_indication;
    response.ue_index = request.ue_index;
  }

  void operator()(coro_context<async_task<ngap_handover_resource_allocation_response>>& ctx)
  {
    CORO_BEGIN(ctx);

    logger.info("ue={}: \"{}\" started...", request.ue_index, name());

    // 1. E1AP Bearer Context Setup at the CU-UP. The CU-UP allocates the N3 GTP-U tunnels and
    //    returns the DL UP TNL information.
    e1ap_request.ue_index                         = request.ue_index;
    e1ap_request.ue_dl_aggregate_maximum_bit_rate = request.ue_aggr_max_bit_rate.ue_aggr_max_bit_rate_dl;
    e1ap_request.serving_plmn                     = request.guami.plmn;
    e1ap_request.activity_notif_level             = "ue";
    // Map PDU sessions from HO request to E1AP setup items. Only the metadata that the CU-UP needs
    // for tunnel allocation is forwarded; the per-flow QoS already lives inside the items.
    for (const auto& ps : request.pdu_session_res_setup_list_ho_req) {
      e1ap_pdu_session_res_to_setup_item item;
      item.pdu_session_id    = ps.pdu_session_id;
      item.pdu_session_type  = ps.pdu_session_type;
      item.snssai            = ps.s_nssai;
      item.security_ind      = ps.security_ind.value_or(default_security_indication);
      item.ng_ul_up_tnl_info = ps.ul_ngu_up_tnl_info;
      e1ap_request.pdu_session_res_to_setup_list.emplace(ps.pdu_session_id, std::move(item));
    }

    CORO_AWAIT_VALUE(e1ap_response, e1ap.handle_bearer_context_setup_request(e1ap_request));

    if (not e1ap_response.success) {
      logger.warning("ue={}: \"{}\" failed: E1AP Bearer Context Setup rejected by CU-UP", request.ue_index, name());
      CORO_AWAIT(handle_failure());
      CORO_EARLY_RETURN(response);
    }

    // 2. F1AP UE Context Setup at the target DU. The DU creates the air-interface resources and
    //    returns the RRC Reconfiguration container in du_to_cu_rrc_info.cell_group_cfg.
    f1ap_request.ue_index      = request.ue_index;
    f1ap_request.sp_cell_id    = request.source_to_target_transparent_container.target_cell_id;
    f1ap_request.serv_cell_idx = 0;
    {
      // Forward the source-side HandoverPreparationInfo so the target DU can build the RRC
      // Reconfiguration on top of the source UE configuration.
      f1ap_cu_to_du_rrc_info_ext_ies_container ext_ies;
      ext_ies.ho_prep_info = request.source_to_target_transparent_container.rrc_container.copy();
      f1ap_request.cu_to_du_rrc_info.ie_exts.emplace(std::move(ext_ies));
    }
    // Always set up SRB1 and SRB2 for the incoming UE.
    f1ap_request.srbs_to_be_setup_list.push_back(f1ap_srb_to_setup{srb_id_t::srb1});
    f1ap_request.srbs_to_be_setup_list.push_back(f1ap_srb_to_setup{srb_id_t::srb2});

    CORO_AWAIT_VALUE(f1ap_response, f1ap.handle_ue_context_setup_request(f1ap_request, std::nullopt));

    if (not f1ap_response.success) {
      logger.warning("ue={}: \"{}\" failed: F1AP UE Context Setup rejected by target DU", request.ue_index, name());
      CORO_AWAIT(handle_failure());
      CORO_EARLY_RETURN(response);
    }

    // 3. Build the success response. The target-to-source transparent container carries the RRC
    //    Reconfiguration that the source gNB must forward to the UE.
    response.success = true;
    response.target_to_source_transparent_container.rrc_container =
        f1ap_response.du_to_cu_rrc_info.cell_group_cfg.copy();

    // Mark each PDU session admitted using the DL N3 UP TNL info returned by the CU-UP.
    for (const auto& setup_item : e1ap_response.pdu_session_resource_setup_list) {
      ngap_pdu_session_res_admitted_item admitted_item;
      admitted_item.pdu_session_id                             = setup_item.pdu_session_id;
      admitted_item.ho_request_ack_transfer.dl_ngu_up_tnl_info = setup_item.ng_dl_up_tnl_info;
      response.pdu_session_res_admitted_list.emplace(setup_item.pdu_session_id, std::move(admitted_item));
    }

    logger.info("ue={}: \"{}\" finished successfully", request.ue_index, name());
    CORO_RETURN(response);
  }

  static const char* name() { return "Inter-CU Handover Target Routine"; }

private:
  async_task<void> handle_failure()
  {
    response.success = false;
    response.cause   = ngap_cause_radio_network_t::ho_target_not_allowed;
    return ue_removal_handler.handle_ue_removal_request(request.ue_index);
  }

  const ngap_handover_request& request;
  e1ap_bearer_context_manager& e1ap;
  f1ap_ue_context_manager&     f1ap;
  cu_cp_ue_removal_handler&    ue_removal_handler;
  ue_manager&                  ue_mng;
  cell_meas_manager&           cell_meas_mngr;
  const security_indication_t& default_security_indication;
  srslog::basic_logger&        logger;

  e1ap_bearer_context_setup_request          e1ap_request;
  e1ap_bearer_context_setup_response         e1ap_response;
  f1ap_ue_context_setup_request              f1ap_request;
  f1ap_ue_context_setup_response             f1ap_response;
  ngap_handover_resource_allocation_response response;
};

} // namespace

async_task<ngap_handover_resource_allocation_response>
srsran::srs_cu_cp::start_inter_cu_handover_target_routine(const ngap_handover_request& request,
                                                          e1ap_bearer_context_manager& e1ap_bearer_ctxt_mng,
                                                          f1ap_ue_context_manager&     f1ap_ue_ctxt_mng,
                                                          cu_cp_ue_removal_handler&    ue_removal_handler,
                                                          ue_manager&                  ue_mng,
                                                          cell_meas_manager&           cell_meas_mngr,
                                                          const security_indication_t& default_security_indication,
                                                          srslog::basic_logger&        logger)
{
  return launch_async<inter_cu_handover_target_routine_impl>(request,
                                                             e1ap_bearer_ctxt_mng,
                                                             f1ap_ue_ctxt_mng,
                                                             ue_removal_handler,
                                                             ue_mng,
                                                             cell_meas_mngr,
                                                             default_security_indication,
                                                             logger);
}
