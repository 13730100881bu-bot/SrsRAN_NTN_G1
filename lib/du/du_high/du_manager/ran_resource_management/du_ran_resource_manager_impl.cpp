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

#include "du_ran_resource_manager_impl.h"
#include "srsran/f1ap/ntn_ul_slot_resource_request.h"
#include "srsran/mac/config/mac_cell_group_config_factory.h"
#include "srsran/ran/sr_configuration.h"
#include "srsran/scheduler/config/serving_cell_config_factory.h"
#include "srsran/srslog/srslog.h"
#include <algorithm>
#include <limits>

using namespace srsran;
using namespace srs_du;

du_ue_ran_resource_updater_impl::du_ue_ran_resource_updater_impl(du_ue_resource_config* cell_grp_cfg_,
                                                                 const std::optional<ue_capability_summary>& ue_caps_,
                                                                 du_ran_resource_manager_impl&               parent_,
                                                                 du_ue_index_t ue_index_) :
  cell_grp(cell_grp_cfg_), ue_caps(&ue_caps_), parent(&parent_), ue_index(ue_index_)
{
}

du_ue_ran_resource_updater_impl::~du_ue_ran_resource_updater_impl()
{
  parent->deallocate_context(ue_index);
}

du_ue_resource_update_response
du_ue_ran_resource_updater_impl::update(du_cell_index_t                       pcell_index,
                                        const f1ap_ue_context_update_request& upd_req,
                                        const du_ue_resource_config*          reestablished_context,
                                        const ue_capability_summary*          reestablished_ue_caps)
{
  return parent->update_context(ue_index, pcell_index, upd_req, reestablished_context, reestablished_ue_caps);
}

void du_ue_ran_resource_updater_impl::update_completed(bool applied)
{
  parent->complete_versioned_slot_request(ue_index, applied);
}

void du_ue_ran_resource_updater_impl::config_applied()
{
  parent->ue_config_applied(ue_index);
}

///////////////////////////

// Helper that resets the PUCCH and SRS configurations in the serving cell configuration.
static void reset_serv_cell_cfg(serving_cell_config& serv_cell_cfg)
{
  srsran_assert(serv_cell_cfg.ul_config.has_value() and
                    serv_cell_cfg.ul_config.value().init_ul_bwp.pucch_cfg.has_value() and
                    serv_cell_cfg.ul_config.value().init_ul_bwp.srs_cfg.has_value(),
                "UL configuration in Serving cell config not configured");

  serv_cell_cfg.ul_config->init_ul_bwp.pucch_cfg.reset();
  if (serv_cell_cfg.csi_meas_cfg.has_value()) {
    serv_cell_cfg.csi_meas_cfg.value().csi_report_cfg_list.clear();
  }

  serv_cell_cfg.ul_config->init_ul_bwp.srs_cfg.reset();
}

static std::optional<ntn_ul_slot_resource_request>
make_du_ntn_ul_slot_resource_request(const std::optional<f1ap_ntn_ul_slot_resource_request>& request)
{
  if (!request.has_value() || is_empty(*request)) {
    return std::nullopt;
  }

  ntn_ul_slot_resource_request du_request;
  du_request.sr_slot_offset  = request->sr_slot_offset;
  du_request.srs_slot_offset = request->srs_slot_offset;
  du_request.sr_slot_period  = request->sr_slot_period;
  du_request.srs_slot_period = request->srs_slot_period;
  return du_request;
}

static bool is_same_ntn_ul_slot_resource_request(const std::optional<ntn_ul_slot_resource_request>& current_request,
                                                 const std::optional<ntn_ul_slot_resource_request>& next_request)
{
  if (current_request.has_value() != next_request.has_value()) {
    return false;
  }
  if (!current_request.has_value()) {
    return true;
  }
  return current_request->sr_slot_offset == next_request->sr_slot_offset &&
         current_request->srs_slot_offset == next_request->srs_slot_offset &&
         current_request->sr_slot_period == next_request->sr_slot_period &&
         current_request->srs_slot_period == next_request->srs_slot_period;
}

static f1ap_ntn_ul_slot_resource_result
make_f1ap_ntn_ul_slot_resource_result(bool                                               accepted,
                                      f1ap_ntn_ul_slot_resource_result_reason            reason,
                                      const std::optional<ntn_ul_slot_resource_request>& request)
{
  f1ap_ntn_ul_slot_resource_result result;
  result.accepted = accepted;
  result.reason   = reason;
  if (request.has_value()) {
    f1ap_ntn_ul_slot_resource_request f1ap_request;
    f1ap_request.sr_slot_offset  = request->sr_slot_offset;
    f1ap_request.srs_slot_offset = request->srs_slot_offset;
    f1ap_request.sr_slot_period  = request->sr_slot_period;
    f1ap_request.srs_slot_period = request->srs_slot_period;
    if (!is_empty(f1ap_request)) {
      result.applied_request = f1ap_request;
    }
  }
  return result;
}

