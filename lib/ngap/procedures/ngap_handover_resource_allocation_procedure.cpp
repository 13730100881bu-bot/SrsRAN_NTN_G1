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

#include "ngap_handover_resource_allocation_procedure.h"
#include "../ngap_asn1_helpers.h"
#include "srsran/asn1/ngap/common.h"
#include "srsran/ngap/ngap_message.h"

using namespace srsran;
using namespace srsran::srs_cu_cp;
using namespace asn1::ngap;

namespace {

class ngap_handover_resource_allocation_procedure_impl
{
public:
  ngap_handover_resource_allocation_procedure_impl(const ngap_handover_request& request_,
                                                   amf_ue_id_t                  amf_ue_id_,
                                                   ngap_ue_context_list&        ue_ctxt_list_,
                                                   ngap_cu_cp_notifier&         cu_cp_notifier_,
                                                   ngap_message_notifier&       amf_notifier_,
                                                   srslog::basic_logger&        logger_) :
    request(request_),
    amf_ue_id(amf_ue_id_),
    ue_ctxt_list(ue_ctxt_list_),
    cu_cp_notifier(cu_cp_notifier_),
    amf_notifier(amf_notifier_),
    logger(logger_)
  {
  }

  void operator()(coro_context<async_task<void>>& ctx)
  {
    CORO_BEGIN(ctx);

    logger.info("ue={}: \"{}\" started...", request.ue_index, name());

    // Hand off to the CU-CP target routine that drives E1AP Bearer Setup + F1AP UE Context Setup.
    CORO_AWAIT_VALUE(response, cu_cp_notifier.on_ngap_handover_request(request));

    if (not send_response()) {
      logger.warning("ue={}: \"{}\" failed: could not forward response to AMF", request.ue_index, name());
    } else {
      logger.info(
          "ue={}: \"{}\" finished {}", request.ue_index, name(), response.success ? "successfully" : "with failure");
    }

    CORO_RETURN();
  }

  static const char* name() { return "NG Handover Resource Allocation Procedure"; }

private:
  bool send_response()
  {
    ngap_message ngap_msg = {};

    if (response.success) {
      ngap_msg.pdu.set_successful_outcome();
      ngap_msg.pdu.successful_outcome().load_info_obj(ASN1_NGAP_ID_HO_RES_ALLOC);

      auto& ho_request_ack           = ngap_msg.pdu.successful_outcome().value.ho_request_ack();
      ho_request_ack->amf_ue_ngap_id = amf_ue_id_to_uint(amf_ue_id);

      // Find the new UE in the NGAP UE context list to obtain the assigned RAN-UE-NGAP-ID.
      if (ue_ctxt_list.contains(response.ue_index)) {
        ho_request_ack->ran_ue_ngap_id = ran_ue_id_to_uint(ue_ctxt_list[response.ue_index].ue_ids.ran_ue_id);
      }

      if (not fill_asn1_handover_resource_allocation_response(ho_request_ack, response)) {
        logger.warning("ue={}: Failed to pack HandoverRequestAcknowledge", response.ue_index);
        return send_failure();
      }
    } else {
      return send_failure();
    }

    if (not amf_notifier.on_new_message(ngap_msg)) {
      logger.warning("AMF notifier is not set. Cannot send HandoverRequestAcknowledge");
      return false;
    }

    return true;
  }

  bool send_failure()
  {
    ngap_message ngap_msg = {};
    ngap_msg.pdu.set_unsuccessful_outcome();
    ngap_msg.pdu.unsuccessful_outcome().load_info_obj(ASN1_NGAP_ID_HO_RES_ALLOC);

    auto& ho_fail           = ngap_msg.pdu.unsuccessful_outcome().value.ho_fail();
    ho_fail->amf_ue_ngap_id = amf_ue_id_to_uint(amf_ue_id);

    // If we never got a meaningful cause from the routine, default to a protocol cause.
    if (response.success) {
      ho_fail->cause.set_protocol();
    } else {
      // Best-effort fill; if helper rejects (e.g. response.success false but no cause set), fall back.
      if (not fill_asn1_handover_resource_allocation_response(ho_fail, response)) {
        ho_fail->cause.set_protocol();
      }
    }

    return amf_notifier.on_new_message(ngap_msg);
  }

  const ngap_handover_request& request;
  amf_ue_id_t                  amf_ue_id;
  ngap_ue_context_list&        ue_ctxt_list;
  ngap_cu_cp_notifier&         cu_cp_notifier;
  ngap_message_notifier&       amf_notifier;
  srslog::basic_logger&        logger;

  ngap_handover_resource_allocation_response response;
};

} // namespace

async_task<void> srsran::srs_cu_cp::start_ngap_handover_resource_allocation(const ngap_handover_request& request,
                                                                            const amf_ue_id_t            amf_ue_id,
                                                                            ngap_ue_context_list&        ue_ctxt_list,
                                                                            ngap_cu_cp_notifier&         cu_cp_notifier,
                                                                            ngap_message_notifier&       amf_notifier,
                                                                            srslog::basic_logger&        logger)
{
  return launch_async<ngap_handover_resource_allocation_procedure_impl>(
      request, amf_ue_id, ue_ctxt_list, cu_cp_notifier, amf_notifier, logger);
}
