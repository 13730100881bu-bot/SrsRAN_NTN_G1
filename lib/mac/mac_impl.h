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

#pragma once

#include "mac_ctrl/mac_controller.h"
#include "mac_dl/mac_dl_processor.h"
#include "mac_ntn_access_calendar_compiler.h"
#include "mac_ntn_access_calendar_manager.h"
#include "mac_sched/mac_scheduler_adapter.h"
#include "mac_sched/rlf_detector.h"
#include "mac_ul/mac_ul_processor.h"
#include "rnti_manager.h"
#include "srsran/mac/mac.h"
#include "srsran/mac/mac_config.h"
#include <mutex>

namespace srsran {

class mac_impl : public mac_interface
{
public:
  explicit mac_impl(const mac_config& mac_cfg);

  mac_cell_rach_handler& get_rach_handler(du_cell_index_t cell_index) override
  {
    return mac_sched->get_cell_rach_handler(cell_index);
  }

  mac_ue_configurator& get_ue_configurator() override { return ctrl_unit; }

  mac_cell_control_information_handler& get_control_info_handler(du_cell_index_t cell_index) override
  {
    return mac_sched->get_cell_control_info_handler(cell_index);
  }

  mac_ue_control_information_handler& get_ue_control_info_handler() override { return *mac_sched; }

  mac_cell_slot_handler& get_slot_handler(du_cell_index_t cell_index) override
  {
    return dl_unit.get_slot_handler(cell_index);
  }

  mac_cell_manager& get_cell_manager() override { return ctrl_unit; }

  mac_pdu_handler& get_pdu_handler() override { return ul_unit; }

  mac_paging_information_handler& get_cell_paging_info_handler() override { return *mac_sched; }

  mac_positioning_measurement_handler& get_positioning_handler() override
  {
    return mac_sched->get_positioning_handler();
  }

  mac_ntn_rnti_lease_pool_result apply_ntn_rnti_lease_pool_update(
      const mac_ntn_rnti_lease_pool_update& request) override
  {
    mac_ntn_rnti_lease_pool_result result;
    if (request.cell_index == INVALID_DU_CELL_INDEX) {
      result.reason = "invalid_cell";
      return result;
    }
    if (request.operation == mac_ntn_rnti_lease_pool_operation::replace ||
        request.operation == mac_ntn_rnti_lease_pool_operation::clear) {
      rnti_table.clear_ntn_rnti_leases(request.cell_index);
    }
    if (request.operation != mac_ntn_rnti_lease_pool_operation::clear) {
      rnti_table.set_ntn_rnti_lease_mode(request.cell_index, true);
      for (rnti_t lease : request.leases) {
        if (rnti_table.add_ntn_rnti_lease(request.cell_index, lease)) {
          result.accepted_leases.push_back(lease);
        } else {
          result.rejected_leases.push_back(lease);
        }
      }
    } else {
      rnti_table.set_ntn_rnti_lease_mode(request.cell_index, false);
    }
    result.accepted = result.rejected_leases.empty();
    result.reason   = result.accepted ? "accepted" : "rejected_lease";
    return result;
  }

  mac_ntn_access_calendar_result
  apply_ntn_access_calendar_update(const mac_ntn_access_calendar_update& request) override
  {
    std::lock_guard<std::mutex> lock(ntn_calendar_mutex);

    mac_ntn_access_calendar_result result;
    result.schedule_version = request.schedule_version;
    result.calendar_hash    = request.calendar_hash;

    if (request.operation != mac_ntn_access_calendar_operation::prepare) {
      return ntn_calendar_manager.handle_query_or_clear(
          request,
          [this](const ntn_access_calendar_request& scheduler_request) {
            return mac_sched->handle_ntn_access_calendar_update(scheduler_request);
          });
    }

    if (const auto cleanup_rejection = ntn_calendar_manager.reject_prepare_if_cleanup_pending(request);
        cleanup_rejection.has_value()) {
      return cleanup_rejection.value();
    }

    if (request.schedule_version == 0 || request.calendar_hash.empty() || request.cycle_duration.count() <= 0 ||
        request.activation_epoch >= request.valid_until ||
        request.cells[0].cell_index == INVALID_DU_CELL_INDEX ||
        request.cells[1].cell_index == INVALID_DU_CELL_INDEX ||
        request.cells[0].cell_index == request.cells[1].cell_index) {
      result.reason = "invalid_prepare_request";
      return result;
    }

    std::array<ntn_access_calendar_request, 2> scheduler_requests;
    std::array<unsigned, 2>                    accepted_intents{};

    for (unsigned i = 0; i != request.cells.size(); ++i) {
      mac_cell_time_mapper& mapper = ctrl_unit.get_time_mapper(request.cells[i].cell_index);
      const auto last_mapping      = mapper.get_last_mapping();
      const auto activation_slot   = mapper.get_slot_point(request.activation_epoch);
      if (!activation_slot.has_value() || !last_mapping.has_value()) {
        result.reason = "cell_time_mapping_unavailable";
        return result;
      }
      mac_ntn_access_calendar_compile_result compile_result = compile_mac_ntn_access_calendar_cell(
          request, request.cells[i], last_mapping.value(), activation_slot.value());
      if (!compile_result.successful()) {
        result.reason = std::move(compile_result.reject_reason);
        return result;
      }
      scheduler_requests[i] = std::move(compile_result.scheduler_request.value());
      accepted_intents[i]   = compile_result.accepted_intents;
    }

    return ntn_calendar_manager.handle_prepare(
        request,
        scheduler_requests,
        accepted_intents,
        [this](const ntn_access_calendar_request& scheduler_request) {
          return mac_sched->handle_ntn_access_calendar_update(scheduler_request);
        });
  }

private:
  /// Used to allocate new TC-RNTIs and convert from C-RNTI to UE index.
  rnti_manager rnti_table;

  /// MAC scheduler.
  std::unique_ptr<mac_scheduler_adapter> mac_sched;

  /// Used to generate MAC DL PDUs and UL grants to be forwarded to the PHY.
  mac_dl_processor dl_unit;

  /// MAC Rx PDU processor.
  mac_ul_processor ul_unit;

  /// Orchestrates the interaction and configuration of other MAC components.
  mac_controller ctrl_unit;

  /// Management-plane calendar records. Scheduler slot processing never takes this mutex.
  std::mutex                      ntn_calendar_mutex;
  mac_ntn_access_calendar_manager ntn_calendar_manager;
};

} // namespace srsran
