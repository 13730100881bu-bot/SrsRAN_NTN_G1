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

#include "lib/nrppa/nrppa_dummy_impl.h"
#include "srsran/nrppa/nrppa_pdu.h"
#include "srsran/support/async/fifo_async_task_scheduler.h"
#include <gtest/gtest.h>

using namespace srsran;
using namespace srs_cu_cp;

namespace {

class dummy_common_task_scheduler : public common_task_scheduler
{
public:
  bool schedule_async_task(async_task<void> task) override { return ctrl_loop.schedule(std::move(task)); }

private:
  fifo_async_task_scheduler ctrl_loop{16};
};

class dummy_nrppa_cu_cp_notifier : public nrppa_cu_cp_notifier
{
public:
  nrppa_cu_cp_ue_notifier* on_new_nrppa_ue(ue_index_t ue_index) override { return nullptr; }

  void on_ul_nrppa_pdu(const byte_buffer& pdu, std::variant<ue_index_t, amf_index_t> ue_or_amf_index) override
  {
    ++nof_ul_nrppa_pdus;
    last_ul_nrppa_pdu = pdu.copy();
    last_ul_index     = ue_or_amf_index;
  }

  async_task<trp_information_cu_cp_response_t>
  on_trp_information_request(const trp_information_request_t& request) override
  {
    ++nof_trp_information_requests;
    last_trp_information_request = request;

    trp_information_cu_cp_response_t response;
    response.transaction_id = request.transaction_id;

    trp_information_response_t du_response;
    trp_information_list_trp_response_item_t trp_item;
    trp_item.trp_info.trp_id = uint_to_trp_id(0x101);
    trp_item.trp_info.trp_info_type_resp_list.emplace_back(pci_t{7});
    du_response.trp_info_list_trp_resp.push_back(std::move(trp_item));
    response.trp_info_responses.emplace(du_index_t::min, std::move(du_response));

    return launch_async([response = std::move(response)](
                            coro_context<async_task<trp_information_cu_cp_response_t>>& ctx) mutable {
      CORO_BEGIN(ctx);
      CORO_RETURN(response);
    });
  }

  async_task<expected<positioning_information_response_t, positioning_information_failure_t>>
  on_positioning_information_request(const positioning_information_request_t& request) override
  {
    ++nof_positioning_information_requests;
    last_positioning_information_request = request;

    positioning_information_response_t response;
    response.sfn_initialization_time = 0x0102030405060708U;

    return launch_async([response](coro_context<async_task<expected<positioning_information_response_t,
                                                                 positioning_information_failure_t>>>& ctx) mutable {
      CORO_BEGIN(ctx);
      CORO_RETURN(response);
    });
  }

  async_task<expected<measurement_response_t, measurement_failure_t>>
  on_measurement_information_request(const measurement_request_t& request) override
  {
    ++nof_measurement_requests;
    last_measurement_request = request;

    measurement_response_t response;
    response.lmf_meas_id = request.lmf_meas_id;
    response.ran_meas_id = request.ran_meas_id;

    return launch_async([response](coro_context<async_task<expected<measurement_response_t,
                                                               measurement_failure_t>>>& ctx) mutable {
      CORO_BEGIN(ctx);
      CORO_RETURN(response);
    });
  }

  async_task<expected<positioning_activation_response_t, positioning_activation_failure_t>>
  on_positioning_activation_request(const positioning_activation_request_t& request) override
  {
    ++nof_positioning_activation_requests;
    last_positioning_activation_request = request;

    positioning_activation_response_t response;
    response.sys_frame_num = 123;
    response.slot_num      = 4;

    return launch_async([response](coro_context<async_task<expected<positioning_activation_response_t,
                                                                  positioning_activation_failure_t>>>& ctx) mutable {
      CORO_BEGIN(ctx);
      CORO_RETURN(response);
    });
  }

  async_task<expected<positioning_deactivation_response_t, positioning_deactivation_failure_t>>
  on_positioning_deactivation_request(const positioning_deactivation_request_t& request) override
  {
    ++nof_positioning_deactivation_requests;
    last_positioning_deactivation_request = request;

    positioning_deactivation_response_t response;
    return launch_async([response](coro_context<async_task<expected<positioning_deactivation_response_t,
                                                                    positioning_deactivation_failure_t>>>& ctx) mutable {
      CORO_BEGIN(ctx);
      CORO_RETURN(response);
    });
  }

  async_task<expected<positioning_assistance_information_feedback_t, positioning_assistance_information_failure_t>>
  on_positioning_assistance_information_control(
      const positioning_assistance_information_control_request_t& request) override
  {
    ++nof_positioning_assistance_information_control_requests;
    last_positioning_assistance_information_control_request = request;

    positioning_assistance_information_feedback_t feedback;
    feedback.transaction_id = request.transaction_id;
    feedback.routing_id     = request.routing_id.has_value() ? std::make_optional(request.routing_id->copy()) :
                                                                std::nullopt;
    feedback.positioning_broadcast_cells = request.positioning_broadcast_cells;

    return launch_async([feedback = std::move(feedback)](
                            coro_context<async_task<expected<positioning_assistance_information_feedback_t,
                                                               positioning_assistance_information_failure_t>>>&
                                ctx) mutable {
      CORO_BEGIN(ctx);
      CORO_RETURN(feedback);
    });
  }

