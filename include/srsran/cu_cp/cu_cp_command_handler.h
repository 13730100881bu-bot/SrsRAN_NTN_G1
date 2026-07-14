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

#include "srsran/cu_cp/cu_cp_types.h"
#include "srsran/ran/ntn.h"
#include "srsran/ran/pci.h"
#include "srsran/ran/rnti.h"
#include "srsran/support/async/async_task.h"
#include <string>
#include <vector>

namespace srsran {
namespace srs_cu_cp {

/// Runtime CU-CP state of one NTN beam assignment.
enum class cu_cp_ntn_beam_assignment_state { inactive, candidate, active, draining };

/// Public snapshot of one NTN beam placement decision.
struct cu_cp_ntn_beam_status {
  std::string beam_id;
  nr_cell_identity nci      = nr_cell_identity::min();
  du_index_t       du_index = du_index_t::invalid;
  cu_cp_ntn_beam_assignment_state state = cu_cp_ntn_beam_assignment_state::inactive;
  double   elevation_deg = 0.0;
  unsigned nof_ues       = 0;
  unsigned nof_drbs      = 0;
  bool     in_hopping_window = false;
  unsigned antenna_slot_index = 0;
  unsigned nof_antenna_slots  = 0;
  unsigned antenna_slot_period = 0;
  unsigned sr_slot_offset      = 0;
  unsigned sr_slot_period      = 0;
  unsigned srs_slot_offset     = 0;
  unsigned srs_slot_period     = 0;
};

class cu_cp_mobility_command_handler
{
public:
  virtual ~cu_cp_mobility_command_handler() = default;

  /// \brief Trigger handover of a given UE to a target cell.
  ///
  /// The UE is uniquely identified in the CU-CP through the serving Cell PCI
  /// and RNTI. The target is identified through the Target PCI.
  virtual void trigger_handover(pci_t source_pci, rnti_t rnti, pci_t target_pci) = 0;
};

class cu_cp_ntn_command_handler
{
public:
  virtual ~cu_cp_ntn_command_handler() = default;

  /// Update CU-CP with the latest NTN satellite position. Returns false when NTN served beam scheduling is disabled
  /// or the resulting served beam update is rejected.
  virtual bool handle_ntn_satellite_state_update(const ecef_coordinates_t& satellite) = 0;

  /// Get the last accepted NTN served beam identifiers selected from satellite state updates.
  virtual std::vector<std::string> get_current_ntn_served_beam_ids() const = 0;

  /// Get the current NTN beam placement plan snapshot, including inactive, candidate, active and draining beams.
  virtual std::vector<cu_cp_ntn_beam_status> get_current_ntn_beam_status() const = 0;
};

class cu_cp_ue_command_handler
{
public:
  virtual ~cu_cp_ue_command_handler() = default;

  /// Trigger release of a batch of UEs using the regular UE context release command path.
  virtual async_task<cu_cp_ue_context_release_batch_response>
  release_ues(const cu_cp_ue_context_release_batch_command& command) = 0;
};

class cu_cp_admission_command_handler
{
public:
  virtual ~cu_cp_admission_command_handler() = default;

  /// Enable or disable the admission of new UEs at the CU-CP.
  virtual void set_ue_admission_enabled(bool enabled) = 0;

  /// Get the current CU-CP admission control status.
  virtual cu_cp_admission_control_status get_admission_control_status() = 0;
};

/// Handler for external commands to the CU-CP.
class cu_cp_command_handler
{
public:
  virtual ~cu_cp_command_handler() = default;

  /// Get handler for mobility commands.
  virtual cu_cp_mobility_command_handler& get_mobility_command_handler() = 0;

  /// Get handler for NTN runtime commands.
  virtual cu_cp_ntn_command_handler& get_ntn_command_handler() = 0;

  /// Get handler for UE administrative commands.
  virtual cu_cp_ue_command_handler& get_ue_command_handler() = 0;

  /// Get handler for admission control commands.
  virtual cu_cp_admission_command_handler& get_admission_command_handler() = 0;
};

} // namespace srs_cu_cp

} // namespace srsran
