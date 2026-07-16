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

#include "du_manager_impl.h"
#include "du_positioning_handler_factory.h"
#include "procedures/cu_configuration_procedure.h"
#include "procedures/du_cell_stop_procedure.h"
#include "procedures/du_mac_si_pdu_update_procedure.h"
#include "procedures/du_param_config_procedure.h"
#include "procedures/du_setup_procedure.h"
#include "procedures/du_stop_procedure.h"
#include "procedures/du_ue_reset_procedure.h"
#include "procedures/du_ue_ric_configuration_procedure.h"
#include "procedures/f1c_disconnection_handling_procedure.h"
#include "srsran/mac/mac_pdu_handler.h"
#include "srsran/support/async/async_timer.h"
#include "srsran/support/executors/execute_until_success.h"
#include <algorithm>
#include <condition_variable>
#include <future>
#include <limits>
#include <thread>

using namespace srsran;
using namespace srs_du;

du_manager_impl::du_manager_impl(const du_manager_params& params_) :
  params(params_),
  logger(srslog::fetch_basic_logger("DU-MNG")),
  cell_mng(params),
  cell_res_alloc(params.ran.cells, params.mac.sched_cfg, params.ran.srbs, params.ran.qos, params.test_cfg),
  ue_mng(params, cell_res_alloc),
  positioning_handler(create_du_positioning_handler(params, cell_mng, ue_mng, logger)),
  metrics(params.metrics, params.services.du_mng_exec, params.services.timers, params.f1ap.metrics),
  proc_ctxt{params, ctxt, cell_mng, ue_mng, metrics, logger},
  main_ctrl_loop(128)
{
}

du_manager_impl::~du_manager_impl()
{
  stop();
}

void du_manager_impl::start()
{
  {
    std::unique_lock<std::mutex> lock(mutex);
    if (ctxt.running) {
      logger.warning("Ignoring start request. Cause: DU Manager already started.");
      return;
    }
  }

  logger.info("DU manager starting...");

  if (not params.services.du_mng_exec.execute([this]() {
        main_ctrl_loop.schedule([this](coro_context<async_task<void>>& ctx) {
          CORO_BEGIN(ctx);

          // Connect to CU-CP and send F1 Setup Request and await for F1 setup response.
          CORO_AWAIT(launch_async<du_setup_procedure>(proc_ctxt));

          // Signal start() caller thread that the operation is complete.
          std::lock_guard<std::mutex> lock(mutex);
          ctxt.running = true;
          cvar.notify_all();

          CORO_RETURN();
        });
      })) {
    report_fatal_error("Unable to initiate DU setup procedure");
  }

  // Block waiting for DU setup to complete.
  std::unique_lock<std::mutex> lock(mutex);
  cvar.wait(lock, [this]() { return ctxt.running; });

  logger.info("DU manager started successfully.");
}

void du_manager_impl::stop()
{
  {
    // Avoid stopping the DU Manager multiple times.
    std::lock_guard<std::mutex> lock(mutex);
    if (not ctxt.running) {
      return;
    }
  }

  while (not params.services.du_mng_exec.execute([this]() { handle_du_stop_request(); })) {
    logger.error("Unable to dispatch DU Manager shutdown. Retrying...");
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return;
  }

  // Wait for the DU Manager thread to signal that the stop was completed.
  std::unique_lock<std::mutex> lock(mutex);
  cvar.wait(lock, [this]() { return not ctxt.running; });
}

void du_manager_impl::handle_ul_ccch_indication(const ul_ccch_indication_message& msg)
{
  // Switch DU Manager exec context
  if (not params.services.du_mng_exec.execute([this, msg = std::move(msg)]() {
        // Start UE create procedure
        ue_mng.handle_ue_create_request(msg);
      })) {
    logger.warning("Discarding UL-CCCH message cell={} tc-rnti={} slot_rx={}. Cause: DU manager task queue is full",
                   fmt::underlying(msg.cell_index),
                   msg.tc_rnti,
                   msg.slot_rx);
  }
}

void du_manager_impl::handle_f1c_connection_loss()
{
  schedule_async_task(launch_async<f1c_disconnection_handling_procedure>(proc_ctxt));
}