static f1ap_ntn_ul_slot_resource_result make_versioned_ntn_ul_slot_resource_result(
    bool                                                    accepted,
    f1ap_ntn_ul_slot_resource_result_reason                 reason,
    const f1ap_ntn_ul_slot_resource_request&                request,
    const std::optional<f1ap_ntn_ul_slot_resource_request>& applied_request = std::nullopt)
{
  f1ap_ntn_ul_slot_resource_result result;
  result.accepted              = accepted;
  result.reason                = reason;
  result.assignment_generation = request.assignment_generation;
  result.operation             = request.operation;
  if (accepted) {
    result.applied_request = applied_request.has_value() ? applied_request : std::optional{request};
  }
  return result;
}

static f1ap_ntn_ul_slot_resource_result_reason get_ntn_ul_slot_reject_reason(const std::string& error)
{
  if (error.find("SRS") != std::string::npos) {
    return f1ap_ntn_ul_slot_resource_result_reason::srs_offset_unavailable;
  }
  if (error.find("PUCCH") != std::string::npos) {
    return f1ap_ntn_ul_slot_resource_result_reason::sr_offset_unavailable;
  }
  return f1ap_ntn_ul_slot_resource_result_reason::du_resource_conflict;
}

du_ran_resource_manager_impl::du_ran_resource_manager_impl(span<const du_cell_config>                cell_cfg_list_,
                                                           const scheduler_expert_config&            scheduler_cfg,
                                                           const std::map<srb_id_t, du_srb_config>&  srb_config,
                                                           const std::map<five_qi_t, du_qos_config>& qos_config,
                                                           const du_test_mode_config&                test_cfg_) :
  cell_cfg_list(cell_cfg_list_),
  logger(srslog::fetch_basic_logger("DU-MNG")),
  test_cfg(test_cfg_),
  pucch_res_mng(cell_cfg_list, scheduler_cfg.ue.max_pucchs_per_slot),
  bearer_res_mng(srb_config, qos_config, logger),
  srs_res_mng(std::make_unique<du_srs_policy_max_ul_rate>(cell_cfg_list)),
  meas_cfg_mng(cell_cfg_list),
  drx_res_mng(cell_cfg_list),
  ra_res_alloc(cell_cfg_list)
{
  for (const auto& cell : cell_cfg_list) {
    const du_cell_index_t cell_idx  = cell.ue_ded_serv_cell_cfg.cell_index;
    unsigned              sr_limit  = pucch_res_mng.get_nof_sr_free_res_offsets(cell_idx);
    unsigned              csi_limit = 0;
    unsigned              srs_limit = 0;

    unsigned max_nof_ues = sr_limit;
    if (cell.ue_ded_serv_cell_cfg.csi_meas_cfg.has_value()) {
      csi_limit   = pucch_res_mng.get_nof_csi_free_res_offsets(cell_idx);
      max_nof_ues = std::min(max_nof_ues, csi_limit);
    }
    if (cell.srs_cfg.srs_period.has_value()) {
      srs_limit   = srs_res_mng->get_nof_srs_free_res_offsets(cell_idx);
      max_nof_ues = std::min(max_nof_ues, srs_limit);
    }

    logger.info("The upper-bound on the number of UEs supported by cell {{pci={}, du_cell_index={}}} is {} (the actual "
                "number might be lower than that). This is determined by the lowest of the following limits: SR ({}), "
                "CSI ({}) and SRS ({}).",
                cell.pci,
                fmt::underlying(cell_idx),
                max_nof_ues,
                sr_limit,
                cell.ue_ded_serv_cell_cfg.csi_meas_cfg.has_value() ? fmt::to_string(csi_limit) : "n/a",
                cell.srs_cfg.srs_period.has_value() ? fmt::to_string(srs_limit) : "n/a");
  }
}

