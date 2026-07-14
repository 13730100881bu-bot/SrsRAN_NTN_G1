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

#include "inter_cu_handover_execution_target_routine.h"
#include "srsran/e1ap/cu_cp/e1ap_cu_cp.h"
#include "srsran/ngap/ngap.h"

using namespace srsran;
using namespace srs_cu_cp;

namespace {

class inter_cu_handover_execution_target_routine_impl
{
public:
  inter_cu_handover_execution_target_routine_impl(cu_cp_ue*                    ue_,
                                                  e1ap_bearer_context_manager& e1ap_,
                                                  ngap_interface&              ngap_,
                                                  srslog::basic_logger&        logger_) :
    ue(ue_), e1ap(e1ap_), ngap(ngap_), logger(logger_)
  {
    (void)e1ap; // MVP: PDCP SN status sync is not propagated, so no E1AP modify is required.
  }

  void operator()(coro_context<async_task<void>>& ctx)
  {
    CORO_BEGIN(ctx);

    if (ue == nullptr) {
      logger.warning("\"{}\" failed: UE not found", name());
      CORO_EARLY_RETURN();
    }

    logger.info("ue={}: \"{}\" started...", ue->get_ue_index(), name());

    // Await DLRANStatusTransfer from AMF. The MVP tolerates a timeout: when the source side does
    // not perform PDCP SN synchronization, the AMF may not send this message at all.
    CORO_AWAIT_VALUE(dl_status,
                     ngap.get_ngap_control_message_handler().handle_dl_ran_status_transfer_required(ue->get_ue_index()));

    if (not dl_status.has_value()) {
      logger.info("ue={}: continuing handover execution without DLRANStatusTransfer", ue->get_ue_index());
    }

    // Send HandoverNotify to the AMF. The CGI/TAC carry the location of the UE in the target cell;
    // for the MVP we use a default-constructed CGI/TAC since the cell context lookup at this layer
    // requires extra plumbing. Production deployments should populate these from the target cell.
    {
      nr_cell_global_id_t cgi;
      cgi.plmn_id = ue->get_ue_context().plmn;
      tac_t tac   = 0;
      ngap.get_ngap_control_message_handler().handle_inter_cu_ho_rrc_recfg_complete(ue->get_ue_index(), cgi, tac);
    }

    logger.info("ue={}: \"{}\" finished successfully", ue->get_ue_index(), name());
    CORO_RETURN();
  }

  static const char* name() { return "Inter-CU Handover Execution Target Routine"; }

private:
  cu_cp_ue*                    ue;
  e1ap_bearer_context_manager& e1ap;
  ngap_interface&              ngap;
  srslog::basic_logger&        logger;

  expected<ngap_dl_ran_status_transfer> dl_status;
};

} // namespace

async_task<void> srsran::srs_cu_cp::start_inter_cu_handover_execution_target_routine(cu_cp_ue* ue,
                                                                                     e1ap_bearer_context_manager& e1ap,
                                                                                     ngap_interface&              ngap,
                                                                                     srslog::basic_logger&        logger)
{
  return launch_async<inter_cu_handover_execution_target_routine_impl>(ue, e1ap, ngap, logger);
}
