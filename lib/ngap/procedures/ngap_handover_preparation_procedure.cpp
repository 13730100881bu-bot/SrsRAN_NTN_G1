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

#include "ngap_handover_preparation_procedure.h"
#include "../ngap_asn1_converters.h"
#include "srsran/asn1/ngap/common.h"
#include "srsran/asn1/ngap/ngap_pdu_contents.h"
#include "srsran/ngap/ngap_message.h"
#include "srsran/support/async/coroutine.h"

using namespace srsran;
using namespace srsran::srs_cu_cp;
using namespace asn1::ngap;

namespace {

/// 3GPP TS 38.413: HandoverPreparation guard timer. Spec leaves the value to implementation; 5 s is conservative
/// enough to allow the AMF round-trip and the target gNB resource allocation to complete.
constexpr std::chrono::milliseconds handover_preparation_timeout{5000};

/// Dispatch the HandoverCommand to CU-CP. Defined outside the procedure coroutine to keep its CORO macros
/// in a separate function body (the macros define a `coro_context__` variable that would shadow the outer
/// procedure's one if nested).
static async_task<void>
dispatch_ho_command(ngap_cu_cp_notifier& handler, ue_index_t ue_index, byte_buffer container)
{
  return launch_async(
      [&handler, ue_index, container = std::move(container)](coro_context<async_task<void>>& ctx) mutable {
        CORO_BEGIN(ctx);
        CORO_AWAIT(handler.on_new_handover_command(ue_index, std::move(container)));
        CORO_RETURN();
      });
}

/// Source-side N2 HandoverPreparation procedure (TS 38.413 §8.4.1).
class ngap_handover_preparation_procedure_impl
{
public:
  ngap_handover_preparation_procedure_impl(const ngap_handover_preparation_request& req_,
                                           const plmn_identity&                     serving_plmn_,
                                           const ngap_ue_ids&                       ue_ids_,
                                           ngap_message_notifier&                   amf_notifier_,
                                           ngap_rrc_ue_notifier&                    rrc_ue_notifier_,
                                           ngap_cu_cp_notifier&                     cu_cp_notifier_,
                                           ngap_ue_transaction_manager&             ev_mng_,
                                           timer_factory                            timers_,
                                           ngap_ue_logger&                          logger_) :
    req(req_),
    serving_plmn_bytes(serving_plmn_.to_bytes()),
    ue_ids(ue_ids_),
    amf_notifier(amf_notifier_),
    rrc_ue_notifier(rrc_ue_notifier_),
    cu_cp_notifier(cu_cp_notifier_),
    ev_mng(ev_mng_),
    timers(timers_),
    logger(logger_)
  {
  }

  void operator()(coro_context<async_task<ngap_handover_preparation_response>>& ctx)
  {
    CORO_BEGIN(ctx);

    logger.log_info("\"{}\" started...", name());

    // Build and send HandoverRequired to AMF.
    if (not send_handover_required()) {
      logger.log_warning("\"{}\" failed: could not send HandoverRequired", name());
      CORO_EARLY_RETURN(ngap_handover_preparation_response{false});
    }

    // Notify CU-CP for metrics.
    cu_cp_notifier.on_transmission_of_handover_required();

    // Subscribe to HandoverCommand / HandoverPreparationFailure event.
    transaction.subscribe_to(ev_mng.handover_preparation_outcome, handover_preparation_timeout);

    // Await AMF response.
    CORO_AWAIT(transaction);

    if (transaction.successful()) {
      // Forward the target-to-source transparent container (carrying the RRC Reconfiguration with
      // mobilityControlInfo) to CU-CP so it can drive F1AP UE Context Modification on the source DU.
      cu_cp_notifier.schedule_async_task(
          req.ue_index,
          dispatch_ho_command(
              cu_cp_notifier, req.ue_index, transaction.response()->target_to_source_transparent_container.copy()));

      logger.log_info("\"{}\" finished successfully", name());
      CORO_EARLY_RETURN(ngap_handover_preparation_response{true});
    }

    if (transaction.failed()) {
      logger.log_info("\"{}\" failed: HandoverPreparationFailure received from AMF", name());
    } else if (transaction.timeout_expired()) {
      logger.log_warning("\"{}\" failed: timed out after {}ms", name(), handover_preparation_timeout.count());
    } else {
      logger.log_warning("\"{}\" failed: transaction was cancelled", name());
    }

    CORO_RETURN(ngap_handover_preparation_response{false});
  }