du_ran_resource_manager_impl::versioned_slot_request_decision
du_ran_resource_manager_impl::validate_versioned_slot_request(du_ue_index_t                            ue_index,
                                                              du_cell_index_t                          cell_index,
                                                              const f1ap_ntn_ul_slot_resource_request& request)
{
  versioned_slot_request_decision decision;

  const bool has_sr    = request.sr_slot_offset.has_value();
  const bool has_srs   = request.srs_slot_offset.has_value();
  const bool malformed = request.assignment_generation == 0 ||
                         request.operation == f1ap_ntn_ul_slot_resource_operation::legacy ||
                         (request.operation != f1ap_ntn_ul_slot_resource_operation::set &&
                          request.operation != f1ap_ntn_ul_slot_resource_operation::clear) ||
                         (request.operation == f1ap_ntn_ul_slot_resource_operation::set && !has_sr && !has_srs) ||
                         (request.operation == f1ap_ntn_ul_slot_resource_operation::clear &&
                          (has_sr || has_srs || request.sr_slot_period.has_value() ||
                           request.srs_slot_period.has_value() || request.requested_c_rnti.has_value())) ||
                         (request.sr_slot_period.has_value() && (!has_sr || *request.sr_slot_period == 0)) ||
                         (request.srs_slot_period.has_value() && (!has_srs || *request.srs_slot_period == 0)) ||
                         (request.requested_c_rnti.has_value() && !is_crnti(*request.requested_c_rnti));
  if (malformed) {
    decision.action        = versioned_slot_request_action::reject;
    decision.reject_reason = f1ap_ntn_ul_slot_resource_result_reason::malformed_request;
    return decision;
  }

  std::lock_guard<std::mutex> lock(ntn_ue_slot_registry_mutex);
  if (!ntn_ue_slot_registry.contains(ue_index)) {
    decision.action        = versioned_slot_request_action::reject;
    decision.reject_reason = f1ap_ntn_ul_slot_resource_result_reason::du_context_update_failed;
    return decision;
  }

  ntn_ue_slot_registry_state& state = ntn_ue_slot_registry[ue_index];
  if (state.update_in_progress) {
    decision.action        = versioned_slot_request_action::reject;
    decision.reject_reason = f1ap_ntn_ul_slot_resource_result_reason::du_resource_conflict;
    return decision;
  }
  if (request.assignment_generation < state.assignment_generation_high_water) {
    decision.action        = versioned_slot_request_action::reject;
    decision.reject_reason = f1ap_ntn_ul_slot_resource_result_reason::stale_assignment_generation;
    return decision;
  }
  if (request.assignment_generation == state.assignment_generation_high_water &&
      state.assignment_generation_high_water != 0) {
    if (!state.last_request.has_value() ||
        !are_f1ap_ntn_ul_slot_resource_requests_equal(*state.last_request, request)) {
      decision.action        = versioned_slot_request_action::reject;
      decision.reject_reason = state.assignment_generation_high_water == std::numeric_limits<uint32_t>::max()
                                   ? f1ap_ntn_ul_slot_resource_result_reason::slot_assignment_generation_exhausted
                                   : f1ap_ntn_ul_slot_resource_result_reason::assignment_generation_conflict;
      return decision;
    }
    if (!state.consistent) {
      decision.action        = versioned_slot_request_action::reject;
      decision.reject_reason = f1ap_ntn_ul_slot_resource_result_reason::du_resource_conflict;
      return decision;
    }

    if (request.operation == f1ap_ntn_ul_slot_resource_operation::set) {
      if (!state.active_assignment.has_value()) {
        decision.action        = versioned_slot_request_action::reject;
        decision.reject_reason = f1ap_ntn_ul_slot_resource_result_reason::du_context_update_failed;
        return decision;
      }
      decision.applied_request                   = state.active_assignment->request;
      decision.applied_request->requested_c_rnti = request.requested_c_rnti;
    } else {
      decision.applied_request = request;
    }
    decision.action = versioned_slot_request_action::idempotent;
    return decision;
  }

  state.cell_index         = cell_index;
  state.update_in_progress = true;
  decision.action          = versioned_slot_request_action::apply;
  return decision;
}

std::optional<f1ap_ntn_ul_slot_resource_request>
du_ran_resource_manager_impl::read_actual_slot_assignment(const du_ue_resource_config&             ue_res,
                                                          const f1ap_ntn_ul_slot_resource_request& requested) const
{
  if (!ue_res.cell_group.cells.contains(SERVING_CELL_PCELL_IDX)) {
    return std::nullopt;
  }
  const serving_cell_config& pcell = ue_res.cell_group.cells[SERVING_CELL_PCELL_IDX].serv_cell_cfg;
  if (!pcell.ul_config.has_value()) {
    return std::nullopt;
  }

  f1ap_ntn_ul_slot_resource_request actual = requested;
  if (requested.sr_slot_offset.has_value()) {
    if (!pcell.ul_config->init_ul_bwp.pucch_cfg.has_value() ||
        pcell.ul_config->init_ul_bwp.pucch_cfg->sr_res_list.empty()) {
      return std::nullopt;
    }
    const auto& sr_resource = pcell.ul_config->init_ul_bwp.pucch_cfg->sr_res_list.front();
    actual.sr_slot_offset   = sr_resource.offset;
    actual.sr_slot_period   = sr_periodicity_to_slot(sr_resource.period);
  } else {
    actual.sr_slot_period.reset();
  }

  if (requested.srs_slot_offset.has_value()) {
    if (!pcell.ul_config->init_ul_bwp.srs_cfg.has_value() ||
        pcell.ul_config->init_ul_bwp.srs_cfg->srs_res_list.empty() ||
        !pcell.ul_config->init_ul_bwp.srs_cfg->srs_res_list.front().periodicity_and_offset.has_value()) {
      return std::nullopt;
    }
    const auto& srs_resource = pcell.ul_config->init_ul_bwp.srs_cfg->srs_res_list.front();
    actual.srs_slot_offset   = srs_resource.periodicity_and_offset->offset;
    actual.srs_slot_period   = static_cast<unsigned>(srs_resource.periodicity_and_offset->period);
  } else {
    actual.srs_slot_period.reset();
  }
  return actual;
}

void du_ran_resource_manager_impl::stage_versioned_slot_request(
    du_ue_index_t                                           ue_index,
    du_cell_index_t                                         cell_index,
    const f1ap_ntn_ul_slot_resource_request&                request,
    const std::optional<f1ap_ntn_ul_slot_resource_request>& actual_request)
{
  std::lock_guard<std::mutex> lock(ntn_ue_slot_registry_mutex);
  if (!ntn_ue_slot_registry.contains(ue_index)) {
    return;
  }

  ntn_ue_slot_registry_state& state = ntn_ue_slot_registry[ue_index];
  state.pending_cell_index          = cell_index;
  state.pending_request             = request;
  state.pending_actual_request      = actual_request;
}