  void on_unsupported_nrppa_pdu(std::string_view reason) override
  {
    ++nof_unsupported_nrppa_pdus;
    last_unsupported_reason = std::string(reason);
  }

  void on_nrppa_standard_codec_event(const nrppa_standard_codec_event& event) override
  {
    switch (event.type) {
      case nrppa_standard_codec_event_type::decode_success:
        ++nof_standard_decode_success;
        break;
      case nrppa_standard_codec_event_type::decode_failure:
        ++nof_standard_decode_failure;
        break;
      case nrppa_standard_codec_event_type::encode_response:
        ++nof_standard_encode_response;
        break;
      case nrppa_standard_codec_event_type::encode_failure:
        ++nof_standard_encode_failure;
        break;
      case nrppa_standard_codec_event_type::minimal_fallback_decode:
        ++nof_minimal_fallback_decodes;
        break;
    }
    last_standard_codec_reason = std::string(event.reason);
  }

  unsigned nof_trp_information_requests = 0;
  std::optional<trp_information_request_t> last_trp_information_request;
  unsigned nof_positioning_information_requests = 0;
  std::optional<positioning_information_request_t> last_positioning_information_request;
  unsigned nof_measurement_requests = 0;
  std::optional<measurement_request_t> last_measurement_request;
  unsigned nof_positioning_activation_requests = 0;
  std::optional<positioning_activation_request_t> last_positioning_activation_request;
  unsigned nof_positioning_deactivation_requests = 0;
  std::optional<positioning_deactivation_request_t> last_positioning_deactivation_request;
  unsigned nof_positioning_assistance_information_control_requests = 0;
  std::optional<positioning_assistance_information_control_request_t>
      last_positioning_assistance_information_control_request;
  unsigned nof_ul_nrppa_pdus = 0;
  byte_buffer last_ul_nrppa_pdu;
  std::optional<std::variant<ue_index_t, amf_index_t>> last_ul_index;
  unsigned nof_unsupported_nrppa_pdus = 0;
  std::string last_unsupported_reason;
  unsigned nof_standard_decode_success = 0;
  unsigned nof_standard_decode_failure = 0;
  unsigned nof_standard_encode_response = 0;
  unsigned nof_standard_encode_failure = 0;
  unsigned nof_minimal_fallback_decodes = 0;
  std::string last_standard_codec_reason;
};

} // namespace

TEST(nrppa_trp_information, request_decodes_and_response_encodes)
{
  trp_information_request_t request;
  request.transaction_id = 37;
  request.trp_list.push_back(uint_to_trp_id(0x101));
  request.trp_info_type_list_trp_req.push_back(trp_information_type_item_t::nr_pci);
  request.trp_info_type_list_trp_req.push_back(trp_information_type_item_t::ng_ran_cgi);

  byte_buffer encoded_request = encode_nrppa_trp_information_request(request);
  auto        decoded_request = decode_nrppa_pdu(encoded_request);
  ASSERT_TRUE(decoded_request.has_value());
  ASSERT_TRUE(std::holds_alternative<trp_information_request_t>(decoded_request.value().value));

  const auto& decoded = std::get<trp_information_request_t>(decoded_request.value().value);
  ASSERT_EQ(decoded.transaction_id, 37);
  ASSERT_EQ(decoded.trp_list.size(), 1);
  ASSERT_EQ(decoded.trp_list.front(), uint_to_trp_id(0x101));
  ASSERT_EQ(decoded.trp_info_type_list_trp_req.size(), 2);
  ASSERT_EQ(decoded.trp_info_type_list_trp_req[0], trp_information_type_item_t::nr_pci);
  ASSERT_EQ(decoded.trp_info_type_list_trp_req[1], trp_information_type_item_t::ng_ran_cgi);

  trp_information_cu_cp_response_t response;
  response.transaction_id = decoded.transaction_id;
  trp_information_response_t du_response;
  trp_information_list_trp_response_item_t trp_item;
  trp_item.trp_info.trp_id = uint_to_trp_id(0x101);
  trp_item.trp_info.trp_info_type_resp_list.emplace_back(pci_t{7});
  trp_item.trp_info.trp_info_type_resp_list.emplace_back(nr_cell_global_id_t{plmn_identity::test_value(),
                                                                             nr_cell_identity::create(0x66c000).value()});
  du_response.trp_info_list_trp_resp.push_back(std::move(trp_item));
  response.trp_info_responses.emplace(du_index_t::min, std::move(du_response));

  byte_buffer encoded_response = encode_nrppa_trp_information_response(response);
  auto        decoded_response = decode_nrppa_pdu(encoded_response);
  ASSERT_TRUE(decoded_response.has_value());
  ASSERT_TRUE(std::holds_alternative<trp_information_cu_cp_response_t>(decoded_response.value().value));

  const auto& roundtrip = std::get<trp_information_cu_cp_response_t>(decoded_response.value().value);
  ASSERT_EQ(roundtrip.transaction_id, 37);
  ASSERT_EQ(roundtrip.trp_info_responses.size(), 1);
  ASSERT_EQ(roundtrip.trp_info_responses.begin()->second.trp_info_list_trp_resp.front().trp_info.trp_id,
            uint_to_trp_id(0x101));
}

