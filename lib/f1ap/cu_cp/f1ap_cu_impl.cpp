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

#include "f1ap_cu_impl.h"
#include "asn1_helpers.h"
#include "f1ap_asn1_helpers.h"
#include "f1ap_asn1_utils.h"
#include "log_helpers.h"
#include "procedures/f1_removal_procedure.h"
#include "procedures/f1_setup_procedure.h"
#include "procedures/f1ap_stop_procedure.h"
#include "procedures/gnb_cu_configuration_update_procedure.h"
#include "procedures/gnb_du_resource_coordination_procedure.h"
#include "procedures/ue_context_modification_procedure.h"
#include "procedures/ue_context_release_procedure.h"
#include "procedures/ue_context_setup_procedure.h"
#include "srsran/asn1/f1ap/f1ap.h"
#include "srsran/cu_cp/cu_cp_types.h"
#include "srsran/f1ap/f1ap_message.h"
#include "srsran/ran/positioning/measurement_information.h"

using namespace srsran;
using namespace srs_cu_cp;

f1ap_cu_impl::f1ap_cu_impl(const f1ap_configuration&   f1ap_cfg_,
                           f1ap_message_notifier&      tx_pdu_notifier_,
                           f1ap_du_processor_notifier& f1ap_du_processor_notifier_,
                           timer_manager&              timers_,
                           task_executor&              ctrl_exec_) :
  cfg(f1ap_cfg_),
  logger(srslog::fetch_basic_logger("CU-CP-F1")),
  ue_ctxt_list(timer_factory{timers_, ctrl_exec_}, logger),
  du_processor_notifier(f1ap_du_processor_notifier_),
  ctrl_exec(ctrl_exec_),
  tx_pdu_notifier(*this, tx_pdu_notifier_),
  ev_mng(timer_factory{timers_, ctrl_exec_})
{
}

// Note: For fwd declaration of member types, dtor cannot be trivial.
f1ap_cu_impl::~f1ap_cu_impl() {}

async_task<void> f1ap_cu_impl::stop()
{
  return launch_async<f1ap_stop_procedure>(du_processor_notifier, ue_ctxt_list);
}

const f1ap_du_context& f1ap_cu_impl::get_context() const
{
  return du_ctxt;
}

void f1ap_cu_impl::handle_dl_rrc_message_transfer(const f1ap_dl_rrc_message& msg)
{
  f1ap_ue_context* ue_ctxt = ue_ctxt_list.find(msg.ue_index);
  if (ue_ctxt == nullptr) {
    logger.warning("ue={}: Dropping \"DLRRCMessageTransfer\". UE context does not exist", msg.ue_index);
    return;
  }

  ue_ctxt->handle_dl_rrc_message(msg, tx_pdu_notifier);
}

async_task<f1ap_ue_context_setup_response>
f1ap_cu_impl::handle_ue_context_setup_request(const f1ap_ue_context_setup_request&          request,
                                              const std::optional<rrc_ue_transfer_context>& rrc_context)
{
  return launch_async<ue_context_setup_procedure>(
      cfg, request, ue_ctxt_list, du_processor_notifier, tx_pdu_notifier, logger, rrc_context);
}

async_task<ue_index_t> f1ap_cu_impl::handle_ue_context_release_command(const f1ap_ue_context_release_command& msg)
{
  if (!ue_ctxt_list.contains(msg.ue_index)) {
    logger.warning("ue={}: Dropping \"UEContextReleaseCommand\". Cause: UE context does not exist", msg.ue_index);

    return launch_async([](coro_context<async_task<ue_index_t>>& ctx) mutable {
      CORO_BEGIN(ctx);
      CORO_RETURN(ue_index_t::invalid);
    });
  }

  return launch_async<ue_context_release_procedure>(cfg, msg, ue_ctxt_list[msg.ue_index], tx_pdu_notifier);
}

async_task<f1ap_ue_context_modification_response>
f1ap_cu_impl::handle_ue_context_modification_request(const f1ap_ue_context_modification_request& request)
{
  if (!ue_ctxt_list.contains(request.ue_index)) {
    logger.warning("ue={}: Dropping \"UEContextModificationRequest\". UE context does not exist", request.ue_index);

    return launch_async([](coro_context<async_task<f1ap_ue_context_modification_response>>& ctx) mutable {
      CORO_BEGIN(ctx);
      CORO_RETURN(f1ap_ue_context_modification_response{});
    });
  }

  return launch_async<ue_context_modification_procedure>(cfg, request, ue_ctxt_list[request.ue_index], tx_pdu_notifier);
}

bool f1ap_cu_impl::handle_ue_id_update(ue_index_t ue_index, ue_index_t old_ue_index)
{
  if (!ue_ctxt_list.contains(ue_index) or !ue_ctxt_list.contains(old_ue_index)) {
    return false;
  }

  // Mark that an old gNB-DU UE F1AP ID needs to be sent to the DU in the next DL RRC Message Transfer.
  srsran_assert(ue_ctxt_list[old_ue_index].ue_ids.du_ue_f1ap_id &&
                    ue_ctxt_list[old_ue_index].ue_ids.du_ue_f1ap_id != gnb_du_ue_f1ap_id_t::invalid,
                "GNB-DU-UE-F1AP-ID should be valid");
  ue_ctxt_list[ue_index].pending_old_ue_id = *ue_ctxt_list[old_ue_index].ue_ids.du_ue_f1ap_id;
  return true;
}

void f1ap_cu_impl::handle_paging(const cu_cp_paging_message& msg)
{
  asn1::f1ap::paging_s paging = {};

  // Pack message into PDU.
  f1ap_message paging_msg;
  paging_msg.pdu.set_init_msg();
  paging_msg.pdu.init_msg().load_info_obj(ASN1_F1AP_ID_PAGING);

  fill_asn1_paging_message(paging_msg.pdu.init_msg().value.paging(), msg);

  // Send message to DU.
  tx_pdu_notifier.on_new_message(paging_msg);
}

void f1ap_cu_impl::handle_message(const f1ap_message& msg)
{
  // Run F1AP protocols in Control executor.
  if (not ctrl_exec.execute([this, msg]() {
        // Log received message.
        log_pdu(true, msg);

        switch (msg.pdu.type().value) {
          case asn1::f1ap::f1ap_pdu_c::types_opts::init_msg:
            handle_initiating_message(msg.pdu.init_msg());
            break;
          case asn1::f1ap::f1ap_pdu_c::types_opts::successful_outcome:
            handle_successful_outcome(msg.pdu.successful_outcome());
            break;
          case asn1::f1ap::f1ap_pdu_c::types_opts::unsuccessful_outcome:
            handle_unsuccessful_outcome(msg.pdu.unsuccessful_outcome());
            break;
          default:
            logger.warning("Invalid PDU type");
            break;
        }
      })) {
    logger.warning("Discarding F1AP PDU. Cause: CU-CP task queue is full");
  }
}

void f1ap_cu_impl::remove_ue_context(ue_index_t ue_index)
{
  if (!ue_ctxt_list.contains(ue_index)) {
    logger.debug("ue={}: UE context not found", ue_index);
    return;
  }

  ue_ctxt_list.remove_ue(ue_index);
}

#ifndef SRSRAN_HAS_ENTERPRISE

namespace {

class positioning_information_procedure
{
public:
  positioning_information_procedure(const f1ap_configuration& f1ap_cfg_,
                                    f1ap_ue_context&         ue_ctxt_,
                                    f1ap_message_notifier&   tx_pdu_notifier_,
                                    srslog::basic_logger&    logger_) :
    f1ap_cfg(f1ap_cfg_), ue_ctxt(ue_ctxt_), tx_pdu_notifier(tx_pdu_notifier_), logger(logger_)
  {
  }