void du_ran_resource_manager_impl::complete_versioned_slot_request(du_ue_index_t ue_index, bool applied)
{
  std::lock_guard<std::mutex> lock(ntn_ue_slot_registry_mutex);
  if (!ntn_ue_slot_registry.contains(ue_index)) {
    return;
  }

  ntn_ue_slot_registry_state& state = ntn_ue_slot_registry[ue_index];
  if (!state.pending_request.has_value()) {
    return;
  }

  const f1ap_ntn_ul_slot_resource_request request = *state.pending_request;
  const auto actual_request                       = state.pending_actual_request;
  const du_cell_index_t cell_index                = state.pending_cell_index;
  state.pending_request.reset();
  state.pending_actual_request.reset();
  state.pending_cell_index = INVALID_DU_CELL_INDEX;
  state.update_in_progress = false;

  if (!applied) {
    // A failed scheduler transaction can leave local allocations and scheduler state out of step. Keep the previous
    // authoritative record, but withhold complete snapshots until a later successful versioned update.
    state.consistent = false;
    return;
  }

  state.cell_index                        = cell_index;
  state.assignment_generation_high_water = request.assignment_generation;
  state.last_request                      = request;
  state.consistent                        = true;
  if (request.operation == f1ap_ntn_ul_slot_resource_operation::clear) {
    state.active_assignment.reset();
    return;
  }

  if (!actual_request.has_value()) {
    state.consistent = false;
    return;
  }

  du_ntn_ue_slot_resource_snapshot_entry entry;
  entry.ue_index              = ue_index;
  entry.cell_index            = cell_index;
  entry.assignment_generation = request.assignment_generation;
  entry.request               = *actual_request;
  // C-RNTI is copied from the current DU UE context when the connection-bound audit response is built.
  entry.request.requested_c_rnti.reset();
  state.active_assignment = std::move(entry);
}

void du_ran_resource_manager_impl::mark_slot_snapshot_inconsistent(du_ue_index_t                  ue_index,
                                                                   std::optional<du_cell_index_t> cell_index,
                                                                   bool include_uncommitted_request)
{
  std::lock_guard<std::mutex> lock(ntn_ue_slot_registry_mutex);
  if (ntn_ue_slot_registry.contains(ue_index) &&
      (include_uncommitted_request || ntn_ue_slot_registry[ue_index].assignment_generation_high_water != 0)) {
    ntn_ue_slot_registry[ue_index].consistent         = false;
    ntn_ue_slot_registry[ue_index].update_in_progress = false;
    if (cell_index.has_value()) {
      ntn_ue_slot_registry[ue_index].cell_index = *cell_index;
    }
  }
}

void du_ran_resource_manager_impl::clear_slot_update_in_progress(du_ue_index_t ue_index)
{
  std::lock_guard<std::mutex> lock(ntn_ue_slot_registry_mutex);
  if (ntn_ue_slot_registry.contains(ue_index)) {
    ntn_ue_slot_registry[ue_index].update_in_progress = false;
  }
}

du_ntn_ue_slot_resource_snapshot
du_ran_resource_manager_impl::get_ntn_ue_slot_resource_snapshot(du_cell_index_t cell_index) const
{
  du_ntn_ue_slot_resource_snapshot snapshot;
  snapshot.cell_index = cell_index;
  if (fmt::underlying(cell_index) >= cell_cfg_list.size()) {
    snapshot.failure_reason = "invalid_cell";
    return snapshot;
  }

  snapshot.complete = true;
  std::lock_guard<std::mutex> lock(ntn_ue_slot_registry_mutex);
  for (const ntn_ue_slot_registry_state& state : ntn_ue_slot_registry) {
    if (state.cell_index != cell_index) {
      continue;
    }
    snapshot.assignment_generation_high_water =
        std::max(snapshot.assignment_generation_high_water, state.assignment_generation_high_water);
    if (state.update_in_progress) {
      snapshot.complete       = false;
      snapshot.failure_reason = "slot_update_in_progress";
      continue;
    }
    if (!state.consistent) {
      snapshot.complete       = false;
      snapshot.failure_reason = "slot_registry_inconsistent";
      continue;
    }
    if (!state.active_assignment.has_value()) {
      continue;
    }

    const du_ntn_ue_slot_resource_snapshot_entry& entry = *state.active_assignment;
    if (entry.ue_index != state.ue_index || entry.cell_index != cell_index || entry.assignment_generation == 0 ||
        entry.assignment_generation != state.assignment_generation_high_water ||
        !is_valid_f1ap_ntn_authoritative_ul_slot_set(entry.request) ||
        entry.request.assignment_generation != entry.assignment_generation) {
      snapshot.complete       = false;
      snapshot.failure_reason = "slot_registry_inconsistent";
      continue;
    }
    snapshot.entries.push_back(entry);
  }

  if (!snapshot.complete) {
    snapshot.entries.clear();
  }
  return snapshot;
}

