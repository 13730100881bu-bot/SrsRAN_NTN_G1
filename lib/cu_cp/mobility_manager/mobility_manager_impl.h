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

#include "../ntn_mobility/ntn_beam_placement_planner.h"
#include "../ngap_repository.h"
#include "../ue_manager/ue_manager_impl.h"
#include "metrics/mobility_manager_metrics_aggregator.h"
#include "srsran/cu_cp/cu_cp_command_handler.h"
#include "srsran/cu_cp/cu_cp_f1c_handler.h"
#include "srsran/cu_cp/cu_cp_types.h"
#include "srsran/cu_cp/mobility_manager_config.h"
#include "srsran/cu_cp/ntn_location.h"
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace srsran {
namespace srs_cu_cp {

class du_processor_repository;

/// Handler for measurement related events.
class mobility_manager_measurement_handler
{
public:
  virtual ~mobility_manager_measurement_handler() = default;

  /// \brief Handle event where neighbor became better than serving cell.
  virtual void handle_neighbor_better_than_spcell(ue_index_t       ue_index,
                                                  gnb_id_t         neighbor_gnb_id,
                                                  nr_cell_identity neighbor_nci,
                                                  pci_t            neighbor_pci) = 0;

  /// \brief Handle NTN location-based handover trigger.
  virtual bool handle_ntn_location_handover_required(const ntn_location_handover_trigger& trigger) = 0;

  /// \brief Update the current set of NTN beams that can be served by the CU-CP.
  virtual void handle_ntn_served_beams_updated(const std::vector<std::string>& beam_ids) {}

  /// \brief Update the CU-CP NTN beam-to-DU placement plan.
  virtual void handle_ntn_beam_placement_plan_updated(const ntn_beam_placement_plan& plan) {}
};

/// Interface used to capture the mobility management metrics to the CU-CP.
class mobility_manager_metrics_handler
{
public:
  virtual ~mobility_manager_metrics_handler() = default;

  /// \brief Handle new metrics request for the mobility manager of the CU-CP.
  virtual mobility_management_metrics handle_mobility_metrics_report_request() const = 0;
};

/// Basic mobility manager implementation.
class mobility_manager final : public mobility_manager_measurement_handler,
                               public cu_cp_mobility_command_handler,
                               public mobility_manager_metrics_handler
{
public:
  mobility_manager(const mobility_manager_cfg&      cfg,
                   mobility_manager_cu_cp_notifier& cu_cp_notifier_,
                   ngap_repository&                 ngap_db_,
                   du_processor_repository&         du_db_,
                   ue_manager&                      ue_mng_);

  void trigger_handover(pci_t source_pci, rnti_t rnti, pci_t target_pci) override;

  void handle_neighbor_better_than_spcell(ue_index_t       ue_index,
                                          gnb_id_t         neighbor_gnb_id,
                                          nr_cell_identity neighbor_nci,
                                          pci_t            neighbor_pci) override;

  bool handle_ntn_location_handover_required(const ntn_location_handover_trigger& trigger) override;
  void handle_ntn_served_beams_updated(const std::vector<std::string>& beam_ids) override;
  void handle_ntn_beam_placement_plan_updated(const ntn_beam_placement_plan& plan) override;

  mobility_manager_metrics_aggregator& get_metrics_handler() { return metrics_handler; }

  mobility_management_metrics handle_mobility_metrics_report_request() const override
  {
    return metrics_handler.request_metrics_report();
  }

private:
  bool handle_handover(ue_index_t                               ue_index,
                       gnb_id_t                                 neighbor_gnb_id,
                       nr_cell_identity                         neighbor_nci,
                       pci_t                                    neighbor_pci,
                       const std::optional<ntn_handover_context>& ntn_context = std::nullopt,
                       const std::optional<du_index_t>&           planned_target_du_index = std::nullopt);
  bool handle_inter_cu_handover(ue_index_t source_ue_index, gnb_id_t target_gnb_id, nr_cell_identity target_nci);
  bool handle_intra_cu_handover(ue_index_t source_ue_index,
                                pci_t      neighbor_pci,
                                du_index_t source_du_index,
                                du_index_t target_du_index,
                                const std::optional<ntn_handover_context>& ntn_context);

  mobility_manager_cfg             cfg;
  mobility_manager_cu_cp_notifier& cu_cp_notifier;
  ngap_repository&                 ngap_db;
  du_processor_repository&         du_db;
  ue_manager&                      ue_mng;

  mobility_manager_metrics_aggregator metrics_handler;

  std::vector<std::string> current_served_ntn_beam_ids;
  std::map<std::string, ntn_beam_du_assignment> current_ntn_beam_assignments_by_id;

  srslog::basic_logger& logger;
};

} // namespace srs_cu_cp
} // namespace srsran
