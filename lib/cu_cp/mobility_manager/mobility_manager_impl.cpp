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

#include "mobility_manager_impl.h"
#include "../du_processor/du_processor_repository.h"
#include "srsran/ran/nr_cgi.h"
#include <algorithm>

using namespace srsran;
using namespace srs_cu_cp;

mobility_manager::mobility_manager(const mobility_manager_cfg&      cfg_,
                                   mobility_manager_cu_cp_notifier& cu_cp_notifier_,
                                   ngap_repository&                 ngap_db_,
                                   du_processor_repository&         du_db_,
                                   ue_manager&                      ue_mng_) :
  cfg(cfg_),
  cu_cp_notifier(cu_cp_notifier_),
  ngap_db(ngap_db_),
  du_db(du_db_),
  ue_mng(ue_mng_),
  logger(srslog::fetch_basic_logger("CU-CP"))
{
}

void mobility_manager::trigger_handover(pci_t source_pci, rnti_t rnti, pci_t target_pci)
{
  ue_index_t ue_index = ue_mng.get_ue_index(source_pci, rnti);
  if (ue_index == ue_index_t::invalid) {
    logger.warning("Could not trigger handover, UE is invalid. rnti={} pci={}", rnti, source_pci);
    return;
  }

  du_index_t target_du_index = du_db.find_du(target_pci);
  if (target_du_index == du_index_t::invalid) {
    logger.warning("Could not trigger handover, target PCI={} is not served by any connected DU", target_pci);
    return;
  }

  std::optional<nr_cell_global_id_t> target_cgi =
      du_db.get_du_processor(target_du_index).get_mobility_handler().get_cgi(target_pci);
  if (!target_cgi.has_value()) {
    logger.warning("Could not trigger handover, target CGI for PCI={} at du_index={} was not found",
                   target_pci,
                   target_du_index);
    return;
  }

  handle_handover(
      ue_index, target_cgi->nci.gnb_id(22), target_cgi->nci, target_pci, std::nullopt, target_du_index);
}

void mobility_manager::handle_neighbor_better_than_spcell(ue_index_t       ue_index,
                                                          gnb_id_t         neighbor_gnb_id,
                                                          nr_cell_identity neighbor_nci,
                                                          pci_t            neighbor_pci)
{
  if (!cfg.trigger_handover_from_measurements) {
    logger.debug("ue={}: Ignoring better neighbor pci={}", ue_index, neighbor_pci);
    return;
  }
  handle_handover(ue_index, neighbor_gnb_id, neighbor_nci, neighbor_pci);
}

void mobility_manager::handle_ntn_served_beams_updated(const std::vector<std::string>& beam_ids)
{
  current_served_ntn_beam_ids = beam_ids;
  logger.debug("Updated NTN mobility served beam set with {} beams", current_served_ntn_beam_ids.size());
}

void mobility_manager::handle_ntn_beam_placement_plan_updated(const ntn_beam_placement_plan& plan)
{
  current_ntn_beam_assignments_by_id.clear();
  for (const auto& assignment : plan.assignments) {
    if (!assignment.beam_id.empty()) {
      current_ntn_beam_assignments_by_id[assignment.beam_id] = assignment;
    }
  }
  logger.debug("Updated NTN mobility beam placement plan with {} assignments",
               current_ntn_beam_assignments_by_id.size());
}

