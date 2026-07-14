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

#include "nrppa_dummy_impl.h"
#include "srsran/cu_cp/cu_cp_types.h"
#include "srsran/nrppa/nrppa_pdu.h"
#include "srsran/srslog/srslog.h"

using namespace srsran;
using namespace srs_cu_cp;

nrppa_dummy_impl::nrppa_dummy_impl(nrppa_cu_cp_notifier& cu_cp_notifier_,
                                   common_task_scheduler& common_task_sched_) :
  logger(srslog::fetch_basic_logger("NRPPA")), cu_cp_notifier(cu_cp_notifier_), common_task_sched(common_task_sched_)
{
}

// Note: For fwd declaration of member types, dtor cannot be trivial.
nrppa_dummy_impl::~nrppa_dummy_impl() {}

void nrppa_dummy_impl::remove_ue_context(ue_index_t ue_index) {}

void nrppa_dummy_impl::handle_new_nrppa_pdu(const byte_buffer&                    nrppa_pdu,
                                            std::variant<ue_index_t, amf_index_t> ue_or_amf_index)
{
  if (!is_nrppa_minimal_pdu(nrppa_pdu)) {
    auto standard_decoded = decode_nrppa_standard_pdu(nrppa_pdu);
    if (!standard_decoded.has_value()) {
      logger.info("Dropping standard NRPPa Transport PDU. Cause: {}", standard_decoded.error());
      cu_cp_notifier.on_nrppa_standard_codec_event(
          {nrppa_standard_codec_event_type::decode_failure, standard_decoded.error()});
      cu_cp_notifier.on_unsupported_nrppa_pdu(standard_decoded.error());
      return;
    }

    cu_cp_notifier.on_nrppa_standard_codec_event(
        {nrppa_standard_codec_event_type::decode_success, "trp_information"});
    if (std::holds_alternative<trp_information_request_t>(standard_decoded.value().value)) {
      if (!std::holds_alternative<amf_index_t>(ue_or_amf_index)) {
        logger.info("Dropping UE associated standard TRP Information Request. Cause: only non UE associated TRP "
                    "Information is supported");
        cu_cp_notifier.on_unsupported_nrppa_pdu("ue_associated_standard_trp_information_unsupported");
        return;
      }

      auto request = std::get<trp_information_request_t>(std::move(standard_decoded.value().value));
      if (!common_task_sched.schedule_async_task(
              handle_trp_information_request(std::move(request), std::get<amf_index_t>(ue_or_amf_index), true))) {
        logger.info("Dropping standard TRP Information Request. Cause: scheduling failed");
        cu_cp_notifier.on_unsupported_nrppa_pdu("standard_trp_information_schedule_failed");
      }
      return;
    }

    logger.info("Dropping standard NRPPa Transport PDU. Cause: unsupported payload procedure");
    cu_cp_notifier.on_unsupported_nrppa_pdu("standard_unsupported_payload_procedure");
    return;
  }

  auto decoded = decode_nrppa_pdu(nrppa_pdu);
  if (!decoded.has_value()) {
    logger.info("Dropping NRPPa Transport PDU. Cause: {}", decoded.error());
    cu_cp_notifier.on_unsupported_nrppa_pdu(decoded.error());
    return;
  }
  cu_cp_notifier.on_nrppa_standard_codec_event(
      {nrppa_standard_codec_event_type::minimal_fallback_decode, "minimal_nrppa_pdu"});

  if (std::holds_alternative<trp_information_request_t>(decoded.value().value)) {
    if (!std::holds_alternative<amf_index_t>(ue_or_amf_index)) {
      logger.info("Dropping UE associated TRP Information Request. Cause: only non UE associated TRP Information is supported");
      cu_cp_notifier.on_unsupported_nrppa_pdu("ue_associated_trp_information_unsupported");
      return;
    }

    auto request = std::get<trp_information_request_t>(std::move(decoded.value().value));
    if (!common_task_sched.schedule_async_task(
            handle_trp_information_request(std::move(request), std::get<amf_index_t>(ue_or_amf_index), false))) {
      logger.info("Dropping TRP Information Request. Cause: scheduling failed");
    }
    return;
  }

  if (std::holds_alternative<positioning_information_request_t>(decoded.value().value)) {
    if (!std::holds_alternative<ue_index_t>(ue_or_amf_index)) {
      logger.info("Dropping non UE associated Positioning Information Request. Cause: UE association is required");
      cu_cp_notifier.on_unsupported_nrppa_pdu("non_ue_positioning_information_unsupported");
      return;
    }

    auto request = std::get<positioning_information_request_t>(std::move(decoded.value().value));
    request.ue_index = std::get<ue_index_t>(ue_or_amf_index);
    if (!common_task_sched.schedule_async_task(
            handle_positioning_information_request(std::move(request), std::get<ue_index_t>(ue_or_amf_index)))) {
      logger.info("Dropping Positioning Information Request. Cause: scheduling failed");
      cu_cp_notifier.on_unsupported_nrppa_pdu("positioning_information_schedule_failed");
    }
    return;
  }

  if (std::holds_alternative<measurement_request_t>(decoded.value().value)) {
    if (!std::holds_alternative<ue_index_t>(ue_or_amf_index)) {
      logger.info("Dropping non UE associated Measurement Request. Cause: UE association is required");
      cu_cp_notifier.on_unsupported_nrppa_pdu("non_ue_measurement_unsupported");
      return;
    }

    auto request = std::get<measurement_request_t>(std::move(decoded.value().value));
    request.ue_index = std::get<ue_index_t>(ue_or_amf_index);
    if (!common_task_sched.schedule_async_task(
            handle_measurement_request(std::move(request), std::get<ue_index_t>(ue_or_amf_index)))) {
      logger.info("Dropping Measurement Request. Cause: scheduling failed");
      cu_cp_notifier.on_unsupported_nrppa_pdu("measurement_schedule_failed");
    }
    return;
  }

  if (std::holds_alternative<positioning_activation_request_t>(decoded.value().value)) {
    if (!std::holds_alternative<ue_index_t>(ue_or_amf_index)) {
      logger.info("Dropping non UE associated Positioning Activation Request. Cause: UE association is required");
      cu_cp_notifier.on_unsupported_nrppa_pdu("non_ue_positioning_activation_unsupported");
      return;
    }

    auto request = std::get<positioning_activation_request_t>(std::move(decoded.value().value));
    request.ue_index = std::get<ue_index_t>(ue_or_amf_index);
    if (!common_task_sched.schedule_async_task(
            handle_positioning_activation_request(std::move(request), std::get<ue_index_t>(ue_or_amf_index)))) {
      logger.info("Dropping Positioning Activation Request. Cause: scheduling failed");
      cu_cp_notifier.on_unsupported_nrppa_pdu("positioning_activation_schedule_failed");
    }
    return;
  }

  if (std::holds_alternative<positioning_deactivation_request_t>(decoded.value().value)) {
    if (!std::holds_alternative<ue_index_t>(ue_or_amf_index)) {
      logger.info("Dropping non UE associated Positioning Deactivation Request. Cause: UE association is required");
      cu_cp_notifier.on_unsupported_nrppa_pdu("non_ue_positioning_deactivation_unsupported");
      return;
    }

    auto request = std::get<positioning_deactivation_request_t>(std::move(decoded.value().value));
    request.ue_index = std::get<ue_index_t>(ue_or_amf_index);
    if (!common_task_sched.schedule_async_task(
            handle_positioning_deactivation_request(std::move(request), std::get<ue_index_t>(ue_or_amf_index)))) {
      logger.info("Dropping Positioning Deactivation Request. Cause: scheduling failed");
      cu_cp_notifier.on_unsupported_nrppa_pdu("positioning_deactivation_schedule_failed");
    }
    return;
  }

  if (std::holds_alternative<positioning_assistance_information_control_request_t>(decoded.value().value)) {
    if (!std::holds_alternative<amf_index_t>(ue_or_amf_index)) {
      logger.info("Dropping UE associated Positioning Assistance Information Control. Cause: only non UE associated "
                  "control is supported");
      cu_cp_notifier.on_unsupported_nrppa_pdu("ue_associated_positioning_assistance_information_unsupported");
      return;
    }

    auto request =
        std::get<positioning_assistance_information_control_request_t>(std::move(decoded.value().value));
    if (!common_task_sched.schedule_async_task(handle_positioning_assistance_information_control(
            std::move(request), std::get<amf_index_t>(ue_or_amf_index)))) {
      logger.info("Dropping Positioning Assistance Information Control. Cause: scheduling failed");
      cu_cp_notifier.on_unsupported_nrppa_pdu("positioning_assistance_information_schedule_failed");
    }
    return;
  }

  logger.info("Dropping NRPPa Transport PDU. Cause: unsupported payload procedure");
  cu_cp_notifier.on_unsupported_nrppa_pdu("unsupported_payload_procedure");
}