TEST(nrppa_standard_trp_information, request_decodes_from_aper_without_minimal_magic)
{
  trp_information_request_t request;
  request.transaction_id = 37;
  request.trp_list.push_back(uint_to_trp_id(0x101));
  request.trp_info_type_list_trp_req.push_back(trp_information_type_item_t::nr_pci);
  request.trp_info_type_list_trp_req.push_back(trp_information_type_item_t::ng_ran_cgi);
  request.trp_info_type_list_trp_req.push_back(trp_information_type_item_t::arfcn);

  byte_buffer encoded_request = encode_nrppa_standard_trp_information_request(request);
  ASSERT_GT(encoded_request.length(), 4);
  ASSERT_FALSE(encoded_request[0] == 'N' && encoded_request[1] == 'P' && encoded_request[2] == 'P' &&
               encoded_request[3] == 'A');

  auto decoded_request = decode_nrppa_standard_pdu(encoded_request);
  ASSERT_TRUE(decoded_request.has_value()) << decoded_request.error();
  ASSERT_TRUE(std::holds_alternative<trp_information_request_t>(decoded_request.value().value));

  const auto& decoded = std::get<trp_information_request_t>(decoded_request.value().value);
  ASSERT_EQ(decoded.transaction_id, 37);
  ASSERT_EQ(decoded.trp_list.size(), 1);
  ASSERT_EQ(decoded.trp_list.front(), uint_to_trp_id(0x101));
  ASSERT_EQ(decoded.trp_info_type_list_trp_req.size(), 3);
  ASSERT_EQ(decoded.trp_info_type_list_trp_req[0], trp_information_type_item_t::nr_pci);
  ASSERT_EQ(decoded.trp_info_type_list_trp_req[1], trp_information_type_item_t::ng_ran_cgi);
  ASSERT_EQ(decoded.trp_info_type_list_trp_req[2], trp_information_type_item_t::arfcn);
}

TEST(nrppa_standard_trp_information, response_encodes_and_roundtrips)
{
  trp_information_cu_cp_response_t response;
  response.transaction_id = 41;

  trp_information_response_t du_response;
  trp_information_list_trp_response_item_t trp_item;
  trp_item.trp_info.trp_id = uint_to_trp_id(0x101);
  trp_item.trp_info.trp_info_type_resp_list.emplace_back(pci_t{7});
  trp_item.trp_info.trp_info_type_resp_list.emplace_back(nr_cell_global_id_t{plmn_identity::test_value(),
                                                                             nr_cell_identity::create(0x66c000).value()});
  trp_item.trp_info.trp_info_type_resp_list.emplace_back(uint32_t{635040});
  trp_item.trp_info.trp_info_type_resp_list.emplace_back(trp_type_t::trp);
  du_response.trp_info_list_trp_resp.push_back(std::move(trp_item));
  response.trp_info_responses.emplace(du_index_t::min, std::move(du_response));

  byte_buffer encoded_response = encode_nrppa_standard_trp_information_response(response);
  ASSERT_GT(encoded_response.length(), 4);

  auto decoded_response = decode_nrppa_standard_pdu(encoded_response);
  ASSERT_TRUE(decoded_response.has_value()) << decoded_response.error();
  ASSERT_TRUE(std::holds_alternative<trp_information_cu_cp_response_t>(decoded_response.value().value));

  const auto& roundtrip = std::get<trp_information_cu_cp_response_t>(decoded_response.value().value);
  ASSERT_EQ(roundtrip.transaction_id, 41);
  ASSERT_EQ(roundtrip.trp_info_responses.size(), 1);
  const auto& decoded_trp = roundtrip.trp_info_responses.begin()->second.trp_info_list_trp_resp.front().trp_info;
  ASSERT_EQ(decoded_trp.trp_id, uint_to_trp_id(0x101));
  ASSERT_EQ(decoded_trp.trp_info_type_resp_list.size(), 4);
  ASSERT_EQ(std::get<pci_t>(decoded_trp.trp_info_type_resp_list[0]), pci_t{7});
  ASSERT_EQ(std::get<nr_cell_global_id_t>(decoded_trp.trp_info_type_resp_list[1]).nci,
            nr_cell_identity::create(0x66c000).value());
  ASSERT_EQ(std::get<uint32_t>(decoded_trp.trp_info_type_resp_list[2]), 635040);
  ASSERT_EQ(std::get<trp_type_t>(decoded_trp.trp_info_type_resp_list[3]), trp_type_t::trp);
}

