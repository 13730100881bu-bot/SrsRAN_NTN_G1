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

#include "inter_cu_handover_source_routine.h"
#include "srsran/asn1/asn1_utils.h"
#include "srsran/asn1/ngap/ngap_ies.h"
#include "srsran/f1ap/cu_cp/f1ap_cu.h"
#include "srsran/f1ap/cu_cp/f1ap_cu_ue_context_update.h"
#include "srsran/ngap/ngap_handover.h"

using namespace srsran;
using namespace srs_cu_cp;

namespace {

class inter_cu_handover_source_routine_impl
{
public:
  inter_cu_handover_source_routine_impl(ue_index_t                    ue_index_,
                                        byte_buffer                   command_,
                                        ue_manager&                   ue_mng_,
                                        du_processor_repository&      du_db_,
                                        cu_up_processor_repository&   cu_up_db_,
                                        ngap_control_message_handler& ngap_,
                                        srslog::basic_logger&         logger_) :
    ue_index(ue_index_),
    command(std::move(command_)),
    ue_mng(ue_mng_),
    du_db(du_db_),
    cu_up_db(cu_up_db_),
    ngap(ngap_),
    logger(logger_)
  {
    (void)cu_up_db; // CU-UP not directly touched in MVP; data forwarding is not negotiated.
  }

  void operator()(coro_context<async_task<bool>>& ctx)
  {
    CORO_BEGIN(ctx);

    logger.info("ue={}: \"{}\" started...", ue_index, name());

    // Extract the embedded RRC Reconfiguration from the target-to-source transparent container.
    if (not extract_rrc_container()) {
      logger.warning("ue={}: \"{}\" failed: invalid HandoverCommand container", ue_index, name());
      CORO_EARLY_RETURN(false);
    }

    // Locate the source UE and the F1AP handler of its DU.
    source_ue = ue_mng.find_du_ue(ue_index);
    if (source_ue == nullptr) {
      logger.warning("ue={}: \"{}\" failed: UE not found", ue_index, name());
      CORO_EARLY_RETURN(false);
    }

    // Build F1AP UE Context Modification with the RRC Reconfiguration container. The DU will forward
    // it to the UE on the air interface, triggering the handover execution.
    {
      f1ap_ue_context_modification_request req;
      req.ue_index      = ue_index;
      req.rrc_container = rrc_container.copy();
      f1ap_request      = std::move(req);
    }

    CORO_AWAIT_VALUE(f1ap_response,
                     du_db.get_du_processor(source_ue->get_du_index())
                         .get_f1ap_handler()
                         .handle_ue_context_modification_request(f1ap_request));

    if (not f1ap_response.success) {
      logger.warning("ue={}: \"{}\" failed: F1AP UE Context Modification rejected by source DU", ue_index, name());
      CORO_EARLY_RETURN(false);
    }

    // Send UL RAN Status Transfer to the AMF. The MVP carries an empty DRB list since no PDCP SN
    // synchronization is performed.
    {
      ngap_ul_ran_status_transfer status_transfer;
      status_transfer.ue_index = ue_index;
      ngap.handle_ul_ran_status_transfer(status_transfer);
    }

    logger.info("ue={}: \"{}\" finished successfully", ue_index, name());
    // Note: The AMF will subsequently send a UEContextReleaseCommand which clears the source UE
    // context. We deliberately do not trigger the release locally: doing so could race the AMF
    // signalling and remove the UE before the target side completes the HandoverNotify.
    CORO_RETURN(true);
  }

  static const char* name() { return "Inter-CU Handover Source Routine"; }

private:
  bool extract_rrc_container()
  {
    asn1::cbit_ref bref(command);
    asn1::ngap::target_ngran_node_to_source_ngran_node_transparent_container_s container;
    if (container.unpack(bref) != asn1::SRSASN_SUCCESS) {
      return false;
    }
    rrc_container = container.rrc_container.copy();
    return not rrc_container.empty();
  }

  ue_index_t                    ue_index;
  byte_buffer                   command;
  ue_manager&                   ue_mng;
  du_processor_repository&      du_db;
  cu_up_processor_repository&   cu_up_db;
  ngap_control_message_handler& ngap;
  srslog::basic_logger&         logger;

  byte_buffer                          rrc_container;
  cu_cp_ue*                            source_ue = nullptr;
  f1ap_ue_context_modification_request f1ap_request;
  f1ap_ue_context_modification_response f1ap_response;
};

} // namespace

async_task<bool> srsran::srs_cu_cp::start_inter_cu_handover_source_routine(ue_index_t                    ue_index,
                                                                           byte_buffer                   command,
                                                                           ue_manager&                   ue_mng,
                                                                           du_processor_repository&      du_db,
                                                                           cu_up_processor_repository&   cu_up_db,
                                                                           ngap_control_message_handler& ngap,
                                                                           srslog::basic_logger&         logger)
{
  return launch_async<inter_cu_handover_source_routine_impl>(
      ue_index, std::move(command), ue_mng, du_db, cu_up_db, ngap, logger);
}