async_task<void> nrppa_dummy_impl::handle_trp_information_request(trp_information_request_t request,
                                                                  amf_index_t               amf_index,
                                                                  bool use_standard_codec)
{
  return launch_async([this, request = std::move(request), amf_index, use_standard_codec](
                          coro_context<async_task<void>>& ctx) mutable {
    CORO_BEGIN(ctx);
    CORO_AWAIT_VALUE(trp_information_cu_cp_response_t response, cu_cp_notifier.on_trp_information_request(request));
    response.transaction_id = request.transaction_id;
    if (use_standard_codec) {
      cu_cp_notifier.on_nrppa_standard_codec_event(
          {nrppa_standard_codec_event_type::encode_response, "trp_information_response"});
    }
    cu_cp_notifier.on_ul_nrppa_pdu(use_standard_codec ? encode_nrppa_standard_trp_information_response(response) :
                                                        encode_nrppa_trp_information_response(response),
                                   std::variant<ue_index_t, amf_index_t>{amf_index});
    CORO_RETURN();
  });
}

async_task<void> nrppa_dummy_impl::handle_measurement_request(measurement_request_t request, ue_index_t ue_index)
{
  return launch_async([this, request = std::move(request), ue_index](coro_context<async_task<void>>& ctx) mutable {
    expected<measurement_response_t, measurement_failure_t> outcome;
    CORO_BEGIN(ctx);
    CORO_AWAIT_VALUE(outcome, cu_cp_notifier.on_measurement_information_request(request));
    if (outcome.has_value()) {
      cu_cp_notifier.on_ul_nrppa_pdu(encode_nrppa_measurement_response(outcome.value()),
                                     std::variant<ue_index_t, amf_index_t>{ue_index});
    } else {
      cu_cp_notifier.on_ul_nrppa_pdu(encode_nrppa_measurement_failure(outcome.error()),
                                     std::variant<ue_index_t, amf_index_t>{ue_index});
    }
    CORO_RETURN();
  });
}

