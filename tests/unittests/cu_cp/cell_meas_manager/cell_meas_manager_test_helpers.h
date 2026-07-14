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

#include "lib/cu_cp/cell_meas_manager/cell_meas_manager_impl.h"
#include "srsran/support/executors/manual_task_worker.h"
#include <gtest/gtest.h>
#include <vector>

namespace srsran {
namespace srs_cu_cp {

struct dummy_mobility_event {
  ue_index_t       ue_index = ue_index_t::invalid;
  gnb_id_t         neighbor_gnb_id{0, 22};
  nr_cell_identity neighbor_nci = nr_cell_identity::min();
  pci_t            neighbor_pci = INVALID_PCI;
};

class dummy_mobility_manager : public cell_meas_mobility_manager_notifier
{
public:
  std::vector<dummy_mobility_event> events;
  std::vector<ntn_location_handover_trigger> ntn_events;
  std::vector<std::vector<std::string>>       ntn_served_beam_updates;
  bool                                       accept_ntn_handover = true;

  void on_neighbor_better_than_spcell(ue_index_t       ue_index,
                                      gnb_id_t         neighbor_gnb_id,
                                      nr_cell_identity neighbor_nci,
                                      pci_t            neighbor_pci) override
  {
    events.push_back({ue_index, neighbor_gnb_id, neighbor_nci, neighbor_pci});
  }

  bool on_ntn_location_handover_required(const ntn_location_handover_trigger& trigger) override
  {
    ntn_events.push_back(trigger);
    return accept_ntn_handover;
  }

  void on_ntn_served_beams_updated(const std::vector<std::string>& beam_ids) override
  {
    ntn_served_beam_updates.push_back(beam_ids);
  }
};

/// Fixture class to create cell meas manager object.
class cell_meas_manager_test : public ::testing::Test
{
protected:
  cell_meas_manager_test();
  ~cell_meas_manager_test() override;

  void create_empty_manager();
  void create_default_manager();
  void create_ntn_location_manager();
  void create_default_manager_with_cell_params();
  void create_manager_with_incomplete_cells_and_periodic_report_at_target_cell();
  void create_manager_without_ncells_and_periodic_report();
  void check_default_meas_cfg(const std::optional<rrc_meas_cfg>& meas_cfg, meas_obj_id_t meas_obj_id);
  void verify_meas_cfg(const std::optional<rrc_meas_cfg>& meas_cfg);
  void verify_empty_meas_cfg(const std::optional<rrc_meas_cfg>& meas_cfg);

  srslog::basic_logger& test_logger  = srslog::fetch_basic_logger("TEST");
  srslog::basic_logger& cu_cp_logger = srslog::fetch_basic_logger("CU-CP", false);

  std::unique_ptr<cell_meas_manager> manager;
  dummy_mobility_manager             mobility_manager;
  manual_task_worker                 ctrl_worker{128};
  timer_manager                      timers;
  cu_cp_configuration                cu_cp_cfg;

  ue_manager ue_mng{cu_cp_cfg};
};

} // namespace srs_cu_cp
} // namespace srsran