expected<ue_ran_resource_configurator, std::string> du_ran_resource_manager_impl::create_ue_resource_configurator(
    du_ue_index_t                               ue_index,
    du_cell_index_t                             pcell_index,
    bool                                        has_tc_rnti,
    std::optional<ntn_ul_slot_resource_request> ntn_ul_slot_request)
{
  if (ue_res_pool.contains(ue_index)) {
    return make_unexpected(std::string("Double allocation of same UE not supported"));
  }
  ue_res_pool.emplace(ue_index, *this);
  {
    std::lock_guard<std::mutex> lock(ntn_ue_slot_registry_mutex);
    ntn_ue_slot_registry.emplace(ue_index, ue_index);
  }
  auto& ue_res                       = ue_res_pool[ue_index];
  auto& mcg                          = ue_res.cg_cfg;
  mcg.cell_group.ntn_ul_slot_request = std::move(ntn_ul_slot_request);

  // UE initialized PCell.
  // Note: In case of lack of RAN resource availability, the return will be error type.
  error_type<std::string> err = allocate_cell_resources(ue_index, pcell_index, SERVING_CELL_PCELL_IDX);
  if (not err.has_value()) {
    logger.info("Failed to create a configuration for ue={}. Cause: {}", static_cast<unsigned>(ue_index), err.error());
  }

  // Initialize correct defaults for UE RAN resources dependent on UE capabilities.
  ue_res.ue_cap_manager.handle_ue_creation(ue_res.cg_cfg);

  // Allocate CFRA resources when TC-RNTI was not yet assigned (e.g. during for Handover).
  if (not has_tc_rnti) {
    ra_res_alloc.allocate_cfra_resources(ue_res.cg_cfg);
  }

  return ue_ran_resource_configurator{
      std::make_unique<du_ue_ran_resource_updater_impl>(&mcg, ue_res.ue_cap_manager.summary(), *this, ue_index),
      err.has_value() ? std::string{} : err.error()};
}