  void operator()(coro_context<async_task<expected<positioning_information_response_t,
                                                   positioning_information_failure_t>>>& ctx)
  {
    CORO_BEGIN(ctx);

    transaction_sink.subscribe_to(ue_ctxt.ev_mng.positioning_information_outcome, f1ap_cfg.proc_timeout);

    send_positioning_information_request();

    CORO_AWAIT(transaction_sink);

    CORO_RETURN(handle_procedure_result());
  }

private:
  void send_positioning_information_request()
  {
    f1ap_message msg;
    msg.pdu.set_init_msg().load_info_obj(ASN1_F1AP_ID_POSITIONING_INFO_EXCHANGE);
    auto& asn1_request                 = msg.pdu.init_msg().value.positioning_info_request();
    asn1_request->gnb_cu_ue_f1ap_id    = gnb_cu_ue_f1ap_id_to_uint(ue_ctxt.ue_ids.cu_ue_f1ap_id);
    asn1_request->gnb_du_ue_f1ap_id    = gnb_du_ue_f1ap_id_to_uint(ue_ctxt.ue_ids.du_ue_f1ap_id.value());

    tx_pdu_notifier.on_new_message(msg);
  }

  expected<positioning_information_response_t, positioning_information_failure_t> handle_procedure_result()
  {
    if (transaction_sink.successful()) {
      logger.debug("{}: Positioning Information Request finished successfully",
                   f1ap_ue_log_prefix{ue_ctxt.ue_ids, "PositioningInformation"});
      return positioning_information_response_t{};
    }

    positioning_information_failure_t failure;
    failure.cause = nrppa_cause_misc_t::unspecified;
    if (transaction_sink.failed()) {
      logger.warning("{}: Positioning Information Request failed. Cause: {}",
                     f1ap_ue_log_prefix{ue_ctxt.ue_ids, "PositioningInformation"},
                     get_cause_str(transaction_sink.failure()->cause));
    } else {
      logger.warning("{}: Positioning Information Request failed. Cause: timeout",
                     f1ap_ue_log_prefix{ue_ctxt.ue_ids, "PositioningInformation"});
    }
    return make_unexpected(failure);
  }

  const f1ap_configuration& f1ap_cfg;
  f1ap_ue_context&         ue_ctxt;
  f1ap_message_notifier&   tx_pdu_notifier;
  srslog::basic_logger&    logger;

  protocol_transaction_outcome_observer<asn1::f1ap::positioning_info_resp_s, asn1::f1ap::positioning_info_fail_s>
      transaction_sink;
};

static measurement_failure_t make_measurement_failure(const measurement_request_t& request, nrppa_cause_t cause)
{
  measurement_failure_t failure;
  failure.lmf_meas_id = request.lmf_meas_id;
  failure.ran_meas_id = request.ran_meas_id;
  failure.cause       = cause;
  return failure;
}

static bool has_unsupported_measurement_request_ies(const measurement_request_t& request)
{
  if (request.meas_periodicity.has_value() || request.sfn_initialization_time.has_value() ||
      request.srs_config.has_value() || request.meas_beam_info_request.has_value() ||
      request.sys_frame_num.has_value() || request.slot_num.has_value() ||
      request.meas_periodicity_extended.has_value() || request.resp_time.has_value() ||
      request.meas_characteristics_request_ind.has_value() || request.meas_time_occasion.has_value() ||
      request.meas_amount.has_value()) {
    return true;
  }

  for (const trp_meas_request_item_t& item : request.trp_meas_request_list) {
    if (item.search_win_info.has_value() || item.cgi_nr.has_value() || item.aoa_search_win.has_value() ||
        item.nof_trp_rx_teg.has_value() || item.nof_trp_rx_tx_teg.has_value()) {
      return true;
    }
  }

  for (const trp_meas_quantities_list_item_t& item : request.trp_meas_quantities) {
    if (item.timing_report_granularity_factor.has_value()) {
      return true;
    }
  }

  return false;
}

static asn1::f1ap::pos_report_characteristics_opts::options
to_asn1_pos_report_characteristics(report_characteristics_t characteristics)
{
  switch (characteristics) {
    case report_characteristics_t::on_demand:
      return asn1::f1ap::pos_report_characteristics_opts::ondemand;
    case report_characteristics_t::periodic:
      return asn1::f1ap::pos_report_characteristics_opts::periodic;
  }
  return asn1::f1ap::pos_report_characteristics_opts::ondemand;
}

static asn1::f1ap::pos_meas_type_opts::options
to_asn1_pos_meas_type(trp_meas_quantities_item_t quantity)
{
  switch (quantity) {
    case trp_meas_quantities_item_t::gnb_rx_tx_time_diff:
      return asn1::f1ap::pos_meas_type_opts::gnb_rx_tx;
    case trp_meas_quantities_item_t::ul_srs_rsrp:
      return asn1::f1ap::pos_meas_type_opts::ul_srs_rsrp;
    case trp_meas_quantities_item_t::ul_aoa:
      return asn1::f1ap::pos_meas_type_opts::ul_aoa;
    case trp_meas_quantities_item_t::ul_rtoa:
      return asn1::f1ap::pos_meas_type_opts::ul_rtoa;
    case trp_meas_quantities_item_t::multiple_ul_aoa:
      return asn1::f1ap::pos_meas_type_opts::multiple_ul_aoa;
    case trp_meas_quantities_item_t::ul_srs_rsrp_p:
      return asn1::f1ap::pos_meas_type_opts::ul_srs_rsrpp;
  }
  return asn1::f1ap::pos_meas_type_opts::ul_rtoa;
}

class positioning_measurement_procedure
{
public:
  positioning_measurement_procedure(const f1ap_configuration& f1ap_cfg_,
                                    const measurement_request_t& request_,
                                    f1ap_message_notifier&       tx_pdu_notifier_,
                                    f1ap_event_manager&          ev_mng_,
                                    srslog::basic_logger&        logger_) :
    f1ap_cfg(f1ap_cfg_), request(request_), tx_pdu_notifier(tx_pdu_notifier_), ev_mng(ev_mng_), logger(logger_)
  {
  }

  void operator()(coro_context<async_task<expected<measurement_response_t, measurement_failure_t>>>& ctx)
  {
    CORO_BEGIN(ctx);

    transaction = ev_mng.transactions.create_transaction(f1ap_cfg.proc_timeout);
    if (!transaction.valid()) {
      logger.warning("\"PositioningMeasurement\": failed to allocate F1AP transaction");
      CORO_EARLY_RETURN(make_unexpected(make_measurement_failure(request, nrppa_cause_misc_t::unspecified)));
    }

    send_positioning_measurement_request();

    CORO_AWAIT(transaction);

    CORO_RETURN(handle_procedure_result());
  }

private:
  void send_positioning_measurement_request()
  {
    f1ap_message msg;
    msg.pdu.set_init_msg().load_info_obj(ASN1_F1AP_ID_POSITIONING_MEAS_EXCHANGE);
    auto& asn1_request = msg.pdu.init_msg().value.positioning_meas_request();

    asn1_request->transaction_id             = transaction.id();
    asn1_request->lmf_meas_id                = lmf_meas_id_to_uint(request.lmf_meas_id);
    asn1_request->ran_meas_id                = ran_meas_id_to_uint(request.ran_meas_id);
    asn1_request->pos_report_characteristics = to_asn1_pos_report_characteristics(request.report_characteristics);

    for (const trp_meas_request_item_t& item : request.trp_meas_request_list) {
      asn1::f1ap::trp_meas_request_item_s asn1_item;
      asn1_item.trp_id = trp_id_to_uint(item.trp_id);
      asn1_request->trp_meas_request_list.push_back(asn1_item);
    }
    for (const trp_meas_quantities_list_item_t& item : request.trp_meas_quantities) {
      asn1::f1ap::pos_meas_quantities_item_s asn1_item;
      asn1_item.pos_meas_type = to_asn1_pos_meas_type(item.trp_meas_quantities_item);
      asn1_request->pos_meas_quantities.push_back(asn1_item);
    }

    tx_pdu_notifier.on_new_message(msg);
  }