bool mobility_manager::handle_ntn_location_handover_required(const ntn_location_handover_trigger& trigger)
{
  const bool has_service_pair_target = !trigger.target_uplink_resource_beam_id.empty() &&
                                       trigger.target_uplink_resource_beam_id != trigger.target_beam_id;
  if (trigger.ue_index == ue_index_t::invalid) {
    logger.warning("Ignoring NTN location handover trigger with invalid UE index");
    return false;
  }
  if (trigger.target_beam_id.empty()) {
    logger.warning("ue={}: Ignoring NTN location handover trigger with empty target beam id", trigger.ue_index);
    return false;
  }
  if (trigger.target_pci == INVALID_PCI) {
    logger.warning("ue={}: Ignoring NTN location handover trigger for beam id={}. Cause: invalid target PCI",
                   trigger.ue_index,
                   trigger.target_beam_id);
    return false;
  }
  if (std::find(current_served_ntn_beam_ids.begin(), current_served_ntn_beam_ids.end(), trigger.target_beam_id) ==
          current_served_ntn_beam_ids.end() &&
      !has_service_pair_target) {
    logger.warning("ue={}: Ignoring NTN location handover trigger attempt={} beam={}. Cause: target beam is not in "
                   "the current served beam set",
                   trigger.ue_index,
                   trigger.handover_attempt_id,
                   trigger.target_beam_id);
    return false;
  }
  if (std::find(trigger.served_beam_ids_snapshot.begin(),
                trigger.served_beam_ids_snapshot.end(),
                trigger.target_beam_id) == trigger.served_beam_ids_snapshot.end()) {
    if (!has_service_pair_target) {
      logger.warning("ue={}: Ignoring NTN location handover trigger attempt={} beam={}. Cause: target beam is not in "
                     "the served beam snapshot",
                     trigger.ue_index,
                     trigger.handover_attempt_id,
                     trigger.target_beam_id);
      return false;
    }
  }

  std::optional<du_index_t> planned_target_du_index;
  const auto assignment_it = current_ntn_beam_assignments_by_id.find(trigger.target_beam_id);
  if (assignment_it != current_ntn_beam_assignments_by_id.end()) {
    const ntn_beam_du_assignment& assignment = assignment_it->second;
    const bool target_is_active_loaded = assignment.state == ntn_beam_assignment_state::active_loaded;
    const bool target_is_eligible_candidate =
        assignment.state == ntn_beam_assignment_state::candidate && assignment.in_hopping_window;
    if ((!target_is_active_loaded && !target_is_eligible_candidate) || assignment.du_index == du_index_t::invalid) {
      logger.warning("ue={}: Ignoring NTN location handover trigger attempt={} beam={}. Cause: target beam is not "
                     "mobility eligible in the current beam placement plan",
                     trigger.ue_index,
                     trigger.handover_attempt_id,
                     trigger.target_beam_id);
      return false;
    }
    if (assignment.nci != trigger.target_nci) {
      logger.warning("ue={}: Ignoring NTN location handover trigger attempt={} beam={}. Cause: placement plan nci={:#x} "
                     "does not match trigger target_nci={:#x}",
                     trigger.ue_index,
                     trigger.handover_attempt_id,
                     trigger.target_beam_id,
                     assignment.nci,
                     trigger.target_nci);
      return false;
    }
    planned_target_du_index = assignment.du_index;
  }

  const auto candidate_age =
      std::chrono::duration_cast<std::chrono::milliseconds>(trigger.last_report_time - trigger.candidate_since);
  ntn_handover_context ntn_context;
  ntn_context.handover_attempt_id         = trigger.handover_attempt_id;
  ntn_context.source_beam_id              = trigger.source_beam_id;
  ntn_context.target_beam_id                = trigger.target_beam_id;
  ntn_context.source_analog_beam_id       = trigger.source_analog_beam_id;
  ntn_context.target_analog_beam_id       = trigger.target_analog_beam_id;
  ntn_context.handover_reason             = trigger.handover_reason;
  ntn_context.serving_nci                   = trigger.serving_nci;
  ntn_context.target_nci                    = trigger.target_nci;
  ntn_context.consecutive_location_reports  = trigger.consecutive_location_reports;
  ntn_context.candidate_age                 = std::max(candidate_age, std::chrono::milliseconds{0});
  ntn_context.target_preloaded              = trigger.target_preloaded;
  ntn_context.target_du_index               = trigger.target_du_index;
  ntn_context.target_c_rnti                 = trigger.target_c_rnti;
  ntn_context.target_uplink_resource_beam_id = trigger.target_uplink_resource_beam_id;
  ntn_context.target_uplink_resource_nci     = trigger.target_uplink_resource_nci;
  ntn_context.target_uplink_resource_du_index = trigger.target_uplink_resource_du_index;
  ntn_context.target_service_pair_reason     = trigger.target_service_pair_reason;
  ntn_context.target_ul_slot_request        = trigger.target_ul_slot_request;
  ntn_context.target_resource_state         = trigger.target_resource_state;
  ntn_context.target_sr_srs_applied         = trigger.target_sr_srs_applied;

  logger.info("ue={}: NTN location handover trigger attempt={} beam={} target_nci={:#x} pci={} reports={} age={}ms "
              "served_beams={} snapshot_beams={}",
              trigger.ue_index,
              trigger.handover_attempt_id,
              trigger.target_beam_id,
              trigger.target_nci,
              trigger.target_pci,
              trigger.consecutive_location_reports,
              ntn_context.candidate_age.count(),
              current_served_ntn_beam_ids.size(),
              trigger.served_beam_ids_snapshot.size());

  return handle_handover(trigger.ue_index,
                         trigger.target_gnb_id,
                         trigger.target_nci,
                         trigger.target_pci,
                         ntn_context,
                         planned_target_du_index);
}

