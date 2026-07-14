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

#include "du_mac_si_pdu_update_procedure.h"
#include "../du_cell_manager.h"
#include "srsran/mac/mac_cell_manager.h"
#include "srsran/srslog/srslog.h"

using namespace srsran;
using namespace srs_du;

#ifndef SRSRAN_HAS_ENTERPRISE_NTN

async_task<du_si_pdu_update_response> srsran::srs_du::start_du_mac_si_pdu_update(const du_si_pdu_update_request& req,
                                                                                 const du_manager_params&        params,
                                                                                 du_cell_manager& cell_mng)
{
  srslog::basic_logger& logger = srslog::fetch_basic_logger("DU-MNG");

  return launch_async([&req,
                       &params,
                       &cell_mng,
                       &logger,
                       cell_index = INVALID_DU_CELL_INDEX,
                       mac_req    = mac_cell_reconfig_request{},
                       mac_resp   = mac_cell_reconfig_response{}](
                          coro_context<async_task<du_si_pdu_update_response>>& ctx) mutable {
    CORO_BEGIN(ctx);

    cell_index = cell_mng.get_cell_index(req.nr_cgi);
    if (cell_index == INVALID_DU_CELL_INDEX) {
      logger.warning("Discarding SI PDU update. Cause: No DU cell with NR-CGI={} was found", req.nr_cgi.nci);
      CORO_EARLY_RETURN(du_si_pdu_update_response{false});
    }

    if (not cell_mng.is_cell_active(cell_index)) {
      logger.warning("Discarding SI PDU update for cell={}. Cause: Cell is not active", fmt::underlying(cell_index));
      CORO_EARLY_RETURN(du_si_pdu_update_response{false});
    }

    mac_req.new_si_pdu_info.emplace();
    mac_req.new_si_pdu_info->si_msg_idx     = req.si_msg_idx;
    mac_req.new_si_pdu_info->sib_idx        = static_cast<uint8_t>(req.sib_idx);
    mac_req.new_si_pdu_info->slot           = req.slot;
    mac_req.new_si_pdu_info->si_slot_period = req.si_slot_period;
    mac_req.new_si_pdu_info->clear          = req.clear;
    mac_req.new_si_pdu_info->si_messages    = req.si_messages;

    CORO_AWAIT_VALUE(mac_resp, params.mac.mgr.get_cell_manager().get_cell_controller(cell_index).reconfigure(mac_req));

    CORO_RETURN(du_si_pdu_update_response{mac_resp.si_pdus_enqueued});
  });
}

#endif // SRSRAN_HAS_ENTERPRISE_NTN