TEST(nrppa_standard_trp_information, dummy_endpoint_prefers_standard_request_and_returns_standard_response)
{
  dummy_nrppa_cu_cp_notifier notifier;
  dummy_common_task_scheduler scheduler;
  nrppa_dummy_impl           nrppa(notifier, scheduler);

  trp_information_request_t request;
  request.transaction_id = 11;
  request.trp_list.push_back(uint_to_trp_id(0x101));
  request.trp_info_type_list_trp_req.push_back(trp_information_type_item_t::nr_pci);

  nrppa.handle_new_nrppa_pdu(encode_nrppa_standard_trp_information_request(request),
                             std::variant<ue_index_t, amf_index_t>{amf_index_t::min});

  ASSERT_EQ(notifier.nof_trp_information_requests, 1);
  ASSERT_EQ(notifier.nof_standard_decode_success, 1);
  ASSERT_EQ(notifier.nof_standard_encode_response, 1);
  ASSERT_EQ(notifier.nof_minimal_fallback_decodes, 0);
  ASSERT_EQ(notifier.nof_ul_nrppa_pdus, 1);
  ASSERT_TRUE(notifier.last_ul_index.has_value());
  ASSERT_TRUE(std::holds_alternative<amf_index_t>(notifier.last_ul_index.value()));

  auto minimal_response = decode_nrppa_pdu(notifier.last_ul_nrppa_pdu);
  ASSERT_FALSE(minimal_response.has_value());

  auto standard_response = decode_nrppa_standard_pdu(notifier.last_ul_nrppa_pdu);
  ASSERT_TRUE(standard_response.has_value()) << standard_response.error();
  ASSERT_TRUE(std::holds_alternative<trp_information_cu_cp_response_t>(standard_response.value().value));
  ASSERT_EQ(std::get<trp_information_cu_cp_response_t>(standard_response.value().value).transaction_id, 11);
}

TEST(nrppa_standard_trp_information, malformed_standard_payload_is_counted_and_dropped)
{
  dummy_nrppa_cu_cp_notifier notifier;
  dummy_common_task_scheduler scheduler;
  nrppa_dummy_impl           nrppa(notifier, scheduler);

  byte_buffer malformed = byte_buffer::create({0x20, 0x01, 0x02, 0x03}).value();
  nrppa.handle_new_nrppa_pdu(malformed, std::variant<ue_index_t, amf_index_t>{amf_index_t::min});

  ASSERT_EQ(notifier.nof_trp_information_requests, 0);
  ASSERT_EQ(notifier.nof_standard_decode_failure, 1);
  ASSERT_EQ(notifier.nof_unsupported_nrppa_pdus, 1);
  ASSERT_NE(notifier.last_standard_codec_reason.find("standard"), std::string::npos);
}

TEST(nrppa_trp_information, dummy_endpoint_forwards_trp_request_and_returns_ul_transport)
{
  dummy_nrppa_cu_cp_notifier notifier;
  dummy_common_task_scheduler scheduler;
  nrppa_dummy_impl           nrppa(notifier, scheduler);

  trp_information_request_t request;
  request.transaction_id = 11;
  request.trp_list.push_back(uint_to_trp_id(0x101));
  request.trp_info_type_list_trp_req.push_back(trp_information_type_item_t::nr_pci);

  nrppa.handle_new_nrppa_pdu(encode_nrppa_trp_information_request(request),
                             std::variant<ue_index_t, amf_index_t>{amf_index_t::min});

  ASSERT_EQ(notifier.nof_trp_information_requests, 1);
  ASSERT_TRUE(notifier.last_trp_information_request.has_value());
  ASSERT_EQ(notifier.last_trp_information_request->transaction_id, 11);
  ASSERT_EQ(notifier.last_trp_information_request->trp_list.front(), uint_to_trp_id(0x101));
  ASSERT_EQ(notifier.nof_ul_nrppa_pdus, 1);
  ASSERT_TRUE(notifier.last_ul_index.has_value());
  ASSERT_TRUE(std::holds_alternative<amf_index_t>(notifier.last_ul_index.value()));
  ASSERT_EQ(std::get<amf_index_t>(notifier.last_ul_index.value()), amf_index_t::min);

  auto decoded_response = decode_nrppa_pdu(notifier.last_ul_nrppa_pdu);
  ASSERT_TRUE(decoded_response.has_value());
  ASSERT_TRUE(std::holds_alternative<trp_information_cu_cp_response_t>(decoded_response.value().value));
  ASSERT_EQ(std::get<trp_information_cu_cp_response_t>(decoded_response.value().value).transaction_id, 11);
}

TEST(nrppa_positioning_assistance_information, control_request_decodes_and_feedback_encodes)
{
  positioning_assistance_information_control_request_t request;
  request.transaction_id = 37;
  request.pos_assist_info = byte_buffer::create({0x10, 0x20, 0x30}).value();
  request.pos_broadcast = positioning_assistance_broadcast_action::start;
  request.positioning_broadcast_cells.push_back(
      nr_cell_global_id_t{plmn_identity::test_value(), nr_cell_identity::create(0x66c000).value()});
  request.routing_id = byte_buffer::create({0xaa, 0xbb}).value();

  byte_buffer encoded_request = encode_nrppa_positioning_assistance_information_control_request(request);
  auto        decoded_request = decode_nrppa_pdu(encoded_request);
  ASSERT_TRUE(decoded_request.has_value());
  ASSERT_TRUE(std::holds_alternative<positioning_assistance_information_control_request_t>(
      decoded_request.value().value));

  const auto& decoded =
      std::get<positioning_assistance_information_control_request_t>(decoded_request.value().value);
  ASSERT_EQ(decoded.transaction_id, 37);
  ASSERT_TRUE(decoded.pos_assist_info.has_value());
  ASSERT_EQ(decoded.pos_assist_info.value(), request.pos_assist_info.value());
  ASSERT_TRUE(decoded.pos_broadcast.has_value());
  ASSERT_EQ(decoded.pos_broadcast.value(), positioning_assistance_broadcast_action::start);
  ASSERT_EQ(decoded.positioning_broadcast_cells.size(), 1);
  ASSERT_EQ(decoded.positioning_broadcast_cells.front().nci, request.positioning_broadcast_cells.front().nci);
  ASSERT_TRUE(decoded.routing_id.has_value());
  ASSERT_EQ(decoded.routing_id.value(), request.routing_id.value());

  positioning_assistance_information_feedback_t feedback;
  feedback.transaction_id = decoded.transaction_id;
  feedback.pos_assist_info_fail_list = byte_buffer::create({0x01, 0x02}).value();
  feedback.positioning_broadcast_cells = decoded.positioning_broadcast_cells;
  feedback.routing_id = decoded.routing_id->copy();

  byte_buffer encoded_feedback = encode_nrppa_positioning_assistance_information_feedback(feedback);
  auto        decoded_feedback = decode_nrppa_pdu(encoded_feedback);
  ASSERT_TRUE(decoded_feedback.has_value());
  ASSERT_TRUE(
      std::holds_alternative<positioning_assistance_information_feedback_t>(decoded_feedback.value().value));

  const auto& roundtrip =
      std::get<positioning_assistance_information_feedback_t>(decoded_feedback.value().value);
  ASSERT_EQ(roundtrip.transaction_id, 37);
  ASSERT_TRUE(roundtrip.pos_assist_info_fail_list.has_value());
  ASSERT_EQ(roundtrip.pos_assist_info_fail_list.value(), feedback.pos_assist_info_fail_list.value());
  ASSERT_EQ(roundtrip.positioning_broadcast_cells.size(), 1);
  ASSERT_TRUE(roundtrip.routing_id.has_value());
  ASSERT_EQ(roundtrip.routing_id.value(), feedback.routing_id.value());
}

