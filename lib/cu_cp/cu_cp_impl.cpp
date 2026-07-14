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

#include "cu_cp_impl.h"
#include "du_processor/du_processor_repository.h"
#include "metrics_handler/metrics_handler_impl.h"
#include "routines/amf_connection_loss_routine.h"
#include "routines/cell_activation_routine.h"
#include "routines/initial_context_setup_routine.h"
#include "routines/mobility/inter_cu_handover_execution_target_routine.h"
#include "routines/mobility/inter_cu_handover_source_routine.h"
#include "routines/mobility/inter_cu_handover_target_routine.h"
#include "routines/mobility/intra_cu_handover_routine.h"
#include "routines/mobility/intra_cu_handover_target_routine.h"
#include "routines/pdu_session_resource_modification_routine.h"
#include "routines/pdu_session_resource_release_routine.h"
#include "routines/pdu_session_resource_setup_routine.h"
#include "routines/reestablishment_context_modification_routine.h"
#include "routines/ue_amf_context_release_request_routine.h"
#include "routines/ue_batch_release_routine.h"
#include "routines/ue_context_release_routine.h"
#include "routines/ue_removal_routine.h"
#include "routines/ue_transaction_info_release_routine.h"
#include "srsran/cu_cp/cu_cp_types.h"
#include "srsran/f1ap/cu_cp/f1ap_cu.h"
#include "srsran/f1ap/ntn_ul_slot_resource_request.h"
#include "srsran/nrppa/nrppa.h"
#include "srsran/nrppa/nrppa_factory.h"
#include "srsran/ran/plmn_identity.h"
#include "srsran/rrc/rrc_ue.h"
#include "srsran/support/async/coroutine.h"
#include "srsran/support/synchronization/sync_event.h"
#include <algorithm>
#include <chrono>
#include <dlfcn.h>
#include <future>
#include <map>
#include <set>
#include <thread>

using namespace srsran;
using namespace srs_cu_cp;

static void assert_cu_cp_configuration_valid(const cu_cp_configuration& cfg)
{
  srsran_assert(cfg.services.cu_cp_executor != nullptr, "Invalid CU-CP executor");
  srsran_assert(!cfg.ngap.ngaps.empty(), "No NGAPs configured");
  for (const auto& ngap : cfg.ngap.ngaps) {
    srsran_assert(ngap.n2_gw != nullptr, "Invalid N2 GW client handler");
  }
  srsran_assert(cfg.services.timers != nullptr, "Invalid timers");

  report_error_if_not(cfg.admission.max_nof_dus <= MAX_NOF_DUS, "Invalid max number of DUs");
  report_error_if_not(cfg.admission.max_nof_cu_ups <= MAX_NOF_CU_UPS, "Invalid max number of CU-UPs");
}

static std::optional<ntn_served_beam_scheduler> create_ntn_served_beam_scheduler(const cell_meas_manager_cfg& cfg)
{
  const auto& ntn_cfg = cfg.ntn_location_mobility;
  if (!ntn_cfg.enabled || ntn_cfg.beams.empty()) {
    return std::nullopt;
  }

  ntn_served_beam_scheduler_config scheduler_cfg;
  scheduler_cfg.beams                = ntn_cfg.beams;
  scheduler_cfg.min_elevation_deg    = ntn_cfg.served_beam_min_elevation_deg;
  scheduler_cfg.max_nof_served_beams = ntn_cfg.max_nof_served_beams;
  scheduler_cfg.served_beam_hopping_enabled = ntn_cfg.served_beam_hopping_enabled;
  scheduler_cfg.served_beam_hopping_dwell_updates = ntn_cfg.served_beam_hopping_dwell_updates;
  return ntn_served_beam_scheduler{std::move(scheduler_cfg)};
}

static std::string format_beam_ids(const std::vector<std::string>& beam_ids)
{
  std::string formatted;
  for (unsigned i = 0; i != beam_ids.size(); ++i) {
    if (i != 0) {
      formatted += ",";
    }
    formatted += beam_ids[i];
  }
  return formatted;
}

static std::string format_beam_assignment_state(ntn_beam_assignment_state state)
{
  switch (state) {
    case ntn_beam_assignment_state::inactive:
      return "inactive";
    case ntn_beam_assignment_state::candidate:
      return "candidate";
    case ntn_beam_assignment_state::active:
      return "active";
    case ntn_beam_assignment_state::draining:
      return "draining";
  }
  return "unknown";
}

static cu_cp_ntn_beam_assignment_state to_cu_cp_ntn_beam_assignment_state(ntn_beam_assignment_state state)
{
  switch (state) {
    case ntn_beam_assignment_state::inactive:
      return cu_cp_ntn_beam_assignment_state::inactive;
    case ntn_beam_assignment_state::candidate:
      return cu_cp_ntn_beam_assignment_state::candidate;
    case ntn_beam_assignment_state::active:
      return cu_cp_ntn_beam_assignment_state::active;
    case ntn_beam_assignment_state::draining:
      return cu_cp_ntn_beam_assignment_state::draining;
  }
  return cu_cp_ntn_beam_assignment_state::inactive;
}

static std::string format_beam_assignment(const ntn_beam_du_assignment& assignment)
{
  std::string formatted;
  formatted += assignment.beam_id + ":" + format_beam_assignment_state(assignment.state);
  if (assignment.in_hopping_window) {
    formatted += ":window";
  }
  if (assignment.du_index != du_index_t::invalid) {
    formatted += "@du";
    formatted += std::to_string(du_index_to_uint(assignment.du_index));
  }
  if (assignment.nof_antenna_slots != 0) {
    formatted += ":slot";
    formatted += std::to_string(assignment.antenna_slot_index);
    if (assignment.nof_antenna_slots > 1) {
      formatted += "+";
      formatted += std::to_string(assignment.nof_antenna_slots);
    }
    formatted += "/";
    formatted += std::to_string(assignment.antenna_slot_period);
  }
  if (assignment.sr_slot_period != 0) {
    formatted += ":sr";
    formatted += std::to_string(assignment.sr_slot_offset);
    formatted += "/";
    formatted += std::to_string(assignment.sr_slot_period);
  }
  if (assignment.srs_slot_period != 0) {
    formatted += ":srs";
    formatted += std::to_string(assignment.srs_slot_offset);
    formatted += "/";
    formatted += std::to_string(assignment.srs_slot_period);
  }
  return formatted;
}

static std::string format_beam_placement_plan(const ntn_beam_placement_plan& plan)
{
  unsigned nof_active         = 0;
  unsigned nof_candidate      = 0;
  unsigned nof_draining       = 0;
  unsigned nof_inactive       = 0;
  unsigned nof_window         = 0;
  unsigned nof_scheduled      = 0;
  unsigned nof_entries_shown  = 0;
  std::string entries;

  for (unsigned i = 0; i != plan.assignments.size(); ++i) {
    const auto& assignment = plan.assignments[i];
    switch (assignment.state) {
      case ntn_beam_assignment_state::active:
        ++nof_active;
        break;
      case ntn_beam_assignment_state::candidate:
        ++nof_candidate;
        break;
      case ntn_beam_assignment_state::draining:
        ++nof_draining;
        break;
      case ntn_beam_assignment_state::inactive:
        ++nof_inactive;
        break;
    }
    if (assignment.in_hopping_window) {
      ++nof_window;
    }
    if (assignment.nof_antenna_slots != 0) {
      ++nof_scheduled;
    }

    if (assignment.state == ntn_beam_assignment_state::inactive || nof_entries_shown >= 32) {
      continue;
    }
    if (!entries.empty()) {
      entries += ",";
    }
    entries += format_beam_assignment(assignment);
    ++nof_entries_shown;
  }

  return fmt::format("total={} active={} candidate={} draining={} inactive={} window={} scheduled={} shown=[{}]{}",
                     plan.assignments.size(),
                     nof_active,
                     nof_candidate,
                     nof_draining,
                     nof_inactive,
                     nof_window,
                     nof_scheduled,
                     entries,
                     nof_entries_shown < nof_active + nof_candidate + nof_draining ? "..." : "");
}

static bool are_beam_assignments_equal(const ntn_beam_du_assignment& lhs, const ntn_beam_du_assignment& rhs)
{
  return lhs.beam_id == rhs.beam_id && lhs.nci == rhs.nci && lhs.du_index == rhs.du_index && lhs.state == rhs.state &&
         lhs.elevation_deg == rhs.elevation_deg && lhs.nof_ues == rhs.nof_ues && lhs.nof_drbs == rhs.nof_drbs &&
         lhs.in_hopping_window == rhs.in_hopping_window && lhs.antenna_slot_index == rhs.antenna_slot_index &&
         lhs.nof_antenna_slots == rhs.nof_antenna_slots && lhs.antenna_slot_period == rhs.antenna_slot_period &&
         lhs.sr_slot_offset == rhs.sr_slot_offset && lhs.sr_slot_period == rhs.sr_slot_period &&
         lhs.srs_slot_offset == rhs.srs_slot_offset && lhs.srs_slot_period == rhs.srs_slot_period;
}

static bool are_beam_placement_plans_equal(const ntn_beam_placement_plan& lhs, const ntn_beam_placement_plan& rhs)
{
  if (lhs.assignments.size() != rhs.assignments.size()) {
    return false;
  }
  for (unsigned i = 0; i != lhs.assignments.size(); ++i) {
    if (!are_beam_assignments_equal(lhs.assignments[i], rhs.assignments[i])) {
      return false;
    }
  }
  return true;
}

static std::set<nr_cell_identity> get_ntn_core_reportable_ncis(const ntn_beam_placement_plan& plan)
{
  std::set<nr_cell_identity> ncis;
  for (const auto& assignment : plan.assignments) {
    if (assignment.state == ntn_beam_assignment_state::active ||
        assignment.state == ntn_beam_assignment_state::draining) {
      ncis.insert(assignment.nci);
    }
  }
  return ncis;
}

static std::optional<nr_cell_identity> get_ue_serving_nci_for_ntn_load(du_processor_repository& du_db, cu_cp_ue& ue)
{
  // 优先使用最近一次被接受的 NTN 位置报告。该报告记录了 UE 在 NTN 移动性逻辑中判断出的
  // serving NCI，通常比静态的 PCell 映射更能反映波束切换后的当前位置。
  if (ue.get_meas_context().last_ntn_location_report.has_value()) {
    return ue.get_meas_context().last_ntn_location_report->serving_nci;
  }

  // 没有位置报告时，回退到 UE 当前挂靠的 DU 和 PCell。任一索引无效都说明 CU-CP 暂时无法
  // 从 DU 上下文推导出服务小区 NCI。
  if (ue.get_du_index() == du_index_t::invalid || ue.get_pcell_index() == srs_cu_cp::du_cell_index_t::invalid) {
    return std::nullopt;
  }

  // 通过 DU 索引找到 DU processor，再读取其配置上下文；没有 DU 或上下文时，无法访问
  // served_cells，也就无法把 DU 内部 cell_index 转换成全局的 NR Cell Identity。
  du_processor* processor = du_db.find_du_processor(ue.get_du_index());
  if (processor == nullptr || processor->get_context() == nullptr) {
    return std::nullopt;
  }

  // served_cells 中保存了 DU 本地 cell_index 到 CGI/NCI 的映射。找到 UE 的 PCell 后返回
  // 对应的 NCI，供 NTN 负载统计和 beam placement 匹配使用。
  for (const auto& cell : processor->get_context()->served_cells) {
    if (cell.cell_index == ue.get_pcell_index()) {
      return cell.cgi.nci;
    }
  }
  return std::nullopt;
}

