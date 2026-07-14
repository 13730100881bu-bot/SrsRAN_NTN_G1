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

#include "../ue_manager/ue_manager_impl.h"
#include "ntn_location_mobility_controller.h"
#include "srsran/cu_cp/cell_meas_manager_config.h"
#include "srsran/cu_cp/cu_cp_types.h"
#include "srsran/cu_cp/ntn_location.h"
#include "srsran/ran/nr_cgi.h"
#include "srsran/rrc/meas_types.h"
#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

namespace srsran {
namespace srs_cu_cp {

/// Methods used by cell measurement manager to signal measurement events to the mobility manager.
class cell_meas_mobility_manager_notifier
{
public:
  virtual ~cell_meas_mobility_manager_notifier() = default;

  /// \brief Notifies that a neighbor cell became stronger than the current serving cell.
  virtual void on_neighbor_better_than_spcell(ue_index_t       ue_index,
                                              gnb_id_t         neighbor_gnb_id,
                                              nr_cell_identity neighbor_nci,
                                              pci_t            neighbor_pci) = 0;

  /// \brief Notifies that NTN location mobility selected a served target beam for handover.
  virtual bool on_ntn_location_handover_required(const ntn_location_handover_trigger& trigger) { return false; }

  /// \brief Notifies that the currently served NTN beam set changed.
  virtual void on_ntn_served_beams_updated(const std::vector<std::string>& beam_ids) {}
};

/// Basic cell manager implementation
class cell_meas_manager
{
public:
  cell_meas_manager(const cell_meas_manager_cfg&         cfg_,
                    cell_meas_mobility_manager_notifier& mobility_mng_notifier_,
                    ue_manager&                          ue_mng_);
  ~cell_meas_manager() = default;

  std::optional<rrc_meas_cfg>
                                  get_measurement_config(ue_index_t                         ue_index,
                                                         nr_cell_identity                   nci,
                                                         const std::optional<rrc_meas_cfg>& current_meas_config = std::nullopt);
  std::optional<cell_meas_config> get_cell_config(nr_cell_identity nci);
  bool update_cell_config(nr_cell_identity nci, const serving_cell_meas_config& serv_cell_cfg);
  bool update_ntn_served_beams(const std::vector<std::string>& beam_ids);
  void report_measurement(ue_index_t ue_index, const rrc_meas_results& meas_results);
  ntn_location_report_result report_ue_location(const ntn_ue_location_report& location_report);
  std::optional<ntn_ue_location_report> get_last_ue_location_report(ue_index_t ue_index) const;
  void handle_ntn_handover_result(const ntn_handover_result& result);

private:
  /// \brief Generate measurement objects for the given cell configuration.
  void generate_measurement_objects_for_serving_cells();

  void update_measurement_object(nr_cell_identity nci, const serving_cell_meas_config& serving_cell_cfg);

  void store_measurement_results(ue_index_t ue_index, const rrc_meas_results& meas_results);

  cell_meas_manager_cfg                cfg;
  cell_meas_mobility_manager_notifier& mobility_mng_notifier;
  ue_manager&                          ue_mng;
  srslog::basic_logger&                logger;

  std::unordered_map<ssb_frequency_t, rrc_meas_obj_nr>
      ssb_freq_to_meas_object; // unique measurement objects, indexed by SSB frequency.
  std::unordered_map<ssb_frequency_t, std::vector<nr_cell_identity>> ssb_freq_to_ncis;
  std::map<nr_cell_identity, serving_cell_meas_config>               nci_to_serving_cell_meas_config;

  ntn_location_mobility_controller ntn_location_mobility;
};

} // namespace srs_cu_cp
} // namespace srsran