  static const char* name() { return "NG Handover Preparation Procedure"; }

private:
  bool send_handover_required()
  {
    // Get the packed HandoverPreparationInfo RRC container from the source RRC UE.
    byte_buffer rrc_container = rrc_ue_notifier.on_handover_preparation_message_required();
    if (rrc_container.empty()) {
      logger.log_warning("Cannot send HandoverRequired: empty RRC container");
      return false;
    }

    ngap_message ngap_msg = {};
    ngap_msg.pdu.set_init_msg();
    ngap_msg.pdu.init_msg().load_info_obj(ASN1_NGAP_ID_HO_PREP);

    auto& ho_required           = ngap_msg.pdu.init_msg().value.ho_required();
    ho_required->amf_ue_ngap_id = amf_ue_id_to_uint(ue_ids.amf_ue_id);
    ho_required->ran_ue_ngap_id = ran_ue_id_to_uint(ue_ids.ran_ue_id);

    // Handover type: only intra-5GS is supported in this MVP.
    ho_required->handov_type.value = handov_type_opts::intra5gs;

    // Cause: handover desirable for radio reasons (TS 38.413).
    ho_required->cause.set_radio_network();
    ho_required->cause.radio_network().value = cause_radio_network_opts::ho_desirable_for_radio_reason;

    // Target ID: target gNB, identified by the gNB-ID in the request.
    auto& target_ran_node = ho_required->target_id.set_target_ran_node_id();
    auto& global_gnb      = target_ran_node.global_ran_node_id.set_global_gnb_id();
    global_gnb.plmn_id    = serving_plmn_bytes;
    global_gnb.gnb_id.set_gnb_id().from_number(req.gnb_id.id, req.gnb_id.bit_length);

    // Selected TAI is part of the target_ran_node_id; left at default (zero TAC) in this MVP.
    target_ran_node.sel_tai.tac.from_number(0);
    target_ran_node.sel_tai.plmn_id = global_gnb.plmn_id;

    // PDU Session list: at least one entry is mandatory per ASN.1.
    for (const auto& ps : req.pdu_sessions) {
      pdu_session_res_item_ho_rqd_s asn1_item;
      asn1_item.pdu_session_id = pdu_session_id_to_uint(ps.first);
      // ho_required_transfer is opaque per session (per-session container). Empty payload acceptable for MVP.
      ho_required_transfer_s ho_transfer;
      asn1_item.ho_required_transfer = pack_into_pdu(ho_transfer, "HandoverRequiredTransfer");
      ho_required->pdu_session_res_list_ho_rqd.push_back(asn1_item);
    }

    // Source-to-target transparent container.
    source_ngran_node_to_target_ngran_node_transparent_container_s container = {};
    container.target_cell_id.set_nr_cgi();
    container.target_cell_id.nr_cgi().plmn_id = global_gnb.plmn_id;
    container.target_cell_id.nr_cgi().nr_cell_id.from_number(req.nci.value());
    container.rrc_container = std::move(rrc_container);

    ho_required->source_to_target_transparent_container = pack_into_pdu(container, "SourceToTargetTransparentContainer");
    if (ho_required->source_to_target_transparent_container.empty()) {
      logger.log_warning("Cannot send HandoverRequired: failed to pack source-to-target transparent container");
      return false;
    }

    if (not amf_notifier.on_new_message(ngap_msg)) {
      logger.log_warning("AMF notifier is not set. Cannot send HandoverRequired");
      return false;
    }

    logger.log_info("HandoverRequired sent to AMF");
    return true;
  }

  const ngap_handover_preparation_request& req;
  const std::array<uint8_t, 3>             serving_plmn_bytes;
  const ngap_ue_ids&                       ue_ids;
  ngap_message_notifier&                   amf_notifier;
  ngap_rrc_ue_notifier&                    rrc_ue_notifier;
  ngap_cu_cp_notifier&                     cu_cp_notifier;
  ngap_ue_transaction_manager&             ev_mng;
  timer_factory                            timers;
  ngap_ue_logger&                          logger;

  protocol_transaction_outcome_observer<asn1::ngap::ho_cmd_s, asn1::ngap::ho_prep_fail_s> transaction;
};

} // namespace

async_task<ngap_handover_preparation_response>
srsran::srs_cu_cp::start_ngap_handover_preparation(const ngap_handover_preparation_request& req,
                                                   const plmn_identity&                     serving_plmn,
                                                   const ngap_ue_ids&                       ue_ids,
                                                   ngap_message_notifier&                   amf_notifier,
                                                   ngap_rrc_ue_notifier&                    rrc_ue_notifier,
                                                   ngap_cu_cp_notifier&                     cu_cp_notifier,
                                                   ngap_ue_transaction_manager&             ev_mng,
                                                   timer_factory                            timers,
                                                   ngap_ue_logger&                          logger)
{
  return launch_async<ngap_handover_preparation_procedure_impl>(
      req, serving_plmn, ue_ids, amf_notifier, rrc_ue_notifier, cu_cp_notifier, ev_mng, timers, logger);
}