  expected<measurement_response_t, measurement_failure_t> handle_procedure_result()
  {
    if (!transaction.valid() || transaction.aborted() || !transaction.has_response()) {
      logger.warning("\"PositioningMeasurement\": failed. Cause: timeout");
      return make_unexpected(make_measurement_failure(request, nrppa_cause_misc_t::unspecified));
    }

    const f1ap_transaction_response& outcome = transaction.response();
    if (!outcome.has_value()) {
      const auto& asn1_failure = outcome.error().value.positioning_meas_fail();
      measurement_failure_t failure;
      failure.lmf_meas_id = uint_to_lmf_meas_id(asn1_failure->lmf_meas_id);
      failure.ran_meas_id = uint_to_ran_meas_id(asn1_failure->ran_meas_id);
      failure.cause       = nrppa_cause_misc_t::unspecified;
      logger.warning("\"PositioningMeasurement\": failed. Cause: {}", get_cause_str(asn1_failure->cause));
      return make_unexpected(failure);
    }

    const auto& asn1_response = outcome.value().value.positioning_meas_resp();
    measurement_response_t response;
    response.lmf_meas_id = uint_to_lmf_meas_id(asn1_response->lmf_meas_id);
    response.ran_meas_id = uint_to_ran_meas_id(asn1_response->ran_meas_id);
    logger.debug("\"PositioningMeasurement\": finished successfully");
    return response;
  }

  const f1ap_configuration& f1ap_cfg;
  measurement_request_t     request;
  f1ap_message_notifier&    tx_pdu_notifier;
  f1ap_event_manager&       ev_mng;
  srslog::basic_logger&     logger;
  f1ap_transaction          transaction;
};

static positioning_activation_failure_t make_positioning_activation_failure(nrppa_cause_t cause)
{
  positioning_activation_failure_t failure;
  failure.cause = cause;
  return failure;
}

static positioning_deactivation_failure_t make_positioning_deactivation_failure(nrppa_cause_t cause)
{
  positioning_deactivation_failure_t failure;
  failure.cause = cause;
  return failure;
}

static positioning_assistance_information_failure_t
make_positioning_assistance_information_failure(const positioning_assistance_information_control_request_t& request,
                                                nrppa_cause_t cause)
{
  positioning_assistance_information_failure_t failure;
  failure.transaction_id = request.transaction_id;
  failure.cause          = cause;
  return failure;
}

static bool has_unsupported_activation_request_ies(const positioning_activation_request_t& request)
{
  if (const auto* semipersistent = std::get_if<semipersistent_srs_t>(&request.srs_type)) {
    return semipersistent->srs_spatial_relation.has_value() ||
           semipersistent->srs_spatial_relation_per_srs_res.has_value();
  }
  const auto* aperiodic = std::get_if<aperiodic_srs_t>(&request.srs_type);
  return aperiodic == nullptr || !aperiodic->aperiodic || aperiodic->srs_res_trigger.has_value();
}

static bool is_empty_assistance_information_control_request(
    const positioning_assistance_information_control_request_t& request)
{
  return !request.pos_assist_info.has_value() && !request.pos_broadcast.has_value() &&
         request.positioning_broadcast_cells.empty() && !request.routing_id.has_value();
}

static void fill_asn1_srs_type(asn1::f1ap::srs_type_c& asn1_srs_type, const srs_type_t& srs_type)
{
  if (const auto* semipersistent = std::get_if<semipersistent_srs_t>(&srs_type)) {
    asn1_srs_type.set_semipersistent_srs().srs_res_set_id = semipersistent->srs_res_set_id;
    return;
  }

  asn1_srs_type.set_aperiodic_srs().aperiodic.value = asn1::f1ap::aperiodic_srs_s::aperiodic_opts::true_value;
}

static positioning_assistance_information_feedback_t
make_positioning_assistance_feedback(const asn1::f1ap::positioning_assist_info_feedback_s& asn1_feedback)
{
  positioning_assistance_information_feedback_t feedback;
  feedback.transaction_id = asn1_feedback->transaction_id;
  if (asn1_feedback->pos_assist_info_fail_list_present) {
    feedback.pos_assist_info_fail_list = asn1_feedback->pos_assist_info_fail_list.copy();
  }
  if (asn1_feedback->positioning_broadcast_cells_present) {
    for (const asn1::f1ap::nr_cgi_s& asn1_cgi : asn1_feedback->positioning_broadcast_cells) {
      expected<nr_cell_global_id_t> cgi = cgi_from_asn1(asn1_cgi);
      if (cgi.has_value()) {
        feedback.positioning_broadcast_cells.push_back(cgi.value());
      }
    }
  }
  if (asn1_feedback->routing_id_present) {
    feedback.routing_id = asn1_feedback->routing_id.copy();
  }
  return feedback;
}

class positioning_activation_procedure
{
public:
  positioning_activation_procedure(const f1ap_configuration&          f1ap_cfg_,
                                   const positioning_activation_request_t& request_,
                                   f1ap_ue_context&                  ue_ctxt_,
                                   f1ap_message_notifier&            tx_pdu_notifier_,
                                   srslog::basic_logger&             logger_) :
    f1ap_cfg(f1ap_cfg_),
    request(request_),
    ue_ctxt(ue_ctxt_),
    tx_pdu_notifier(tx_pdu_notifier_),
    logger(logger_)
  {
  }

  void operator()(coro_context<async_task<expected<positioning_activation_response_t,
                                                    positioning_activation_failure_t>>>& ctx)
  {
    CORO_BEGIN(ctx);

    transaction_sink.subscribe_to(ue_ctxt.ev_mng.positioning_activation_outcome, f1ap_cfg.proc_timeout);

    send_positioning_activation_request();

    CORO_AWAIT(transaction_sink);

    CORO_RETURN(handle_procedure_result());
  }

private:
  void send_positioning_activation_request()
  {
    f1ap_message msg;
    msg.pdu.set_init_msg().load_info_obj(ASN1_F1AP_ID_POSITIONING_ACTIVATION);
    auto& asn1_request = msg.pdu.init_msg().value.positioning_activation_request();

    asn1_request->gnb_cu_ue_f1ap_id = gnb_cu_ue_f1ap_id_to_uint(ue_ctxt.ue_ids.cu_ue_f1ap_id);
    asn1_request->gnb_du_ue_f1ap_id = gnb_du_ue_f1ap_id_to_uint(ue_ctxt.ue_ids.du_ue_f1ap_id.value());
    fill_asn1_srs_type(asn1_request->srs_type, request.srs_type);
    if (request.activation_time.has_value()) {
      asn1_request->activation_time_present = true;
      asn1_request->activation_time.from_number(request.activation_time.value());
    }

    tx_pdu_notifier.on_new_message(msg);
  }