TEST(nrppa_positioning_assistance_information, dummy_endpoint_forwards_non_ue_control_and_returns_ul_transport)
{
  dummy_nrppa_cu_cp_notifier notifier;
  dummy_common_task_scheduler scheduler;
  nrppa_dummy_impl           nrppa(notifier, scheduler);

  positioning_assistance_information_control_request_t request;
  request.transaction_id = 12;
  request.pos_broadcast = positioning_assistance_broadcast_action::stop;
  request.positioning_broadcast_cells.push_back(
      nr_cell_global_id_t{plmn_identity::test_value(), nr_cell_identity::create(0x66c000).value()});
  request.routing_id = byte_buffer::create({0x44}).value();

  nrppa.handle_new_nrppa_pdu(encode_nrppa_positioning_assistance_information_control_request(request),
                             std::variant<ue_index_t, amf_index_t>{amf_index_t::min});

  ASSERT_EQ(notifier.nof_positioning_assistance_information_control_requests, 1);
  ASSERT_TRUE(notifier.last_positioning_assistance_information_control_request.has_value());
  ASSERT_EQ(notifier.last_positioning_assistance_information_control_request->transaction_id, 12);
  ASSERT_EQ(notifier.last_positioning_assistance_information_control_request->pos_broadcast.value(),
            positioning_assistance_broadcast_action::stop);
  ASSERT_EQ(notifier.nof_ul_nrppa_pdus, 1);
  ASSERT_TRUE(notifier.last_ul_index.has_value());
  ASSERT_TRUE(std::holds_alternative<amf_index_t>(notifier.last_ul_index.value()));
  ASSERT_EQ(std::get<amf_index_t>(notifier.last_ul_index.value()), amf_index_t::min);

  auto decoded_feedback = decode_nrppa_pdu(notifier.last_ul_nrppa_pdu);
  ASSERT_TRUE(decoded_feedback.has_value());
  ASSERT_TRUE(
      std::holds_alternative<positioning_assistance_information_feedback_t>(decoded_feedback.value().value));
  ASSERT_EQ(std::get<positioning_assistance_information_feedback_t>(decoded_feedback.value().value).transaction_id, 12);
}

TEST(nrppa_positioning_assistance_information, ue_associated_control_is_counted_as_unsupported)
{
  dummy_nrppa_cu_cp_notifier notifier;
  dummy_common_task_scheduler scheduler;
  nrppa_dummy_impl           nrppa(notifier, scheduler);

  positioning_assistance_information_control_request_t request;
  request.transaction_id = 13;

  nrppa.handle_new_nrppa_pdu(encode_nrppa_positioning_assistance_information_control_request(request),
                             std::variant<ue_index_t, amf_index_t>{uint_to_ue_index(5)});

  ASSERT_EQ(notifier.nof_positioning_assistance_information_control_requests, 0);
  ASSERT_EQ(notifier.nof_ul_nrppa_pdus, 0);
  ASSERT_EQ(notifier.nof_unsupported_nrppa_pdus, 1);
  ASSERT_EQ(notifier.last_unsupported_reason, "ue_associated_positioning_assistance_information_unsupported");
}