static std::vector<ntn_du_beam_capacity> build_ntn_du_beam_capacities(du_processor_repository&            du_db,
                                                                      ue_manager&                         ue_mng,
                                                                      const std::vector<ntn_beam_position>& beams,
                                                                      unsigned                            max_active_beams,
                                                                      unsigned                            max_ues,
                                                                      unsigned                            max_drbs_per_ue)
{
  std::set<nr_cell_identity> ntn_beam_ncis;
  for (const auto& beam : beams) {
    if (beam.enabled && !beam.beam_id.empty()) {
      ntn_beam_ncis.insert(beam.nci);
    }
  }

  std::map<du_index_t, std::pair<unsigned, unsigned>> non_ntn_loads;
  for (cu_cp_ue* ue : ue_mng.get_ues()) {
    if (ue == nullptr) {
      continue;
    }
    const std::optional<nr_cell_identity> serving_nci = get_ue_serving_nci_for_ntn_load(du_db, *ue);
    if (serving_nci.has_value() && ntn_beam_ncis.count(serving_nci.value()) != 0) {
      continue;
    }

    const du_index_t du_index = ue->get_du_index();
    if (du_index == du_index_t::invalid) {
      continue;
    }

    auto& load = non_ntn_loads[du_index];
    ++load.first;
    load.second += ue->get_up_resource_manager().get_nof_drbs();
  }

  std::vector<ntn_du_beam_capacity> capacities;
  const std::vector<du_index_t>     du_indexes = du_db.get_du_processor_indexes();
  capacities.reserve(du_indexes.size());
  for (du_index_t du_index : du_indexes) {
    const du_configuration_context* du_context = du_db.get_du_processor(du_index).get_context();
    if (du_context == nullptr) {
      continue;
    }

    std::set<nr_cell_identity> served_cell_ncis;
    for (const auto& cell : du_context->served_cells) {
      served_cell_ncis.insert(cell.cgi.nci);
    }

    std::vector<std::string> supported_beam_ids;
    for (const auto& beam : beams) {
      if (!beam.enabled || beam.beam_id.empty()) {
        continue;
      }
      if (served_cell_ncis.count(beam.nci) != 0) {
        supported_beam_ids.push_back(beam.beam_id);
      }
    }

    if (!supported_beam_ids.empty()) {
      const auto load_it = non_ntn_loads.find(du_index);
      const unsigned current_ues  = load_it != non_ntn_loads.end() ? load_it->second.first : 0;
      const unsigned current_drbs = load_it != non_ntn_loads.end() ? load_it->second.second : 0;
      capacities.push_back({du_index,
                            max_active_beams,
                            max_ues,
                            max_ues * max_drbs_per_ue,
                            std::move(supported_beam_ids),
                            current_ues,
                            current_drbs});
    }
  }
  return capacities;
}

static std::vector<ntn_beam_load>
build_ntn_beam_loads(du_processor_repository& du_db, ue_manager& ue_mng, const std::vector<ntn_beam_position>& beams)
{
  std::map<nr_cell_identity, std::string> nci_to_beam_id;
  for (const auto& beam : beams) {
    if (beam.enabled && !beam.beam_id.empty()) {
      nci_to_beam_id[beam.nci] = beam.beam_id;
    }
  }

  std::map<std::string, ntn_beam_load> loads;
  for (cu_cp_ue* ue : ue_mng.get_ues()) {
    if (ue == nullptr) {
      continue;
    }

    const std::optional<nr_cell_identity> serving_nci = get_ue_serving_nci_for_ntn_load(du_db, *ue);
    if (!serving_nci.has_value()) {
      continue;
    }

    const auto beam_it = nci_to_beam_id.find(serving_nci.value());
    if (beam_it == nci_to_beam_id.end()) {
      continue;
    }

    ntn_beam_load& load = loads[beam_it->second];
    load.beam_id = beam_it->second;
    ++load.nof_ues;
    load.nof_drbs += ue->get_up_resource_manager().get_nof_drbs();
  }

  std::vector<ntn_beam_load> result;
  result.reserve(loads.size());
  for (const auto& entry : loads) {
    result.push_back(entry.second);
  }
  return result;
}

static std::optional<f1ap_ntn_ul_slot_resource_request>
make_ntn_ul_slot_request_for_ue_context_setup(du_processor_repository&        du_db,
                                              cu_cp_ue&                       ue,
                                              const ntn_beam_placement_plan& current_plan)
{
  // 先解析 UE 当前服务小区的 NCI。没有服务小区时，CU-CP 无法把 UE 映射到当前放置计划中的
  // 某个 NTN 波束分配项，因此也就无法构造需要发给 DU 的上行时隙资源请求。
  const std::optional<nr_cell_identity> serving_nci = get_ue_serving_nci_for_ntn_load(du_db, ue);
  if (!serving_nci.has_value()) {
    return std::nullopt;
  }

  for (const ntn_beam_du_assignment& assignment : current_plan.assignments) {
    // 当前计划可能包含多个波束、多个 DU 的分配项。UE Context Setup 只能使用同时匹配 UE
    // 服务小区 NCI 和当前所属 DU 的那一项。
    if (assignment.nci != serving_nci.value() || assignment.du_index != ue.get_du_index()) {
      continue;
    }
    // candidate/inactive 波束还没有实际服务该 UE。draining 波束虽然正在退出服务集合，但其上
    // 已有 UE 在迁出前仍需要保持有效的 SR/SRS 资源。
    if (assignment.state != ntn_beam_assignment_state::active &&
        assignment.state != ntn_beam_assignment_state::draining) {
      continue;
    }

    f1ap_ntn_ul_slot_resource_request request;
    // period 为 0 表示规划器没有分配该类资源。只有当前 DU/波束确实预留了周期性 SR
    // (Scheduling Request) 资源时，才把 SR offset/period 写入请求。
    if (assignment.sr_slot_period != 0) {
      request.sr_slot_offset = assignment.sr_slot_offset;
      request.sr_slot_period = assignment.sr_slot_period;
    }
    // SRS 与 SR 相互独立，也是可选资源；只有携带有效周期时才写入请求。
    if (assignment.srs_slot_period != 0) {
      request.srs_slot_offset = assignment.srs_slot_offset;
      request.srs_slot_period = assignment.srs_slot_period;
    }
    // 只返回真正包含资源信息的请求。某个分配项可以处于 active/draining 状态，但没有携带面向
    // UE 的 SR 或 SRS 时隙信息；这种情况下，UE Context Setup 应表现为不需要额外请求。
    if (!is_empty(request)) {
      return request;
    }
  }

  return std::nullopt;
}

static bool are_ntn_ul_slot_resource_requests_equal(const f1ap_ntn_ul_slot_resource_request& lhs,
                                                    const f1ap_ntn_ul_slot_resource_request& rhs)
{
  return lhs.sr_slot_offset == rhs.sr_slot_offset && lhs.srs_slot_offset == rhs.srs_slot_offset &&
         lhs.sr_slot_period == rhs.sr_slot_period && lhs.srs_slot_period == rhs.srs_slot_period;
}

static void erase_matching_ntn_ul_slot_request(
    std::unordered_map<ue_index_t, f1ap_ntn_ul_slot_resource_request>& cached_requests,
    ue_index_t                                                        ue_index,
    const f1ap_ntn_ul_slot_resource_request&                          request)
{
  const auto cached_it = cached_requests.find(ue_index);
  if (cached_it != cached_requests.end() && are_ntn_ul_slot_resource_requests_equal(cached_it->second, request)) {
    cached_requests.erase(cached_it);
  }
}

static void restore_or_erase_matching_ntn_ul_slot_request(
    std::unordered_map<ue_index_t, f1ap_ntn_ul_slot_resource_request>& cached_requests,
    ue_index_t                                                        ue_index,
    const f1ap_ntn_ul_slot_resource_request&                          request,
    const std::optional<f1ap_ntn_ul_slot_resource_request>&            restore_request)
{
  const auto cached_it = cached_requests.find(ue_index);
  if (cached_it == cached_requests.end() || !are_ntn_ul_slot_resource_requests_equal(cached_it->second, request)) {
    return;
  }
  if (restore_request.has_value()) {
    cached_it->second = *restore_request;
    return;
  }
  cached_requests.erase(cached_it);
}

void cu_cp_impl::schedule_ntn_ul_slot_updates_for_online_ues()
{
  for (auto cached_it = ntn_ul_slot_requests_by_ue.begin(); cached_it != ntn_ul_slot_requests_by_ue.end();) {
    if (ue_mng.find_ue(cached_it->first) == nullptr) {
      cached_it = ntn_ul_slot_requests_by_ue.erase(cached_it);
      continue;
    }
    ++cached_it;
  }

  auto schedule_slot_update =
      [this](cu_cp_ue&                                        ue,
             const f1ap_ntn_ul_slot_resource_request&         requested_slot_request,
             std::optional<f1ap_ntn_ul_slot_resource_request> restore_request_on_failure) {
        const ue_index_t ue_index = ue.get_ue_index();
        return ue.get_task_sched().schedule_async_task(
            launch_async([this,
                          ue_index,
                          slot_request = requested_slot_request,
                          restore_request_on_failure](coro_context<async_task<void>>& ctx) mutable {
          cu_cp_ue*                                current_ue = nullptr;
          f1ap_ue_context_modification_request    ue_context_mod_request;
          f1ap_ue_context_modification_response   ue_context_mod_response;
          rrc_reconfiguration_procedure_request    rrc_reconfig_args;
          rrc_recfg_v1530_ies                      non_crit_ext;
          bool                                     rrc_reconfig_result = false;

          CORO_BEGIN(ctx);

          current_ue = ue_mng.find_du_ue(ue_index);
          if (current_ue == nullptr || current_ue->get_rrc_ue() == nullptr ||
              current_ue->get_ue_context().reconfiguration_disabled) {
            restore_or_erase_matching_ntn_ul_slot_request(
                ntn_ul_slot_requests_by_ue, ue_index, slot_request, restore_request_on_failure);
            CORO_EARLY_RETURN();
          }

          ue_context_mod_request = {};
          ue_context_mod_request.ue_index            = ue_index;
          ue_context_mod_request.ntn_ul_slot_request = slot_request;

          CORO_AWAIT_VALUE(ue_context_mod_response,
                           du_db.get_du_processor(current_ue->get_du_index())
                               .get_f1ap_handler()
                               .handle_ue_context_modification_request(ue_context_mod_request));

          if (!ue_context_mod_response.success) {
            logger.warning("ue={}: Failed to apply NTN SR/SRS slot update at DU", ue_index);
            restore_or_erase_matching_ntn_ul_slot_request(
                ntn_ul_slot_requests_by_ue, ue_index, slot_request, restore_request_on_failure);
            CORO_EARLY_RETURN();
          }

          current_ue = ue_mng.find_du_ue(ue_index);
          if (current_ue == nullptr || current_ue->get_rrc_ue() == nullptr) {
            restore_or_erase_matching_ntn_ul_slot_request(
                ntn_ul_slot_requests_by_ue, ue_index, slot_request, restore_request_on_failure);
            CORO_EARLY_RETURN();
          }

          if (!ue_context_mod_response.du_to_cu_rrc_info.cell_group_cfg.empty()) {
            rrc_reconfig_args = {};
            non_crit_ext      = {};
            non_crit_ext.master_cell_group = ue_context_mod_response.du_to_cu_rrc_info.cell_group_cfg.copy();
            rrc_reconfig_args.non_crit_ext = std::move(non_crit_ext);

            CORO_AWAIT_VALUE(rrc_reconfig_result,
                             current_ue->get_rrc_ue()->handle_rrc_reconfiguration_request(rrc_reconfig_args));
            if (!rrc_reconfig_result) {
              logger.warning("ue={}: Failed to deliver NTN SR/SRS slot RRC reconfiguration", ue_index);
              restore_or_erase_matching_ntn_ul_slot_request(
                  ntn_ul_slot_requests_by_ue, ue_index, slot_request, restore_request_on_failure);
              CORO_EARLY_RETURN();
            }
          }

          if (is_empty(slot_request)) {
            erase_matching_ntn_ul_slot_request(ntn_ul_slot_requests_by_ue, ue_index, slot_request);
          }

          CORO_RETURN();
        }));
      };

  auto schedule_slot_clear = [&](cu_cp_ue& ue, const f1ap_ntn_ul_slot_resource_request& previous_slot_request) {
    f1ap_ntn_ul_slot_resource_request                    clear_slot_request;
    std::optional<f1ap_ntn_ul_slot_resource_request>      restore_request{previous_slot_request};
    const ue_index_t                                      ue_index = ue.get_ue_index();
    ntn_ul_slot_requests_by_ue[ue_index]                           = clear_slot_request;
    if (not schedule_slot_update(ue, clear_slot_request, restore_request)) {
      restore_or_erase_matching_ntn_ul_slot_request(
          ntn_ul_slot_requests_by_ue, ue_index, clear_slot_request, restore_request);
      logger.warning("ue={}: Failed to schedule NTN SR/SRS slot clear", ue_index);
    }
  };

  for (cu_cp_ue* ue : ue_mng.get_ues()) {
    if (ue == nullptr) {
      continue;
    }

    const ue_index_t ue_index = ue->get_ue_index();
    if (ue->get_du_index() == du_index_t::invalid || ue->get_pcell_index() == srs_cu_cp::du_cell_index_t::invalid) {
      ntn_ul_slot_requests_by_ue.erase(ue_index);
      continue;
    }
    if (ue->get_ue_context().reconfiguration_disabled || ue->get_rrc_ue() == nullptr) {
      ntn_ul_slot_requests_by_ue.erase(ue_index);
      continue;
    }
    const auto cached_request_it = ntn_ul_slot_requests_by_ue.find(ue_index);
    if (ue->get_cu_up_index() == cu_up_index_t::invalid || ue->get_up_resource_manager().get_nof_drbs() == 0) {
      if (cached_request_it != ntn_ul_slot_requests_by_ue.end() && !is_empty(cached_request_it->second)) {
        schedule_slot_clear(*ue, cached_request_it->second);
      }
      continue;
    }

    std::optional<f1ap_ntn_ul_slot_resource_request> slot_request =
        make_ntn_ul_slot_request_for_ue_context_setup(du_db, *ue, current_ntn_beam_placement_plan);
    if (!slot_request.has_value()) {
      if (cached_request_it != ntn_ul_slot_requests_by_ue.end() && !is_empty(cached_request_it->second)) {
        schedule_slot_clear(*ue, cached_request_it->second);
      }
      continue;
    }

    const f1ap_ntn_ul_slot_resource_request requested_slot_request = *slot_request;
    if (cached_request_it != ntn_ul_slot_requests_by_ue.end() &&
        are_ntn_ul_slot_resource_requests_equal(cached_request_it->second, requested_slot_request)) {
      continue;
    }

    ntn_ul_slot_requests_by_ue[ue_index] = requested_slot_request;
    if (not schedule_slot_update(*ue, requested_slot_request, std::nullopt)) {
      logger.warning("ue={}: Failed to schedule NTN SR/SRS slot update", ue_index);
      erase_matching_ntn_ul_slot_request(ntn_ul_slot_requests_by_ue, ue_index, requested_slot_request);
      continue;
    }
  }
}