  expected<positioning_activation_response_t, positioning_activation_failure_t> handle_procedure_result()
  {
    if (transaction_sink.successful()) {
      positioning_activation_response_t response;
      const auto&                       asn1_response = transaction_sink.response();
      if (asn1_response->sys_frame_num_present) {
        response.sys_frame_num = asn1_response->sys_frame_num;
      }
      if (asn1_response->slot_num_present) {
        response.slot_num = asn1_response->slot_num;
      }
      logger.debug("{}: Positioning Activation Request finished successfully",
                   f1ap_ue_log_prefix{ue_ctxt.ue_ids, "PositioningActivation"});
      return response;
    }

    positioning_activation_failure_t failure = make_positioning_activation_failure(nrppa_cause_misc_t::unspecified);
    if (transaction_sink.failed()) {
      logger.warning("{}: Positioning Activation Request failed. Cause: {}",
                     f1ap_ue_log_prefix{ue_ctxt.ue_ids, "PositioningActivation"},
                     get_cause_str(transaction_sink.failure()->cause));
    } else {
      logger.warning("{}: Positioning Activation Request failed. Cause: timeout",
                     f1ap_ue_log_prefix{ue_ctxt.ue_ids, "PositioningActivation"});
    }
    return make_unexpected(failure);
  }

  const f1ap_configuration& f1ap_cfg;
  positioning_activation_request_t request;
  f1ap_ue_context&                 ue_ctxt;
  f1ap_message_notifier&           tx_pdu_notifier;
  srslog::basic_logger&            logger;

  protocol_transaction_outcome_observer<asn1::f1ap::positioning_activation_resp_s,
                                        asn1::f1ap::positioning_activation_fail_s>
      transaction_sink;
};

class positioning_assistance_information_control_procedure
{
public:
  positioning_assistance_information_control_procedure(
      const f1ap_configuration&                                  f1ap_cfg_,
      const positioning_assistance_information_control_request_t& request_,
      f1ap_message_notifier&                                     tx_pdu_notifier_,
      f1ap_event_manager&                                        ev_mng_,
      srslog::basic_logger&                                      logger_) :
    f1ap_cfg(f1ap_cfg_), request(request_), tx_pdu_notifier(tx_pdu_notifier_), ev_mng(ev_mng_), logger(logger_)
  {
  }

  void operator()(coro_context<async_task<expected<positioning_assistance_information_feedback_t,
                                                   positioning_assistance_information_failure_t>>>& ctx)
  {
    CORO_BEGIN(ctx);

    transaction =
        ev_mng.positioning_assistance_transactions.create_transaction(request.transaction_id, f1ap_cfg.proc_timeout);
    if (!transaction.valid()) {
      logger.warning("\"PositioningAssistanceInformation\": failed to allocate transaction id={}",
                     request.transaction_id);
      CORO_EARLY_RETURN(
          make_unexpected(make_positioning_assistance_information_failure(request, nrppa_cause_misc_t::unspecified)));
    }

    send_positioning_assistance_information_control();

    CORO_AWAIT(transaction);

    CORO_RETURN(handle_procedure_result());
  }

private:
  void send_positioning_assistance_information_control()
  {
    f1ap_message msg;
    msg.pdu.set_init_msg().load_info_obj(ASN1_F1AP_ID_POSITIONING_ASSIST_INFO_CTRL);
    auto& asn1_request = msg.pdu.init_msg().value.positioning_assist_info_ctrl();

    asn1_request->transaction_id = request.transaction_id;
    if (request.pos_assist_info.has_value()) {
      asn1_request->pos_assist_info_present = true;
      asn1_request->pos_assist_info         = request.pos_assist_info->copy();
    }
    if (request.pos_broadcast.has_value()) {
      asn1_request->pos_broadcast_present = true;
      asn1_request->pos_broadcast.value =
          request.pos_broadcast.value() == positioning_assistance_broadcast_action::start
              ? asn1::f1ap::pos_broadcast_opts::start
              : asn1::f1ap::pos_broadcast_opts::stop;
    }
    if (!request.positioning_broadcast_cells.empty()) {
      asn1_request->positioning_broadcast_cells_present = true;
      for (const nr_cell_global_id_t& cgi : request.positioning_broadcast_cells) {
        asn1_request->positioning_broadcast_cells.push_back(cgi_to_asn1(cgi));
      }
    }
    if (request.routing_id.has_value()) {
      asn1_request->routing_id_present = true;
      asn1_request->routing_id         = request.routing_id->copy();
    }

    tx_pdu_notifier.on_new_message(msg);
  }

  expected<positioning_assistance_information_feedback_t, positioning_assistance_information_failure_t>
  handle_procedure_result()
  {
    if (!transaction.valid() || transaction.aborted() || !transaction.has_response()) {
      logger.warning("\"PositioningAssistanceInformation\": failed. Cause: timeout");
      return make_unexpected(make_positioning_assistance_information_failure(request, nrppa_cause_misc_t::unspecified));
    }

    logger.debug("\"PositioningAssistanceInformation\": finished successfully");
    return make_positioning_assistance_feedback(transaction.response());
  }

  const f1ap_configuration&                                  f1ap_cfg;
  positioning_assistance_information_control_request_t       request;
  f1ap_message_notifier&                                     tx_pdu_notifier;
  f1ap_event_manager&                                        ev_mng;
  srslog::basic_logger&                                      logger;
  f1ap_positioning_assistance_feedback_transaction           transaction;
};

} // namespace

async_task<expected<trp_information_response_t, trp_information_failure_t>>
f1ap_cu_impl::handle_trp_information_request(const trp_information_request_t& request)
{
  logger.info("TRP information requests are not supported");
  return launch_async(
      [](coro_context<async_task<expected<trp_information_response_t, trp_information_failure_t>>>& ctx) {
        CORO_BEGIN(ctx);
        CORO_RETURN(make_unexpected(trp_information_failure_t{}));
      });
}

async_task<expected<positioning_information_response_t, positioning_information_failure_t>>
f1ap_cu_impl::handle_positioning_information_request(const positioning_information_request_t& request)
{
  auto make_failure_task = [](nrppa_cause_t cause)
      -> async_task<expected<positioning_information_response_t, positioning_information_failure_t>> {
    return launch_async([cause](coro_context<async_task<expected<positioning_information_response_t,
                                                            positioning_information_failure_t>>>& ctx) mutable {
      positioning_information_failure_t failure;
      CORO_BEGIN(ctx);
      failure.cause = cause;
      CORO_RETURN(make_unexpected(failure));
    });
  };

  f1ap_ue_context* ue_ctxt = ue_ctxt_list.find(request.ue_index);
  if (ue_ctxt == nullptr) {
    logger.warning("ue={}: Dropping Positioning Information Request. Cause: UE context does not exist",
                   request.ue_index);
    return make_failure_task(nrppa_cause_misc_t::unspecified);
  }
  if (!ue_ctxt->ue_ids.du_ue_f1ap_id.has_value() ||
      ue_ctxt->ue_ids.du_ue_f1ap_id.value() == gnb_du_ue_f1ap_id_t::invalid) {
    logger.warning("{}: Dropping Positioning Information Request. Cause: gNB-DU UE F1AP ID is missing",
                   f1ap_ue_log_prefix{ue_ctxt->ue_ids, "PositioningInformation"});
    return make_failure_task(nrppa_cause_misc_t::unspecified);
  }
  if (request.requested_srs_tx_characteristics.has_value() || request.ue_report_info.has_value() ||
      request.ue_teg_info_request.has_value() || request.ue_teg_report_periodicity.has_value()) {
    logger.info("{}: Dropping Positioning Information Request. Cause: requested optional IEs are not supported",
                f1ap_ue_log_prefix{ue_ctxt->ue_ids, "PositioningInformation"});
    return make_failure_task(nrppa_cause_radio_network_t::requested_item_not_supported);
  }

  return launch_async<positioning_information_procedure>(cfg, *ue_ctxt, tx_pdu_notifier, logger);
}

