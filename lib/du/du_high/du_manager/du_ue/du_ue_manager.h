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

#include "du_ue.h"
#include "du_ue_controller_impl.h"
#include "du_ue_manager_repository.h"
#include "du_ntn_initial_ul_position_store.h"
#include "srsran/adt/slotted_array.h"
#include "srsran/du/du_high/du_manager/du_manager.h"
#include "srsran/du/du_high/du_manager/du_manager_params.h"
#include "srsran/support/async/fifo_async_task_scheduler.h"
#include <unordered_map>

namespace srsran {
namespace srs_du {

/// \brief This entity orchestrates the addition/reconfiguration/removal of UE contexts in the DU.
class du_ue_manager final : public du_ue_manager_repository
{
public:
  explicit du_ue_manager(du_manager_params& cfg_, du_ran_resource_manager& cell_res_alloc);

  du_ue_index_t find_unused_du_ue_index();

  /// \brief Handle the creation of a new UE context when a UL-CCCH is received.
  void handle_ue_create_request(const ul_ccch_indication_message& msg);

  /// \brief Handle the creation of a new UE context by F1AP request.
  async_task<f1ap_ue_context_creation_response> handle_ue_create_request(const f1ap_ue_context_creation_request& msg);

  /// \brief Handle the update of an existing UE context by F1AP request.
  async_task<f1ap_ue_context_update_response> handle_ue_config_request(const f1ap_ue_context_update_request& msg);

  /// \brief Handle the removal of an existing UE context by F1AP request.
  async_task<void> handle_ue_delete_request(const f1ap_ue_delete_request& msg);

  /// \brief Handle the DRB deactivation of an existing UE context by F1AP request.
  async_task<void> handle_ue_drb_deactivation_request(du_ue_index_t ue_index);

  void handle_reestablishment_request(du_ue_index_t new_ue_index, du_ue_index_t old_ue_index);

  void handle_ue_config_applied(du_ue_index_t ue_index);

  /// \brief Handle the configuration of an existing UE context by RIC request.
  async_task<du_mac_sched_control_config_response> handle_ue_config_request(const du_mac_sched_control_config& msg);

  /// \brief Force the interruption of all UE activity.
  async_task<void> stop();

  /// \brief Find a UE context by UE index.
  const du_ue* find_ue(du_ue_index_t ue_index) const override;
  du_ue*       find_ue(du_ue_index_t ue_index) override;

  /// \brief Number of DU UEs currently active.
  size_t nof_ues() const { return ue_db.size(); }

  const auto& get_du_ues() const { return ue_db; }

  /// Consume one exact, connection-bound Initial UL observation query.
  f1ap_ntn_initial_ul_position_result
  handle_ntn_initial_ul_position_query(const f1ap_ntn_initial_ul_position_query& request);

  void invalidate_ntn_initial_ul_positions() { ntn_initial_ul_positions.invalidate_all(); }
  void invalidate_ntn_initial_ul_positions(du_cell_index_t cell_index)
  {
    ntn_initial_ul_positions.invalidate_cell(cell_index);
  }
  void invalidate_ntn_initial_ul_positions(du_cell_index_t cell_index, const std::vector<rnti_t>& rntis)
  {
    ntn_initial_ul_positions.invalidate_rntis(cell_index, rntis);
  }
  void invalidate_ntn_initial_ul_position(gnb_du_ue_f1ap_id_t f1ap_ue_id)
  {
    ntn_initial_ul_positions.erase(f1ap_ue_id);
  }
  void retain_ntn_initial_ul_position_plan(uint64_t schedule_version, const std::string& calendar_hash)
  {
    ntn_initial_ul_positions.retain_plan(schedule_version, calendar_hash);
  }
  void erase_ntn_initial_ul_position_plan(uint64_t schedule_version, const std::string& calendar_hash)
  {
    ntn_initial_ul_positions.erase_plan(schedule_version, calendar_hash);
  }

  /// \brief Schedule an asynchronous task to be executed in the UE control loop.
  void schedule_async_task(du_ue_index_t ue_index, async_task<void> task) override
  {
    ue_ctrl_loop[ue_index].schedule(std::move(task));
  }

  gtpu_teid_pool& get_f1u_teid_pool() override { return *f1u_teid_pool; }

private:
  expected<du_ue*, std::string> add_ue(const du_ue_context& ue_ctx, ue_ran_resource_configurator ue_ran_res) override;
  void                          update_crnti(du_ue_index_t ue_index, rnti_t crnti) override;
  bool store_ntn_initial_ul_position(gnb_du_ue_f1ap_id_t                       f1ap_ue_id,
                                     const mac_ntn_initial_ul_position_record& observation,
                                     std::chrono::steady_clock::time_point     received_at) override;
  du_ue*                        find_rnti(rnti_t rnti) override;
  du_ue*                        find_f1ap_ue_id(gnb_du_ue_f1ap_id_t f1ap_ue_id) override;
  void                          remove_ue(du_ue_index_t ue_index) override;

  du_manager_params&       cfg;
  du_ran_resource_manager& cell_res_alloc;
  srslog::basic_logger&    logger;

  // Pool of available TEIDs for F1-U.
  std::unique_ptr<gtpu_teid_pool> f1u_teid_pool;

  // Mapping of ue_index and rnti to UEs.
  slotted_id_table<du_ue_index_t, du_ue_controller_impl, MAX_NOF_DU_UES> ue_db;
  std::unordered_map<rnti_t, du_ue_index_t>                              rnti_to_ue_index;

  du_ntn_initial_ul_position_store ntn_initial_ul_positions;

  // task event loops indexed by ue_index
  slotted_array<fifo_async_task_scheduler, MAX_NOF_DU_UES> ue_ctrl_loop;

  // Whether new UEs should be created.
  bool stop_accepting_ues = false;
};

} // namespace srs_du
} // namespace srsran
