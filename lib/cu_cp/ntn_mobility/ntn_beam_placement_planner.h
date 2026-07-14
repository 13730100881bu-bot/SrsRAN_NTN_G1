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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * A copy of the GNU Affero General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */

#pragma once

#include "ntn_served_beam_selector.h"
#include "srsran/cu_cp/cell_meas_manager_config.h"
#include "srsran/cu_cp/cu_cp_types.h"
#include <string>
#include <vector>

namespace srsran {
namespace srs_cu_cp {

/// Runtime state of a static NTN ground beam in the CU-CP placement plan.
enum class ntn_beam_assignment_state { inactive, candidate, active, draining };

/// Current load observed in one ground beam.
struct ntn_beam_load {
  std::string beam_id;
  unsigned    nof_ues  = 0;
  unsigned    nof_drbs = 0;
};

/// Beam scheduling capacity exposed by one DU.
struct ntn_du_beam_capacity {
  du_index_t du_index = du_index_t::invalid;

  /// Maximum number of simultaneously active beams. A value of zero means the DU cannot accept active beams.
  unsigned max_active_beams = 0;

  /// Maximum admitted UE load. A value of zero means unlimited.
  unsigned max_ues = 0;

  /// Maximum admitted DRB load. A value of zero means unlimited.
  unsigned max_drbs = 0;

  /// Supported beam identifiers. An empty list means the DU supports all beams.
  std::vector<std::string> supported_beam_ids;

  /// Current non-NTN or otherwise pre-existing UE load already occupying this DU.
  unsigned current_ues = 0;

  /// Current non-NTN or otherwise pre-existing DRB load already occupying this DU.
  unsigned current_drbs = 0;
};

/// CU-CP decision that maps one ground beam to a DU and a runtime state.
struct ntn_beam_du_assignment {
  std::string               beam_id;
  nr_cell_identity          nci      = nr_cell_identity::min();
  du_index_t                du_index = du_index_t::invalid;
  ntn_beam_assignment_state state    = ntn_beam_assignment_state::inactive;
  double                    elevation_deg = 0.0;
  unsigned                  nof_ues       = 0;
  unsigned                  nof_drbs      = 0;
  bool                      in_hopping_window = false;
  unsigned                  antenna_slot_index = 0;
  unsigned                  nof_antenna_slots  = 0;
  unsigned                  antenna_slot_period = 0;
  unsigned                  sr_slot_offset      = 0;
  unsigned                  sr_slot_period      = 0;
  unsigned                  srs_slot_offset     = 0;
  unsigned                  srs_slot_period     = 0;
};

/// Inputs needed by CU-CP to compute a DU placement plan for NTN ground beams.
struct ntn_beam_placement_request {
  /// Static beam table loaded from configuration.
  std::vector<ntn_beam_position> beams;

  /// Beams currently visible from the serving satellite state.
  std::vector<ntn_served_beam_candidate> visible_beams;

  /// Maximum number of active beams in the CU-CP hopping window. A value of zero means unlimited.
  unsigned max_active_beams = 0;

  /// DU capacities and optional per-DU beam support constraints.
  std::vector<ntn_du_beam_capacity> du_capacities;

  /// Current beam load snapshot.
  std::vector<ntn_beam_load> beam_loads;

  /// Previous plan, used to preserve stable DU ownership and drain beams that leave visibility.
  std::vector<ntn_beam_du_assignment> previous_assignments;
};

/// Computed CU-CP placement plan for NTN ground beams.
struct ntn_beam_placement_plan {
  std::vector<ntn_beam_du_assignment> assignments;
};

/// Extract active beam ids from a placement plan, preserving the order of the visible beam candidates.
std::vector<std::string>
get_active_ntn_beam_ids(const ntn_beam_placement_plan&                     plan,
                        const std::vector<ntn_served_beam_candidate>& visible_beams);

/// Computes stable, deterministic DU placement for NTN ground beams.
class ntn_beam_placement_planner
{
public:
  ntn_beam_placement_plan plan(const ntn_beam_placement_request& request) const;
};

} // namespace srs_cu_cp
} // namespace srsran