du_ue_resource_update_response
du_ran_resource_manager_impl::update_context(du_ue_index_t                         ue_index,
                                             du_cell_index_t                       pcell_idx,
                                             const f1ap_ue_context_update_request& upd_req,
                                             const du_ue_resource_config*          reestablished_context,
                                             const ue_capability_summary*          reestablished_ue_caps)
{
  srsran_assert(ue_res_pool.contains(ue_index), "This function should only be called for an already allocated UE");
  ue_resource_context&           u      = ue_res_pool[ue_index];
  du_ue_resource_config&         ue_mcg = u.cg_cfg;
  du_ue_resource_update_response resp;

  bool apply_ntn_ul_slot_request = upd_req.ntn_ul_slot_request.has_value();
  if (upd_req.ntn_ul_slot_request.has_value() && is_versioned(*upd_req.ntn_ul_slot_request)) {
    const versioned_slot_request_decision decision =
        validate_versioned_slot_request(ue_index, pcell_idx, *upd_req.ntn_ul_slot_request);
    if (decision.action == versioned_slot_request_action::reject) {
      apply_ntn_ul_slot_request = false;
      resp.ntn_ul_slot_result =
          make_versioned_ntn_ul_slot_resource_result(false, decision.reject_reason, *upd_req.ntn_ul_slot_request);
    } else if (decision.action == versioned_slot_request_action::idempotent) {
      apply_ntn_ul_slot_request = false;
      resp.ntn_ul_slot_result   = make_versioned_ntn_ul_slot_resource_result(
          true,
          upd_req.ntn_ul_slot_request->operation == f1ap_ntn_ul_slot_resource_operation::clear
                ? f1ap_ntn_ul_slot_resource_result_reason::clear_applied
                : f1ap_ntn_ul_slot_resource_result_reason::applied,
          *upd_req.ntn_ul_slot_request,
          decision.applied_request);
    }
  } else if (upd_req.ntn_ul_slot_request.has_value()) {
    // Once this UE has accepted a versioned assignment, an old-format request must not bypass its generation guard.
    std::lock_guard<std::mutex> lock(ntn_ue_slot_registry_mutex);
    if (ntn_ue_slot_registry.contains(ue_index) &&
        (ntn_ue_slot_registry[ue_index].assignment_generation_high_water != 0 ||
         ntn_ue_slot_registry[ue_index].update_in_progress)) {
      apply_ntn_ul_slot_request = false;
      resp.ntn_ul_slot_result   = make_f1ap_ntn_ul_slot_resource_result(
          false, f1ap_ntn_ul_slot_resource_result_reason::assignment_generation_conflict, std::nullopt);
    }
  }

  const std::optional<ntn_ul_slot_resource_request> next_ntn_ul_slot_request =
      apply_ntn_ul_slot_request ? make_du_ntn_ul_slot_resource_request(upd_req.ntn_ul_slot_request)
                                : ue_mcg.cell_group.ntn_ul_slot_request;
  const bool pcell_reallocated = !ue_mcg.cell_group.cells.contains(SERVING_CELL_PCELL_IDX) ||
                                 ue_mcg.cell_group.cells[SERVING_CELL_PCELL_IDX].serv_cell_cfg.cell_index != pcell_idx;
  const bool ntn_ul_slot_request_changed =
      apply_ntn_ul_slot_request &&
      !is_same_ntn_ul_slot_resource_request(ue_mcg.cell_group.ntn_ul_slot_request, next_ntn_ul_slot_request);

  // > Deallocate resources for previously configured cells that have now been removed or changed.
  if (ue_mcg.cell_group.cells.contains(0) and ue_mcg.cell_group.cells[0].serv_cell_cfg.cell_index != pcell_idx) {
    // >> PCell changed. Deallocate PCell resources.
    deallocate_cell_resources(ue_index, SERVING_CELL_PCELL_IDX);
  }
  for (serv_cell_index_t scell_idx : upd_req.scells_to_rem) {
    // >> SCells to be removed. Deallocate them.
    deallocate_cell_resources(ue_index, scell_idx);
  }
  for (const f1ap_scell_to_setup& scell : upd_req.scells_to_setup) {
    // >> If SCells to be modified changed DU Cell Index.
    if (ue_mcg.cell_group.cells.contains(scell.serv_cell_index) and
        ue_mcg.cell_group.cells[scell.serv_cell_index].serv_cell_cfg.cell_index != scell.cell_index) {
      deallocate_cell_resources(ue_index, scell.serv_cell_index);
    }
  }

  // > Allocate resources for new or modified cells.
  if (apply_ntn_ul_slot_request && pcell_reallocated) {
    ue_mcg.cell_group.ntn_ul_slot_request = next_ntn_ul_slot_request;
  }
  if (not ue_mcg.cell_group.cells.contains(0) or ue_mcg.cell_group.cells[0].serv_cell_cfg.cell_index != pcell_idx) {
    // >> PCell changed. Allocate new PCell resources.
    error_type<std::string> outcome = allocate_cell_resources(ue_index, pcell_idx, SERVING_CELL_PCELL_IDX);
    if (not outcome.has_value()) {
      resp.procedure_error = outcome;
      mark_slot_snapshot_inconsistent(ue_index,
                                      pcell_idx,
                                      upd_req.ntn_ul_slot_request.has_value() &&
                                          is_versioned(*upd_req.ntn_ul_slot_request) && apply_ntn_ul_slot_request);
      if (upd_req.ntn_ul_slot_request.has_value() && is_versioned(*upd_req.ntn_ul_slot_request) &&
          apply_ntn_ul_slot_request) {
        resp.ntn_ul_slot_result = make_versioned_ntn_ul_slot_resource_result(
            false, get_ntn_ul_slot_reject_reason(outcome.error()), *upd_req.ntn_ul_slot_request);
      }
      return resp;
    }
    if (!apply_ntn_ul_slot_request) {
      // A generation that was verified for the old PCell cannot silently become authoritative for a different cell.
      mark_slot_snapshot_inconsistent(ue_index, pcell_idx);
      if (upd_req.ntn_ul_slot_request.has_value() && is_versioned(*upd_req.ntn_ul_slot_request) &&
          resp.ntn_ul_slot_result.has_value() && resp.ntn_ul_slot_result->accepted) {
        resp.ntn_ul_slot_result = make_versioned_ntn_ul_slot_resource_result(
            false, f1ap_ntn_ul_slot_resource_result_reason::du_context_update_failed, *upd_req.ntn_ul_slot_request);
      }
    }
  }
  for (const f1ap_scell_to_setup& sc : upd_req.scells_to_setup) {
    // >> SCells Added/Modified. Allocate new SCell resources.
    if (not allocate_cell_resources(ue_index, sc.cell_index, sc.serv_cell_index).has_value()) {
      resp.failed_scells.push_back(sc.serv_cell_index);
    }
  }
  if (ntn_ul_slot_request_changed && !pcell_reallocated) {
    bool                    restore_failed = false;
    error_type<std::string> outcome =
        reallocate_pcell_ul_slot_resources(ue_index, ue_mcg, next_ntn_ul_slot_request, restore_failed);
    if (not outcome.has_value()) {
      if (restore_failed) {
        mark_slot_snapshot_inconsistent(ue_index, pcell_idx, is_versioned(*upd_req.ntn_ul_slot_request));
      } else if (is_versioned(*upd_req.ntn_ul_slot_request)) {
        clear_slot_update_in_progress(ue_index);
      }
      resp.ntn_ul_slot_result =
          is_versioned(*upd_req.ntn_ul_slot_request)
              ? make_versioned_ntn_ul_slot_resource_result(
                    false, get_ntn_ul_slot_reject_reason(outcome.error()), *upd_req.ntn_ul_slot_request)
              : make_f1ap_ntn_ul_slot_resource_result(
                    false, get_ntn_ul_slot_reject_reason(outcome.error()), std::nullopt);
    }
  }

  if (apply_ntn_ul_slot_request && upd_req.ntn_ul_slot_request.has_value() &&
      is_versioned(*upd_req.ntn_ul_slot_request) && !resp.ntn_ul_slot_result.has_value()) {
    std::optional<f1ap_ntn_ul_slot_resource_request> actual_request;
    if (upd_req.ntn_ul_slot_request->operation == f1ap_ntn_ul_slot_resource_operation::clear) {
      actual_request = *upd_req.ntn_ul_slot_request;
    } else {
      actual_request = read_actual_slot_assignment(ue_mcg, *upd_req.ntn_ul_slot_request);
    }
    if (!actual_request.has_value()) {
      mark_slot_snapshot_inconsistent(ue_index, pcell_idx, true);
      resp.ntn_ul_slot_result = make_versioned_ntn_ul_slot_resource_result(
          false, f1ap_ntn_ul_slot_resource_result_reason::du_context_update_failed, *upd_req.ntn_ul_slot_request);
    } else {
      stage_versioned_slot_request(ue_index, pcell_idx, *upd_req.ntn_ul_slot_request, actual_request);
      resp.ntn_ul_slot_result = make_versioned_ntn_ul_slot_resource_result(
          true,
          upd_req.ntn_ul_slot_request->operation == f1ap_ntn_ul_slot_resource_operation::clear
              ? f1ap_ntn_ul_slot_resource_result_reason::clear_applied
              : f1ap_ntn_ul_slot_resource_result_reason::applied,
          *upd_req.ntn_ul_slot_request,
          actual_request);
    }
  }

  // Update measGaps based on the UE measConfig.
  meas_cfg_mng.update(ue_mcg, upd_req.meas_cfg);

  // > Process UE NR capabilities and update UE dedicated configuration only if test mode is not configured.
  if (not test_cfg.test_ue.has_value() or test_cfg.test_ue->rnti == rnti_t::INVALID_RNTI) {
    if (reestablished_ue_caps != nullptr) {
      u.ue_cap_manager.update(ue_mcg, *reestablished_ue_caps);
    }
    u.ue_cap_manager.update(ue_mcg, upd_req.ue_cap_rat_list);
  }

  // > Update UE SRBs and DRBs.
  du_ue_bearer_resource_update_response bearer_resp =
      bearer_res_mng.update(ue_mcg,
                            du_ue_bearer_resource_update_request{
                                upd_req.srbs_to_setup, upd_req.drbs_to_setup, upd_req.drbs_to_mod, upd_req.drbs_to_rem},
                            reestablished_context);
  resp.failed_drbs = std::move(bearer_resp.drbs_failed_to_setup);
  resp.failed_drbs.insert(
      resp.failed_drbs.end(), bearer_resp.drbs_failed_to_mod.begin(), bearer_resp.drbs_failed_to_mod.end());

  if (upd_req.ntn_ul_slot_request.has_value() && !resp.ntn_ul_slot_result.has_value()) {
    resp.ntn_ul_slot_result = make_f1ap_ntn_ul_slot_resource_result(
        true,
        next_ntn_ul_slot_request.has_value() ? f1ap_ntn_ul_slot_resource_result_reason::applied
                                             : f1ap_ntn_ul_slot_resource_result_reason::clear_applied,
        next_ntn_ul_slot_request);
  }

  return resp;
}