TEST(nrppa_positioning_information, request_decodes_and_response_encodes)
{
  positioning_information_request_t request;
  request.ue_index = uint_to_ue_index(7);

  byte_buffer encoded_request = encode_nrppa_positioning_information_request(request);
  auto        decoded_request = decode_nrppa_pdu(encoded_request);
  ASSERT_TRUE(decoded_request.has_value());
  ASSERT_TRUE(std::holds_alternative<positioning_information_request_t>(decoded_request.value().value));

  const auto& decoded = std::get<positioning_information_request_t>(decoded_request.value().value);
  ASSERT_EQ(decoded.ue_index, ue_index_t::invalid);
  ASSERT_FALSE(decoded.requested_srs_tx_characteristics.has_value());
  ASSERT_FALSE(decoded.ue_report_info.has_value());

  positioning_information_response_t response;
  response.sfn_initialization_time = 0x0102030405060708U;

  byte_buffer encoded_response = encode_nrppa_positioning_information_response(response);
  auto        decoded_response = decode_nrppa_pdu(encoded_response);
  ASSERT_TRUE(decoded_response.has_value());
  ASSERT_TRUE(std::holds_alternative<positioning_information_response_t>(decoded_response.value().value));

  const auto& roundtrip = std::get<positioning_information_response_t>(decoded_response.value().value);
  ASSERT_TRUE(roundtrip.sfn_initialization_time.has_value());
  ASSERT_EQ(roundtrip.sfn_initialization_time.value(), response.sfn_initialization_time.value());
}

TEST(nrppa_positioning_information, dummy_endpoint_forwards_ue_request_and_returns_ul_transport)
{
  dummy_nrppa_cu_cp_notifier notifier;
  dummy_common_task_scheduler scheduler;
  nrppa_dummy_impl           nrppa(notifier, scheduler);

  positioning_information_request_t request;
  request.ue_index = ue_index_t::invalid;
  const ue_index_t ue_index = uint_to_ue_index(5);

  nrppa.handle_new_nrppa_pdu(encode_nrppa_positioning_information_request(request),
                             std::variant<ue_index_t, amf_index_t>{ue_index});

  ASSERT_EQ(notifier.nof_positioning_information_requests, 1);
  ASSERT_TRUE(notifier.last_positioning_information_request.has_value());
  ASSERT_EQ(notifier.last_positioning_information_request->ue_index, ue_index);
  ASSERT_EQ(notifier.nof_ul_nrppa_pdus, 1);
  ASSERT_TRUE(notifier.last_ul_index.has_value());
  ASSERT_TRUE(std::holds_alternative<ue_index_t>(notifier.last_ul_index.value()));
  ASSERT_EQ(std::get<ue_index_t>(notifier.last_ul_index.value()), ue_index);

  auto decoded_response = decode_nrppa_pdu(notifier.last_ul_nrppa_pdu);
  ASSERT_TRUE(decoded_response.has_value());
  ASSERT_TRUE(std::holds_alternative<positioning_information_response_t>(decoded_response.value().value));
  const auto& response = std::get<positioning_information_response_t>(decoded_response.value().value);
  ASSERT_TRUE(response.sfn_initialization_time.has_value());
}

TEST(nrppa_measurement, request_decodes_and_response_encodes)
{
  measurement_request_t request;
  request.ue_index    = uint_to_ue_index(7);
  request.lmf_meas_id = uint_to_lmf_meas_id(23);
  request.ran_meas_id = uint_to_ran_meas_id(24);
  request.trp_meas_request_list.push_back({uint_to_trp_id(0x101)});
  request.trp_meas_quantities.push_back({trp_meas_quantities_item_t::ul_rtoa});

  byte_buffer encoded_request = encode_nrppa_measurement_request(request);
  auto        decoded_request = decode_nrppa_pdu(encoded_request);
  ASSERT_TRUE(decoded_request.has_value());
  ASSERT_TRUE(std::holds_alternative<measurement_request_t>(decoded_request.value().value));

  const auto& decoded = std::get<measurement_request_t>(decoded_request.value().value);
  ASSERT_EQ(decoded.ue_index, ue_index_t::invalid);
  ASSERT_EQ(decoded.lmf_meas_id, request.lmf_meas_id);
  ASSERT_EQ(decoded.ran_meas_id, request.ran_meas_id);
  ASSERT_EQ(decoded.trp_meas_request_list.size(), 1);
  ASSERT_EQ(decoded.trp_meas_request_list.front().trp_id, uint_to_trp_id(0x101));
  ASSERT_EQ(decoded.trp_meas_quantities.size(), 1);
  ASSERT_EQ(decoded.trp_meas_quantities.front().trp_meas_quantities_item, trp_meas_quantities_item_t::ul_rtoa);

  measurement_response_t response;
  response.lmf_meas_id = decoded.lmf_meas_id;
  response.ran_meas_id = decoded.ran_meas_id;

  byte_buffer encoded_response = encode_nrppa_measurement_response(response);
  auto        decoded_response = decode_nrppa_pdu(encoded_response);
  ASSERT_TRUE(decoded_response.has_value());
  ASSERT_TRUE(std::holds_alternative<measurement_response_t>(decoded_response.value().value));
  const auto& roundtrip_response = std::get<measurement_response_t>(decoded_response.value().value);
  ASSERT_EQ(roundtrip_response.lmf_meas_id, response.lmf_meas_id);
  ASSERT_EQ(roundtrip_response.ran_meas_id, response.ran_meas_id);

  measurement_failure_t failure;
  failure.lmf_meas_id = decoded.lmf_meas_id;
  failure.cause       = nrppa_cause_misc_t::unspecified;

  byte_buffer encoded_failure = encode_nrppa_measurement_failure(failure);
  auto        decoded_failure = decode_nrppa_pdu(encoded_failure);
  ASSERT_TRUE(decoded_failure.has_value());
  ASSERT_TRUE(std::holds_alternative<measurement_failure_t>(decoded_failure.value().value));
  ASSERT_EQ(std::get<measurement_failure_t>(decoded_failure.value().value).lmf_meas_id, failure.lmf_meas_id);
}