void du_manager_impl::handle_du_stop_request()
{
  if (not ctxt.running) {
    // Already stopped.
    return;
  }

  // Notify other procedures that the DU needs to stop.
  ctxt.stop_command_received = true;

  // Start DU stop procedure.
  schedule_async_task(launch_async([this](coro_context<async_task<void>>& ctx) {
    CORO_BEGIN(ctx);

    if (not ctxt.running) {
      // Already stopped.
      CORO_EARLY_RETURN();
    }

    // Tear down activity in remaining layers.
    CORO_AWAIT(launch_async<du_stop_procedure>(ue_mng, cell_mng, params.f1ap.conn_mng));

    // DU stop successfully finished.
    // Dispatch main async task loop destruction via defer so that the current coroutine ends successfully.
    while (not params.services.du_mng_exec.defer([this]() {
      // Let main loop go out of scope and be destroyed.
      auto main_loop = main_ctrl_loop.request_stop();

      std::lock_guard<std::mutex> lock(mutex);
      ctxt.running               = false;
      ctxt.stop_command_received = false;
      cvar.notify_all();
    })) {
      logger.warning("Unable to stop DU Manager. Retrying...");
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    CORO_RETURN();
  }));
}

du_ue_index_t du_manager_impl::find_unused_du_ue_index()
{
  return ue_mng.find_unused_du_ue_index();
}

async_task<void> du_manager_impl::handle_f1_reset_request(const std::vector<du_ue_index_t>& ues_to_reset)
{
  return launch_async<du_ue_reset_procedure>(ues_to_reset, ue_mng, params, std::nullopt);
}

async_task<gnbcu_config_update_response>
du_manager_impl::handle_cu_context_update_request(const gnbcu_config_update_request& request)
{
  return launch_async<cu_configuration_procedure>(request, cell_mng, ue_mng, params, metrics);
}

async_task<f1ap_ntn_rnti_lease_pool_result>
du_manager_impl::handle_ntn_rnti_lease_pool_update_request(const f1ap_ntn_rnti_lease_pool_update& request)
{
  auto launch_result = [](f1ap_ntn_rnti_lease_pool_result result) {
    return launch_async(
        [result = std::move(result)](coro_context<async_task<f1ap_ntn_rnti_lease_pool_result>>& ctx) mutable {
          CORO_BEGIN(ctx);
          CORO_RETURN(result);
        });
  };

  f1ap_ntn_rnti_lease_pool_result result;
  result.generation_id   = request.generation_id;
  result.rejected_leases = request.leases;
  if (request.gnb_du_id != params.ran.gnb_du_id) {
    result.reject_reason = "gnb_du_id_mismatch";
    return launch_result(std::move(result));
  }
  if (!cell_mng.has_cell(request.cell_index)) {
    result.reject_reason = "unknown_cell";
    return launch_result(std::move(result));
  }
  const du_cell_config& cell_cfg = cell_mng.get_cell_cfg(request.cell_index);
  if (cell_cfg.nr_cgi != request.cell_cgi || cell_cfg.pci != request.pci) {
    result.reject_reason = "identity_mismatch";
    return launch_result(std::move(result));
  }

  mac_ntn_rnti_lease_pool_update mac_update;
  mac_update.cell_index = request.cell_index;
  mac_update.generation_id = request.generation_id;
  mac_update.expiry_ms     = request.expiry_ms;
  switch (request.operation) {
    case f1ap_ntn_rnti_lease_pool_operation::replace:
      mac_update.operation = mac_ntn_rnti_lease_pool_operation::replace;
      break;
    case f1ap_ntn_rnti_lease_pool_operation::add:
      mac_update.operation = mac_ntn_rnti_lease_pool_operation::add;
      break;
    case f1ap_ntn_rnti_lease_pool_operation::clear:
      mac_update.operation = mac_ntn_rnti_lease_pool_operation::clear;
      break;
  }
  mac_update.leases = request.leases;

  const mac_ntn_rnti_lease_pool_result mac_result = params.mac.mgr.apply_ntn_rnti_lease_pool_update(mac_update);

  result.accepted         = mac_result.accepted;
  result.reject_reason    = mac_result.reason;
  result.accepted_leases  = mac_result.accepted_leases;
  result.rejected_leases  = mac_result.rejected_leases;
  return launch_result(std::move(result));
}

async_task<f1ap_ntn_resource_audit_result>
du_manager_impl::handle_ntn_resource_audit_request(const f1ap_ntn_resource_audit_request& request)
{
  auto launch_result = [](f1ap_ntn_resource_audit_result result) {
    return launch_async(
        [result = std::move(result)](coro_context<async_task<f1ap_ntn_resource_audit_result>>& ctx) mutable {
          CORO_BEGIN(ctx);
          CORO_RETURN(result);
        });
  };

  f1ap_ntn_resource_audit_result result;
  result.generation_id = request.generation_id;
  if (!cell_mng.has_cell(request.cell_index)) {
    result.reject_reason = "unknown_cell";
    return launch_result(std::move(result));
  }
  if (cell_mng.get_cell_cfg(request.cell_index).pci != request.pci) {
    result.reject_reason = "pci_mismatch";
    return launch_result(std::move(result));
  }

  const mac_ntn_rnti_lease_pool_snapshot snapshot = params.mac.mgr.get_ntn_rnti_lease_pool_snapshot(request.cell_index);
  if (snapshot.complete && snapshot.cell_index != request.cell_index) {
    result.reject_reason = "snapshot_cell_mismatch";
    return launch_result(std::move(result));
  }
  result.accepted      = true;
  result.rnti_snapshot_complete    = snapshot.complete;
  result.ue_slot_snapshot_complete = false;
  result.reject_reason = snapshot.complete ? "ue_slot_snapshot_incomplete" : "rnti_and_ue_slot_snapshots_incomplete";
  if (snapshot.complete) {
    result.rnti_leases.reserve(snapshot.leases.size());
    for (const auto& lease : snapshot.leases) {
      result.rnti_leases.push_back({lease.rnti, lease.state, lease.distribution_state, lease.generation_id});
    }
  }

  return launch_result(std::move(result));
}

async_task<f1ap_ntn_sib19_broadcast_result>
du_manager_impl::handle_ntn_sib19_broadcast_update_request(const f1ap_ntn_sib19_broadcast_update& request)
{
  auto launch_result = [](f1ap_ntn_sib19_broadcast_result result) {
    return launch_async([result](coro_context<async_task<f1ap_ntn_sib19_broadcast_result>>& ctx) mutable {
      CORO_BEGIN(ctx);
      CORO_RETURN(result);
    });
  };

  f1ap_ntn_sib19_broadcast_result result;
  result.generation_id = request.generation_id;

  std::vector<byte_buffer> si_messages;
  if (request.operation == f1ap_ntn_sib19_broadcast_operation::update) {
    if (request.packed_sib19.empty()) {
      result.status        = f1ap_ntn_sib19_broadcast_result_status::packing_failed;
      result.reject_reason = "empty_sib19_payload";
      return launch_result(result);
    }
    auto sib19_copy = request.packed_sib19.deep_copy(byte_buffer::fallback_allocation_tag{});
    if (!sib19_copy.has_value()) {
      result.status        = f1ap_ntn_sib19_broadcast_result_status::packing_failed;
      result.reject_reason = "failed_to_copy_sib19_payload";
      return launch_result(result);
    }
    si_messages.push_back(std::move(sib19_copy.value()));
  }

  if (request.operation != f1ap_ntn_sib19_broadcast_operation::update &&
      request.operation != f1ap_ntn_sib19_broadcast_operation::clear) {
    result.status        = f1ap_ntn_sib19_broadcast_result_status::malformed_request;
    result.reject_reason = "invalid_operation";
    return launch_result(result);
  }
  if (!cell_mng.has_cell(request.cell_index)) {
    result.status        = f1ap_ntn_sib19_broadcast_result_status::si_slot_missing;
    result.reject_reason = "unknown_cell";
    return launch_result(result);
  }
  const du_cell_config& cell_cfg = cell_mng.get_cell_cfg(request.cell_index);
  if (cell_cfg.nr_cgi.nci != request.nci || cell_cfg.pci != request.pci) {
    result.status        = f1ap_ntn_sib19_broadcast_result_status::malformed_request;
    result.reject_reason = "wrong_cell_or_pci";
    return launch_result(result);
  }
  if (!cell_cfg.si_config.has_value() || request.si_msg_idx >= cell_cfg.si_config->si_sched_info.size()) {
    result.status        = f1ap_ntn_sib19_broadcast_result_status::si_slot_missing;
    result.reject_reason = "sib19_si_slot_missing";
    return launch_result(result);
  }
  const auto& mapping = cell_cfg.si_config->si_sched_info[request.si_msg_idx].sib_mapping_info;
  if (std::find(mapping.begin(), mapping.end(), sib_type::sib19) == mapping.end() || request.sib_idx != 19) {
    result.status        = f1ap_ntn_sib19_broadcast_result_status::si_slot_missing;
    result.reject_reason = "sib19_not_mapped_to_si_message";
    return launch_result(result);
  }
  if (!cell_mng.is_cell_active(request.cell_index)) {
    result.status        = f1ap_ntn_sib19_broadcast_result_status::mac_update_failed;
    result.reject_reason = "cell_not_active";
    return launch_result(result);
  }

  return launch_async([this,
                       request,
                       nr_cgi      = cell_cfg.nr_cgi,
                       si_messages = std::move(si_messages),
                       du_req      = du_si_pdu_update_request{},
                       si_resp     = du_si_pdu_update_response{},
                       result](
                          coro_context<async_task<f1ap_ntn_sib19_broadcast_result>>& ctx) mutable {
    CORO_BEGIN(ctx);

    du_req.nr_cgi      = nr_cgi;
    du_req.si_msg_idx  = request.si_msg_idx;
    du_req.sib_idx     = request.sib_idx;
    du_req.slot        = request.valid_from;
    du_req.clear       = request.operation == f1ap_ntn_sib19_broadcast_operation::clear;
    du_req.si_messages = span<byte_buffer>(si_messages.data(), si_messages.size());

    CORO_AWAIT_VALUE(si_resp, start_du_mac_si_pdu_update(du_req, params, cell_mng));

    if (si_resp.success) {
      result.status = du_req.clear ? f1ap_ntn_sib19_broadcast_result_status::clear_applied
                                   : f1ap_ntn_sib19_broadcast_result_status::applied;
    } else {
      result.status        = f1ap_ntn_sib19_broadcast_result_status::mac_update_failed;
      result.reject_reason = "mac_update_failed";
    }
    CORO_RETURN(result);
  });
}

async_task<f1ap_ntn_access_calendar_result>
du_manager_impl::handle_ntn_access_calendar_update_request(const f1ap_ntn_access_calendar_update& request)
{
  auto launch_result = [](f1ap_ntn_access_calendar_result result) {
    return launch_async([result = std::move(result)](
                            coro_context<async_task<f1ap_ntn_access_calendar_result>>& ctx) mutable {
      CORO_BEGIN(ctx);
      CORO_RETURN(result);
    });
  };

  f1ap_ntn_access_calendar_result result;
  result.catalog_version     = request.catalog_version;
  result.schedule_version    = request.schedule_version;
  result.source_content_hash = request.source_content_hash;
  result.calendar_hash       = request.calendar_hash;

  mac_ntn_access_calendar_update mac_request;
  switch (request.operation) {
    case f1ap_ntn_access_calendar_operation::prepare:
      mac_request.operation = mac_ntn_access_calendar_operation::prepare;
      break;
    case f1ap_ntn_access_calendar_operation::query:
      mac_request.operation = mac_ntn_access_calendar_operation::query;
      break;
    case f1ap_ntn_access_calendar_operation::clear:
      mac_request.operation = mac_ntn_access_calendar_operation::clear;
      break;
    case f1ap_ntn_access_calendar_operation::invalid:
      result.reject_reason = "invalid_operation";
      return launch_result(std::move(result));
  }
  mac_request.schedule_version = request.schedule_version;
  mac_request.calendar_hash    = request.calendar_hash;
  mac_request.activation_epoch = std::chrono::system_clock::time_point{
      std::chrono::milliseconds{static_cast<int64_t>(request.activation_epoch_unix_ms)}};
  mac_request.valid_until =
      std::chrono::system_clock::time_point{
          std::chrono::milliseconds{static_cast<int64_t>(request.valid_until_unix_ms)}};
  mac_request.cycle_duration = std::chrono::microseconds{request.cycle_duration_us};

  if (request.operation == f1ap_ntn_access_calendar_operation::prepare) {
    if (request.cells.size() != 2 || request.cells[0].du_cell_index == request.cells[1].du_cell_index ||
        request.cells[0].nci == request.cells[1].nci) {
      result.reject_reason = "prepare_requires_two_distinct_cells";
      return launch_result(std::move(result));
    }

    for (unsigned i = 0; i != request.cells.size(); ++i) {
      const f1ap_ntn_access_calendar_cell& source_cell = request.cells[i];
      if (!cell_mng.has_cell(source_cell.du_cell_index)) {
        result.reject_reason = "unknown_cell";
        return launch_result(std::move(result));
      }
      const du_cell_config& cell_cfg = cell_mng.get_cell_cfg(source_cell.du_cell_index);
      if (cell_cfg.nr_cgi.nci != source_cell.nci || cell_cfg.pci != source_cell.pci) {
        result.reject_reason = "identity_mismatch";
        return launch_result(std::move(result));
      }
      if (!cell_mng.is_cell_active(source_cell.du_cell_index)) {
        result.reject_reason = "cell_not_active";
        return launch_result(std::move(result));
      }

      mac_ntn_access_calendar_cell& target_cell = mac_request.cells[i];
      target_cell.cell_index = source_cell.du_cell_index;
      target_cell.nci        = source_cell.nci;
      target_cell.pci        = source_cell.pci;
      target_cell.intents.reserve(source_cell.intents.size());
      for (const f1ap_ntn_access_calendar_intent& source_intent : source_cell.intents) {
        mac_ntn_access_calendar_intent target_intent;
        target_intent.position_id = source_intent.position_id;
        target_intent.start_time  = std::chrono::microseconds{source_intent.start_time_us};
        target_intent.duration    = std::chrono::microseconds{source_intent.duration_us};
        target_intent.port_id     = source_intent.port_id;
        switch (source_intent.direction) {
          case f1ap_ntn_access_calendar_direction::downlink:
            target_intent.direction = mac_ntn_access_calendar_direction::downlink;
            break;
          case f1ap_ntn_access_calendar_direction::uplink:
            target_intent.direction = mac_ntn_access_calendar_direction::uplink;
            break;
          case f1ap_ntn_access_calendar_direction::invalid:
            result.reject_reason = "invalid_intent_direction";
            return launch_result(std::move(result));
        }
        switch (source_intent.purpose) {
          case f1ap_ntn_access_calendar_purpose::ssb_sib_paging:
            target_intent.purpose = mac_ntn_access_calendar_purpose::ssb_sib_paging;
            break;
          case f1ap_ntn_access_calendar_purpose::ssb_sib_paging_rar:
            target_intent.purpose = mac_ntn_access_calendar_purpose::ssb_sib_paging_rar;
            break;
          case f1ap_ntn_access_calendar_purpose::prach_ro:
            target_intent.purpose = mac_ntn_access_calendar_purpose::prach_ro;
            break;
          case f1ap_ntn_access_calendar_purpose::prach_ul_beam:
            target_intent.purpose = mac_ntn_access_calendar_purpose::prach_ul_beam;
            break;
          case f1ap_ntn_access_calendar_purpose::invalid:
            result.reject_reason = "invalid_intent_purpose";
            return launch_result(std::move(result));
        }
        target_cell.intents.push_back(std::move(target_intent));
      }
    }
  }

  const mac_ntn_access_calendar_result mac_result = params.mac.mgr.apply_ntn_access_calendar_update(mac_request);
  switch (mac_result.status) {
    case mac_ntn_access_calendar_status::preparing:
      result.status = f1ap_ntn_access_calendar_result_status::preparing;
      break;
    case mac_ntn_access_calendar_status::ready:
      result.status = f1ap_ntn_access_calendar_result_status::ready;
      break;
    case mac_ntn_access_calendar_status::applied:
      result.status = f1ap_ntn_access_calendar_result_status::applied;
      break;
    case mac_ntn_access_calendar_status::cleared:
      result.status = f1ap_ntn_access_calendar_result_status::cleared;
      break;
    case mac_ntn_access_calendar_status::unsupported:
      result.status = f1ap_ntn_access_calendar_result_status::unsupported;
      break;
    case mac_ntn_access_calendar_status::rejected:
      result.status = f1ap_ntn_access_calendar_result_status::rejected;
      break;
  }
  result.reject_reason = mac_result.reason;
  if (mac_result.effective_activation_slot.valid()) {
    result.activation_slot = mac_result.effective_activation_slot;
  }
  for (unsigned i = 0; i != result.accepted_intents_per_cell.size(); ++i) {
    result.accepted_intents_per_cell[i] = static_cast<uint16_t>(
        std::min(mac_result.accepted_intents[i], static_cast<unsigned>(std::numeric_limits<uint16_t>::max())));

    const mac_ntn_access_calendar_preflight_report& source_report = mac_result.preflight_reports[i];
    f1ap_ntn_access_calendar_preflight_report&      target_report = result.preflight_reports[i];
    if (!source_report.performed) {
      continue;
    }
    target_report.performed    = true;
    target_report.passed       = source_report.passed;
    target_report.numerology   = source_report.numerology;
    target_report.expected_ssb = static_cast<uint16_t>(
        std::min(source_report.expected_ssb, static_cast<uint32_t>(std::numeric_limits<uint16_t>::max())));
    target_report.matched_ssb = static_cast<uint16_t>(
        std::min(source_report.matched_ssb, static_cast<uint32_t>(std::numeric_limits<uint16_t>::max())));
    target_report.expected_prach = static_cast<uint16_t>(
        std::min(source_report.expected_prach, static_cast<uint32_t>(std::numeric_limits<uint16_t>::max())));
    target_report.matched_prach = static_cast<uint16_t>(
        std::min(source_report.matched_prach, static_cast<uint32_t>(std::numeric_limits<uint16_t>::max())));
    target_report.max_ssb_gap_slots   = source_report.max_ssb_gap_slots;
    target_report.max_prach_gap_slots = source_report.max_prach_gap_slots;
    if (source_report.first_unmatched.has_value()) {
      f1ap_ntn_access_calendar_unmatched_intent unmatched;
      unmatched.position_id       = source_report.first_unmatched->position_id;
      unmatched.start_slot_offset = source_report.first_unmatched->start_slot_offset;
      unmatched.nof_slots         = source_report.first_unmatched->nof_slots;
      switch (source_report.first_unmatched->purpose) {
        case mac_ntn_access_calendar_preflight_purpose::ssb:
          unmatched.purpose = f1ap_ntn_access_calendar_preflight_purpose::ssb;
          break;
        case mac_ntn_access_calendar_preflight_purpose::prach:
          unmatched.purpose = f1ap_ntn_access_calendar_preflight_purpose::prach;
          break;
        case mac_ntn_access_calendar_preflight_purpose::invalid:
          unmatched.purpose = f1ap_ntn_access_calendar_preflight_purpose::invalid;
          break;
      }
      target_report.first_unmatched.emplace(std::move(unmatched));
    }
  }
  return launch_result(std::move(result));
}

async_task<f1ap_ue_context_creation_response>
du_manager_impl::handle_ue_context_creation(const f1ap_ue_context_creation_request& request)
{
  return ue_mng.handle_ue_create_request(request);
}

async_task<f1ap_ue_context_update_response>
du_manager_impl::handle_ue_context_update(const f1ap_ue_context_update_request& request)
{
  return ue_mng.handle_ue_config_request(request);
}

async_task<void> du_manager_impl::handle_ue_delete_request(const f1ap_ue_delete_request& request)
{
  return ue_mng.handle_ue_delete_request(request);
}

async_task<void> du_manager_impl::handle_ue_drb_deactivation_request(du_ue_index_t ue_index)
{
  return ue_mng.handle_ue_drb_deactivation_request(ue_index);
}

void du_manager_impl::handle_ue_reestablishment(du_ue_index_t new_ue_index, du_ue_index_t old_ue_index)
{
  ue_mng.handle_reestablishment_request(new_ue_index, old_ue_index);
}

void du_manager_impl::handle_ue_config_applied(du_ue_index_t ue_index)
{
  ue_mng.handle_ue_config_applied(ue_index);
}

size_t du_manager_impl::nof_ues()
{
  // TODO: This is temporary code.
  std::promise<size_t> p;
  std::future<size_t>  fut = p.get_future();
  if (not params.services.du_mng_exec.execute([this, &p]() { p.set_value(ue_mng.nof_ues()); })) {
    logger.warning("Unable to compute the number of UEs active in the DU");
    return std::numeric_limits<size_t>::max();
  }
  return fut.get();
}

mac_cell_time_mapper& du_manager_impl::get_time_mapper()
{
  return params.mac.mgr.get_cell_manager().get_time_mapper(to_du_cell_index(0));
}

async_task<du_mac_sched_control_config_response>
du_manager_impl::configure_ue_mac_scheduler(du_mac_sched_control_config reconf)
{
  return launch_async<du_ue_ric_configuration_procedure>(reconf, ue_mng, params);
}

du_param_config_response du_manager_impl::handle_operator_config_request(const du_param_config_request& req)
{
  std::promise<du_param_config_response> p;
  std::future<du_param_config_response>  fut = p.get_future();

  // Switch to DU manager execution context.
  execute_until_success(params.services.du_mng_exec, params.services.timers, [this, req, &p]() {
    // Dispatch common task.
    schedule_async_task(launch_async([&](coro_context<async_task<void>>& ctx) {
      CORO_BEGIN(ctx);

      // Launch config procedure.
      CORO_AWAIT_VALUE(auto resp, launch_async<du_param_config_procedure>(req, params, cell_mng));

      // Signal back to caller.
      p.set_value(resp);

      CORO_RETURN();
    }));
  });

  return fut.get();
}

void du_manager_impl::handle_si_pdu_update(const du_si_pdu_update_request& req)
{
  std::vector<byte_buffer> si_messages;
  si_messages.reserve(req.si_messages.size());
  for (const byte_buffer& si_message : req.si_messages) {
    auto si_message_copy = si_message.deep_copy(byte_buffer::fallback_allocation_tag{});
    if (not si_message_copy.has_value()) {
      logger.warning("Discarding SI PDU update. Cause: Failed to copy SI message buffer");
      return;
    }
    si_messages.push_back(std::move(si_message_copy.value()));
  }

  schedule_async_task(launch_async([this,
                                    nr_cgi         = req.nr_cgi,
                                    si_msg_idx     = req.si_msg_idx,
                                    sib_idx        = req.sib_idx,
                                    slot           = req.slot,
                                    si_slot_period = req.si_slot_period,
                                    clear          = req.clear,
                                    si_messages    = std::move(si_messages),
                                    req_copy       = du_si_pdu_update_request{}](
                                       coro_context<async_task<void>>& ctx) mutable {
    CORO_BEGIN(ctx);

    if (not ctxt.running) {
      // Already stopped.
      CORO_EARLY_RETURN();
    }

    req_copy.nr_cgi         = nr_cgi;
    req_copy.si_msg_idx     = si_msg_idx;
    req_copy.sib_idx        = sib_idx;
    req_copy.slot           = slot;
    req_copy.si_slot_period = si_slot_period;
    req_copy.clear          = clear;
    req_copy.si_messages    = span<byte_buffer>(si_messages.data(), si_messages.size());

    CORO_AWAIT(start_du_mac_si_pdu_update(req_copy, params, cell_mng));

    CORO_RETURN();
  }));
}