error_type<std::string> du_ran_resource_manager_impl::reallocate_pcell_ul_slot_resources(
    du_ue_index_t                                      ue_index,
    du_ue_resource_config&                             ue_res,
    const std::optional<ntn_ul_slot_resource_request>& slot_request,
    bool&                                              restore_failed)
{
  restore_failed = false;
  if (!ue_res.cell_group.cells.contains(SERVING_CELL_PCELL_IDX)) {
    return make_unexpected(fmt::format("Unable to reallocate NTN UL slot resources for ue={}: PCell is missing",
                                       fmt::underlying(ue_index)));
  }

  const std::optional<ntn_ul_slot_resource_request> previous_slot_request = ue_res.cell_group.ntn_ul_slot_request;

  auto restore_previous_resources = [&]() -> bool {
    ue_res.cell_group.ntn_ul_slot_request = previous_slot_request;
    if (not srs_res_mng->alloc_resources(ue_res.cell_group)) {
      logger.error("ue={}: Failed to restore previous SRS resources after NTN UL slot reallocation failure",
                   fmt::underlying(ue_index));
      return false;
    }
    if (not pucch_res_mng.alloc_resources(ue_res.cell_group)) {
      srs_res_mng->dealloc_resources(ue_res.cell_group);
      logger.error("ue={}: Failed to restore previous PUCCH resources after NTN UL slot reallocation failure",
                   fmt::underlying(ue_index));
      return false;
    }
    return true;
  };

  pucch_res_mng.dealloc_resources(ue_res.cell_group);
  srs_res_mng->dealloc_resources(ue_res.cell_group);

  ue_res.cell_group.ntn_ul_slot_request = slot_request;
  if (not srs_res_mng->alloc_resources(ue_res.cell_group)) {
    restore_failed = !restore_previous_resources();
    return make_unexpected(fmt::format("Unable to reallocate SRS resources for ue={}", fmt::underlying(ue_index)));
  }

  if (not pucch_res_mng.alloc_resources(ue_res.cell_group)) {
    srs_res_mng->dealloc_resources(ue_res.cell_group);
    restore_failed = !restore_previous_resources();
    return make_unexpected(fmt::format("Unable to reallocate PUCCH resources for ue={}", fmt::underlying(ue_index)));
  }

  return {};
}

void du_ran_resource_manager_impl::deallocate_context(du_ue_index_t ue_index)
{
  srsran_assert(ue_res_pool.contains(ue_index), "This function should only be called for an already allocated UE");
  {
    std::lock_guard<std::mutex> lock(ntn_ue_slot_registry_mutex);
    ntn_ue_slot_registry.erase(ue_index);
  }
  ue_resource_context&   ue_res = ue_res_pool[ue_index];
  du_ue_resource_config& ue_mcg = ue_res.cg_cfg;

  ra_res_alloc.deallocate_cfra_resources(ue_mcg);

  ue_res.ue_cap_manager.release(ue_mcg);

  for (const auto& sc : ue_mcg.cell_group.cells) {
    deallocate_cell_resources(ue_index, sc.serv_cell_idx);
  }

  ue_res_pool.erase(ue_index);
}