TEST(nrppa_measurement, dummy_endpoint_forwards_ue_request_and_returns_ul_transport)
{
  dummy_nrppa_cu_cp_notifier notifier;
  dummy_common_task_scheduler scheduler;
  nrppa_dummy_impl           nrppa(notifier, scheduler);

  measurement_request_t request;
  request.ue_index    = ue_index_t::invalid;
  request.lmf_meas_id = uint_to_lmf_meas_id(31);
  request.ran_meas_id = uint_to_ran_meas_id(32);
  request.trp_meas_request_list.push_back({uint_to_trp_id(0x202)});
  request.trp_meas_quantities.push_back({trp_meas_quantities_item_t::gnb_rx_tx_time_diff});
  const ue_index_t ue_index = uint_to_ue_index(5);

  nrppa.handle_new_nrppa_pdu(encode_nrppa_measurement_request(request),
                             std::variant<ue_index_t, amf_index_t>{ue_index});

  ASSERT_EQ(notifier.nof_measurement_requests, 1);
  ASSERT_TRUE(notifier.last_measurement_request.has_value());
  ASSERT_EQ(notifier.last_measurement_request->ue_index, ue_index);
  ASSERT_EQ(notifier.last_measurement_request->lmf_meas_id, request.lmf_meas_id);
  ASSERT_EQ(notifier.nof_ul_nrppa_pdus, 1);
  ASSERT_TRUE(notifier.last_ul_index.has_value());
  ASSERT_TRUE(std::holds_alternative<ue_index_t>(notifier.last_ul_index.value()));
  ASSERT_EQ(std::get<ue_index_t>(notifier.last_ul_index.value()), ue_index);

  auto decoded_response = decode_nrppa_pdu(notifier.last_ul_nrppa_pdu);
  ASSERT_TRUE(decoded_response.has_value());
  ASSERT_TRUE(std::holds_alternative<measurement_response_t>(decoded_response.value().value));
  ASSERT_EQ(std::get<measurement_response_t>(decoded_response.value().value).lmf_meas_id, request.lmf_meas_id);
}

TEST(nrppa_positioning_activation, request_decodes_and_response_encodes)
{
  positioning_activation_request_t request;
  request.ue_index = uint_to_ue_index(7);
  request.srs_type = aperiodic_srs_t{true, std::nullopt};
  request.activation_time = 0x0102030405060708U;

  byte_buffer encoded_request = encode_nrppa_positioning_activation_request(request);
  auto        decoded_request = decode_nrppa_pdu(encoded_request);
  ASSERT_TRUE(decoded_request.has_value());
  ASSERT_TRUE(std::holds_alternative<positioning_activation_request_t>(decoded_request.value().value));

  const auto& decoded = std::get<positioning_activation_request_t>(decoded_request.value().value);
  ASSERT_EQ(decoded.ue_index, ue_index_t::invalid);
  ASSERT_TRUE(std::holds_alternative<aperiodic_srs_t>(decoded.srs_type));
  ASSERT_TRUE(std::get<aperiodic_srs_t>(decoded.srs_type).aperiodic);
  ASSERT_TRUE(decoded.activation_time.has_value());
  ASSERT_EQ(decoded.activation_time.value(), request.activation_time.value());

  positioning_activation_response_t response;
  response.sys_frame_num = 42;
  response.slot_num      = 11;

  byte_buffer encoded_response = encode_nrppa_positioning_activation_response(response);
  auto        decoded_response = decode_nrppa_pdu(encoded_response);
  ASSERT_TRUE(decoded_response.has_value());
  ASSERT_TRUE(std::holds_alternative<positioning_activation_response_t>(decoded_response.value().value));
  const auto& roundtrip_response = std::get<positioning_activation_response_t>(decoded_response.value().value);
  ASSERT_TRUE(roundtrip_response.sys_frame_num.has_value());
  ASSERT_TRUE(roundtrip_response.slot_num.has_value());
  ASSERT_EQ(roundtrip_response.sys_frame_num.value(), response.sys_frame_num.value());
  ASSERT_EQ(roundtrip_response.slot_num.value(), response.slot_num.value());

  positioning_activation_failure_t failure;
  failure.cause = nrppa_cause_misc_t::unspecified;

  byte_buffer encoded_failure = encode_nrppa_positioning_activation_failure(failure);
  auto        decoded_failure = decode_nrppa_pdu(encoded_failure);
  ASSERT_TRUE(decoded_failure.has_value());
  ASSERT_TRUE(std::holds_alternative<positioning_activation_failure_t>(decoded_failure.value().value));
}