async_task<expected<positioning_activation_response_t, positioning_activation_failure_t>>
f1ap_cu_impl::handle_positioning_activation_request(const positioning_activation_request_t& request)
{
  auto make_failure_task = [](nrppa_cause_t cause)
      -> async_task<expected<positioning_activation_response_t, positioning_activation_failure_t>> {
    return launch_async([cause](coro_context<async_task<expected<positioning_activation_response_t,
                                                           positioning_activation_failure_t>>>& ctx) mutable {
      CORO_BEGIN(ctx);
      CORO_RETURN(make_unexpected(make_positioning_activation_failure(cause)));
    });
  };

  f1ap_ue_context* ue_ctxt = ue_ctxt_list.find(request.ue_index);
  if (ue_ctxt == nullptr) {
    logger.warning("ue={}: Dropping Positioning Activation Request. Cause: UE context does not exist",
                   request.ue_index);
    return make_failure_task(nrppa_cause_misc_t::unspecified);
  }
  if (!ue_ctxt->ue_ids.du_ue_f1ap_id.has_value() ||
      ue_ctxt->ue_ids.du_ue_f1ap_id.value() == gnb_du_ue_f1ap_id_t::invalid) {
    logger.warning("{}: Dropping Positioning Activation Request. Cause: gNB-DU UE F1AP ID is missing",
                   f1ap_ue_log_prefix{ue_ctxt->ue_ids, "PositioningActivation"});
    return make_failure_task(nrppa_cause_misc_t::unspecified);
  }
  if (has_unsupported_activation_request_ies(request)) {
    logger.info("{}: Dropping Positioning Activation Request. Cause: requested optional IEs are not supported",
                f1ap_ue_log_prefix{ue_ctxt->ue_ids, "PositioningActivation"});
    return make_failure_task(nrppa_cause_radio_network_t::requested_item_not_supported);
  }

  return launch_async<positioning_activation_procedure>(cfg, request, *ue_ctxt, tx_pdu_notifier, logger);
}

async_task<expected<positioning_deactivation_response_t, positioning_deactivation_failure_t>>
f1ap_cu_impl::handle_positioning_deactivation_request(const positioning_deactivation_request_t& request)
{
  auto make_failure_task = [](nrppa_cause_t cause)
      -> async_task<expected<positioning_deactivation_response_t, positioning_deactivation_failure_t>> {
    return launch_async([cause](coro_context<async_task<expected<positioning_deactivation_response_t,
                                                           positioning_deactivation_failure_t>>>& ctx) mutable {
      CORO_BEGIN(ctx);
      CORO_RETURN(make_unexpected(make_positioning_deactivation_failure(cause)));
    });
  };

  f1ap_ue_context* ue_ctxt = ue_ctxt_list.find(request.ue_index);
  if (ue_ctxt == nullptr) {
    logger.warning("ue={}: Dropping Positioning Deactivation. Cause: UE context does not exist", request.ue_index);
    return make_failure_task(nrppa_cause_misc_t::unspecified);
  }
  if (!ue_ctxt->ue_ids.du_ue_f1ap_id.has_value() ||
      ue_ctxt->ue_ids.du_ue_f1ap_id.value() == gnb_du_ue_f1ap_id_t::invalid) {
    logger.warning("{}: Dropping Positioning Deactivation. Cause: gNB-DU UE F1AP ID is missing",
                   f1ap_ue_log_prefix{ue_ctxt->ue_ids, "PositioningDeactivation"});
    return make_failure_task(nrppa_cause_misc_t::unspecified);
  }

  f1ap_message msg;
  msg.pdu.set_init_msg().load_info_obj(ASN1_F1AP_ID_POSITIONING_DEACTIVATION);
  auto& deactivation = msg.pdu.init_msg().value.positioning_deactivation();
  deactivation->gnb_cu_ue_f1ap_id = gnb_cu_ue_f1ap_id_to_uint(ue_ctxt->ue_ids.cu_ue_f1ap_id);
  deactivation->gnb_du_ue_f1ap_id = gnb_du_ue_f1ap_id_to_uint(ue_ctxt->ue_ids.du_ue_f1ap_id.value());
  if (request.srs_res_set_id.has_value()) {
    deactivation->abort_tx.set_srs_res_set_id() = request.srs_res_set_id.value();
  } else {
    deactivation->abort_tx.set_release_all();
  }

  tx_pdu_notifier.on_new_message(msg);
  return launch_async(
      [](coro_context<async_task<expected<positioning_deactivation_response_t,
                                         positioning_deactivation_failure_t>>>& ctx) {
        positioning_deactivation_response_t response;
        CORO_BEGIN(ctx);
        CORO_RETURN(response);
      });
}

async_task<expected<positioning_assistance_information_feedback_t, positioning_assistance_information_failure_t>>
f1ap_cu_impl::handle_positioning_assistance_information_control(
    const positioning_assistance_information_control_request_t& request)
{
  auto make_failure_task =
      [](positioning_assistance_information_failure_t failure)
      -> async_task<expected<positioning_assistance_information_feedback_t,
                             positioning_assistance_information_failure_t>> {
    return launch_async(
        [failure](coro_context<async_task<expected<positioning_assistance_information_feedback_t,
                                                   positioning_assistance_information_failure_t>>>& ctx) mutable {
          CORO_BEGIN(ctx);
          CORO_RETURN(make_unexpected(failure));
        });
  };

  if (request.transaction_id > 255U) {
    logger.warning("\"PositioningAssistanceInformation\": dropping request. Cause: transaction id is out of range");
    return make_failure_task(
        make_positioning_assistance_information_failure(request, nrppa_cause_misc_t::unspecified));
  }
  if (is_empty_assistance_information_control_request(request)) {
    logger.info("\"PositioningAssistanceInformation\": dropping request. Cause: no supported assistance field present");
    return make_failure_task(make_positioning_assistance_information_failure(
        request, nrppa_cause_radio_network_t::requested_item_not_supported));
  }

  return launch_async<positioning_assistance_information_control_procedure>(
      cfg, request, tx_pdu_notifier, ev_mng, logger);
}

async_task<expected<measurement_response_t, measurement_failure_t>>
f1ap_cu_impl::handle_positioning_measurement_request(const measurement_request_t& request)
{
  auto make_failure_task = [](measurement_failure_t failure)
      -> async_task<expected<measurement_response_t, measurement_failure_t>> {
    return launch_async([failure](coro_context<async_task<expected<measurement_response_t,
                                                           measurement_failure_t>>>& ctx) mutable {
      CORO_BEGIN(ctx);
      CORO_RETURN(make_unexpected(failure));
    });
  };

  f1ap_ue_context* ue_ctxt = ue_ctxt_list.find(request.ue_index);
  if (ue_ctxt == nullptr) {
    logger.warning("ue={}: Dropping Positioning Measurement Request. Cause: UE context does not exist",
                   request.ue_index);
    return make_failure_task(make_measurement_failure(request, nrppa_cause_misc_t::unspecified));
  }
  if (!ue_ctxt->ue_ids.du_ue_f1ap_id.has_value() ||
      ue_ctxt->ue_ids.du_ue_f1ap_id.value() == gnb_du_ue_f1ap_id_t::invalid) {
    logger.warning("{}: Dropping Positioning Measurement Request. Cause: gNB-DU UE F1AP ID is missing",
                   f1ap_ue_log_prefix{ue_ctxt->ue_ids, "PositioningMeasurement"});
    return make_failure_task(make_measurement_failure(request, nrppa_cause_misc_t::unspecified));
  }
  if (request.trp_meas_request_list.empty() || request.trp_meas_quantities.empty()) {
    logger.info("{}: Dropping Positioning Measurement Request. Cause: mandatory TRP or quantity list is empty",
                f1ap_ue_log_prefix{ue_ctxt->ue_ids, "PositioningMeasurement"});
    return make_failure_task(
        make_measurement_failure(request, nrppa_cause_radio_network_t::requested_item_not_supported));
  }
  if (has_unsupported_measurement_request_ies(request)) {
    logger.info("{}: Dropping Positioning Measurement Request. Cause: requested optional IEs are not supported",
                f1ap_ue_log_prefix{ue_ctxt->ue_ids, "PositioningMeasurement"});
    return make_failure_task(
        make_measurement_failure(request, nrppa_cause_radio_network_t::requested_item_not_supported));
  }

  return launch_async<positioning_measurement_procedure>(cfg, request, tx_pdu_notifier, ev_mng, logger);
}

