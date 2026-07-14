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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * A copy of the GNU Affero General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */

#pragma once

#include "cu_cp/procedures/f1ap_cu_event_manager.h"
#include "srsran/f1ap/cu_cp/f1ap_configuration.h"
#include "srsran/f1ap/cu_cp/f1ap_cu_resource_coordination.h"
#include "srsran/f1ap/f1ap_message_notifier.h"
#include "srsran/support/async/async_task.h"

namespace srsran::srs_cu_cp {

class gnb_du_resource_coordination_procedure
{
public:
  gnb_du_resource_coordination_procedure(const f1ap_configuration&                      f1ap_cfg_,
                                         const f1ap_gnb_du_resource_coordination_request& request_,
                                         f1ap_message_notifier&                         f1ap_notifier_,
                                         f1ap_event_manager&                            ev_mng_,
                                         srslog::basic_logger&                          logger_);

  void operator()(coro_context<async_task<f1ap_gnb_du_resource_coordination_response>>& ctx);

  static const char* name() { return "GNB DU Resource Coordination Procedure"; }

private:
  void send_gnb_du_resource_coordination_request();

  f1ap_gnb_du_resource_coordination_response handle_procedure_result();

  const f1ap_configuration&                       f1ap_cfg;
  const f1ap_gnb_du_resource_coordination_request request;
  f1ap_message_notifier&                          f1ap_notifier;
  f1ap_event_manager&                             ev_mng;
  srslog::basic_logger&                           logger;

  f1ap_transaction transaction;
};

} // namespace srsran::srs_cu_cp
