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

#include "f1ap_du_gnbdu_resource_coordination_procedure.h"
#include "srsran/asn1/f1ap/common.h"
#include "srsran/f1ap/f1ap_message.h"
#include "srsran/f1ap/f1ap_message_notifier.h"

using namespace srsran;
using namespace srsran::srs_du;

f1ap_du_gnbdu_resource_coordination_procedure::f1ap_du_gnbdu_resource_coordination_procedure(
    const asn1::f1ap::gnb_du_res_coordination_request_s& msg_,
    f1ap_du_configurator&                                du_mng_,
    f1ap_message_notifier&                               cu_notifier_) :
  request(msg_), du_mng(du_mng_), cu_notifier(cu_notifier_)
{
}

void f1ap_du_gnbdu_resource_coordination_procedure::operator()(coro_context<async_task<void>>& ctx)
{
  CORO_BEGIN(ctx);

  position_query_container_seen =
      is_f1ap_ntn_initial_ul_position_query_container(request->eutra_nr_cell_res_coordination_req_container);
  if (position_query_container_seen) {
    position_query =
        decode_f1ap_ntn_initial_ul_position_query(request->eutra_nr_cell_res_coordination_req_container);
    if (position_query.has_value()) {
      CORO_AWAIT_VALUE(position_result, du_mng.request_ntn_initial_ul_position(position_query.value()));
    }
  } else if ((calendar_container_seen =
                  is_f1ap_ntn_access_calendar_update_container(request->eutra_nr_cell_res_coordination_req_container))) {
    calendar_update = decode_f1ap_ntn_access_calendar_update(request->eutra_nr_cell_res_coordination_req_container);
    if (calendar_update.has_value()) {
      CORO_AWAIT_VALUE(calendar_result, du_mng.request_ntn_access_calendar_update(calendar_update.value()));
    } else {
      calendar_result.status        = f1ap_ntn_access_calendar_result_status::rejected;
      calendar_result.reject_reason = "malformed_ntn_access_calendar_update";
    }
  } else if ((audit_request =
                  decode_f1ap_ntn_resource_audit_request(request->eutra_nr_cell_res_coordination_req_container))
                 .has_value()) {
    CORO_AWAIT_VALUE(audit_result, du_mng.request_ntn_resource_audit(audit_request.value()));
  } else if ((sib19_update = decode_f1ap_ntn_sib19_broadcast_update(request->eutra_nr_cell_res_coordination_req_container))
                 .has_value()) {
    CORO_AWAIT_VALUE(sib19_result, du_mng.request_ntn_sib19_broadcast_update(sib19_update.value()));
  } else if ((update = decode_f1ap_ntn_rnti_lease_pool_update(request->eutra_nr_cell_res_coordination_req_container))
                 .has_value()) {
    CORO_AWAIT_VALUE(result, du_mng.request_ntn_rnti_lease_pool_update(update.value()));
  } else {
    result.accepted      = false;
    result.reject_reason = "malformed_ntn_rnti_lease_pool_update";
  }

  send_response();

  CORO_RETURN();
}

void f1ap_du_gnbdu_resource_coordination_procedure::send_response()
{
  f1ap_message msg;
  msg.pdu.set_successful_outcome().load_info_obj(ASN1_F1AP_ID_GNB_DU_RES_COORDINATION);
  auto& resp = msg.pdu.successful_outcome().value.gnb_du_res_coordination_resp();

  resp->transaction_id = request->transaction_id;
  resp->eutra_nr_cell_res_coordination_req_ack_container =
      position_query_container_seen ? encode_f1ap_ntn_initial_ul_position_result(position_result)
      : calendar_container_seen ? encode_f1ap_ntn_access_calendar_result(calendar_result)
      : audit_request.has_value() ? encode_f1ap_ntn_resource_audit_result(audit_result)
      : sib19_update.has_value() ? encode_f1ap_ntn_sib19_broadcast_result(sib19_result)
                                : encode_f1ap_ntn_rnti_lease_pool_result(result);

  cu_notifier.on_new_message(msg);
}