bool mobility_manager::handle_handover(ue_index_t       ue_index,
                                       gnb_id_t         neighbor_gnb_id,
                                       nr_cell_identity neighbor_nci,
                                       pci_t            neighbor_pci,
                                       const std::optional<ntn_handover_context>& ntn_context,
                                       const std::optional<du_index_t>&           planned_target_du_index)
{
  // Find the UE context.
  cu_cp_ue* u = ue_mng.find_du_ue(ue_index);
  if (u == nullptr) {
    logger.error("ue={}: Couldn't find UE", ue_index);
    return false;
  }
  cu_cp_ue_context& ue_ctxt = u->get_ue_context();
  if (ue_ctxt.reconfiguration_disabled) {
    logger.debug("ue={}: MeasurementReport ignored. Cause: UE cannot be reconfigured", ue_index);
    return false;
  }
  if (neighbor_pci == INVALID_PCI) {
    logger.error("ue={}: Ignoring Handover Request. Cause: Invalid target PCI {} received", ue_index, neighbor_pci);
    return false;
  }

  // Handover is going ahead.

  // Try to find target DU. If it is not found, it means that the target cell is not managed by this CU-CP and
  // a NG Handover is required.
  du_index_t target_du = du_index_t::invalid;
  if (planned_target_du_index.has_value()) {
    target_du = planned_target_du_index.value();
    if (du_db.find_du_processor(target_du) == nullptr) {
      logger.warning("ue={}: Rejecting handover. Cause: planned target_du={} is not connected", ue_index, target_du);
      return false;
    }
  } else {
    target_du = du_db.find_du(neighbor_pci);
  }
  if (target_du == du_index_t::invalid) {
    logger.debug("ue={}: Requesting inter CU handover. No local DU/cell with pci={} found", ue_index, neighbor_pci);
    return handle_inter_cu_handover(ue_index, neighbor_gnb_id, neighbor_nci);
  }

  du_index_t source_du = ue_mng.find_du_ue(ue_index)->get_du_index();

  if (source_du == target_du) {
    logger.info("ue={}: Trigger intra-CU (intra-DU) handover on du={}", ue_index, source_du);
  } else {
    logger.info("ue={}: Trigger intra-CU (inter-DU) handover from source_du={} to target_du={}",
                ue_index,
                source_du,
                target_du);
  }
  return handle_intra_cu_handover(ue_index, neighbor_pci, source_du, target_du, ntn_context);
}

