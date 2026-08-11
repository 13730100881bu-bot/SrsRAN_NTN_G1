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

#include "srsran/asn1/f1ap/f1ap_pdu_contents.h"
#include "srsran/f1ap/du/f1ap_du.h"
#include "srsran/support/async/async_task.h"
#include <optional>

namespace srsran {

class f1ap_message_notifier;

namespace srs_du {

class f1ap_du_gnbdu_resource_coordination_procedure
{
public:
  f1ap_du_gnbdu_resource_coordination_procedure(const asn1::f1ap::gnb_du_res_coordination_request_s& msg_,
                                                f1ap_du_configurator&                                du_mng_,
                                                f1ap_message_notifier&                               cu_notifier_);

  void operator()(coro_context<async_task<void>>& ctx);

private:
  void send_response();

  const asn1::f1ap::gnb_du_res_coordination_request_s request;
  f1ap_du_configurator&                               du_mng;
  f1ap_message_notifier&                              cu_notifier;

  std::optional<f1ap_ntn_rnti_lease_pool_update>   update;
  std::optional<f1ap_ntn_initial_ul_position_query> position_query;
  std::optional<f1ap_ntn_resource_audit_request>   audit_request;
  std::optional<f1ap_ntn_sib19_broadcast_update>   sib19_update;
  std::optional<f1ap_ntn_access_calendar_update>    calendar_update;
  bool                                              calendar_container_seen       = false;
  bool                                              position_query_container_seen = false;
  f1ap_ntn_rnti_lease_pool_result                   result;
  f1ap_ntn_initial_ul_position_result                position_result;
  f1ap_ntn_resource_audit_result                     audit_result;
  f1ap_ntn_sib19_broadcast_result                    sib19_result;
  f1ap_ntn_access_calendar_result                    calendar_result;
};

} // namespace srs_du
} // namespace srsran
