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
#include "srsran/cu_cp/ntn_beam_service_resources.h"
#include "srsran/cu_cp/ntn_location.h"
#include "srsran/cu_cp/ntn_qos_policy.h"
#include "srsran/cu_cp/ntn_service_switch_over.h"
#include "srsran/cu_cp/ntn_ue_capability.h"
#include "srsran/ran/ntn.h"
#include "srsran/ran/pci.h"
#include "srsran/ran/rnti.h"
#include "srsran/support/async/async_task.h"
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace srsran {
namespace srs_cu_cp {

/// Runtime CU-CP state of one NTN beam assignment.
enum class cu_cp_ntn_beam_assignment_state {
  inactive,
  candidate,
  active_loaded,
  active = active_loaded, // Backward-compatible command alias. New CU-CP code should use active_loaded.
  draining
};

/// Public snapshot of one NTN beam placement decision.
struct cu_cp_ntn_beam_status {
  std::string beam_id;
  std::string analog_beam_id;
  std::string serving_satellite_id = "sat-0";
  std::optional<std::chrono::milliseconds> predictive_entry_offset;
  std::optional<std::chrono::milliseconds> predictive_exit_offset;
  nr_cell_identity nci      = nr_cell_identity::min();
  du_index_t       du_index = du_index_t::invalid;
  du_index_t       access_du_index       = du_index_t::invalid;
  du_index_t       service_du_index      = du_index_t::invalid;
  std::string      access_du_reason      = "no_supported_child";
  std::string      du_assignment_reason  = "no_supported_child";
  cu_cp_ntn_beam_assignment_state state = cu_cp_ntn_beam_assignment_state::inactive;
  double   elevation_deg = 0.0;
  unsigned nof_ues       = 0;
  unsigned nof_drbs      = 0;
  bool     in_hopping_window = false;
  bool     downlink_enabled  = true;
  bool     uplink_enabled    = true;
  bool     downlink_ready    = false;
  bool     uplink_ready      = false;
  bool     downlink_visible  = false;
  bool     uplink_access_ready = false;
  bool     access_roundtrip_ready = false;
  bool     paired_uplink_access_ready = false;
  std::string paired_uplink_beam_id;
  std::optional<nr_cell_identity> paired_uplink_nci;
  du_index_t paired_uplink_du_index = du_index_t::invalid;
  std::string access_pair_reason = "not_evaluated";
  bool     bidirectional_service_ready = false;
  unsigned scheduling_score  = 0;
  unsigned window_rank       = std::numeric_limits<unsigned>::max();
  std::string scheduling_reason = "legacy";
  unsigned antenna_slot_index = 0;
  unsigned nof_antenna_slots  = 0;
  unsigned antenna_slot_period = 0;
  unsigned sr_slot_offset      = 0;
  unsigned sr_slot_period      = 0;
  unsigned srs_slot_offset     = 0;
  unsigned srs_slot_period     = 0;
  unsigned resource_weight     = 0;
  double   resource_share      = 0.0;
  std::string resource_weight_reason = "none";
  bool        headroom_reserved      = false;
  std::string headroom_reason        = "none";
  ntn_qos_demand_summary qos;
  ntn_service_beam_policy service_policy = ntn_service_beam_policy::normal;
  bool                    new_demand_blocked = false;
  bool                    drain_forced = false;
  std::optional<tac_t>    derived_tac;
  std::string             derived_tac_reason           = "missing_decimal_suffix";
  bool                    paging_recommendable         = false;
  std::string             paging_recommendation_reason = "inactive";
  std::string             sib19_broadcast_state        = "stale_blocked";
  std::string             sib19_broadcast_reason       = "not_configured";
  uint32_t                sib19_broadcast_generation   = 0;
  unsigned                sib19_packed_bytes           = 0;
  uint32_t                sib19_packed_hash            = 0;
  bool                    analog_access_eligible       = false;
  std::string             analog_access_reason         = "not_configured";
  bool                    analog_edge_partial          = false;
  bool                    resource_domain_eligible     = true;
  std::string             resource_domain_reason       = "eligible";
  std::string             reuse_group_id;
  std::vector<std::string> conflict_group_ids;
  unsigned                analog_loaded_digital_child_cap  = 0;
  unsigned                analog_loaded_digital_child_load = 0;
  unsigned                analog_service_bound_ue_cap      = 0;
  unsigned                analog_service_bound_ue_load     = 0;
  unsigned                analog_drb_cap                   = 0;
  unsigned                analog_drb_load                  = 0;
  unsigned                digital_ue_cap                   = 0;
  unsigned                digital_ue_load                  = 0;
  unsigned                digital_drb_cap                  = 0;
  unsigned                digital_drb_load                 = 0;
  std::string             state_reason;
};

/// CU-CP-only analog access antenna intent. This is an observability contract, not DU/RF execution state.
struct cu_cp_ntn_analog_access_intent {
  std::string analog_beam_id;
  du_index_t  selected_du_index     = du_index_t::invalid;
  unsigned    nof_access_active_ues = 0;
  std::string reason                = "none";
};

/// CU-CP-only digital service antenna intent. This mirrors loaded-calendar/SR/SRS intent, not MAC scheduling.
struct cu_cp_ntn_digital_service_intent {
  std::string digital_beam_id;
  du_index_t  service_du_index    = du_index_t::invalid;
  unsigned    nof_ues             = 0;
  unsigned    nof_drbs            = 0;
  unsigned    nof_antenna_slots   = 0;
  unsigned    antenna_slot_index  = 0;
  unsigned    antenna_slot_period = 0;
  unsigned    sr_slot_offset      = 0;
  unsigned    sr_slot_period      = 0;
  unsigned    srs_slot_offset     = 0;
  unsigned    srs_slot_period     = 0;
  unsigned    resource_weight     = 0;
  double      resource_share      = 0.0;
  std::string resource_weight_reason = "none";
  ntn_qos_demand_summary qos;
  std::string reason = "none";
};

/// CU-CP-only NTN antenna intent snapshot split by analog access and digital service ownership.
struct cu_cp_ntn_antenna_intent_snapshot {
  std::vector<cu_cp_ntn_analog_access_intent>  analog_access_intents;
  std::vector<cu_cp_ntn_digital_service_intent> digital_service_intents;
};

/// Read-only projection of one long-lived onboard NR cell in the versioned position plan.
struct cu_cp_ntn_onboard_cell_plan_status {
  nr_cell_identity nci = nr_cell_identity::min();
  pci_t            pci = INVALID_PCI;
  unsigned         active_l1_positions       = 0;
  unsigned         pending_l1_positions      = 0;
  unsigned         capacity                  = 0;
  unsigned         analog_ports_used         = 0;
  unsigned         analog_port_capacity      = 0;
  unsigned         digital_planning_capacity = 0;
  std::string      digital_binding_state     = "not_bound_to_service_runtime";
  unsigned         mapped_l1_positions       = 0;
  std::string      runtime_plmn               = "none";
  tac_t            runtime_tac                = INVALID_TAC;
  std::string      runtime_tai_status         = "unavailable";
};

/// Static scheduler opportunity evidence for one onboard cell. This does not prove position, antenna or RF execution.
struct cu_cp_ntn_static_opportunity_status {
  bool        performed            = false;
  bool        passed               = false;
  unsigned    numerology           = 0xffU;
  unsigned    expected_ssb         = 0;
  unsigned    matched_ssb          = 0;
  unsigned    expected_prach       = 0;
  unsigned    matched_prach        = 0;
  unsigned    max_ssb_gap_slots    = 0;
  unsigned    max_prach_gap_slots  = 0;
  std::string first_unmatched      = "none";
};

/// Read-only versioned position-plan projection. Calendar values describe checked CU-CP intent, not RF execution.
struct cu_cp_ntn_position_plan_status {
  bool        enabled              = false;
  bool        du_execution_enabled = false;
  bool                                               signature_required                 = false;
  unsigned                                           trusted_signing_key_count          = 0;
  std::string                                        signature_status                   = "disabled";
  std::string                                        signing_key_id                     = "none";
  std::string                                        signing_key_fingerprint            = "none";
  bool                                               state_file_configured             = false;
  bool                                               state_file_required               = false;
  unsigned                                           state_schema_version              = 0;
  uint64_t                                           state_generation                  = 0;
  std::string                                        state_hash                        = "none";
  std::string                                        state_store_status                = "disabled";
  std::string                                        state_store_error                 = "none";
  bool                                               state_write_blocked               = false;
  int64_t                                            last_state_save_unix_ms           = -1;
  bool                                               version_anchor_configured         = false;
  std::string                                        version_anchor_mode               = "disabled";
  std::string                                        version_anchor_status             = "disabled";
  std::string                                        version_anchor_error              = "none";
  uint64_t                                           version_anchor_generation         = 0;
  std::string                                        version_anchor_hash               = "none";
  uint64_t                                           version_anchor_catalog_version    = 0;
  uint64_t                                           version_anchor_schedule_version   = 0;
  uint64_t                                           version_anchor_reserved_version   = 0;
  std::string                                        recovery_stage                    = "disabled";
  std::string                                        recovery_detail                   = "state_recovery_disabled";
  uint64_t                                           recovery_schedule_version         = 0;
  uint64_t                                           catalog_version_high_water        = 0;
  uint64_t                                           schedule_version_high_water       = 0;
  unsigned    schema_version       = 0;
  std::string planning_run_id      = "none";
  std::string access_profile_id    = "none";
  std::string access_profile_hash = "none";
  std::string identity_authority  = "none";
  std::string stage               = "disabled";
  std::string deployment_stage    = "disabled";
  std::string deployment_detail   = "external_execution_disabled";
  std::string execution_evidence  = "intent_only";
  std::string runtime_mapping_stage  = "disabled";
  std::string runtime_mapping_detail = "feature_disabled";
  uint64_t    runtime_mapping_schedule_version = 0;
  std::string runtime_mapping_calendar_hash     = "none";
  unsigned    runtime_mapped_l1_positions       = 0;
  std::string paging_state                      = "disabled";
  unsigned    valid_idle_paging_contexts        = 0;
  /// Compatibility summary retained for existing read-only command consumers.
  std::string initial_access_position_check     = "disabled";
  std::string initial_access_position_mode      = "disabled";
  std::string initial_access_position_source_state = "disabled";
  std::string initial_access_position_source_authority = "none";
  unsigned    initial_access_position_pending         = 0;
  unsigned    initial_access_position_active_contexts = 0;
  uint64_t    initial_access_position_accepted        = 0;
  uint64_t    initial_access_position_rejected        = 0;
  uint64_t    initial_access_position_audited         = 0;
  uint64_t    initial_access_position_expired         = 0;
  uint64_t    initial_access_position_replayed        = 0;
  std::string initial_access_position_last_reason     = "none";
  unsigned    clear_queue_depth   = 0;
  bool        clear_in_flight     = false;
  uint64_t    clear_queue_head_schedule_version = 0;
  std::string clear_queue_head_calendar_hash     = "none";
  std::string clear_queue_head_reason            = "none";
  std::string satellite_id        = "none";
  uint64_t    active_catalog_version   = 0;
  uint64_t    active_schedule_version  = 0;
  std::string active_content_hash      = "none";
  std::string active_calendar_hash     = "none";
  int64_t     active_activation_epoch_unix_ms = -1;
  uint64_t    pending_catalog_version  = 0;
  uint64_t    pending_schedule_version = 0;
  std::string pending_content_hash     = "none";
  std::string pending_calendar_hash    = "none";
  int64_t     pending_activation_epoch_unix_ms = -1;
  bool        received_plan_present     = false;
  uint64_t    received_catalog_version  = 0;
  uint64_t    received_schedule_version = 0;
  std::string received_content_hash     = "none";
  int64_t     received_activation_epoch_unix_ms = -1;
  unsigned    candidate_l1_positions   = 0;
  unsigned    assigned_l1_positions    = 0;
  uint64_t    audited_schedule_version = 0;
  unsigned    calendar_intents         = 0;
  unsigned    ssb_intents              = 0;
  unsigned    prach_ro_intents         = 0;
  unsigned    prach_ul_beam_intents    = 0;
  unsigned    max_ssb_interval_ms      = 0;
  unsigned    max_prach_interval_ms    = 0;
  unsigned    prach_ro_without_beam    = 0;
  unsigned    resource_conflicts       = 0;
  uint64_t    static_preflight_schedule_version = 0;
  std::array<cu_cp_ntn_static_opportunity_status, 2> static_opportunities{};
  std::string last_rejection           = "none";
  uint64_t    last_rejected_schedule_version = 0;
  std::array<cu_cp_ntn_onboard_cell_plan_status, 2> cells{};
};

struct cu_cp_ntn_ue_slot_audit_du_status {
  du_index_t  du_index = du_index_t::invalid;
  std::string capability = "unknown";
  std::string stage = "awaiting_capability";
  bool        snapshot_complete = false;
  unsigned    matched = 0;
  unsigned    missing = 0;
  unsigned    conflict = 0;
  unsigned    quarantined = 0;
  unsigned    repaired = 0;
  uint32_t    assignment_generation_high_water = 0;
  std::string last_reason = "none";
};

/// Public CU-CP NTN runtime summary for observability commands.
struct cu_cp_ntn_runtime_status {
  bool enabled                   = false;
  cu_cp_ntn_position_plan_status onboard_position_plan;
  bool satellite_state_available = false;
  bool assistance_valid          = false;
  ntn_assistance_invalid_reason assistance_invalid_reason = ntn_assistance_invalid_reason::no_satellite_state;
  unsigned nof_total_beams             = 0;
  unsigned nof_candidate_beams         = 0;
  unsigned nof_active_loaded_beams     = 0;
  unsigned nof_loaded_service_beams    = 0;
  unsigned nof_draining_beams          = 0;
  unsigned nof_inactive_beams          = 0;
  unsigned nof_mobility_eligible_beams = 0;
  unsigned nof_downlink_ready_beams    = 0;
  unsigned nof_uplink_ready_beams      = 0;
  unsigned nof_downlink_visible_beams  = 0;
  unsigned nof_uplink_access_ready_beams = 0;
  unsigned nof_access_roundtrip_ready_beams = 0;
  unsigned nof_paired_uplink_access_ready_beams = 0;
  unsigned nof_downlink_only_without_ul_pair_beams = 0;
  unsigned nof_bidirectional_service_ready_beams = 0;
  unsigned nof_ues_with_ntn_context    = 0;
  unsigned nof_active_switch_over_events = 0;
  unsigned nof_qos_prioritized_loaded_beams = 0;
  unsigned nof_valid_service_area_beams     = 0;
  unsigned nof_invalid_service_area_beams   = 0;
  unsigned nof_paging_recommendable_beams   = 0;
  unsigned nof_total_analog_access_beams     = 0;
  unsigned nof_full_analog_access_beams      = 0;
  unsigned nof_partial_analog_access_beams   = 0;
  unsigned nof_active_analog_access_beams    = 0;
  unsigned nof_loaded_digital_service_beams  = 0;
  unsigned nof_access_du_assigned_analog_beams   = 0;
  unsigned nof_access_du_unassigned_analog_beams = 0;
  unsigned nof_same_du_service_beams             = 0;
  unsigned nof_split_du_service_beams            = 0;
  unsigned nof_pre_service_relocations_pending   = 0;
  unsigned nof_pre_service_relocations_active    = 0;
  unsigned nof_pre_service_relocations_blocked   = 0;
  unsigned nof_connected_handovers_candidate     = 0;
  unsigned nof_connected_handovers_preloaded     = 0;
  unsigned nof_connected_handovers_resource_preparing = 0;
  unsigned nof_connected_handovers_resource_applied   = 0;
  unsigned nof_connected_handovers_blocked       = 0;
  unsigned nof_connected_handovers_active        = 0;
  unsigned nof_connected_handovers_rollback      = 0;
  unsigned nof_ntn_access_only_ues                = 0;
  unsigned nof_ntn_service_binding_pending_ues    = 0;
  unsigned nof_ntn_service_binding_blocked_ues    = 0;
  unsigned nof_ntn_service_bound_ues              = 0;
  unsigned nof_ntn_access_active_ues              = 0;
  unsigned nof_ntn_control_only_ues               = 0;
  unsigned nof_ntn_analog_released_ues            = 0;
  unsigned nof_ntn_digital_service_bound_ues      = 0;
  unsigned nof_ntn_location_bound_service_ues     = 0;
  unsigned nof_ntn_access_cell_fallback_service_ues = 0;
  unsigned nof_ntn_beam_hopping_ues_requested = 0;
  unsigned nof_ntn_beam_hopping_ues_scheduled = 0;
  unsigned nof_ntn_beam_hopping_ues_skipped   = 0;
  bool     predictive_window_valid            = false;
  bool     multi_satellite_window_valid       = false;
  unsigned nof_ntn_current_window_satellites  = 0;
  unsigned nof_ntn_next_window_satellites     = 0;
  unsigned nof_ntn_multi_satellite_visible_beams = 0;
  unsigned nof_ntn_satellite_owner_changes    = 0;
  std::chrono::milliseconds ntn_predictive_service_window_horizon{0};
  std::chrono::milliseconds ntn_predictive_handover_lead_time{0};
  unsigned nof_ntn_predictive_timeline_steps       = 0;
  unsigned nof_ntn_predictive_timeline_entry_beams = 0;
  unsigned nof_ntn_predictive_timeline_exit_beams  = 0;
  std::optional<std::chrono::milliseconds> earliest_ntn_predictive_upcoming_offset;
  std::optional<std::chrono::milliseconds> earliest_ntn_predictive_drain_offset;
  unsigned nof_ntn_predictive_upcoming_beams  = 0;
  unsigned nof_ntn_predictive_drain_soon_beams = 0;
  unsigned nof_ntn_predictive_beam_hopping_ues_requested = 0;
  unsigned nof_ntn_predictive_beam_hopping_ues_scheduled = 0;
  unsigned nof_ntn_predictive_beam_hopping_ues_skipped   = 0;
  unsigned nof_ntn_handover_preferred_ues_requested = 0;
  unsigned nof_ntn_handover_preferred_ues_scheduled = 0;
  unsigned nof_ntn_handover_preferred_ues_skipped   = 0;
  unsigned nof_ntn_load_balancing_evaluations       = 0;
  unsigned nof_ntn_load_balancing_admission_steered = 0;
  unsigned nof_ntn_load_balancing_handover_requested = 0;
  unsigned nof_ntn_load_balancing_handover_scheduled = 0;
  unsigned nof_ntn_load_balancing_handover_skipped   = 0;
  unsigned nof_ntn_load_balancing_same_analog_scheduled = 0;
  unsigned nof_ntn_load_balancing_cross_analog_scheduled = 0;
  unsigned nof_ntn_load_balancing_skipped_projected_capacity = 0;
  unsigned nof_ntn_load_balancing_skipped_cold_analog = 0;
  std::string last_ntn_load_balancing_reason         = "none";
  std::string last_ntn_load_balancing_source_beam_id = "none";
  std::string last_ntn_load_balancing_target_beam_id = "none";
  std::string last_ntn_load_balancing_source_analog_id = "none";
  std::string last_ntn_load_balancing_target_analog_id = "none";
  unsigned nof_ntn_service_pair_handover_targets     = 0;
  unsigned nof_ntn_service_pair_handover_scheduled   = 0;
  unsigned nof_ntn_service_pair_handover_skipped     = 0;
  unsigned nof_ntn_service_pair_handover_committed   = 0;
  unsigned nof_ntn_service_pair_handover_rolled_back = 0;
  unsigned nof_ntn_service_pair_handover_context_cleared = 0;
  std::string last_ntn_service_pair_handover_reason  = "none";
  std::string last_ntn_service_pair_handover_completion_reason = "none";
  unsigned nof_ntn_preheat_requested                 = 0;
  unsigned nof_ntn_preheat_sent                      = 0;
  unsigned nof_ntn_preheat_applied                   = 0;
  unsigned nof_ntn_preheat_skipped                   = 0;
  unsigned nof_ntn_preheat_demoted                   = 0;
  unsigned nof_ntn_preheat_skipped_by_capacity       = 0;
  unsigned nof_ntn_preheat_skipped_by_policy         = 0;
  unsigned nof_ntn_cold_analog_preheat_requested     = 0;
  std::string last_ntn_preheat_reason                = "none";
  std::string last_ntn_preheat_source_analog_id      = "none";
  std::string last_ntn_preheat_target_analog_id      = "none";
  unsigned nof_ntn_target_reservation_created        = 0;
  unsigned nof_ntn_target_reservation_held           = 0;
  unsigned nof_ntn_target_reservation_consumed       = 0;
  unsigned nof_ntn_target_reservation_expired        = 0;
  unsigned nof_ntn_admission_blocked_by_target_reservation = 0;
  unsigned nof_ntn_handover_skipped_by_pair_cooldown = 0;
  unsigned nof_ntn_handover_skipped_by_preheat_ready_guard = 0;
  unsigned nof_ntn_preheat_demote_deferred_by_reservation = 0;
  std::string last_ntn_scheduling_guard_reason       = "none";
  std::string last_ntn_scheduling_guard_source_beam_id = "none";
  std::string last_ntn_scheduling_guard_target_beam_id = "none";
  std::string last_ntn_scheduling_guard_source_analog_id = "none";
  std::string last_ntn_scheduling_guard_target_analog_id = "none";
  unsigned nof_ntn_beam_scheduling_evaluations = 0;
  unsigned nof_ntn_beam_scheduling_demand_prioritized_windows = 0;
  unsigned nof_ntn_beam_scheduling_legacy_fallback = 0;
  unsigned nof_ntn_beam_scheduling_sticky_kept = 0;
  std::string last_ntn_beam_scheduling_reason = "none";
  unsigned nof_ntn_resource_weighting_evaluations = 0;
  unsigned nof_ntn_resource_weighting_weighted_beams = 0;
  unsigned nof_ntn_resource_weighting_qos_boosted_beams = 0;
  unsigned nof_ntn_resource_weighting_legacy_fallback = 0;
  std::string last_ntn_resource_weighting_reason = "none";
  unsigned nof_ntn_headroom_evaluations        = 0;
  unsigned nof_ntn_headroom_reserved_beams     = 0;
  unsigned nof_ntn_headroom_admission_allowed  = 0;
  unsigned nof_ntn_headroom_admission_blocked  = 0;
  unsigned nof_ntn_headroom_handover_protected = 0;
  std::string last_ntn_headroom_reason         = "none";
  unsigned nof_ntn_release_allowed_ues_requested   = 0;
  unsigned nof_ntn_release_allowed_ues_scheduled   = 0;
  unsigned nof_ntn_release_allowed_ues_skipped     = 0;
  unsigned nof_ntn_rrc_location_reports_received   = 0;
  unsigned nof_ntn_rrc_location_reports_decoded    = 0;
  unsigned nof_ntn_rrc_location_reports_unsupported = 0;
  unsigned nof_ntn_rrc_location_reports_decode_failed = 0;
  unsigned nof_ntn_location_reports_accepted       = 0;
  unsigned nof_ntn_location_reports_rejected       = 0;
  unsigned nof_ntn_location_fresh_ues              = 0;
  unsigned nof_ntn_location_missing_ues            = 0;
  unsigned nof_ntn_location_stale_ues              = 0;
  unsigned nof_ntn_location_release_pending_ues    = 0;
  unsigned nof_ntn_location_watchdog_evaluations   = 0;
  unsigned nof_ntn_location_watchdog_refresh_requested = 0;
  unsigned nof_ntn_location_watchdog_release_requested = 0;
  unsigned nof_ntn_location_watchdog_release_scheduled = 0;
  unsigned nof_ntn_location_watchdog_release_skipped   = 0;
  std::string last_ntn_location_watchdog_release_reason = "none";
  unsigned nof_ntn_rrc_location_request_desired_ues = 0;
  unsigned nof_ntn_rrc_location_request_configured_ues = 0;
  unsigned nof_ntn_rrc_location_request_pending_ues = 0;
  unsigned nof_ntn_rrc_location_request_configs_included = 0;
  unsigned nof_ntn_rrc_location_request_configs_removed = 0;
  unsigned nof_ntn_rrc_location_request_configs_skipped_capability = 0;
  unsigned nof_ntn_rrc_location_request_configs_skipped_state = 0;
  unsigned nof_ntn_rrc_location_request_reconfig_sent = 0;
  unsigned nof_ntn_rrc_location_request_reconfig_failed = 0;
  unsigned nof_ntn_nrppa_dl_ue_received                = 0;
  unsigned nof_ntn_nrppa_dl_ue_forwarded               = 0;
  unsigned nof_ntn_nrppa_dl_ue_dropped                 = 0;
  unsigned nof_ntn_nrppa_dl_non_ue_received            = 0;
  unsigned nof_ntn_nrppa_dl_non_ue_forwarded           = 0;
  unsigned nof_ntn_nrppa_dl_non_ue_dropped             = 0;
  unsigned nof_ntn_nrppa_ul_ue_received                = 0;
  unsigned nof_ntn_nrppa_ul_ue_sent                    = 0;
  unsigned nof_ntn_nrppa_ul_ue_dropped                 = 0;
  unsigned nof_ntn_nrppa_ul_non_ue_received            = 0;
  unsigned nof_ntn_nrppa_ul_non_ue_sent                = 0;
  unsigned nof_ntn_nrppa_ul_non_ue_dropped             = 0;
  std::string last_ntn_nrppa_dropped_reason            = "none";
  unsigned nof_ntn_nrppa_trp_requests_received         = 0;
  unsigned nof_ntn_nrppa_trp_requests_decoded          = 0;
  unsigned nof_ntn_nrppa_trp_responses_sent            = 0;
  unsigned nof_ntn_nrppa_trp_failures_sent             = 0;
  unsigned nof_ntn_nrppa_unsupported_procedures        = 0;
  unsigned nof_ntn_nrppa_trp_unsupported_info_items    = 0;
  unsigned nof_ntn_nrppa_trp_empty_results             = 0;
  std::string last_ntn_nrppa_trp_reason                = "none";
  unsigned nof_ntn_nrppa_standard_decode_success       = 0;
  unsigned nof_ntn_nrppa_standard_decode_failure       = 0;
  unsigned nof_ntn_nrppa_standard_encode_responses     = 0;
  unsigned nof_ntn_nrppa_standard_encode_failures      = 0;
  unsigned nof_ntn_nrppa_minimal_fallback_decodes      = 0;
  std::string last_ntn_nrppa_standard_decode_reason    = "none";
  unsigned nof_ntn_nrppa_positioning_info_requests_received  = 0;
  unsigned nof_ntn_nrppa_positioning_info_requests_decoded   = 0;
  unsigned nof_ntn_nrppa_positioning_info_requests_forwarded = 0;
  unsigned nof_ntn_nrppa_positioning_info_responses_sent     = 0;
  unsigned nof_ntn_nrppa_positioning_info_failures_sent      = 0;
  unsigned nof_ntn_nrppa_positioning_info_dropped            = 0;
  std::string last_ntn_nrppa_positioning_info_reason         = "none";
  unsigned nof_ntn_nrppa_measurement_requests_received       = 0;
  unsigned nof_ntn_nrppa_measurement_requests_decoded        = 0;
  unsigned nof_ntn_nrppa_measurement_requests_forwarded      = 0;
  unsigned nof_ntn_nrppa_measurement_responses_sent          = 0;
  unsigned nof_ntn_nrppa_measurement_failures_sent           = 0;
  unsigned nof_ntn_nrppa_measurement_dropped                 = 0;
  std::string last_ntn_nrppa_measurement_reason              = "none";
  unsigned nof_ntn_nrppa_activation_requests_received        = 0;
  unsigned nof_ntn_nrppa_activation_requests_decoded         = 0;
  unsigned nof_ntn_nrppa_activation_requests_forwarded       = 0;
  unsigned nof_ntn_nrppa_activation_responses_sent           = 0;
  unsigned nof_ntn_nrppa_activation_failures_sent            = 0;
  unsigned nof_ntn_nrppa_activation_dropped                  = 0;
  std::string last_ntn_nrppa_activation_reason               = "none";
  unsigned nof_ntn_nrppa_deactivation_requests_received      = 0;
  unsigned nof_ntn_nrppa_deactivation_requests_decoded       = 0;
  unsigned nof_ntn_nrppa_deactivation_requests_forwarded     = 0;
  unsigned nof_ntn_nrppa_deactivation_acks_sent              = 0;
  unsigned nof_ntn_nrppa_deactivation_failures_sent          = 0;
  unsigned nof_ntn_nrppa_deactivation_dropped                = 0;
  std::string last_ntn_nrppa_deactivation_reason             = "none";
  unsigned nof_ntn_nrppa_assistance_control_requests_received = 0;
  unsigned nof_ntn_nrppa_assistance_control_requests_decoded = 0;
  unsigned nof_ntn_nrppa_assistance_control_requests_forwarded = 0;
  unsigned nof_ntn_nrppa_assistance_control_feedbacks_sent = 0;
  unsigned nof_ntn_nrppa_assistance_control_failures_sent = 0;
  unsigned nof_ntn_nrppa_assistance_control_dropped = 0;
  unsigned nof_ntn_nrppa_assistance_control_unsupported_fields = 0;
  std::string last_ntn_nrppa_assistance_control_reason = "none";
  unsigned nof_ntn_idle_paging_contexts                = 0;
  unsigned nof_ntn_idle_paging_contexts_with_5g_s_tmsi = 0;
  unsigned nof_ntn_idle_paging_contexts_expired        = 0;
  unsigned nof_ntn_idle_paging_ue_hits                 = 0;
  unsigned nof_ntn_idle_paging_tac_fallbacks           = 0;
  unsigned nof_ntn_idle_paging_recommendations         = 0;
  unsigned nof_ntn_idle_paging_skipped                 = 0;
  std::string last_ntn_idle_paging_reason              = "none";
  unsigned nof_ntn_paired_access_contexts              = 0;
  unsigned nof_ntn_paired_access_responses             = 0;
  unsigned nof_ntn_service_bindings_from_paired_access = 0;
  unsigned nof_ntn_paired_access_blocked               = 0;
  std::string last_ntn_paired_access_reason            = "none";
  unsigned nof_ntn_service_pair_bound_ues              = 0;
  unsigned nof_ntn_service_pair_blocked_ues            = 0;
  std::string last_ntn_service_pair_reason             = "none";
  unsigned nof_ntn_inactive_contexts                   = 0;
  unsigned nof_ntn_inactive_contexts_expired           = 0;
  unsigned nof_ntn_inactive_suspend_requested          = 0;
  unsigned nof_ntn_inactive_suspend_succeeded          = 0;
  unsigned nof_ntn_inactive_suspend_failed             = 0;
  unsigned nof_ntn_inactive_resume_requested           = 0;
  unsigned nof_ntn_inactive_resume_succeeded           = 0;
  unsigned nof_ntn_inactive_resume_failed              = 0;
  unsigned nof_ntn_inactive_ngap_suspend_responses     = 0;
  unsigned nof_ntn_inactive_ngap_suspend_failures      = 0;
  unsigned nof_ntn_inactive_ngap_resume_responses      = 0;
  unsigned nof_ntn_inactive_ngap_resume_failures       = 0;
  unsigned nof_ntn_inactive_paging_hits                = 0;
  unsigned nof_ntn_inactive_fallback_releases          = 0;
  std::string last_ntn_inactive_reason                 = "none";
  unsigned nof_resource_domain_analog_cap_blocked  = 0;
  unsigned nof_resource_domain_digital_cap_blocked = 0;
  unsigned nof_resource_domain_conflict_blocked    = 0;
  unsigned nof_active_reuse_groups                 = 0;
  unsigned nof_ntn_access_rnti_owned               = 0;
  unsigned nof_ntn_access_rnti_conflicts           = 0;
  unsigned nof_ntn_rnti_leases_reserved            = 0;
  unsigned nof_ntn_rnti_leases_available           = 0;
  unsigned nof_ntn_rnti_leases_sent_to_du          = 0;
  unsigned nof_ntn_rnti_leases_applied_by_du       = 0;
  unsigned nof_ntn_rnti_leases_rejected_by_du      = 0;
  unsigned nof_ntn_rnti_leases_offered_in_rar      = 0;
  unsigned nof_ntn_rnti_leases_consumed_by_du      = 0;
  unsigned nof_ntn_rnti_leases_initial_ul_seen     = 0;
  unsigned nof_ntn_rnti_leases_committed           = 0;
  unsigned nof_ntn_rnti_leases_released            = 0;
  unsigned nof_ntn_rnti_leases_expired             = 0;
  unsigned nof_ntn_rnti_leases_conflict            = 0;
  unsigned nof_ntn_digital_slot_intents            = 0;
  unsigned nof_ntn_digital_slot_cleared_intents    = 0;
  unsigned nof_ntn_digital_slot_sent_to_du         = 0;
  unsigned nof_ntn_digital_slot_applied_by_du      = 0;
  unsigned nof_ntn_digital_slot_rejected_by_du     = 0;
  unsigned nof_ntn_digital_slot_cleared_by_du      = 0;
  unsigned nof_ntn_digital_slot_rollback           = 0;
  uint32_t ntn_resource_audit_generation            = 0;
  unsigned nof_ntn_resource_audit_queries_sent      = 0;
  unsigned nof_ntn_resource_audit_responses_accepted = 0;
  unsigned nof_ntn_resource_audit_mismatches        = 0;
  unsigned nof_ntn_resource_audit_repair_actions    = 0;
  unsigned nof_ntn_resource_audit_failures          = 0;
  unsigned nof_ntn_resource_audit_rnti_incomplete   = 0;
  unsigned nof_ntn_resource_audit_ue_slot_incomplete = 0;
  std::string last_ntn_resource_audit_reason        = "none";
  std::string ue_slot_audit_stage                    = "awaiting_capability";
  unsigned    nof_ue_slot_audit_matched              = 0;
  unsigned    nof_ue_slot_audit_missing              = 0;
  unsigned    nof_ue_slot_audit_conflict             = 0;
  unsigned    nof_ue_slot_audit_quarantined          = 0;
  unsigned    nof_ue_slot_audit_repaired             = 0;
  uint32_t    ue_slot_assignment_generation_high_water = 0;
  std::string last_ue_slot_audit_reason              = "none";
  std::vector<cu_cp_ntn_ue_slot_audit_du_status> ue_slot_audit_du_statuses;
  std::string                              ntn_rnti_retirement_capability                          = "unknown";
  uint32_t                                 ntn_rnti_generation_high_water                          = 0;
  unsigned                                 nof_ntn_rnti_retire_pending                             = 0;
  unsigned                                 nof_ntn_rnti_retire_sent                                = 0;
  unsigned                                 nof_ntn_rnti_quarantined                                = 0;
  uint64_t                                 nof_ntn_rnti_retired                                    = 0;
  uint64_t                                 nof_ntn_rnti_reused                                     = 0;
  uint64_t                                 nof_ntn_rnti_retire_rejected                            = 0;
  unsigned                                 nof_ntn_rnti_retire_unsupported                         = 0;
  unsigned                                 nof_ntn_rnti_namespace_exhausted                        = 0;
  unsigned                                 nof_ntn_rnti_generation_exhausted                       = 0;
  std::string                              last_ntn_rnti_retirement_reason                         = "none";
  unsigned nof_ntn_service_pair_resource_audit_targets    = 0;
  unsigned nof_ntn_service_pair_resource_audit_mismatches = 0;
  unsigned nof_ntn_service_pair_resource_audit_repairs    = 0;
  unsigned nof_ntn_service_pair_resource_audit_skipped    = 0;
  std::string last_ntn_service_pair_resource_audit_reason = "none";
  unsigned nof_ntn_resource_repairs_queued          = 0;
  unsigned nof_ntn_resource_repairs_sent            = 0;
  unsigned nof_ntn_resource_repairs_applied         = 0;
  unsigned nof_ntn_resource_repairs_failed          = 0;
  unsigned nof_ntn_resource_repairs_retry_exhausted = 0;
  unsigned nof_ntn_resource_repairs_blocked_conflict = 0;
  unsigned nof_sib19_broadcast_desired              = 0;
  unsigned nof_sib19_broadcast_sent_to_du           = 0;
  unsigned nof_sib19_broadcast_applied_by_du        = 0;
  unsigned nof_sib19_broadcast_rejected_by_du       = 0;
  unsigned nof_sib19_broadcast_clear_sent           = 0;
  unsigned nof_sib19_broadcast_cleared_by_du        = 0;
  unsigned nof_sib19_broadcast_stale_blocked        = 0;
  unsigned nof_ntn_capability_unknown_ues            = 0;
  unsigned nof_ntn_capability_supported_ues          = 0;
  unsigned nof_ntn_capability_unsupported_ues        = 0;
  unsigned nof_ntn_capability_parse_failed_ues       = 0;
  unsigned nof_ntn_capability_ngso_ues               = 0;
  unsigned nof_ntn_capability_gso_ues                = 0;
  unsigned nof_ntn_capability_both_ues               = 0;
  unsigned nof_ntn_capability_implicit_both_ues      = 0;
  unsigned nof_ntn_capability_profile_blocked_ues    = 0;
  bool automatic_source_updates_allowed = true;
};

enum class ntn_repair_mode { dry_run, apply };

enum class ntn_repair_scope { resources, sib19, all };

struct ntn_repair_command {
  ntn_repair_mode  mode  = ntn_repair_mode::dry_run;
  ntn_repair_scope scope = ntn_repair_scope::all;
  unsigned         limit = 16;
};

struct ntn_repair_response {
  bool        accepted          = false;
  std::string reason            = "none";
  std::string next_step         = "none";
  unsigned    audit_targets     = 0;
  unsigned    sib19_candidates  = 0;
  unsigned    existing_failed   = 0;
  unsigned    existing_blockers = 0;
};

/// Public CU-CP NTN runtime snapshot for one UE.
struct cu_cp_ntn_ue_status {
  ue_index_t ue_index    = ue_index_t::invalid;
  du_index_t du_index    = du_index_t::invalid;
  rnti_t     rnti        = rnti_t::INVALID_RNTI;
  unsigned   nof_drbs    = 0;
  ntn_qos_demand_summary qos;
  std::optional<nr_cell_identity> serving_nci;
  std::optional<std::string>      serving_beam_id;
  std::optional<ntn_ue_location_report> last_location;
  bool                            has_ul_slot_request = false;
  std::optional<std::string>      candidate_beam_id;
  std::optional<nr_cell_identity> candidate_nci;
  bool                            candidate_handover_triggered = false;
  uint64_t                        accepted_handover_attempt_id = 0;
  unsigned                        active_core_location_requests = 0;
  bool                            ntn_rrc_location_request_desired = false;
  bool                            ntn_rrc_location_request_configured = false;
  bool                            ntn_rrc_location_request_pending = false;
  std::string                     ntn_rrc_location_request_skipped_reason = "none";
  std::string                     ntn_location_freshness_state = "not_required";
  std::string                     ntn_location_freshness_reason = "not_required";
  bool                            ntn_location_release_pending = false;
  std::optional<std::chrono::milliseconds> ntn_location_age;
  std::string                     pre_service_relocation_state  = "none";
  std::string                     pre_service_relocation_reason = "none";
  du_index_t                      pre_service_relocation_target_du_index = du_index_t::invalid;
  std::optional<std::string>      pre_service_relocation_target_beam_id;
  std::optional<nr_cell_identity> pre_service_relocation_target_nci;
  uint64_t                        pre_service_relocation_attempt_id = 0;
  unsigned                        pre_service_relocation_retry_count = 0;
  std::string                     connected_handover_state  = "none";
  std::string                     connected_handover_reason = "none";
  std::optional<std::string>      connected_handover_source_beam_id;
  std::optional<std::string>      connected_handover_target_beam_id;
  std::optional<std::string>      connected_handover_source_analog_beam_id;
  std::optional<std::string>      connected_handover_target_analog_beam_id;
  std::optional<nr_cell_identity> connected_handover_target_nci;
  du_index_t                      connected_handover_target_du_index = du_index_t::invalid;
  rnti_t                          connected_handover_target_c_rnti   = rnti_t::INVALID_RNTI;
  std::optional<std::string>      connected_handover_target_uplink_resource_beam_id;
  std::optional<nr_cell_identity> connected_handover_target_uplink_resource_nci;
  du_index_t                      connected_handover_target_uplink_resource_du_index = du_index_t::invalid;
  std::string                     connected_handover_target_service_pair_reason = "none";
  std::string                     connected_handover_target_resource_state = "none";
  bool                            connected_handover_target_sr_srs_applied = false;
  uint64_t                        connected_handover_attempt_id = 0;
  unsigned                        connected_handover_retry_count = 0;
  std::string                     ntn_runtime_state = "none";
  bool                            analog_access_released = false;
  std::string                     access_layer_state  = "none";
  std::string                     access_layer_reason = "none";
  std::optional<std::string>      access_analog_beam_id;
  du_index_t                      access_du_index = du_index_t::invalid;
  std::optional<nr_cell_identity> access_nci;
  std::optional<std::string>      last_downlink_wake_beam_id;
  std::optional<std::string>      paired_uplink_access_beam_id;
  std::optional<nr_cell_identity> paired_uplink_access_nci;
  du_index_t                      paired_uplink_access_du_index = du_index_t::invalid;
  std::string                     paired_access_reason = "none";
  std::string                     service_layer_state  = "none";
  std::string                     service_layer_reason = "none";
  std::optional<std::string>      service_digital_beam_id;
  du_index_t                      service_du_index = du_index_t::invalid;
  std::optional<nr_cell_identity> service_nci;
  std::optional<std::string>      service_downlink_beam_id;
  std::optional<std::string>      service_uplink_resource_beam_id;
  std::optional<nr_cell_identity> service_uplink_resource_nci;
  du_index_t                      service_uplink_resource_du_index = du_index_t::invalid;
  std::string                     service_pair_reason = "none";
  std::string                     service_binding_source = "none";
  std::string                     access_rnti_ownership_state  = "none";
  std::string                     access_rnti_ownership_reason = "none";
  std::string                     digital_slot_intent_state    = "none";
  std::string                     digital_slot_intent_reason   = "none";
  ntn_ue_capability_state         ntn_capability_state         = ntn_ue_capability_state::unknown;
  std::string                     ntn_capability_reason        = "no_capabilities";
  bool                            ntn_capability_parsed        = false;
  bool                            ntn_capability_has_nr_container = false;
  bool                            ntn_capability_non_terrestrial_network_r17 = false;
  bool                            ntn_capability_scenario_support_r17 = false;
  std::string                     ntn_capability_scenario = "none";
  bool                            ntn_capability_parameters_r17 = false;
  ntn_ue_capability_scenario_support ntn_capability_scenario_support = ntn_ue_capability_scenario_support::absent;
  std::string                        ntn_capability_deployment_profile = "leo_ngso";
  bool                               ntn_capability_profile_match = false;
  std::string                        ntn_capability_profile_reason = "no_capabilities";
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
  virtual bool handle_ntn_satellite_state_update(
      const ecef_coordinates_t&           satellite,
      std::optional<ecef_coordinates_t> next_satellite = std::nullopt) = 0;

  /// Update CU-CP with propagated current and next NTN satellite positions for a circular-orbit constellation.
  virtual bool handle_ntn_satellite_state_update(const std::vector<ntn_satellite_state>& current_satellites,
                                                 const std::vector<ntn_satellite_state>& next_satellites)
  {
    if (current_satellites.empty()) {
      return false;
    }
    std::optional<ecef_coordinates_t> next_satellite;
    if (!next_satellites.empty()) {
      next_satellite = next_satellites.front().ecef;
    }
    return handle_ntn_satellite_state_update(current_satellites.front().ecef, next_satellite);
  }

  /// Update CU-CP with propagated current and future NTN satellite positions for predictive service windows.
  virtual bool handle_ntn_satellite_state_update(
      const std::vector<ntn_satellite_state>&              current_satellites,
      const std::vector<ntn_satellite_prediction_step>& future_steps)
  {
    std::vector<ntn_satellite_state> next_satellites;
    if (!future_steps.empty()) {
      next_satellites = future_steps.front().satellites;
    }
    return handle_ntn_satellite_state_update(current_satellites, next_satellites);
  }

  /// Get the last accepted NTN mobility-eligible beam identifiers selected from satellite state updates.
  virtual std::vector<std::string> get_current_ntn_served_beam_ids() const = 0;

  /// Get the current NTN beam placement plan snapshot, including inactive, candidate, active_loaded and draining beams.
  virtual std::vector<cu_cp_ntn_beam_status> get_current_ntn_beam_status() const = 0;

  /// Get the current CU-CP-side NTN assistance snapshot for RRC/SIB19 contracts.
  virtual ntn_assistance_snapshot get_current_ntn_assistance_snapshot() const = 0;

  /// Get the current CU-CP-side SIB19 assistance packaging contract.
  virtual ntn_sib19_assistance_snapshot get_current_ntn_sib19_assistance_snapshot() const = 0;

  /// Get the current CU-CP-side NTN runtime summary.
  virtual cu_cp_ntn_runtime_status get_current_ntn_runtime_status() const = 0;

  /// Get current UE-level CU-CP NTN runtime snapshots.
  virtual std::vector<cu_cp_ntn_ue_status> get_current_ntn_ue_status() const = 0;

  /// Get the current CU-CP-side analog access / digital service antenna intent snapshot.
  virtual cu_cp_ntn_antenna_intent_snapshot get_current_ntn_antenna_intent_snapshot() const = 0;

  /// Get the current CU-CP-side NTN beam service resource-manager snapshot.
  virtual ntn_beam_service_resource_snapshot get_current_ntn_beam_service_resource_snapshot() const = 0;

  /// Apply a CU-CP-only NTN service switch-over event.
  virtual bool handle_ntn_service_switch_over_event(const ntn_service_switch_over_event& event) = 0;

  /// Clear an active CU-CP-only NTN service switch-over event by id.
  virtual bool clear_ntn_service_switch_over_event(uint64_t event_id) = 0;

  /// Apply a CU-CP-only manual override for NTN service source state.
  virtual bool handle_ntn_manual_override(const ntn_manual_override_command& command) = 0;

  /// Run a controlled operator-requested NTN repair action or dry-run preview.
  virtual ntn_repair_response handle_ntn_repair_command(const ntn_repair_command& command) = 0;

  /// Get the current CU-CP-side NTN service switch-over snapshot.
  virtual ntn_service_switch_over_snapshot get_current_ntn_service_switch_over_snapshot() const = 0;
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