void cu_cp_impl::refresh_ntn_beam_placement_for_current_load()
{
  if (current_ntn_served_beam_candidates.empty() && current_ntn_beam_placement_plan.assignments.empty()) {
    return;
  }

  const std::vector<ntn_served_beam_candidate> candidates = current_ntn_served_beam_candidates;
  if (!update_ntn_served_beam_candidates(candidates)) {
    logger.warning("Failed to refresh NTN beam placement after UE bearer load changed");
  }
}

static ngap_location_reporting_request_type make_direct_location_reporting_request()
{
  ngap_location_reporting_request_type request;
  request.event_type = ngap_location_reporting_event_type::direct;
  return request;
}

static bool contains_duplicate_location_report_ref_ids(const std::vector<uint8_t>& ref_ids)
{
  for (unsigned i = 0; i != ref_ids.size(); ++i) {
    for (unsigned j = i + 1; j != ref_ids.size(); ++j) {
      if (ref_ids[i] == ref_ids[j]) {
        return true;
      }
    }
  }
  return false;
}

static bool location_reporting_request_contains_ref(const ngap_location_reporting_request_type& request, uint8_t ref_id)
{
  return std::find(request.area_of_interest_ref_ids.begin(), request.area_of_interest_ref_ids.end(), ref_id) !=
         request.area_of_interest_ref_ids.end();
}

static bool location_reporting_request_has_any_ref(const ngap_location_reporting_request_type& request,
                                                   const std::vector<uint8_t>&                 ref_ids)
{
  return std::any_of(ref_ids.begin(), ref_ids.end(), [&request](uint8_t ref_id) {
    return location_reporting_request_contains_ref(request, ref_id);
  });
}

cu_cp_impl::cu_cp_impl(const cu_cp_configuration& config_) :
  cfg(config_),
  ue_mng(cfg),
  cell_meas_mng(cfg.mobility.meas_manager_config, cell_meas_mobility_notifier, ue_mng),
  du_db(du_repository_config{cfg,
                             *this,
                             get_cu_cp_measurement_config_handler(),
                             get_cu_cp_ue_removal_handler(),
                             get_cu_cp_ue_context_handler(),
                             common_task_sched,
                             ue_mng,
                             conn_notifier,
                             srslog::fetch_basic_logger("CU-CP")}),
  cu_up_db(cu_up_repository_config{cfg, e1ap_ev_notifier, common_task_sched, srslog::fetch_basic_logger("CU-CP")}),
  paging_handler(du_db),
  ngap_db(ngap_repository_config{cfg, get_cu_cp_ngap_handler(), paging_handler, srslog::fetch_basic_logger("CU-CP")}),
  mobility_mng(cfg.mobility.mobility_manager_config, mobility_manager_ev_notifier, ngap_db, du_db, ue_mng),
  controller(cfg,
             get_cu_cp_amf_reconnection_handler(),
             common_task_sched,
             ngap_db,
             cu_up_db,
             du_db,
             ue_mng,
             *cfg.services.cu_cp_executor),
  metrics_hdlr(std::make_unique<metrics_handler_impl>(*cfg.services.cu_cp_executor,
                                                      *cfg.services.timers,
                                                      ue_mng,
                                                      du_db,
                                                      ngap_db,
                                                      mobility_mng)),
  cu_cp_cfgtr(mobility_manager_ev_notifier, du_db, ngap_db, ue_mng)
{
  assert_cu_cp_configuration_valid(cfg);

  ntn_served_beam_sched = create_ntn_served_beam_scheduler(cfg.mobility.meas_manager_config);
  if (ntn_served_beam_sched.has_value()) {
    const auto& ntn_cfg = cfg.mobility.meas_manager_config.ntn_location_mobility;
    logger.debug("Enabled NTN served beam scheduler with {} static beams, min_elevation={}deg, max_served_beams={}",
                 ntn_cfg.beams.size(),
                 ntn_cfg.served_beam_min_elevation_deg,
                 ntn_cfg.max_nof_served_beams);
  }

  nrppa_entity = create_nrppa_entity(cfg, nrppa_cu_cp_ev_notifier, common_task_sched);

  // Connect event notifiers to layers.
  ngap_cu_cp_ev_notifier.connect_cu_cp(get_cu_cp_ngap_handler(), paging_handler);
  nrppa_cu_cp_ev_notifier.connect_cu_cp(get_cu_cp_nrppa_handler());
  mobility_manager_ev_notifier.connect_cu_cp(get_cu_cp_mobility_manager_handler());
  e1ap_ev_notifier.connect_cu_cp(get_cu_cp_e1ap_handler());
  cell_meas_mobility_notifier.connect_mobility_manager(mobility_mng);

  conn_notifier.connect_node_connection_handler(controller);

  const auto& ntn_cfg = cfg.mobility.meas_manager_config.ntn_location_mobility;
  if (ntn_cfg.enabled) {
    auto satellite_updater = create_ntn_satellite_state_updater(ntn_cfg.satellite_state_update,
                                                                *this,
                                                                *cfg.services.timers,
                                                                *cfg.services.cu_cp_executor,
                                                                logger);
    if (!satellite_updater.has_value()) {
      logger.error("Failed to create NTN satellite state updater. Cause: {}", satellite_updater.error());
    } else if (satellite_updater.value() != nullptr) {
      ntn_satellite_updater = std::move(satellite_updater.value());
      ntn_satellite_updater->start();
    }
  }

  // Start statistics report timer.
  statistics_report_timer = cfg.services.timers->create_unique_timer(*cfg.services.cu_cp_executor);
  statistics_report_timer.set(cfg.metrics.statistics_report_period,
                              [this](timer_id_t /*tid*/) { on_statistics_report_timer_expired(); });
  statistics_report_timer.run();
  if (cfg.metrics_notifier != nullptr and cfg.metrics.metrics_report_period.count() != 0) {
    periodic_metric_report_request metric_cfg{cfg.metrics.metrics_report_period, cfg.metrics_notifier};
    metrics_session = metrics_hdlr->create_periodic_report_session(metric_cfg);
  }
}

cu_cp_impl::~cu_cp_impl()
{
  stop();
}

bool cu_cp_impl::start()
{
  std::promise<bool> p;
  std::future<bool>  fut = p.get_future();

  if (not cfg.services.cu_cp_executor->execute([this, &p]() {
        // Start AMF connection procedure.
        controller.amf_connection_handler().connect_to_amf(&p);
      })) {
    report_fatal_error("Failed to initiate CU-CP setup");
  }
  // Block waiting for CU-CP setup to complete.
  return fut.get();
}