bool mobility_manager::handle_intra_cu_handover(ue_index_t source_ue_index,
                                                pci_t      neighbor_pci,
                                                du_index_t source_du_index,
                                                du_index_t target_du_index,
                                                const std::optional<ntn_handover_context>& ntn_context)
{
  // Lookup CGI at target DU.
  std::optional<nr_cell_global_id_t> cgi =
      du_db.get_du_processor(target_du_index).get_mobility_handler().get_cgi(neighbor_pci);
  if (!cgi.has_value()) {
    logger.warning(
        "ue={}: Couldn't retrieve CGI for pci={} at du_index={}", source_ue_index, neighbor_pci, target_du_index);
    return false;
  }

  cu_cp_intra_cu_handover_request request = {};
  request.source_ue_index                 = source_ue_index;
  request.target_pci                      = neighbor_pci;
  request.cgi                             = cgi.value();
  request.target_du_index                 = target_du_index;
  request.ntn_context                     = ntn_context;

  cu_cp_ue* u = ue_mng.find_du_ue(source_ue_index);
  if (u == nullptr) {
    logger.error("ue={}: Couldn't find UE", source_ue_index);
    return false;
  }

  // Disable new reconfigurations from now on (except for the Handover Command).
  u->get_ue_context().reconfiguration_disabled = true;

  // Trigger Intra CU handover routine on the DU processor of the source DU.
  auto ho_trigger = [this, request, response = cu_cp_intra_cu_handover_response{}, source_du_index, target_du_index](
                        coro_context<async_task<void>>& ctx) mutable {
    CORO_BEGIN(ctx);
    CORO_AWAIT_VALUE(response, cu_cp_notifier.on_intra_cu_handover_required(request, source_du_index, target_du_index));
    if (!response.success) {
      if (!request.ntn_context.has_value()) {
        if (cu_cp_ue* source_ue = ue_mng.find_du_ue(request.source_ue_index); source_ue != nullptr) {
          source_ue->get_ue_context().reconfiguration_disabled = false;
        }
      } else {
        ntn_handover_result result;
        result.source_ue_index                   = request.source_ue_index;
        result.context                           = request.ntn_context.value();
        result.success                           = false;
        result.failure_cause                     = ntn_handover_failure_cause::source_preparation_failed;
        result.source_reconfiguration_can_resume = true;
        cu_cp_notifier.on_ntn_handover_result(result);
      }
    }
    CORO_RETURN();
  };
  u->get_task_sched().schedule_async_task(launch_async(std::move(ho_trigger)));
  return true;
}

bool mobility_manager::handle_inter_cu_handover(ue_index_t       source_ue_index,
                                                gnb_id_t         target_gnb_id,
                                                nr_cell_identity target_nci)
{
  cu_cp_ue* u = ue_mng.find_du_ue(source_ue_index);
  if (u == nullptr) {
    logger.error("ue={}: Couldn't find UE", source_ue_index);
    return false;
  }

  ngap_handover_preparation_request request = {};
  request.ue_index                          = source_ue_index;
  request.gnb_id                            = target_gnb_id;
  request.nci                               = target_nci;

  // create a map of all PDU sessions and their associated QoS flows
  const std::map<pdu_session_id_t, up_pdu_session_context>& pdu_sessions =
      ue_mng.find_ue(source_ue_index)->get_up_resource_manager().get_pdu_sessions_map();
  for (const auto& pdu_session : pdu_sessions) {
    std::vector<qos_flow_id_t> qos_flows;
    for (const auto& drb : pdu_session.second.drbs) {
      for (const auto& qos_flow : drb.second.qos_flows) {
        qos_flows.push_back(qos_flow.first);
      }
    }
    request.pdu_sessions.insert({pdu_session.first, qos_flows});
  }

  cu_cp_ue_context& ue_ctxt = u->get_ue_context();

  auto* ngap = ngap_db.find_ngap(ue_ctxt.plmn);
  if (ngap == nullptr) {
    logger.error("ue={}: Couldn't find NGAP", source_ue_index);
    return false;
  }

  // Disable new reconfigurations from now on (except for the Handover Command).
  ue_ctxt.reconfiguration_disabled = true;

  // Send handover preparation request to the NGAP handler.
  auto ho_trigger = [ngap, request, response = ngap_handover_preparation_response{}](
                        coro_context<async_task<void>>& ctx) mutable {
    CORO_BEGIN(ctx);
    CORO_AWAIT_VALUE(response, ngap->get_ngap_control_message_handler().handle_handover_preparation_request(request));
    CORO_RETURN();
  };
  u->get_task_sched().schedule_async_task(launch_async(std::move(ho_trigger)));
  return true;
}