TEST(nrppa_positioning_activation, dummy_endpoint_forwards_ue_request_and_returns_ul_transport)
{
  dummy_nrppa_cu_cp_notifier notifier;
  dummy_common_task_scheduler scheduler;
  nrppa_dummy_impl           nrppa(notifier, scheduler);

  positioning_activation_request_t request;
  request.ue_index = ue_index_t::invalid;
  request.srs_type = aperiodic_srs_t{true, std::nullopt};
  const ue_index_t ue_index = uint_to_ue_index(5);

  nrppa.handle_new_nrppa_pdu(encode_nrppa_positioning_activation_request(request),
                             std::variant<ue_index_t, amf_index_t>{ue_index});

  ASSERT_EQ(notifier.nof_positioning_activation_requests, 1);
  ASSERT_TRUE(notifier.last_positioning_activation_request.has_value());
  ASSERT_EQ(notifier.last_positioning_activation_request->ue_index, ue_index);
  ASSERT_EQ(notifier.nof_ul_nrppa_pdus, 1);
  ASSERT_TRUE(notifier.last_ul_index.has_value());
  ASSERT_TRUE(std::holds_alternative<ue_index_t>(notifier.last_ul_index.value()));
  ASSERT_EQ(std::get<ue_index_t>(notifier.last_ul_index.value()), ue_index);

  auto decoded_response = decode_nrppa_pdu(notifier.last_ul_nrppa_pdu);
  ASSERT_TRUE(decoded_response.has_value());
  ASSERT_TRUE(std::holds_alternative<positioning_activation_response_t>(decoded_response.value().value));
  const auto& response = std::get<positioning_activation_response_t>(decoded_response.value().value);
  ASSERT_EQ(response.sys_frame_num.value(), 123);
  ASSERT_EQ(response.slot_num.value(), 4);
}

TEST(nrppa_positioning_deactivation, request_decodes_and_ack_encodes)
{
  positioning_deactivation_request_t request;
  request.ue_index       = uint_to_ue_index(7);
  request.srs_res_set_id = 3;

  byte_buffer encoded_request = encode_nrppa_positioning_deactivation_request(request);
  auto        decoded_request = decode_nrppa_pdu(encoded_request);
  ASSERT_TRUE(decoded_request.has_value());
  ASSERT_TRUE(std::holds_alternative<positioning_deactivation_request_t>(decoded_request.value().value));

  const auto& decoded = std::get<positioning_deactivation_request_t>(decoded_request.value().value);
  ASSERT_EQ(decoded.ue_index, ue_index_t::invalid);
  ASSERT_TRUE(decoded.srs_res_set_id.has_value());
  ASSERT_EQ(decoded.srs_res_set_id.value(), request.srs_res_set_id.value());

  positioning_deactivation_response_t response;
  byte_buffer encoded_response = encode_nrppa_positioning_deactivation_response(response);
  auto        decoded_response = decode_nrppa_pdu(encoded_response);
  ASSERT_TRUE(decoded_response.has_value());
  ASSERT_TRUE(std::holds_alternative<positioning_deactivation_response_t>(decoded_response.value().value));

  positioning_deactivation_failure_t failure;
  failure.cause = nrppa_cause_misc_t::unspecified;

  byte_buffer encoded_failure = encode_nrppa_positioning_deactivation_failure(failure);
  auto        decoded_failure = decode_nrppa_pdu(encoded_failure);
  ASSERT_TRUE(decoded_failure.has_value());
  ASSERT_TRUE(std::holds_alternative<positioning_deactivation_failure_t>(decoded_failure.value().value));
}

TEST(nrppa_positioning_deactivation, dummy_endpoint_forwards_ue_request_and_returns_ack)
{
  dummy_nrppa_cu_cp_notifier notifier;
  dummy_common_task_scheduler scheduler;
  nrppa_dummy_impl           nrppa(notifier, scheduler);

  positioning_deactivation_request_t request;
  request.ue_index = ue_index_t::invalid;
  const ue_index_t ue_index = uint_to_ue_index(5);

  nrppa.handle_new_nrppa_pdu(encode_nrppa_positioning_deactivation_request(request),
                             std::variant<ue_index_t, amf_index_t>{ue_index});

  ASSERT_EQ(notifier.nof_positioning_deactivation_requests, 1);
  ASSERT_TRUE(notifier.last_positioning_deactivation_request.has_value());
  ASSERT_EQ(notifier.last_positioning_deactivation_request->ue_index, ue_index);
  ASSERT_FALSE(notifier.last_positioning_deactivation_request->srs_res_set_id.has_value());
  ASSERT_EQ(notifier.nof_ul_nrppa_pdus, 1);

  auto decoded_response = decode_nrppa_pdu(notifier.last_ul_nrppa_pdu);
  ASSERT_TRUE(decoded_response.has_value());
  ASSERT_TRUE(std::holds_alternative<positioning_deactivation_response_t>(decoded_response.value().value));
}

TEST(nrppa_trp_information, unsupported_payload_is_counted_and_dropped)
{
  dummy_nrppa_cu_cp_notifier notifier;
  dummy_common_task_scheduler scheduler;
  nrppa_dummy_impl           nrppa(notifier, scheduler);

  byte_buffer unsupported_pdu = byte_buffer::create({0x4e, 0x50, 0x50, 0x41, 0x01, 0xff}).value();
  nrppa.handle_new_nrppa_pdu(unsupported_pdu, std::variant<ue_index_t, amf_index_t>{amf_index_t::min});

  ASSERT_EQ(notifier.nof_unsupported_nrppa_pdus, 1);
  ASSERT_EQ(notifier.last_unsupported_reason, "unsupported_nrppa_minimal_pdu_type");
  ASSERT_EQ(notifier.nof_trp_information_requests, 0);
  ASSERT_EQ(notifier.nof_ul_nrppa_pdus, 0);
}