#endif // SRSRAN_HAS_ENTERPRISE

async_task<f1ap_gnb_cu_configuration_update_response>
f1ap_cu_impl::handle_gnb_cu_configuration_update(const f1ap_gnb_cu_configuration_update& request)
{
  return launch_async<gnb_cu_configuration_update_procedure>(cfg, request, tx_pdu_notifier, ev_mng, logger);
}

async_task<f1ap_gnb_du_resource_coordination_response>
f1ap_cu_impl::handle_gnb_du_resource_coordination_request(const f1ap_gnb_du_resource_coordination_request& request)
{
  return launch_async<gnb_du_resource_coordination_procedure>(cfg, request, tx_pdu_notifier, ev_mng, logger);
}

void f1ap_cu_impl::handle_initiating_message(const asn1::f1ap::init_msg_s& msg)
{
  switch (msg.value.type().value) {
    case asn1::f1ap::f1ap_elem_procs_o::init_msg_c::types_opts::options::f1_setup_request:
      handle_f1_setup_request(msg.value.f1_setup_request());
      break;
    case asn1::f1ap::f1ap_elem_procs_o::init_msg_c::types_opts::init_ul_rrc_msg_transfer:
      handle_initial_ul_rrc_message(msg.value.init_ul_rrc_msg_transfer());
      break;
    case asn1::f1ap::f1ap_elem_procs_o::init_msg_c::types_opts::ul_rrc_msg_transfer:
      handle_ul_rrc_message(msg.value.ul_rrc_msg_transfer());
      break;
    case asn1::f1ap::f1ap_elem_procs_o::init_msg_c::types_opts::f1_removal_request:
      du_processor_notifier.schedule_async_task(launch_async<f1_removal_procedure>(
          msg.value.f1_removal_request(), tx_pdu_notifier, du_processor_notifier, ue_ctxt_list, logger));
      break;
    case asn1::f1ap::f1ap_elem_procs_o::init_msg_c::types_opts::options::ue_context_release_request:
      handle_ue_context_release_request(msg.value.ue_context_release_request());
      break;
    case asn1::f1ap::f1ap_elem_procs_o::init_msg_c::types_opts::options::gnb_du_cfg_upd:
      handle_du_cfg_update(msg.value.gnb_du_cfg_upd());
      break;
    case asn1::f1ap::f1ap_elem_procs_o::init_msg_c::types_opts::options::positioning_assist_info_feedback:
      if (!ev_mng.positioning_assistance_transactions.set_response(
              msg.value.positioning_assist_info_feedback()->transaction_id,
              msg.value.positioning_assist_info_feedback())) {
        logger.warning("Unexpected Positioning Assistance Information Feedback transaction id={}",
                       msg.value.positioning_assist_info_feedback()->transaction_id);
      }
      break;
    default:
      logger.warning("Initiating message of type {} is not supported", msg.value.type().to_string());
      break;
  }
}

void f1ap_cu_impl::handle_du_cfg_update(const asn1::f1ap::gnb_du_cfg_upd_s& request)
{
  f1ap_message f1ap_msg;

  // TODO: for now, always reply with a config update acknowledge.
  f1ap_msg.pdu.set_successful_outcome().load_info_obj(ASN1_F1AP_ID_GNB_DU_CFG_UPD);
  auto& resp = f1ap_msg.pdu.successful_outcome().value.gnb_du_cfg_upd_ack();

  resp->transaction_id = request->transaction_id;

  // Send F1AP PDU to DU.
  tx_pdu_notifier.on_new_message(f1ap_msg);
}

void f1ap_cu_impl::handle_f1_setup_request(const asn1::f1ap::f1_setup_request_s& request)
{
  current_transaction_id = request->transaction_id;

  handle_f1_setup_procedure(request, du_ctxt, tx_pdu_notifier, du_processor_notifier, logger);
}

void f1ap_cu_impl::handle_initial_ul_rrc_message(const asn1::f1ap::init_ul_rrc_msg_transfer_s& msg)
{
  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(msg->gnb_du_ue_f1ap_id);

  expected<nr_cell_global_id_t> cgi = cgi_from_asn1(msg->nr_cgi);
  if (not cgi.has_value()) {
    logger.warning("du_ue={}: Dropping \"InitialULRRCMessageTransfer\". Invalid CGI", fmt::underlying(du_ue_id));
    return;
  }

  rnti_t crnti = to_rnti(msg->c_rnti);
  if (crnti == rnti_t::INVALID_RNTI) {
    logger.warning("du_ue={}: Dropping \"InitialULRRCMessageTransfer\". Cause: Invalid C-RNTI",
                   fmt::underlying(du_ue_id));
    return;
  }

  if (msg->sul_access_ind_present) {
    logger.debug("du_ue={}: Ignoring SUL access indicator", fmt::underlying(du_ue_id));
  }

  if (msg->rrc_container_rrc_setup_complete_present) {
    logger.warning("du_ue={}: Ignoring RRC Container RRCSetupComplete. Cause: Network Sharing with multiple cell-ID "
                   "broadcast is not supported",
                   fmt::underlying(du_ue_id));
  }

  const gnb_cu_ue_f1ap_id_t cu_ue_f1ap_id = ue_ctxt_list.allocate_gnb_cu_ue_f1ap_id();
  if (cu_ue_f1ap_id == gnb_cu_ue_f1ap_id_t::invalid) {
    logger.warning("du_ue={}: Dropping \"InitialULRRCMessageTransfer\". Cause: Failed to allocate CU-UE-F1AP-ID",
                   fmt::underlying(du_ue_id));
    return;
  }

  // Request RRC UE entity creation.
  ue_rrc_context_creation_request req;
  req.c_rnti = crnti;
  req.cgi    = cgi.value();
  if (msg->du_to_cu_rrc_container_present) {
    req.du_to_cu_rrc_container = byte_buffer(msg->du_to_cu_rrc_container);
  } else {
    // Assume the DU can't serve the UE, so the CU-CP should reject the UE, see TS 38.473 section 8.4.1.2.
    // We will forward an empty container to the RRC UE entity, that will trigger an RRC Reject.
    logger.debug("du_ue={}: Forwarding \"InitialULRRCMessageTransfer\" to RRC to reject the UE. Cause: Missing DU "
                 "to CU container",
                 fmt::underlying(du_ue_id));
  }
  ue_rrc_context_creation_outcome resp = du_processor_notifier.on_ue_rrc_context_creation_request(req);

  // Reject the UE if the creation was not successful.
  if (not resp.has_value()) {
    asn1::f1ap::dl_rrc_msg_transfer_s dl_rrc_msg = {};
    dl_rrc_msg->gnb_cu_ue_f1ap_id                = gnb_cu_ue_f1ap_id_to_uint(cu_ue_f1ap_id);
    dl_rrc_msg->gnb_du_ue_f1ap_id                = gnb_du_ue_f1ap_id_to_uint(du_ue_id);
    dl_rrc_msg->srb_id                           = srb_id_to_uint(srb_id_t::srb0);
    dl_rrc_msg->rrc_container                    = resp.error().copy();

    // Pack message into PDU.
    f1ap_message f1ap_dl_rrc_msg;
    f1ap_dl_rrc_msg.pdu.set_init_msg();
    f1ap_dl_rrc_msg.pdu.init_msg().load_info_obj(ASN1_F1AP_ID_DL_RRC_MSG_TRANSFER);
    f1ap_dl_rrc_msg.pdu.init_msg().value.dl_rrc_msg_transfer() = std::move(dl_rrc_msg);

    // Send DL RRC message.
    tx_pdu_notifier.on_new_message(f1ap_dl_rrc_msg);
    return;
  }

  // Create UE context and store it.
  f1ap_ue_context& ue_ctxt = ue_ctxt_list.add_ue(resp->ue_index, cu_ue_f1ap_id);
  ue_ctxt_list.add_du_ue_f1ap_id(cu_ue_f1ap_id, du_ue_id);
  ue_ctxt_list.add_srb0_rrc_notifier(resp->ue_index, resp->f1ap_srb0_notifier);
  ue_ctxt_list.add_srb1_rrc_notifier(resp->ue_index, resp->f1ap_srb1_notifier);
  ue_ctxt_list.add_srb2_rrc_notifier(resp->ue_index, resp->f1ap_srb2_notifier);

  ue_ctxt.logger.log_info("Added UE context");

  // Forward RRC container.
  f1c_ul_bearer_handler* bearer = ue_ctxt.get_ul_bearer_manager().get_srb(srb_id_t::srb0);
  srsran_assert(bearer, "SRB0 should be always active");
  bearer->handle_ul_rrc_message(msg->rrc_container.copy());
}