void cu_cp_impl::stop()
{
  bool already_stopped = stopped.exchange(true);
  if (already_stopped) {
    return;
  }
  logger.info("Stopping CU-CP...");

  // Shut down components from within CU-CP executor.
  sync_event ev;
  while (not cfg.services.cu_cp_executor->execute([this, token = ev.get_token()]() {
    // Stop statistics gathering.
    statistics_report_timer.stop();
    if (ntn_satellite_updater != nullptr) {
      ntn_satellite_updater->stop();
    }
    if (metrics_session != nullptr) {
      metrics_session->stop();
    }
  })) {
    logger.debug("Failed to dispatch CU-CP stop task. Retrying...");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  ev.wait();

  controller.stop();
  logger.info("CU-CP stopped successfully.");
}

ngap_message_handler* cu_cp_impl::get_ngap_message_handler(const plmn_identity& plmn)
{
  return ngap_db.find_ngap(plmn);
}

bool cu_cp_impl::amfs_are_connected()
{
  if (cfg.ngap.no_core) {
    return true;
  }

  for (const auto& [amf_index, ngap] : ngap_db.get_ngaps()) {
    if (not controller.amf_connection_handler().is_amf_connected(amf_index)) {
      return false;
    }
  }

  return true;
}

#ifndef SRSRAN_HAS_ENTERPRISE

std::unique_ptr<srsran::srs_cu_cp::nrppa_interface>
cu_cp_impl::create_nrppa_entity(const cu_cp_configuration& cu_cp_cfg,
                                nrppa_cu_cp_notifier&      cu_cp_notif,
                                common_task_scheduler&     common_task_sched_)
{
  return create_nrppa(cu_cp_cfg, cu_cp_notif, common_task_sched_);
}

#endif // SRSRAN_HAS_ENTERPRISE

void cu_cp_impl::handle_bearer_context_release_request(const cu_cp_bearer_context_release_request& msg)
{
  cu_cp_ue* ue = ue_mng.find_du_ue(msg.ue_index);
  srsran_assert(ue != nullptr, "ue={}: Could not find DU UE", msg.ue_index);

  if (ue->get_handover_ue_release_timer().is_running()) {
    logger.debug("ue={}: Ignoring Bearer Context Release Request. Cause: Ongoing handover for this UE", msg.ue_index);
    return;
  }

  cu_cp_ue_context_release_request req;
  req.ue_index = msg.ue_index;
  req.cause    = msg.cause;

  // Add PDU Session IDs.
  auto& up_resource_manager            = ue->get_up_resource_manager();
  req.pdu_session_res_list_cxt_rel_req = up_resource_manager.get_pdu_sessions();

  logger.debug("ue={}: Requesting UE context release with cause={}", req.ue_index, req.cause);

  // Schedule on UE task scheduler.
  ue->get_task_sched().schedule_async_task(launch_async([this, req](coro_context<async_task<void>>& ctx) mutable {
    CORO_BEGIN(ctx);
    // Notify NGAP to request a release from the AMF.
    CORO_AWAIT(handle_ue_context_release(req));
    CORO_RETURN();
  }));
}

void cu_cp_impl::handle_bearer_context_inactivity_notification(const cu_cp_inactivity_notification& msg)
{
  if (msg.ue_inactive) {
    cu_cp_ue* ue = ue_mng.find_du_ue(msg.ue_index);
    srsran_assert(ue != nullptr, "ue={}: Could not find DU UE", msg.ue_index);

    if (ue->get_handover_ue_release_timer().is_running()) {
      logger.debug("ue={}: Ignoring UE inactivity. Cause: Ongoing handover for this UE", msg.ue_index);
      return;
    }

    cu_cp_ue_context_release_request req;
    req.ue_index = msg.ue_index;
    req.cause    = ngap_cause_radio_network_t::user_inactivity;

    // Add PDU Session IDs.
    auto& up_resource_manager            = ue->get_up_resource_manager();
    req.pdu_session_res_list_cxt_rel_req = up_resource_manager.get_pdu_sessions();

    logger.debug("ue={}: Requesting UE context release with cause={}", req.ue_index, req.cause);

    // Schedule on UE task scheduler.
    ue->get_task_sched().schedule_async_task(launch_async([this, req](coro_context<async_task<void>>& ctx) mutable {
      CORO_BEGIN(ctx);
      // Notify NGAP to request a release from the AMF.
      CORO_AWAIT(handle_ue_context_release(req));
      CORO_RETURN();
    }));
  } else {
    logger.debug("Inactivity notification level not supported");
  }
}

void cu_cp_impl::handle_e1_release_request(cu_up_index_t cu_up_index)
{
  // TODO
}

bool cu_cp_impl::handle_ue_plmn_selected(ue_index_t ue_index, const plmn_identity& plmn)
{
  if (!controller.is_supported_plmn(plmn)) {
    logger.warning("ue={}: PLMN {} not supported, rejecting UE", ue_index, plmn);
    return false;
  }

  if (!ue_mng.set_plmn(ue_index, plmn)) {
    logger.error("ue={}: Could not set PLMN {}", ue_index, plmn);
    return false;
  }

  // Connect NGAP to RRC UE to NGAP adapter.
  logger.debug("ue={}: Connecting NGAP (plmn={}) to RRC UE adapter", ue_index, plmn);
  ue_mng.get_rrc_ue_ngap_adapter(ue_index).connect_ngap(ngap_db.find_ngap(plmn));

  return true;
}

rrc_ue_reestablishment_context_response
cu_cp_impl::handle_rrc_reestablishment_request(pci_t old_pci, rnti_t old_c_rnti, ue_index_t ue_index)
{
  rrc_ue_reestablishment_context_response reest_context{};

  if (!controller.request_ue_setup(cu_cp_admission_request_type::reestablishment)) {
    logger.debug("ue={}: Rejecting RRC reestablishment. Cause: admission control", ue_index);
    return reest_context;
  }

  ue_index_t old_ue_index = ue_mng.get_ue_index(old_pci, old_c_rnti);
  if (old_ue_index == ue_index_t::invalid || old_ue_index == ue_index) {
    return reest_context;
  }

  auto* const old_ue = ue_mng.find_du_ue(old_ue_index);
  if (old_ue == nullptr) {
    logger.debug("ue={}: Could not find UE", old_ue_index);
    return reest_context;
  }

  // Stop the UE release timer if it is running.
  if (old_ue->get_handover_ue_release_timer().is_running()) {
    logger.debug("ue={}: Stopping handover UE release timer", old_ue_index);
    old_ue->get_handover_ue_release_timer().stop();
  }

  // Cancel any ongoing handover transaction for the UE.
  if (old_ue->get_ho_context().has_value()) {
    logger.debug("ue={}: Cancelling handover transaction", old_ue_index);

    auto* const target_ue = ue_mng.find_du_ue(old_ue->get_ho_context()->target_ue_index);
    if (target_ue == nullptr) {
      logger.debug("ue={}: Could not find UE", old_ue->get_ho_context()->target_ue_index);
    } else {
      target_ue->get_rrc_ue()->cancel_handover_reconfiguration_transaction(
          old_ue->get_ho_context().value().rrc_reconfig_transaction_id);
    }
  }

  // Check if a DRB and SRB2 were setup.
  if (old_ue->get_up_resource_manager().get_drbs().empty()) {
    logger.debug("ue={}: No DRB setup for this UE - rejecting RRC reestablishment", old_ue_index);
    reest_context.ue_index = old_ue_index;
    return reest_context;
  }

  auto srbs = old_ue->get_rrc_ue()->get_srbs();
  if (std::find(srbs.begin(), srbs.end(), srb_id_t::srb2) == srbs.end()) {
    logger.debug("ue={}: SRB2 not setup for this UE - rejecting RRC reestablishment", old_ue_index);
    reest_context.ue_index = old_ue_index;
    return reest_context;
  }

  auto* rrc_ue = old_ue->get_rrc_ue();
  if (rrc_ue == nullptr) {
    logger.debug("ue={}: RRC UE not found for this UE - rejecting RRC reestablishment", old_ue_index);
    reest_context.ue_index = old_ue_index;
    return reest_context;
  }

  // Get RRC Reestablishment UE Context from old UE.
  reest_context                       = rrc_ue->get_context();
  reest_context.old_ue_fully_attached = true;
  reest_context.ue_index              = old_ue_index;

  return reest_context;
}

async_task<bool> cu_cp_impl::handle_rrc_reestablishment_context_modification_required(ue_index_t ue_index)
{
  cu_cp_ue* ue = ue_mng.find_du_ue(ue_index);
  srsran_assert(ue != nullptr, "ue={}: Could not find DU UE", ue_index);
  srsran_assert(ue->get_cu_up_index() != cu_up_index_t::invalid, "ue={}: could not find CU-UP of the UE", ue_index);

  return launch_async<reestablishment_context_modification_routine>(
      ue_index,
      ue->get_security_manager().get_up_as_config(),
      cu_up_db.find_cu_up_processor(ue->get_cu_up_index())->get_e1ap_bearer_context_manager(),
      du_db.get_du_processor(ue->get_du_index()).get_f1ap_handler(),
      ue->get_rrc_ue(),
      get_cu_cp_rrc_ue_interface(),
      ue->get_task_sched(),
      ue->get_up_resource_manager(),
      logger);
}

void cu_cp_impl::handle_rrc_reestablishment_failure(const cu_cp_ue_context_release_request& request)
{
  auto* ue = ue_mng.find_ue(request.ue_index);
  if (ue != nullptr) {
    ue->get_task_sched().schedule_async_task(handle_ue_context_release(request));
  };
}

void cu_cp_impl::handle_rrc_reestablishment_complete(ue_index_t old_ue_index)
{
  auto* ue = ue_mng.find_ue(old_ue_index);
  if (ue != nullptr) {
    ue->get_task_sched().schedule_async_task(handle_ue_removal_request(old_ue_index));
  };
}

void cu_cp_impl::handle_rrc_reconf_complete_indicator(ue_index_t ue_index)
{
  cu_cp_ue* ue = ue_mng.find_du_ue(ue_index);
  srsran_assert(ue != nullptr, "ue={}: Could not find DU UE", ue_index);

  if (ue != nullptr) {
    ue->get_task_sched().schedule_async_task(
        launch_async([this, ue_index, ue_context_mod_request = f1ap_ue_context_modification_request{}](
                         coro_context<async_task<void>>& ctx) mutable {
          CORO_BEGIN(ctx);

          if (ue_mng.find_du_ue(ue_index) == nullptr) {
            CORO_EARLY_RETURN();
          }

          ue_context_mod_request.ue_index               = ue_index;
          ue_context_mod_request.rrc_recfg_complete_ind = f1ap_rrc_recfg_complete_ind::true_value;

          CORO_AWAIT(du_db.get_du_processor(ue_mng.find_du_ue(ue_index)->get_du_index())
                         .get_f1ap_handler()
                         .handle_ue_context_modification_request(ue_context_mod_request));

          CORO_RETURN();
        }));
  }
}

async_task<bool> cu_cp_impl::handle_ue_context_transfer(ue_index_t ue_index, ue_index_t old_ue_index)
{
  if (cu_up_db.get_nof_cu_ups() == 0) {
    logger.warning("No CU-UP connected");
    return launch_async([](coro_context<async_task<bool>>& ctx) {
      CORO_BEGIN(ctx);
      CORO_RETURN(false);
    });
  }

  if (ue_mng.find_ue(ue_index) == nullptr) {
    logger.warning("ue={} not found", ue_index);
    return launch_async([](coro_context<async_task<bool>>& ctx) {
      CORO_BEGIN(ctx);
      CORO_RETURN(false);
    });
  }

  cu_cp_ue* old_ue = ue_mng.find_du_ue(old_ue_index);
  if (old_ue == nullptr) {
    logger.warning("Old UE index={} got removed", old_ue_index);
    return launch_async([](coro_context<async_task<bool>>& ctx) {
      CORO_BEGIN(ctx);
      CORO_RETURN(false);
    });
  }

  // Cancel all ongoing RRC transactions of the old UE.
  old_ue->get_rrc_ue()->get_rrc_ue_control_message_handler().cancel_all_transactions();

  // Task to run in old UE task scheduler.
  auto handle_ue_context_transfer_impl = [this, ue_index, old_ue_index]() {
    if (ue_mng.find_du_ue(old_ue_index) == nullptr) {
      logger.warning("Old UE index={} got removed", old_ue_index);
      return false;
    }

    auto* source_ue = ue_mng.find_du_ue(old_ue_index);
    if (source_ue->get_cu_up_index() == cu_up_index_t::invalid) {
      logger.warning("ue={}: could not find CU-UP of the old UE", old_ue_index);
      return false;
    }

    if (ue_mng.find_du_ue(ue_index) == nullptr) {
      logger.warning("UE index={} got removed", ue_index);
      return false;
    }

    auto* ue = ue_mng.find_du_ue(ue_index);

    // Transfer CU-UP index.
    ue->set_cu_up_index(source_ue->get_cu_up_index());

    // Transfer source F1AP UE context to F1AP.
    if (source_ue->get_du_index() == ue->get_du_index()) {
      const bool result = du_db.get_du_processor(source_ue->get_du_index())
                              .get_f1ap_handler()
                              .handle_ue_id_update(ue_index, old_ue_index);
      if (not result) {
        logger.warning("The F1AP UE context of the old UE index {} does not exist", old_ue_index);
        return false;
      }
    }

    auto* ngap = ngap_db.find_ngap(ue->get_ue_context().plmn);
    if (ngap == nullptr) {
      logger.warning(
          "ue={}: Can't transfer UE context. Cause: NGAP not found for plmn={}", ue_index, ue->get_ue_context().plmn);
      return false;
    }

    // Transfer NGAP UE Context to new UE and remove the old context.
    if (not ngap->update_ue_index(ue_index, old_ue_index, ue_mng.find_ue(ue_index)->get_ngap_cu_cp_ue_notifier())) {
      return false;
    }

    // Connect NGAP to RRC UE to NGAP adapter.
    logger.debug("ue={}: Connecting NGAP (plmn={}) to RRC UE adapter", ue_index, ue->get_ue_context().plmn);
    ue_mng.get_rrc_ue_ngap_adapter(ue_index).connect_ngap(ngap);

    // Transfer E1AP UE Context to new UE and remove old context.
    cu_up_db.find_cu_up_processor(source_ue->get_cu_up_index())->update_ue_index(ue_index, old_ue_index);

    return true;
  };

  // Task that the caller will use to sync with the old UE task scheduler.
  struct transfer_context_task {
    transfer_context_task(cu_cp_impl& parent_, ue_index_t old_ue_index_, unique_function<bool()> callable) :
      parent(parent_),
      old_ue_index(old_ue_index_),
      task([this, callable = std::move(callable)]() { transfer_successful = callable(); })
    {
    }

    void operator()(coro_context<async_task<bool>>& ctx)
    {
      CORO_BEGIN(ctx);

      CORO_AWAIT_VALUE(
          const bool task_run,
          parent.ue_mng.get_task_sched().dispatch_and_await_task_completion(old_ue_index, std::move(task)));

      CORO_RETURN(task_run and transfer_successful);
    }

    cu_cp_impl& parent;
    ue_index_t  old_ue_index;
    unique_task task;

    bool transfer_successful = false;
  };

  return launch_async<transfer_context_task>(*this, old_ue_index, handle_ue_context_transfer_impl);
}

void cu_cp_impl::handle_handover_reconfiguration_sent(const cu_cp_intra_cu_handover_target_request& request)
{
  if (ue_mng.find_du_ue(request.target_ue_index) == nullptr) {
    logger.warning("UE index={} got removed", request.target_ue_index);
    return;
  }

  cu_cp_ue* ue = ue_mng.find_du_ue(request.target_ue_index);

  ue->get_task_sched().schedule_async_task(launch_async<intra_cu_handover_target_routine>(
      request,
      cu_up_db.find_cu_up_processor(ue->get_cu_up_index())->get_e1ap_bearer_context_manager(),
      du_db.get_du_processor(ue->get_du_index()).get_f1ap_handler(),
      *this,
      get_cu_cp_ue_removal_handler(),
      *this,
      ue_mng,
      mobility_mng,
      logger));
}

void cu_cp_impl::handle_handover_ue_context_push(ue_index_t source_ue_index, ue_index_t target_ue_index)
{
  auto* ue = ue_mng.find_ue(target_ue_index);
  srsran_assert(ue != nullptr, "ue={} not found", target_ue_index);
  srsran_assert(
      ue->get_cu_up_index() != cu_up_index_t::invalid, "ue={}: could not find CU-UP of the target UE", target_ue_index);

  auto* ngap = ngap_db.find_ngap(ue->get_ue_context().plmn);
  if (ngap == nullptr) {
    logger.warning(
        "ue={}: could not find NGAP of the target UE for plmn={}", target_ue_index, ue->get_ue_context().plmn);
    return;
  }

  // Transfer NGAP UE Context to new UE and remove the old context.
  if (!ngap->update_ue_index(target_ue_index, source_ue_index, ue->get_ngap_cu_cp_ue_notifier())) {
    return;
  }
  // Transfer E1AP UE Context to new UE and remove old context.
  cu_up_db.find_cu_up_processor(ue->get_cu_up_index())->update_ue_index(target_ue_index, source_ue_index);
}

void cu_cp_impl::handle_ntn_handover_result(const ntn_handover_result& result)
{
  if (!result.success && result.source_reconfiguration_can_resume) {
    if (cu_cp_ue* ue = ue_mng.find_du_ue(result.source_ue_index); ue != nullptr) {
      ue->get_ue_context().reconfiguration_disabled = false;
      logger.debug("ue={}: Restored reconfiguration after NTN handover source preparation failure",
                   result.source_ue_index);
    }
  }

  cell_meas_mng.handle_ntn_handover_result(result);
}

void cu_cp_impl::handle_mobility_ntn_handover_result(const ntn_handover_result& result)
{
  handle_ntn_handover_result(result);
}

async_task<void> cu_cp_impl::handle_ue_context_release(const cu_cp_ue_context_release_request& request)
{
  auto* ue = ue_mng.find_ue(request.ue_index);
  if (ue == nullptr) {
    logger.warning("ue={}: Could not find UE", request.ue_index);
    return launch_async([](coro_context<async_task<void>>& ctx) {
      CORO_BEGIN(ctx);
      CORO_RETURN();
    });
  }

  auto* ngap = ngap_db.find_ngap(ue->get_ue_context().plmn);

  return launch_async<ue_amf_context_release_request_routine>(
      request, ngap ? &ngap->get_ngap_control_message_handler() : nullptr, *this, logger);
}

bool cu_cp_impl::handle_handover_request(ue_index_t                        ue_index,
                                         const plmn_identity&              selected_plmn,
                                         const security::security_context& sec_ctxt)
{
  cu_cp_ue* ue = ue_mng.find_ue(ue_index);
  if (ue == nullptr) {
    logger.debug("ue={}: Could not find UE", ue_index);
    return false;
  }

  if (!handle_ue_plmn_selected(ue_index, selected_plmn)) {
    logger.info("ue={}: PLMN selection failed", ue_index);
    return false;
  }

  if (!ue->get_security_manager().init_security_context(sec_ctxt)) {
    logger.info("ue={}: Security context initialization failed", ue_index);
    return false;
  }

  return true;
}

async_task<expected<ngap_init_context_setup_response, ngap_init_context_setup_failure>>
cu_cp_impl::handle_new_initial_context_setup_request(const ngap_init_context_setup_request& request)
{
  cu_cp_ue* ue = ue_mng.find_du_ue(request.ue_index);
  srsran_assert(ue != nullptr, "ue={}: Could not find UE", request.ue_index);
  rrc_ue_interface* rrc_ue = ue->get_rrc_ue();
  srsran_assert(rrc_ue != nullptr, "ue={}: Could not find RRC UE", request.ue_index);

  auto* ngap = ngap_db.find_ngap(ue->get_ue_context().plmn);
  if (ngap == nullptr) {
    logger.warning("ue={}: Initial context setup failed. Cause: NGAP not found for plmn={}",
                   request.ue_index,
                   ue->get_ue_context().plmn);
    return launch_async(
        [](coro_context<async_task<expected<ngap_init_context_setup_response, ngap_init_context_setup_failure>>>& ctx) {
          CORO_BEGIN(ctx);
          CORO_RETURN(make_unexpected(ngap_init_context_setup_failure{}));
        });
  }

  return launch_async<initial_context_setup_routine>(request,
                                                     *rrc_ue,
                                                     ngap->get_ngap_ue_radio_cap_management_handler(),
                                                     ue->get_security_manager(),
                                                     du_db.get_du_processor(ue->get_du_index()).get_f1ap_handler(),
                                                     get_cu_cp_ngap_handler(),
                                                     logger,
                                                     make_ntn_ul_slot_request_for_ue_context_setup(
                                                         du_db, *ue, current_ntn_beam_placement_plan));
}

async_task<cu_cp_pdu_session_resource_setup_response>
cu_cp_impl::handle_new_pdu_session_resource_setup_request(cu_cp_pdu_session_resource_setup_request& request)
{
  cu_cp_ue* ue = ue_mng.find_du_ue(request.ue_index);
  srsran_assert(ue != nullptr, "ue={}: Could not find DU UE", request.ue_index);

  unsigned expected_drbs = 0;
  for (const auto& setup_item : request.pdu_session_res_setup_items) {
    expected_drbs += setup_item.qos_flow_setup_request_items.size();
  }
  if (!controller.request_ue_setup(cu_cp_admission_request_type::initial_access, 0, expected_drbs)) {
    logger.warning("ue={}: Rejecting PDU session resource setup. Cause: admission control", request.ue_index);
    cu_cp_pdu_session_resource_setup_response response;
    for (const auto& setup_item : request.pdu_session_res_setup_items) {
      cu_cp_pdu_session_res_setup_failed_item failed_item;
      failed_item.pdu_session_id              = setup_item.pdu_session_id;
      failed_item.unsuccessful_transfer.cause = ngap_cause_radio_network_t::radio_res_not_available;
      response.pdu_session_res_failed_to_setup_items.emplace(failed_item.pdu_session_id, failed_item);
    }
    return launch_async([response](coro_context<async_task<cu_cp_pdu_session_resource_setup_response>>& ctx) mutable {
      CORO_BEGIN(ctx);
      CORO_RETURN(response);
    });
  }

  // Select a CU-UP to serve the UE if it is not already assigned.
  if (ue->get_cu_up_index() == cu_up_index_t::invalid) {
    ue->set_cu_up_index(cu_up_db.select_cu_up());
  }
  srsran_assert(ue->get_cu_up_index() != cu_up_index_t::invalid,
                "ue={}: could not find a CU-UP to serve the UE",
                request.ue_index);

  async_task<cu_cp_pdu_session_resource_setup_response> setup_task = launch_async<pdu_session_resource_setup_routine>(
      request,
      ue_mng.get_ue_config(),
      ue->get_security_manager().get_up_as_config(),
      cfg.security.default_security_indication,
      cu_up_db.find_cu_up_processor(ue->get_cu_up_index())->get_e1ap_bearer_context_manager(),
      du_db.get_du_processor(ue->get_du_index()).get_f1ap_handler(),
      ue->get_rrc_ue(),
      get_cu_cp_rrc_ue_interface(),
      ue->get_task_sched(),
      ue->get_up_resource_manager(),
      logger);

  return launch_async([this,
                       setup_task = std::move(setup_task)](
                          coro_context<async_task<cu_cp_pdu_session_resource_setup_response>>& ctx) mutable {
    cu_cp_pdu_session_resource_setup_response response;
    CORO_BEGIN(ctx);
    CORO_AWAIT_VALUE(response, setup_task);
    if (!response.pdu_session_res_setup_response_items.empty()) {
      refresh_ntn_beam_placement_for_current_load();
    }
    CORO_RETURN(response);
  });
}

async_task<cu_cp_pdu_session_resource_modify_response>
cu_cp_impl::handle_new_pdu_session_resource_modify_request(const cu_cp_pdu_session_resource_modify_request& request)
{
  cu_cp_ue* ue = ue_mng.find_du_ue(request.ue_index);
  srsran_assert(ue != nullptr, "ue={}: Could not find DU UE", request.ue_index);
  srsran_assert(
      ue->get_cu_up_index() != cu_up_index_t::invalid, "ue={}: could not find CU-UP of the UE", request.ue_index);

  async_task<cu_cp_pdu_session_resource_modify_response> modify_task =
      launch_async<pdu_session_resource_modification_routine>(
      request,
      cu_up_db.find_cu_up_processor(ue->get_cu_up_index())->get_e1ap_bearer_context_manager(),
      du_db.get_du_processor(ue->get_du_index()).get_f1ap_handler(),
      ue->get_rrc_ue(),
      get_cu_cp_rrc_ue_interface(),
      ue->get_task_sched(),
      ue->get_up_resource_manager(),
      logger);

  return launch_async([this,
                       modify_task = std::move(modify_task)](
                          coro_context<async_task<cu_cp_pdu_session_resource_modify_response>>& ctx) mutable {
    cu_cp_pdu_session_resource_modify_response response;
    CORO_BEGIN(ctx);
    CORO_AWAIT_VALUE(response, modify_task);
    refresh_ntn_beam_placement_for_current_load();
    CORO_RETURN(response);
  });
}

async_task<cu_cp_pdu_session_resource_release_response>
cu_cp_impl::handle_new_pdu_session_resource_release_command(const cu_cp_pdu_session_resource_release_command& command)
{
  cu_cp_ue* ue = ue_mng.find_du_ue(command.ue_index);
  srsran_assert(ue != nullptr, "ue={}: Could not find DU UE", command.ue_index);
  srsran_assert(
      ue->get_cu_up_index() != cu_up_index_t::invalid, "ue={}: could not find CU-UP of the UE", command.ue_index);

  async_task<cu_cp_pdu_session_resource_release_response> release_task =
      launch_async<pdu_session_resource_release_routine>(
      command,
      cu_up_db.find_cu_up_processor(ue->get_cu_up_index())->get_e1ap_bearer_context_manager(),
      du_db.get_du_processor(ue->get_du_index()).get_f1ap_handler(),
      ue->get_rrc_ue(),
      get_cu_cp_rrc_ue_interface(),
      ue->get_task_sched(),
      ue->get_up_resource_manager(),
      logger);

  return launch_async([this,
                       release_task = std::move(release_task)](
                          coro_context<async_task<cu_cp_pdu_session_resource_release_response>>& ctx) mutable {
    cu_cp_pdu_session_resource_release_response response;
    CORO_BEGIN(ctx);
    CORO_AWAIT_VALUE(response, release_task);
    refresh_ntn_beam_placement_for_current_load();
    CORO_RETURN(response);
  });
}

async_task<cu_cp_ue_context_release_complete>
cu_cp_impl::handle_ue_context_release_command(const cu_cp_ue_context_release_command& command)
{
  cu_cp_ue* ue = ue_mng.find_du_ue(command.ue_index);
  srsran_assert(ue != nullptr, "ue={}: Could not find DU UE", command.ue_index);

  e1ap_bearer_context_manager* e1ap_bearer_ctxt_mng = nullptr;
  if (ue->get_cu_up_index() != cu_up_index_t::invalid) {
    e1ap_bearer_ctxt_mng = &cu_up_db.find_cu_up_processor(ue->get_cu_up_index())->get_e1ap_bearer_context_manager();
  }

  return launch_async<ue_context_release_routine>(command,
                                                  e1ap_bearer_ctxt_mng,
                                                  du_db.get_du_processor(ue->get_du_index()).get_f1ap_handler(),
                                                  get_cu_cp_ue_removal_handler(),
                                                  ue_mng,
                                                  logger);
}

async_task<ngap_handover_resource_allocation_response>
cu_cp_impl::handle_ngap_handover_request(const ngap_handover_request& request)
{
  cu_cp_ue* ue = ue_mng.find_du_ue(request.ue_index);
  srsran_assert(ue != nullptr, "ue={}: Could not find DU UE", request.ue_index);

  unsigned expected_drbs = 0;
  for (const auto& pdu_session : request.pdu_session_res_setup_list_ho_req) {
    expected_drbs += pdu_session.qos_flow_setup_request_items.size();
  }
  if (!controller.request_ue_setup(cu_cp_admission_request_type::handover, 0, expected_drbs)) {
    logger.warning("ue={}: Rejecting incoming N2 handover. Cause: admission control", request.ue_index);
    ngap_handover_resource_allocation_response response;
    response.ue_index = request.ue_index;
    response.success  = false;
    response.cause    = ngap_cause_radio_network_t::ho_target_not_allowed;
    return launch_async([response](coro_context<async_task<ngap_handover_resource_allocation_response>>& ctx) mutable {
      CORO_BEGIN(ctx);
      CORO_RETURN(response);
    });
  }

  // Select a CU-UP to serve the UE.
  ue->set_cu_up_index(cu_up_db.select_cu_up());
  srsran_assert(ue->get_cu_up_index() != cu_up_index_t::invalid,
                "ue={}: could not find a CU-UP to serve the UE",
                request.ue_index);

  return start_inter_cu_handover_target_routine(
      request,
      cu_up_db.find_cu_up_processor(ue->get_cu_up_index())->get_e1ap_bearer_context_manager(),
      du_db.get_du_processor(ue->get_du_index()).get_f1ap_handler(),
      get_cu_cp_ue_removal_handler(),
      ue_mng,
      cell_meas_mng,
      cfg.security.default_security_indication,
      logger);
}

void cu_cp_impl::handle_n2_handover_execution(ue_index_t ue_index)
{
  cu_cp_ue* ue = ue_mng.find_du_ue(ue_index);
  srsran_assert(ue != nullptr, "ue={}: Could not find DU UE", ue_index);
  srsran_assert(cu_up_db.find_cu_up_processor(uint_to_cu_up_index(0)) != nullptr,
                "cu_up_index={}: could not find CU-UP",
                uint_to_cu_up_index(0));

  ngap_interface* ngap = ngap_db.find_ngap(ue->get_ue_context().plmn);
  if (ngap == nullptr) {
    logger.warning("ue={}: NGAP not found for PLMN={}", ue_index, ue->get_ue_context().plmn);
    return;
  }

  cu_up_index_t    cu_up_index = uint_to_cu_up_index(0); // TODO: Update when mapping from UE index to CU-UP exists
  cu_up_processor* cu_up       = cu_up_db.find_cu_up_processor(cu_up_index);
  if (cu_up == nullptr) {
    logger.warning("ue={}: could not find CU-UP for handover execution. cu_up={}", ue_index, cu_up_index);
    return;
  }
  e1ap_bearer_context_manager& e1ap = cu_up->get_e1ap_bearer_context_manager();

  ue->get_task_sched().schedule_async_task(start_inter_cu_handover_execution_target_routine(ue, e1ap, *ngap, logger));
}

void cu_cp_impl::handle_transmission_of_handover_required()
{
  // Notify mobility manager metrics handler about the requested handover preparation.
  mobility_mng.get_metrics_handler().aggregate_requested_handover_preparation();
}

async_task<bool> cu_cp_impl::handle_new_handover_command(ue_index_t ue_index, byte_buffer command)
{
  // Notify mobility manager metrics handler about the successful handover preparation.
  mobility_mng.get_metrics_handler().aggregate_successful_handover_preparation();

  cu_cp_ue* ue = ue_mng.find_du_ue(ue_index);
  if (ue == nullptr) {
    logger.warning("ue={}: UE not found for handover command handling", ue_index);
    return launch_async([](coro_context<async_task<bool>>& ctx) {
      CORO_BEGIN(ctx);
      CORO_RETURN(false);
    });
  }
  ngap_interface* ngap = ngap_db.find_ngap(ue->get_ue_context().plmn);
  if (ngap == nullptr) {
    logger.warning("ue={}: NGAP not found for PLMN={}", ue_index, ue->get_ue_context().plmn);
    return launch_async([](coro_context<async_task<bool>>& ctx) {
      CORO_BEGIN(ctx);
      CORO_RETURN(false);
    });
  }
  return start_inter_cu_handover_source_routine(
      ue_index, std::move(command), ue_mng, du_db, cu_up_db, ngap->get_ngap_control_message_handler(), logger);
}

ue_index_t cu_cp_impl::handle_ue_index_allocation_request(const nr_cell_global_id_t& cgi, const plmn_identity& plmn)
{
  du_index_t du_index = du_db.find_du(cgi);
  if (du_index == du_index_t::invalid) {
    logger.warning("Could not find DU for CGI={}", cgi.nci);
    return ue_index_t::invalid;
  }

  if (!controller.request_ue_setup(cu_cp_admission_request_type::handover, 1)) {
    logger.warning("Could not allocate new UE index for incoming handover CGI={}. Cause: admission control", cgi.nci);
    return ue_index_t::invalid;
  }

  ue_index_t ue_index = ue_mng.add_ue(du_index);
  if (ue_index == ue_index_t::invalid) {
    logger.warning("Could not allocate new UE index for CGI={}", cgi.nci);
    return ue_index_t::invalid;
  }

  if (!handle_ue_plmn_selected(ue_index, plmn)) {
    logger.warning("ue={}: PLMN selection failed", ue_index);
    ntn_ul_slot_requests_by_ue.erase(ue_index);
    ue_mng.remove_ue(ue_index);
    return ue_index_t::invalid;
  }

  return ue_index;
}

#ifndef SRSRAN_HAS_ENTERPRISE

void cu_cp_impl::handle_dl_ue_associated_nrppa_transport_pdu(ue_index_t ue_index, const byte_buffer& nrppa_pdu)
{
  logger.info("DL UE associated NRPPa messages are not supported");
}

void cu_cp_impl::handle_dl_non_ue_associated_nrppa_transport_pdu(amf_index_t amf_index, const byte_buffer& nrppa_pdu)
{
  logger.info("DL non UE associated NRPPa messages are not supported");
}

nrppa_cu_cp_ue_notifier* cu_cp_impl::handle_new_nrppa_ue(ue_index_t ue_index)
{
  return nullptr;
}

void cu_cp_impl::handle_ul_nrppa_pdu(const byte_buffer&                    nrppa_pdu,
                                     std::variant<ue_index_t, amf_index_t> ue_or_amf_index)
{
  logger.info("UL NRPPa messages are not supported");
}

async_task<trp_information_cu_cp_response_t>
cu_cp_impl::handle_trp_information_request(const trp_information_request_t& request)
{
  logger.info("TRP information requests are not supported");
  return launch_async([](coro_context<async_task<trp_information_cu_cp_response_t>>& ctx) {
    CORO_BEGIN(ctx);
    CORO_RETURN(trp_information_cu_cp_response_t{});
  });
}

#endif // SRSRAN_HAS_ENTERPRISE

void cu_cp_impl::handle_n2_disconnection(amf_index_t amf_index)
{
  std::vector<plmn_identity> plmns = ngap_db.find_ngap(amf_index)->get_ngap_context().get_supported_plmns();

  logger.warning("Handling N2 disconnection. Lost PLMNs: {}", fmt::format("{}", fmt::join(plmns, " ")));

  common_task_sched.schedule_async_task(
      launch_async<amf_connection_loss_routine>(amf_index, cfg, plmns, du_db, *this, ue_mng, controller, logger));
}

std::optional<rrc_meas_cfg>
cu_cp_impl::handle_measurement_config_request(ue_index_t                         ue_index,
                                              nr_cell_identity                   nci,
                                              const std::optional<rrc_meas_cfg>& current_meas_config)
{
  return cell_meas_mng.get_measurement_config(ue_index, nci, current_meas_config);
}

void cu_cp_impl::handle_measurement_report(const ue_index_t ue_index, const rrc_meas_results& meas_results)
{
  cell_meas_mng.report_measurement(ue_index, meas_results);
}

void cu_cp_impl::handle_ue_location_report(const ntn_ue_location_report& location_report)
{
  const ntn_location_report_result result = cell_meas_mng.report_ue_location(location_report);
  if (result != ntn_location_report_result::accepted) {
    logger.debug("ue={}: NTN location report filtered before core-network reporting. result={}",
                 location_report.ue_index,
                 static_cast<unsigned>(result));
    return;
  }

  report_ntn_location_to_core_if_required(location_report);
}

std::optional<cu_cp_user_location_info_nr>
cu_cp_impl::build_ntn_core_user_location_info(const ntn_ue_location_report& report)
{
  std::optional<cell_meas_config> cell_cfg = cell_meas_mng.get_cell_config(report.serving_nci);
  if (!cell_cfg.has_value()) {
    logger.debug("ue={}: Cannot build NTN core location report. Cause: serving nci={:#x} not configured",
                 report.ue_index,
                 report.serving_nci);
    return std::nullopt;
  }

  const auto&          serving_cfg = cell_cfg->serving_cell_cfg;
  nr_cell_global_id_t  nr_cgi{serving_cfg.plmn, report.serving_nci};
  std::optional<tac_t> tac;

  du_index_t du_index = du_db.find_du(nr_cgi);
  if (du_index != du_index_t::invalid) {
    const du_configuration_context* du_context = du_db.get_du_processor(du_index).get_context();
    if (du_context != nullptr) {
      const du_cell_configuration* du_cell = du_context->find_cell(nr_cgi);
      if (du_cell != nullptr) {
        tac = du_cell->tac;
      }
    }
  }

  if (!tac.has_value()) {
    for (const auto& ngap_cfg : cfg.ngap.ngaps) {
      for (const auto& supported_ta : ngap_cfg.supported_tas) {
        const auto plmn_it = std::find_if(supported_ta.plmn_list.begin(),
                                          supported_ta.plmn_list.end(),
                                          [&serving_cfg](const plmn_item& item) {
                                            return item.plmn_id == serving_cfg.plmn;
                                          });
        if (plmn_it != supported_ta.plmn_list.end()) {
          tac = supported_ta.tac;
          break;
        }
      }
      if (tac.has_value()) {
        break;
      }
    }
  }

  if (!tac.has_value()) {
    logger.debug("ue={}: Cannot build NTN core location report. Cause: no TAC for plmn={} nci={:#x}",
                 report.ue_index,
                 serving_cfg.plmn,
                 report.serving_nci);
    return std::nullopt;
  }

  cu_cp_user_location_info_nr user_location;
  user_location.nr_cgi = nr_cgi;
  user_location.tai    = cu_cp_tai{serving_cfg.plmn, tac.value()};

  const auto timestamp_seconds =
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  user_location.time_stamp = static_cast<uint32_t>(timestamp_seconds & 0xffffffffU);

  if (cfg.mobility.meas_manager_config.ntn_location_mobility.enabled) {
    user_location.ntn_derived_tac = tac.value();
  }

  return user_location;
}

bool cu_cp_impl::send_ntn_location_report_to_core(const ngap_location_report& report)
{
  ngap_interface* ngap = ngap_db.find_ngap(report.user_location_info.nr_cgi.plmn_id);
  if (ngap == nullptr) {
    logger.debug("ue={}: Cannot send NTN LocationReport. Cause: NGAP not found for PLMN={}",
                 report.ue_index,
                 report.user_location_info.nr_cgi.plmn_id);
    return false;
  }

  return ngap->get_ngap_control_message_handler().handle_location_report_required(report);
}

bool cu_cp_impl::should_throttle_ntn_core_location_report(ue_index_t ue_index)
{
  const auto min_interval =
      cfg.mobility.meas_manager_config.ntn_location_mobility.core_network_reporting.min_report_interval;
  if (min_interval.count() == 0) {
    return false;
  }

  auto state_it = ntn_core_location_reporting_states.find(ue_index);
  if (state_it == ntn_core_location_reporting_states.end() || !state_it->second.has_last_sent_time) {
    return false;
  }

  return std::chrono::steady_clock::now() - state_it->second.last_sent_time < min_interval;
}

bool cu_cp_impl::is_ntn_serving_cell_core_reportable(nr_cell_identity nci) const
{
  return current_ntn_core_reportable_ncis.count(nci) != 0;
}

void cu_cp_impl::report_ntn_location_to_core_if_required(const ntn_ue_location_report& report)
{
  const auto& reporting_cfg =
      cfg.mobility.meas_manager_config.ntn_location_mobility.core_network_reporting;

  auto state_it = ntn_core_location_reporting_states.find(report.ue_index);
  const bool has_active_amf_requests =
      state_it != ntn_core_location_reporting_states.end() && !state_it->second.active_requests.empty();
  if (!reporting_cfg.local_forwarding_enabled && !has_active_amf_requests) {
    return;
  }

  if (!is_ntn_serving_cell_core_reportable(report.serving_nci)) {
    logger.debug("ue={}: Suppressing NTN LocationReport. Cause: serving nci={:#x} is not core-reportable",
                 report.ue_index,
                 report.serving_nci);
    return;
  }

  auto user_location = build_ntn_core_user_location_info(report);
  if (!user_location.has_value()) {
    return;
  }

  ntn_core_location_reporting_ue_state& state = ntn_core_location_reporting_states[report.ue_index];
  if (should_throttle_ntn_core_location_report(report.ue_index)) {
    logger.debug("ue={}: Throttling NTN LocationReport to core network", report.ue_index);
    return;
  }

  bool sent = false;
  if (reporting_cfg.local_forwarding_enabled) {
    ngap_location_report ngap_report;
    ngap_report.ue_index            = report.ue_index;
    ngap_report.user_location_info  = user_location.value();
    ngap_report.request_type        = make_direct_location_reporting_request();
    sent |= send_ntn_location_report_to_core(ngap_report);
  }

  bool serving_cell_change_detected = false;
  if (!state.last_reported_serving_nci.has_value()) {
    state.last_reported_serving_nci = report.serving_nci;
  } else {
    serving_cell_change_detected = state.last_reported_serving_nci.value() != report.serving_nci;
  }

  bool serving_cell_change_report_sent = false;
  for (const auto& active_request : state.active_requests) {
    if (active_request.event_type == ngap_location_reporting_event_type::change_of_serving_cell &&
        !serving_cell_change_detected) {
      continue;
    }

    ngap_location_report ngap_report;
    ngap_report.ue_index           = report.ue_index;
    ngap_report.user_location_info = user_location.value();
    ngap_report.request_type       = active_request;
    if (active_request.event_type == ngap_location_reporting_event_type::ue_presence_in_area_of_interest) {
      for (uint8_t ref_id : active_request.area_of_interest_ref_ids) {
        ngap_report.ue_presence_in_area_of_interest_list.push_back({ref_id});
      }
    }
    const bool request_sent = send_ntn_location_report_to_core(ngap_report);
    sent |= request_sent;
    if (request_sent && active_request.event_type == ngap_location_reporting_event_type::change_of_serving_cell) {
      serving_cell_change_report_sent = true;
    }
  }

  if (serving_cell_change_report_sent) {
    state.last_reported_serving_nci = report.serving_nci;
  }

  if (sent) {
    state.last_sent_time     = std::chrono::steady_clock::now();
    state.has_last_sent_time = true;
  }
}

ngap_location_reporting_control_response
cu_cp_impl::handle_location_reporting_control(const ngap_location_reporting_control& request)
{
  const auto& reporting_cfg =
      cfg.mobility.meas_manager_config.ntn_location_mobility.core_network_reporting;
  if (!reporting_cfg.amf_control_enabled) {
    return {false, ngap_cause_radio_network_t::unspecified};
  }

  if (ue_mng.find_ue(request.ue_index) == nullptr) {
    return {false, ngap_cause_radio_network_t::unknown_local_ue_ngap_id};
  }

  const auto& request_type = request.request_type;
  if (contains_duplicate_location_report_ref_ids(request_type.area_of_interest_ref_ids)) {
    return {false, ngap_cause_radio_network_t::multiple_location_report_ref_id_instances};
  }

  switch (request_type.event_type) {
    case ngap_location_reporting_event_type::direct: {
      std::optional<ntn_ue_location_report> last_report = cell_meas_mng.get_last_ue_location_report(request.ue_index);
      if (!last_report.has_value()) {
        logger.debug("ue={}: Rejecting direct NTN LocationReportingControl. Cause: no accepted UE location is available",
                     request.ue_index);
        return {false, ngap_cause_radio_network_t::unspecified};
      }
      if (!is_ntn_serving_cell_core_reportable(last_report->serving_nci)) {
        logger.debug("ue={}: Rejecting direct NTN LocationReportingControl. Cause: serving nci={:#x} is not core-reportable",
                     request.ue_index,
                     last_report->serving_nci);
        return {false, ngap_cause_radio_network_t::unspecified};
      }
      std::optional<cu_cp_user_location_info_nr> user_location = build_ntn_core_user_location_info(last_report.value());
      if (!user_location.has_value()) {
        return {false, ngap_cause_radio_network_t::unspecified};
      }
      ngap_location_report ngap_report;
      ngap_report.ue_index           = request.ue_index;
      ngap_report.user_location_info = user_location.value();
      ngap_report.request_type       = request_type;
      if (!send_ntn_location_report_to_core(ngap_report)) {
        return {false, ngap_cause_radio_network_t::unspecified};
      }
      return {};
    }
    case ngap_location_reporting_event_type::change_of_serving_cell:
    case ngap_location_reporting_event_type::ue_presence_in_area_of_interest: {
      ntn_core_location_reporting_ue_state& state = ntn_core_location_reporting_states[request.ue_index];
      if (request_type.event_type == ngap_location_reporting_event_type::change_of_serving_cell) {
        std::optional<ntn_ue_location_report> last_report = cell_meas_mng.get_last_ue_location_report(request.ue_index);
        if (last_report.has_value() && is_ntn_serving_cell_core_reportable(last_report->serving_nci)) {
          state.last_reported_serving_nci = last_report->serving_nci;
        }
        const bool already_active =
            std::any_of(state.active_requests.begin(),
                        state.active_requests.end(),
                        [](const ngap_location_reporting_request_type& active_request) {
                          return active_request.event_type ==
                                 ngap_location_reporting_event_type::change_of_serving_cell;
                        });
        if (already_active) {
          return {};
        }
      }
      if (request_type.event_type == ngap_location_reporting_event_type::ue_presence_in_area_of_interest) {
        if (request_type.area_of_interest_ref_ids.empty()) {
          return {false, ngap_cause_radio_network_t::unspecified};
        }
        for (const auto& active_request : state.active_requests) {
          if (location_reporting_request_has_any_ref(active_request, request_type.area_of_interest_ref_ids)) {
            return {false, ngap_cause_radio_network_t::multiple_location_report_ref_id_instances};
          }
        }
      }
      state.active_requests.push_back(request_type);
      return {};
    }
    case ngap_location_reporting_event_type::stop_change_of_serving_cell: {
      auto state_it = ntn_core_location_reporting_states.find(request.ue_index);
      if (state_it != ntn_core_location_reporting_states.end()) {
        auto& active_requests = state_it->second.active_requests;
        active_requests.erase(std::remove_if(active_requests.begin(),
                                             active_requests.end(),
                                             [](const ngap_location_reporting_request_type& active_request) {
                                               return active_request.event_type ==
                                                      ngap_location_reporting_event_type::change_of_serving_cell;
                                             }),
                              active_requests.end());
      }
      return {};
    }
    case ngap_location_reporting_event_type::stop_ue_presence_in_area_of_interest: {
      auto state_it = ntn_core_location_reporting_states.find(request.ue_index);
      if (state_it != ntn_core_location_reporting_states.end()) {
        auto& active_requests = state_it->second.active_requests;
        active_requests.erase(std::remove_if(active_requests.begin(),
                                             active_requests.end(),
                                             [&request_type](const ngap_location_reporting_request_type& active_request) {
                                               if (active_request.event_type !=
                                                   ngap_location_reporting_event_type::ue_presence_in_area_of_interest) {
                                                 return false;
                                               }
                                               return request_type.area_of_interest_ref_ids.empty() ||
                                                      location_reporting_request_has_any_ref(
                                                          active_request, request_type.area_of_interest_ref_ids);
                                             }),
                              active_requests.end());
      }
      return {};
    }
    case ngap_location_reporting_event_type::cancel_location_report_for_the_ue: {
      if (!request_type.location_report_ref_id_to_be_cancelled.has_value()) {
        ntn_core_location_reporting_states.erase(request.ue_index);
        return {};
      }
      auto state_it = ntn_core_location_reporting_states.find(request.ue_index);
      if (state_it != ntn_core_location_reporting_states.end()) {
        auto& active_requests = state_it->second.active_requests;
        const uint8_t ref_id  = request_type.location_report_ref_id_to_be_cancelled.value();
        active_requests.erase(std::remove_if(active_requests.begin(),
                                             active_requests.end(),
                                             [ref_id](const ngap_location_reporting_request_type& active_request) {
                                               return location_reporting_request_contains_ref(active_request, ref_id);
                                             }),
                              active_requests.end());
      }
      return {};
    }
  }

  return {false, ngap_cause_radio_network_t::unspecified};
}

bool cu_cp_impl::handle_ntn_satellite_state_update(const ecef_coordinates_t& satellite)
{
  if (!ntn_served_beam_sched.has_value()) {
    logger.debug("Ignoring NTN satellite state update because served beam scheduling is disabled");
    return false;
  }

  ntn_served_beam_schedule schedule = ntn_served_beam_sched->update_from_satellite_state(satellite, *this);
  if (!schedule.changed) {
    if (!update_ntn_served_beam_candidates(schedule.candidates)) {
      logger.warning("NTN satellite state kept served beam candidates [{}] but CU-CP rejected the placement update",
                     format_beam_ids(schedule.beam_ids));
      return false;
    }
    logger.debug("NTN satellite state update kept served beam set unchanged: [{}]",
                 format_beam_ids(schedule.beam_ids));
    return true;
  }

  if (!schedule.applied) {
    logger.warning("NTN satellite state update selected served beam set [{}] but CU-CP rejected it",
                   format_beam_ids(schedule.beam_ids));
    return false;
  }

  logger.debug("NTN satellite state update selected served beam set [{}]", format_beam_ids(schedule.beam_ids));
  return true;
}

std::vector<std::string> cu_cp_impl::get_current_ntn_served_beam_ids() const
{
  return current_ntn_active_served_beam_ids;
}

std::vector<cu_cp_ntn_beam_status> cu_cp_impl::get_current_ntn_beam_status() const
{
  std::vector<cu_cp_ntn_beam_status> result;
  result.reserve(current_ntn_beam_placement_plan.assignments.size());
  for (const auto& assignment : current_ntn_beam_placement_plan.assignments) {
    result.push_back({assignment.beam_id,
                      assignment.nci,
                      assignment.du_index,
                      to_cu_cp_ntn_beam_assignment_state(assignment.state),
                      assignment.elevation_deg,
                      assignment.nof_ues,
                      assignment.nof_drbs,
                      assignment.in_hopping_window,
                      assignment.antenna_slot_index,
                      assignment.nof_antenna_slots,
                      assignment.antenna_slot_period,
                      assignment.sr_slot_offset,
                      assignment.sr_slot_period,
                      assignment.srs_slot_offset,
                      assignment.srs_slot_period});
  }
  return result;
}

async_task<cu_cp_ue_context_release_batch_response>
cu_cp_impl::release_ues(const cu_cp_ue_context_release_batch_command& command)
{
  return launch_async<ue_batch_release_routine>(command.ues, *this, ue_mng, logger);
}

void cu_cp_impl::set_ue_admission_enabled(bool enabled)
{
  controller.set_ue_admission_enabled(enabled);
}

cu_cp_admission_control_status cu_cp_impl::get_admission_control_status()
{
  cu_cp_admission_control_status status = {};
  status.ue_admission_enabled                  = controller.is_ue_admission_enabled();
  status.amf_connected                         = amfs_are_connected();
  status.cu_up_connected                       = cu_up_db.get_nof_cu_ups() > 0;
  status.ue_setup_allowed                      = controller.request_ue_setup(cu_cp_admission_request_type::initial_access, 1);
  status.reestablishment_allowed               = controller.request_ue_setup(cu_cp_admission_request_type::reestablishment, 1);
  status.handover_allowed                      = controller.request_ue_setup(cu_cp_admission_request_type::handover, 1);
  status.nof_ues                               = ue_mng.get_nof_ues();
  status.max_nof_ues                           = cfg.admission.max_nof_ues;
  status.nof_drbs                              = ue_mng.get_nof_drbs();
  status.max_nof_drbs                          = cfg.admission.max_nof_ues * cfg.admission.max_nof_drbs_per_ue;
  status.initial_access_max_ue_usage_percent   = cfg.admission.initial_access_watermark.max_ue_usage;
  status.initial_access_max_drb_usage_percent  = cfg.admission.initial_access_watermark.max_drb_usage;
  status.reestablishment_max_ue_usage_percent  = cfg.admission.reestablishment_watermark.max_ue_usage;
  status.reestablishment_max_drb_usage_percent = cfg.admission.reestablishment_watermark.max_drb_usage;
  status.handover_max_ue_usage_percent         = cfg.admission.handover_watermark.max_ue_usage;
  status.handover_max_drb_usage_percent        = cfg.admission.handover_watermark.max_drb_usage;
  status.nof_dus                               = du_db.get_nof_dus();
  status.max_nof_dus                           = cfg.admission.max_nof_dus;
  status.nof_cu_ups                            = cu_up_db.get_nof_cu_ups();
  status.max_nof_cu_ups                        = cfg.admission.max_nof_cu_ups;
  return status;
}

bool cu_cp_impl::handle_cell_config_update_request(nr_cell_identity nci, const serving_cell_meas_config& serv_cell_cfg)
{
  return cell_meas_mng.update_cell_config(nci, serv_cell_cfg);
}

bool cu_cp_impl::update_ntn_served_beams(const std::vector<std::string>& beam_ids)
{
  std::vector<ntn_served_beam_candidate> candidates;
  candidates.reserve(beam_ids.size());
  for (const auto& beam_id : beam_ids) {
    candidates.push_back({beam_id, 0.0});
  }
  return update_ntn_served_beam_candidates(candidates);
}

bool cu_cp_impl::update_ntn_served_beam_candidates(const std::vector<ntn_served_beam_candidate>& candidates)
{
  const auto& ntn_cfg = cfg.mobility.meas_manager_config.ntn_location_mobility;
  ntn_beam_placement_request request;
  request.beams                = ntn_cfg.beams;
  request.visible_beams        = candidates;
  request.max_active_beams     = ntn_cfg.max_nof_served_beams;
  request.du_capacities        = build_ntn_du_beam_capacities(du_db,
                                                              ue_mng,
                                                              ntn_cfg.beams,
                                                              ntn_cfg.max_nof_served_beams,
                                                              cfg.admission.max_nof_ues,
                                                              cfg.admission.max_nof_drbs_per_ue);
  request.beam_loads           = build_ntn_beam_loads(du_db, ue_mng, ntn_cfg.beams);
  request.previous_assignments = current_ntn_beam_placement_plan.assignments;

  ntn_beam_placement_plan next_plan = ntn_beam_planner.plan(request);

  std::vector<std::string> active_beam_ids = get_active_ntn_beam_ids(next_plan, candidates);

  if (!cell_meas_mng.update_ntn_served_beams(active_beam_ids)) {
    return false;
  }

  const bool plan_changed = !are_beam_placement_plans_equal(current_ntn_beam_placement_plan, next_plan);
  current_ntn_served_beam_candidates = candidates;
  current_ntn_active_served_beam_ids = active_beam_ids;
  if (plan_changed) {
    current_ntn_core_reportable_ncis = get_ntn_core_reportable_ncis(next_plan);
    current_ntn_beam_placement_plan = std::move(next_plan);
    mobility_mng.handle_ntn_beam_placement_plan_updated(current_ntn_beam_placement_plan);
    schedule_ntn_ul_slot_updates_for_online_ues();
    logger.debug("Updated NTN beam placement plan [{}]", format_beam_placement_plan(current_ntn_beam_placement_plan));

    if (active_beam_ids.size() != candidates.size()) {
      logger.debug("NTN beam placement kept {}/{} visible beam candidates outside the active served set",
                   candidates.size() - active_beam_ids.size(),
                   candidates.size());
    }
  }
  return true;
}

async_task<cu_cp_intra_cu_handover_response>
cu_cp_impl::handle_intra_cu_handover_request(const cu_cp_intra_cu_handover_request& request,
                                             du_index_t&                            source_du_index,
                                             du_index_t&                            target_du_index)
{
  cu_cp_ue* ue = ue_mng.find_du_ue(request.source_ue_index);
  srsran_assert(ue != nullptr, "ue={}: Could not find DU UE", request.source_ue_index);

  const unsigned expected_drbs = ue->get_up_resource_manager().get_nof_drbs();
  if (!controller.request_ue_setup(cu_cp_admission_request_type::handover, 1, expected_drbs)) {
    logger.warning("ue={}: Rejecting intra-CU handover. Cause: admission control", request.source_ue_index);
    return launch_async([](coro_context<async_task<cu_cp_intra_cu_handover_response>>& ctx) {
      CORO_BEGIN(ctx);
      cu_cp_intra_cu_handover_response response;
      response.success = false;
      CORO_RETURN(response);
    });
  }

  byte_buffer sib1 = du_db.get_du_processor(target_du_index).get_mobility_handler().get_packed_sib1(request.cgi);

  return launch_async<intra_cu_handover_routine>(request,
                                                 std::move(sib1),
                                                 du_db.get_du_processor(source_du_index).get_f1ap_handler(),
                                                 du_db.get_du_processor(target_du_index).get_f1ap_handler(),
                                                 *this,
                                                 ue_mng,
                                                 mobility_mng,
                                                 logger);
}

async_task<void> cu_cp_impl::handle_ue_removal_request(ue_index_t ue_index)
{
  if (ue_mng.find_du_ue(ue_index) == nullptr) {
    logger.warning("ue={}: Could not find DU UE", ue_index);
    return launch_async([](coro_context<async_task<void>>& ctx) {
      CORO_BEGIN(ctx);
      CORO_RETURN();
    });
  }
  auto* ue = ue_mng.find_ue(ue_index);

  du_index_t    du_index    = ue->get_du_index();
  cu_up_index_t cu_up_index = ue->get_cu_up_index();

  e1ap_bearer_context_removal_handler* e1ap_removal_handler = nullptr;
  if (cu_up_index != cu_up_index_t::invalid) {
    e1ap_removal_handler = &cu_up_db.find_cu_up_processor(cu_up_index)->get_e1ap_bearer_context_removal_handler();
  }

  auto*                            ngap                 = ngap_db.find_ngap(ue->get_ue_context().plmn);
  ngap_ue_context_removal_handler* ngap_removal_handler = nullptr;
  if (ngap != nullptr) {
    ngap_removal_handler = &ngap->get_ngap_ue_context_removal_handler();
  }

  ntn_core_location_reporting_states.erase(ue_index);
  ntn_ul_slot_requests_by_ue.erase(ue_index);

  nrppa_ue_context_removal_handler* nrppa_removal_handler = nullptr;
  nrppa_removal_handler                                   = &nrppa_entity->get_nrppa_ue_context_removal_handler();

  return launch_async<ue_removal_routine>(ue_index,
                                          du_db.get_du_processor(du_index).get_rrc_du_handler(),
                                          e1ap_removal_handler,
                                          du_db.get_du_processor(du_index).get_f1ap_handler(),
                                          ngap_removal_handler,
                                          nrppa_removal_handler,
                                          ue_mng,
                                          logger);
}

void cu_cp_impl::handle_pending_ue_task_cancellation(ue_index_t ue_index)
{
  srsran_assert(ue_mng.find_du_ue(ue_index) != nullptr, "ue={}: Could not find DU UE", ue_index);

  // Clear all enqueued tasks for this UE.
  ue_mng.get_task_sched().clear_pending_tasks(ue_index);

  // Cancel running transactions for the RRC UE.
  rrc_ue_interface* rrc_ue = ue_mng.find_du_ue(ue_index)->get_rrc_ue();
  if (rrc_ue != nullptr) {
    rrc_ue->get_controller().stop();
  }
}

void cu_cp_impl::handle_amf_reconnection(amf_index_t amf_index)
{
  if (ngap_db.find_ngap(amf_index) == nullptr) {
    logger.warning("AMF index={} not found", amf_index);
    return;
  }

  std::vector<plmn_identity> served_plmns = ngap_db.find_ngap(amf_index)->get_ngap_context().get_supported_plmns();

  common_task_sched.schedule_async_task(launch_async<cell_activation_routine>(cfg, served_plmns, du_db, logger));
}

void cu_cp_impl::initialize_handover_ue_release_timer(
    ue_index_t                              ue_index,
    std::chrono::milliseconds               handover_ue_release_timeout,
    const cu_cp_ue_context_release_request& ue_context_release_request)
{
  if (ue_mng.find_du_ue(ue_index) == nullptr) {
    logger.warning("ue={}: Could not find UE", ue_index);
    return;
  }

  cu_cp_ue* ue = ue_mng.find_ue(ue_index);

  if (ue->get_handover_ue_release_timer().is_running()) {
    logger.warning("ue={}: handover UE release timer already running", ue_index);
    return;
  }

  // Start timer.
  logger.debug("ue={}: Setting release timer to {}ms", ue_index, handover_ue_release_timeout.count());
  ue->get_handover_ue_release_timer().set(
      handover_ue_release_timeout, [this, ue, ue_context_release_request](timer_id_t /*tid*/) {
        ue->get_task_sched().schedule_async_task(handle_ue_context_release(ue_context_release_request));
      });
  ue->get_handover_ue_release_timer().run();
}

// private

void cu_cp_impl::handle_rrc_ue_creation(ue_index_t ue_index, rrc_ue_interface& rrc_ue)
{
  // Store the RRC UE in the UE manager.
  auto* ue = ue_mng.find_ue(ue_index);
  ue->set_rrc_ue(rrc_ue);

  // Connect RRC UE to NGAP to RRC UE adapter.
  ue_mng.get_ngap_rrc_ue_adapter(ue_index).connect_rrc_ue(rrc_ue.get_rrc_ngap_message_handler());

  // Connect CU-CP to RRC UE adapter.
  ue_mng.get_rrc_ue_cu_cp_adapter(ue_index).connect_cu_cp(get_cu_cp_rrc_ue_interface(),
                                                          get_cu_cp_ue_removal_handler(),
                                                          controller,
                                                          ue->get_up_resource_manager(),
                                                          get_cu_cp_measurement_handler());
}

byte_buffer cu_cp_impl::handle_target_cell_sib1_required(du_index_t du_index, nr_cell_global_id_t cgi)
{
  return du_db.get_du_processor(du_index).get_mobility_handler().get_packed_sib1(cgi);
}

async_task<void> cu_cp_impl::handle_transaction_info_loss(const ue_transaction_info_loss_event& ev)
{
  return launch_async<ue_transaction_info_release_routine>(ev, ue_mng, ngap_db, cu_up_db, *this, logger);
}

ngap_cu_cp_ue_notifier* cu_cp_impl::handle_new_ngap_ue(ue_index_t ue_index)
{
  auto* ue = ue_mng.find_ue(ue_index);
  if (ue == nullptr) {
    return nullptr;
  }
  return &ue->get_ngap_cu_cp_ue_notifier();
}

bool cu_cp_impl::schedule_ue_task(ue_index_t ue_index, async_task<void> task)
{
  if (ue_mng.find_ue_task_scheduler(ue_index) == nullptr) {
    logger.debug("UE task scheduler not found for UE index={}", ue_index);
    return false;
  }

  return ue_mng.find_ue_task_scheduler(ue_index)->schedule_async_task(std::move(task));
}

void cu_cp_impl::on_statistics_report_timer_expired()
{
  // Get number of F1AP UEs.
  unsigned nof_f1ap_ues = du_db.get_nof_f1ap_ues();

  // Get number of RRC UEs.
  unsigned nof_rrc_ues = du_db.get_nof_rrc_ues();

  // Get number of NGAP UEs.
  unsigned nof_ngap_ues = ngap_db.get_nof_ngap_ues();

  // Get number of E1AP UEs.
  unsigned nof_e1ap_ues = cu_up_db.get_nof_e1ap_ues();

  // Get number of CU-CP UEs.
  unsigned nof_cu_cp_ues = ue_mng.get_nof_ues();

  // Log statistics.
  logger.debug("num_f1ap_ues={} num_rrc_ues={} num_ngap_ues={} num_e1ap_ues={} num_cu_cp_ues={}",
               nof_f1ap_ues,
               nof_rrc_ues,
               nof_ngap_ues,
               nof_e1ap_ues,
               nof_cu_cp_ues);

  // Restart timer.
  statistics_report_timer.set(cfg.metrics.statistics_report_period,
                              [this](timer_id_t /*tid*/) { on_statistics_report_timer_expired(); });
  statistics_report_timer.run();
}
