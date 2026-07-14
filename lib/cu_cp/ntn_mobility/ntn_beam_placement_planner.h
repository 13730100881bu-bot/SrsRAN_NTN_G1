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
#include "srsran/cu_cp/ntn_qos_policy.h"
#include <string>
#include <vector>

namespace srsran {
namespace srs_cu_cp {

/// Runtime state of a static NTN ground beam in the CU-CP placement plan.
enum class ntn_beam_assignment_state { inactive, candidate, active_loaded, draining };

/// Return a stable runtime taxonomy name for a beam placement state.
std::string ntn_beam_assignment_state_to_string(ntn_beam_assignment_state state);

/// Current load observed in one ground beam.
struct ntn_beam_load {
  std::string beam_id;
  unsigned    nof_ues       = 0;
  unsigned    nof_drbs      = 0;
  ntn_qos_demand_summary qos;
  bool                      downlink_service_required = true;
  bool                      uplink_resource_required  = true;
};

/// Beam scheduling capacity exposed by one DU.
struct ntn_du_beam_capacity {
  du_index_t du_index = du_index_t::invalid;

  /// Maximum number of simultaneously active_loaded beams. A value of zero means the DU cannot accept active_loaded beams.
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
  ntn_qos_demand_summary    qos;
  unsigned                  resource_weight      = 0;
  double                    resource_share       = 0.0;
  std::string               resource_weight_reason = "none";

  /// DU selected for the parent analog access beam, when analog access policy is configured.
  du_index_t access_du_index = du_index_t::invalid;

  /// Reason for the analog access DU selection.
  std::string access_du_reason = "no_supported_child";

  /// Reason for the final digital service DU policy decision.
  std::string du_assignment_reason = "no_supported_child";

  /// Whether CU-CP resource-domain policy allows this beam to become active_loaded.
  bool resource_domain_eligible = true;

  /// Reason for the CU-CP resource-domain decision.
  std::string resource_domain_reason = "eligible";

  /// Optional digital reuse group id used only for CU-CP observability and policy decisions.
  std::string reuse_group_id;

  /// Explicit conflict groups that prevent simultaneous active_loaded service with matching groups.
  std::vector<std::string> conflict_group_ids;

  unsigned analog_loaded_digital_child_cap  = 0;
  unsigned analog_loaded_digital_child_load = 0;
  unsigned analog_service_bound_ue_cap      = 0;
  unsigned analog_service_bound_ue_load     = 0;
  unsigned analog_drb_cap                   = 0;
  unsigned analog_drb_load                  = 0;
  unsigned digital_ue_cap                   = 0;
  unsigned digital_ue_load                  = 0;
  unsigned digital_drb_cap                  = 0;
  unsigned digital_drb_load                 = 0;

  /// Satellite that currently owns the best-position service geometry for this beam.
  std::string serving_satellite_id = "sat-0";

  /// Link-direction availability and runtime readiness derived from the static beam table and assignment state.
  bool downlink_enabled = true;
  bool uplink_enabled   = true;
  bool downlink_service_required = true;
  bool uplink_resource_required  = true;
  bool downlink_ready   = false;
  bool uplink_ready     = false;
  bool downlink_visible = false;
  bool uplink_access_ready = false;
  bool access_roundtrip_ready = false;
  bool bidirectional_service_ready = false;
};

/// CU-CP decision that maps one analog access beam to an access DU.
struct ntn_analog_access_du_assignment {
  std::string analog_beam_id;
  du_index_t  selected_access_du_index = du_index_t::invalid;
  unsigned    supported_child_count    = 0;
  unsigned    assigned_child_count     = 0;
  std::string reason                   = "no_supported_child";
};

/// Inputs needed by CU-CP to compute a DU placement plan for NTN ground beams.
struct ntn_beam_placement_request {
  /// Static beam table loaded from configuration.
  std::vector<ntn_beam_position> beams;

  /// Static analog access beam groups loaded from configuration.
  std::vector<ntn_analog_beam_position> analog_beams;

  /// Beams currently visible from the serving satellite state.
  std::vector<ntn_served_beam_candidate> visible_beams;

  /// Maximum number of active_loaded beams in the CU-CP hopping window. A value of zero means unlimited.
  unsigned max_active_beams = 0;

  /// DU capacities and optional per-DU beam support constraints.
  std::vector<ntn_du_beam_capacity> du_capacities;

  /// Current beam load snapshot.
  std::vector<ntn_beam_load> beam_loads;

  /// Digital service beams CU-CP wants to preheat as empty active_loaded targets.
  std::vector<std::string> preheated_beam_ids;

  /// Use demand and QoS to weight CU-CP antenna/SR/SRS service intent for loaded beams.
  bool demand_aware_resource_weighting_enabled = false;

  /// CU-CP resource-domain policy defaults.
  ntn_resource_domain_policy resource_policy;

  /// Previous plan, used to preserve stable DU ownership and drain beams that leave visibility.
  std::vector<ntn_beam_du_assignment> previous_assignments;
};

/// Computed CU-CP placement plan for NTN ground beams.
struct ntn_beam_placement_plan {
  std::vector<ntn_beam_du_assignment> assignments;
  std::vector<ntn_analog_access_du_assignment> analog_assignments;
};

/// Extract active_loaded beam ids from a placement plan, preserving the order of the visible beam candidates.
std::vector<std::string>
get_active_loaded_ntn_beam_ids(const ntn_beam_placement_plan&                plan,
                               const std::vector<ntn_served_beam_candidate>& visible_beams);

/// Extract loaded service calendar beam ids, preserving visible-candidate order and excluding empty candidates.
std::vector<std::string>
get_loaded_service_calendar_ntn_beam_ids(const ntn_beam_placement_plan&                plan,
                                         const std::vector<ntn_served_beam_candidate>& visible_beams);

/// Extract mobility-eligible beam ids, preserving visible-candidate order and excluding draining/inactive beams.
std::vector<std::string>
get_mobility_eligible_ntn_beam_ids(const ntn_beam_placement_plan&                plan,
                                   const std::vector<ntn_served_beam_candidate>& visible_beams);

/// Computes stable, deterministic DU placement for NTN ground beams.
class ntn_beam_placement_planner
{
public:
  ntn_beam_placement_plan plan(const ntn_beam_placement_request& request) const;
};

} // namespace srs_cu_cp
} // namespace srsran
