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

#include "gnb_du_resource_coordination_procedure.h"
#include "f1ap_asn1_utils.h"
#include "srsran/asn1/f1ap/common.h"
#include "srsran/asn1/f1ap/f1ap_pdu_contents.h"
#include "srsran/f1ap/f1ap_message.h"

using namespace srsran;
using namespace srsran::srs_cu_cp;

gnb_du_resource_coordination_procedure::gnb_du_resource_coordination_procedure(
    const f1ap_configuration&                        f1ap_cfg_,
    const f1ap_gnb_du_resource_coordination_request& request_,
    f1ap_message_notifier&                           f1ap_notifier_,
    f1ap_event_manager&                              ev_mng_,
    srslog::basic_logger&                            logger_) :
  f1ap_cfg(f1ap_cfg_), request(request_), f1ap_notifier(f1ap_notifier_), ev_mng(ev_mng_), logger(logger_)
{
}

void gnb_du_resource_coordination_procedure::operator()(
    coro_context<async_task<f1ap_gnb_du_resource_coordination_response>>& ctx)
{
  CORO_BEGIN(ctx);

  logger.debug("\"{}\": started...", name());

  transaction = ev_mng.transactions.create_transaction(f1ap_cfg.proc_timeout);

  send_gnb_du_resource_coordination_request();

  CORO_AWAIT(transaction);

  CORO_RETURN(handle_procedure_result());
}

void gnb_du_resource_coordination_procedure::send_gnb_du_resource_coordination_request()
{
  f1ap_message msg;
  msg.pdu.set_init_msg().load_info_obj(ASN1_F1AP_ID_GNB_DU_RES_COORDINATION);
  auto& req = msg.pdu.init_msg().value.gnb_du_res_coordination_request();

  req->transaction_id = transaction.id();
  req->request_type.value = asn1::f1ap::request_type_opts::execution;
  if (request.ntn_access_calendar_update.has_value()) {
    req->eutra_nr_cell_res_coordination_req_container =
        encode_f1ap_ntn_access_calendar_update(request.ntn_access_calendar_update.value());
  } else if (request.ntn_resource_audit_request.du_index != du_index_t::invalid) {
    req->eutra_nr_cell_res_coordination_req_container =
        encode_f1ap_ntn_resource_audit_request(request.ntn_resource_audit_request);
  } else if (request.ntn_sib19_broadcast_update.operation != f1ap_ntn_sib19_broadcast_operation::invalid) {
    req->eutra_nr_cell_res_coordination_req_container =
        encode_f1ap_ntn_sib19_broadcast_update(request.ntn_sib19_broadcast_update);
  } else {
    req->eutra_nr_cell_res_coordination_req_container =
        encode_f1ap_ntn_rnti_lease_pool_update(request.ntn_rnti_lease_update);
  }

  f1ap_notifier.on_new_message(msg);
}

f1ap_gnb_du_resource_coordination_response gnb_du_resource_coordination_procedure::handle_procedure_result()
{
  f1ap_gnb_du_resource_coordination_response response;

  if (!transaction.valid()) {
    logger.debug("\"{}\" cancelled. Cause: Failed to allocate transaction", name());
    return response;
  }
  if (transaction.aborted()) {
    logger.debug("\"{}\" cancelled. Cause: Timeout reached", name());
    return response;
  }
  if (!transaction.has_response() || !transaction.response().has_value()) {
    return response;
  }

  const auto& asn1_resp = transaction.response().value().value.gnb_du_res_coordination_resp();
  response.calendar_result =
      decode_f1ap_ntn_access_calendar_result(asn1_resp->eutra_nr_cell_res_coordination_req_ack_container);
  response.result       = decode_f1ap_ntn_rnti_lease_pool_result(asn1_resp->eutra_nr_cell_res_coordination_req_ack_container);
  response.audit_result = decode_f1ap_ntn_resource_audit_result(asn1_resp->eutra_nr_cell_res_coordination_req_ack_container);
  response.sib19_result =
      decode_f1ap_ntn_sib19_broadcast_result(asn1_resp->eutra_nr_cell_res_coordination_req_ack_container);
  response.success      = (response.calendar_result.has_value() && response.calendar_result->accepted()) ||
                     (response.result.has_value() && response.result->accepted) ||
                     (response.audit_result.has_value() && response.audit_result->accepted) ||
                     (response.sib19_result.has_value() && response.sib19_result->accepted());
  if (!response.calendar_result.has_value() && !response.result.has_value() && !response.audit_result.has_value() &&
      !response.sib19_result.has_value()) {
    logger.warning("\"{}\": DU response carried an invalid NTN resource coordination ack container", name());
  } else if (response.calendar_result.has_value()) {
    logger.debug("\"{}\": access calendar finished with schedule_version={} accepted={}",
                 name(),
                 response.calendar_result->schedule_version,
                 response.calendar_result->accepted());
  } else if (response.audit_result.has_value()) {
    logger.debug("\"{}\": audit finished with generation={} accepted={}",
                 name(),
                 response.audit_result->generation_id,
                 response.audit_result->accepted);
  } else if (response.sib19_result.has_value()) {
    logger.debug("\"{}\": SIB19 broadcast finished with generation={} accepted={}",
                 name(),
                 response.sib19_result->generation_id,
                 response.sib19_result->accepted());
  } else {
    logger.debug("\"{}\": finished with generation={} accepted={}",
                 name(),
                 response.result->generation_id,
                 response.result->accepted);
  }

  return response;
}