async_task<void>
nrppa_dummy_impl::handle_positioning_activation_request(positioning_activation_request_t request, ue_index_t ue_index)
{
  return launch_async([this, request = std::move(request), ue_index](coro_context<async_task<void>>& ctx) mutable {
    expected<positioning_activation_response_t, positioning_activation_failure_t> outcome;
    CORO_BEGIN(ctx);
    CORO_AWAIT_VALUE(outcome, cu_cp_notifier.on_positioning_activation_request(request));
    if (outcome.has_value()) {
      cu_cp_notifier.on_ul_nrppa_pdu(encode_nrppa_positioning_activation_response(outcome.value()),
                                     std::variant<ue_index_t, amf_index_t>{ue_index});
    } else {
      cu_cp_notifier.on_ul_nrppa_pdu(encode_nrppa_positioning_activation_failure(outcome.error()),
                                     std::variant<ue_index_t, amf_index_t>{ue_index});
    }
    CORO_RETURN();
  });
}

async_task<void>
nrppa_dummy_impl::handle_positioning_deactivation_request(positioning_deactivation_request_t request, ue_index_t ue_index)
{
  return launch_async([this, request = std::move(request), ue_index](coro_context<async_task<void>>& ctx) mutable {
    expected<positioning_deactivation_response_t, positioning_deactivation_failure_t> outcome;
    CORO_BEGIN(ctx);
    CORO_AWAIT_VALUE(outcome, cu_cp_notifier.on_positioning_deactivation_request(request));
    if (outcome.has_value()) {
      cu_cp_notifier.on_ul_nrppa_pdu(encode_nrppa_positioning_deactivation_response(outcome.value()),
                                     std::variant<ue_index_t, amf_index_t>{ue_index});
    } else {
      cu_cp_notifier.on_ul_nrppa_pdu(encode_nrppa_positioning_deactivation_failure(outcome.error()),
                                     std::variant<ue_index_t, amf_index_t>{ue_index});
    }
    CORO_RETURN();
  });
}

async_task<void> nrppa_dummy_impl::handle_positioning_assistance_information_control(
    positioning_assistance_information_control_request_t request, amf_index_t amf_index)
{
  return launch_async([this, request = std::move(request), amf_index](coro_context<async_task<void>>& ctx) mutable {
    expected<positioning_assistance_information_feedback_t, positioning_assistance_information_failure_t> outcome;
    CORO_BEGIN(ctx);
    CORO_AWAIT_VALUE(outcome, cu_cp_notifier.on_positioning_assistance_information_control(request));
    if (outcome.has_value()) {
      cu_cp_notifier.on_ul_nrppa_pdu(encode_nrppa_positioning_assistance_information_feedback(outcome.value()),
                                     std::variant<ue_index_t, amf_index_t>{amf_index});
    } else {
      cu_cp_notifier.on_ul_nrppa_pdu(encode_nrppa_positioning_assistance_information_failure(outcome.error()),
                                     std::variant<ue_index_t, amf_index_t>{amf_index});
    }
    CORO_RETURN();
  });
}

async_task<void>
nrppa_dummy_impl::handle_positioning_information_request(positioning_information_request_t request, ue_index_t ue_index)
{
  return launch_async([this, request = std::move(request), ue_index](coro_context<async_task<void>>& ctx) mutable {
    expected<positioning_information_response_t, positioning_information_failure_t> outcome;
    CORO_BEGIN(ctx);
    CORO_AWAIT_VALUE(outcome, cu_cp_notifier.on_positioning_information_request(request));
    if (outcome.has_value()) {
      cu_cp_notifier.on_ul_nrppa_pdu(encode_nrppa_positioning_information_response(outcome.value()),
                                     std::variant<ue_index_t, amf_index_t>{ue_index});
    } else {
      cu_cp_notifier.on_ul_nrppa_pdu(encode_nrppa_positioning_information_failure(outcome.error()),
                                     std::variant<ue_index_t, amf_index_t>{ue_index});
    }
    CORO_RETURN();
  });
}