void f1ap_cu_impl::handle_ul_rrc_message(const asn1::f1ap::ul_rrc_msg_transfer_s& msg)
{
  f1ap_ue_context* ue_ctxt = ue_ctxt_list.find(int_to_gnb_cu_ue_f1ap_id(msg->gnb_cu_ue_f1ap_id));
  if (ue_ctxt == nullptr) {
    logger.warning("cu_ue={} du_ue={}: Dropping \"ULRRCMessageTransfer\". UE context does not exist",
                   msg->gnb_cu_ue_f1ap_id,
                   msg->gnb_du_ue_f1ap_id);
    return;
  }

  f1c_ul_bearer_handler* bearer = ue_ctxt->get_ul_bearer_manager().get_srb(int_to_srb_id(msg->srb_id));
  if (bearer == nullptr) {
    logger.warning("cu_ue={} du_ue={}: Dropping \"ULRRCMessageTransfer\". Cause: SRB{} has not been activated",
                   msg->gnb_cu_ue_f1ap_id,
                   msg->gnb_du_ue_f1ap_id,
                   int_to_srb_id(msg->srb_id));
    return;
  }
  bearer->handle_ul_rrc_message(msg->rrc_container.copy());
}

void f1ap_cu_impl::handle_ue_context_release_request(const asn1::f1ap::ue_context_release_request_s& msg)
{
  if (!ue_ctxt_list.contains(int_to_gnb_cu_ue_f1ap_id(msg->gnb_cu_ue_f1ap_id))) {
    logger.warning("cu_ue={} du_ue={}: Dropping \"UeContextReleaseRequest\". UE context does not exist",
                   msg->gnb_cu_ue_f1ap_id,
                   msg->gnb_du_ue_f1ap_id);
    return;
  }

  f1ap_ue_context& ue_ctxt = ue_ctxt_list[int_to_gnb_cu_ue_f1ap_id(msg->gnb_cu_ue_f1ap_id)];

  if (ue_ctxt.marked_for_release) {
    // UE context is already being released. Ignore the request.
    ue_ctxt.logger.log_debug("\"UeContextReleaseRequest\" ignored. UE context release procedure has already started");
    return;
  }

  ue_ctxt.logger.log_debug("Received \"UeContextReleaseRequest\"");

  f1ap_ue_context_release_request req;
  req.ue_index = ue_ctxt.ue_ids.ue_index;
  req.cause    = asn1_to_cause(msg->cause);

  du_processor_notifier.on_du_initiated_ue_context_release_request(req);
}

void f1ap_cu_impl::handle_successful_outcome(const asn1::f1ap::successful_outcome_s& outcome)
{
  auto get_ue_ctxt_in_ue_assoc_msg = [this](const asn1::f1ap::successful_outcome_s& outcome_) -> f1ap_ue_context* {
    std::optional<gnb_cu_ue_f1ap_id_t> cu_ue_id = get_gnb_cu_ue_f1ap_id(outcome_);
    // The GNB-CU-UE-F1AP-ID field is mandatory in all UE associated successful messages.
    srsran_assert(cu_ue_id,
                  "Discarding received \"{}\". Cause: GNB-CU-UE-F1AP-ID field is mandatory",
                  outcome_.value.type().to_string());

    f1ap_ue_context* ue_ctxt = ue_ctxt_list.find(*cu_ue_id);
    if (ue_ctxt == nullptr) {
      logger.warning("cu_ue={}: Discarding received \"{}\". Cause: UE was not found.",
                     fmt::underlying(*cu_ue_id),
                     outcome_.value.type().to_string());
      return nullptr;
    }
    return ue_ctxt;
  };

  std::optional<uint8_t> transaction_id = std::nullopt;

  switch (outcome.value.type().value) {
    case asn1::f1ap::f1ap_elem_procs_o::successful_outcome_c::types_opts::ue_context_release_complete:
      if (auto* ue_ctxt = get_ue_ctxt_in_ue_assoc_msg(outcome)) {
        ue_ctxt->ev_mng.context_release_complete.set(outcome.value.ue_context_release_complete());
      }
      break;
    case asn1::f1ap::f1ap_elem_procs_o::successful_outcome_c::types_opts::ue_context_setup_resp:
      if (auto* ue_ctxt = get_ue_ctxt_in_ue_assoc_msg(outcome)) {
        ue_ctxt->ev_mng.context_setup_outcome.set(outcome.value.ue_context_setup_resp());
      }
      break;
    case asn1::f1ap::f1ap_elem_procs_o::successful_outcome_c::types_opts::ue_context_mod_resp:
      if (auto* ue_ctxt = get_ue_ctxt_in_ue_assoc_msg(outcome)) {
        ue_ctxt->ev_mng.context_modification_outcome.set(outcome.value.ue_context_mod_resp());
      }
      break;
    case asn1::f1ap::f1ap_elem_procs_o::successful_outcome_c::types_opts::trp_info_resp:
      transaction_id = get_transaction_id(outcome);
      if (not transaction_id.has_value()) {
        logger.error("Successful outcome of type {} is not supported", outcome.value.type().to_string());
        break;
      }
      if (not ev_mng.transactions.set_response(transaction_id.value(), outcome)) {
        logger.warning("Unexpected transaction id={}", transaction_id.value());
      }
      break;
    case asn1::f1ap::f1ap_elem_procs_o::successful_outcome_c::types_opts::positioning_info_resp:
      if (auto* ue_ctxt = get_ue_ctxt_in_ue_assoc_msg(outcome)) {
        ue_ctxt->ev_mng.positioning_information_outcome.set(outcome.value.positioning_info_resp());
      }
      break;
    case asn1::f1ap::f1ap_elem_procs_o::successful_outcome_c::types_opts::positioning_activation_resp:
      if (auto* ue_ctxt = get_ue_ctxt_in_ue_assoc_msg(outcome)) {
        ue_ctxt->ev_mng.positioning_activation_outcome.set(outcome.value.positioning_activation_resp());
      }
      break;
    case asn1::f1ap::f1ap_elem_procs_o::successful_outcome_c::types_opts::positioning_meas_resp:
      transaction_id = get_transaction_id(outcome);
      if (not transaction_id.has_value()) {
        logger.error("Successful outcome of type {} is not supported", outcome.value.type().to_string());
        break;
      }
      if (not ev_mng.transactions.set_response(transaction_id.value(), outcome)) {
        logger.warning("Unexpected transaction id={}", transaction_id.value());
      }
      break;
    case asn1::f1ap::f1ap_elem_procs_o::successful_outcome_c::types_opts::gnb_cu_cfg_upd_ack:
    case asn1::f1ap::f1ap_elem_procs_o::successful_outcome_c::types_opts::gnb_du_res_coordination_resp:
      transaction_id = get_transaction_id(outcome);
      if (not transaction_id.has_value()) {
        logger.error("Successful outcome of type {} is not supported", outcome.value.type().to_string());
        break;
      }
      if (not ev_mng.transactions.set_response(transaction_id.value(), outcome)) {
        logger.warning("Unexpected transaction id={}", transaction_id.value());
      }
      break;
    default:
      logger.warning("Successful outcome of type {} is not supported", outcome.value.type().to_string());
      break;
  }
}