void du_ran_resource_manager_impl::ue_config_applied(du_ue_index_t ue_index)
{
  srsran_assert(ue_res_pool.contains(ue_index), "This function should only be called for an already allocated UE");
  ue_resource_context&   ue_res = ue_res_pool[ue_index];
  du_ue_resource_config& ue_mcg = ue_res.cg_cfg;

  // We can remove previously used CFRA resources, if any.
  ra_res_alloc.deallocate_cfra_resources(ue_mcg);
}

error_type<std::string> du_ran_resource_manager_impl::allocate_cell_resources(du_ue_index_t     ue_index,
                                                                              du_cell_index_t   cell_index,
                                                                              serv_cell_index_t serv_cell_index)
{
  du_ue_resource_config& ue_res = ue_res_pool[ue_index].cg_cfg;

  const du_cell_config& cell_cfg_cmn = cell_cfg_list[cell_index];

  if (serv_cell_index == SERVING_CELL_PCELL_IDX) {
    // It is a PCell.
    srsran_assert(not ue_res.cell_group.cells.contains(SERVING_CELL_PCELL_IDX), "Reallocation of PCell detected");
    ue_res.cell_group.cells.emplace(SERVING_CELL_PCELL_IDX);
    ue_res.cell_group.cells[0].serv_cell_idx            = SERVING_CELL_PCELL_IDX;
    ue_res.cell_group.cells[0].serv_cell_cfg            = cell_cfg_cmn.ue_ded_serv_cell_cfg;
    ue_res.cell_group.cells[0].serv_cell_cfg.cell_index = cell_index;
    ue_res.cell_group.mcg_cfg = config_helpers::make_initial_mac_cell_group_config(cell_cfg_cmn.mcg_params);
    // TODO: Move to helper.
    if (cell_cfg_cmn.pcg_params.p_nr_fr1.has_value()) {
      ue_res.cell_group.pcg_cfg.p_nr_fr1 = cell_cfg_cmn.pcg_params.p_nr_fr1->to_int();
    }
    ue_res.cell_group.pcg_cfg.pdsch_harq_codebook = pdsch_harq_ack_codebook::dynamic;

    // Start with removing PUCCH and SRS configurations. This step simplifies the handling of the allocation failure
    // path.
    reset_serv_cell_cfg(ue_res.cell_group.cells[0].serv_cell_cfg);

    if (not srs_res_mng->alloc_resources(ue_res.cell_group)) {
      // Deallocate dedicated Search Spaces.
      ue_res.cell_group.cells[0].serv_cell_cfg.init_dl_bwp.pdcch_cfg->search_spaces.clear();
      return make_unexpected(fmt::format("Unable to allocate SRS resources for cell={}", fmt::underlying(cell_index)));
    }

    if (not pucch_res_mng.alloc_resources(ue_res.cell_group)) {
      // Deallocate previously allocated SRS + dedicated Search Spaces.
      srs_res_mng->dealloc_resources(ue_res.cell_group);
      ue_res.cell_group.cells[0].serv_cell_cfg.init_dl_bwp.pdcch_cfg->search_spaces.clear();
      return make_unexpected(
          fmt::format("Unable to allocate dedicated PUCCH resources for cell={}", fmt::underlying(cell_index)));
    }

  } else {
    srsran_assert(not ue_res.cell_group.cells.contains(serv_cell_index), "Reallocation of SCell detected");
    ue_res.cell_group.cells.emplace(serv_cell_index);
    ue_res.cell_group.cells[serv_cell_index].serv_cell_idx            = serv_cell_index;
    ue_res.cell_group.cells[serv_cell_index].serv_cell_cfg            = cell_cfg_cmn.ue_ded_serv_cell_cfg;
    ue_res.cell_group.cells[serv_cell_index].serv_cell_cfg.cell_index = cell_index;
    // TODO: Allocate SCell params.
  }
  return {};
}

void du_ran_resource_manager_impl::deallocate_cell_resources(du_ue_index_t ue_index, serv_cell_index_t serv_cell_index)
{
  ue_resource_context&   ue_res_updater = ue_res_pool[ue_index];
  du_ue_resource_config& ue_res         = ue_res_updater.cg_cfg;

  // Return resources back to free lists.
  if (serv_cell_index == SERVING_CELL_PCELL_IDX) {
    srsran_assert(not ue_res.cell_group.cells.empty() and
                      ue_res.cell_group.cells[0].serv_cell_cfg.cell_index != INVALID_DU_CELL_INDEX,
                  "Double deallocation of same UE cell resources detected");
    pucch_res_mng.dealloc_resources(ue_res.cell_group);
    srs_res_mng->dealloc_resources(ue_res.cell_group);
    ue_res.cell_group.cells[0].serv_cell_cfg.cell_index = INVALID_DU_CELL_INDEX;
  } else {
    // TODO: Remove of SCell params.
    ue_res.cell_group.cells.erase(serv_cell_index);
  }
}

du_ran_resource_manager_impl::ue_resource_context::ue_resource_context(du_ran_resource_manager_impl& parent) :
  ue_cap_manager(parent.cell_cfg_list, parent.drx_res_mng, parent.logger, parent.test_cfg)
{
}