void f1ap_cu_impl::handle_unsuccessful_outcome(const asn1::f1ap::unsuccessful_outcome_s& outcome)
{
  auto get_ue_ctxt_in_ue_assoc_msg = [this](const asn1::f1ap::unsuccessful_outcome_s& outcome_) -> f1ap_ue_context* {
    std::optional<gnb_cu_ue_f1ap_id_t> cu_ue_id = get_gnb_cu_ue_f1ap_id(outcome_);
    // The GNB-CU-UE-F1AP-ID field is mandatory in all UE associated failure messages.
    srsran_assert(cu_ue_id,
                  "Discarding received \"{}\". Cause: GNB-CU-UE-F1AP-ID field is mandatory",
                  outcome_.value.type().to_string());

    f1ap_ue_context* ue_ctxt = ue_ctxt_list.find(*cu_ue_id);
    if (ue_ctxt == nullptr) {
      logger.warning("cu_ue={}: Discarding received \"{}\". Cause: UE was not found.",
                     fmt::underlying(*cu_ue_id),
                     outcome_.value.type().to_string());
      return nullptr;
    }
    return ue_ctxt;
  };

  std::optional<uint8_t> transaction_id = std::nullopt;

  switch (outcome.value.type().value) {
    case asn1::f1ap::f1ap_elem_procs_o::unsuccessful_outcome_c::types_opts::ue_context_setup_fail:
      if (auto* ue_ctxt = get_ue_ctxt_in_ue_assoc_msg(outcome)) {
        ue_ctxt->ev_mng.context_setup_outcome.set(outcome.value.ue_context_setup_fail());
      }
      break;
    case asn1::f1ap::f1ap_elem_procs_o::unsuccessful_outcome_c::types_opts::ue_context_mod_fail:
      if (auto* ue_ctxt = get_ue_ctxt_in_ue_assoc_msg(outcome)) {
        ue_ctxt->ev_mng.context_modification_outcome.set(outcome.value.ue_context_mod_fail());
      }
      break;
    case asn1::f1ap::f1ap_elem_procs_o::unsuccessful_outcome_c::types_opts::trp_info_fail:
      transaction_id = get_transaction_id(outcome);
      if (not transaction_id.has_value()) {
        logger.error("Unsuccessful outcome of type {} is not supported", outcome.value.type().to_string());
        break;
      }
      if (not ev_mng.transactions.set_response(transaction_id.value(), make_unexpected(outcome))) {
        logger.warning("Unexpected transaction id={}", transaction_id.value());
      }
      break;
    case asn1::f1ap::f1ap_elem_procs_o::unsuccessful_outcome_c::types_opts::positioning_info_fail:
      if (auto* ue_ctxt = get_ue_ctxt_in_ue_assoc_msg(outcome)) {
        ue_ctxt->ev_mng.positioning_information_outcome.set(outcome.value.positioning_info_fail());
      }
      break;
    case asn1::f1ap::f1ap_elem_procs_o::unsuccessful_outcome_c::types_opts::positioning_activation_fail:
      if (auto* ue_ctxt = get_ue_ctxt_in_ue_assoc_msg(outcome)) {
        ue_ctxt->ev_mng.positioning_activation_outcome.set(outcome.value.positioning_activation_fail());
      }
      break;
    case asn1::f1ap::f1ap_elem_procs_o::unsuccessful_outcome_c::types_opts::positioning_meas_fail:
      transaction_id = get_transaction_id(outcome);
      if (not transaction_id.has_value()) {
        logger.error("Unsuccessful outcome of type {} is not supported", outcome.value.type().to_string());
        break;
      }
      if (not ev_mng.transactions.set_response(transaction_id.value(), make_unexpected(outcome))) {
        logger.warning("Unexpected transaction id={}", transaction_id.value());
      }
      break;
    case asn1::f1ap::f1ap_elem_procs_o::unsuccessful_outcome_c::types_opts::gnb_cu_cfg_upd_fail:
      transaction_id = get_transaction_id(outcome);
      if (not transaction_id.has_value()) {
        logger.error("Unsuccessful outcome of type {} is not supported", outcome.value.type().to_string());
        break;
      }
      if (not ev_mng.transactions.set_response(transaction_id.value(), make_unexpected(outcome))) {
        logger.warning("Unexpected transaction id={}", transaction_id.value());
      }
      break;
    default:
      logger.warning("Unsuccessful outcome of type {} is not supported", outcome.value.type().to_string());
      break;
  }
}

void f1ap_cu_impl::log_pdu(bool is_rx, const f1ap_message& msg)
{
  using namespace asn1::f1ap;

  if (not logger.info.enabled()) {
    return;
  }

  // In case of F1 Setup, the gNB-DU-Id might not be set yet.
  gnb_du_id_t du_id = du_ctxt.gnb_du_id;
  if (du_id == gnb_du_id_t::invalid) {
    if (msg.pdu.type().value == f1ap_pdu_c::types_opts::init_msg and
        msg.pdu.init_msg().value.type().value == f1ap_elem_procs_o::init_msg_c::types_opts::f1_setup_request) {
      du_id = int_to_gnb_du_id(msg.pdu.init_msg().value.f1_setup_request()->gnb_du_id);
    }
  }

  // Fetch UE index.
  auto                      cu_ue_id = get_gnb_cu_ue_f1ap_id(msg.pdu);
  std::optional<ue_index_t> ue_idx;
  if (cu_ue_id.has_value()) {
    const auto* ue_ptr = ue_ctxt_list.find(cu_ue_id.value());
    if (ue_ptr != nullptr and ue_ptr->ue_ids.ue_index != ue_index_t::invalid) {
      ue_idx = ue_ptr->ue_ids.ue_index;
    }
  }

  // Log PDU.
  log_f1ap_pdu(logger, is_rx, du_id, ue_idx, msg, cfg.json_log_enabled);
}

void f1ap_cu_impl::tx_pdu_notifier_with_logging::on_new_message(const f1ap_message& msg)
{
  // Log message.
  parent.log_pdu(false, msg);

  // Forward message to DU.
  decorated.on_new_message(msg);
}
