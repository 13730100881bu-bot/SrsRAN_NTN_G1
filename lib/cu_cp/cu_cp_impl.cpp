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
#include "ntn_mobility/ntn_qos_policy.h"
#include "srsran/cu_cp/cu_cp_types.h"
#include "srsran/f1ap/cu_cp/f1ap_cu.h"
#include "srsran/f1ap/ntn_rnti_lease_pool.h"
#include "srsran/f1ap/ntn_ul_slot_resource_request.h"
#include "srsran/nrppa/nrppa.h"
#include "srsran/nrppa/nrppa_factory.h"
#include "srsran/ran/plmn_identity.h"
#include "srsran/ran/cause/ngap_cause_converters.h"
#include "srsran/rrc/rrc_ue.h"
#include "srsran/support/async/coroutine.h"
#include "srsran/support/synchronization/sync_event.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <dlfcn.h>
#include <cmath>
#include <future>
#include <limits>
#include <map>
#include <set>
#include <thread>
#include <variant>

using namespace srsran;
using namespace srs_cu_cp;

static std::string normalize_ntn_calendar_hash_for_comparison(std::string value)
{
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::tolower(c); });
  if (value.rfind("sha256:", 0) != 0) {
    value.insert(0, "sha256:");
  }
  return value;
}

static bool ntn_deployment_stage_may_have_installed_calendar(ntn_position_plan_deployment_stage stage)
{
  return stage == ntn_position_plan_deployment_stage::preparing ||
         stage == ntn_position_plan_deployment_stage::ready ||
         stage == ntn_position_plan_deployment_stage::applied;
}

static bool ntn_snapshot_matches_recorded_deployment(
    const ntn_onboard_position_plan_state_snapshot& snapshot,
    const ntn_onboard_position_plan_persistent_state& state)
{
  return snapshot.source.schedule_version == state.recorded_deployment_schedule_version &&
         normalize_ntn_calendar_hash_for_comparison(snapshot.calendar_hash) ==
             normalize_ntn_calendar_hash_for_comparison(state.recorded_deployment_calendar_hash);
}

static constexpr const char* ntn_access_state_none               = "none";
static constexpr const char* ntn_access_state_access_active      = "access_active";
static constexpr const char* ntn_access_state_released_after_ics = "released_after_ics";
static constexpr const char* ntn_access_state_blocked            = "blocked";

static constexpr const char* ntn_service_state_none            = "none";
static constexpr const char* ntn_service_state_binding_pending = "binding_pending";
static constexpr const char* ntn_service_state_service_bound   = "service_bound";
static constexpr const char* ntn_service_state_blocked         = "blocked";

static constexpr const char* ntn_runtime_state_none           = "none";
static constexpr const char* ntn_runtime_state_access_active  = "access_active";
static constexpr const char* ntn_runtime_state_control_only   = "control_only";
static constexpr const char* ntn_runtime_state_binding_pending = "binding_pending";
static constexpr const char* ntn_runtime_state_service_bound  = "service_bound";
static constexpr const char* ntn_runtime_state_blocked        = "blocked";

static constexpr const char* ntn_location_freshness_not_required = "not_required";
static constexpr const char* ntn_location_freshness_fresh        = "fresh";
static constexpr const char* ntn_location_freshness_missing      = "missing";
static constexpr const char* ntn_location_freshness_stale        = "stale";
static constexpr const char* ntn_location_freshness_release_pending = "release_pending";

static constexpr unsigned ntn_rnti_lease_pool_size          = 8;
static constexpr unsigned ntn_rnti_lease_pool_low_watermark = 2;
static constexpr uint32_t ntn_rnti_lease_expiry_ms          = 30000;
static constexpr uint16_t ntn_rnti_lease_min_value          = 0x4601;
static constexpr uint16_t ntn_rnti_lease_max_value          = 0xffef;

struct ntn_static_opportunity_validation {
  bool        accepted = false;
  std::string detail   = "du_static_opportunity_preflight_missing";
};

struct ntn_static_opportunity_expectation_count {
  uint32_t ssb   = 0;
  uint32_t prach = 0;
};

using ntn_static_opportunity_expectation_counts = std::array<ntn_static_opportunity_expectation_count, 2>;

static ntn_static_opportunity_expectation_counts
count_ntn_static_opportunity_expectations(const f1ap_ntn_access_calendar_update& update)
{
  ntn_static_opportunity_expectation_counts result{};
  for (unsigned cell_index = 0; cell_index != std::min(update.cells.size(), result.size()); ++cell_index) {
    for (const f1ap_ntn_access_calendar_intent& intent : update.cells[cell_index].intents) {
      switch (intent.purpose) {
        case f1ap_ntn_access_calendar_purpose::ssb_sib_paging:
        case f1ap_ntn_access_calendar_purpose::ssb_sib_paging_rar:
          ++result[cell_index].ssb;
          break;
        case f1ap_ntn_access_calendar_purpose::prach_ro:
          ++result[cell_index].prach;
          break;
        case f1ap_ntn_access_calendar_purpose::prach_ul_beam:
        case f1ap_ntn_access_calendar_purpose::invalid:
          break;
      }
    }
  }
  return result;
}

static ntn_static_opportunity_validation validate_ntn_static_opportunity_preflight(
    const f1ap_ntn_access_calendar_result&           result,
    const ntn_static_opportunity_expectation_counts& expected)
{
  for (unsigned cell_index = 0; cell_index != expected.size(); ++cell_index) {
    const f1ap_ntn_access_calendar_preflight_report& report = result.preflight_reports[cell_index];
    if (!report.performed) {
      return {false, fmt::format("du_static_opportunity_preflight_missing_cell_{}", cell_index)};
    }
    if (report.expected_ssb != expected[cell_index].ssb || report.expected_prach != expected[cell_index].prach) {
      return {false, fmt::format("du_static_opportunity_preflight_expected_count_mismatch_cell_{}", cell_index)};
    }
    if (!report.passed || report.first_unmatched.has_value()) {
      if (report.first_unmatched.has_value()) {
        const auto& unmatched = *report.first_unmatched;
        const char* purpose = unmatched.purpose == f1ap_ntn_access_calendar_preflight_purpose::ssb ? "ssb" : "prach";
        return {false,
                fmt::format("du_static_opportunity_unmatched_cell_{}_position_{}_purpose_{}_start_slot_{}",
                            cell_index,
                            unmatched.position_id,
                            purpose,
                            unmatched.start_slot_offset)};
      }
      return {false, fmt::format("du_static_opportunity_preflight_failed_cell_{}", cell_index)};
    }
    if (report.matched_ssb != expected[cell_index].ssb || report.matched_prach != expected[cell_index].prach) {
      return {false, fmt::format("du_static_opportunity_preflight_matched_count_mismatch_cell_{}", cell_index)};
    }

    const uint32_t slots_per_ms        = 1U << report.numerology;
    const uint32_t max_ssb_gap_slots   = 80U * slots_per_ms;
    const uint32_t max_prach_gap_slots = 640U * slots_per_ms;
    if ((expected[cell_index].ssb != 0 &&
         (report.max_ssb_gap_slots == 0 || report.max_ssb_gap_slots > max_ssb_gap_slots)) ||
        (expected[cell_index].prach != 0 &&
         (report.max_prach_gap_slots == 0 || report.max_prach_gap_slots > max_prach_gap_slots))) {
      return {false, fmt::format("du_static_opportunity_interval_mismatch_cell_{}", cell_index)};
    }
  }
  return {true, "static_ssb_prach_opportunities_matched_no_position_or_rf_evidence"};
}

static ntn_static_opportunity_validation validate_ntn_static_opportunity_preflight(
    const f1ap_ntn_access_calendar_result& result, const f1ap_ntn_access_calendar_update& update)
{
  if (update.cells.size() != result.preflight_reports.size()) {
    return {};
  }
  return validate_ntn_static_opportunity_preflight(result, count_ntn_static_opportunity_expectations(update));
}

static bool carries_ntn_static_opportunity_feedback(const f1ap_ntn_access_calendar_result& result)
{
  if (result.status == f1ap_ntn_access_calendar_result_status::preparing ||
      result.status == f1ap_ntn_access_calendar_result_status::ready ||
      result.status == f1ap_ntn_access_calendar_result_status::applied) {
    return true;
  }
  if (std::any_of(result.preflight_reports.begin(), result.preflight_reports.end(), [](const auto& report) {
        return report.performed;
      })) {
    return true;
  }
  return result.reject_reason == "scheduler_preflight_not_performed" ||
         result.reject_reason == "scheduler_preflight_failed" ||
         result.reject_reason == "static_opportunity_missing" ||
         result.reject_reason == "static_opportunity_mismatch";
}

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
  scheduler_cfg.beams                              = ntn_cfg.beams;
  scheduler_cfg.analog_beams                       = ntn_cfg.analog_beams;
  scheduler_cfg.min_elevation_deg                  = ntn_cfg.served_beam_min_elevation_deg;
  scheduler_cfg.max_nof_served_beams               = ntn_cfg.max_nof_served_beams;
  scheduler_cfg.max_nof_active_analog_access_beams = ntn_cfg.max_nof_active_analog_access_beams;
  scheduler_cfg.served_beam_hopping_enabled        = ntn_cfg.served_beam_hopping_enabled;
  scheduler_cfg.demand_aware_beam_scheduling_enabled = ntn_cfg.demand_aware_beam_scheduling_enabled;
  scheduler_cfg.served_beam_hopping_dwell_updates = ntn_cfg.served_beam_hopping_dwell_updates;
  return ntn_served_beam_scheduler{std::move(scheduler_cfg)};
}

static ntn_onboard_position_plan_config make_ntn_onboard_position_plan_config(const cu_cp_configuration& cfg)
{
  ntn_onboard_position_plan_config result;
  const auto& source_cfg = cfg.mobility.onboard_position_plan;
  result.enabled                                  = source_cfg.enabled;
  result.require_external_apply                   = source_cfg.du_execution_enabled;
  result.satellite_id                             = source_cfg.satellite_id;
  result.expected_catalog_id                      = source_cfg.expected_catalog_id;
  result.expected_catalog_hash                    = source_cfg.expected_catalog_hash;
  result.expected_identity_registry_version       = source_cfg.expected_identity_registry_version;
  result.expected_identity_registry_hash          = source_cfg.expected_identity_registry_hash;
  result.expected_access_profile_id               = source_cfg.expected_access_profile_id;
  result.expected_access_profile_hash             = source_cfg.expected_access_profile_hash;
  result.max_l1_positions_per_cell                = source_cfg.max_l1_positions_per_cell;
  result.max_l1_positions_per_satellite           = source_cfg.max_l1_positions_per_satellite;
  result.max_analog_ports_per_cell                = source_cfg.max_analog_ports_per_cell;
  result.max_analog_ports_per_satellite           = source_cfg.max_analog_ports_per_satellite;
  result.max_digital_ports_per_cell               = source_cfg.max_digital_ports_per_cell;
  result.max_digital_ports_per_satellite          = source_cfg.max_digital_ports_per_satellite;
  result.access_slot                              = source_cfg.access_slot;
  result.subvisit_duration                        = source_cfg.subvisit_duration;
  result.max_ssb_interval                         = source_cfg.max_ssb_interval;
  result.max_prach_interval                       = source_cfg.max_prach_interval;
  result.activation_alignment                     = source_cfg.activation_alignment;
  for (unsigned index = 0; index != result.onboard_cells.size(); ++index) {
    result.onboard_cells[index] = {source_cfg.cell_ncis[index], source_cfg.cell_pcis[index]};
  }
  return result;
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

static const char* to_ntn_handover_failure_reason(ntn_handover_failure_cause cause)
{
  switch (cause) {
    case ntn_handover_failure_cause::none:
      return "none";
    case ntn_handover_failure_cause::source_preparation_failed:
      return "source_preparation_failed";
    case ntn_handover_failure_cause::target_ue_removed:
      return "target_ue_removed";
    case ntn_handover_failure_cause::target_reconfiguration_timeout:
      return "target_reconfiguration_timeout";
    case ntn_handover_failure_cause::target_security_context_missing:
      return "target_security_context_missing";
    case ntn_handover_failure_cause::target_bearer_context_modification_failed:
      return "target_bearer_context_modification_failed";
  }
  return "unknown";
}

static unsigned enable_common_location_info_in_periodic_reports(rrc_meas_cfg& meas_cfg)
{
  unsigned nof_updated_reports = 0;
  for (rrc_report_cfg_to_add_mod& report_cfg : meas_cfg.report_cfg_to_add_mod_list) {
    rrc_periodical_report_cfg* periodical_cfg = std::get_if<rrc_periodical_report_cfg>(&report_cfg.report_cfg);
    if (periodical_cfg == nullptr) {
      continue;
    }
    if (!periodical_cfg->include_common_location_info_r16) {
      ++nof_updated_reports;
    }
    periodical_cfg->include_common_location_info_r16 = true;
  }
  return nof_updated_reports;
}

static unsigned disable_common_location_info_in_periodic_reports(rrc_meas_cfg& meas_cfg)
{
  unsigned nof_updated_reports = 0;
  for (rrc_report_cfg_to_add_mod& report_cfg : meas_cfg.report_cfg_to_add_mod_list) {
    rrc_periodical_report_cfg* periodical_cfg = std::get_if<rrc_periodical_report_cfg>(&report_cfg.report_cfg);
    if (periodical_cfg == nullptr) {
      continue;
    }
    if (periodical_cfg->include_common_location_info_r16) {
      ++nof_updated_reports;
    }
    periodical_cfg->include_common_location_info_r16 = false;
  }
  return nof_updated_reports;
}

static bool meas_config_has_common_location_info_in_periodic_reports(const rrc_meas_cfg& meas_cfg)
{
  for (const rrc_report_cfg_to_add_mod& report_cfg : meas_cfg.report_cfg_to_add_mod_list) {
    const rrc_periodical_report_cfg* periodical_cfg = std::get_if<rrc_periodical_report_cfg>(&report_cfg.report_cfg);
    if (periodical_cfg != nullptr && periodical_cfg->include_common_location_info_r16) {
      return true;
    }
  }
  return false;
}

static bool is_ntn_location_report_fresh(const ntn_location_mobility_config&     ntn_cfg,
                                         const ntn_ue_location_report&           report,
                                         std::chrono::steady_clock::time_point   now)
{
  return ntn_cfg.location_max_age.count() == 0 || report.received_time >= now ||
         now - report.received_time <= ntn_cfg.location_max_age;
}

static std::string format_beam_assignment_state(ntn_beam_assignment_state state)
{
  return ntn_beam_assignment_state_to_string(state);
}

static cu_cp_ntn_beam_assignment_state to_cu_cp_ntn_beam_assignment_state(ntn_beam_assignment_state state)
{
  switch (state) {
    case ntn_beam_assignment_state::inactive:
      return cu_cp_ntn_beam_assignment_state::inactive;
    case ntn_beam_assignment_state::candidate:
      return cu_cp_ntn_beam_assignment_state::candidate;
    case ntn_beam_assignment_state::active_loaded:
      return cu_cp_ntn_beam_assignment_state::active_loaded;
    case ntn_beam_assignment_state::draining:
      return cu_cp_ntn_beam_assignment_state::draining;
  }
  return cu_cp_ntn_beam_assignment_state::inactive;
}

static ntn_service_beam_policy get_beam_service_policy(
    const ntn_service_switch_over_snapshot& switch_over_snapshot,
    const std::string&                      beam_id,
    nr_cell_identity                        nci)
{
  const auto beam_it =
      std::find_if(switch_over_snapshot.beams.begin(),
                   switch_over_snapshot.beams.end(),
                   [&beam_id, nci](const ntn_service_switch_over_beam_snapshot& beam) {
                     return beam.beam_id == beam_id || beam.nci.value() == nci.value();
                   });
  return beam_it != switch_over_snapshot.beams.end() ? beam_it->policy : ntn_service_beam_policy::normal;
}

static bool switch_over_event_affects_beam_or_nci(const ntn_service_switch_over_event& event,
                                                  const std::string&                   beam_id,
                                                  std::optional<nr_cell_identity>      nci)
{
  if (std::find(event.affected_beam_ids.begin(), event.affected_beam_ids.end(), beam_id) !=
      event.affected_beam_ids.end()) {
    return true;
  }
  if (!nci.has_value()) {
    return false;
  }
  return std::any_of(event.affected_ncis.begin(), event.affected_ncis.end(), [nci](nr_cell_identity affected_nci) {
    return affected_nci.value() == nci->value();
  });
}

static bool contains_beam_id(const std::vector<std::string>& beam_ids, const std::string& beam_id)
{
  return std::find(beam_ids.begin(), beam_ids.end(), beam_id) != beam_ids.end();
}

static bool contains_beam_id(const std::set<std::string>& beam_ids, const std::string& beam_id)
{
  return beam_ids.find(beam_id) != beam_ids.end();
}

static bool contains_candidate_beam_id(const std::vector<ntn_served_beam_candidate>& candidates,
                                       const std::string&                            beam_id)
{
  return std::find_if(candidates.begin(), candidates.end(), [&beam_id](const ntn_served_beam_candidate& candidate) {
           return candidate.beam_id == beam_id;
         }) != candidates.end();
}

static unsigned get_served_candidate_order(const std::vector<ntn_served_beam_candidate>& candidates,
                                           const std::string&                            beam_id)
{
  for (unsigned i = 0; i != candidates.size(); ++i) {
    if (candidates[i].beam_id == beam_id) {
      return i;
    }
  }
  return std::numeric_limits<unsigned>::max();
}

static std::map<std::string, std::string>
map_candidate_satellite_ids(const std::vector<ntn_served_beam_candidate>& candidates)
{
  std::map<std::string, std::string> result;
  for (const ntn_served_beam_candidate& candidate : candidates) {
    if (!candidate.beam_id.empty()) {
      result[candidate.beam_id] = candidate.serving_satellite_id;
    }
  }
  return result;
}

static std::set<std::string> collect_window_satellite_ids(const std::vector<ntn_served_beam_candidate>& candidates)
{
  std::set<std::string> result;
  for (const ntn_served_beam_candidate& candidate : candidates) {
    if (candidate.in_hopping_window && !candidate.serving_satellite_id.empty()) {
      result.insert(candidate.serving_satellite_id);
    }
  }
  return result;
}

static unsigned count_satellite_owner_changes(const std::map<std::string, std::string>& current_owners,
                                              const std::map<std::string, std::string>& next_owners)
{
  unsigned count = 0;
  for (const auto& current_owner : current_owners) {
    const auto next_it = next_owners.find(current_owner.first);
    if (next_it != next_owners.end() && next_it->second != current_owner.second) {
      ++count;
    }
  }
  return count;
}

static bool ntn_service_policy_blocks_new_demand(ntn_service_beam_policy policy)
{
  return policy == ntn_service_beam_policy::block_new_demand || policy == ntn_service_beam_policy::drain ||
         policy == ntn_service_beam_policy::release_allowed;
}

static bool ntn_service_policy_forces_drain(ntn_service_beam_policy policy)
{
  return policy == ntn_service_beam_policy::drain || policy == ntn_service_beam_policy::release_allowed;
}

static unsigned get_ntn_loaded_digital_service_beam_cap(const ntn_location_mobility_config& ntn_cfg)
{
  return ntn_cfg.max_nof_loaded_digital_service_beams;
}

static std::string make_ntn_beam_state_reason(ntn_beam_assignment_state state,
                                              bool                      stale_assistance,
                                              ntn_service_beam_policy   service_policy)
{
  if (ntn_service_policy_forces_drain(service_policy)) {
    return "service_switch_over";
  }
  if (stale_assistance && (state == ntn_beam_assignment_state::active_loaded ||
                           state == ntn_beam_assignment_state::draining)) {
    return "stale_assistance";
  }
  switch (state) {
    case ntn_beam_assignment_state::active_loaded:
      return "loaded_service_calendar";
    case ntn_beam_assignment_state::candidate:
      return ntn_service_policy_blocks_new_demand(service_policy) ? "service_switch_over_prepare"
                                                                  : "visible_without_loaded_demand";
    case ntn_beam_assignment_state::draining:
      return "draining_existing_demand";
    case ntn_beam_assignment_state::inactive:
      return "not_mobility_eligible";
  }
  return "unknown";
}

static const char* ntn_beam_tac_reason_to_string(const ntn_beam_tac_result& result)
{
  if (result.tac.has_value()) {
    return "valid";
  }
  switch (result.reason) {
    case ntn_beam_tac_invalid_reason::missing_decimal_suffix:
      return "missing_decimal_suffix";
    case ntn_beam_tac_invalid_reason::out_of_range:
      return "out_of_range";
    case ntn_beam_tac_invalid_reason::none:
      break;
  }
  return "missing_decimal_suffix";
}

static const char* make_ntn_paging_recommendation_reason(ntn_beam_assignment_state state,
                                                         bool                      stale_assistance,
                                                         bool                      drain_forced,
                                                         bool                      downlink_ready,
                                                         bool                      uplink_access_ready,
                                                         bool                      access_roundtrip_ready,
                                                         const ntn_beam_tac_result& tac)
{
  if (!tac.tac.has_value()) {
    return "invalid_tac";
  }
  if (stale_assistance) {
    return "stale_assistance";
  }
  if (state == ntn_beam_assignment_state::draining || drain_forced) {
    return "draining";
  }
  if (state == ntn_beam_assignment_state::inactive) {
    return "inactive";
  }
  if (!downlink_ready) {
    return "downlink_unavailable";
  }
  if (!uplink_access_ready) {
    return "uplink_response_unavailable";
  }
  if (!access_roundtrip_ready) {
    return "access_roundtrip_unavailable";
  }
  return "eligible";
}

static std::string make_ntn_service_binding_failure_reason(const ntn_beam_du_assignment* assignment)
{
  if (assignment == nullptr) {
    return "target_not_active_loaded";
  }
  if (!assignment->resource_domain_eligible) {
    return assignment->resource_domain_reason;
  }
  if (!assignment->downlink_enabled) {
    return "downlink_unavailable";
  }
  if (!assignment->uplink_enabled) {
    return "uplink_unavailable";
  }
  if (!assignment->bidirectional_service_ready) {
    return "service_requires_bidirectional_link";
  }
  return assignment->du_assignment_reason;
}

static bool ntn_paging_tai_list_contains_tac(span<const cu_cp_tai_list_for_paging_item> tai_list, tac_t tac)
{
  return std::any_of(tai_list.begin(), tai_list.end(), [&tac](const cu_cp_tai_list_for_paging_item& item) {
    return item.tai.tac == tac;
  });
}

static const ntn_beam_position* find_ntn_beam_cfg(const std::vector<ntn_beam_position>& beams,
                                                  const std::string&                    beam_id)
{
  const auto it = std::find_if(beams.begin(), beams.end(), [&beam_id](const ntn_beam_position& beam) {
    return beam.beam_id == beam_id;
  });
  return it != beams.end() ? &*it : nullptr;
}

static const ntn_beam_position* find_ntn_beam_cfg_by_nci(const std::vector<ntn_beam_position>& beams,
                                                         nr_cell_identity                      nci)
{
  const auto it = std::find_if(beams.begin(), beams.end(), [nci](const ntn_beam_position& beam) {
    return beam.nci == nci;
  });
  return it != beams.end() ? &*it : nullptr;
}

static const ntn_analog_beam_position* find_ntn_analog_beam_cfg(
    const std::vector<ntn_analog_beam_position>& analog_beams,
    const std::string&                           analog_beam_id)
{
  const auto it = std::find_if(
      analog_beams.begin(), analog_beams.end(), [&analog_beam_id](const ntn_analog_beam_position& analog) {
        return analog.analog_beam_id == analog_beam_id;
      });
  return it != analog_beams.end() ? &*it : nullptr;
}

static trp_id_t make_ntn_trp_id_from_nci(nr_cell_identity nci)
{
  uint32_t id = static_cast<uint32_t>(nci.value() & 0xffffU);
  return uint_to_trp_id(id == 0 ? 1 : id);
}

static bool trp_information_request_matches_trp(const trp_information_request_t& request, trp_id_t trp_id)
{
  return request.trp_list.empty() ||
         std::find(request.trp_list.begin(), request.trp_list.end(), trp_id) != request.trp_list.end();
}

static bool is_supported_ntn_trp_information_type(trp_information_type_item_t type)
{
  switch (type) {
    case trp_information_type_item_t::nr_pci:
    case trp_information_type_item_t::ng_ran_cgi:
    case trp_information_type_item_t::arfcn:
    case trp_information_type_item_t::geo_coord:
    case trp_information_type_item_t::trp_type:
      return true;
    default:
      return false;
  }
}

static positioning_information_failure_t make_ntn_positioning_information_failure()
{
  positioning_information_failure_t failure;
  failure.cause = nrppa_cause_misc_t::unspecified;
  return failure;
}

static measurement_failure_t make_ntn_measurement_failure(const measurement_request_t& request)
{
  measurement_failure_t failure;
  failure.lmf_meas_id = request.lmf_meas_id;
  failure.ran_meas_id = request.ran_meas_id;
  failure.cause       = nrppa_cause_misc_t::unspecified;
  return failure;
}

static positioning_activation_failure_t make_ntn_positioning_activation_failure()
{
  positioning_activation_failure_t failure;
  failure.cause = nrppa_cause_misc_t::unspecified;
  return failure;
}

static positioning_deactivation_failure_t make_ntn_positioning_deactivation_failure()
{
  positioning_deactivation_failure_t failure;
  failure.cause = nrppa_cause_misc_t::unspecified;
  return failure;
}

static positioning_assistance_information_failure_t
make_ntn_positioning_assistance_information_failure(const positioning_assistance_information_control_request_t& request)
{
  positioning_assistance_information_failure_t failure;
  failure.transaction_id = request.transaction_id;
  failure.cause          = nrppa_cause_misc_t::unspecified;
  return failure;
}

static std::vector<trp_information_type_item_t>
get_effective_ntn_trp_information_type_request(const trp_information_request_t& request)
{
  if (!request.trp_info_type_list_trp_req.empty()) {
    return request.trp_info_type_list_trp_req;
  }
  return {trp_information_type_item_t::nr_pci,
          trp_information_type_item_t::ng_ran_cgi,
          trp_information_type_item_t::arfcn,
          trp_information_type_item_t::geo_coord,
          trp_information_type_item_t::trp_type};
}

static uint32_t encode_nrppa_latitude_abs(double latitude_deg)
{
  const double clipped = std::clamp(std::abs(latitude_deg), 0.0, 90.0);
  return static_cast<uint32_t>(std::clamp<long long>(std::llround(clipped * 8388608.0 / 90.0), 0, 8388608));
}

static int32_t encode_nrppa_longitude(double longitude_deg)
{
  const double clipped = std::clamp(longitude_deg, -180.0, 180.0);
  return static_cast<int32_t>(std::clamp<long long>(std::llround(clipped * 8388608.0 / 180.0), -8388608, 8388607));
}

static geographical_coordinates_t make_ntn_trp_geographical_coordinates(const ntn_beam_position& beam)
{
  ng_ran_access_point_position_t pos{};
  pos.latitude_sign              = beam.center_latitude_deg < 0.0 ? latitude_sign_t::south : latitude_sign_t::north;
  pos.latitude                   = encode_nrppa_latitude_abs(beam.center_latitude_deg);
  pos.longitude                  = encode_nrppa_longitude(beam.center_longitude_deg);
  pos.direction_of_altitude      = direction_of_altitude_t::height;
  pos.altitude                   = 0;
  pos.uncertainty_semi_major     = 0;
  pos.uncertainty_semi_minor     = 0;
  pos.orientation_of_major_axis  = 0;
  pos.uncertainty_altitude       = 0;
  pos.confidence                 = 100;

  geographical_coordinates_t geo;
  geo.trp_position_definition_type = trp_position_direct_t{pos};
  return geo;
}

static ntn_analog_beam_resource_policy get_effective_analog_resource_policy(
    const ntn_location_mobility_config& ntn_cfg,
    const ntn_analog_beam_position&     analog)
{
  ntn_analog_beam_resource_policy policy = ntn_cfg.resource_policy.analog;
  if (!analog.resource_policy.has_value()) {
    return policy;
  }
  if (analog.resource_policy->max_access_only_ues != 0) {
    policy.max_access_only_ues = analog.resource_policy->max_access_only_ues;
  }
  if (analog.resource_policy->max_service_bound_ues != 0) {
    policy.max_service_bound_ues = analog.resource_policy->max_service_bound_ues;
  }
  if (analog.resource_policy->max_loaded_digital_children != 0) {
    policy.max_loaded_digital_children = analog.resource_policy->max_loaded_digital_children;
  }
  if (analog.resource_policy->max_drbs != 0) {
    policy.max_drbs = analog.resource_policy->max_drbs;
  }
  return policy;
}

static double deg_to_rad(double value)
{
  constexpr double pi = 3.14159265358979323846;
  return value * pi / 180.0;
}

static double normalized_longitude_delta_deg(double lhs, double rhs)
{
  double delta = std::fmod(lhs - rhs + 540.0, 360.0);
  if (delta < 0.0) {
    delta += 360.0;
  }
  return delta - 180.0;
}

static double distance_m(double latitude_deg, double longitude_deg, const ntn_beam_position& beam)
{
  constexpr double earth_radius_m = 6371000.0;
  const double     lat1           = deg_to_rad(latitude_deg);
  const double     lat2           = deg_to_rad(beam.center_latitude_deg);
  const double     dlat           = deg_to_rad(beam.center_latitude_deg - latitude_deg);
  const double     dlon           = deg_to_rad(normalized_longitude_delta_deg(beam.center_longitude_deg, longitude_deg));

  const double sin_dlat = std::sin(dlat / 2.0);
  const double sin_dlon = std::sin(dlon / 2.0);
  const double a        = sin_dlat * sin_dlat + std::cos(lat1) * std::cos(lat2) * sin_dlon * sin_dlon;
  const double c        = 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
  return earth_radius_m * c;
}

static std::string make_ntn_analog_access_reason(const ntn_location_mobility_config& ntn_cfg,
                                                 const ntn_beam_du_assignment&       assignment,
                                                 const ntn_beam_position*            beam_cfg,
                                                 const ntn_analog_beam_position*     analog_cfg,
                                                 bool                                stale_assistance)
{
  if (ntn_cfg.analog_beams.empty()) {
    return "not_configured";
  }
  if (beam_cfg == nullptr || beam_cfg->analog_beam_id.empty() || analog_cfg == nullptr) {
    return "invalid_parent";
  }
  if (!analog_cfg->enabled) {
    return "disabled";
  }
  if (!analog_cfg->uplink_enabled || !beam_cfg->uplink_enabled) {
    return "uplink_unavailable";
  }
  if (stale_assistance) {
    return "stale_assistance";
  }
  if (assignment.state == ntn_beam_assignment_state::draining) {
    return "draining";
  }
  if (assignment.state == ntn_beam_assignment_state::inactive) {
    return "inactive";
  }
  if (!assignment.in_hopping_window) {
    return "outside_analog_window";
  }
  return "eligible";
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
  if (assignment.access_du_index != du_index_t::invalid) {
    formatted += ":access_du";
    formatted += std::to_string(du_index_to_uint(assignment.access_du_index));
  }
  if (!assignment.du_assignment_reason.empty() && assignment.du_assignment_reason != "eligible") {
    formatted += ":du_policy=";
    formatted += assignment.du_assignment_reason;
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
    if (!assignment.resource_weight_reason.empty() && assignment.resource_weight_reason != "legacy") {
      formatted += ":weight=";
      formatted += std::to_string(assignment.resource_weight);
      formatted += ":reason=";
      formatted += assignment.resource_weight_reason;
    }
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
  if (assignment.qos.has_qos_demand) {
    formatted += ":arp";
    formatted += std::to_string(assignment.qos.best_arp_priority);
    formatted += ":qos";
    formatted += std::to_string(assignment.qos.best_qos_priority);
  }
  return formatted;
}

static std::string format_beam_placement_plan(const ntn_beam_placement_plan& plan)
{
  unsigned nof_active_loaded  = 0;
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
      case ntn_beam_assignment_state::active_loaded:
        ++nof_active_loaded;
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

  return fmt::format("total={} active_loaded={} candidate={} draining={} inactive={} window={} scheduled={} shown=[{}]{}",
                     plan.assignments.size(),
                     nof_active_loaded,
                     nof_candidate,
                     nof_draining,
                     nof_inactive,
                     nof_window,
                     nof_scheduled,
                     entries,
                     nof_entries_shown < nof_active_loaded + nof_candidate + nof_draining ? "..." : "");
}

static bool are_beam_assignments_equal(const ntn_beam_du_assignment& lhs, const ntn_beam_du_assignment& rhs)
{
  return lhs.beam_id == rhs.beam_id && lhs.nci == rhs.nci && lhs.du_index == rhs.du_index && lhs.state == rhs.state &&
         lhs.serving_satellite_id == rhs.serving_satellite_id &&
         lhs.elevation_deg == rhs.elevation_deg && lhs.nof_ues == rhs.nof_ues && lhs.nof_drbs == rhs.nof_drbs &&
         lhs.in_hopping_window == rhs.in_hopping_window && lhs.antenna_slot_index == rhs.antenna_slot_index &&
         lhs.nof_antenna_slots == rhs.nof_antenna_slots && lhs.antenna_slot_period == rhs.antenna_slot_period &&
         lhs.sr_slot_offset == rhs.sr_slot_offset && lhs.sr_slot_period == rhs.sr_slot_period &&
         lhs.srs_slot_offset == rhs.srs_slot_offset && lhs.srs_slot_period == rhs.srs_slot_period &&
         lhs.resource_weight == rhs.resource_weight && lhs.resource_share == rhs.resource_share &&
         lhs.resource_weight_reason == rhs.resource_weight_reason &&
         lhs.qos == rhs.qos && lhs.access_du_index == rhs.access_du_index &&
         lhs.access_du_reason == rhs.access_du_reason && lhs.du_assignment_reason == rhs.du_assignment_reason &&
         lhs.resource_domain_eligible == rhs.resource_domain_eligible &&
         lhs.resource_domain_reason == rhs.resource_domain_reason && lhs.reuse_group_id == rhs.reuse_group_id &&
         lhs.conflict_group_ids == rhs.conflict_group_ids &&
         lhs.analog_loaded_digital_child_cap == rhs.analog_loaded_digital_child_cap &&
         lhs.analog_loaded_digital_child_load == rhs.analog_loaded_digital_child_load &&
         lhs.analog_service_bound_ue_cap == rhs.analog_service_bound_ue_cap &&
         lhs.analog_service_bound_ue_load == rhs.analog_service_bound_ue_load &&
         lhs.analog_drb_cap == rhs.analog_drb_cap && lhs.analog_drb_load == rhs.analog_drb_load &&
         lhs.digital_ue_cap == rhs.digital_ue_cap && lhs.digital_ue_load == rhs.digital_ue_load &&
         lhs.digital_drb_cap == rhs.digital_drb_cap && lhs.digital_drb_load == rhs.digital_drb_load &&
         lhs.downlink_enabled == rhs.downlink_enabled && lhs.uplink_enabled == rhs.uplink_enabled &&
         lhs.downlink_ready == rhs.downlink_ready && lhs.uplink_ready == rhs.uplink_ready &&
         lhs.downlink_visible == rhs.downlink_visible && lhs.uplink_access_ready == rhs.uplink_access_ready &&
         lhs.access_roundtrip_ready == rhs.access_roundtrip_ready &&
         lhs.bidirectional_service_ready == rhs.bidirectional_service_ready;
}

static bool are_analog_access_du_assignments_equal(const ntn_analog_access_du_assignment& lhs,
                                                   const ntn_analog_access_du_assignment& rhs)
{
  return lhs.analog_beam_id == rhs.analog_beam_id &&
         lhs.selected_access_du_index == rhs.selected_access_du_index &&
         lhs.supported_child_count == rhs.supported_child_count &&
         lhs.assigned_child_count == rhs.assigned_child_count && lhs.reason == rhs.reason;
}

static bool are_beam_placement_plans_equal(const ntn_beam_placement_plan& lhs, const ntn_beam_placement_plan& rhs)
{
  if (lhs.analog_assignments.size() != rhs.analog_assignments.size()) {
    return false;
  }
  for (unsigned i = 0; i != lhs.analog_assignments.size(); ++i) {
    if (!are_analog_access_du_assignments_equal(lhs.analog_assignments[i], rhs.analog_assignments[i])) {
      return false;
    }
  }
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

static bool is_weighted_resource_assignment(const ntn_beam_du_assignment& assignment)
{
  return assignment.nof_antenna_slots != 0 && assignment.resource_weight_reason != "none" &&
         assignment.resource_weight_reason != "legacy";
}

static bool is_qos_boosted_resource_assignment(const ntn_beam_du_assignment& assignment)
{
  return assignment.resource_weight_reason == "qos_weighted";
}

static std::set<nr_cell_identity> get_ntn_core_reportable_ncis(const ntn_beam_placement_plan& plan)
{
  std::set<nr_cell_identity> ncis;
  for (const auto& assignment : plan.assignments) {
    if (assignment.state == ntn_beam_assignment_state::active_loaded ||
        assignment.state == ntn_beam_assignment_state::draining) {
      ncis.insert(assignment.nci);
    }
  }
  return ncis;
}

static std::optional<nr_cell_identity> get_ue_serving_nci_for_ntn_load(du_processor_repository& du_db, cu_cp_ue& ue)
{
  // Prefer the latest accepted NTN location report. It records the serving NCI selected by NTN mobility and can be
  // newer than the static PCell mapping after beam movement.
  if (ue.get_meas_context().last_ntn_location_report.has_value()) {
    return ue.get_meas_context().last_ntn_location_report->serving_nci;
  }

  // Without an NTN location report, fall back to the UE's current DU and PCell. Invalid indexes mean CU-CP cannot
  // derive a serving NCI from DU context.
  if (ue.get_du_index() == du_index_t::invalid || ue.get_pcell_index() == srs_cu_cp::du_cell_index_t::invalid) {
    return std::nullopt;
  }

  // Resolve the DU processor and its served cell table, then map DU-local cell_index to NR Cell Identity.
  du_processor* processor = du_db.find_du_processor(ue.get_du_index());
  if (processor == nullptr || processor->get_context() == nullptr) {
    return std::nullopt;
  }

  // The served cell list keeps the DU-local cell_index to CGI/NCI mapping used by NTN load accounting.
  for (const auto& cell : processor->get_context()->served_cells) {
    if (cell.cell_index == ue.get_pcell_index()) {
      return cell.cgi.nci;
    }
  }
  return std::nullopt;
}

static const du_cell_configuration*
find_du_cell_by_nci(du_processor_repository& du_db, du_index_t du_index, nr_cell_identity nci)
{
  du_processor* processor = du_db.find_du_processor(du_index);
  if (processor == nullptr || processor->get_context() == nullptr) {
    return nullptr;
  }

  const auto& served_cells = processor->get_context()->served_cells;
  auto        it           = std::find_if(served_cells.begin(), served_cells.end(), [nci](const du_cell_configuration& cell) {
    return cell.cgi.nci == nci;
  });
  return it != served_cells.end() ? &(*it) : nullptr;
}

const char*
ntn_onboard_detail::get_ntn_onboard_du_cell_resolution_detail(ntn_onboard_du_cell_resolution_status status)
{
  switch (status) {
    case ntn_onboard_du_cell_resolution_status::resolved:
      return "resolved";
    case ntn_onboard_du_cell_resolution_status::unavailable:
      return "du_or_cells_unavailable";
    case ntn_onboard_du_cell_resolution_status::missing:
      return "missing_nci_pci_du_cell_mapping";
    case ntn_onboard_du_cell_resolution_status::duplicate:
      return "duplicate_nci_pci_du_cell_mapping";
    case ntn_onboard_du_cell_resolution_status::cross_du:
      return "two_onboard_cells_resolve_to_different_dus";
  }
  return "unknown_nci_pci_du_cell_mapping";
}

ntn_onboard_detail::ntn_onboard_du_cell_resolution
ntn_onboard_detail::resolve_ntn_onboard_du_cells(
    const std::vector<ntn_onboard_du_cell_candidate>&   candidates,
    const std::array<ntn_onboard_cell_position_set, 2>& planned_cells,
    const std::set<du_index_t>&                         disconnected_du_indexes)
{
  ntn_onboard_du_cell_resolution result;
  std::array<unsigned, 2>        match_counts{};
  std::array<du_index_t, 2>      matched_du_indexes{du_index_t::invalid, du_index_t::invalid};

  for (const ntn_onboard_du_cell_candidate& candidate : candidates) {
    if (disconnected_du_indexes.count(candidate.du_index) != 0) {
      continue;
    }
    for (unsigned i = 0; i != planned_cells.size(); ++i) {
      const ntn_onboard_cell_identity& identity = planned_cells[i].identity;
      if (candidate.nci == identity.nci && candidate.pci == identity.pci) {
        ++match_counts[i];
        if (match_counts[i] == 1) {
          result.cells[i]       = candidate.cell;
          matched_du_indexes[i] = candidate.du_index;
        }
      }
    }
  }

  if (match_counts[0] > 1 || match_counts[1] > 1) {
    result.status = ntn_onboard_du_cell_resolution_status::duplicate;
    return result;
  }
  if (match_counts[0] == 0 && match_counts[1] == 0) {
    result.status = ntn_onboard_du_cell_resolution_status::unavailable;
    return result;
  }
  if (match_counts[0] != 1 || match_counts[1] != 1) {
    result.status = ntn_onboard_du_cell_resolution_status::missing;
    return result;
  }
  if (matched_du_indexes[0] != matched_du_indexes[1]) {
    result.status = ntn_onboard_du_cell_resolution_status::cross_du;
    return result;
  }

  result.status   = ntn_onboard_du_cell_resolution_status::resolved;
  result.du_index = matched_du_indexes[0];
  return result;
}

static ntn_onboard_detail::ntn_onboard_du_cell_resolution
resolve_ntn_onboard_du_cells(du_processor_repository&                            du_db,
                             const std::array<ntn_onboard_cell_position_set, 2>& planned_cells,
                             const std::set<du_index_t>&                         disconnected_du_indexes)
{
  std::vector<ntn_onboard_detail::ntn_onboard_du_cell_candidate> candidates;
  for (du_index_t du_index : du_db.get_du_processor_indexes()) {
    du_processor* processor = du_db.find_du_processor(du_index);
    if (processor == nullptr || processor->get_context() == nullptr) {
      continue;
    }
    for (const du_cell_configuration& cell : processor->get_context()->served_cells) {
      candidates.push_back({du_index, cell.cgi.nci, cell.pci, &cell});
    }
  }
  return ntn_onboard_detail::resolve_ntn_onboard_du_cells(candidates, planned_cells, disconnected_du_indexes);
}

static bool du_serves_ntn_onboard_cells(du_processor_repository&                            du_db,
                                        du_index_t                                         du_index,
                                        const std::array<ntn_onboard_cell_position_set, 2>& planned_cells)
{
  du_processor* processor = du_db.find_du_processor(du_index);
  if (processor == nullptr || processor->get_context() == nullptr) {
    return false;
  }

  std::array<bool, 2> matched{};
  for (const du_cell_configuration& cell : processor->get_context()->served_cells) {
    for (unsigned i = 0; i != planned_cells.size(); ++i) {
      matched[i] = matched[i] ||
                   (cell.cgi.nci == planned_cells[i].identity.nci && cell.pci == planned_cells[i].identity.pci);
    }
  }
  return matched[0] && matched[1];
}

using ntn_onboard_detail::get_ntn_onboard_du_cell_resolution_detail;
using ntn_onboard_detail::ntn_onboard_du_cell_resolution;
using ntn_onboard_detail::ntn_onboard_du_cell_resolution_status;

static srsran::du_cell_index_t to_f1ap_du_cell_index(srs_cu_cp::du_cell_index_t cell_index)
{
  if (cell_index == srs_cu_cp::du_cell_index_t::invalid) {
    return srsran::INVALID_DU_CELL_INDEX;
  }
  return srsran::to_du_cell_index(srs_cu_cp::du_cell_index_to_uint(cell_index));
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
      const unsigned per_du_max_active_beams =
          max_active_beams == 0 ? static_cast<unsigned>(supported_beam_ids.size()) : max_active_beams;
      capacities.push_back({du_index,
                            per_du_max_active_beams,
                            max_ues,
                            max_ues * max_drbs_per_ue,
                            std::move(supported_beam_ids),
                            current_ues,
                            current_drbs});
    }
  }
  return capacities;
}

static void add_directional_service_load(std::map<std::string, ntn_beam_load>& loads,
                                         const std::string&                    beam_id,
                                         unsigned                              nof_drbs,
                                         const ntn_qos_demand_summary&         qos,
                                         bool                                  downlink_service_required,
                                         bool                                  uplink_resource_required)
{
  if (beam_id.empty()) {
    return;
  }
  ntn_beam_load& load = loads[beam_id];
  const bool     first_load = load.beam_id.empty();
  load.beam_id              = beam_id;
  ++load.nof_ues;
  load.nof_drbs += nof_drbs;
  if (first_load) {
    load.downlink_service_required = downlink_service_required;
    load.uplink_resource_required  = uplink_resource_required;
  } else {
    load.downlink_service_required = load.downlink_service_required || downlink_service_required;
    load.uplink_resource_required  = load.uplink_resource_required || uplink_resource_required;
  }
  if (!load.qos.has_qos_demand || is_ntn_qos_demand_higher_priority(qos, load.qos)) {
    load.qos = qos;
  } else if (qos.has_qos_demand) {
    load.qos.nof_qos_flows += qos.nof_qos_flows;
  }
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
    const ntn_qos_demand_summary ue_qos = summarize_ntn_qos_demand(ue->get_up_resource_manager().get_up_context());
    if (!load.qos.has_qos_demand || is_ntn_qos_demand_higher_priority(ue_qos, load.qos)) {
      load.qos = ue_qos;
    } else if (ue_qos.has_qos_demand) {
      load.qos.nof_qos_flows += ue_qos.nof_qos_flows;
    }
  }

  std::vector<ntn_beam_load> result;
  result.reserve(loads.size());
  for (const auto& entry : loads) {
    result.push_back(entry.second);
  }
  return result;
}

static void merge_pending_ntn_beam_load(std::vector<ntn_beam_load>&           loads,
                                        const std::string&                    beam_id,
                                        unsigned                              nof_drbs,
                                        const ntn_qos_demand_summary&         qos)
{
  auto load_it = std::find_if(
      loads.begin(), loads.end(), [&beam_id](const ntn_beam_load& load) { return load.beam_id == beam_id; });
  if (load_it == loads.end()) {
    ntn_beam_load load;
    load.beam_id  = beam_id;
    load.nof_ues  = 1;
    load.nof_drbs = nof_drbs;
    load.qos      = qos;
    loads.push_back(load);
    return;
  }

  load_it->nof_drbs += nof_drbs;
  if (!load_it->qos.has_qos_demand || is_ntn_qos_demand_higher_priority(qos, load_it->qos)) {
    load_it->qos = qos;
  } else if (qos.has_qos_demand) {
    load_it->qos.nof_qos_flows += qos.nof_qos_flows;
  }
}

static void merge_pending_ntn_directional_beam_load(std::vector<ntn_beam_load>&           loads,
                                                    const std::string&                    beam_id,
                                                    unsigned                              nof_drbs,
                                                    const ntn_qos_demand_summary&         qos,
                                                    bool                                  downlink_service_required,
                                                    bool                                  uplink_resource_required)
{
  auto load_it = std::find_if(
      loads.begin(), loads.end(), [&beam_id](const ntn_beam_load& load) { return load.beam_id == beam_id; });
  if (load_it == loads.end()) {
    ntn_beam_load load;
    load.beam_id                   = beam_id;
    load.nof_ues                   = 1;
    load.nof_drbs                  = nof_drbs;
    load.qos                       = qos;
    load.downlink_service_required = downlink_service_required;
    load.uplink_resource_required  = uplink_resource_required;
    loads.push_back(load);
    return;
  }

  load_it->nof_drbs += nof_drbs;
  load_it->downlink_service_required = load_it->downlink_service_required || downlink_service_required;
  load_it->uplink_resource_required  = load_it->uplink_resource_required || uplink_resource_required;
  if (!load_it->qos.has_qos_demand || is_ntn_qos_demand_higher_priority(qos, load_it->qos)) {
    load_it->qos = qos;
  } else if (qos.has_qos_demand) {
    load_it->qos.nof_qos_flows += qos.nof_qos_flows;
  }
}

std::vector<ntn_beam_load> cu_cp_impl::build_ntn_beam_loads_for_current_service_contexts()
{
  const auto& ntn_cfg = cfg.mobility.meas_manager_config.ntn_location_mobility;
  if (!ntn_cfg.enabled || ntn_cfg.analog_beams.empty()) {
    return build_ntn_beam_loads(du_db, ue_mng, ntn_cfg.beams);
  }

  std::map<std::string, ntn_beam_load> loads;
  for (cu_cp_ue* ue : ue_mng.get_ues()) {
    if (ue == nullptr) {
      continue;
    }

    const auto layer_it = ntn_ue_layer_states.find(ue->get_ue_index());
    if (layer_it == ntn_ue_layer_states.end()) {
      continue;
    }

    const ntn_ue_access_service_layer_state& layer = layer_it->second;
    if ((layer.service_state != ntn_service_state_binding_pending &&
         layer.service_state != ntn_service_state_service_bound) ||
        layer.service_digital_beam_id.empty()) {
      continue;
    }

    const unsigned committed_drbs =
        layer.service_state == ntn_service_state_service_bound ? ue->get_up_resource_manager().get_nof_drbs() : 0U;
    const unsigned nof_drbs = committed_drbs + layer.pending_drbs;
    ntn_qos_demand_summary ue_qos =
        layer.service_state == ntn_service_state_service_bound
            ? summarize_ntn_qos_demand(ue->get_up_resource_manager().get_up_context())
            : layer.pending_qos;
    if (layer.service_state == ntn_service_state_service_bound && layer.pending_qos.has_qos_demand) {
      if (!ue_qos.has_qos_demand || is_ntn_qos_demand_higher_priority(layer.pending_qos, ue_qos)) {
        ue_qos = layer.pending_qos;
      } else {
        ue_qos.nof_qos_flows += layer.pending_qos.nof_qos_flows;
      }
    }
    const bool has_service_pair = !layer.service_uplink_resource_beam_id.empty() &&
                                  layer.has_service_uplink_resource_nci &&
                                  layer.service_uplink_resource_beam_id != layer.service_digital_beam_id;
    if (has_service_pair) {
      add_directional_service_load(loads, layer.service_digital_beam_id, nof_drbs, ue_qos, true, false);
      add_directional_service_load(loads, layer.service_uplink_resource_beam_id, nof_drbs, ue_qos, false, true);
    } else {
      add_directional_service_load(loads, layer.service_digital_beam_id, nof_drbs, ue_qos, true, true);
    }
  }

  std::vector<ntn_beam_load> result;
  result.reserve(loads.size());
  for (const auto& entry : loads) {
    result.push_back(entry.second);
  }
  return result;
}

std::vector<ntn_served_beam_demand> cu_cp_impl::build_ntn_served_beam_demands_for_current_service_contexts()
{
  std::vector<ntn_beam_load> loads = build_ntn_beam_loads_for_current_service_contexts();
  merge_pending_ntn_connected_handover_loads(loads);

  std::vector<ntn_served_beam_demand> demands;
  demands.reserve(loads.size());
  for (const ntn_beam_load& load : loads) {
    if (load.beam_id.empty()) {
      continue;
    }
    ntn_served_beam_demand demand;
    demand.beam_id  = load.beam_id;
    demand.nof_ues  = load.nof_ues;
    demand.nof_drbs = load.nof_drbs;
    demand.qos      = load.qos;
    demands.push_back(demand);
  }
  return demands;
}

static bool assignment_is_active_loaded_for_beam(const ntn_beam_placement_plan& plan, const std::string& beam_id)
{
  return std::any_of(plan.assignments.begin(), plan.assignments.end(), [&beam_id](const ntn_beam_du_assignment& entry) {
    return entry.beam_id == beam_id && entry.state == ntn_beam_assignment_state::active_loaded;
  });
}

static const ntn_beam_du_assignment* find_assignment_for_beam(const ntn_beam_placement_plan& plan,
                                                              const std::string&             beam_id)
{
  auto it = std::find_if(plan.assignments.begin(), plan.assignments.end(), [&beam_id](const ntn_beam_du_assignment& entry) {
    return entry.beam_id == beam_id;
  });
  return it != plan.assignments.end() ? &*it : nullptr;
}

static bool has_higher_priority_active_loaded_service(const ntn_beam_placement_plan&    plan,
                                                      const ntn_qos_demand_summary& pending_qos)
{
  return std::any_of(plan.assignments.begin(), plan.assignments.end(), [&pending_qos](const ntn_beam_du_assignment& entry) {
    return entry.state == ntn_beam_assignment_state::active_loaded &&
           is_ntn_qos_demand_higher_priority(entry.qos, pending_qos);
  });
}

static bool is_headroom_exempt_ntn_qos_demand(const ntn_qos_demand_summary& pending_qos)
{
  return pending_qos.has_delay_critical_gbr || pending_qos.has_gbr || pending_qos.may_trigger_preemption ||
         pending_qos.best_arp_priority <= 3;
}

static std::string make_ntn_analog_pair_cooldown_key(std::string lhs, std::string rhs)
{
  if (lhs.empty()) {
    lhs = "none";
  }
  if (rhs.empty()) {
    rhs = "none";
  }
  if (rhs < lhs) {
    std::swap(lhs, rhs);
  }
  return lhs + "->" + rhs;
}

static unsigned count_active_loaded_ntn_beams(const ntn_beam_placement_plan& plan)
{
  return std::count_if(plan.assignments.begin(), plan.assignments.end(), [](const ntn_beam_du_assignment& entry) {
    return entry.state == ntn_beam_assignment_state::active_loaded;
  });
}

static std::optional<f1ap_ntn_ul_slot_resource_request>
make_ntn_ul_slot_request_for_nci(nr_cell_identity              serving_nci,
                                 du_index_t                    ue_du_index,
                                 const ntn_beam_placement_plan& current_plan)
{
  for (const ntn_beam_du_assignment& assignment : current_plan.assignments) {
    // Match both serving NCI and current DU because one plan can contain many beams and DU assignments.
    if (assignment.nci != serving_nci || assignment.du_index != ue_du_index) {
      continue;
    }
    // candidate/inactive beams do not serve the UE yet. A draining beam may still carry live UEs and must keep valid
    // SR/SRS resources until the demand is gone.
    if (assignment.state != ntn_beam_assignment_state::active_loaded &&
        assignment.state != ntn_beam_assignment_state::draining) {
      continue;
    }
    if (!assignment.uplink_ready) {
      continue;
    }

    f1ap_ntn_ul_slot_resource_request request;
    // A zero period means the planner did not allocate that resource kind for this DU/beam.
    if (assignment.sr_slot_period != 0) {
      request.sr_slot_offset = assignment.sr_slot_offset;
      request.sr_slot_period = assignment.sr_slot_period;
    }
    // SRS is optional and independent from SR, so include it only when the planner assigned a valid period.
    if (assignment.srs_slot_period != 0) {
      request.srs_slot_offset = assignment.srs_slot_offset;
      request.srs_slot_period = assignment.srs_slot_period;
    }
    // Return only requests that carry actual resources.
    if (!is_empty(request)) {
      return request;
    }
  }

  return std::nullopt;
}

static std::optional<f1ap_ntn_ul_slot_resource_request>
make_ntn_ul_slot_request_for_ue_context_setup(du_processor_repository&        du_db,
                                              cu_cp_ue&                       ue,
                                              const ntn_beam_placement_plan& current_plan)
{
  // Resolve the UE serving NCI first. Without it, CU-CP cannot match the UE to an NTN beam assignment and therefore
  // cannot build the UL slot resource request sent to the DU.
  const std::optional<nr_cell_identity> serving_nci = get_ue_serving_nci_for_ntn_load(du_db, ue);
  if (!serving_nci.has_value()) {
    return std::nullopt;
  }

  return make_ntn_ul_slot_request_for_nci(serving_nci.value(), ue.get_du_index(), current_plan);
}

void cu_cp_impl::schedule_ntn_ul_slot_updates_for_online_ues()
{
  std::set<ue_index_t> live_ues;
  for (const cu_cp_ue* ue : ue_mng.get_ues()) {
    if (ue != nullptr) {
      live_ues.insert(ue->get_ue_index());
    }
  }
  ntn_service_resource_mng.remove_missing_ues(live_ues);

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
            ntn_service_resource_mng.restore_or_clear_failed_slot_update(
                ue_index, slot_request, restore_request_on_failure);
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
            ntn_service_resource_mng.restore_or_clear_failed_slot_update(
                ue_index, slot_request, restore_request_on_failure);
            CORO_EARLY_RETURN();
          }
          if (!ue_context_mod_response.ntn_ul_slot_result.has_value()) {
            logger.warning("ue={}: DU did not return NTN SR/SRS slot update result", ue_index);
            f1ap_ntn_ul_slot_resource_result result;
            result.accepted = false;
            result.reason   = f1ap_ntn_ul_slot_resource_result_reason::malformed_request;
            ntn_service_resource_mng.mark_slot_update_result(
                ue_index, slot_request, restore_request_on_failure, result);
            CORO_EARLY_RETURN();
          }
          if (!ue_context_mod_response.ntn_ul_slot_result->accepted) {
            logger.warning("ue={}: DU rejected NTN SR/SRS slot update. reason={}",
                           ue_index,
                           to_string(ue_context_mod_response.ntn_ul_slot_result->reason));
            ntn_service_resource_mng.mark_slot_update_result(
                ue_index, slot_request, restore_request_on_failure, *ue_context_mod_response.ntn_ul_slot_result);
            CORO_EARLY_RETURN();
          }

          current_ue = ue_mng.find_du_ue(ue_index);
          if (current_ue == nullptr || current_ue->get_rrc_ue() == nullptr) {
            ntn_service_resource_mng.restore_or_clear_failed_slot_update(
                ue_index, slot_request, restore_request_on_failure);
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
              ntn_service_resource_mng.restore_or_clear_failed_slot_update(
                  ue_index, slot_request, restore_request_on_failure);
              CORO_EARLY_RETURN();
            }
          }

          ntn_service_resource_mng.mark_slot_update_result(
              ue_index, slot_request, restore_request_on_failure, *ue_context_mod_response.ntn_ul_slot_result);

          CORO_RETURN();
        }));
      };

  auto schedule_slot_decision = [&](cu_cp_ue& ue, const ntn_slot_resource_update_decision& decision) {
    if (decision.action == ntn_slot_resource_update_action::none) {
      return;
    }
    const f1ap_ntn_ul_slot_resource_request request_to_send =
        decision.request.value_or(f1ap_ntn_ul_slot_resource_request{});
    if (schedule_slot_update(ue, request_to_send, decision.previous_request)) {
      ntn_service_resource_mng.mark_slot_update_sent_to_du(ue.get_ue_index(), request_to_send);
    } else {
      ntn_service_resource_mng.restore_or_clear_failed_slot_update(
          ue.get_ue_index(), request_to_send, decision.previous_request);
      logger.warning("ue={}: Failed to schedule NTN SR/SRS slot {}", ue.get_ue_index(), decision.reason);
    }
  };

  for (cu_cp_ue* ue : ue_mng.get_ues()) {
    if (ue == nullptr) {
      continue;
    }

    const ue_index_t ue_index = ue->get_ue_index();
    if (ue->get_du_index() == du_index_t::invalid || ue->get_pcell_index() == srs_cu_cp::du_cell_index_t::invalid) {
      ntn_service_resource_mng.clear_digital_service_slot_intent(ue_index, "invalid_du_context");
      continue;
    }
    if (ue->get_ue_context().reconfiguration_disabled || ue->get_rrc_ue() == nullptr) {
      ntn_service_resource_mng.clear_digital_service_slot_intent(ue_index, "reconfiguration_unavailable");
      continue;
    }
    const auto connected_ho_it = ntn_connected_handover_states.find(ue_index);
    if (connected_ho_it != ntn_connected_handover_states.end() &&
        (connected_ho_it->second.state == "target_preloading" ||
         connected_ho_it->second.state == "target_resource_preparing" ||
         connected_ho_it->second.state == "target_resource_applied" ||
         connected_ho_it->second.state == "handover_preparing")) {
      continue;
    }
    if (ue->get_cu_up_index() == cu_up_index_t::invalid) {
      schedule_slot_decision(*ue, ntn_service_resource_mng.clear_digital_service_slot_intent(ue_index, "no_cu_up"));
      continue;
    }

    ntn_slot_resource_update_decision slot_decision;
    const auto layer_it = ntn_ue_layer_states.find(ue_index);
    if (layer_it != ntn_ue_layer_states.end() && layer_it->second.has_service_nci &&
        (layer_it->second.service_state == ntn_service_state_binding_pending ||
         layer_it->second.service_state == ntn_service_state_service_bound)) {
      ntn_digital_service_slot_intent_update update;
      update.ue_index         = ue_index;
      update.digital_beam_id  = layer_it->second.service_digital_beam_id;
      update.service_du_index = ue->get_du_index();
      update.service_nci      = layer_it->second.service_nci;
      update.has_service_nci  = true;
      if (const du_cell_configuration* service_cell =
              find_du_cell_by_nci(du_db, update.service_du_index, update.service_nci);
          service_cell != nullptr) {
        update.service_cell_index = to_f1ap_du_cell_index(service_cell->cell_index);
        update.service_pci        = service_cell->pci;
      }
      if (!layer_it->second.service_uplink_resource_beam_id.empty() &&
          layer_it->second.has_service_uplink_resource_nci) {
        update.uplink_resource_beam_id  = layer_it->second.service_uplink_resource_beam_id;
        update.uplink_resource_du_index = layer_it->second.service_uplink_resource_du_index;
        update.uplink_resource_nci      = layer_it->second.service_uplink_resource_nci;
        update.has_uplink_resource_nci  = true;
        if (const du_cell_configuration* uplink_cell =
                find_du_cell_by_nci(du_db, update.uplink_resource_du_index, update.uplink_resource_nci);
            uplink_cell != nullptr) {
          update.uplink_resource_cell_index = to_f1ap_du_cell_index(uplink_cell->cell_index);
          update.uplink_resource_pci        = uplink_cell->pci;
        }
      }
      update.service_state    = layer_it->second.service_state;
      slot_decision = ntn_service_resource_mng.update_digital_service_slot_intent(update, current_ntn_beam_placement_plan);
    } else if (cfg.mobility.meas_manager_config.ntn_location_mobility.enabled &&
               !cfg.mobility.meas_manager_config.ntn_location_mobility.analog_beams.empty() &&
               layer_it != ntn_ue_layer_states.end()) {
      slot_decision = ntn_service_resource_mng.clear_digital_service_slot_intent(ue_index, "no_digital_service");
    } else {
      std::optional<f1ap_ntn_ul_slot_resource_request> slot_request =
          make_ntn_ul_slot_request_for_ue_context_setup(du_db, *ue, current_ntn_beam_placement_plan);
      slot_decision = slot_request.has_value()
                          ? ntn_service_resource_mng.set_digital_slot_intent_from_request(
                                ue_index, *slot_request, "legacy_ntn_slot")
                          : ntn_service_resource_mng.clear_digital_service_slot_intent(ue_index, "no_slot_resources");
    }
    schedule_slot_decision(*ue, slot_decision);
  }
}

std::vector<rnti_t> cu_cp_impl::allocate_ntn_rnti_leases(unsigned nof_leases)
{
  std::vector<rnti_t> leases;
  leases.reserve(nof_leases);
  while (leases.size() != nof_leases) {
    if (next_ntn_rnti_lease_value > ntn_rnti_lease_max_value) {
      next_ntn_rnti_lease_value = ntn_rnti_lease_min_value;
    }
    const rnti_t rnti = to_rnti(next_ntn_rnti_lease_value++);
    if (is_crnti(rnti)) {
      leases.push_back(rnti);
    }
  }
  return leases;
}

void cu_cp_impl::schedule_ntn_rnti_lease_pool_updates_for_access_beams()
{
  const auto& ntn_cfg = cfg.mobility.meas_manager_config.ntn_location_mobility;
  if (!ntn_cfg.enabled || ntn_cfg.analog_beams.empty() || current_ntn_beam_placement_plan.analog_assignments.empty()) {
    return;
  }

  for (const ntn_analog_access_du_assignment& analog_assignment :
       current_ntn_beam_placement_plan.analog_assignments) {
    if (analog_assignment.selected_access_du_index == du_index_t::invalid ||
        analog_assignment.reason != "eligible") {
      continue;
    }

    const ntn_analog_beam_position* analog =
        find_ntn_analog_beam_cfg(ntn_cfg.analog_beams, analog_assignment.analog_beam_id);
    if (analog == nullptr || !analog->enabled || !analog->uplink_enabled) {
      continue;
    }

    struct access_lease_target {
      du_index_t                   du_index = du_index_t::invalid;
      const du_cell_configuration* cell     = nullptr;
    };
    std::vector<access_lease_target>                         targets;
    std::set<std::pair<du_index_t, srs_cu_cp::du_cell_index_t>> target_keys;
    for (du_index_t du_index : du_db.get_du_processor_indexes()) {
      for (const std::string& child_beam_id : analog->child_digital_beam_ids) {
        const ntn_beam_position* child_beam = find_ntn_beam_cfg(ntn_cfg.beams, child_beam_id);
        if (child_beam == nullptr || !child_beam->enabled || !child_beam->uplink_enabled) {
          continue;
        }
        const du_cell_configuration* cell = find_du_cell_by_nci(du_db, du_index, child_beam->nci);
        if (cell == nullptr) {
          continue;
        }
        if (target_keys.insert({du_index, cell->cell_index}).second) {
          targets.push_back({du_index, cell});
        }
      }
    }
    if (targets.empty()) {
      logger.warning("NTN analog beam={} has no DU cell for RNTI lease pool. Cause: no_supported_child",
                     analog_assignment.analog_beam_id);
      continue;
    }

    for (const access_lease_target& target : targets) {
      const srsran::du_cell_index_t cell_index = to_f1ap_du_cell_index(target.cell->cell_index);
      if (ntn_service_resource_mng.has_unresolved_rnti_lease_pool(target.du_index,
                                                                  cell_index,
                                                                  target.cell->pci,
                                                                  analog_assignment.analog_beam_id) ||
          !ntn_service_resource_mng.rnti_pool_below_low_watermark(target.du_index,
                                                                  cell_index,
                                                                  target.cell->pci,
                                                                  analog_assignment.analog_beam_id,
                                                                  ntn_rnti_lease_pool_low_watermark)) {
        continue;
      }

      ntn_rnti_lease_pool_update local_update;
      local_update.du_index       = target.du_index;
      local_update.cell_index     = cell_index;
      local_update.pci            = target.cell->pci;
      local_update.analog_beam_id = analog_assignment.analog_beam_id;
      local_update.generation_id  = next_ntn_rnti_lease_generation_id++;
      local_update.leases         = allocate_ntn_rnti_leases(ntn_rnti_lease_pool_size);

      const ntn_rnti_lease_pool_update_result reserve_result =
          ntn_service_resource_mng.reserve_rnti_leases(local_update);
      if (!reserve_result.accepted) {
        logger.warning("NTN analog beam={} failed to reserve RNTI lease pool. Cause: {}",
                       analog_assignment.analog_beam_id,
                       reserve_result.reason);
        continue;
      }

      du_processor* processor = du_db.find_du_processor(target.du_index);
      if (processor == nullptr || processor->get_context() == nullptr) {
        logger.warning("NTN analog beam={} failed to send RNTI lease pool. Cause: du_unavailable",
                       analog_assignment.analog_beam_id);
        f1ap_ntn_rnti_lease_pool_result rejected_result;
        rejected_result.generation_id   = local_update.generation_id;
        rejected_result.accepted        = false;
        rejected_result.reject_reason   = "du_unavailable";
        rejected_result.rejected_leases = local_update.leases;
        ntn_service_resource_mng.mark_rnti_lease_pool_distribution_result(local_update, rejected_result);
        continue;
      }

      f1ap_gnb_du_resource_coordination_request request;
      request.ntn_rnti_lease_update.gnb_du_id      = processor->get_context()->id;
      request.ntn_rnti_lease_update.du_index       = local_update.du_index;
      request.ntn_rnti_lease_update.cell_index     = local_update.cell_index;
      request.ntn_rnti_lease_update.cell_cgi       = target.cell->cgi;
      request.ntn_rnti_lease_update.pci            = local_update.pci;
      request.ntn_rnti_lease_update.analog_beam_id = local_update.analog_beam_id;
      request.ntn_rnti_lease_update.generation_id  = local_update.generation_id;
      request.ntn_rnti_lease_update.expiry_ms      = ntn_rnti_lease_expiry_ms;
      request.ntn_rnti_lease_update.operation      = f1ap_ntn_rnti_lease_pool_operation::add;
      request.ntn_rnti_lease_update.leases         = local_update.leases;

      ntn_service_resource_mng.mark_rnti_lease_pool_sent_to_du(local_update);

      async_task<f1ap_gnb_du_resource_coordination_response> f1ap_task =
          processor->get_f1ap_handler().handle_gnb_du_resource_coordination_request(request);
      auto completion_task =
          [this, local_update, f1ap_task = std::move(f1ap_task)](
              coro_context<async_task<void>>& ctx) mutable {
            f1ap_gnb_du_resource_coordination_response response;
            CORO_BEGIN(ctx);
            CORO_AWAIT_VALUE(response, f1ap_task);
            if (response.result.has_value()) {
              if (!ntn_service_resource_mng.mark_rnti_lease_pool_distribution_result(local_update,
                                                                                       *response.result)) {
                ntn_service_resource_mng.mark_rnti_lease_pool_ack_unknown(local_update, "invalid_du_result");
              }
            } else {
              ntn_service_resource_mng.mark_rnti_lease_pool_ack_unknown(local_update, "du_response_missing");
            }
            CORO_RETURN();
          };
      if (!common_task_sched.schedule_async_task(launch_async(std::move(completion_task)))) {
        ntn_service_resource_mng.mark_rnti_lease_pool_ack_unknown(local_update, "task_scheduler_unavailable");
      }
    }
  }
}

static uint32_t compute_ntn_sib19_packed_hash(const byte_buffer& pdu)
{
  uint32_t hash = 2166136261U;
  for (unsigned i = 0; i != pdu.length(); ++i) {
    hash ^= static_cast<uint32_t>(pdu[i]);
    hash *= 16777619U;
  }
  return hash;
}

void cu_cp_impl::schedule_ntn_sib19_broadcast_updates()
{
  const auto& ntn_cfg = cfg.mobility.meas_manager_config.ntn_location_mobility;
  if (stopped.load(std::memory_order_relaxed) || !ntn_cfg.enabled || current_ntn_beam_placement_plan.assignments.empty()) {
    return;
  }

  const ntn_sib19_broadcast_snapshot snapshot =
      build_current_ntn_sib19_broadcast_snapshot(std::chrono::steady_clock::now());
  for (const ntn_sib19_broadcast_entry& entry : snapshot.entries) {
    const ntn_beam_du_assignment* assignment = find_assignment_for_beam(current_ntn_beam_placement_plan, entry.beam_id);
    ntn_sib19_broadcast_record& record       = ntn_sib19_broadcast_records[entry.beam_id];
    record.beam_id                           = entry.beam_id;
    record.nci                               = entry.nci;
    record.packed_sib19_bytes                = entry.packed_sib19.length();
    const uint32_t packed_sib19_hash         = compute_ntn_sib19_packed_hash(entry.packed_sib19);

    const bool downlink_update_allowed = assignment != nullptr && assignment->downlink_ready;
    bool send_update = entry.state == ntn_sib19_broadcast_state::desired && downlink_update_allowed;
    bool send_clear  = entry.state == ntn_sib19_broadcast_state::clear_desired;
    if (entry.state == ntn_sib19_broadcast_state::desired && !downlink_update_allowed) {
      const bool du_may_have_dynamic_sib19 =
          record.state == ntn_sib19_broadcast_state::desired ||
          record.state == ntn_sib19_broadcast_state::sent_to_du ||
          record.state == ntn_sib19_broadcast_state::applied_by_du ||
          record.state == ntn_sib19_broadcast_state::rejected_by_du ||
          record.state == ntn_sib19_broadcast_state::clear_sent;
      send_clear = du_may_have_dynamic_sib19;
      if (!send_clear) {
        record.state              = ntn_sib19_broadcast_state::stale_blocked;
        record.reason             = "downlink_unavailable";
        record.packed_sib19_bytes = 0;
        record.packed_sib19_hash  = 0;
        continue;
      }
    }
    if (entry.state == ntn_sib19_broadcast_state::stale_blocked) {
      const bool du_may_have_dynamic_sib19 =
          record.state == ntn_sib19_broadcast_state::desired ||
          record.state == ntn_sib19_broadcast_state::sent_to_du ||
          record.state == ntn_sib19_broadcast_state::applied_by_du ||
          record.state == ntn_sib19_broadcast_state::rejected_by_du ||
          record.state == ntn_sib19_broadcast_state::clear_sent;
      send_clear = du_may_have_dynamic_sib19;
    }

    if (send_update &&
        (record.state == ntn_sib19_broadcast_state::sent_to_du ||
         record.state == ntn_sib19_broadcast_state::applied_by_du) &&
        record.packed_sib19_bytes == entry.packed_sib19.length() &&
        record.packed_sib19_hash == packed_sib19_hash) {
      record.reason = record.state == ntn_sib19_broadcast_state::applied_by_du ? "applied" : record.reason;
      continue;
    }
    if (send_clear && (record.state == ntn_sib19_broadcast_state::clear_sent ||
                       record.state == ntn_sib19_broadcast_state::cleared_by_du)) {
      record.reason = record.state == ntn_sib19_broadcast_state::cleared_by_du ? "cleared" : record.reason;
      continue;
    }

    if (!send_update && !send_clear) {
      record.state  = entry.state;
      record.reason = entry.reason;
      record.packed_sib19_hash = packed_sib19_hash;
      continue;
    }

    if (assignment == nullptr || assignment->du_index == du_index_t::invalid) {
      record.state  = ntn_sib19_broadcast_state::rejected_by_du;
      record.reason = "du_unavailable";
      continue;
    }

    const du_cell_configuration* cell = find_du_cell_by_nci(du_db, assignment->du_index, entry.nci);
    if (cell == nullptr) {
      record.state  = ntn_sib19_broadcast_state::rejected_by_du;
      record.reason = "cell_unavailable";
      continue;
    }

    du_processor* processor = du_db.find_du_processor(assignment->du_index);
    if (processor == nullptr || processor->get_context() == nullptr) {
      record.state  = ntn_sib19_broadcast_state::rejected_by_du;
      record.reason = "du_unavailable";
      continue;
    }

    const uint32_t generation = next_ntn_sib19_broadcast_generation_id++;
    record.generation_id     = generation;
    record.state             = send_clear ? ntn_sib19_broadcast_state::clear_sent
                                          : ntn_sib19_broadcast_state::sent_to_du;
    record.reason            = send_clear ? entry.reason : "gnb_du_resource_coordination_request";
    record.packed_sib19_hash = send_clear ? 0U : packed_sib19_hash;

    f1ap_gnb_du_resource_coordination_request request;
    request.ntn_sib19_broadcast_update.du_index      = assignment->du_index;
    request.ntn_sib19_broadcast_update.cell_index    = to_f1ap_du_cell_index(cell->cell_index);
    request.ntn_sib19_broadcast_update.pci           = cell->pci;
    request.ntn_sib19_broadcast_update.beam_id       = entry.beam_id;
    request.ntn_sib19_broadcast_update.nci           = entry.nci;
    request.ntn_sib19_broadcast_update.generation_id = generation;
    request.ntn_sib19_broadcast_update.operation =
        send_clear ? f1ap_ntn_sib19_broadcast_operation::clear : f1ap_ntn_sib19_broadcast_operation::update;
    request.ntn_sib19_broadcast_update.si_msg_idx    = 0;
    request.ntn_sib19_broadcast_update.sib_idx       = 19;
    request.ntn_sib19_broadcast_update.packed_sib19  = entry.packed_sib19.copy();

    async_task<f1ap_gnb_du_resource_coordination_response> f1ap_task =
        processor->get_f1ap_handler().handle_gnb_du_resource_coordination_request(request);
    auto completion_task =
        [this, beam_id = entry.beam_id, generation, send_clear, f1ap_task = std::move(f1ap_task)](
            coro_context<async_task<void>>& ctx) mutable {
          f1ap_gnb_du_resource_coordination_response response;
          CORO_BEGIN(ctx);
          CORO_AWAIT_VALUE(response, f1ap_task);
          auto completion_it = ntn_sib19_broadcast_records.find(beam_id);
          const ntn_sib19_broadcast_state expected_in_flight_state =
              send_clear ? ntn_sib19_broadcast_state::clear_sent : ntn_sib19_broadcast_state::sent_to_du;
          if (completion_it == ntn_sib19_broadcast_records.end() ||
              completion_it->second.generation_id != generation ||
              completion_it->second.state != expected_in_flight_state) {
            CORO_EARLY_RETURN();
          }
          ntn_sib19_broadcast_record& completion_record = completion_it->second;
          const bool expected_status = response.sib19_result.has_value() &&
                                       ((!send_clear && response.sib19_result->status ==
                                                            f1ap_ntn_sib19_broadcast_result_status::applied) ||
                                        (send_clear && response.sib19_result->status ==
                                                           f1ap_ntn_sib19_broadcast_result_status::clear_applied));
          if (response.success && expected_status) {
            completion_record.state = send_clear ? ntn_sib19_broadcast_state::cleared_by_du
                                                 : ntn_sib19_broadcast_state::applied_by_du;
            completion_record.reason = send_clear ? "cleared" : "applied";
          } else {
            completion_record.state  = ntn_sib19_broadcast_state::rejected_by_du;
            completion_record.reason =
                response.sib19_result.has_value() && !response.sib19_result->reject_reason.empty()
                    ? response.sib19_result->reject_reason
                    : (response.sib19_result.has_value() ? "unexpected_sib19_result_status" : "du_reject");
          }
          CORO_RETURN();
        };
    if (!common_task_sched.schedule_async_task(launch_async(std::move(completion_task)))) {
      record.state  = ntn_sib19_broadcast_state::rejected_by_du;
      record.reason = "task_scheduler_unavailable";
    }
  }
}

static bool is_ntn_service_pair_resource_repair(const ntn_resource_repair& repair)
{
  return !repair.uplink_resource_beam_id.empty() || repair.has_uplink_resource_nci;
}

void cu_cp_impl::mark_ntn_service_pair_resource_repair_skipped(const ntn_resource_repair& repair, const char* reason)
{
  if (is_ntn_service_pair_resource_repair(repair)) {
    ++nof_ntn_service_pair_resource_audit_skipped;
    last_ntn_service_pair_resource_audit_reason = reason;
  }
  ntn_service_resource_mng.mark_resource_repair_result(repair, false, reason);
}

void cu_cp_impl::handle_ntn_resource_audit_decision(const ntn_resource_audit_decision& decision)
{
  nof_ntn_resource_audit_mismatches += decision.nof_mismatches;
  nof_ntn_resource_audit_repair_actions += decision.nof_repairs;
  for (const ntn_resource_repair& repair : decision.repairs) {
    const bool service_pair_repair = is_ntn_service_pair_resource_repair(repair);
    if (service_pair_repair) {
      ++nof_ntn_service_pair_resource_audit_mismatches;
      ++nof_ntn_service_pair_resource_audit_repairs;
      last_ntn_service_pair_resource_audit_reason = repair.reason;
    }
    const ntn_resource_repair_record repair_record =
        ntn_service_resource_mng.queue_resource_repair(repair, decision.generation_id);
    logger.warning("NTN resource audit generation={} repair_action={} du={} cell={} ue={} reason={}",
                   decision.generation_id,
                   static_cast<unsigned>(repair.action),
                   fmt::underlying(repair.du_index),
                   static_cast<unsigned>(repair.cell_index),
                   ue_index_to_uint(repair.ue_index),
                   repair.reason);
    if (repair_record.state != "queued") {
      if (service_pair_repair) {
        ++nof_ntn_service_pair_resource_audit_skipped;
        last_ntn_service_pair_resource_audit_reason = repair_record.reason;
      }
      continue;
    }

    if (repair.action == ntn_resource_repair_action::resend_rnti_lease_pool) {
      if (repair.du_index == du_index_t::invalid || repair.cell_index == srsran::INVALID_DU_CELL_INDEX ||
          repair.pci == INVALID_PCI || repair.analog_beam_id.empty() || repair.rnti_lease_generation_id == 0 ||
          repair.rnti_leases.empty()) {
        ntn_service_resource_mng.mark_resource_repair_result(repair, false, "invalid_rnti_repair");
        continue;
      }

      du_processor* processor = du_db.find_du_processor(repair.du_index);
      if (processor == nullptr || processor->get_context() == nullptr) {
        ntn_service_resource_mng.mark_resource_repair_result(repair, false, "du_unavailable");
        continue;
      }

      const du_cell_configuration* cell = nullptr;
      for (const du_cell_configuration& candidate : processor->get_context()->served_cells) {
        if (to_f1ap_du_cell_index(candidate.cell_index) == repair.cell_index && candidate.pci == repair.pci) {
          cell = &candidate;
          break;
        }
      }
      if (cell == nullptr) {
        ntn_service_resource_mng.mark_resource_repair_result(repair, false, "cell_unavailable");
        continue;
      }

      ntn_rnti_lease_pool_update local_update;
      local_update.du_index       = repair.du_index;
      local_update.cell_index     = repair.cell_index;
      local_update.pci            = repair.pci;
      local_update.analog_beam_id = repair.analog_beam_id;
      local_update.generation_id  = repair.rnti_lease_generation_id;
      local_update.leases         = repair.rnti_leases;

      f1ap_gnb_du_resource_coordination_request request;
      request.ntn_rnti_lease_update.gnb_du_id      = processor->get_context()->id;
      request.ntn_rnti_lease_update.du_index       = local_update.du_index;
      request.ntn_rnti_lease_update.cell_index     = local_update.cell_index;
      request.ntn_rnti_lease_update.cell_cgi       = cell->cgi;
      request.ntn_rnti_lease_update.pci            = local_update.pci;
      request.ntn_rnti_lease_update.analog_beam_id = local_update.analog_beam_id;
      request.ntn_rnti_lease_update.generation_id  = local_update.generation_id;
      request.ntn_rnti_lease_update.expiry_ms      = ntn_rnti_lease_expiry_ms;
      request.ntn_rnti_lease_update.operation      = f1ap_ntn_rnti_lease_pool_operation::add;
      request.ntn_rnti_lease_update.leases         = local_update.leases;

      ntn_service_resource_mng.mark_rnti_lease_pool_sent_to_du(local_update);
      ntn_service_resource_mng.mark_resource_repair_sent(repair);

      async_task<f1ap_gnb_du_resource_coordination_response> f1ap_task =
          processor->get_f1ap_handler().handle_gnb_du_resource_coordination_request(request);
      auto completion_task =
          [this, repair, local_update, f1ap_task = std::move(f1ap_task)](coro_context<async_task<void>>& ctx) mutable {
            f1ap_gnb_du_resource_coordination_response response;
            CORO_BEGIN(ctx);
            CORO_AWAIT_VALUE(response, f1ap_task);
            if (!response.result.has_value()) {
              ntn_service_resource_mng.mark_rnti_lease_pool_ack_unknown(local_update, "du_response_missing");
              ntn_service_resource_mng.mark_resource_repair_result(repair, false, "du_response_missing");
              CORO_EARLY_RETURN();
            }
            const bool result_applied =
                ntn_service_resource_mng.mark_rnti_lease_pool_distribution_result(local_update, *response.result);
            if (!result_applied) {
              ntn_service_resource_mng.mark_rnti_lease_pool_ack_unknown(local_update, "invalid_du_result");
            }
            ntn_service_resource_mng.mark_resource_repair_result(
                repair,
                result_applied && response.result->accepted && response.result->rejected_leases.empty(),
                !result_applied ? "invalid_du_result" :
                                  (response.result->reject_reason.empty() ? "du_ack" : response.result->reject_reason));
            CORO_RETURN();
          };
      if (!common_task_sched.schedule_async_task(launch_async(std::move(completion_task)))) {
        ntn_service_resource_mng.mark_rnti_lease_pool_ack_unknown(local_update, "task_scheduler_unavailable");
        ntn_service_resource_mng.mark_resource_repair_result(repair, false, "task_scheduler_unavailable");
      }
      continue;
    }

    if (repair.action == ntn_resource_repair_action::apply_sr_srs_assignment ||
        repair.action == ntn_resource_repair_action::resend_sr_srs_clear ||
        repair.action == ntn_resource_repair_action::clear_unknown_sr_srs_assignment) {
      const f1ap_ntn_ul_slot_resource_request slot_request =
          repair.slot_request.value_or(f1ap_ntn_ul_slot_resource_request{});
      if (repair.action == ntn_resource_repair_action::apply_sr_srs_assignment && is_empty(slot_request)) {
        mark_ntn_service_pair_resource_repair_skipped(repair, "missing_slot_request");
        continue;
      }

      cu_cp_ue* ue = ue_mng.find_du_ue(repair.ue_index);
      if (ue == nullptr) {
        mark_ntn_service_pair_resource_repair_skipped(repair, "ue_not_found");
        continue;
      }

      if (service_pair_repair &&
          (ntn_release_allowed_release_requested_ues.count(repair.ue_index) != 0 ||
           ntn_location_watchdog_release_requested_ues.count(repair.ue_index) != 0)) {
        mark_ntn_service_pair_resource_repair_skipped(repair, "release_pending");
        continue;
      }

      const auto connected_handover_it = ntn_connected_handover_states.find(repair.ue_index);
      if (service_pair_repair && connected_handover_it != ntn_connected_handover_states.end() &&
          (connected_handover_it->second.state == "target_preloading" ||
           connected_handover_it->second.state == "target_resource_preparing" ||
           connected_handover_it->second.state == "target_resource_applied" ||
           connected_handover_it->second.state == "handover_preparing")) {
        mark_ntn_service_pair_resource_repair_skipped(repair, "connected_handover_pending");
        continue;
      }

      if (service_pair_repair && repair.uplink_resource_du_index != du_index_t::invalid &&
          repair.uplink_resource_du_index != ue->get_du_index()) {
        mark_ntn_service_pair_resource_repair_skipped(repair, "cross_du_service_pair_repair_unsupported");
        continue;
      }

      ntn_service_resource_mng.mark_resource_repair_sent(repair);
      if (!ue->get_task_sched().schedule_async_task(
              launch_async([this, repair, slot_request](coro_context<async_task<void>>& ctx) mutable {
                cu_cp_ue*                              current_ue = nullptr;
                f1ap_ue_context_modification_request  ue_context_mod_request;
                f1ap_ue_context_modification_response ue_context_mod_response;
                rrc_reconfiguration_procedure_request  rrc_reconfig_args;
                rrc_recfg_v1530_ies                    non_crit_ext;
                bool                                   rrc_reconfig_result = false;

                CORO_BEGIN(ctx);

                current_ue = ue_mng.find_du_ue(repair.ue_index);
                if (current_ue == nullptr || current_ue->get_rrc_ue() == nullptr ||
                    current_ue->get_ue_context().reconfiguration_disabled) {
                  mark_ntn_service_pair_resource_repair_skipped(repair, "ue_reconfiguration_unavailable");
                  CORO_EARLY_RETURN();
                }

                ue_context_mod_request = {};
                ue_context_mod_request.ue_index            = repair.ue_index;
                ue_context_mod_request.ntn_ul_slot_request = slot_request;

                CORO_AWAIT_VALUE(ue_context_mod_response,
                                 du_db.get_du_processor(current_ue->get_du_index())
                                     .get_f1ap_handler()
                                     .handle_ue_context_modification_request(ue_context_mod_request));

                if (!ue_context_mod_response.success || !ue_context_mod_response.ntn_ul_slot_result.has_value()) {
                  mark_ntn_service_pair_resource_repair_skipped(repair, "du_response_missing");
                  CORO_EARLY_RETURN();
                }
                if (!ue_context_mod_response.ntn_ul_slot_result->accepted) {
                  const std::string reason = to_string(ue_context_mod_response.ntn_ul_slot_result->reason);
                  if (is_ntn_service_pair_resource_repair(repair)) {
                    ++nof_ntn_service_pair_resource_audit_skipped;
                    last_ntn_service_pair_resource_audit_reason = reason;
                  }
                  ntn_service_resource_mng.mark_resource_repair_result(repair, false, reason);
                  CORO_EARLY_RETURN();
                }

                current_ue = ue_mng.find_du_ue(repair.ue_index);
                if (current_ue == nullptr || current_ue->get_rrc_ue() == nullptr) {
                  mark_ntn_service_pair_resource_repair_skipped(repair, "ue_not_found");
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
                    mark_ntn_service_pair_resource_repair_skipped(repair, "rrc_reconfiguration_failed");
                    CORO_EARLY_RETURN();
                  }
                }

                ntn_service_resource_mng.mark_slot_update_applied(
                    repair.ue_index,
                    ue_context_mod_response.ntn_ul_slot_result->applied_request.value_or(slot_request));
                ntn_service_resource_mng.mark_resource_repair_result(
                    repair, true, to_string(ue_context_mod_response.ntn_ul_slot_result->reason));
                CORO_RETURN();
              }))) {
        mark_ntn_service_pair_resource_repair_skipped(repair, "task_scheduler_unavailable");
      }
      continue;
    }

    if (repair.action == ntn_resource_repair_action::rollback_handover_target_rnti) {
      ntn_service_resource_mng.rollback_handover_target_rnti(repair.ue_index, repair.reason);
      ntn_service_resource_mng.mark_resource_repair_result(repair, true, "rollback_restored");
      continue;
    }
  }
}

std::vector<cu_cp_impl::ntn_resource_audit_target> cu_cp_impl::collect_ntn_resource_audit_targets()
{
  std::vector<ntn_resource_audit_target>                 targets;
  std::map<std::tuple<du_index_t, srsran::du_cell_index_t, pci_t>, size_t> target_index_by_key;
  auto add_target = [&](du_index_t du_index, srsran::du_cell_index_t cell_index, pci_t pci, bool service_pair) {
    const auto key = std::make_tuple(du_index, cell_index, pci);
    const auto it  = target_index_by_key.find(key);
    if (it != target_index_by_key.end()) {
      targets[it->second].service_pair = targets[it->second].service_pair || service_pair;
      return;
    }
    target_index_by_key[key] = targets.size();
    targets.push_back({du_index, cell_index, pci, service_pair});
  };

  const ntn_beam_service_resource_snapshot snapshot = ntn_service_resource_mng.get_snapshot();
  for (const ntn_rnti_lease& lease : snapshot.rnti_leases) {
    if (lease.distribution_state != "sent_to_du" && lease.distribution_state != "applied_by_du") {
      continue;
    }
    add_target(lease.du_index, lease.cell_index, lease.pci, false);
  }
  for (const ntn_digital_slot_resource_intent& intent : snapshot.digital_slot_intents) {
    if (intent.service_du_index == du_index_t::invalid || !intent.has_service_nci) {
      continue;
    }
    const bool has_service_pair = !intent.uplink_resource_beam_id.empty() && intent.has_uplink_resource_nci &&
                                  intent.uplink_resource_du_index != du_index_t::invalid;
    du_index_t              target_du_index   = has_service_pair ? intent.uplink_resource_du_index : intent.service_du_index;
    srsran::du_cell_index_t target_cell_index =
        has_service_pair ? intent.uplink_resource_cell_index : intent.service_cell_index;
    pci_t target_pci = has_service_pair ? intent.uplink_resource_pci : intent.service_pci;
    const nr_cell_identity target_nci = has_service_pair ? intent.uplink_resource_nci : intent.service_nci;
    if (target_cell_index == srsran::INVALID_DU_CELL_INDEX || target_pci == INVALID_PCI) {
      const du_cell_configuration* cell = find_du_cell_by_nci(du_db, target_du_index, target_nci);
      if (cell == nullptr) {
        continue;
      }
      target_cell_index = to_f1ap_du_cell_index(cell->cell_index);
      target_pci        = cell->pci;
    }
    add_target(target_du_index, target_cell_index, target_pci, has_service_pair);
  }
  return targets;
}

void cu_cp_impl::schedule_ntn_resource_audits()
{
  const auto& ntn_cfg = cfg.mobility.meas_manager_config.ntn_location_mobility;
  if (!ntn_cfg.enabled) {
    return;
  }

  for (const ntn_resource_audit_target& target : collect_ntn_resource_audit_targets()) {
    du_processor* processor = du_db.find_du_processor(target.du_index);
    if (processor == nullptr || processor->get_context() == nullptr) {
      ++nof_ntn_resource_audit_failures;
      last_ntn_resource_audit_reason = "du_unavailable";
      continue;
    }

    f1ap_gnb_du_resource_coordination_request request;
    request.ntn_resource_audit_request.du_index      = target.du_index;
    request.ntn_resource_audit_request.cell_index    = target.cell_index;
    request.ntn_resource_audit_request.pci           = target.pci;
    request.ntn_resource_audit_request.generation_id = next_ntn_resource_audit_generation_id++;
    last_ntn_resource_audit_generation               = request.ntn_resource_audit_request.generation_id;
    ++nof_ntn_resource_audit_queries_sent;
    if (target.service_pair) {
      ++nof_ntn_service_pair_resource_audit_targets;
      last_ntn_service_pair_resource_audit_reason = "audit_requested";
    }

    async_task<f1ap_gnb_du_resource_coordination_response> f1ap_task =
        processor->get_f1ap_handler().handle_gnb_du_resource_coordination_request(request);
    auto completion_task =
        [this, audit_request = request.ntn_resource_audit_request, f1ap_task = std::move(f1ap_task)](
            coro_context<async_task<void>>& ctx) mutable {
          f1ap_gnb_du_resource_coordination_response response;
          ntn_resource_audit_report                  report;
          ntn_resource_audit_decision                decision;
          bool                                       rnti_domain_clean = false;
          CORO_BEGIN(ctx);
          CORO_AWAIT_VALUE(response, f1ap_task);
          // A present result is an explicit DU answer even when it rejects the audit. Route that rejection through
          // the authoritative manager so its reason becomes a conflict instead of being dropped as transport loss.
          if (!response.audit_result.has_value()) {
            ++nof_ntn_resource_audit_failures;
            last_ntn_resource_audit_reason = "du_response_missing";
            CORO_EARLY_RETURN();
          }
          if (response.audit_result->generation_id != audit_request.generation_id) {
            ++nof_ntn_resource_audit_failures;
            last_ntn_resource_audit_reason = "generation_mismatch";
            CORO_EARLY_RETURN();
          }

          report.du_index                  = audit_request.du_index;
          report.cell_index                = audit_request.cell_index;
          report.pci                       = audit_request.pci;
          report.generation_id             = response.audit_result->generation_id;
          report.accepted                  = response.audit_result->accepted;
          report.rnti_snapshot_complete    = response.audit_result->rnti_snapshot_complete;
          report.ue_slot_snapshot_complete = response.audit_result->ue_slot_snapshot_complete;
          report.reject_reason             = response.audit_result->reject_reason;
          last_ntn_resource_audit_reason =
              report.reject_reason.empty() ? (report.accepted ? "accepted" : "du_audit_rejected") : report.reject_reason;
          for (const f1ap_ntn_resource_audit_rnti_lease& lease : response.audit_result->rnti_leases) {
            report.rnti_leases.push_back({lease.rnti, lease.state, lease.distribution_state, lease.generation_id});
          }
          for (const f1ap_ntn_resource_audit_ue_slot& slot : response.audit_result->ue_slots) {
            report.ue_slots.push_back({slot.ue_index, slot.state, slot.request});
          }

          if (report.accepted) {
            ++nof_ntn_resource_audit_responses_accepted;
            if (!report.rnti_snapshot_complete) {
              ++nof_ntn_resource_audit_rnti_incomplete;
            }
            if (!report.ue_slot_snapshot_complete) {
              ++nof_ntn_resource_audit_ue_slot_incomplete;
            }
          } else {
            ++nof_ntn_resource_audit_failures;
          }
          decision = ntn_service_resource_mng.handle_resource_audit_report(report);
          handle_ntn_resource_audit_decision(decision);
          rnti_domain_clean = std::none_of(decision.repairs.begin(),
                                           decision.repairs.end(),
                                           [](const ntn_resource_repair& repair) {
                                             return repair.action == ntn_resource_repair_action::resend_rnti_lease_pool ||
                                                    repair.action == ntn_resource_repair_action::mark_resource_conflict;
                                           });
          if (report.accepted && report.rnti_snapshot_complete && rnti_domain_clean) {
            schedule_ntn_rnti_lease_pool_updates_for_access_beams();
          }
          CORO_RETURN();
        };
    if (!common_task_sched.schedule_async_task(launch_async(std::move(completion_task)))) {
      ++nof_ntn_resource_audit_failures;
      last_ntn_resource_audit_reason = "task_scheduler_unavailable";
    }
  }
}

void cu_cp_impl::on_ntn_resource_audit_timer_expired()
{
  schedule_ntn_resource_audits();

  ntn_resource_audit_timer.set(std::chrono::seconds{1},
                               [this](timer_id_t /*tid*/) { on_ntn_resource_audit_timer_expired(); });
  ntn_resource_audit_timer.run();
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

void cu_cp_impl::clear_ntn_predictive_service_window()
{
  current_ntn_predictive_window_valid = false;
  current_ntn_predictive_upcoming_beam_ids.clear();
  current_ntn_predictive_drain_soon_beam_ids.clear();
  current_ntn_predictive_beam_timeline.clear();
  current_ntn_predictive_horizon = std::chrono::milliseconds{0};
  current_ntn_predictive_lead_time = std::chrono::milliseconds{0};
  current_ntn_predictive_timeline_steps = 0;
  current_ntn_earliest_predictive_upcoming_offset.reset();
  current_ntn_earliest_predictive_drain_offset.reset();
  current_ntn_next_beam_satellite_ids.clear();
  current_ntn_next_window_satellite_ids.clear();
  current_ntn_satellite_owner_change_count = 0;
}

std::vector<ntn_served_beam_candidate> cu_cp_impl::build_ntn_predictive_service_window_candidates(
    const ntn_served_beam_schedule&          current_schedule,
    const std::vector<ntn_satellite_prediction_step>& future_steps,
    const std::vector<ntn_served_beam_demand>&        demands)
{
  clear_ntn_predictive_service_window();
  current_ntn_beam_satellite_ids             = map_candidate_satellite_ids(current_schedule.candidates);
  current_ntn_current_window_satellite_ids   = collect_window_satellite_ids(current_schedule.candidates);

  const auto& ntn_cfg = cfg.mobility.meas_manager_config.ntn_location_mobility;
  if (future_steps.empty() || !ntn_served_beam_sched.has_value() ||
      ntn_cfg.satellite_state_update.source == ntn_satellite_state_source::manual ||
      ntn_cfg.satellite_state_update.update_period.count() == 0) {
    return current_schedule.candidates;
  }

  const std::set<std::string> current_window_ids(current_schedule.beam_ids.begin(), current_schedule.beam_ids.end());

  current_ntn_predictive_window_valid = true;
  current_ntn_predictive_horizon =
      ntn_cfg.satellite_state_update.predictive_service_window_horizon.count() > 0
          ? ntn_cfg.satellite_state_update.predictive_service_window_horizon
          : ntn_cfg.satellite_state_update.update_period;
  current_ntn_predictive_lead_time =
      ntn_cfg.satellite_state_update.predictive_handover_lead_time.count() > 0
          ? ntn_cfg.satellite_state_update.predictive_handover_lead_time
          : ntn_cfg.satellite_state_update.update_period;
  current_ntn_predictive_timeline_steps = future_steps.size();

  std::map<std::string, bool> previous_window_presence;
  for (const std::string& beam_id : current_window_ids) {
    previous_window_presence[beam_id] = true;
  }
  std::map<std::string, ntn_served_beam_candidate> first_entry_candidates;
  std::optional<ntn_served_beam_schedule>          first_future_schedule;

  for (const ntn_satellite_prediction_step& step : future_steps) {
    if (step.offset.count() <= 0 || step.satellites.empty()) {
      continue;
    }

    const ntn_served_beam_schedule step_schedule = ntn_served_beam_sched->compute_schedule(step.satellites, demands);
    if (!first_future_schedule.has_value()) {
      first_future_schedule = step_schedule;
      current_ntn_next_beam_satellite_ids = map_candidate_satellite_ids(step_schedule.candidates);
      current_ntn_next_window_satellite_ids = collect_window_satellite_ids(step_schedule.candidates);
      current_ntn_satellite_owner_change_count =
          count_satellite_owner_changes(current_ntn_beam_satellite_ids, current_ntn_next_beam_satellite_ids);
    }

    const std::set<std::string> step_window_ids(step_schedule.beam_ids.begin(), step_schedule.beam_ids.end());
    std::set<std::string>       beam_ids_to_evaluate = step_window_ids;
    for (const auto& previous : previous_window_presence) {
      beam_ids_to_evaluate.insert(previous.first);
    }

    for (const std::string& beam_id : beam_ids_to_evaluate) {
      const bool was_in_window = previous_window_presence[beam_id];
      const bool is_in_window  = contains_beam_id(step_window_ids, beam_id);
      auto&      timeline      = current_ntn_predictive_beam_timeline[beam_id];
      if (!was_in_window && is_in_window && !timeline.first_entry_offset.has_value()) {
        timeline.first_entry_offset = step.offset;
        if (!current_ntn_earliest_predictive_upcoming_offset.has_value() ||
            step.offset < current_ntn_earliest_predictive_upcoming_offset.value()) {
          current_ntn_earliest_predictive_upcoming_offset = step.offset;
        }
        auto candidate_it =
            std::find_if(step_schedule.candidates.begin(),
                         step_schedule.candidates.end(),
                         [&beam_id](const ntn_served_beam_candidate& candidate) {
                           return candidate.beam_id == beam_id && candidate.in_hopping_window;
                         });
        if (candidate_it != step_schedule.candidates.end()) {
          first_entry_candidates.emplace(beam_id, *candidate_it);
        }
      }
      if (was_in_window && !is_in_window && !timeline.first_exit_offset.has_value()) {
        timeline.first_exit_offset = step.offset;
        if (!current_ntn_earliest_predictive_drain_offset.has_value() ||
            step.offset < current_ntn_earliest_predictive_drain_offset.value()) {
          current_ntn_earliest_predictive_drain_offset = step.offset;
        }
      }
      previous_window_presence[beam_id] = is_in_window;
    }
  }

  for (const auto& entry : current_ntn_predictive_beam_timeline) {
    if (entry.second.first_entry_offset.has_value()) {
      current_ntn_predictive_upcoming_beam_ids.insert(entry.first);
    }
    if (entry.second.first_exit_offset.has_value() &&
        entry.second.first_exit_offset.value() <= current_ntn_predictive_lead_time) {
      current_ntn_predictive_drain_soon_beam_ids.insert(entry.first);
    }
  }

  std::vector<ntn_served_beam_candidate> predictive_candidates;
  predictive_candidates.reserve(current_schedule.candidates.size() + current_ntn_predictive_upcoming_beam_ids.size());

  for (ntn_served_beam_candidate candidate : current_schedule.candidates) {
    if (contains_beam_id(current_ntn_predictive_drain_soon_beam_ids, candidate.beam_id)) {
      candidate.in_hopping_window = false;
      predictive_candidates.push_back(std::move(candidate));
      continue;
    }
    const auto next_it = first_future_schedule.has_value()
                             ? std::find_if(first_future_schedule->candidates.begin(),
                                            first_future_schedule->candidates.end(),
                                            [&candidate](const ntn_served_beam_candidate& next_candidate) {
                                              return next_candidate.beam_id == candidate.beam_id;
                                            })
                             : current_schedule.candidates.end();
    if (first_future_schedule.has_value() && next_it != first_future_schedule->candidates.end() &&
        next_it->in_hopping_window) {
      candidate.in_hopping_window = true;
      candidate.elevation_deg     = next_it->elevation_deg;
      candidate.serving_satellite_id = next_it->serving_satellite_id;
    }
    predictive_candidates.push_back(std::move(candidate));
  }

  for (const auto& entry : first_entry_candidates) {
    ntn_served_beam_candidate candidate = entry.second;
    if (!contains_beam_id(current_ntn_predictive_upcoming_beam_ids, candidate.beam_id) ||
        contains_candidate_beam_id(predictive_candidates, candidate.beam_id)) {
      continue;
    }
    predictive_candidates.push_back(std::move(candidate));
  }

  return predictive_candidates;
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

  ntn_service_resource_mng.set_authoritative_rnti_lease_validation_enabled(
      cfg.mobility.meas_manager_config.ntn_location_mobility.enabled);

  ntn_served_beam_sched = create_ntn_served_beam_scheduler(cfg.mobility.meas_manager_config);
  if (ntn_served_beam_sched.has_value()) {
    const auto& ntn_cfg = cfg.mobility.meas_manager_config.ntn_location_mobility;
    logger.debug("Enabled NTN served beam scheduler with {} digital beams, {} analog beams, min_elevation={}deg, "
                 "max_active_analog={}, max_loaded_digital={}",
                 ntn_cfg.beams.size(),
                 ntn_cfg.analog_beams.size(),
                 ntn_cfg.served_beam_min_elevation_deg,
                 ntn_cfg.max_nof_active_analog_access_beams,
                 get_ntn_loaded_digital_service_beam_cap(ntn_cfg));
  }

  nrppa_entity = create_nrppa_entity(cfg, nrppa_cu_cp_ev_notifier, common_task_sched);

  // Connect event notifiers to layers.
  ngap_cu_cp_ev_notifier.connect_cu_cp(get_cu_cp_ngap_handler(), paging_handler);
  nrppa_cu_cp_ev_notifier.connect_cu_cp(get_cu_cp_nrppa_handler());
  mobility_manager_ev_notifier.connect_cu_cp(get_cu_cp_mobility_manager_handler());
  e1ap_ev_notifier.connect_cu_cp(get_cu_cp_e1ap_handler());
  cell_meas_mobility_notifier.connect_mobility_manager(*this);

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

  const auto& position_plan_cfg = cfg.mobility.onboard_position_plan;
  if (position_plan_cfg.enabled) {
    ntn_onboard_position_plan_ctrl.emplace(make_ntn_onboard_position_plan_config(cfg));
    ntn_position_plan_reload_timer = cfg.services.timers->create_unique_timer(*cfg.services.cu_cp_executor);
    ntn_position_plan_activation_timer = cfg.services.timers->create_unique_timer(*cfg.services.cu_cp_executor);
    restore_ntn_onboard_position_plan_state();
    bool recovery_required = false;
    {
      std::lock_guard<std::mutex> lock(ntn_onboard_position_plan_mutex);
      recovery_required =
          ntn_onboard_position_plan_ctrl.has_value() && ntn_onboard_position_plan_ctrl->recovery_plan().has_value();
    }
    if (!recovery_required && !ntn_position_plan_state_write_blocked) {
      reload_ntn_onboard_position_plan();
    }
    if (position_plan_cfg.reload_period.count() > 0) {
      ntn_position_plan_reload_deadline = std::chrono::steady_clock::now() + position_plan_cfg.reload_period;
      schedule_ntn_onboard_position_plan_reload();
    }
    schedule_ntn_onboard_position_plan_activation();
  }

  // Start statistics report timer.
  statistics_report_timer = cfg.services.timers->create_unique_timer(*cfg.services.cu_cp_executor);
  statistics_report_timer.set(cfg.metrics.statistics_report_period,
                              [this](timer_id_t /*tid*/) { on_statistics_report_timer_expired(); });
  statistics_report_timer.run();
  if (ntn_cfg.enabled) {
    ntn_resource_audit_timer = cfg.services.timers->create_unique_timer(*cfg.services.cu_cp_executor);
    ntn_resource_audit_timer.set(std::chrono::seconds{1},
                                 [this](timer_id_t /*tid*/) { on_ntn_resource_audit_timer_expired(); });
    ntn_resource_audit_timer.run();
  }
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
    ntn_resource_audit_timer.stop();
    ntn_position_plan_reload_timer.stop();
    ntn_position_plan_activation_timer.stop();
    // Cancel calendar F1 I/O before DU/F1AP repositories are torn down. Destroying the returned eager tasks cancels
    // any currently awaited transaction and prevents completion frames from outliving their dependencies.
    [[maybe_unused]] auto ntn_prepare_loop = ntn_position_plan_prepare_io_sched.request_stop();
    [[maybe_unused]] auto ntn_query_loop   = ntn_position_plan_query_io_sched.request_stop();
    [[maybe_unused]] auto ntn_clear_loop   = ntn_position_plan_clear_io_sched.request_stop();
    ntn_position_plan_query_in_flight = false;
    ntn_position_plan_clear_in_flight = false;
    ntn_position_plan_query_du_index.reset();
    ntn_position_plan_clear_du_index.reset();
    ntn_position_plan_prepare_dispatched.reset();
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

    if (schedule_ntn_inactive_suspend_if_eligible(msg.ue_index)) {
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

bool cu_cp_impl::handle_ue_setup_request(ue_index_t ue_index)
{
  std::optional<std::string>       serving_beam_id;
  std::optional<nr_cell_identity>  serving_nci;
  du_index_t                       ue_du_index = du_index_t::invalid;
  if (cu_cp_ue* ue = ue_mng.find_du_ue(ue_index); ue != nullptr) {
    ue_du_index = ue->get_du_index();
    if (ue->get_meas_context().last_ntn_location_report.has_value()) {
      serving_nci = ue->get_meas_context().last_ntn_location_report->serving_nci;
    } else {
      serving_nci = get_ue_serving_nci_for_ntn_load(du_db, *ue);
    }
    if (serving_nci.has_value()) {
      serving_beam_id = find_ntn_beam_id_by_nci(serving_nci.value());
    }
  }
  auto make_access_ownership_update = [this, ue_index]() -> std::optional<ntn_access_rnti_ownership_update> {
    cu_cp_ue* current_ue = ue_mng.find_du_ue(ue_index);
    if (current_ue == nullptr) {
      return std::nullopt;
    }
    std::optional<ntn_ue_access_service_layer_state> layer_state =
        build_ntn_access_layer_state_for_ue(*current_ue);
    if (!layer_state.has_value()) {
      return std::nullopt;
    }

    ntn_access_rnti_ownership_update ownership;
    ownership.ue_index       = ue_index;
    ownership.du_index       = current_ue->get_du_index();
    ownership.cell_index     = to_f1ap_du_cell_index(current_ue->get_pcell_index());
    ownership.pci            = current_ue->get_pci();
    ownership.rnti           = current_ue->get_c_rnti();
    ownership.analog_beam_id = layer_state->access_analog_beam_id;
    ownership.access_nci     = layer_state->access_nci;
    ownership.has_access_nci = layer_state->has_access_nci;
    return ownership;
  };
  if (block_new_ntn_demand_if_ntn_policy_blocks("RRC setup", serving_beam_id, serving_nci)) {
    logger.debug("ue={}: Rejecting RRC setup. Cause: NTN service policy", ue_index);
    return false;
  }
  if (block_new_ntn_access_if_rnti_pool_unavailable(serving_beam_id, ue_index)) {
    logger.debug("ue={}: Rejecting RRC setup. Cause: NTN RNTI lease pool unavailable", ue_index);
    return false;
  }
  if (std::optional<ntn_access_rnti_ownership_update> ownership = make_access_ownership_update();
      ownership.has_value()) {
    const ntn_access_rnti_ownership_result ownership_validation =
        ntn_service_resource_mng.validate_access_rnti_ownership(*ownership);
    if (!ownership_validation.accepted) {
      logger.warning("ue={} c-rnti={}: Rejecting NTN access. Cause: {}",
                     ue_index,
                     ownership->rnti,
                     ownership_validation.reason);
      return false;
    }
  }
  auto store_access_active_context = [this, ue_index]() {
    cu_cp_ue* current_ue = ue_mng.find_du_ue(ue_index);
    if (current_ue == nullptr) {
      return;
    }
    std::optional<ntn_ue_access_service_layer_state> layer_state =
        build_ntn_access_layer_state_for_ue(*current_ue);
    if (!layer_state.has_value()) {
      return;
    }
    layer_state->access_state  = ntn_access_state_access_active;
    layer_state->access_reason = "valid";
    ntn_ue_layer_states[ue_index] = *layer_state;

    ntn_access_rnti_ownership_update ownership;
    ownership.ue_index       = ue_index;
    ownership.du_index       = current_ue->get_du_index();
    ownership.cell_index     = to_f1ap_du_cell_index(current_ue->get_pcell_index());
    ownership.pci            = current_ue->get_pci();
    ownership.rnti           = current_ue->get_c_rnti();
    ownership.analog_beam_id = layer_state->access_analog_beam_id;
    ownership.access_nci     = layer_state->access_nci;
    ownership.has_access_nci = layer_state->has_access_nci;
    const ntn_access_rnti_ownership_result ownership_result =
        ntn_service_resource_mng.register_access_rnti_ownership(ownership);
    if (!ownership_result.accepted) {
      logger.warning("ue={} c-rnti={}: NTN access RNTI ownership {}", ue_index, current_ue->get_c_rnti(), ownership_result.reason);
    } else {
      schedule_ntn_rnti_lease_pool_updates_for_access_beams();
    }
  };
  if (prepare_ntn_pre_service_relocation_if_access_du_mismatch(ue_index, serving_beam_id, serving_nci, ue_du_index)) {
    const bool accepted = controller.request_ue_setup(cu_cp_admission_request_type::initial_access);
    if (accepted) {
      store_access_active_context();
    }
    return accepted;
  }
  if (block_new_ntn_demand_if_access_du_policy_blocks("RRC setup", serving_beam_id, ue_du_index, false)) {
    logger.debug("ue={}: Rejecting RRC setup. Cause: NTN access DU policy", ue_index);
    return false;
  }
  if (block_new_ntn_access_if_resource_domain_blocks(serving_beam_id, ue_index)) {
    logger.debug("ue={}: Rejecting RRC setup. Cause: NTN analog access resource domain", ue_index);
    return false;
  }
  const bool accepted = controller.request_ue_setup(cu_cp_admission_request_type::initial_access);
  if (accepted) {
    store_access_active_context();
  }
  return accepted;
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

  refresh_ntn_beam_placement_for_current_load();

  return true;
}

rrc_ue_reestablishment_context_response
cu_cp_impl::handle_rrc_reestablishment_request(pci_t old_pci, rnti_t old_c_rnti, ue_index_t ue_index)
{
  rrc_ue_reestablishment_context_response reest_context{};

  ue_index_t old_ue_index = ue_mng.get_ue_index(old_pci, old_c_rnti);
  if (old_ue_index == ue_index_t::invalid || old_ue_index == ue_index) {
    return reest_context;
  }

  auto* const old_ue = ue_mng.find_du_ue(old_ue_index);
  if (old_ue == nullptr) {
    logger.debug("ue={}: Could not find UE", old_ue_index);
    return reest_context;
  }

  std::optional<nr_cell_identity>  serving_nci = get_ue_serving_nci_for_ntn_load(du_db, *old_ue);
  std::optional<std::string>       serving_beam_id;
  if (serving_nci.has_value()) {
    serving_beam_id = find_ntn_beam_id_by_nci(serving_nci.value());
  }
  if (block_new_ntn_demand_if_ntn_policy_blocks("RRC reestablishment", serving_beam_id, serving_nci)) {
    logger.debug("ue={}: Rejecting RRC reestablishment. Cause: NTN service policy", ue_index);
    return reest_context;
  }

  if (!controller.request_ue_setup(cu_cp_admission_request_type::reestablishment)) {
    logger.debug("ue={}: Rejecting RRC reestablishment. Cause: admission control", ue_index);
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

void cu_cp_impl::handle_rrc_resume_request(ue_index_t            ue_index,
                                           ue_index_t            old_ue_index,
                                           establishment_cause_t rrc_resume_cause)
{
  ++nof_ntn_inactive_resume_requested;
  last_ntn_inactive_reason = "resume_requested";

  const auto inactive_it = ntn_inactive_contexts.find(old_ue_index);
  if (inactive_it == ntn_inactive_contexts.end()) {
    ++nof_ntn_inactive_resume_failed;
    last_ntn_inactive_reason = "inactive_context_not_found";
    return;
  }

  const auto max_age = cfg.mobility.meas_manager_config.ntn_location_mobility.idle_paging_context_max_age;
  if (max_age.count() > 0 && inactive_it->second.updated_time != std::chrono::steady_clock::time_point{} &&
      std::chrono::steady_clock::now() - inactive_it->second.updated_time > max_age) {
    ++nof_ntn_inactive_resume_failed;
    ++nof_ntn_inactive_contexts_expired;
    last_ntn_inactive_reason = "inactive_context_expired";
    ntn_inactive_contexts.erase(inactive_it);
    return;
  }

  cu_cp_ue* old_ue = ue_mng.find_ue(old_ue_index);
  if (old_ue == nullptr) {
    ++nof_ntn_inactive_resume_failed;
    last_ntn_inactive_reason = "inactive_ue_not_found";
    return;
  }

  ngap_interface* ngap = ngap_db.find_ngap(old_ue->get_ue_context().plmn);
  if (ngap == nullptr) {
    ++nof_ntn_inactive_resume_failed;
    last_ntn_inactive_reason = "resume_ngap_not_found";
    return;
  }

  old_ue->get_task_sched().schedule_async_task(
      launch_async([this, ngap, old_ue_index, ue_index, rrc_resume_cause](coro_context<async_task<void>>& ctx) mutable {
        bool resume_sent = false;

        CORO_BEGIN(ctx);

        CORO_AWAIT_VALUE(resume_sent,
                         ngap->get_ngap_control_message_handler().handle_ue_context_resume_request(old_ue_index,
                                                                                                    rrc_resume_cause));
        if (!resume_sent) {
          ++nof_ntn_inactive_resume_failed;
          last_ntn_inactive_reason = "ngap_resume_failed";
          CORO_EARLY_RETURN();
        }

        if (auto active_it = ntn_inactive_contexts.find(old_ue_index); active_it != ntn_inactive_contexts.end()) {
          active_it->second.state        = "resume_requested";
          active_it->second.reason       = "rrc_resume_request";
          active_it->second.updated_time = std::chrono::steady_clock::now();
        }
        ++nof_ntn_inactive_resume_succeeded;
        last_ntn_inactive_reason = ue_index == old_ue_index ? "resume_sent" : "resume_sent_from_new_ue";
        CORO_RETURN();
      }));
}

void cu_cp_impl::handle_ue_context_suspend_outcome(ue_index_t ue_index, bool success)
{
  auto inactive_it = ntn_inactive_contexts.find(ue_index);
  if (success) {
    ++nof_ntn_inactive_ngap_suspend_responses;
    if (inactive_it != ntn_inactive_contexts.end()) {
      inactive_it->second.state        = "suspended";
      inactive_it->second.reason       = "ngap_suspend_response";
      inactive_it->second.updated_time = std::chrono::steady_clock::now();
    }
    last_ntn_inactive_reason = "ngap_suspend_response";
    return;
  }

  ++nof_ntn_inactive_ngap_suspend_failures;
  ++nof_ntn_inactive_suspend_failed;
  ++nof_ntn_inactive_fallback_releases;
  if (inactive_it != ntn_inactive_contexts.end()) {
    inactive_it->second.state        = "suspend_failed";
    inactive_it->second.reason       = "ngap_suspend_failure";
    inactive_it->second.updated_time = std::chrono::steady_clock::now();
  }
  last_ntn_inactive_reason = "ngap_suspend_failure";

  if (cu_cp_ue* ue = ue_mng.find_ue(ue_index); ue != nullptr) {
    cu_cp_ue_context_release_request release_req;
    release_req.ue_index = ue_index;
    release_req.cause    = ngap_cause_radio_network_t::user_inactivity;
    release_req.pdu_session_res_list_cxt_rel_req = ue->get_up_resource_manager().get_pdu_sessions();
    ue->get_task_sched().schedule_async_task(handle_ue_context_release(release_req));
  }
}

void cu_cp_impl::handle_ue_context_resume_outcome(ue_index_t ue_index, bool success)
{
  auto inactive_it = ntn_inactive_contexts.find(ue_index);
  if (success) {
    ++nof_ntn_inactive_ngap_resume_responses;
    if (inactive_it != ntn_inactive_contexts.end()) {
      ntn_inactive_contexts.erase(inactive_it);
    }
    last_ntn_inactive_reason = "ngap_resume_response";
    return;
  }

  ++nof_ntn_inactive_ngap_resume_failures;
  ++nof_ntn_inactive_resume_failed;
  ++nof_ntn_inactive_fallback_releases;
  if (inactive_it != ntn_inactive_contexts.end()) {
    inactive_it->second.state        = "resume_failed";
    inactive_it->second.reason       = "ngap_resume_failure";
    inactive_it->second.updated_time = std::chrono::steady_clock::now();
  }
  last_ntn_inactive_reason = "ngap_resume_failure";

  if (cu_cp_ue* ue = ue_mng.find_ue(ue_index); ue != nullptr) {
    cu_cp_ue_context_release_request release_req;
    release_req.ue_index = ue_index;
    release_req.cause    = ngap_cause_radio_network_t::unspecified;
    release_req.pdu_session_res_list_cxt_rel_req = ue->get_up_resource_manager().get_pdu_sessions();
    ue->get_task_sched().schedule_async_task(handle_ue_context_release(release_req));
  }
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
  if (request.ntn_context.has_value()) {
    auto state_it = ntn_connected_handover_states.find(request.source_ue_index);
    if (state_it != ntn_connected_handover_states.end() &&
        (state_it->second.attempt_id == 0 ||
         state_it->second.attempt_id == request.ntn_context->handover_attempt_id)) {
      state_it->second.state = "handover_preparing";
    }
  }

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
  handle_ntn_pre_service_relocation_result(result);
  handle_ntn_connected_handover_result(result);

  if (!result.success && result.source_reconfiguration_can_resume) {
    if (cu_cp_ue* ue = ue_mng.find_du_ue(result.source_ue_index); ue != nullptr) {
      ue->get_ue_context().reconfiguration_disabled = false;
      logger.debug("ue={}: Restored reconfiguration after NTN handover source preparation failure",
                   result.source_ue_index);
    }
  }

  cell_meas_mng.handle_ntn_handover_result(result);
}

void cu_cp_impl::handle_ntn_handover_target_resources_applied(
    ue_index_t                                  source_ue_index,
    ue_index_t                                  target_ue_index,
    const ntn_handover_context&                 context,
    rnti_t                                      target_c_rnti,
    const f1ap_ntn_ul_slot_resource_result&     slot_result)
{
  auto state_it = ntn_connected_handover_states.find(source_ue_index);
  if (state_it == ntn_connected_handover_states.end()) {
    return;
  }
  if (state_it->second.attempt_id != 0 && state_it->second.attempt_id != context.handover_attempt_id) {
    return;
  }

  state_it->second.state                  = "target_resource_applied";
  state_it->second.target_resource_state  = "applied_by_du";
  state_it->second.target_c_rnti          = target_c_rnti;
  state_it->second.target_sr_srs_applied  = slot_result.accepted;

  if (context.target_ul_slot_request.has_value() && slot_result.accepted) {
    ntn_service_resource_mng.set_digital_slot_intent_from_request(
        target_ue_index, *context.target_ul_slot_request, "connected_handover_target");
    ntn_service_resource_mng.mark_slot_update_applied(target_ue_index, *context.target_ul_slot_request);
  }
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

  async_task<expected<ngap_init_context_setup_response, ngap_init_context_setup_failure>> setup_task =
      launch_async<initial_context_setup_routine>(request,
                                                  *rrc_ue,
                                                  ngap->get_ngap_ue_radio_cap_management_handler(),
                                                  ue->get_security_manager(),
                                                  du_db.get_du_processor(ue->get_du_index()).get_f1ap_handler(),
                                                  get_cu_cp_ngap_handler(),
                                                  logger,
                                                  make_ntn_ul_slot_request_for_ue_context_setup(
                                                      du_db, *ue, current_ntn_beam_placement_plan));

  return launch_async([this, ue_index = request.ue_index, setup_task = std::move(setup_task)](
                          coro_context<async_task<expected<ngap_init_context_setup_response,
                                                        ngap_init_context_setup_failure>>>& ctx) mutable {
    expected<ngap_init_context_setup_response, ngap_init_context_setup_failure> response;
    CORO_BEGIN(ctx);
    CORO_AWAIT_VALUE(response, setup_task);
    if (response.has_value()) {
      release_ntn_analog_access_after_initial_context_setup(ue_index);
      schedule_ntn_pre_service_relocation_if_needed(ue_index);
    }
    CORO_RETURN(response);
  });
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
  auto make_radio_resource_unavailable_response =
      [&request]() -> async_task<cu_cp_pdu_session_resource_setup_response> {
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
  };

  const auto& ntn_cfg = cfg.mobility.meas_manager_config.ntn_location_mobility;
  const bool access_service_layer_enabled = ntn_cfg.enabled && !ntn_cfg.analog_beams.empty();
  const ntn_qos_demand_summary pending_qos = summarize_ntn_qos_demand(request);
  std::optional<std::string>      serving_beam_id;
  std::optional<nr_cell_identity> serving_nci;
  if (ue->get_meas_context().last_ntn_location_report.has_value()) {
    serving_nci = ue->get_meas_context().last_ntn_location_report->serving_nci;
  } else {
    serving_nci = get_ue_serving_nci_for_ntn_load(du_db, *ue);
  }
  if (serving_nci.has_value()) {
    serving_beam_id = find_ntn_beam_id_by_nci(serving_nci.value());
  }

  if (block_new_ntn_pdu_session_demand_if_pre_service_relocation_pending(request.ue_index)) {
    logger.warning("ue={}: Rejecting PDU session resource setup. Cause: NTN pre-service relocation", request.ue_index);
    return make_radio_resource_unavailable_response();
  }

  if (access_service_layer_enabled) {
    serving_beam_id = prepare_ntn_digital_service_binding_for_pdu_session(*ue, expected_drbs, pending_qos);
    if (!serving_beam_id.has_value()) {
      logger.warning("ue={}: Rejecting PDU session resource setup. Cause: NTN digital service binding", request.ue_index);
      return make_radio_resource_unavailable_response();
    }
    const ntn_beam_position* serving_beam = find_ntn_beam_cfg(ntn_cfg.beams, serving_beam_id.value());
    if (serving_beam != nullptr) {
      serving_nci = serving_beam->nci;
    }
  } else {
    if (block_new_ntn_demand_if_ntn_policy_blocks("PDU session setup", serving_beam_id, serving_nci)) {
      logger.warning("ue={}: Rejecting PDU session resource setup. Cause: NTN service policy", request.ue_index);
      return make_radio_resource_unavailable_response();
    }
    if (block_new_ntn_pdu_session_demand_if_access_du_policy_blocks(serving_beam_id,
                                                                    ue->get_du_index(),
                                                                    expected_drbs,
                                                                    pending_qos)) {
      logger.warning("ue={}: Rejecting PDU session resource setup. Cause: NTN access DU policy", request.ue_index);
      return make_radio_resource_unavailable_response();
    }
    if (block_low_priority_ntn_pdu_session_demand_if_headroom_is_reserved(serving_beam_id, expected_drbs, pending_qos)) {
      logger.warning("ue={}: Rejecting PDU session resource setup. Cause: NTN headroom admission", request.ue_index);
      return make_radio_resource_unavailable_response();
    }
    if (block_low_priority_ntn_pdu_session_demand_if_capacity_is_reserved(serving_beam_id, expected_drbs, pending_qos)) {
      logger.warning("ue={}: Rejecting PDU session resource setup. Cause: NTN QoS service policy", request.ue_index);
      return make_radio_resource_unavailable_response();
    }
  }
  if (!controller.request_ue_setup(cu_cp_admission_request_type::initial_access, 0, expected_drbs)) {
    logger.warning("ue={}: Rejecting PDU session resource setup. Cause: admission control", request.ue_index);
    if (access_service_layer_enabled) {
      commit_ntn_digital_service_binding_if_pdu_setup_succeeded(request.ue_index, false);
    }
    return make_radio_resource_unavailable_response();
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
                       ue_index = request.ue_index,
                       access_service_layer_enabled,
                       setup_task = std::move(setup_task)](
                          coro_context<async_task<cu_cp_pdu_session_resource_setup_response>>& ctx) mutable {
    cu_cp_pdu_session_resource_setup_response response;
    CORO_BEGIN(ctx);
    CORO_AWAIT_VALUE(response, setup_task);
    if (access_service_layer_enabled) {
      commit_ntn_digital_service_binding_if_pdu_setup_succeeded(ue_index,
                                                                !response.pdu_session_res_setup_response_items.empty());
    }
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
                       ue_index = command.ue_index,
                       release_task = std::move(release_task)](
                          coro_context<async_task<cu_cp_pdu_session_resource_release_response>>& ctx) mutable {
    cu_cp_pdu_session_resource_release_response response;
    CORO_BEGIN(ctx);
    CORO_AWAIT_VALUE(response, release_task);
    clear_ntn_digital_service_context_if_no_service_remains(ue_index);
    refresh_ntn_beam_placement_for_current_load();
    CORO_RETURN(response);
  });
}

async_task<cu_cp_ue_context_release_complete>
cu_cp_impl::handle_ue_context_release_command(const cu_cp_ue_context_release_command& command)
{
  cu_cp_ue* ue = ue_mng.find_du_ue(command.ue_index);
  srsran_assert(ue != nullptr, "ue={}: Could not find DU UE", command.ue_index);

  persist_ntn_idle_paging_context_for_ue(command.ue_index, "release_command");

  e1ap_bearer_context_manager* e1ap_bearer_ctxt_mng = nullptr;
  if (ue->get_cu_up_index() != cu_up_index_t::invalid) {
    e1ap_bearer_ctxt_mng = &cu_up_db.find_cu_up_processor(ue->get_cu_up_index())->get_e1ap_bearer_context_manager();
  }

  return launch_async<ue_context_release_routine>(command,
                                                  e1ap_bearer_ctxt_mng,
                                                  du_db.get_du_processor(ue->get_du_index()).get_f1ap_handler(),
                                                  get_cu_cp_ue_removal_handler(),
                                                  ue_mng,
                                                  logger,
                                                  build_ntn_release_user_location_info(command.ue_index),
                                                  build_ntn_paging_recommendation(command.ue_index));
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
  const nr_cell_identity target_nci = request.source_to_target_transparent_container.target_cell_id.nci;
  std::optional<std::string> target_beam_id = find_ntn_beam_id_by_nci(target_nci);
  if (block_new_ntn_demand_if_ntn_policy_blocks("incoming N2 handover", target_beam_id, target_nci)) {
    logger.warning("ue={}: Rejecting incoming N2 handover. Cause: NTN service policy", request.ue_index);
    ngap_handover_resource_allocation_response response;
    response.ue_index = request.ue_index;
    response.success  = false;
    response.cause    = ngap_cause_radio_network_t::ho_target_not_allowed;
    return launch_async([response](coro_context<async_task<ngap_handover_resource_allocation_response>>& ctx) mutable {
      CORO_BEGIN(ctx);
      CORO_RETURN(response);
    });
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

  std::optional<std::string> target_beam_id = find_ntn_beam_id_by_nci(cgi.nci);
  if (block_new_ntn_demand_if_ntn_policy_blocks("incoming handover", target_beam_id, cgi.nci)) {
    logger.warning("Could not allocate new UE index for incoming handover CGI={}. Cause: NTN service policy", cgi.nci);
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
    ntn_service_resource_mng.remove_ue(ue_index);
    ue_mng.remove_ue(ue_index);
    return ue_index_t::invalid;
  }

  return ue_index;
}

#ifndef SRSRAN_HAS_ENTERPRISE

void cu_cp_impl::handle_dl_ue_associated_nrppa_transport_pdu(ue_index_t ue_index, const byte_buffer& nrppa_pdu)
{
  ++nof_ntn_nrppa_dl_ue_received;
  if (ue_mng.find_ue(ue_index) == nullptr) {
    ++nof_ntn_nrppa_dl_ue_dropped;
    last_ntn_nrppa_dropped_reason = "unknown_ue";
    logger.debug("ue={}: Dropping DL UE associated NRPPa Transport. Cause: UE context does not exist", ue_index);
    return;
  }
  if (nrppa_entity == nullptr) {
    ++nof_ntn_nrppa_dl_ue_dropped;
    last_ntn_nrppa_dropped_reason = "nrppa_unavailable";
    logger.debug("ue={}: Dropping DL UE associated NRPPa Transport. Cause: NRPPa entity is unavailable", ue_index);
    return;
  }

  nrppa_entity->get_nrppa_message_handler().handle_new_nrppa_pdu(nrppa_pdu.copy(),
                                                                 std::variant<ue_index_t, amf_index_t>{ue_index});
  ++nof_ntn_nrppa_dl_ue_forwarded;
}

void cu_cp_impl::handle_dl_non_ue_associated_nrppa_transport_pdu(amf_index_t amf_index, const byte_buffer& nrppa_pdu)
{
  ++nof_ntn_nrppa_dl_non_ue_received;
  if (ngap_db.find_ngap(amf_index) == nullptr) {
    ++nof_ntn_nrppa_dl_non_ue_dropped;
    last_ntn_nrppa_dropped_reason = "unknown_amf";
    logger.debug("amf={}: Dropping DL non UE associated NRPPa Transport. Cause: NGAP context does not exist",
                 fmt::underlying(amf_index));
    return;
  }
  if (nrppa_entity == nullptr) {
    ++nof_ntn_nrppa_dl_non_ue_dropped;
    last_ntn_nrppa_dropped_reason = "nrppa_unavailable";
    logger.debug("amf={}: Dropping DL non UE associated NRPPa Transport. Cause: NRPPa entity is unavailable",
                 fmt::underlying(amf_index));
    return;
  }

  nrppa_entity->get_nrppa_message_handler().handle_new_nrppa_pdu(nrppa_pdu.copy(),
                                                                 std::variant<ue_index_t, amf_index_t>{amf_index});
  ++nof_ntn_nrppa_dl_non_ue_forwarded;
}

nrppa_cu_cp_ue_notifier* cu_cp_impl::handle_new_nrppa_ue(ue_index_t ue_index)
{
  cu_cp_ue* ue = ue_mng.find_ue(ue_index);
  if (ue == nullptr) {
    last_ntn_nrppa_dropped_reason = "unknown_ue";
    logger.debug("ue={}: Cannot create NRPPa UE notifier. Cause: UE context does not exist", ue_index);
    return nullptr;
  }
  return &ue->get_nrppa_cu_cp_ue_notifier();
}

void cu_cp_impl::handle_ul_nrppa_pdu(const byte_buffer&                    nrppa_pdu,
                                     std::variant<ue_index_t, amf_index_t> ue_or_amf_index)
{
  if (std::holds_alternative<ue_index_t>(ue_or_amf_index)) {
    ++nof_ntn_nrppa_ul_ue_received;
    const ue_index_t ue_index = std::get<ue_index_t>(ue_or_amf_index);
    cu_cp_ue*        ue       = ue_mng.find_ue(ue_index);
    if (ue == nullptr) {
      ++nof_ntn_nrppa_ul_ue_dropped;
      last_ntn_nrppa_dropped_reason = "unknown_ue";
      logger.debug("ue={}: Dropping UL UE associated NRPPa Transport. Cause: UE context does not exist", ue_index);
      return;
    }
    ngap_interface* ngap = ngap_db.find_ngap(ue->get_ue_context().plmn);
    if (ngap == nullptr) {
      ++nof_ntn_nrppa_ul_ue_dropped;
      last_ntn_nrppa_dropped_reason = "ngap_not_found";
      logger.debug("ue={}: Dropping UL UE associated NRPPa Transport. Cause: NGAP not found for PLMN={}",
                   ue_index,
                   ue->get_ue_context().plmn);
      return;
    }

    ngap->get_ngap_control_message_handler().handle_ul_ue_associated_nrppa_transport(ue_index, nrppa_pdu);
    ++nof_ntn_nrppa_ul_ue_sent;
    return;
  }

  ++nof_ntn_nrppa_ul_non_ue_received;
  const amf_index_t amf_index = std::get<amf_index_t>(ue_or_amf_index);
  ngap_interface*   ngap      = ngap_db.find_ngap(amf_index);
  if (ngap == nullptr) {
    ++nof_ntn_nrppa_ul_non_ue_dropped;
    last_ntn_nrppa_dropped_reason = "unknown_amf";
    logger.debug("amf={}: Dropping UL non UE associated NRPPa Transport. Cause: NGAP context does not exist",
                 fmt::underlying(amf_index));
    return;
  }
  if (!common_task_sched.schedule_async_task(
          ngap->get_ngap_control_message_handler().handle_ul_non_ue_associated_nrppa_transport(nrppa_pdu.copy()))) {
    ++nof_ntn_nrppa_ul_non_ue_dropped;
    last_ntn_nrppa_dropped_reason = "schedule_failed";
    logger.debug("amf={}: Dropping UL non UE associated NRPPa Transport. Cause: scheduling failed",
                 fmt::underlying(amf_index));
    return;
  }
  ++nof_ntn_nrppa_ul_non_ue_sent;
}

void cu_cp_impl::handle_unsupported_nrppa_pdu(std::string_view reason)
{
  ++nof_ntn_nrppa_unsupported_procedures;
  last_ntn_nrppa_trp_reason = reason.empty() ? "unsupported_payload_procedure" : std::string(reason);
}

void cu_cp_impl::handle_nrppa_standard_codec_event(const nrppa_standard_codec_event& event)
{
  switch (event.type) {
    case nrppa_standard_codec_event_type::decode_success:
      ++nof_ntn_nrppa_standard_decode_success;
      break;
    case nrppa_standard_codec_event_type::decode_failure:
      ++nof_ntn_nrppa_standard_decode_failure;
      break;
    case nrppa_standard_codec_event_type::encode_response:
      ++nof_ntn_nrppa_standard_encode_responses;
      break;
    case nrppa_standard_codec_event_type::encode_failure:
      ++nof_ntn_nrppa_standard_encode_failures;
      break;
    case nrppa_standard_codec_event_type::minimal_fallback_decode:
      ++nof_ntn_nrppa_minimal_fallback_decodes;
      break;
  }
  last_ntn_nrppa_standard_decode_reason = event.reason.empty() ? "none" : std::string(event.reason);
}

async_task<trp_information_cu_cp_response_t>
cu_cp_impl::handle_trp_information_request(const trp_information_request_t& request)
{
  ++nof_ntn_nrppa_trp_requests_received;
  ++nof_ntn_nrppa_trp_requests_decoded;

  trp_information_cu_cp_response_t response;
  response.transaction_id = request.transaction_id;

  const auto& ntn_cfg = cfg.mobility.meas_manager_config.ntn_location_mobility;
  if (!ntn_cfg.enabled) {
    ++nof_ntn_nrppa_trp_failures_sent;
    last_ntn_nrppa_trp_reason = "ntn_disabled";
    logger.debug("TRP Information Request returned no NTN TRP. Cause: NTN location mobility is disabled");
    return launch_async([response](coro_context<async_task<trp_information_cu_cp_response_t>>& ctx) mutable {
      CORO_BEGIN(ctx);
      CORO_RETURN(response);
    });
  }

  const std::vector<trp_information_type_item_t> requested_types =
      get_effective_ntn_trp_information_type_request(request);

  for (const ntn_beam_du_assignment& assignment : current_ntn_beam_placement_plan.assignments) {
    if (assignment.state == ntn_beam_assignment_state::inactive || assignment.du_index == du_index_t::invalid) {
      continue;
    }

    const ntn_beam_position* beam_cfg = find_ntn_beam_cfg(ntn_cfg.beams, assignment.beam_id);
    if (beam_cfg == nullptr || !beam_cfg->enabled) {
      continue;
    }

    const trp_id_t trp_id = make_ntn_trp_id_from_nci(assignment.nci);
    if (!trp_information_request_matches_trp(request, trp_id)) {
      continue;
    }

    const du_cell_configuration* du_cell = find_du_cell_by_nci(du_db, assignment.du_index, assignment.nci);
    const std::optional<cell_meas_config> runtime_cell_cfg = cell_meas_mng.get_cell_config(assignment.nci);
    const serving_cell_meas_config*       serving_cell_cfg =
        runtime_cell_cfg.has_value() ? &runtime_cell_cfg->serving_cell_cfg : nullptr;

    trp_information_list_trp_response_item_t trp_item;
    trp_item.trp_info.trp_id = trp_id;
    for (trp_information_type_item_t type : requested_types) {
      if (!is_supported_ntn_trp_information_type(type)) {
        ++nof_ntn_nrppa_trp_unsupported_info_items;
        continue;
      }

      switch (type) {
        case trp_information_type_item_t::nr_pci:
          if (du_cell != nullptr) {
            trp_item.trp_info.trp_info_type_resp_list.emplace_back(du_cell->pci);
          } else if (serving_cell_cfg != nullptr && serving_cell_cfg->pci.has_value()) {
            trp_item.trp_info.trp_info_type_resp_list.emplace_back(serving_cell_cfg->pci.value());
          }
          break;
        case trp_information_type_item_t::ng_ran_cgi:
          if (du_cell != nullptr) {
            trp_item.trp_info.trp_info_type_resp_list.emplace_back(du_cell->cgi);
          } else {
            trp_item.trp_info.trp_info_type_resp_list.emplace_back(nr_cell_global_id_t{
                serving_cell_cfg != nullptr ? serving_cell_cfg->plmn : plmn_identity::test_value(), assignment.nci});
          }
          break;
        case trp_information_type_item_t::arfcn:
          if (serving_cell_cfg != nullptr && serving_cell_cfg->ssb_arfcn.has_value()) {
            trp_item.trp_info.trp_info_type_resp_list.emplace_back(
                static_cast<uint32_t>(serving_cell_cfg->ssb_arfcn.value()));
          }
          break;
        case trp_information_type_item_t::geo_coord:
          trp_item.trp_info.trp_info_type_resp_list.emplace_back(make_ntn_trp_geographical_coordinates(*beam_cfg));
          break;
        case trp_information_type_item_t::trp_type:
          trp_item.trp_info.trp_info_type_resp_list.emplace_back(trp_type_t::trp);
          break;
        default:
          break;
      }
    }

    if (!trp_item.trp_info.trp_info_type_resp_list.empty()) {
      response.trp_info_responses[assignment.du_index].trp_info_list_trp_resp.push_back(std::move(trp_item));
    }
  }

  if (response.trp_info_responses.empty()) {
    ++nof_ntn_nrppa_trp_empty_results;
    last_ntn_nrppa_trp_reason = "empty_result";
    logger.debug("TRP Information Request returned no NTN TRP. requested_trps={}", request.trp_list.size());
  } else {
    ++nof_ntn_nrppa_trp_responses_sent;
    last_ntn_nrppa_trp_reason = "responded";
    logger.debug("TRP Information Request returned {} DU TRP response sets", response.trp_info_responses.size());
  }

  return launch_async([response = std::move(response)](coro_context<async_task<trp_information_cu_cp_response_t>>& ctx) mutable {
    CORO_BEGIN(ctx);
    CORO_RETURN(response);
  });
}

async_task<expected<positioning_information_response_t, positioning_information_failure_t>>
cu_cp_impl::handle_positioning_information_request(const positioning_information_request_t& request)
{
  ++nof_ntn_nrppa_positioning_info_requests_received;
  ++nof_ntn_nrppa_positioning_info_requests_decoded;

  auto make_failure_task = [](positioning_information_failure_t failure)
      -> async_task<expected<positioning_information_response_t, positioning_information_failure_t>> {
    return launch_async([failure](coro_context<async_task<expected<positioning_information_response_t,
                                                              positioning_information_failure_t>>>& ctx) mutable {
      CORO_BEGIN(ctx);
      CORO_RETURN(make_unexpected(failure));
    });
  };

  cu_cp_ue* ue = ue_mng.find_ue(request.ue_index);
  if (ue == nullptr) {
    ++nof_ntn_nrppa_positioning_info_dropped;
    ++nof_ntn_nrppa_positioning_info_failures_sent;
    last_ntn_nrppa_positioning_info_reason = "unknown_ue";
    logger.debug("ue={}: Positioning Information Request failed. Cause: UE context does not exist", request.ue_index);
    return make_failure_task(make_ntn_positioning_information_failure());
  }

  du_processor* processor = du_db.find_du_processor(ue->get_du_index());
  if (processor == nullptr) {
    ++nof_ntn_nrppa_positioning_info_dropped;
    ++nof_ntn_nrppa_positioning_info_failures_sent;
    last_ntn_nrppa_positioning_info_reason = "du_not_found";
    logger.debug("ue={}: Positioning Information Request failed. Cause: DU processor does not exist", request.ue_index);
    return make_failure_task(make_ntn_positioning_information_failure());
  }

  ++nof_ntn_nrppa_positioning_info_requests_forwarded;
  last_ntn_nrppa_positioning_info_reason = "forwarded";

  return launch_async([this, request, processor](
                          coro_context<async_task<expected<positioning_information_response_t,
                                                          positioning_information_failure_t>>>& ctx) mutable {
    expected<positioning_information_response_t, positioning_information_failure_t> outcome;
    CORO_BEGIN(ctx);
    CORO_AWAIT_VALUE(outcome,
                     processor->get_f1ap_handler()
                         .get_f1ap_nrppa_message_handler()
                         .handle_positioning_information_request(request));
    if (outcome.has_value()) {
      ++nof_ntn_nrppa_positioning_info_responses_sent;
      last_ntn_nrppa_positioning_info_reason = "responded";
      CORO_EARLY_RETURN(outcome.value());
    }

    ++nof_ntn_nrppa_positioning_info_failures_sent;
    last_ntn_nrppa_positioning_info_reason = "f1ap_failure";
    CORO_RETURN(make_unexpected(outcome.error()));
  });
}

async_task<expected<positioning_activation_response_t, positioning_activation_failure_t>>
cu_cp_impl::handle_positioning_activation_request(const positioning_activation_request_t& request)
{
  ++nof_ntn_nrppa_activation_requests_received;
  ++nof_ntn_nrppa_activation_requests_decoded;

  auto make_failure_task = [](positioning_activation_failure_t failure)
      -> async_task<expected<positioning_activation_response_t, positioning_activation_failure_t>> {
    return launch_async([failure](coro_context<async_task<expected<positioning_activation_response_t,
                                                           positioning_activation_failure_t>>>& ctx) mutable {
      CORO_BEGIN(ctx);
      CORO_RETURN(make_unexpected(failure));
    });
  };

  cu_cp_ue* ue = ue_mng.find_ue(request.ue_index);
  if (ue == nullptr) {
    ++nof_ntn_nrppa_activation_dropped;
    ++nof_ntn_nrppa_activation_failures_sent;
    last_ntn_nrppa_activation_reason = "unknown_ue";
    logger.debug("ue={}: Positioning Activation Request failed. Cause: UE context does not exist", request.ue_index);
    return make_failure_task(make_ntn_positioning_activation_failure());
  }

  du_processor* processor = du_db.find_du_processor(ue->get_du_index());
  if (processor == nullptr) {
    ++nof_ntn_nrppa_activation_dropped;
    ++nof_ntn_nrppa_activation_failures_sent;
    last_ntn_nrppa_activation_reason = "du_not_found";
    logger.debug("ue={}: Positioning Activation Request failed. Cause: DU processor does not exist", request.ue_index);
    return make_failure_task(make_ntn_positioning_activation_failure());
  }

  ++nof_ntn_nrppa_activation_requests_forwarded;
  last_ntn_nrppa_activation_reason = "forwarded";

  return launch_async([this, request, processor](
                          coro_context<async_task<expected<positioning_activation_response_t,
                                                          positioning_activation_failure_t>>>& ctx) mutable {
    expected<positioning_activation_response_t, positioning_activation_failure_t> outcome;
    CORO_BEGIN(ctx);
    CORO_AWAIT_VALUE(outcome,
                     processor->get_f1ap_handler()
                         .get_f1ap_nrppa_message_handler()
                         .handle_positioning_activation_request(request));
    if (outcome.has_value()) {
      ++nof_ntn_nrppa_activation_responses_sent;
      last_ntn_nrppa_activation_reason = "responded";
      CORO_EARLY_RETURN(outcome.value());
    }

    ++nof_ntn_nrppa_activation_failures_sent;
    last_ntn_nrppa_activation_reason = "f1ap_failure";
    CORO_RETURN(make_unexpected(outcome.error()));
  });
}

async_task<expected<positioning_deactivation_response_t, positioning_deactivation_failure_t>>
cu_cp_impl::handle_positioning_deactivation_request(const positioning_deactivation_request_t& request)
{
  ++nof_ntn_nrppa_deactivation_requests_received;
  ++nof_ntn_nrppa_deactivation_requests_decoded;

  auto make_failure_task = [](positioning_deactivation_failure_t failure)
      -> async_task<expected<positioning_deactivation_response_t, positioning_deactivation_failure_t>> {
    return launch_async([failure](coro_context<async_task<expected<positioning_deactivation_response_t,
                                                           positioning_deactivation_failure_t>>>& ctx) mutable {
      CORO_BEGIN(ctx);
      CORO_RETURN(make_unexpected(failure));
    });
  };

  cu_cp_ue* ue = ue_mng.find_ue(request.ue_index);
  if (ue == nullptr) {
    ++nof_ntn_nrppa_deactivation_dropped;
    ++nof_ntn_nrppa_deactivation_failures_sent;
    last_ntn_nrppa_deactivation_reason = "unknown_ue";
    logger.debug("ue={}: Positioning Deactivation failed. Cause: UE context does not exist", request.ue_index);
    return make_failure_task(make_ntn_positioning_deactivation_failure());
  }

  du_processor* processor = du_db.find_du_processor(ue->get_du_index());
  if (processor == nullptr) {
    ++nof_ntn_nrppa_deactivation_dropped;
    ++nof_ntn_nrppa_deactivation_failures_sent;
    last_ntn_nrppa_deactivation_reason = "du_not_found";
    logger.debug("ue={}: Positioning Deactivation failed. Cause: DU processor does not exist", request.ue_index);
    return make_failure_task(make_ntn_positioning_deactivation_failure());
  }

  ++nof_ntn_nrppa_deactivation_requests_forwarded;
  last_ntn_nrppa_deactivation_reason = "forwarded";

  return launch_async([this, request, processor](
                          coro_context<async_task<expected<positioning_deactivation_response_t,
                                                          positioning_deactivation_failure_t>>>& ctx) mutable {
    expected<positioning_deactivation_response_t, positioning_deactivation_failure_t> outcome;
    CORO_BEGIN(ctx);
    CORO_AWAIT_VALUE(outcome,
                     processor->get_f1ap_handler()
                         .get_f1ap_nrppa_message_handler()
                         .handle_positioning_deactivation_request(request));
    if (outcome.has_value()) {
      ++nof_ntn_nrppa_deactivation_acks_sent;
      last_ntn_nrppa_deactivation_reason = "acked";
      CORO_EARLY_RETURN(outcome.value());
    }

    ++nof_ntn_nrppa_deactivation_failures_sent;
    last_ntn_nrppa_deactivation_reason = "f1ap_failure";
    CORO_RETURN(make_unexpected(outcome.error()));
  });
}

async_task<expected<positioning_assistance_information_feedback_t, positioning_assistance_information_failure_t>>
cu_cp_impl::handle_positioning_assistance_information_control(
    const positioning_assistance_information_control_request_t& request)
{
  ++nof_ntn_nrppa_assistance_control_requests_received;
  ++nof_ntn_nrppa_assistance_control_requests_decoded;

  auto make_failure_task =
      [](positioning_assistance_information_failure_t failure)
      -> async_task<expected<positioning_assistance_information_feedback_t,
                             positioning_assistance_information_failure_t>> {
    return launch_async(
        [failure](coro_context<async_task<expected<positioning_assistance_information_feedback_t,
                                                   positioning_assistance_information_failure_t>>>& ctx) mutable {
          CORO_BEGIN(ctx);
          CORO_RETURN(make_unexpected(failure));
        });
  };

  std::vector<du_processor*> target_processors;
  std::set<du_index_t>       seen_dus;
  if (!request.positioning_broadcast_cells.empty()) {
    for (const nr_cell_global_id_t& cgi : request.positioning_broadcast_cells) {
      const du_index_t du_index = du_db.find_du(cgi);
      if (du_index == du_index_t::invalid || !seen_dus.insert(du_index).second) {
        continue;
      }
      if (du_processor* processor = du_db.find_du_processor(du_index)) {
        target_processors.push_back(processor);
      }
    }
  } else {
    for (du_index_t du_index : du_db.get_du_processor_indexes()) {
      if (!seen_dus.insert(du_index).second) {
        continue;
      }
      if (du_processor* processor = du_db.find_du_processor(du_index)) {
        target_processors.push_back(processor);
      }
    }
  }

  if (target_processors.empty()) {
    ++nof_ntn_nrppa_assistance_control_dropped;
    ++nof_ntn_nrppa_assistance_control_failures_sent;
    last_ntn_nrppa_assistance_control_reason = "no_target_du";
    logger.debug("Positioning Assistance Information Control failed. Cause: no matching DU target");
    return make_failure_task(make_ntn_positioning_assistance_information_failure(request));
  }

  ++nof_ntn_nrppa_assistance_control_requests_forwarded;
  last_ntn_nrppa_assistance_control_reason = "forwarded";

  return launch_async([this, request, target_processors = std::move(target_processors)](
                          coro_context<async_task<expected<positioning_assistance_information_feedback_t,
                                                          positioning_assistance_information_failure_t>>>& ctx) mutable {
    expected<positioning_assistance_information_feedback_t, positioning_assistance_information_failure_t> outcome;
    positioning_assistance_information_failure_t last_failure = make_ntn_positioning_assistance_information_failure(request);
    size_t                                      target_index = 0;
    du_processor*                              processor    = nullptr;
    CORO_BEGIN(ctx);
    while (target_index < target_processors.size()) {
      processor = target_processors[target_index++];
      CORO_AWAIT_VALUE(outcome,
                       processor->get_f1ap_handler()
                           .get_f1ap_nrppa_message_handler()
                           .handle_positioning_assistance_information_control(request));
      if (outcome.has_value()) {
        ++nof_ntn_nrppa_assistance_control_feedbacks_sent;
        last_ntn_nrppa_assistance_control_reason = "responded";
        CORO_EARLY_RETURN(outcome.value());
      }
      last_failure = outcome.error();
    }

    ++nof_ntn_nrppa_assistance_control_failures_sent;
    last_ntn_nrppa_assistance_control_reason = "f1ap_failure";
    CORO_RETURN(make_unexpected(last_failure));
  });
}

async_task<expected<measurement_response_t, measurement_failure_t>>
cu_cp_impl::handle_positioning_measurement_request(const measurement_request_t& request)
{
  ++nof_ntn_nrppa_measurement_requests_received;
  ++nof_ntn_nrppa_measurement_requests_decoded;

  auto make_failure_task = [](measurement_failure_t failure)
      -> async_task<expected<measurement_response_t, measurement_failure_t>> {
    return launch_async([failure](coro_context<async_task<expected<measurement_response_t,
                                                           measurement_failure_t>>>& ctx) mutable {
      CORO_BEGIN(ctx);
      CORO_RETURN(make_unexpected(failure));
    });
  };

  cu_cp_ue* ue = ue_mng.find_ue(request.ue_index);
  if (ue == nullptr) {
    ++nof_ntn_nrppa_measurement_dropped;
    ++nof_ntn_nrppa_measurement_failures_sent;
    last_ntn_nrppa_measurement_reason = "unknown_ue";
    logger.debug("ue={}: Measurement Request failed. Cause: UE context does not exist", request.ue_index);
    return make_failure_task(make_ntn_measurement_failure(request));
  }

  du_processor* processor = du_db.find_du_processor(ue->get_du_index());
  if (processor == nullptr) {
    ++nof_ntn_nrppa_measurement_dropped;
    ++nof_ntn_nrppa_measurement_failures_sent;
    last_ntn_nrppa_measurement_reason = "du_not_found";
    logger.debug("ue={}: Measurement Request failed. Cause: DU processor does not exist", request.ue_index);
    return make_failure_task(make_ntn_measurement_failure(request));
  }

  ++nof_ntn_nrppa_measurement_requests_forwarded;
  last_ntn_nrppa_measurement_reason = "forwarded";

  return launch_async([this, request, processor](
                          coro_context<async_task<expected<measurement_response_t, measurement_failure_t>>>& ctx) mutable {
    expected<measurement_response_t, measurement_failure_t> outcome;
    CORO_BEGIN(ctx);
    CORO_AWAIT_VALUE(outcome,
                     processor->get_f1ap_handler()
                         .get_f1ap_nrppa_message_handler()
                         .handle_positioning_measurement_request(request));
    if (outcome.has_value()) {
      ++nof_ntn_nrppa_measurement_responses_sent;
      last_ntn_nrppa_measurement_reason = "responded";
      CORO_EARLY_RETURN(outcome.value());
    }

    ++nof_ntn_nrppa_measurement_failures_sent;
    last_ntn_nrppa_measurement_reason = "f1ap_failure";
    CORO_RETURN(make_unexpected(outcome.error()));
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

bool cu_cp_impl::is_ntn_rrc_location_request_capability_allowed(const cu_cp_ue& ue, std::string& skipped_reason) const
{
  if (!cfg.mobility.meas_manager_config.ntn_location_mobility.enabled) {
    skipped_reason = "ntn_location_disabled";
    return false;
  }

  const ntn_ue_capability_summary capability = evaluate_ntn_ue_capability_for_ue(ue);
  if (capability.state != ntn_ue_capability_state::supported) {
    skipped_reason = capability.reason;
    return false;
  }
  if (!capability.matched_deployment_profile) {
    skipped_reason = capability.profile_block_reason;
    return false;
  }

  skipped_reason = "none";
  return true;
}

bool cu_cp_impl::is_ntn_rrc_location_request_desired(ue_index_t ue_index,
                                                     const cu_cp_ue& ue,
                                                     std::string& skipped_reason) const
{
  static_cast<void>(ue);

  const auto layer_it = ntn_ue_layer_states.find(ue_index);
  if (layer_it != ntn_ue_layer_states.end() &&
      (layer_it->second.service_state == ntn_service_state_binding_pending ||
       layer_it->second.service_state == ntn_service_state_service_bound)) {
    skipped_reason = "none";
    return true;
  }

  const auto core_location_it = ntn_core_location_reporting_states.find(ue_index);
  if (core_location_it != ntn_core_location_reporting_states.end() &&
      !core_location_it->second.active_requests.empty()) {
    skipped_reason = "none";
    return true;
  }

  const auto relocation_it = ntn_pre_service_relocation_states.find(ue_index);
  if (relocation_it != ntn_pre_service_relocation_states.end() &&
      (relocation_it->second.state == "preparing" || relocation_it->second.state == "pending_pre_service" ||
       relocation_it->second.state == "failed_retryable")) {
    skipped_reason = "none";
    return true;
  }

  const auto connected_handover_it = ntn_connected_handover_states.find(ue_index);
  if (connected_handover_it != ntn_connected_handover_states.end() &&
      (connected_handover_it->second.state == "target_preloading" ||
       connected_handover_it->second.state == "target_resource_preparing" ||
       connected_handover_it->second.state == "target_resource_applied" ||
       connected_handover_it->second.state == "handover_preparing")) {
    skipped_reason = "none";
    return true;
  }

  skipped_reason = "not_needed";
  return false;
}

void cu_cp_impl::refresh_ntn_rrc_location_request_for_ue(ue_index_t ue_index, const char* reason)
{
  cu_cp_ue* ue = ue_mng.find_du_ue(ue_index);
  if (ue == nullptr) {
    return;
  }

  ntn_rrc_location_request_ue_state& request_state = ntn_rrc_location_request_states[ue_index];
  std::string skipped_reason;
  bool desired = false;
  if (is_ntn_rrc_location_request_capability_allowed(*ue, skipped_reason)) {
    desired = is_ntn_rrc_location_request_desired(ue_index, *ue, skipped_reason);
  }
  request_state.desired        = desired;
  request_state.skipped_reason = desired ? "none" : skipped_reason;

  if (request_state.pending || request_state.configured == desired) {
    return;
  }

  if (ue->get_rrc_ue() == nullptr || ue->get_ue_context().reconfiguration_disabled) {
    request_state.pending = true;
    ++nof_ntn_rrc_location_request_configs_skipped_state;
    logger.debug("ue={}: Deferring NTN RRC location request reconfiguration. reason={} skipped={}",
                 ue_index,
                 reason,
                 request_state.skipped_reason);
    return;
  }

  const bool previous_configured = request_state.configured;
  request_state.pending          = true;
  if (!ue->get_task_sched().schedule_async_task(
          launch_async([this, ue_index, previous_configured](coro_context<async_task<void>>& ctx) mutable {
            cu_cp_ue*                             current_ue = nullptr;
            rrc_reconfiguration_procedure_request rrc_reconfig_args;
            bool                                  rrc_reconfig_result = false;

            CORO_BEGIN(ctx);

            current_ue = ue_mng.find_du_ue(ue_index);
            if (current_ue == nullptr || current_ue->get_rrc_ue() == nullptr ||
                current_ue->get_ue_context().reconfiguration_disabled) {
              ntn_rrc_location_request_ue_state& state = ntn_rrc_location_request_states[ue_index];
              state.configured = previous_configured;
              state.pending    = true;
              ++nof_ntn_rrc_location_request_reconfig_failed;
              CORO_EARLY_RETURN();
            }

            rrc_reconfig_args          = {};
            rrc_reconfig_args.meas_cfg = current_ue->get_rrc_ue()->generate_meas_config();
            ntn_rrc_location_request_states[ue_index].pending = true;
            CORO_AWAIT_VALUE(rrc_reconfig_result,
                             current_ue->get_rrc_ue()->handle_rrc_reconfiguration_request(rrc_reconfig_args));

            if (!rrc_reconfig_result) {
              ntn_rrc_location_request_ue_state& state = ntn_rrc_location_request_states[ue_index];
              state.configured = previous_configured;
              state.pending    = true;
              ++nof_ntn_rrc_location_request_reconfig_failed;
              CORO_EARLY_RETURN();
            }

            ntn_rrc_location_request_ue_state& state = ntn_rrc_location_request_states[ue_index];
            state.pending = false;
            CORO_RETURN();
          }))) {
    request_state.configured = previous_configured;
    request_state.pending    = true;
    ++nof_ntn_rrc_location_request_reconfig_failed;
    return;
  }

  ++nof_ntn_rrc_location_request_reconfig_sent;
}

std::chrono::milliseconds cu_cp_impl::get_ntn_location_lost_release_grace_period() const
{
  const auto& ntn_cfg = cfg.mobility.meas_manager_config.ntn_location_mobility;
  if (ntn_cfg.location_lost_release_grace_period.count() > 0) {
    return ntn_cfg.location_lost_release_grace_period;
  }
  if (ntn_cfg.measurement_report_period.count() > 0) {
    return std::max(std::chrono::milliseconds{ntn_cfg.measurement_report_period.count() * 3},
                    std::chrono::milliseconds{10000});
  }
  return std::chrono::seconds{10};
}

bool cu_cp_impl::is_ntn_location_watchdog_enabled() const
{
  const auto& ntn_cfg = cfg.mobility.meas_manager_config.ntn_location_mobility;
  return ntn_cfg.enabled && ntn_cfg.location_max_age.count() > 0;
}

bool cu_cp_impl::is_ntn_location_watchdog_release_temporarily_blocked(ue_index_t ue_index) const
{
  if (ntn_release_allowed_release_requested_ues.count(ue_index) != 0 ||
      ntn_location_watchdog_release_requested_ues.count(ue_index) != 0) {
    return true;
  }

  const auto relocation_it = ntn_pre_service_relocation_states.find(ue_index);
  if (relocation_it != ntn_pre_service_relocation_states.end() &&
      (relocation_it->second.state == "pending_pre_service" || relocation_it->second.state == "preparing" ||
       relocation_it->second.state == "failed_retryable" || relocation_it->second.state == "blocked")) {
    return true;
  }

  const auto handover_it = ntn_connected_handover_states.find(ue_index);
  if (handover_it != ntn_connected_handover_states.end() &&
      (handover_it->second.state == "target_preloading" ||
       handover_it->second.state == "target_resource_preparing" ||
       handover_it->second.state == "target_resource_applied" ||
       handover_it->second.state == "handover_preparing" ||
       handover_it->second.state == "failed_retryable" ||
       handover_it->second.state == "blocked")) {
    return true;
  }

  return false;
}

void cu_cp_impl::refresh_ntn_location_freshness_watchdog(const char* reason)
{
  static_cast<void>(reason);
  if (!is_ntn_location_watchdog_enabled()) {
    return;
  }

  const auto& ntn_cfg = cfg.mobility.meas_manager_config.ntn_location_mobility;
  const auto  now     = std::chrono::steady_clock::now();
  const std::chrono::milliseconds release_grace = get_ntn_location_lost_release_grace_period();
  cu_cp_ue_context_release_batch_command release_command;
  std::vector<ue_index_t>                release_ues_to_track;

  for (const auto& layer_entry : ntn_ue_layer_states) {
    const ue_index_t                         ue_index = layer_entry.first;
    const ntn_ue_access_service_layer_state& layer    = layer_entry.second;
    ntn_location_freshness_ue_state& freshness_state   = ntn_location_freshness_states[ue_index];

    if (layer.service_state != ntn_service_state_service_bound) {
      freshness_state.state                = ntn_location_freshness_not_required;
      freshness_state.reason               = ntn_location_freshness_not_required;
      freshness_state.first_unfresh_time.reset();
      freshness_state.release_pending = false;
      freshness_state.location_age.reset();
      continue;
    }

    ++nof_ntn_location_watchdog_evaluations;
    cu_cp_ue* ue = ue_mng.find_du_ue(ue_index);
    if (ue == nullptr) {
      freshness_state.state  = ntn_location_freshness_missing;
      freshness_state.reason = "ue_not_found";
      freshness_state.location_age.reset();
      continue;
    }

    const std::optional<ntn_ue_location_report>& last_location = ue->get_meas_context().last_ntn_location_report;
    if (last_location.has_value() && is_ntn_location_report_fresh(ntn_cfg, last_location.value(), now)) {
      freshness_state.state  = ntn_location_freshness_fresh;
      freshness_state.reason = ntn_location_freshness_fresh;
      freshness_state.first_unfresh_time.reset();
      freshness_state.release_pending = false;
      freshness_state.location_age =
          last_location->received_time >= now
              ? std::chrono::milliseconds{0}
              : std::chrono::duration_cast<std::chrono::milliseconds>(now - last_location->received_time);
      continue;
    }

    const bool has_last_location = last_location.has_value();
    freshness_state.state        = has_last_location ? ntn_location_freshness_stale : ntn_location_freshness_missing;
    freshness_state.reason       = has_last_location ? "last_location_expired" : "missing_location";
    if (has_last_location) {
      freshness_state.location_age =
          last_location->received_time >= now
              ? std::chrono::milliseconds{0}
              : std::chrono::duration_cast<std::chrono::milliseconds>(now - last_location->received_time);
      freshness_state.first_unfresh_time = last_location->received_time + ntn_cfg.location_max_age;
    } else if (!freshness_state.first_unfresh_time.has_value()) {
      freshness_state.first_unfresh_time = now;
      freshness_state.location_age.reset();
    }

    const bool release_grace_expired =
        freshness_state.first_unfresh_time.has_value() && now >= freshness_state.first_unfresh_time.value() &&
        now - freshness_state.first_unfresh_time.value() >= release_grace;
    if (!release_grace_expired) {
      ++nof_ntn_location_watchdog_refresh_requested;
      refresh_ntn_rrc_location_request_for_ue(ue_index, "location_freshness_watchdog");
      freshness_state.release_pending = false;
      continue;
    }

    freshness_state.state           = ntn_location_freshness_release_pending;
    freshness_state.release_pending = true;
    ++nof_ntn_location_watchdog_release_requested;
    last_ntn_location_watchdog_release_reason = freshness_state.reason;

    if (is_ntn_location_watchdog_release_temporarily_blocked(ue_index)) {
      ++nof_ntn_location_watchdog_release_skipped;
      continue;
    }

    release_command.ues.push_back({ue_index, ngap_cause_radio_network_t::release_due_to_ngran_generated_reason});
    release_ues_to_track.push_back(ue_index);
  }

  if (release_command.ues.empty()) {
    return;
  }

  for (ue_index_t ue_index : release_ues_to_track) {
    ntn_location_watchdog_release_requested_ues.insert(ue_index);
  }
  const unsigned nof_releases_to_schedule = release_command.ues.size();
  auto           completion_task =
      [this, release_command = std::move(release_command)](coro_context<async_task<void>>& ctx) mutable {
        cu_cp_ue_context_release_batch_response response;
        CORO_BEGIN(ctx);
        CORO_AWAIT_VALUE(response, release_ues(release_command));
        for (ue_index_t ue_index : response.ues_not_found) {
          ntn_location_watchdog_release_requested_ues.erase(ue_index);
        }
        for (ue_index_t ue_index : response.duplicate_ues) {
          ntn_location_watchdog_release_requested_ues.erase(ue_index);
        }
        for (ue_index_t ue_index : response.failed_to_schedule_ues) {
          ntn_location_watchdog_release_requested_ues.erase(ue_index);
        }
        CORO_RETURN();
      };

  if (!common_task_sched.schedule_async_task(launch_async(std::move(completion_task)))) {
    for (ue_index_t ue_index : release_ues_to_track) {
      ntn_location_watchdog_release_requested_ues.erase(ue_index);
      ntn_location_freshness_states[ue_index].release_pending = false;
    }
    nof_ntn_location_watchdog_release_skipped += nof_releases_to_schedule;
    return;
  }

  nof_ntn_location_watchdog_release_scheduled += nof_releases_to_schedule;
}

std::optional<rrc_meas_cfg>
cu_cp_impl::handle_measurement_config_request(ue_index_t                         ue_index,
                                              nr_cell_identity                   nci,
                                              const std::optional<rrc_meas_cfg>& current_meas_config)
{
  std::optional<rrc_meas_cfg> meas_cfg = cell_meas_mng.get_measurement_config(ue_index, nci, current_meas_config);
  if (!meas_cfg.has_value() || !cfg.mobility.meas_manager_config.ntn_location_mobility.enabled) {
    return meas_cfg;
  }

  cu_cp_ue* ue = ue_mng.find_ue(ue_index);
  if (ue == nullptr) {
    return meas_cfg;
  }

  ntn_rrc_location_request_ue_state& request_state = ntn_rrc_location_request_states[ue_index];
  const bool                         was_configured = request_state.configured;

  std::string skipped_reason;
  if (!is_ntn_rrc_location_request_capability_allowed(*ue, skipped_reason)) {
    disable_common_location_info_in_periodic_reports(meas_cfg.value());
    request_state.desired        = false;
    request_state.configured     = false;
    request_state.pending        = false;
    request_state.skipped_reason = skipped_reason;
    ++nof_ntn_rrc_location_request_configs_skipped_capability;
    if (was_configured) {
      ++nof_ntn_rrc_location_request_configs_removed;
    }
    return meas_cfg;
  }

  if (!is_ntn_rrc_location_request_desired(ue_index, *ue, skipped_reason)) {
    disable_common_location_info_in_periodic_reports(meas_cfg.value());
    request_state.desired        = false;
    request_state.configured     = false;
    request_state.pending        = false;
    request_state.skipped_reason = skipped_reason;
    ++nof_ntn_rrc_location_request_configs_skipped_state;
    if (was_configured) {
      ++nof_ntn_rrc_location_request_configs_removed;
    }
    return meas_cfg;
  }

  enable_common_location_info_in_periodic_reports(meas_cfg.value());
  const bool configured = meas_config_has_common_location_info_in_periodic_reports(meas_cfg.value());
  request_state.desired        = true;
  request_state.configured     = configured;
  request_state.pending        = false;
  request_state.skipped_reason = "none";
  if (configured && !was_configured) {
    ++nof_ntn_rrc_location_request_configs_included;
  }
  return meas_cfg;
}

void cu_cp_impl::handle_measurement_report(const ue_index_t ue_index, const rrc_meas_results& meas_results)
{
  cell_meas_mng.report_measurement(ue_index, meas_results);
}

void cu_cp_impl::handle_ue_capability_update(ue_index_t ue_index)
{
  refresh_ntn_rrc_location_request_for_ue(ue_index, "ue_capability_updated");
}

void cu_cp_impl::handle_rrc_ue_location_report_outcome(ue_index_t ue_index, ntn_rrc_ue_location_report_outcome outcome)
{
  static_cast<void>(ue_index);
  switch (outcome) {
    case ntn_rrc_ue_location_report_outcome::received:
      ++nof_ntn_rrc_location_reports_received;
      break;
    case ntn_rrc_ue_location_report_outcome::decoded:
      ++nof_ntn_rrc_location_reports_decoded;
      break;
    case ntn_rrc_ue_location_report_outcome::unsupported:
      ++nof_ntn_rrc_location_reports_unsupported;
      break;
    case ntn_rrc_ue_location_report_outcome::decode_failed:
      ++nof_ntn_rrc_location_reports_decode_failed;
      break;
  }
}

void cu_cp_impl::handle_ue_location_report(const ntn_ue_location_report& location_report)
{
  std::optional<std::string> serving_beam_id = find_ntn_beam_id_by_nci(location_report.serving_nci);
  if (block_new_ntn_demand_if_ntn_policy_blocks("location handover", serving_beam_id, location_report.serving_nci)) {
    ++nof_ntn_location_reports_rejected;
    logger.debug("ue={}: NTN location report ignored. Cause: NTN service policy", location_report.ue_index);
    return;
  }

  const ntn_location_report_result result = cell_meas_mng.report_ue_location(location_report);
  if (result != ntn_location_report_result::accepted) {
    ++nof_ntn_location_reports_rejected;
    logger.debug("ue={}: NTN location report filtered before core-network reporting. result={}",
                 location_report.ue_index,
                 static_cast<unsigned>(result));
    return;
  }

  ++nof_ntn_location_reports_accepted;
  refresh_ntn_location_freshness_watchdog("location_report_accepted");
  refresh_ntn_beam_placement_for_current_load();
  schedule_ntn_multi_beam_load_balancing_handovers();
  report_ntn_location_to_core_if_required(location_report);
}

void cu_cp_impl::handle_neighbor_better_than_spcell(ue_index_t       ue_index,
                                                    gnb_id_t         neighbor_gnb_id,
                                                    nr_cell_identity neighbor_nci,
                                                    pci_t            neighbor_pci)
{
  mobility_mng.handle_neighbor_better_than_spcell(ue_index, neighbor_gnb_id, neighbor_nci, neighbor_pci);
}

bool cu_cp_impl::handle_ntn_location_handover_required(const ntn_location_handover_trigger& trigger)
{
  ntn_location_handover_trigger prepared_trigger = trigger;
  if (!prepare_ntn_connected_handover(prepared_trigger)) {
    return false;
  }

  const bool accepted = mobility_mng.handle_ntn_location_handover_required(prepared_trigger);
  auto       state_it = ntn_connected_handover_states.find(trigger.ue_index);
  if (state_it != ntn_connected_handover_states.end()) {
    if (accepted) {
      state_it->second.state = "target_resource_preparing";
    } else {
      state_it->second.state = "failed_retryable";
      state_it->second.target_resource_state = "rollback_restored";
      ntn_service_resource_mng.rollback_handover_target_rnti(trigger.ue_index, "handover_request_rejected");
      ++state_it->second.retry_count;
      refresh_ntn_beam_placement_for_current_load();
    }
  }
  return accepted;
}

void cu_cp_impl::handle_ntn_served_beams_updated(const std::vector<std::string>& beam_ids)
{
  mobility_mng.handle_ntn_served_beams_updated(beam_ids);
}

void cu_cp_impl::handle_ntn_beam_placement_plan_updated(const ntn_beam_placement_plan& plan)
{
  mobility_mng.handle_ntn_beam_placement_plan_updated(plan);
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
  std::optional<tac_t> beam_derived_tac;

  const auto& ntn_cfg = cfg.mobility.meas_manager_config.ntn_location_mobility;
  if (ntn_cfg.enabled) {
    const auto beam_it = std::find_if(ntn_cfg.beams.begin(), ntn_cfg.beams.end(), [&report](const ntn_beam_position& beam) {
      return beam.nci.value() == report.serving_nci.value();
    });
    if (beam_it != ntn_cfg.beams.end()) {
      const ntn_beam_tac_result derived_tac = derive_ntn_beam_tac(beam_it->beam_id);
      if (derived_tac.tac.has_value()) {
        beam_derived_tac = derived_tac.tac;
        tac              = derived_tac.tac;
      } else {
        logger.debug("ue={}: Cannot derive NTN TAC from beam_id={}. Cause: {}",
                     report.ue_index,
                     beam_it->beam_id,
                     derived_tac.reason == ntn_beam_tac_invalid_reason::missing_decimal_suffix
                         ? "missing_decimal_suffix"
                         : "out_of_range");
      }
    } else {
      logger.debug("ue={}: Cannot derive NTN TAC. Cause: no beam for nci={:#x}", report.ue_index, report.serving_nci);
    }
  }

  if (!tac.has_value()) {
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
    user_location.ntn_derived_tac = beam_derived_tac.value_or(tac.value());
  }

  return user_location;
}

std::optional<cu_cp_user_location_info_nr> cu_cp_impl::build_ntn_release_user_location_info(ue_index_t ue_index)
{
  const auto& ntn_cfg = cfg.mobility.meas_manager_config.ntn_location_mobility;
  if (!ntn_cfg.enabled || is_current_ntn_assistance_stale(std::chrono::steady_clock::now())) {
    return std::nullopt;
  }

  const cu_cp_ue* ue = ue_mng.find_ue(ue_index);
  if (ue == nullptr) {
    return std::nullopt;
  }
  const ntn_ue_capability_summary capability = evaluate_ntn_ue_capability_for_ue(*ue);
  if (capability.state != ntn_ue_capability_state::supported || !capability.matched_deployment_profile) {
    return std::nullopt;
  }

  std::optional<ntn_ue_location_report> last_report = cell_meas_mng.get_last_ue_location_report(ue_index);
  if (!last_report.has_value()) {
    return std::nullopt;
  }

  return build_ntn_core_user_location_info(last_report.value());
}

std::optional<cu_cp_info_on_recommended_cells_and_ran_nodes_for_paging>
cu_cp_impl::build_ntn_paging_recommendation(ue_index_t ue_index)
{
  const auto& ntn_cfg = cfg.mobility.meas_manager_config.ntn_location_mobility;
  if (!ntn_cfg.enabled || is_current_ntn_assistance_stale(std::chrono::steady_clock::now())) {
    return std::nullopt;
  }

  std::optional<ntn_ue_location_report> last_report = cell_meas_mng.get_last_ue_location_report(ue_index);
  if (!last_report.has_value()) {
    return std::nullopt;
  }

  std::optional<std::string> beam_id = find_ntn_beam_id_by_nci(last_report->serving_nci);
  if (!beam_id.has_value()) {
    return std::nullopt;
  }

  const ntn_beam_tac_result derived_tac = derive_ntn_beam_tac(beam_id.value());
  if (!derived_tac.tac.has_value()) {
    return std::nullopt;
  }

  const std::vector<cu_cp_ntn_beam_status> beam_status = get_current_ntn_beam_status();
  const auto status_it = std::find_if(beam_status.begin(), beam_status.end(), [&beam_id](const auto& status) {
    return status.beam_id == beam_id.value();
  });
  if (status_it == beam_status.end()) {
    return std::nullopt;
  }
  if (!status_it->paging_recommendable) {
    return std::nullopt;
  }

  std::optional<cu_cp_user_location_info_nr> user_location = build_ntn_core_user_location_info(last_report.value());
  if (!user_location.has_value()) {
    return std::nullopt;
  }

  cu_cp_info_on_recommended_cells_and_ran_nodes_for_paging recommendation;
  cu_cp_recommended_cell_item recommended_cell;
  recommended_cell.ngran_cgi = user_location->nr_cgi;
  recommendation.recommended_cells_for_paging.recommended_cell_list.push_back(recommended_cell);
  return recommendation;
}

void cu_cp_impl::store_ntn_paired_access_context_for_paging(const cu_cp_five_g_s_tmsi&   five_g_s_tmsi,
                                                            const cu_cp_ntn_beam_status& selected_beam,
                                                            const char*                  reason)
{
  if (!selected_beam.paired_uplink_access_ready || selected_beam.paired_uplink_beam_id.empty() ||
      !selected_beam.paired_uplink_nci.has_value()) {
    return;
  }

  ntn_idle_paging_context context;
  context.five_g_s_tmsi                 = five_g_s_tmsi;
  context.last_service_beam_id          = selected_beam.beam_id;
  context.last_downlink_wake_beam_id    = selected_beam.beam_id;
  context.paired_uplink_access_beam_id  = selected_beam.paired_uplink_beam_id;
  context.paired_uplink_access_nci      = selected_beam.paired_uplink_nci;
  context.paired_uplink_access_du_index = selected_beam.paired_uplink_du_index;
  context.paired_access_reason          = selected_beam.access_pair_reason;
  if (!selected_beam.analog_beam_id.empty()) {
    context.last_access_analog_beam_id = selected_beam.analog_beam_id;
  }
  context.last_serving_nci = selected_beam.nci;
  context.updated_time     = std::chrono::steady_clock::now();
  context.invalid_reason   = reason != nullptr ? reason : "paired_access";
  if (selected_beam.derived_tac.has_value()) {
    context.last_derived_tac = selected_beam.derived_tac.value();
  }

  ntn_pending_paired_access_contexts[five_g_s_tmsi.to_number()] = context;
  if (auto idle_it = ntn_idle_paging_contexts.find(five_g_s_tmsi.to_number()); idle_it != ntn_idle_paging_contexts.end()) {
    idle_it->second.last_downlink_wake_beam_id    = context.last_downlink_wake_beam_id;
    idle_it->second.paired_uplink_access_beam_id  = context.paired_uplink_access_beam_id;
    idle_it->second.paired_uplink_access_nci      = context.paired_uplink_access_nci;
    idle_it->second.paired_uplink_access_du_index = context.paired_uplink_access_du_index;
    idle_it->second.paired_access_reason          = context.paired_access_reason;
    idle_it->second.updated_time                  = context.updated_time;
  }
  last_ntn_paired_access_reason = context.invalid_reason;
}

bool cu_cp_impl::apply_ntn_paired_access_context_for_ue(ue_index_t ue_index, ntn_ue_access_service_layer_state& layer)
{
  const auto tmsi_it = ntn_connected_ue_five_g_s_tmsi.find(ue_index);
  if (tmsi_it == ntn_connected_ue_five_g_s_tmsi.end()) {
    return false;
  }

  const uint64_t tmsi_number = tmsi_it->second.to_number();
  const ntn_idle_paging_context* context = nullptr;
  if (const auto pending_it = ntn_pending_paired_access_contexts.find(tmsi_number);
      pending_it != ntn_pending_paired_access_contexts.end()) {
    context = &pending_it->second;
  } else if (const auto idle_it = ntn_idle_paging_contexts.find(tmsi_number);
             idle_it != ntn_idle_paging_contexts.end()) {
    context = &idle_it->second;
  }
  if (context == nullptr || !context->last_downlink_wake_beam_id.has_value() ||
      !context->paired_uplink_access_beam_id.has_value() || !context->paired_uplink_access_nci.has_value()) {
    return false;
  }

  if (!layer.has_access_nci || layer.access_nci != context->paired_uplink_access_nci.value()) {
    last_ntn_paired_access_reason = "paired_access_nci_mismatch";
    return false;
  }

  const std::vector<cu_cp_ntn_beam_status> beam_status = get_current_ntn_beam_status();
  auto find_beam = [&beam_status](const std::string& beam_id) -> const cu_cp_ntn_beam_status* {
    const auto it = std::find_if(beam_status.begin(), beam_status.end(), [&beam_id](const auto& status) {
      return status.beam_id == beam_id;
    });
    return it == beam_status.end() ? nullptr : &*it;
  };

  const cu_cp_ntn_beam_status* downlink_wake_beam = find_beam(context->last_downlink_wake_beam_id.value());
  const cu_cp_ntn_beam_status* paired_uplink_beam = find_beam(context->paired_uplink_access_beam_id.value());
  if (downlink_wake_beam == nullptr || paired_uplink_beam == nullptr) {
    last_ntn_paired_access_reason = "paired_access_beam_missing";
    return false;
  }
  if (!downlink_wake_beam->paired_uplink_access_ready ||
      downlink_wake_beam->paired_uplink_beam_id != context->paired_uplink_access_beam_id.value()) {
    last_ntn_paired_access_reason = "paired_access_stale";
    return false;
  }
  if (paired_uplink_beam->state == cu_cp_ntn_beam_assignment_state::draining ||
      paired_uplink_beam->state == cu_cp_ntn_beam_assignment_state::inactive || paired_uplink_beam->drain_forced ||
      !paired_uplink_beam->uplink_access_ready) {
    last_ntn_paired_access_reason = "paired_uplink_unavailable";
    return false;
  }

  layer.last_downlink_wake_beam_id        = context->last_downlink_wake_beam_id.value();
  layer.paired_uplink_access_beam_id      = context->paired_uplink_access_beam_id.value();
  layer.paired_uplink_access_nci          = context->paired_uplink_access_nci.value();
  layer.has_paired_uplink_access_nci      = true;
  layer.paired_uplink_access_du_index     = context->paired_uplink_access_du_index;
  layer.paired_access_reason              = context->paired_access_reason;
  if (layer.access_reason == "valid" || layer.access_reason == "none") {
    layer.access_reason = "paired_uplink_response";
  }
  last_ntn_paired_access_reason = "paired_uplink_response";
  return true;
}

void cu_cp_impl::mark_ntn_access_released_after_ics(ntn_ue_access_service_layer_state& layer)
{
  layer.access_state = ntn_access_state_released_after_ics;
  if (!layer.paired_uplink_access_beam_id.empty()) {
    layer.access_reason = "paired_uplink_response";
    return;
  }
  layer.access_reason = ntn_access_state_released_after_ics;
}

void cu_cp_impl::persist_ntn_idle_paging_context_for_ue(ue_index_t ue_index, const char* reason)
{
  const auto& ntn_cfg = cfg.mobility.meas_manager_config.ntn_location_mobility;
  if (!ntn_cfg.enabled) {
    return;
  }

  const cu_cp_ue* ue = ue_mng.find_ue(ue_index);
  if (ue == nullptr) {
    return;
  }

  const ntn_ue_capability_summary capability = evaluate_ntn_ue_capability_for_ue(*ue);
  if (capability.state != ntn_ue_capability_state::supported || !capability.matched_deployment_profile) {
    return;
  }

  const auto tmsi_it = ntn_connected_ue_five_g_s_tmsi.find(ue_index);
  if (tmsi_it == ntn_connected_ue_five_g_s_tmsi.end()) {
    return;
  }

  const auto layer_it = ntn_ue_layer_states.find(ue_index);
  std::optional<ntn_ue_location_report> last_report = cell_meas_mng.get_last_ue_location_report(ue_index);
  if (layer_it == ntn_ue_layer_states.end() && !last_report.has_value()) {
    return;
  }

  ntn_idle_paging_context context;
  context.five_g_s_tmsi = tmsi_it->second;
  context.updated_time  = std::chrono::steady_clock::now();
  context.invalid_reason = reason != nullptr ? reason : "none";
  context.last_location = last_report;

  if (layer_it != ntn_ue_layer_states.end()) {
    const ntn_ue_access_service_layer_state& layer = layer_it->second;
    if (!layer.service_digital_beam_id.empty()) {
      context.last_service_beam_id = layer.service_digital_beam_id;
    }
    if (!layer.access_analog_beam_id.empty()) {
      context.last_access_analog_beam_id = layer.access_analog_beam_id;
    }
    if (!layer.last_downlink_wake_beam_id.empty()) {
      context.last_downlink_wake_beam_id = layer.last_downlink_wake_beam_id;
    }
    if (!layer.paired_uplink_access_beam_id.empty()) {
      context.paired_uplink_access_beam_id  = layer.paired_uplink_access_beam_id;
      context.paired_uplink_access_du_index = layer.paired_uplink_access_du_index;
      context.paired_access_reason          = layer.paired_access_reason;
    }
    if (layer.has_paired_uplink_access_nci) {
      context.paired_uplink_access_nci = layer.paired_uplink_access_nci;
    }
    if (layer.has_service_nci) {
      context.last_serving_nci = layer.service_nci;
    } else if (layer.has_access_nci) {
      context.last_serving_nci = layer.access_nci;
    }
  }

  if (!context.last_serving_nci.has_value() && last_report.has_value()) {
    context.last_serving_nci = last_report->serving_nci;
  }
  if (!context.last_service_beam_id.has_value() && context.last_serving_nci.has_value()) {
    context.last_service_beam_id = find_ntn_beam_id_by_nci(context.last_serving_nci.value());
  }
  if (context.last_service_beam_id.has_value()) {
    const ntn_beam_tac_result tac = derive_ntn_beam_tac(context.last_service_beam_id.value());
    if (tac.tac.has_value()) {
      context.last_derived_tac = tac.tac.value();
    }
  }

  ntn_idle_paging_contexts[context.five_g_s_tmsi->to_number()] = std::move(context);
}

bool cu_cp_impl::is_ntn_inactive_suspend_eligible(const cu_cp_ue& ue, std::string& reason)
{
  const auto& ntn_cfg = cfg.mobility.meas_manager_config.ntn_location_mobility;
  if (!ntn_cfg.enabled) {
    reason = "ntn_disabled";
    return false;
  }

  const ntn_ue_capability_summary capability = evaluate_ntn_ue_capability_for_ue(ue);
  if (capability.state != ntn_ue_capability_state::supported || !capability.matched_deployment_profile) {
    reason = "capability_or_profile_blocked";
    return false;
  }

  const auto layer_it = ntn_ue_layer_states.find(ue.get_ue_index());
  if (layer_it == ntn_ue_layer_states.end()) {
    reason = "no_ntn_layer_context";
    return false;
  }
  const ntn_ue_access_service_layer_state& layer = layer_it->second;
  const bool has_ntn_service_or_control_context =
      layer.service_state == ntn_service_state_service_bound ||
      layer.access_state == ntn_access_state_released_after_ics ||
      layer.access_state == ntn_access_state_access_active;
  if (!has_ntn_service_or_control_context) {
    reason = "not_service_or_control_context";
    return false;
  }

  if (ntn_release_allowed_release_requested_ues.count(ue.get_ue_index()) != 0 ||
      ntn_location_watchdog_release_requested_ues.count(ue.get_ue_index()) != 0) {
    reason = "release_pending";
    return false;
  }

  const auto pre_service_it = ntn_pre_service_relocation_states.find(ue.get_ue_index());
  if (pre_service_it != ntn_pre_service_relocation_states.end() &&
      (pre_service_it->second.state == "preparing" || pre_service_it->second.state == "pending_pre_service")) {
    reason = "pre_service_relocation_pending";
    return false;
  }

  const auto handover_it = ntn_connected_handover_states.find(ue.get_ue_index());
  if (handover_it != ntn_connected_handover_states.end() &&
      (handover_it->second.state == "target_preloading" ||
       handover_it->second.state == "target_resource_preparing" ||
       handover_it->second.state == "target_resource_applied" ||
       handover_it->second.state == "handover_preparing")) {
    reason = "connected_handover_pending";
    return false;
  }

  ngap_interface* ngap = ngap_db.find_ngap(ue.get_ue_context().plmn);
  if (ngap == nullptr) {
    reason = "ngap_not_found";
    return false;
  }

  reason = "eligible";
  return true;
}

void cu_cp_impl::persist_ntn_inactive_context_for_ue(ue_index_t                    ue_index,
                                                     const rrc_ue_release_context& release_context,
                                                     const char*                   reason)
{
  const cu_cp_ue* ue = ue_mng.find_ue(ue_index);
  if (ue == nullptr) {
    return;
  }

  ntn_inactive_context context;
  context.updated_time = std::chrono::steady_clock::now();
  context.state        = "suspend_requested";
  context.reason       = reason != nullptr ? reason : "none";
  context.full_i_rnti  = release_context.full_i_rnti;
  context.short_i_rnti = release_context.short_i_rnti;

  const auto tmsi_it = ntn_connected_ue_five_g_s_tmsi.find(ue_index);
  if (tmsi_it != ntn_connected_ue_five_g_s_tmsi.end()) {
    context.five_g_s_tmsi = tmsi_it->second;
  }

  context.last_location = cell_meas_mng.get_last_ue_location_report(ue_index);

  const auto layer_it = ntn_ue_layer_states.find(ue_index);
  if (layer_it != ntn_ue_layer_states.end()) {
    const ntn_ue_access_service_layer_state& layer = layer_it->second;
    if (!layer.service_digital_beam_id.empty()) {
      context.last_service_beam_id = layer.service_digital_beam_id;
    }
    if (!layer.access_analog_beam_id.empty()) {
      context.last_access_analog_beam_id = layer.access_analog_beam_id;
    }
    if (!layer.last_downlink_wake_beam_id.empty()) {
      context.last_downlink_wake_beam_id = layer.last_downlink_wake_beam_id;
    }
    if (!layer.paired_uplink_access_beam_id.empty()) {
      context.paired_uplink_access_beam_id  = layer.paired_uplink_access_beam_id;
      context.paired_uplink_access_du_index = layer.paired_uplink_access_du_index;
      context.paired_access_reason          = layer.paired_access_reason;
    }
    if (layer.has_paired_uplink_access_nci) {
      context.paired_uplink_access_nci = layer.paired_uplink_access_nci;
    }
    if (layer.has_service_nci) {
      context.last_serving_nci = layer.service_nci;
    } else if (layer.has_access_nci) {
      context.last_serving_nci = layer.access_nci;
    }
  }

  if (!context.last_serving_nci.has_value() && context.last_location.has_value()) {
    context.last_serving_nci = context.last_location->serving_nci;
  }

  ntn_inactive_contexts[ue_index] = std::move(context);
}

bool cu_cp_impl::schedule_ntn_inactive_suspend_if_eligible(ue_index_t ue_index)
{
  cu_cp_ue* candidate_ue = ue_mng.find_du_ue(ue_index);
  if (candidate_ue == nullptr) {
    last_ntn_inactive_reason = "ue_not_found";
    return false;
  }

  std::string skipped_reason;
  if (!is_ntn_inactive_suspend_eligible(*candidate_ue, skipped_reason)) {
    last_ntn_inactive_reason = skipped_reason;
    return false;
  }

  ++nof_ntn_inactive_suspend_requested;
  last_ntn_inactive_reason = "suspend_requested";

  candidate_ue->get_task_sched().schedule_async_task(
      launch_async([this, ue_index](coro_context<async_task<void>>& ctx) mutable {
    cu_cp_ue*                   suspend_ue = nullptr;
    ngap_interface*             suspend_ngap = nullptr;
    rrc_ue_release_context      release_context;
    f1ap_ue_context_release_command f1ap_release_cmd;
    ue_index_t                 released_ue_index = ue_index_t::invalid;
    bool                       ngap_suspend_sent = false;
    cu_cp_ue_context_release_request fallback_release_req;

    CORO_BEGIN(ctx);

    suspend_ue = ue_mng.find_du_ue(ue_index);
    if (suspend_ue == nullptr || suspend_ue->get_rrc_ue() == nullptr) {
      ++nof_ntn_inactive_suspend_failed;
      last_ntn_inactive_reason = "ue_not_found";
      CORO_EARLY_RETURN();
    }

    suspend_ngap = ngap_db.find_ngap(suspend_ue->get_ue_context().plmn);
    if (suspend_ngap == nullptr) {
      ++nof_ntn_inactive_suspend_failed;
      ++nof_ntn_inactive_fallback_releases;
      last_ntn_inactive_reason = "ngap_not_found";
      fallback_release_req.ue_index = ue_index;
      fallback_release_req.cause    = ngap_cause_radio_network_t::user_inactivity;
      fallback_release_req.pdu_session_res_list_cxt_rel_req = suspend_ue->get_up_resource_manager().get_pdu_sessions();
      CORO_AWAIT(handle_ue_context_release(fallback_release_req));
      CORO_EARLY_RETURN();
    }

    release_context = suspend_ue->get_rrc_ue()->get_rrc_ue_inactive_release_context();
    if (release_context.rrc_release_pdu.empty()) {
      ++nof_ntn_inactive_suspend_failed;
      ++nof_ntn_inactive_fallback_releases;
      last_ntn_inactive_reason = "rrc_suspend_context_failed";
      fallback_release_req.ue_index = ue_index;
      fallback_release_req.cause    = ngap_cause_radio_network_t::user_inactivity;
      fallback_release_req.pdu_session_res_list_cxt_rel_req = suspend_ue->get_up_resource_manager().get_pdu_sessions();
      CORO_AWAIT(handle_ue_context_release(fallback_release_req));
      CORO_EARLY_RETURN();
    }

    persist_ntn_idle_paging_context_for_ue(ue_index, "inactive_suspend");
    persist_ntn_inactive_context_for_ue(ue_index, release_context, "inactive_suspend");

    f1ap_release_cmd.ue_index        = ue_index;
    f1ap_release_cmd.cause           = ngap_to_f1ap_cause(ngap_cause_radio_network_t::user_inactivity);
    f1ap_release_cmd.rrc_release_pdu = release_context.rrc_release_pdu.copy();
    f1ap_release_cmd.srb_id          = release_context.srb_id;

    CORO_AWAIT_VALUE(
        released_ue_index,
        du_db.get_du_processor(suspend_ue->get_du_index()).get_f1ap_handler().handle_ue_context_release_command(
            f1ap_release_cmd));
    if (released_ue_index == ue_index_t::invalid) {
      ++nof_ntn_inactive_suspend_failed;
      last_ntn_inactive_reason = "f1ap_release_failed";
      CORO_EARLY_RETURN();
    }

    suspend_ue = ue_mng.find_du_ue(ue_index);
    if (suspend_ue == nullptr) {
      ++nof_ntn_inactive_suspend_failed;
      last_ntn_inactive_reason = "ue_not_found_after_f1ap_release";
      CORO_EARLY_RETURN();
    }
    suspend_ngap = ngap_db.find_ngap(suspend_ue->get_ue_context().plmn);
    if (suspend_ngap == nullptr) {
      ++nof_ntn_inactive_suspend_failed;
      last_ntn_inactive_reason = "ngap_not_found_after_f1ap_release";
      CORO_EARLY_RETURN();
    }

    CORO_AWAIT_VALUE(ngap_suspend_sent,
                     suspend_ngap->get_ngap_control_message_handler().handle_ue_context_suspend_request(ue_index));
    if (!ngap_suspend_sent) {
      ++nof_ntn_inactive_suspend_failed;
      last_ntn_inactive_reason = "ngap_suspend_failed";
      CORO_EARLY_RETURN();
    }

    if (auto inactive_it = ntn_inactive_contexts.find(ue_index); inactive_it != ntn_inactive_contexts.end()) {
      inactive_it->second.state  = "suspended";
      inactive_it->second.reason = "ngap_suspend_sent";
    }
    ++nof_ntn_inactive_suspend_succeeded;
    last_ntn_inactive_reason = "suspended";
    CORO_RETURN();
  }));

  return true;
}

void cu_cp_impl::prune_expired_ntn_idle_paging_contexts(std::chrono::steady_clock::time_point now)
{
  const auto max_age = cfg.mobility.meas_manager_config.ntn_location_mobility.idle_paging_context_max_age;
  if (max_age.count() == 0) {
    return;
  }

  for (auto it = ntn_idle_paging_contexts.begin(); it != ntn_idle_paging_contexts.end();) {
    if (it->second.updated_time != std::chrono::steady_clock::time_point{} && now - it->second.updated_time > max_age) {
      it = ntn_idle_paging_contexts.erase(it);
      ++nof_ntn_idle_paging_expired;
    } else {
      ++it;
    }
  }

  for (auto it = ntn_inactive_contexts.begin(); it != ntn_inactive_contexts.end();) {
    if (it->second.updated_time != std::chrono::steady_clock::time_point{} && now - it->second.updated_time > max_age) {
      it = ntn_inactive_contexts.erase(it);
      ++nof_ntn_inactive_contexts_expired;
      last_ntn_inactive_reason = "context_expired";
    } else {
      ++it;
    }
  }
}

bool cu_cp_impl::apply_ntn_idle_paging_recommendation(cu_cp_paging_message& msg)
{
  const auto& ntn_cfg = cfg.mobility.meas_manager_config.ntn_location_mobility;
  if (!ntn_cfg.enabled) {
    return false;
  }

  prune_expired_ntn_idle_paging_contexts(std::chrono::steady_clock::now());

  const std::vector<cu_cp_ntn_beam_status> beam_status = get_current_ntn_beam_status();
  auto is_pageable_for_paging = [&msg](const cu_cp_ntn_beam_status& beam) {
    return beam.paging_recommendable && beam.derived_tac.has_value() &&
           ntn_paging_tai_list_contains_tac(msg.tai_list_for_paging, beam.derived_tac.value());
  };
  auto make_recommended_cell = [this](nr_cell_identity nci) -> std::optional<cu_cp_recommended_cell_item> {
    std::optional<cell_meas_config> cell_cfg = cell_meas_mng.get_cell_config(nci);
    if (!cell_cfg.has_value()) {
      return std::nullopt;
    }
    cu_cp_recommended_cell_item cell;
    cell.ngran_cgi = nr_cell_global_id_t{cell_cfg->serving_cell_cfg.plmn, nci};
    return cell;
  };
  auto paging_priority = [](const cu_cp_ntn_beam_status& beam) {
    if (beam.access_roundtrip_ready) {
      return 0;
    }
    if (beam.paired_uplink_access_ready) {
      return 1;
    }
    return 2;
  };
  auto find_best_pageable = [&beam_status, &is_pageable_for_paging, &paging_priority](const auto& predicate)
      -> std::optional<cu_cp_ntn_beam_status> {
    const cu_cp_ntn_beam_status* best = nullptr;
    int                         best_priority = 3;
    for (const cu_cp_ntn_beam_status& beam : beam_status) {
      if (!predicate(beam) || !is_pageable_for_paging(beam)) {
        continue;
      }
      const int priority = paging_priority(beam);
      if (best == nullptr || priority < best_priority) {
        best          = &beam;
        best_priority = priority;
      }
    }
    if (best == nullptr) {
      return std::nullopt;
    }
    return *best;
  };
  auto find_pageable_by_beam_id = [&find_best_pageable](const std::optional<std::string>& beam_id)
      -> std::optional<cu_cp_ntn_beam_status> {
    if (!beam_id.has_value()) {
      return std::nullopt;
    }
    return find_best_pageable([&beam_id](const cu_cp_ntn_beam_status& beam) { return beam.beam_id == beam_id.value(); });
  };
  auto find_pageable_by_analog = [&find_best_pageable](const std::optional<std::string>& analog_beam_id)
      -> std::optional<cu_cp_ntn_beam_status> {
    if (!analog_beam_id.has_value()) {
      return std::nullopt;
    }
    return find_best_pageable(
        [&analog_beam_id](const cu_cp_ntn_beam_status& beam) { return beam.analog_beam_id == analog_beam_id.value(); });
  };
  auto find_pageable_by_nci = [&find_best_pageable](nr_cell_identity nci) -> std::optional<cu_cp_ntn_beam_status> {
    return find_best_pageable([nci](const cu_cp_ntn_beam_status& beam) { return beam.nci == nci; });
  };
  auto find_pageable_by_tac = [&find_best_pageable]() -> std::optional<cu_cp_ntn_beam_status> {
    return find_best_pageable([](const cu_cp_ntn_beam_status&) { return true; });
  };
  auto find_pageable_amf_recommended_cell = [&msg, &find_pageable_by_nci]() -> std::optional<cu_cp_ntn_beam_status> {
    if (!msg.assist_data_for_paging.has_value() ||
        !msg.assist_data_for_paging->assist_data_for_recommended_cells.has_value()) {
      return std::nullopt;
    }
    const auto& recommended_cells =
        msg.assist_data_for_paging->assist_data_for_recommended_cells->recommended_cells_for_paging
            .recommended_cell_list;
    for (const cu_cp_recommended_cell_item& cell : recommended_cells) {
      if (std::optional<cu_cp_ntn_beam_status> beam = find_pageable_by_nci(cell.ngran_cgi.nci); beam.has_value()) {
        return beam;
      }
    }
    return std::nullopt;
  };

  std::optional<cu_cp_ntn_beam_status> selected_beam;
  const auto context_it = ntn_idle_paging_contexts.find(msg.ue_paging_id.to_number());
  if (context_it != ntn_idle_paging_contexts.end()) {
    ++nof_ntn_idle_paging_ue_hits;
    const auto inactive_it = std::find_if(
        ntn_inactive_contexts.begin(), ntn_inactive_contexts.end(), [&msg](const auto& inactive_context) {
          return inactive_context.second.five_g_s_tmsi.has_value() &&
                 inactive_context.second.five_g_s_tmsi->to_number() == msg.ue_paging_id.to_number();
        });
    if (inactive_it != ntn_inactive_contexts.end()) {
      ++nof_ntn_inactive_paging_hits;
    }
    selected_beam = find_pageable_by_beam_id(context_it->second.last_service_beam_id);
    if (!selected_beam.has_value()) {
      selected_beam = find_pageable_by_analog(context_it->second.last_access_analog_beam_id);
    }
  }
  const std::optional<cu_cp_ntn_beam_status> amf_recommended_beam = find_pageable_amf_recommended_cell();
  if (!selected_beam.has_value() && context_it == ntn_idle_paging_contexts.end() && amf_recommended_beam.has_value()) {
    store_ntn_paired_access_context_for_paging(msg.ue_paging_id, amf_recommended_beam.value(), "amf_recommended");
    last_ntn_idle_paging_reason = "amf_recommended_preserved";
    return false;
  }
  if (!selected_beam.has_value()) {
    selected_beam = find_pageable_by_tac();
    if (selected_beam.has_value()) {
      ++nof_ntn_idle_paging_tac_fallbacks;
    }
  }
  if (!selected_beam.has_value()) {
    ++nof_ntn_idle_paging_skipped;
    last_ntn_idle_paging_reason = "no_pageable_beam";
    return false;
  }

  std::optional<cu_cp_recommended_cell_item> recommended_cell = make_recommended_cell(selected_beam->nci);
  if (!recommended_cell.has_value()) {
    ++nof_ntn_idle_paging_skipped;
    last_ntn_idle_paging_reason = "cell_not_configured";
    return false;
  }

  if (!msg.assist_data_for_paging.has_value()) {
    msg.assist_data_for_paging.emplace();
  }
  if (!msg.assist_data_for_paging->assist_data_for_recommended_cells.has_value()) {
    msg.assist_data_for_paging->assist_data_for_recommended_cells.emplace();
  }
  auto& recommended_cells =
      msg.assist_data_for_paging->assist_data_for_recommended_cells->recommended_cells_for_paging.recommended_cell_list;
  recommended_cells.clear();
  recommended_cells.push_back(recommended_cell.value());
  store_ntn_paired_access_context_for_paging(
      msg.ue_paging_id, selected_beam.value(), context_it != ntn_idle_paging_contexts.end() ? "ue_context" : "tac_fallback");
  ++nof_ntn_idle_paging_recommendations;
  last_ntn_idle_paging_reason = context_it != ntn_idle_paging_contexts.end() ? "ue_context" : "tac_fallback";
  return true;
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

ntn_assistance_snapshot
cu_cp_impl::build_current_ntn_assistance_snapshot(std::chrono::steady_clock::time_point now) const
{
  const auto& ntn_cfg = cfg.mobility.meas_manager_config.ntn_location_mobility;
  if (!ntn_cfg.enabled) {
    ntn_assistance_snapshot snapshot;
    snapshot.valid          = false;
    snapshot.invalid_reason = ntn_assistance_invalid_reason::disabled;
    return snapshot;
  }

  std::chrono::milliseconds satellite_state_max_age{0};
  if (ntn_cfg.satellite_state_update.update_period.count() > 0) {
    satellite_state_max_age = ntn_cfg.satellite_state_update.update_period * 2;
  }

  ntn_assistance_snapshot_request request;
  request.satellite_ecef          = current_ntn_satellite_ecef;
  request.satellite_states        = current_ntn_satellite_states;
  request.satellite_epoch         = current_ntn_satellite_epoch;
  request.satellite_received_time = current_ntn_satellite_received_time;
  request.now                     = now;
  request.satellite_state_max_age = satellite_state_max_age;
  request.beams                   = ntn_cfg.beams;
  request.placement_plan          = current_ntn_beam_placement_plan;
  request.max_snapshot_beams      = 32;
  return build_ntn_assistance_snapshot(request);
}

ntn_sib19_assistance_snapshot
cu_cp_impl::build_current_ntn_sib19_assistance_snapshot(std::chrono::steady_clock::time_point now) const
{
  ntn_sib19_assistance_request request;
  request.assistance = build_current_ntn_assistance_snapshot(now);
  return build_ntn_sib19_assistance_snapshot(request);
}

ntn_sib19_broadcast_snapshot
cu_cp_impl::build_current_ntn_sib19_broadcast_snapshot(std::chrono::steady_clock::time_point now) const
{
  ntn_sib19_broadcast_request request;
  request.assistance = build_current_ntn_sib19_assistance_snapshot(now);
  std::set<std::string> known_beam_ids;
  for (const ntn_beam_du_assignment& assignment : current_ntn_beam_placement_plan.assignments) {
    if (known_beam_ids.insert(assignment.beam_id).second) {
      request.known_beams.push_back({assignment.beam_id, assignment.nci});
    }
  }
  for (const auto& record : ntn_sib19_broadcast_records) {
    if (known_beam_ids.insert(record.first).second) {
      request.known_beams.push_back({record.second.beam_id, record.second.nci});
    }
  }
  return build_ntn_sib19_broadcast_snapshot(request);
}

bool cu_cp_impl::is_current_ntn_assistance_stale(std::chrono::steady_clock::time_point now) const
{
  return build_current_ntn_assistance_snapshot(now).invalid_reason ==
         ntn_assistance_invalid_reason::stale_satellite_state;
}

bool cu_cp_impl::block_new_ntn_demand_if_assistance_is_stale(const char* demand_name)
{
  if (!is_current_ntn_assistance_stale(std::chrono::steady_clock::now())) {
    return false;
  }

  logger.warning("Blocking new NTN {} demand. Cause: stale satellite assistance", demand_name);
  if (!current_ntn_beam_placement_plan.assignments.empty() || !current_ntn_served_beam_candidates.empty()) {
    if (!update_ntn_served_beam_candidates({})) {
      logger.debug("Stale NTN assistance could not refresh beam placement into draining state");
    }
  }
  return true;
}

bool cu_cp_impl::block_new_ntn_demand_if_service_policy_blocks(const char*                demand_name,
                                                               std::optional<std::string> beam_id,
                                                               std::optional<nr_cell_identity> beam_nci)
{
  if (!beam_id.has_value()) {
    return false;
  }

  const bool blocked = beam_nci.has_value()
                           ? ntn_service_switch_over_ctrl.blocks_new_demand_for_beam(beam_id.value(), beam_nci.value())
                           : ntn_service_switch_over_ctrl.blocks_new_demand_for_beam(beam_id.value());
  if (!blocked) {
    return false;
  }

  logger.warning("Blocking new NTN {} demand for beam={}. Cause: service switch-over policy",
                 demand_name,
                 beam_id.value());
  refresh_ntn_beam_placement_for_service_policy();
  return true;
}

bool cu_cp_impl::block_new_ntn_demand_if_predictive_window_blocks(const char*                demand_name,
                                                                  std::optional<std::string> beam_id,
                                                                  std::optional<nr_cell_identity> beam_nci)
{
  if (!beam_id.has_value() && beam_nci.has_value()) {
    beam_id = find_ntn_beam_id_by_nci(beam_nci.value());
  }
  if (!beam_id.has_value() || !current_ntn_predictive_window_valid ||
      !contains_beam_id(current_ntn_predictive_drain_soon_beam_ids, beam_id.value())) {
    return false;
  }

  logger.warning("Blocking new NTN {} demand for beam={}. Cause: predictive service-window drain-soon",
                 demand_name,
                 beam_id.value());
  return true;
}

bool cu_cp_impl::block_new_ntn_demand_if_access_du_policy_blocks(const char*                demand_name,
                                                                 std::optional<std::string> beam_id,
                                                                 du_index_t                 ue_du_index,
                                                                 bool                       require_service_du)
{
  const auto& ntn_cfg = cfg.mobility.meas_manager_config.ntn_location_mobility;
  if (!ntn_cfg.enabled || ntn_cfg.analog_beams.empty() || !beam_id.has_value()) {
    return false;
  }

  const ntn_beam_du_assignment* assignment =
      find_assignment_for_beam(current_ntn_beam_placement_plan, beam_id.value());
  if (assignment == nullptr) {
    return false;
  }

  const bool access_du_matches =
      assignment->access_du_index != du_index_t::invalid && ue_du_index == assignment->access_du_index;
  if (!access_du_matches) {
    logger.warning("Blocking new NTN {} demand for beam={}. Cause: UE DU does not match selected analog access DU",
                   demand_name,
                   beam_id.value());
    return true;
  }

  if (!require_service_du) {
    return false;
  }

  const bool service_du_matches =
      assignment->du_index != du_index_t::invalid && ue_du_index == assignment->du_index &&
      assignment->du_assignment_reason == "eligible";
  if (service_du_matches) {
    return false;
  }

  logger.warning("Blocking new NTN {} demand for beam={}. Cause: service DU is not co-located with access DU ({})",
                 demand_name,
                 beam_id.value(),
                 assignment->du_assignment_reason);
  return true;
}

bool cu_cp_impl::block_new_ntn_access_if_resource_domain_blocks(std::optional<std::string> beam_id, ue_index_t ue_index)
{
  const auto& ntn_cfg = cfg.mobility.meas_manager_config.ntn_location_mobility;
  if (!ntn_cfg.enabled || !beam_id.has_value() || ntn_cfg.analog_beams.empty()) {
    return false;
  }

  const ntn_beam_position* beam = find_ntn_beam_cfg(ntn_cfg.beams, beam_id.value());
  if (beam == nullptr || beam->analog_beam_id.empty()) {
    return false;
  }
  const ntn_analog_beam_position* analog = find_ntn_analog_beam_cfg(ntn_cfg.analog_beams, beam->analog_beam_id);
  if (analog == nullptr) {
    return false;
  }
  const ntn_analog_beam_resource_policy policy = get_effective_analog_resource_policy(ntn_cfg, *analog);
  if (policy.max_access_only_ues == 0) {
    return false;
  }

  unsigned nof_access_active_ues = 0;
  for (const auto& entry : ntn_ue_layer_states) {
    if (entry.first == ue_index) {
      continue;
    }
    if (entry.second.access_analog_beam_id == beam->analog_beam_id &&
        entry.second.access_state == ntn_access_state_access_active) {
      ++nof_access_active_ues;
    }
  }
  if (nof_access_active_ues < policy.max_access_only_ues) {
    return false;
  }

  logger.warning("ue={}: Blocking new NTN access for analog beam={}. Cause: access-only UE resource cap exhausted",
                 ue_index,
                 beam->analog_beam_id);
  return true;
}

bool cu_cp_impl::block_new_ntn_access_if_rnti_pool_unavailable(std::optional<std::string> beam_id,
                                                               ue_index_t                 ue_index)
{
  const auto& ntn_cfg = cfg.mobility.meas_manager_config.ntn_location_mobility;
  if (!ntn_cfg.enabled || !beam_id.has_value() || ntn_cfg.analog_beams.empty()) {
    return false;
  }

  const ntn_beam_position* beam = find_ntn_beam_cfg(ntn_cfg.beams, beam_id.value());
  if (beam == nullptr || beam->analog_beam_id.empty()) {
    return false;
  }
  const ntn_analog_beam_position* analog = find_ntn_analog_beam_cfg(ntn_cfg.analog_beams, beam->analog_beam_id);
  if (analog == nullptr || !analog->enabled) {
    return false;
  }

  cu_cp_ue* ue = ue_mng.find_du_ue(ue_index);
  if (ue == nullptr || ue->get_du_index() == du_index_t::invalid ||
      ue->get_pcell_index() == srs_cu_cp::du_cell_index_t::invalid || ue->get_pci() == INVALID_PCI) {
    logger.warning("ue={}: Blocking new NTN access for analog beam={}. Cause: missing DU/cell/RNTI pool context",
                   ue_index,
                   beam->analog_beam_id);
    return true;
  }

  const srsran::du_cell_index_t cell_index = to_f1ap_du_cell_index(ue->get_pcell_index());
  if (ntn_service_resource_mng.is_access_rnti_pool_ready(
          ue->get_du_index(), cell_index, ue->get_pci(), beam->analog_beam_id)) {
    return false;
  }

  logger.warning("ue={}: Blocking new NTN access for analog beam={} du={} pci={}. Cause: rnti_pool_unavailable",
                 ue_index,
                 beam->analog_beam_id,
                 ue->get_du_index(),
                 ue->get_pci());
  return true;
}

bool cu_cp_impl::block_new_ntn_pdu_session_demand_if_access_du_policy_blocks(
    std::optional<std::string>    beam_id,
    du_index_t                    ue_du_index,
    unsigned                      expected_drbs,
    const ntn_qos_demand_summary& pending_qos)
{
  const auto& ntn_cfg = cfg.mobility.meas_manager_config.ntn_location_mobility;
  if (!ntn_cfg.enabled || ntn_cfg.analog_beams.empty() || !beam_id.has_value() ||
      current_ntn_served_beam_candidates.empty()) {
    return false;
  }

  const unsigned loaded_digital_service_beam_cap = get_ntn_loaded_digital_service_beam_cap(ntn_cfg);
  ntn_beam_placement_request request;
  request.beams            = ntn_cfg.beams;
  request.analog_beams     = ntn_cfg.analog_beams;
  request.visible_beams    = current_ntn_served_beam_candidates;
  request.max_active_beams = loaded_digital_service_beam_cap;
  request.resource_policy  = ntn_cfg.resource_policy;
  request.demand_aware_resource_weighting_enabled = ntn_cfg.demand_aware_beam_scheduling_enabled;
  request.du_capacities    = build_ntn_du_beam_capacities(du_db,
                                                           ue_mng,
                                                           ntn_cfg.beams,
                                                           loaded_digital_service_beam_cap,
                                                           cfg.admission.max_nof_ues,
                                                           cfg.admission.max_nof_drbs_per_ue);
  request.beam_loads       = build_ntn_beam_loads_for_current_service_contexts();
  merge_pending_ntn_beam_load(request.beam_loads, beam_id.value(), expected_drbs, pending_qos);
  request.previous_assignments = current_ntn_beam_placement_plan.assignments;

  const ntn_beam_placement_plan pending_plan = ntn_beam_planner.plan(request);
  const ntn_beam_du_assignment* assignment   = find_assignment_for_beam(pending_plan, beam_id.value());
  if (assignment != nullptr && assignment->bidirectional_service_ready &&
      assignment->access_du_index == ue_du_index && assignment->du_index == ue_du_index &&
      assignment->du_assignment_reason == "eligible") {
    return false;
  }

  logger.warning("Blocking new NTN PDU session demand for beam={}. Cause: pending access/service DU policy",
                 beam_id.value());
  return true;
}

bool cu_cp_impl::block_new_ntn_demand_if_ntn_policy_blocks(const char*                demand_name,
                                                           std::optional<std::string> beam_id,
                                                           std::optional<nr_cell_identity> beam_nci)
{
  if (!beam_id.has_value() && beam_nci.has_value()) {
    beam_id = find_ntn_beam_id_by_nci(beam_nci.value());
  }
  if (!beam_id.has_value()) {
    return false;
  }

  if (ntn_service_resource_mng.has_blocking_resource_repair()) {
    logger.warning("Blocking new NTN {} demand for beam={}. Cause: resource repair conflict", demand_name, beam_id.value());
    return true;
  }

  if (block_new_ntn_demand_if_assistance_is_stale(demand_name)) {
    return true;
  }
  if (block_new_ntn_demand_if_predictive_window_blocks(demand_name, beam_id, beam_nci)) {
    return true;
  }
  return block_new_ntn_demand_if_service_policy_blocks(demand_name, std::move(beam_id), beam_nci);
}

std::optional<cu_cp_impl::ntn_ue_access_service_layer_state>
cu_cp_impl::build_ntn_access_layer_state_for_ue(cu_cp_ue& ue)
{
  const auto& ntn_cfg = cfg.mobility.meas_manager_config.ntn_location_mobility;
  if (!ntn_cfg.enabled || ntn_cfg.analog_beams.empty()) {
    return std::nullopt;
  }

  std::optional<nr_cell_identity> access_nci = get_ue_serving_nci_for_ntn_load(du_db, ue);
  if (!access_nci.has_value()) {
    return std::nullopt;
  }

  const ntn_beam_position* access_beam = find_ntn_beam_cfg_by_nci(ntn_cfg.beams, access_nci.value());
  if (access_beam == nullptr || access_beam->analog_beam_id.empty()) {
    return std::nullopt;
  }

  const ntn_analog_beam_position* analog = find_ntn_analog_beam_cfg(ntn_cfg.analog_beams, access_beam->analog_beam_id);
  if (analog == nullptr || !analog->enabled) {
    return std::nullopt;
  }

  ntn_ue_access_service_layer_state state;
  state.access_state          = ntn_access_state_access_active;
  state.access_reason         = "valid";
  state.access_analog_beam_id = access_beam->analog_beam_id;
  state.access_du_index       = ue.get_du_index();
  state.access_nci            = access_nci.value();
  state.has_access_nci        = true;

  const auto existing_it = ntn_ue_layer_states.find(ue.get_ue_index());
  if (existing_it != ntn_ue_layer_states.end()) {
    state.access_state  = existing_it->second.access_state;
    state.access_reason = existing_it->second.access_reason;
    if (!existing_it->second.access_analog_beam_id.empty()) {
      state.access_analog_beam_id = existing_it->second.access_analog_beam_id;
    }
    if (existing_it->second.has_access_nci) {
      state.access_nci     = existing_it->second.access_nci;
      state.has_access_nci = true;
    }
    state.last_downlink_wake_beam_id        = existing_it->second.last_downlink_wake_beam_id;
    state.paired_uplink_access_beam_id      = existing_it->second.paired_uplink_access_beam_id;
    state.paired_uplink_access_nci          = existing_it->second.paired_uplink_access_nci;
    state.has_paired_uplink_access_nci      = existing_it->second.has_paired_uplink_access_nci;
    state.paired_uplink_access_du_index     = existing_it->second.paired_uplink_access_du_index;
    state.paired_access_reason              = existing_it->second.paired_access_reason;
    state.service_state          = existing_it->second.service_state;
    state.service_reason         = existing_it->second.service_reason;
    state.service_digital_beam_id = existing_it->second.service_digital_beam_id;
    state.service_du_index       = existing_it->second.service_du_index;
    state.service_nci            = existing_it->second.service_nci;
    state.has_service_nci        = existing_it->second.has_service_nci;
    state.service_uplink_resource_beam_id = existing_it->second.service_uplink_resource_beam_id;
    state.service_uplink_resource_du_index = existing_it->second.service_uplink_resource_du_index;
    state.service_uplink_resource_nci = existing_it->second.service_uplink_resource_nci;
    state.has_service_uplink_resource_nci = existing_it->second.has_service_uplink_resource_nci;
    state.service_pair_reason    = existing_it->second.service_pair_reason;
    state.service_binding_source = existing_it->second.service_binding_source;
    state.pending_drbs           = existing_it->second.pending_drbs;
    state.pending_qos            = existing_it->second.pending_qos;
  }

  apply_ntn_paired_access_context_for_ue(ue.get_ue_index(), state);
  return state;
}

std::optional<std::string> cu_cp_impl::select_ntn_digital_service_beam_for_ue(
    const cu_cp_ue&                              ue,
    const ntn_ue_access_service_layer_state&     access_state,
    unsigned                                     expected_drbs,
    const ntn_qos_demand_summary&                pending_qos,
    std::string&                                 binding_source,
    std::string&                                 binding_reason,
    std::optional<std::string>&                  service_uplink_resource_beam_id,
    std::string&                                 service_pair_reason) const
{
  (void)pending_qos;
  service_uplink_resource_beam_id.reset();
  service_pair_reason = "none";
  const auto& ntn_cfg = cfg.mobility.meas_manager_config.ntn_location_mobility;
  const ntn_analog_beam_position* analog =
      find_ntn_analog_beam_cfg(ntn_cfg.analog_beams, access_state.access_analog_beam_id);
  if (analog == nullptr) {
    binding_source = "none";
    binding_reason = "invalid_parent";
    return std::nullopt;
  }

  const auto child_contains_beam = [&analog](const std::string& beam_id) {
    return std::find(analog->child_digital_beam_ids.begin(), analog->child_digital_beam_ids.end(), beam_id) !=
           analog->child_digital_beam_ids.end();
  };

  std::optional<std::string> legacy_selected_beam_id;
  std::vector<std::string>   load_balance_candidate_beam_ids;

  const cell_meas_manager_ue_context& meas_context = ue.get_meas_context();
  if (meas_context.last_ntn_location_report.has_value() &&
      is_ntn_location_report_fresh(ntn_cfg, *meas_context.last_ntn_location_report, std::chrono::steady_clock::now())) {
    const ntn_ue_location_report& report = *meas_context.last_ntn_location_report;
    double                        best_margin_m = -std::numeric_limits<double>::max();
    std::string                   best_beam_id;
    for (const std::string& child_beam_id : analog->child_digital_beam_ids) {
      const ntn_beam_position* child_beam = find_ntn_beam_cfg(ntn_cfg.beams, child_beam_id);
      if (child_beam == nullptr || !child_beam->enabled) {
        continue;
      }
      const double margin_m = child_beam->coverage_radius_m - distance_m(report.latitude_deg, report.longitude_deg, *child_beam);
      if (margin_m < 0.0) {
        continue;
      }
      load_balance_candidate_beam_ids.push_back(child_beam_id);
      if (best_beam_id.empty() || margin_m > best_margin_m) {
        best_beam_id  = child_beam_id;
        best_margin_m = margin_m;
      }
    }
    if (!best_beam_id.empty()) {
      binding_source = "location";
      binding_reason = "bound";
      legacy_selected_beam_id = best_beam_id;
    }
  }

  const bool has_paired_access_context =
      !access_state.last_downlink_wake_beam_id.empty() && !access_state.paired_uplink_access_beam_id.empty() &&
      access_state.has_paired_uplink_access_nci;
  if (!legacy_selected_beam_id.has_value() && has_paired_access_context) {
    std::optional<tac_t> paired_access_tac;
    if (!access_state.last_downlink_wake_beam_id.empty()) {
      const ntn_beam_tac_result tac = derive_ntn_beam_tac(access_state.last_downlink_wake_beam_id);
      paired_access_tac             = tac.tac;
    }
    if (!paired_access_tac.has_value() && !access_state.paired_uplink_access_beam_id.empty()) {
      const ntn_beam_tac_result tac = derive_ntn_beam_tac(access_state.paired_uplink_access_beam_id);
      paired_access_tac             = tac.tac;
    }

    for (const std::string& child_beam_id : analog->child_digital_beam_ids) {
      const ntn_beam_position* child_beam = find_ntn_beam_cfg(ntn_cfg.beams, child_beam_id);
      if (child_beam == nullptr || !child_beam->enabled || !child_beam->downlink_enabled || !child_beam->uplink_enabled) {
        continue;
      }
      if (paired_access_tac.has_value()) {
        const ntn_beam_tac_result child_tac = derive_ntn_beam_tac(child_beam_id);
        if (!child_tac.tac.has_value() || child_tac.tac.value() != paired_access_tac.value()) {
          continue;
        }
      }
      binding_source = "paired_access_fallback";
      binding_reason = "bound";
      legacy_selected_beam_id = child_beam_id;
      break;
    }
    if (!legacy_selected_beam_id.has_value()) {
      const auto beam_tac_matches_paired_access = [&](const std::string& beam_id) {
        if (!paired_access_tac.has_value()) {
          return true;
        }
        const ntn_beam_tac_result tac = derive_ntn_beam_tac(beam_id);
        return tac.tac.has_value() && tac.tac.value() == paired_access_tac.value();
      };
      const auto beam_policy_allows_new_service = [&](const ntn_beam_position& beam) {
        if (current_ntn_predictive_window_valid &&
            contains_beam_id(current_ntn_predictive_drain_soon_beam_ids, beam.beam_id)) {
          return false;
        }
        return !ntn_service_policy_blocks_new_demand(
            get_beam_service_policy(get_current_ntn_service_switch_over_snapshot(), beam.beam_id, beam.nci));
      };

      const ntn_beam_position* downlink_beam =
          find_ntn_beam_cfg(ntn_cfg.beams, access_state.last_downlink_wake_beam_id);
      const ntn_beam_position* uplink_beam =
          find_ntn_beam_cfg(ntn_cfg.beams, access_state.paired_uplink_access_beam_id);
      const bool can_pair_downlink = downlink_beam != nullptr && downlink_beam->enabled &&
                                     downlink_beam->downlink_enabled &&
                                     child_contains_beam(downlink_beam->beam_id) &&
                                     beam_tac_matches_paired_access(downlink_beam->beam_id) &&
                                     beam_policy_allows_new_service(*downlink_beam);
      const bool can_pair_uplink = uplink_beam != nullptr && uplink_beam->enabled && uplink_beam->uplink_enabled &&
                                   child_contains_beam(uplink_beam->beam_id) &&
                                   beam_tac_matches_paired_access(uplink_beam->beam_id) &&
                                   beam_policy_allows_new_service(*uplink_beam);
      if (can_pair_downlink && can_pair_uplink && downlink_beam->beam_id != uplink_beam->beam_id) {
        binding_source                  = "paired_access_service_pair";
        binding_reason                  = "service_pair_bound";
        service_pair_reason             = "same_analog_tac_uplink_resource";
        service_uplink_resource_beam_id = uplink_beam->beam_id;
        return downlink_beam->beam_id;
      }

      binding_source      = "paired_access_service_pair";
      binding_reason      = "no_uplink_resource_pair_for_service";
      service_pair_reason = binding_reason;
      return std::nullopt;
    }
  }

  if (!legacy_selected_beam_id.has_value() && access_state.has_access_nci) {
    const ntn_beam_position* access_beam = find_ntn_beam_cfg_by_nci(ntn_cfg.beams, access_state.access_nci);
    if (access_beam != nullptr && child_contains_beam(access_beam->beam_id)) {
      binding_source = "access_cell_fallback";
      binding_reason = "bound";
      legacy_selected_beam_id = access_beam->beam_id;
    }
  }

  if (!legacy_selected_beam_id.has_value()) {
    binding_source = "none";
    binding_reason = "no_matching_child";
    return std::nullopt;
  }

  if (!ntn_cfg.multi_beam_load_balancing_enabled) {
    return legacy_selected_beam_id;
  }

  if (load_balance_candidate_beam_ids.empty()) {
    load_balance_candidate_beam_ids = analog->child_digital_beam_ids;
  }

  const ntn_service_switch_over_snapshot switch_over_snapshot = get_current_ntn_service_switch_over_snapshot();
  const ntn_beam_du_assignment*          selected_assignment = nullptr;

  auto is_assignment_eligible = [&](const ntn_beam_du_assignment& assignment) {
    if (assignment.beam_id.empty()) {
      return false;
    }
    if (!contains_beam_id(load_balance_candidate_beam_ids, assignment.beam_id)) {
      return false;
    }
    if (!contains_beam_id(current_ntn_mobility_eligible_beam_ids, assignment.beam_id)) {
      return false;
    }
    if (!assignment.downlink_enabled || !assignment.uplink_enabled) {
      return false;
    }
    if (current_ntn_predictive_window_valid &&
        contains_beam_id(current_ntn_predictive_drain_soon_beam_ids, assignment.beam_id)) {
      return false;
    }
    if (assignment.state != ntn_beam_assignment_state::active_loaded &&
        assignment.state != ntn_beam_assignment_state::candidate) {
      return false;
    }
    if (assignment.du_index == du_index_t::invalid || assignment.du_assignment_reason != "eligible") {
      return false;
    }
    if (!ntn_cfg.analog_beams.empty() && assignment.access_du_index == du_index_t::invalid) {
      return false;
    }
    if (!assignment.resource_domain_eligible) {
      return false;
    }
    if (assignment.digital_ue_cap != 0 && assignment.digital_ue_load + 1 > assignment.digital_ue_cap) {
      return false;
    }
    if (assignment.digital_drb_cap != 0 && assignment.digital_drb_load + expected_drbs > assignment.digital_drb_cap) {
      return false;
    }
    return !ntn_service_policy_blocks_new_demand(
        get_beam_service_policy(switch_over_snapshot, assignment.beam_id, assignment.nci));
  };

  auto is_better_assignment = [&](const ntn_beam_du_assignment& candidate, const ntn_beam_du_assignment& current) {
    if (candidate.nof_ues != current.nof_ues) {
      return candidate.nof_ues < current.nof_ues;
    }
    if (candidate.nof_drbs != current.nof_drbs) {
      return candidate.nof_drbs < current.nof_drbs;
    }

    const bool candidate_active = candidate.state == ntn_beam_assignment_state::active_loaded;
    const bool current_active   = current.state == ntn_beam_assignment_state::active_loaded;
    if (candidate_active != current_active) {
      return candidate_active;
    }

    const unsigned candidate_order = get_served_candidate_order(current_ntn_served_beam_candidates, candidate.beam_id);
    const unsigned current_order   = get_served_candidate_order(current_ntn_served_beam_candidates, current.beam_id);
    if (candidate_order != current_order) {
      return candidate_order < current_order;
    }
    return candidate.beam_id < current.beam_id;
  };

  for (const ntn_beam_du_assignment& assignment : current_ntn_beam_placement_plan.assignments) {
    if (!is_assignment_eligible(assignment)) {
      continue;
    }
    if (selected_assignment == nullptr || is_better_assignment(assignment, *selected_assignment)) {
      selected_assignment = &assignment;
    }
  }

  if (selected_assignment == nullptr) {
    return legacy_selected_beam_id;
  }

  if (selected_assignment->beam_id != legacy_selected_beam_id.value()) {
    binding_reason = "load_balanced";
  }
  return selected_assignment->beam_id;
}

std::optional<std::string> cu_cp_impl::prepare_ntn_digital_service_binding_for_pdu_session(
    cu_cp_ue&                         ue,
    unsigned                          expected_drbs,
    const ntn_qos_demand_summary&     pending_qos)
{
  const auto& ntn_cfg = cfg.mobility.meas_manager_config.ntn_location_mobility;
  if (!ntn_cfg.enabled || ntn_cfg.analog_beams.empty()) {
    return std::nullopt;
  }

  std::optional<ntn_ue_access_service_layer_state> access_state = build_ntn_access_layer_state_for_ue(ue);
  if (!access_state.has_value()) {
    ntn_ue_access_service_layer_state blocked_state;
    blocked_state.access_state  = ntn_access_state_blocked;
    blocked_state.access_reason = "no_valid_access_analog";
    blocked_state.service_state = ntn_service_state_none;
    blocked_state.service_reason = "no_valid_access_analog";
    ntn_ue_layer_states[ue.get_ue_index()] = blocked_state;
    return std::nullopt;
  }

  ntn_ue_access_service_layer_state layer = *access_state;
  const ntn_ue_capability_summary capability = evaluate_ntn_ue_capability_for_ue(ue);
  if (capability.state != ntn_ue_capability_state::supported || !capability.matched_deployment_profile) {
    layer.service_state          = ntn_service_state_blocked;
    layer.service_reason         = capability.state == ntn_ue_capability_state::supported ? capability.profile_block_reason :
                                                                                             capability.reason;
    layer.service_digital_beam_id.clear();
    layer.service_du_index       = du_index_t::invalid;
    layer.has_service_nci        = false;
    layer.service_uplink_resource_beam_id.clear();
    layer.service_uplink_resource_du_index = du_index_t::invalid;
    layer.has_service_uplink_resource_nci = false;
    layer.service_pair_reason = "none";
    layer.service_binding_source = ntn_service_state_none;
    layer.pending_drbs           = 0;
    layer.pending_qos            = {};
    ntn_ue_layer_states[ue.get_ue_index()] = layer;
    logger.warning("ue={}: Blocking NTN PDU session setup. Cause: UE NTN capability {} ({})",
                   ue.get_ue_index(),
                   to_string(capability.state),
                   layer.service_reason);
    return std::nullopt;
  }

  if (block_new_ntn_demand_if_assistance_is_stale("PDU session setup")) {
    layer.service_state  = ntn_service_state_none;
    layer.service_reason = "stale_assistance";
    layer.service_digital_beam_id.clear();
    layer.service_du_index = du_index_t::invalid;
    layer.has_service_nci  = false;
    layer.service_uplink_resource_beam_id.clear();
    layer.service_uplink_resource_du_index = du_index_t::invalid;
    layer.has_service_uplink_resource_nci = false;
    layer.service_pair_reason = "none";
    layer.service_binding_source = ntn_service_state_none;
    layer.pending_drbs   = 0;
    layer.pending_qos    = {};
    ntn_ue_layer_states[ue.get_ue_index()] = layer;
    return std::nullopt;
  }

  if (layer.service_state == ntn_service_state_service_bound && !layer.service_digital_beam_id.empty() &&
      layer.has_service_nci) {
    if (block_new_ntn_demand_if_predictive_window_blocks(
            "PDU session setup", layer.service_digital_beam_id, layer.service_nci)) {
      auto layer_it = ntn_ue_layer_states.find(ue.get_ue_index());
      if (layer_it != ntn_ue_layer_states.end()) {
        layer_it->second.service_reason = "predictive_drain_soon";
        layer_it->second.pending_drbs   = 0;
        layer_it->second.pending_qos    = {};
      }
      return std::nullopt;
    }

    const unsigned loaded_digital_service_beam_cap = get_ntn_loaded_digital_service_beam_cap(ntn_cfg);
    ntn_beam_placement_request request;
    request.beams            = ntn_cfg.beams;
    request.analog_beams     = ntn_cfg.analog_beams;
    request.visible_beams    = current_ntn_served_beam_candidates;
    request.max_active_beams = loaded_digital_service_beam_cap;
    request.resource_policy  = ntn_cfg.resource_policy;
    request.demand_aware_resource_weighting_enabled = ntn_cfg.demand_aware_beam_scheduling_enabled;
    request.du_capacities    = build_ntn_du_beam_capacities(du_db,
                                                             ue_mng,
                                                             ntn_cfg.beams,
                                                             loaded_digital_service_beam_cap,
                                                             cfg.admission.max_nof_ues,
                                                             cfg.admission.max_nof_drbs_per_ue);
    request.beam_loads       = build_ntn_beam_loads_for_current_service_contexts();
    merge_pending_ntn_beam_load(request.beam_loads, layer.service_digital_beam_id, expected_drbs, pending_qos);
    request.previous_assignments = current_ntn_beam_placement_plan.assignments;

    const ntn_beam_placement_plan pending_plan = ntn_beam_planner.plan(request);
    const ntn_beam_du_assignment* assignment =
        find_assignment_for_beam(pending_plan, layer.service_digital_beam_id);
    if (assignment != nullptr && assignment->bidirectional_service_ready &&
        assignment->access_du_index == ue.get_du_index() && assignment->du_index == ue.get_du_index() &&
        assignment->du_assignment_reason == "eligible") {
      layer.pending_drbs = expected_drbs;
      layer.pending_qos  = pending_qos;
      ntn_ue_layer_states[ue.get_ue_index()] = layer;
      return layer.service_digital_beam_id;
    }

    auto layer_it = ntn_ue_layer_states.find(ue.get_ue_index());
    if (layer_it != ntn_ue_layer_states.end()) {
      layer_it->second.service_reason = make_ntn_service_binding_failure_reason(assignment);
      layer_it->second.pending_drbs   = 0;
      layer_it->second.pending_qos    = {};
    }
    return std::nullopt;
  }

  std::string binding_source;
  std::string binding_reason;
  std::optional<std::string> service_uplink_resource_beam_id;
  std::string service_pair_reason = "none";
  std::optional<std::string> selected_beam_id =
      select_ntn_digital_service_beam_for_ue(ue,
                                             layer,
                                             expected_drbs,
                                             pending_qos,
                                             binding_source,
                                             binding_reason,
                                             service_uplink_resource_beam_id,
                                             service_pair_reason);
  if (!selected_beam_id.has_value()) {
    if (binding_source == "paired_access_fallback") {
      ++nof_ntn_paired_access_blocked;
      last_ntn_paired_access_reason = binding_reason;
    } else if (binding_source == "paired_access_service_pair") {
      ++nof_ntn_service_pair_blocked;
      last_ntn_service_pair_reason = binding_reason;
    }
    layer.service_state          = ntn_service_state_none;
    layer.service_reason         = binding_reason;
    layer.service_digital_beam_id.clear();
    layer.service_du_index       = du_index_t::invalid;
    layer.has_service_nci        = false;
    layer.service_uplink_resource_beam_id.clear();
    layer.service_uplink_resource_du_index = du_index_t::invalid;
    layer.has_service_uplink_resource_nci = false;
    layer.service_pair_reason = service_pair_reason;
    layer.service_binding_source = binding_source;
    layer.pending_drbs           = 0;
    layer.pending_qos            = {};
    ntn_ue_layer_states[ue.get_ue_index()] = layer;
    refresh_ntn_beam_placement_for_current_load();
    return std::nullopt;
  }

  const ntn_beam_position* selected_beam = find_ntn_beam_cfg(ntn_cfg.beams, selected_beam_id.value());
  if (selected_beam == nullptr || !selected_beam->enabled) {
    layer.service_state          = ntn_service_state_none;
    layer.service_reason         = "target_not_candidate";
    layer.service_digital_beam_id.clear();
    layer.service_du_index       = du_index_t::invalid;
    layer.has_service_nci        = false;
    layer.service_uplink_resource_beam_id.clear();
    layer.service_uplink_resource_du_index = du_index_t::invalid;
    layer.has_service_uplink_resource_nci = false;
    layer.service_pair_reason = "none";
    layer.service_binding_source = binding_source;
    layer.pending_drbs           = 0;
    layer.pending_qos            = {};
    ntn_ue_layer_states[ue.get_ue_index()] = layer;
    refresh_ntn_beam_placement_for_current_load();
    return std::nullopt;
  }

  const ntn_beam_position* selected_uplink_resource_beam = nullptr;
  if (service_uplink_resource_beam_id.has_value()) {
    selected_uplink_resource_beam = find_ntn_beam_cfg(ntn_cfg.beams, service_uplink_resource_beam_id.value());
    if (selected_uplink_resource_beam == nullptr || !selected_uplink_resource_beam->enabled ||
        !selected_uplink_resource_beam->uplink_enabled) {
      ++nof_ntn_service_pair_blocked;
      last_ntn_service_pair_reason = "no_uplink_resource_pair_for_service";
      layer.service_state          = ntn_service_state_none;
      layer.service_reason         = "no_uplink_resource_pair_for_service";
      layer.service_digital_beam_id.clear();
      layer.service_du_index       = du_index_t::invalid;
      layer.has_service_nci        = false;
      layer.service_uplink_resource_beam_id.clear();
      layer.service_uplink_resource_du_index = du_index_t::invalid;
      layer.has_service_uplink_resource_nci = false;
      layer.service_pair_reason    = "no_uplink_resource_pair_for_service";
      layer.service_binding_source = binding_source;
      layer.pending_drbs           = 0;
      layer.pending_qos            = {};
      ntn_ue_layer_states[ue.get_ue_index()] = layer;
      refresh_ntn_beam_placement_for_current_load();
      return std::nullopt;
    }
  }

  if (block_new_ntn_demand_if_predictive_window_blocks("PDU session setup", selected_beam_id, selected_beam->nci)) {
    layer.service_state          = ntn_service_state_none;
    layer.service_reason         = "predictive_drain_soon";
    layer.service_digital_beam_id.clear();
    layer.service_du_index       = du_index_t::invalid;
    layer.has_service_nci        = false;
    layer.service_uplink_resource_beam_id.clear();
    layer.service_uplink_resource_du_index = du_index_t::invalid;
    layer.has_service_uplink_resource_nci = false;
    layer.service_pair_reason = "none";
    layer.service_binding_source = binding_source;
    layer.pending_drbs           = 0;
    layer.pending_qos            = {};
    ntn_ue_layer_states[ue.get_ue_index()] = layer;
    return std::nullopt;
  }

  if (block_new_ntn_demand_if_service_policy_blocks("PDU session setup", selected_beam_id, selected_beam->nci)) {
    layer.service_state          = ntn_service_state_none;
    layer.service_reason         = "service_policy_blocked";
    layer.service_digital_beam_id.clear();
    layer.service_du_index       = du_index_t::invalid;
    layer.has_service_nci        = false;
    layer.service_uplink_resource_beam_id.clear();
    layer.service_uplink_resource_du_index = du_index_t::invalid;
    layer.has_service_uplink_resource_nci = false;
    layer.service_pair_reason = "none";
    layer.service_binding_source = binding_source;
    layer.pending_drbs           = 0;
    layer.pending_qos            = {};
    ntn_ue_layer_states[ue.get_ue_index()] = layer;
    return std::nullopt;
  }

  if (block_low_priority_ntn_pdu_session_demand_if_headroom_is_reserved(
          selected_beam_id, expected_drbs, pending_qos)) {
    layer.service_state          = ntn_service_state_none;
    layer.service_reason         = "headroom_reserved";
    layer.service_digital_beam_id.clear();
    layer.service_du_index       = du_index_t::invalid;
    layer.has_service_nci        = false;
    layer.service_uplink_resource_beam_id.clear();
    layer.service_uplink_resource_du_index = du_index_t::invalid;
    layer.has_service_uplink_resource_nci = false;
    layer.service_pair_reason = "none";
    layer.service_binding_source = binding_source;
    layer.pending_drbs           = 0;
    layer.pending_qos            = {};
    ntn_ue_layer_states[ue.get_ue_index()] = layer;
    return std::nullopt;
  }

  layer.service_state           = ntn_service_state_binding_pending;
  layer.service_reason          = binding_reason;
  layer.service_digital_beam_id = selected_beam_id.value();
  layer.service_du_index        = du_index_t::invalid;
  layer.service_nci             = selected_beam->nci;
  layer.has_service_nci         = true;
  layer.service_uplink_resource_beam_id = service_uplink_resource_beam_id.value_or(std::string{});
  layer.service_uplink_resource_du_index = du_index_t::invalid;
  if (selected_uplink_resource_beam != nullptr) {
    layer.service_uplink_resource_nci = selected_uplink_resource_beam->nci;
    layer.has_service_uplink_resource_nci = true;
  } else {
    layer.has_service_uplink_resource_nci = false;
  }
  layer.service_pair_reason             = service_pair_reason;
  layer.service_binding_source  = binding_source;
  layer.pending_drbs            = expected_drbs;
  layer.pending_qos             = pending_qos;
  ntn_ue_layer_states[ue.get_ue_index()] = layer;

  refresh_ntn_beam_placement_for_current_load();

  auto layer_it = ntn_ue_layer_states.find(ue.get_ue_index());
  if (layer_it == ntn_ue_layer_states.end()) {
    return std::nullopt;
  }
  const ntn_beam_du_assignment* assignment =
      find_assignment_for_beam(current_ntn_beam_placement_plan, selected_beam_id.value());
  const bool has_service_pair = !layer_it->second.service_uplink_resource_beam_id.empty() &&
                                layer_it->second.has_service_uplink_resource_nci;
  const ntn_beam_du_assignment* uplink_resource_assignment =
      has_service_pair
          ? find_assignment_for_beam(current_ntn_beam_placement_plan,
                                     layer_it->second.service_uplink_resource_beam_id)
          : nullptr;
  const auto assignment_is_service_du_eligible = [&](const ntn_beam_du_assignment* candidate) {
    return candidate != nullptr && candidate->access_du_index == ue.get_du_index() &&
           candidate->du_index == ue.get_du_index() && candidate->du_assignment_reason == "eligible" &&
           candidate->resource_domain_eligible;
  };
  const bool normal_service_ready = !has_service_pair && assignment != nullptr && assignment->bidirectional_service_ready &&
                                    assignment_is_service_du_eligible(assignment);
  const bool service_pair_ready =
      has_service_pair && assignment_is_service_du_eligible(assignment) &&
      assignment->state == ntn_beam_assignment_state::active_loaded && assignment->downlink_ready &&
      assignment->downlink_service_required && assignment_is_service_du_eligible(uplink_resource_assignment) &&
      uplink_resource_assignment->state == ntn_beam_assignment_state::active_loaded &&
      uplink_resource_assignment->uplink_ready && uplink_resource_assignment->uplink_resource_required;
  if (normal_service_ready || service_pair_ready) {
    layer_it->second.service_du_index = assignment->du_index;
    if (service_pair_ready) {
      layer_it->second.service_uplink_resource_du_index = uplink_resource_assignment->du_index;
      layer_it->second.service_pair_reason             = "same_analog_tac_uplink_resource";
      last_ntn_service_pair_reason                     = "service_pair_bound";
    }
    if (binding_reason == "load_balanced") {
      ++nof_ntn_load_balancing_admission_steered;
      last_ntn_load_balancing_reason         = "admission_steered";
      last_ntn_load_balancing_source_beam_id = "none";
      last_ntn_load_balancing_target_beam_id = selected_beam_id.value();
      last_ntn_load_balancing_source_analog_id = "none";
      last_ntn_load_balancing_target_analog_id =
          selected_beam != nullptr && !selected_beam->analog_beam_id.empty() ? selected_beam->analog_beam_id : "none";
    }
    if (binding_source == "paired_access_fallback") {
      ++nof_ntn_service_bindings_from_paired_access;
      last_ntn_paired_access_reason = "service_bound";
    }
    if (binding_source == "paired_access_service_pair") {
      ++nof_ntn_service_bindings_from_paired_access;
      last_ntn_paired_access_reason = "service_pair_bound";
    }
    return selected_beam_id;
  }

  if (binding_source == "paired_access_fallback") {
    ++nof_ntn_paired_access_blocked;
    last_ntn_paired_access_reason = make_ntn_service_binding_failure_reason(assignment);
  } else if (binding_source == "paired_access_service_pair") {
    ++nof_ntn_service_pair_blocked;
    last_ntn_service_pair_reason = "service_pair_capacity_blocked";
  }
  layer_it->second.service_state  = ntn_service_state_none;
  layer_it->second.service_reason =
      binding_source == "paired_access_service_pair" ? "service_pair_capacity_blocked" :
                                                       make_ntn_service_binding_failure_reason(assignment);
  layer_it->second.service_digital_beam_id.clear();
  layer_it->second.service_du_index = du_index_t::invalid;
  layer_it->second.has_service_nci  = false;
  layer_it->second.service_uplink_resource_beam_id.clear();
  layer_it->second.service_uplink_resource_du_index = du_index_t::invalid;
  layer_it->second.has_service_uplink_resource_nci = false;
  layer_it->second.service_pair_reason =
      binding_source == "paired_access_service_pair" ? "service_pair_capacity_blocked" : "none";
  layer_it->second.service_binding_source = ntn_service_state_none;
  layer_it->second.pending_drbs   = 0;
  layer_it->second.pending_qos    = {};
  refresh_ntn_beam_placement_for_current_load();
  return std::nullopt;
}

void cu_cp_impl::commit_ntn_digital_service_binding_if_pdu_setup_succeeded(ue_index_t ue_index, bool success)
{
  auto layer_it = ntn_ue_layer_states.find(ue_index);
  if (layer_it == ntn_ue_layer_states.end()) {
    return;
  }

  if (layer_it->second.service_state == ntn_service_state_service_bound) {
    layer_it->second.pending_drbs = 0;
    layer_it->second.pending_qos  = {};
    if (success) {
      layer_it->second.service_reason =
          !layer_it->second.service_uplink_resource_beam_id.empty() ? "service_pair_bound" : "bound";
    }
    refresh_ntn_beam_placement_for_current_load();
    refresh_ntn_rrc_location_request_for_ue(ue_index, "service_binding_refresh");
    refresh_ntn_location_freshness_watchdog("service_binding_refresh");
    schedule_ntn_multi_beam_load_balancing_handovers();
    return;
  }

  if (layer_it->second.service_state != ntn_service_state_binding_pending) {
    return;
  }

  if (!success) {
    mark_ntn_access_released_after_ics(layer_it->second);
    layer_it->second.service_state  = ntn_service_state_none;
    layer_it->second.service_reason = ntn_service_state_none;
    layer_it->second.service_digital_beam_id.clear();
    layer_it->second.service_du_index = du_index_t::invalid;
    layer_it->second.has_service_nci  = false;
    layer_it->second.service_uplink_resource_beam_id.clear();
    layer_it->second.service_uplink_resource_du_index = du_index_t::invalid;
    layer_it->second.has_service_uplink_resource_nci = false;
    layer_it->second.service_pair_reason = "none";
    layer_it->second.service_binding_source = ntn_service_state_none;
    layer_it->second.pending_drbs = 0;
    layer_it->second.pending_qos  = {};
    ntn_service_resource_mng.clear_digital_service_slot_intent(ue_index, "pdu_setup_failed");
    refresh_ntn_beam_placement_for_current_load();
    refresh_ntn_rrc_location_request_for_ue(ue_index, "service_binding_failed");
    refresh_ntn_location_freshness_watchdog("service_binding_failed");
    return;
  }

  mark_ntn_access_released_after_ics(layer_it->second);
  layer_it->second.service_state  = ntn_service_state_service_bound;
  layer_it->second.service_reason =
      !layer_it->second.service_uplink_resource_beam_id.empty() ? "service_pair_bound" : "bound";
  layer_it->second.pending_drbs   = 0;
  layer_it->second.pending_qos    = {};
  refresh_ntn_beam_placement_for_current_load();
  refresh_ntn_rrc_location_request_for_ue(ue_index, "service_bound");
  refresh_ntn_location_freshness_watchdog("service_bound");
  schedule_ntn_ul_slot_updates_for_online_ues();
  schedule_ntn_multi_beam_load_balancing_handovers();
}

void cu_cp_impl::clear_ntn_digital_service_context_if_no_service_remains(ue_index_t ue_index)
{
  auto layer_it = ntn_ue_layer_states.find(ue_index);
  if (layer_it == ntn_ue_layer_states.end()) {
    return;
  }

  cu_cp_ue* ue = ue_mng.find_du_ue(ue_index);
  if (ue == nullptr || ue->get_up_resource_manager().get_nof_drbs() != 0) {
    return;
  }

  mark_ntn_access_released_after_ics(layer_it->second);
  layer_it->second.service_state  = ntn_service_state_none;
  layer_it->second.service_reason = ntn_service_state_none;
  layer_it->second.service_digital_beam_id.clear();
  layer_it->second.service_du_index = du_index_t::invalid;
  layer_it->second.has_service_nci  = false;
  layer_it->second.service_uplink_resource_beam_id.clear();
  layer_it->second.service_uplink_resource_du_index = du_index_t::invalid;
  layer_it->second.has_service_uplink_resource_nci = false;
  layer_it->second.service_pair_reason = "none";
  layer_it->second.service_binding_source = ntn_service_state_none;
  layer_it->second.pending_drbs = 0;
  layer_it->second.pending_qos  = {};
  ntn_service_resource_mng.clear_digital_service_slot_intent(ue_index, "service_released");
  refresh_ntn_beam_placement_for_current_load();
  refresh_ntn_rrc_location_request_for_ue(ue_index, "service_released");
  refresh_ntn_location_freshness_watchdog("service_released");
  schedule_ntn_ul_slot_updates_for_online_ues();
}

void cu_cp_impl::release_ntn_analog_access_after_initial_context_setup(ue_index_t ue_index)
{
  cu_cp_ue* ue = ue_mng.find_du_ue(ue_index);
  if (ue == nullptr) {
    return;
  }

  auto layer_it = ntn_ue_layer_states.find(ue_index);
  if (layer_it == ntn_ue_layer_states.end()) {
    std::optional<ntn_ue_access_service_layer_state> layer_state = build_ntn_access_layer_state_for_ue(*ue);
    if (!layer_state.has_value()) {
      return;
    }
    layer_it = ntn_ue_layer_states.emplace(ue_index, *layer_state).first;
  }

  if (layer_it->second.access_state != ntn_access_state_blocked) {
    mark_ntn_access_released_after_ics(layer_it->second);
  }
  ntn_service_resource_mng.release_analog_access_after_ics(ue_index);
  refresh_ntn_beam_placement_for_current_load();
}

bool cu_cp_impl::block_low_priority_ntn_pdu_session_demand_if_capacity_is_reserved(
    std::optional<std::string>    beam_id,
    unsigned                      expected_drbs,
    const ntn_qos_demand_summary& pending_qos)
{
  const auto& ntn_cfg = cfg.mobility.meas_manager_config.ntn_location_mobility;
  if (!ntn_cfg.enabled || !beam_id.has_value() || !pending_qos.has_qos_demand || current_ntn_served_beam_candidates.empty()) {
    return false;
  }
  if (assignment_is_active_loaded_for_beam(current_ntn_beam_placement_plan, beam_id.value())) {
    return false;
  }
  if (!has_higher_priority_active_loaded_service(current_ntn_beam_placement_plan, pending_qos)) {
    return false;
  }

  const unsigned loaded_digital_service_beam_cap = get_ntn_loaded_digital_service_beam_cap(ntn_cfg);
  ntn_beam_placement_request request;
  request.beams            = ntn_cfg.beams;
  request.analog_beams     = ntn_cfg.analog_beams;
  request.visible_beams    = current_ntn_served_beam_candidates;
  request.max_active_beams = loaded_digital_service_beam_cap;
  request.resource_policy  = ntn_cfg.resource_policy;
  request.demand_aware_resource_weighting_enabled = ntn_cfg.demand_aware_beam_scheduling_enabled;
  request.du_capacities    = build_ntn_du_beam_capacities(du_db,
                                                           ue_mng,
                                                           ntn_cfg.beams,
                                                           loaded_digital_service_beam_cap,
                                                           cfg.admission.max_nof_ues,
                                                           cfg.admission.max_nof_drbs_per_ue);
  request.beam_loads       = build_ntn_beam_loads_for_current_service_contexts();
  merge_pending_ntn_beam_load(request.beam_loads, beam_id.value(), expected_drbs, pending_qos);
  request.previous_assignments = current_ntn_beam_placement_plan.assignments;

  const ntn_beam_placement_plan pending_plan = ntn_beam_planner.plan(request);
  if (assignment_is_active_loaded_for_beam(pending_plan, beam_id.value())) {
    return false;
  }

  logger.warning("Blocking new NTN PDU session demand for beam={}. Cause: lower QoS priority than loaded service",
                 beam_id.value());
  return true;
}

bool cu_cp_impl::block_low_priority_ntn_pdu_session_demand_if_headroom_is_reserved(
    std::optional<std::string>    beam_id,
    unsigned                      expected_drbs,
    const ntn_qos_demand_summary& pending_qos)
{
  const auto& ntn_cfg = cfg.mobility.meas_manager_config.ntn_location_mobility;
  if (!ntn_cfg.enabled || !ntn_cfg.multi_beam_headroom_admission_enabled || !beam_id.has_value() ||
      !pending_qos.has_qos_demand || current_ntn_served_beam_candidates.empty()) {
    return false;
  }

  ++nof_ntn_headroom_evaluations;

  if (!has_ntn_headroom_protected_handover_demand()) {
    ++nof_ntn_headroom_admission_allowed;
    last_ntn_headroom_reason = "no_protected_handover";
    return false;
  }

  const unsigned reserved_beams = std::max(1U, get_nof_ntn_headroom_reserved_beams());
  nof_ntn_headroom_handover_protected += reserved_beams;

  if (is_headroom_exempt_ntn_qos_demand(pending_qos)) {
    ++nof_ntn_headroom_admission_allowed;
    last_ntn_headroom_reason = "priority_allowed";
    return false;
  }

  if (is_ntn_target_capacity_reserved_for_beam(beam_id.value())) {
    ++nof_ntn_headroom_admission_blocked;
    ++nof_ntn_admission_blocked_by_target_reservation;
    ++nof_ntn_target_reservation_held;
    last_ntn_headroom_reason                  = "target_reserved";
    last_ntn_scheduling_guard_reason          = "target_reserved";
    last_ntn_scheduling_guard_source_beam_id  = "none";
    last_ntn_scheduling_guard_target_beam_id  = beam_id.value();
    last_ntn_scheduling_guard_source_analog_id = "none";
    last_ntn_scheduling_guard_target_analog_id = "none";
    const auto reservation_it = ntn_target_reservations.find(beam_id.value());
    if (reservation_it != ntn_target_reservations.end()) {
      reservation_it->second.last_held_time = std::chrono::steady_clock::now();
      last_ntn_scheduling_guard_source_beam_id   = reservation_it->second.source_beam_id;
      last_ntn_scheduling_guard_source_analog_id = reservation_it->second.source_analog_beam_id;
      last_ntn_scheduling_guard_target_analog_id = reservation_it->second.target_analog_beam_id;
    }
    logger.warning("Blocking new NTN PDU session demand for beam={}. Cause: NTN target reservation", beam_id.value());
    return true;
  }

  if (assignment_is_active_loaded_for_beam(current_ntn_beam_placement_plan, beam_id.value())) {
    ++nof_ntn_headroom_admission_allowed;
    last_ntn_headroom_reason = "existing_loaded_beam";
    return false;
  }

  const unsigned loaded_digital_service_beam_cap = get_ntn_loaded_digital_service_beam_cap(ntn_cfg);
  if (loaded_digital_service_beam_cap == 0) {
    ++nof_ntn_headroom_admission_allowed;
    last_ntn_headroom_reason = "unlimited_loaded_cap";
    return false;
  }

  const unsigned active_loaded_beams = count_active_loaded_ntn_beams(current_ntn_beam_placement_plan);
  const unsigned low_priority_limit =
      loaded_digital_service_beam_cap > reserved_beams ? loaded_digital_service_beam_cap - reserved_beams : 0;
  if (active_loaded_beams < low_priority_limit) {
    ++nof_ntn_headroom_admission_allowed;
    last_ntn_headroom_reason = "headroom_available";
    return false;
  }

  ntn_beam_placement_request request;
  request.beams            = ntn_cfg.beams;
  request.analog_beams     = ntn_cfg.analog_beams;
  request.visible_beams    = current_ntn_served_beam_candidates;
  request.max_active_beams = loaded_digital_service_beam_cap;
  request.resource_policy  = ntn_cfg.resource_policy;
  request.demand_aware_resource_weighting_enabled = ntn_cfg.demand_aware_beam_scheduling_enabled;
  request.du_capacities    = build_ntn_du_beam_capacities(du_db,
                                                           ue_mng,
                                                           ntn_cfg.beams,
                                                           loaded_digital_service_beam_cap,
                                                           cfg.admission.max_nof_ues,
                                                           cfg.admission.max_nof_drbs_per_ue);
  request.beam_loads       = build_ntn_beam_loads_for_current_service_contexts();
  merge_pending_ntn_connected_handover_loads(request.beam_loads);
  merge_pending_ntn_beam_load(request.beam_loads, beam_id.value(), expected_drbs, pending_qos);
  request.previous_assignments = current_ntn_beam_placement_plan.assignments;

  const ntn_beam_placement_plan pending_plan = ntn_beam_planner.plan(request);
  if (!assignment_is_active_loaded_for_beam(pending_plan, beam_id.value())) {
    ++nof_ntn_headroom_admission_allowed;
    last_ntn_headroom_reason = "placement_rejected_before_headroom";
    return false;
  }

  ++nof_ntn_headroom_admission_blocked;
  last_ntn_headroom_reason = "reserved_for_handover_target";
  logger.warning("Blocking new NTN PDU session demand for beam={}. Cause: headroom reserved for NTN handover target",
                 beam_id.value());
  return true;
}

bool cu_cp_impl::prepare_ntn_pre_service_relocation_if_access_du_mismatch(ue_index_t                  ue_index,
                                                                          std::optional<std::string> beam_id,
                                                                          std::optional<nr_cell_identity> beam_nci,
                                                                          du_index_t                 ue_du_index)
{
  const auto& ntn_cfg = cfg.mobility.meas_manager_config.ntn_location_mobility;
  if (!ntn_cfg.enabled || ntn_cfg.analog_beams.empty() || !beam_id.has_value()) {
    return false;
  }

  const ntn_beam_du_assignment* assignment =
      find_assignment_for_beam(current_ntn_beam_placement_plan, beam_id.value());
  if (assignment == nullptr || assignment->access_du_index == du_index_t::invalid ||
      assignment->access_du_index == ue_du_index) {
    return false;
  }

  if (assignment->state == ntn_beam_assignment_state::inactive ||
      assignment->state == ntn_beam_assignment_state::draining) {
    return false;
  }

  const nr_cell_identity target_nci = beam_nci.value_or(assignment->nci);
  nr_cell_identity               relocation_target_nci = target_nci;
  std::string                    relocation_target_beam_id = beam_id.value();
  const du_cell_configuration*   target_cell =
      find_du_cell_by_nci(du_db, assignment->access_du_index, relocation_target_nci);
  const ntn_beam_du_assignment* relocation_target_assignment = assignment;
  if (target_cell == nullptr) {
    const ntn_beam_position* source_beam_cfg = find_ntn_beam_cfg(ntn_cfg.beams, beam_id.value());
    if (source_beam_cfg != nullptr && !source_beam_cfg->analog_beam_id.empty()) {
      for (const ntn_beam_du_assignment& candidate : current_ntn_beam_placement_plan.assignments) {
        if (candidate.access_du_index != assignment->access_du_index ||
            candidate.state == ntn_beam_assignment_state::inactive ||
            candidate.state == ntn_beam_assignment_state::draining) {
          continue;
        }
        const ntn_beam_position* candidate_beam_cfg = find_ntn_beam_cfg(ntn_cfg.beams, candidate.beam_id);
        if (candidate_beam_cfg == nullptr ||
            candidate_beam_cfg->analog_beam_id != source_beam_cfg->analog_beam_id) {
          continue;
        }
        const du_cell_configuration* candidate_cell =
            find_du_cell_by_nci(du_db, assignment->access_du_index, candidate.nci);
        if (candidate_cell == nullptr) {
          continue;
        }
        relocation_target_assignment = &candidate;
        relocation_target_nci        = candidate.nci;
        relocation_target_beam_id    = candidate.beam_id;
        target_cell                  = candidate_cell;
        break;
      }
    }
  }
  if (target_cell == nullptr) {
    logger.warning("ue={}: Cannot prepare NTN pre-service relocation for beam={}. Cause: selected access DU does not "
                   "support any eligible digital service beam under the parent analog access beam",
                   ue_index,
                   beam_id.value());
    return false;
  }

  ntn_pre_service_relocation_ue_state state;
  state.state           = "pending_pre_service";
  state.reason          = "access_du_mismatch";
  state.source_du_index = ue_du_index;
  state.target_du_index = relocation_target_assignment->access_du_index;
  state.target_beam_id  = relocation_target_beam_id;
  state.serving_nci     = target_nci;
  state.target_nci      = relocation_target_nci;
  state.target_pci      = target_cell->pci;
  state.attempt_id      = next_ntn_pre_service_relocation_attempt_id++;

  auto previous_it = ntn_pre_service_relocation_states.find(ue_index);
  if (previous_it != ntn_pre_service_relocation_states.end()) {
    state.retry_count = previous_it->second.retry_count;
  }
  ntn_pre_service_relocation_states[ue_index] = state;
  ntn_connected_handover_states.erase(ue_index);

  logger.info("ue={}: Allowing temporary NTN RRC setup on du={} before pre-service relocation to du={} beam={} nci={:#x}",
              ue_index,
              ue_du_index,
              state.target_du_index,
              state.target_beam_id,
              state.target_nci.value());
  return true;
}

bool cu_cp_impl::block_new_ntn_pdu_session_demand_if_pre_service_relocation_pending(ue_index_t ue_index)
{
  const auto relocation_it = ntn_pre_service_relocation_states.find(ue_index);
  if (relocation_it == ntn_pre_service_relocation_states.end()) {
    return false;
  }

  const std::string& state = relocation_it->second.state;
  return state == "pending_pre_service" || state == "preparing" || state == "failed_retryable" ||
         state == "blocked";
}

ntn_ue_capability_summary cu_cp_impl::evaluate_ntn_ue_capability_for_ue(const cu_cp_ue& ue) const
{
  const rrc_ue_interface* rrc_ue = ue.get_rrc_ue();
  if (rrc_ue == nullptr) {
    return {};
  }
  return evaluate_ntn_ue_capability(rrc_ue->get_packed_ue_capability_rat_container_list());
}

void cu_cp_impl::schedule_ntn_pre_service_relocation_if_needed(ue_index_t ue_index)
{
  auto relocation_it = ntn_pre_service_relocation_states.find(ue_index);
  if (relocation_it == ntn_pre_service_relocation_states.end()) {
    return;
  }

  if (relocation_it->second.state != "pending_pre_service" &&
      relocation_it->second.state != "failed_retryable") {
    return;
  }

  cu_cp_ue* ue = ue_mng.find_du_ue(ue_index);
  if (ue == nullptr) {
    ntn_pre_service_relocation_states.erase(relocation_it);
    return;
  }

  ntn_pre_service_relocation_ue_state relocation = relocation_it->second;
  const du_cell_configuration* target_cell =
      find_du_cell_by_nci(du_db, relocation.target_du_index, relocation.target_nci);
  if (target_cell == nullptr) {
    relocation_it->second.state  = "blocked";
    relocation_it->second.reason = "service_du_not_supported";
    return;
  }

  relocation.source_du_index = ue->get_du_index();
  relocation.target_pci      = target_cell->pci;
  relocation_it->second      = relocation;
  relocation_it->second.state = "preparing";

  cu_cp_intra_cu_handover_request request;
  request.source_ue_index = ue_index;
  request.target_du_index = relocation.target_du_index;
  request.cgi             = target_cell->cgi;
  request.target_pci      = target_cell->pci;

  ntn_handover_context ntn_context;
  ntn_context.handover_attempt_id = relocation.attempt_id;
  ntn_context.target_beam_id      = relocation.target_beam_id;
  ntn_context.serving_nci         = relocation.serving_nci;
  ntn_context.target_nci          = relocation.target_nci;
  request.ntn_context             = ntn_context;

  du_index_t source_du_index = relocation.source_du_index;
  du_index_t target_du_index = relocation.target_du_index;

  auto relocation_task = [this, ue_index, request, source_du_index, target_du_index](
                             coro_context<async_task<void>>& ctx) mutable {
    cu_cp_intra_cu_handover_response response;
    CORO_BEGIN(ctx);

    CORO_AWAIT_VALUE(response, handle_intra_cu_handover_request(request, source_du_index, target_du_index));

    if (!response.success) {
      auto state_it = ntn_pre_service_relocation_states.find(ue_index);
      if (state_it != ntn_pre_service_relocation_states.end()) {
        state_it->second.state = "failed_retryable";
        state_it->second.reason = "handover_in_progress";
        ++state_it->second.retry_count;
      }
    }
    CORO_RETURN();
  };

  if (!common_task_sched.schedule_async_task(launch_async(std::move(relocation_task)))) {
    relocation_it->second.state  = "failed_retryable";
    relocation_it->second.reason = "handover_in_progress";
    ++relocation_it->second.retry_count;
  }
}

void cu_cp_impl::handle_ntn_pre_service_relocation_result(const ntn_handover_result& result)
{
  auto relocation_it = ntn_pre_service_relocation_states.find(result.source_ue_index);
  if (relocation_it == ntn_pre_service_relocation_states.end()) {
    return;
  }
  if (relocation_it->second.attempt_id != 0 &&
      relocation_it->second.attempt_id != result.context.handover_attempt_id) {
    return;
  }

  if (result.success) {
    ntn_pre_service_relocation_states.erase(relocation_it);
    return;
  }

  relocation_it->second.state = "failed_retryable";
  relocation_it->second.reason = "handover_in_progress";
  ++relocation_it->second.retry_count;
}

void cu_cp_impl::merge_pending_ntn_connected_handover_loads(std::vector<ntn_beam_load>& loads) const
{
  for (const auto& entry : ntn_connected_handover_states) {
    const ntn_connected_handover_ue_state& state = entry.second;
    if (state.state != "target_preloading" && state.state != "target_resource_preparing" &&
        state.state != "target_resource_applied" && state.state != "handover_preparing") {
      continue;
    }
    if (state.target_beam_id.empty()) {
      continue;
    }
    if (!state.target_uplink_resource_beam_id.empty() &&
        state.target_uplink_resource_beam_id != state.target_beam_id) {
      merge_pending_ntn_directional_beam_load(loads, state.target_beam_id, state.nof_drbs, state.qos, true, false);
      merge_pending_ntn_directional_beam_load(
          loads, state.target_uplink_resource_beam_id, state.nof_drbs, state.qos, false, true);
    } else {
      merge_pending_ntn_beam_load(loads, state.target_beam_id, state.nof_drbs, state.qos);
    }
  }
}

bool cu_cp_impl::has_ntn_headroom_protected_handover_demand() const
{
  if (get_nof_ntn_headroom_reserved_beams() != 0 || !ntn_target_reservations.empty()) {
    return true;
  }

  if (!current_ntn_predictive_window_valid || current_ntn_predictive_drain_soon_beam_ids.empty()) {
    return false;
  }

  return std::any_of(ntn_ue_layer_states.begin(), ntn_ue_layer_states.end(), [this](const auto& entry) {
    const ntn_ue_access_service_layer_state& layer = entry.second;
    return layer.service_state == ntn_service_state_service_bound && !layer.service_digital_beam_id.empty() &&
           contains_beam_id(current_ntn_predictive_drain_soon_beam_ids, layer.service_digital_beam_id);
  });
}

unsigned cu_cp_impl::get_nof_ntn_headroom_reserved_beams() const
{
  std::set<std::string> reserved_beams;
  for (const auto& entry : ntn_connected_handover_states) {
    const ntn_connected_handover_ue_state& state = entry.second;
    if (state.target_beam_id.empty()) {
      continue;
    }
    if (state.state == "target_preloading" || state.state == "target_resource_preparing" ||
        state.state == "target_resource_applied" || state.state == "handover_preparing") {
      reserved_beams.insert(state.target_beam_id);
      if (!state.target_uplink_resource_beam_id.empty()) {
        reserved_beams.insert(state.target_uplink_resource_beam_id);
      }
    }
  }
  for (const auto& entry : ntn_target_reservations) {
    reserved_beams.insert(entry.first);
  }
  return reserved_beams.size();
}

bool cu_cp_impl::is_ntn_headroom_reserved_beam(const std::string& beam_id) const
{
  if (beam_id.empty()) {
    return false;
  }
  return std::any_of(ntn_connected_handover_states.begin(), ntn_connected_handover_states.end(), [&beam_id](const auto& entry) {
    const ntn_connected_handover_ue_state& state = entry.second;
    const bool target_matches =
        state.target_beam_id == beam_id || state.target_uplink_resource_beam_id == beam_id;
    return target_matches &&
           (state.state == "target_preloading" || state.state == "target_resource_preparing" ||
            state.state == "target_resource_applied" || state.state == "handover_preparing");
  });
}

std::string cu_cp_impl::get_ntn_headroom_reserved_beam_reason(const std::string& beam_id) const
{
  for (const auto& entry : ntn_connected_handover_states) {
    const ntn_connected_handover_ue_state& state = entry.second;
    if (state.target_beam_id != beam_id && state.target_uplink_resource_beam_id != beam_id) {
      continue;
    }
    if (state.state == "target_preloading" || state.state == "target_resource_preparing" ||
        state.state == "target_resource_applied" || state.state == "handover_preparing") {
      return state.reason.empty() ? "handover_target" : state.reason;
    }
  }
  if (current_ntn_predictive_window_valid && contains_beam_id(current_ntn_predictive_upcoming_beam_ids, beam_id)) {
    return "predictive_handover_target";
  }
  const auto reservation_it = ntn_target_reservations.find(beam_id);
  if (reservation_it != ntn_target_reservations.end()) {
    return reservation_it->second.reason.empty() ? "target_reserved" : reservation_it->second.reason;
  }
  return "none";
}

bool cu_cp_impl::prepare_ntn_connected_handover(ntn_location_handover_trigger& trigger)
{
  const auto& ntn_cfg = cfg.mobility.meas_manager_config.ntn_location_mobility;
  if (!ntn_cfg.enabled) {
    return true;
  }

  auto make_state = [&](std::string state_name, std::string reason) {
    ntn_connected_handover_ue_state state;
    state.state       = std::move(state_name);
    state.reason      = std::move(reason);
    state.target_beam_id = trigger.target_beam_id;
    state.serving_nci = trigger.serving_nci;
    state.target_nci  = trigger.target_nci;
    state.attempt_id  = trigger.handover_attempt_id;
    state.target_uplink_resource_beam_id = trigger.target_uplink_resource_beam_id;
    state.target_uplink_resource_nci     = trigger.target_uplink_resource_nci;
    state.target_uplink_resource_du_index = trigger.target_uplink_resource_du_index;
    state.has_target_uplink_resource_nci =
        !trigger.target_uplink_resource_beam_id.empty() &&
        trigger.target_uplink_resource_nci != nr_cell_identity::min();
    state.target_service_pair_reason = trigger.target_service_pair_reason;

    if (std::optional<std::string> source_beam_id = find_ntn_beam_id_by_nci(trigger.serving_nci);
        source_beam_id.has_value()) {
      state.source_beam_id = source_beam_id.value();
      if (const ntn_beam_position* source_beam_cfg = find_ntn_beam_cfg(ntn_cfg.beams, source_beam_id.value());
          source_beam_cfg != nullptr) {
        state.source_analog_beam_id = source_beam_cfg->analog_beam_id;
      }
    }
    if (const ntn_beam_position* target_beam_cfg = find_ntn_beam_cfg(ntn_cfg.beams, trigger.target_beam_id);
        target_beam_cfg != nullptr) {
      state.target_analog_beam_id = target_beam_cfg->analog_beam_id;
    }
    if (const auto previous_it = ntn_connected_handover_states.find(trigger.ue_index);
        previous_it != ntn_connected_handover_states.end()) {
      state.retry_count = previous_it->second.retry_count;
    }
    return state;
  };

  auto block = [&](const char* reason) {
    ntn_connected_handover_states[trigger.ue_index] = make_state("blocked", reason);
    refresh_ntn_beam_placement_for_current_load();
    logger.warning("ue={}: Blocking NTN connected handover attempt={} beam={}. Cause: {}",
                   trigger.ue_index,
                   trigger.handover_attempt_id,
                   trigger.target_beam_id,
                   reason);
    return false;
  };

  if (trigger.ue_index == ue_index_t::invalid || trigger.target_beam_id.empty()) {
    return false;
  }
  if (ntn_pre_service_relocation_states.count(trigger.ue_index) != 0) {
    return block("handover_in_progress");
  }
  if (const auto state_it = ntn_connected_handover_states.find(trigger.ue_index);
      state_it != ntn_connected_handover_states.end() &&
      (state_it->second.state == "target_resource_preparing" ||
       state_it->second.state == "target_resource_applied" ||
       state_it->second.state == "handover_preparing")) {
    return block("handover_in_progress");
  }
  if (is_current_ntn_assistance_stale(std::chrono::steady_clock::now())) {
    return block("stale_assistance");
  }
  if (std::find_if(current_ntn_served_beam_candidates.begin(),
                   current_ntn_served_beam_candidates.end(),
                   [&trigger](const ntn_served_beam_candidate& candidate) {
                     return candidate.beam_id == trigger.target_beam_id;
                   }) == current_ntn_served_beam_candidates.end()) {
    return block("target_not_candidate");
  }

  cu_cp_ue* ue = ue_mng.find_du_ue(trigger.ue_index);
  if (ue == nullptr) {
    return false;
  }
  const ntn_ue_capability_summary capability = evaluate_ntn_ue_capability_for_ue(*ue);
  if (capability.state != ntn_ue_capability_state::supported || !capability.matched_deployment_profile) {
    return block(capability.state == ntn_ue_capability_state::supported ? capability.profile_block_reason.c_str() :
                                                                          capability.reason.c_str());
  }

  const std::string handover_reason = trigger.handover_reason.empty() ? "location_boundary" : trigger.handover_reason;
  ntn_connected_handover_ue_state state = make_state("target_resource_preparing", handover_reason);
  state.nof_drbs = ue->get_up_resource_manager().get_nof_drbs();
  state.qos      = summarize_ntn_qos_demand(ue->get_up_resource_manager().get_up_context());
  state.target_resource_state = "target_resource_preparing";
  ntn_connected_handover_states[trigger.ue_index] = state;

  refresh_ntn_beam_placement_for_current_load();

  const ntn_beam_du_assignment* assignment =
      find_assignment_for_beam(current_ntn_beam_placement_plan, trigger.target_beam_id);
  if (assignment == nullptr || assignment->state != ntn_beam_assignment_state::active_loaded) {
    return block("capacity_reserved_for_higher_priority");
  }
  if (!ntn_cfg.analog_beams.empty() && assignment->access_du_index == du_index_t::invalid) {
    return block("target_analog_ineligible");
  }
  if (assignment->du_index == du_index_t::invalid || assignment->du_assignment_reason != "eligible") {
    return block("target_du_unavailable");
  }

  const du_cell_configuration* target_cell = find_du_cell_by_nci(du_db, assignment->du_index, assignment->nci);
  if (target_cell == nullptr) {
    return block("target_du_unavailable");
  }

  const ntn_beam_du_assignment* slot_assignment = assignment;
  if (!trigger.target_uplink_resource_beam_id.empty()) {
    slot_assignment = find_assignment_for_beam(current_ntn_beam_placement_plan, trigger.target_uplink_resource_beam_id);
    if (slot_assignment == nullptr || slot_assignment->du_index != assignment->du_index ||
        !slot_assignment->uplink_enabled || !slot_assignment->resource_domain_eligible) {
      return block("target_uplink_resource_unavailable");
    }
    if (slot_assignment->state != ntn_beam_assignment_state::active_loaded) {
      return block("target_uplink_resource_not_loaded");
    }
  }

  std::optional<f1ap_ntn_ul_slot_resource_request> target_slot_request =
      make_ntn_ul_slot_request_for_nci(slot_assignment->nci, slot_assignment->du_index, current_ntn_beam_placement_plan);
  if (!target_slot_request.has_value()) {
    auto& blocked_state = ntn_connected_handover_states[trigger.ue_index];
    blocked_state.target_resource_state = "target_sr_srs_pending";
    return block("target_sr_srs_pending");
  }

  const ntn_beam_du_assignment* rnti_assignment = slot_assignment;
  const du_cell_configuration*  rnti_cell       = target_cell;
  if (slot_assignment != assignment) {
    rnti_cell = find_du_cell_by_nci(du_db, slot_assignment->du_index, slot_assignment->nci);
    if (rnti_cell == nullptr) {
      return block("target_uplink_resource_unavailable");
    }
  }

  const ntn_handover_target_rnti_reservation_result rnti_reservation =
      ntn_service_resource_mng.reserve_handover_target_rnti(trigger.ue_index,
                                                            rnti_assignment->du_index,
                                                            to_f1ap_du_cell_index(rnti_cell->cell_index),
                                                            rnti_cell->pci,
                                                            state.target_analog_beam_id);
  if (!rnti_reservation.accepted) {
    auto& blocked_state = ntn_connected_handover_states[trigger.ue_index];
    blocked_state.target_resource_state = rnti_reservation.reason;
    return block("target_rnti_unavailable");
  }

  auto& accepted_state = ntn_connected_handover_states[trigger.ue_index];
  accepted_state.target_du_index       = assignment->du_index;
  accepted_state.target_c_rnti         = rnti_reservation.rnti;
  accepted_state.target_uplink_resource_beam_id = trigger.target_uplink_resource_beam_id;
  accepted_state.target_uplink_resource_nci     = trigger.target_uplink_resource_nci;
  accepted_state.target_uplink_resource_du_index = trigger.target_uplink_resource_du_index;
  accepted_state.has_target_uplink_resource_nci =
      !trigger.target_uplink_resource_beam_id.empty() &&
      trigger.target_uplink_resource_nci != nr_cell_identity::min();
  accepted_state.target_service_pair_reason = trigger.target_service_pair_reason;
  accepted_state.target_ul_slot_request = target_slot_request;
  accepted_state.target_resource_state = "target_resource_preparing";
  accepted_state.target_sr_srs_applied = false;
  trigger.source_beam_id               = accepted_state.source_beam_id;
  trigger.source_analog_beam_id        = accepted_state.source_analog_beam_id;
  trigger.target_analog_beam_id        = accepted_state.target_analog_beam_id;
  trigger.handover_reason              = accepted_state.reason;
  trigger.target_preloaded             = true;
  trigger.target_du_index              = assignment->du_index;
  trigger.target_c_rnti                = rnti_reservation.rnti;
  trigger.target_uplink_resource_du_index = accepted_state.target_uplink_resource_du_index;
  trigger.target_ul_slot_request       = target_slot_request;
  trigger.target_resource_state        = "target_resource_preparing";
  trigger.target_sr_srs_applied        = false;
  return true;
}

void cu_cp_impl::handle_ntn_connected_handover_result(const ntn_handover_result& result)
{
  auto state_it = ntn_connected_handover_states.find(result.source_ue_index);
  if (state_it == ntn_connected_handover_states.end()) {
    return;
  }
  if (state_it->second.attempt_id != 0 && state_it->second.attempt_id != result.context.handover_attempt_id) {
    return;
  }

  const bool has_service_pair_target = !state_it->second.target_uplink_resource_beam_id.empty();
  if (result.success) {
    if (state_it->second.target_c_rnti != rnti_t::INVALID_RNTI) {
      ntn_service_resource_mng.commit_handover_target_rnti(
          result.source_ue_index, result.target_ue_index, state_it->second.target_c_rnti);
    }
    if (result.target_ue_index != ue_index_t::invalid) {
      if (auto layer_it = ntn_ue_layer_states.find(result.source_ue_index); layer_it != ntn_ue_layer_states.end()) {
        ntn_ue_access_service_layer_state target_layer = layer_it->second;
        target_layer.service_state           = ntn_service_state_service_bound;
        target_layer.service_digital_beam_id = result.context.target_beam_id;
        target_layer.service_du_index        = result.context.target_du_index;
        target_layer.service_nci             = result.context.target_nci;
        target_layer.has_service_nci         = result.context.target_nci != nr_cell_identity::min();
        target_layer.service_binding_source  = "connected_handover";
        if (!state_it->second.target_uplink_resource_beam_id.empty()) {
          target_layer.service_uplink_resource_beam_id = state_it->second.target_uplink_resource_beam_id;
          target_layer.service_uplink_resource_du_index = state_it->second.target_uplink_resource_du_index;
          target_layer.service_uplink_resource_nci     = state_it->second.target_uplink_resource_nci;
          target_layer.has_service_uplink_resource_nci = state_it->second.has_target_uplink_resource_nci;
          target_layer.service_reason                  = "service_pair_bound";
          target_layer.service_pair_reason =
              state_it->second.target_service_pair_reason.empty() ? "same_analog_tac_uplink_resource" :
                                                                    state_it->second.target_service_pair_reason;
          last_ntn_service_pair_reason = "service_pair_bound";
        } else {
          target_layer.service_uplink_resource_beam_id.clear();
          target_layer.service_uplink_resource_du_index = du_index_t::invalid;
          target_layer.has_service_uplink_resource_nci  = false;
          target_layer.service_reason                   = "bound";
          target_layer.service_pair_reason              = "none";
        }
        ntn_ue_layer_states[result.target_ue_index] = std::move(target_layer);
      }
    }
    if (has_service_pair_target) {
      ++nof_ntn_service_pair_handover_committed;
      last_ntn_service_pair_handover_completion_reason = "committed";
    }
    ntn_connected_handover_states.erase(state_it);
  } else {
    state_it->second.state = "failed_retryable";
    state_it->second.target_resource_state = "rollback_restored";
    state_it->second.target_sr_srs_applied = false;
    ntn_service_resource_mng.rollback_handover_target_rnti(result.source_ue_index, "connected_handover_failed");
    if (result.target_ue_index != ue_index_t::invalid) {
      ntn_service_resource_mng.clear_digital_service_slot_intent(result.target_ue_index, "connected_handover_failed");
    }
    if (has_service_pair_target) {
      ++nof_ntn_service_pair_handover_rolled_back;
      last_ntn_service_pair_handover_completion_reason = to_ntn_handover_failure_reason(result.failure_cause);
    }
    ++state_it->second.retry_count;
  }
  refresh_ntn_beam_placement_for_current_load();
}

void cu_cp_impl::clear_ntn_connected_handover_state_for_ue_removal(ue_index_t ue_index, const std::string& reason)
{
  auto state_it = ntn_connected_handover_states.find(ue_index);
  if (state_it == ntn_connected_handover_states.end()) {
    return;
  }

  if (!state_it->second.target_uplink_resource_beam_id.empty()) {
    ++nof_ntn_service_pair_handover_context_cleared;
    last_ntn_service_pair_handover_completion_reason = reason;
  }
  ntn_connected_handover_states.erase(state_it);
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
      refresh_ntn_rrc_location_request_for_ue(request.ue_index, "core_location_reporting_started");
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
      refresh_ntn_rrc_location_request_for_ue(request.ue_index, "core_location_reporting_stopped");
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
      refresh_ntn_rrc_location_request_for_ue(request.ue_index, "core_location_reporting_stopped");
      return {};
    }
    case ngap_location_reporting_event_type::cancel_location_report_for_the_ue: {
      if (!request_type.location_report_ref_id_to_be_cancelled.has_value()) {
        ntn_core_location_reporting_states.erase(request.ue_index);
        refresh_ntn_rrc_location_request_for_ue(request.ue_index, "core_location_reporting_cancelled");
        return {};
      }
      auto state_it = ntn_core_location_reporting_states.find(request.ue_index);
      if (state_it != ntn_core_location_reporting_states.end()) {
        auto& active_requests = state_it->second.active_requests;
        const uint8_t ref_id  = request_type.location_report_ref_id_to_be_cancelled.value();
        for (auto& active_request : active_requests) {
          if (active_request.event_type != ngap_location_reporting_event_type::ue_presence_in_area_of_interest) {
            continue;
          }
          active_request.area_of_interest_ref_ids.erase(
              std::remove(active_request.area_of_interest_ref_ids.begin(),
                          active_request.area_of_interest_ref_ids.end(),
                          ref_id),
              active_request.area_of_interest_ref_ids.end());
        }
        active_requests.erase(std::remove_if(active_requests.begin(),
                                             active_requests.end(),
                                             [](const ngap_location_reporting_request_type& active_request) {
                                              return active_request.event_type ==
                                                          ngap_location_reporting_event_type::ue_presence_in_area_of_interest &&
                                                      active_request.area_of_interest_ref_ids.empty();
                                             }),
                              active_requests.end());
      }
      refresh_ntn_rrc_location_request_for_ue(request.ue_index, "core_location_reporting_cancelled");
      return {};
    }
  }

  return {false, ngap_cause_radio_network_t::unspecified};
}

bool cu_cp_impl::handle_ntn_satellite_state_update(const ecef_coordinates_t&           satellite,
                                                   std::optional<ecef_coordinates_t> next_satellite)
{
  std::vector<ntn_satellite_state> current_satellites = {{"sat-0", satellite}};
  std::vector<ntn_satellite_state> next_satellites;
  if (next_satellite.has_value()) {
    next_satellites.push_back({"sat-0", next_satellite.value()});
  }
  return handle_ntn_satellite_state_update(current_satellites, next_satellites);
}

bool cu_cp_impl::handle_ntn_satellite_state_update(const std::vector<ntn_satellite_state>& current_satellites,
                                                   const std::vector<ntn_satellite_state>& next_satellites)
{
  std::vector<ntn_satellite_prediction_step> future_steps;
  if (!next_satellites.empty()) {
    ntn_satellite_prediction_step step;
    step.offset = cfg.mobility.meas_manager_config.ntn_location_mobility.satellite_state_update.update_period;
    step.satellites = next_satellites;
    future_steps.push_back(std::move(step));
  }
  return handle_ntn_satellite_state_update(current_satellites, future_steps);
}

bool cu_cp_impl::handle_ntn_satellite_state_update(
    const std::vector<ntn_satellite_state>&              current_satellites,
    const std::vector<ntn_satellite_prediction_step>& future_steps)
{
  if (!ntn_served_beam_sched.has_value()) {
    logger.debug("Ignoring NTN satellite state update because served beam scheduling is disabled");
    return false;
  }
  if (current_satellites.empty()) {
    logger.debug("Ignoring NTN satellite state update because no current satellite state was provided");
    return false;
  }
  if (!ntn_service_switch_over_ctrl.automatic_source_updates_allowed()) {
    logger.debug("Ignoring NTN satellite state update because manual override freezes automatic source updates");
    return false;
  }

  const auto received_time = std::chrono::steady_clock::now();
  const auto epoch         = std::chrono::system_clock::now();

  const std::vector<ntn_served_beam_demand> demand_snapshot =
      build_ntn_served_beam_demands_for_current_service_contexts();
  ntn_served_beam_schedule schedule =
      ntn_served_beam_sched->update_from_satellite_state(current_satellites, demand_snapshot, *this);
  if (schedule.demand_aware_evaluated) {
    ++nof_ntn_beam_scheduling_evaluations;
    if (schedule.demand_prioritized_window) {
      ++nof_ntn_beam_scheduling_demand_prioritized_windows;
    }
    if (schedule.legacy_fallback) {
      ++nof_ntn_beam_scheduling_legacy_fallback;
    }
    if (schedule.sticky_kept) {
      ++nof_ntn_beam_scheduling_sticky_kept;
    }
    last_ntn_beam_scheduling_reason = schedule.scheduling_reason;
  }
  if (!schedule.changed) {
    if (!update_ntn_served_beam_candidates(schedule.candidates)) {
      logger.warning("NTN satellite state kept served beam candidates [{}] but CU-CP rejected the placement update",
                     format_beam_ids(schedule.beam_ids));
      return false;
    }
  } else if (!schedule.applied) {
    logger.warning("NTN satellite state update selected served beam set [{}] but CU-CP rejected it",
                   format_beam_ids(schedule.beam_ids));
    return false;
  }

  const std::vector<ntn_served_beam_candidate> predictive_candidates =
      build_ntn_predictive_service_window_candidates(schedule, future_steps, demand_snapshot);
  if (current_ntn_predictive_window_valid) {
    if (!update_ntn_served_beam_candidates(predictive_candidates)) {
      logger.warning("NTN predictive service window selected upcoming/drain-soon beams but CU-CP rejected placement");
      clear_ntn_predictive_service_window();
      return false;
    }
  }

  current_ntn_satellite_ecef          = current_satellites.front().ecef;
  current_ntn_satellite_states        = current_satellites;
  current_ntn_satellite_epoch         = epoch;
  current_ntn_satellite_received_time = received_time;
  schedule_ntn_sib19_broadcast_updates();
  schedule_ntn_predictive_beam_hopping_service_handovers();
  schedule_ntn_beam_hopping_service_handovers();
  logger.debug(
      "NTN satellite state update selected served beam set [{}] satellites={} timeline_steps={} predictive_window={} upcoming={} drain_soon={} owner_changes={}",
               format_beam_ids(schedule.beam_ids),
               current_satellites.size(),
               future_steps.size(),
               current_ntn_predictive_window_valid,
               current_ntn_predictive_upcoming_beam_ids.size(),
               current_ntn_predictive_drain_soon_beam_ids.size(),
               current_ntn_satellite_owner_change_count);
  return true;
}

std::vector<std::string> cu_cp_impl::get_current_ntn_served_beam_ids() const
{
  return current_ntn_mobility_eligible_beam_ids;
}

std::vector<cu_cp_ntn_beam_status> cu_cp_impl::get_current_ntn_beam_status() const
{
  std::vector<cu_cp_ntn_beam_status> result;
  result.reserve(current_ntn_beam_placement_plan.assignments.size());
  const auto& ntn_cfg = cfg.mobility.meas_manager_config.ntn_location_mobility;
  const auto now = std::chrono::steady_clock::now();
  const bool stale_assistance = is_current_ntn_assistance_stale(now);
  const ntn_sib19_broadcast_snapshot sib19_broadcast = build_current_ntn_sib19_broadcast_snapshot(now);
  std::map<std::string, const ntn_sib19_broadcast_entry*> sib19_broadcast_by_beam;
  for (const ntn_sib19_broadcast_entry& entry : sib19_broadcast.entries) {
    sib19_broadcast_by_beam[entry.beam_id] = &entry;
  }
  const ntn_service_switch_over_snapshot service_snapshot =
      ntn_service_switch_over_ctrl.build_snapshot(get_configured_ntn_beam_cells());
  std::map<std::string, const ntn_served_beam_candidate*> candidate_by_beam;
  for (const ntn_served_beam_candidate& candidate : current_ntn_served_beam_candidates) {
    candidate_by_beam[candidate.beam_id] = &candidate;
  }
  for (const auto& assignment : current_ntn_beam_placement_plan.assignments) {
    const ntn_beam_position* beam_cfg = find_ntn_beam_cfg(ntn_cfg.beams, assignment.beam_id);
    const ntn_analog_beam_position* analog_cfg =
        beam_cfg != nullptr ? find_ntn_analog_beam_cfg(ntn_cfg.analog_beams, beam_cfg->analog_beam_id) : nullptr;
    cu_cp_ntn_beam_status status;
    status.beam_id             = assignment.beam_id;
    status.analog_beam_id      = beam_cfg != nullptr ? beam_cfg->analog_beam_id : "";
    status.nci                 = assignment.nci;
    status.du_index            = assignment.du_index;
    status.access_du_index     = assignment.access_du_index;
    status.service_du_index    = assignment.du_index;
    status.access_du_reason    = assignment.access_du_reason;
    status.du_assignment_reason = assignment.du_assignment_reason;
    status.serving_satellite_id = assignment.serving_satellite_id;
    const auto timeline_it = current_ntn_predictive_beam_timeline.find(assignment.beam_id);
    if (timeline_it != current_ntn_predictive_beam_timeline.end()) {
      status.predictive_entry_offset = timeline_it->second.first_entry_offset;
      status.predictive_exit_offset  = timeline_it->second.first_exit_offset;
    }
    status.state               = to_cu_cp_ntn_beam_assignment_state(assignment.state);
    status.elevation_deg       = assignment.elevation_deg;
    status.nof_ues             = assignment.nof_ues;
    status.nof_drbs            = assignment.nof_drbs;
    status.in_hopping_window   = assignment.in_hopping_window;
    status.downlink_enabled    = assignment.downlink_enabled;
    status.uplink_enabled      = assignment.uplink_enabled;
    status.downlink_ready      = assignment.downlink_ready;
    status.uplink_ready        = assignment.uplink_ready;
    status.downlink_visible    = assignment.downlink_visible;
    status.uplink_access_ready = assignment.uplink_access_ready;
    status.access_roundtrip_ready = assignment.access_roundtrip_ready;
    status.bidirectional_service_ready = assignment.bidirectional_service_ready;
    const auto candidate_it = candidate_by_beam.find(assignment.beam_id);
    if (candidate_it != candidate_by_beam.end()) {
      status.scheduling_score  = candidate_it->second->scheduling_score;
      status.window_rank       = candidate_it->second->window_rank;
      status.scheduling_reason = candidate_it->second->scheduling_reason;
    }
    status.antenna_slot_index  = assignment.antenna_slot_index;
    status.nof_antenna_slots   = assignment.nof_antenna_slots;
    status.antenna_slot_period = assignment.antenna_slot_period;
    status.sr_slot_offset      = assignment.sr_slot_offset;
    status.sr_slot_period      = assignment.sr_slot_period;
    status.srs_slot_offset     = assignment.srs_slot_offset;
    status.srs_slot_period     = assignment.srs_slot_period;
    status.resource_weight     = assignment.resource_weight;
    status.resource_share      = assignment.resource_share;
    status.resource_weight_reason = assignment.resource_weight_reason;
    status.headroom_reserved   = is_ntn_target_capacity_reserved_for_beam(assignment.beam_id);
    status.headroom_reason     = get_ntn_headroom_reserved_beam_reason(assignment.beam_id);
    if (!status.headroom_reserved && ntn_cfg.multi_beam_headroom_admission_enabled &&
        has_ntn_headroom_protected_handover_demand() && current_ntn_predictive_window_valid &&
        contains_beam_id(current_ntn_predictive_upcoming_beam_ids, assignment.beam_id)) {
      status.headroom_reserved = true;
      status.headroom_reason   = "predictive_handover_target";
    }
    status.qos                 = assignment.qos;
    status.service_policy      = get_beam_service_policy(service_snapshot, assignment.beam_id, assignment.nci);
    status.drain_forced        = ntn_service_policy_forces_drain(status.service_policy);
    status.state_reason        = make_ntn_beam_state_reason(assignment.state, stale_assistance, status.service_policy);
    if (current_ntn_predictive_window_valid &&
        contains_beam_id(current_ntn_predictive_drain_soon_beam_ids, assignment.beam_id)) {
      status.state_reason = "predictive_drain_soon";
    } else if (current_ntn_predictive_window_valid &&
               contains_beam_id(current_ntn_predictive_upcoming_beam_ids, assignment.beam_id) &&
               assignment.state == ntn_beam_assignment_state::candidate) {
      status.state_reason = "predictive_upcoming";
    }
    status.analog_access_reason =
        make_ntn_analog_access_reason(ntn_cfg, assignment, beam_cfg, analog_cfg, stale_assistance);
    status.analog_access_eligible = status.analog_access_reason == "eligible";
    status.analog_edge_partial    = analog_cfg != nullptr && analog_cfg->is_edge_partial;
    status.resource_domain_eligible = assignment.resource_domain_eligible;
    status.resource_domain_reason   = assignment.resource_domain_reason;
    status.reuse_group_id           = assignment.reuse_group_id;
    status.conflict_group_ids       = assignment.conflict_group_ids;
    status.analog_loaded_digital_child_cap  = assignment.analog_loaded_digital_child_cap;
    status.analog_loaded_digital_child_load = assignment.analog_loaded_digital_child_load;
    status.analog_service_bound_ue_cap      = assignment.analog_service_bound_ue_cap;
    status.analog_service_bound_ue_load     = assignment.analog_service_bound_ue_load;
    status.analog_drb_cap                   = assignment.analog_drb_cap;
    status.analog_drb_load                  = assignment.analog_drb_load;
    status.digital_ue_cap                   = assignment.digital_ue_cap;
    status.digital_ue_load                  = assignment.digital_ue_load;
    status.digital_drb_cap                  = assignment.digital_drb_cap;
    status.digital_drb_load                 = assignment.digital_drb_load;
    const bool analog_blocks_new_demand = !ntn_cfg.analog_beams.empty() && !status.analog_access_eligible;
    const bool du_policy_blocks_new_demand =
        !ntn_cfg.analog_beams.empty() && status.du_assignment_reason != "eligible" &&
        status.du_assignment_reason != "draining";
    const bool resource_domain_blocks_new_demand = !status.resource_domain_eligible;
    const bool predictive_window_blocks_new_demand =
        current_ntn_predictive_window_valid &&
        contains_beam_id(current_ntn_predictive_drain_soon_beam_ids, assignment.beam_id);
    status.new_demand_blocked =
        stale_assistance || ntn_service_policy_blocks_new_demand(status.service_policy) || analog_blocks_new_demand ||
        du_policy_blocks_new_demand || resource_domain_blocks_new_demand || predictive_window_blocks_new_demand;
    const ntn_beam_tac_result derived_tac = derive_ntn_beam_tac(assignment.beam_id);
    status.derived_tac                   = derived_tac.tac;
    status.derived_tac_reason            = ntn_beam_tac_reason_to_string(derived_tac);
    status.paging_recommendation_reason =
        make_ntn_paging_recommendation_reason(
            assignment.state,
            stale_assistance,
            status.drain_forced,
            status.downlink_ready,
            status.uplink_access_ready,
            status.access_roundtrip_ready,
            derived_tac);
    status.paging_recommendable = status.paging_recommendation_reason == "eligible";
    const auto sib19_record_it = ntn_sib19_broadcast_records.find(assignment.beam_id);
    if (sib19_record_it != ntn_sib19_broadcast_records.end()) {
      status.sib19_broadcast_state      = ntn_sib19_broadcast_state_to_string(sib19_record_it->second.state);
      status.sib19_broadcast_reason     = sib19_record_it->second.reason;
      status.sib19_broadcast_generation = sib19_record_it->second.generation_id;
      status.sib19_packed_bytes         = sib19_record_it->second.packed_sib19_bytes;
      status.sib19_packed_hash          = sib19_record_it->second.packed_sib19_hash;
    } else {
      const auto sib19_it = sib19_broadcast_by_beam.find(assignment.beam_id);
      if (sib19_it != sib19_broadcast_by_beam.end()) {
        status.sib19_broadcast_state  = ntn_sib19_broadcast_state_to_string(sib19_it->second->state);
        status.sib19_broadcast_reason = sib19_it->second->reason;
        status.sib19_packed_bytes     = sib19_it->second->packed_sib19.length();
        status.sib19_packed_hash      = compute_ntn_sib19_packed_hash(sib19_it->second->packed_sib19);
      }
    }
    if (!status.downlink_ready && status.sib19_broadcast_state == "desired") {
      status.sib19_broadcast_state  = "stale_blocked";
      status.sib19_broadcast_reason = "downlink_unavailable";
      status.sib19_packed_bytes     = 0;
      status.sib19_packed_hash      = 0;
    }
    result.push_back(std::move(status));
  }

  const ntn_beam_service_resource_snapshot resource_snapshot = ntn_service_resource_mng.get_snapshot();
  auto has_applied_rnti_lease = [&resource_snapshot](const cu_cp_ntn_beam_status& beam) {
    return std::any_of(resource_snapshot.rnti_leases.begin(),
                       resource_snapshot.rnti_leases.end(),
                       [&beam](const ntn_rnti_lease& lease) {
                         return lease.distribution_state == "applied_by_du" &&
                                lease.analog_beam_id == beam.analog_beam_id &&
                                lease.du_index == beam.access_du_index;
                       });
  };
  auto can_source_use_access_pair = [](const cu_cp_ntn_beam_status& beam) {
    if (!beam.downlink_visible || beam.access_roundtrip_ready || !beam.derived_tac.has_value()) {
      return false;
    }
    if (beam.state != cu_cp_ntn_beam_assignment_state::candidate &&
        beam.state != cu_cp_ntn_beam_assignment_state::active_loaded) {
      return false;
    }
    return beam.paging_recommendation_reason == "uplink_response_unavailable" ||
           beam.paging_recommendation_reason == "access_roundtrip_unavailable";
  };
  auto is_candidate_uplink_pair = [](const cu_cp_ntn_beam_status& source, const cu_cp_ntn_beam_status& target) {
    if (target.beam_id == source.beam_id || source.analog_beam_id.empty()) {
      return false;
    }
    if (target.analog_beam_id != source.analog_beam_id || !target.derived_tac.has_value() ||
        target.derived_tac.value() != source.derived_tac.value()) {
      return false;
    }
    if (!target.uplink_access_ready || target.state == cu_cp_ntn_beam_assignment_state::draining ||
        target.state == cu_cp_ntn_beam_assignment_state::inactive || target.drain_forced) {
      return false;
    }
    return true;
  };
  for (cu_cp_ntn_beam_status& source : result) {
    if (source.access_roundtrip_ready) {
      source.access_pair_reason = "same_beam_roundtrip";
      continue;
    }
    if (!can_source_use_access_pair(source)) {
      if (!source.downlink_visible) {
        source.access_pair_reason = "downlink_unavailable";
      } else if (source.uplink_access_ready) {
        source.access_pair_reason = "access_roundtrip_unavailable";
      } else {
        source.access_pair_reason = "no_uplink_access_needed_or_available";
      }
      continue;
    }

    std::string no_pair_reason = "no_same_analog_uplink_beam";
    for (const cu_cp_ntn_beam_status& target : result) {
      if (!is_candidate_uplink_pair(source, target)) {
        continue;
      }
      if (!has_applied_rnti_lease(target)) {
        no_pair_reason = "uplink_access_lease_not_applied";
        continue;
      }
      source.paired_uplink_access_ready = true;
      source.paired_uplink_beam_id      = target.beam_id;
      source.paired_uplink_nci          = target.nci;
      source.paired_uplink_du_index     = target.access_du_index;
      source.access_pair_reason         = "paired_same_analog_tac";
      source.paging_recommendation_reason = "eligible";
      source.paging_recommendable         = true;
      break;
    }
    if (!source.paired_uplink_access_ready) {
      source.access_pair_reason = no_pair_reason;
    }
  }
  return result;
}

ntn_assistance_snapshot cu_cp_impl::get_current_ntn_assistance_snapshot() const
{
  return build_current_ntn_assistance_snapshot(std::chrono::steady_clock::now());
}

ntn_sib19_assistance_snapshot cu_cp_impl::get_current_ntn_sib19_assistance_snapshot() const
{
  return build_current_ntn_sib19_assistance_snapshot(std::chrono::steady_clock::now());
}

cu_cp_ntn_runtime_status cu_cp_impl::get_current_ntn_runtime_status() const
{
  cu_cp_ntn_runtime_status status;
  const auto&              ntn_cfg = cfg.mobility.meas_manager_config.ntn_location_mobility;
  status.enabled                   = ntn_cfg.enabled;
  status.satellite_state_available = current_ntn_satellite_ecef.has_value();

  cu_cp_ntn_position_plan_status& position_status = status.onboard_position_plan;
  position_status.enabled      = cfg.mobility.onboard_position_plan.enabled;
  position_status.du_execution_enabled = cfg.mobility.onboard_position_plan.du_execution_enabled;
  position_status.state_file_configured           = !cfg.mobility.onboard_position_plan.state_file.empty();
  position_status.state_file_required             = position_status.enabled && position_status.du_execution_enabled;
  position_status.access_profile_id = cfg.mobility.onboard_position_plan.expected_access_profile_id.empty()
                                          ? "none"
                                          : cfg.mobility.onboard_position_plan.expected_access_profile_id;
  position_status.access_profile_hash = cfg.mobility.onboard_position_plan.expected_access_profile_hash.empty()
                                            ? "none"
                                            : cfg.mobility.onboard_position_plan.expected_access_profile_hash;
  position_status.identity_authority =
      cfg.mobility.onboard_position_plan.enabled
          ? (cfg.mobility.onboard_position_plan.du_execution_enabled
                 ? "onboard_position_plan"
                 : (ntn_cfg.enabled ? "legacy" : "onboard_position_plan_dry_run"))
          : (ntn_cfg.enabled ? "legacy" : "none");
  position_status.satellite_id = cfg.mobility.onboard_position_plan.satellite_id.empty()
                                     ? "none"
                                     : cfg.mobility.onboard_position_plan.satellite_id;
  {
    std::lock_guard<std::mutex> lock(ntn_onboard_position_plan_mutex);
    position_status.state_schema_version = ntn_position_plan_state_schema_version;
    position_status.state_generation   = ntn_position_plan_state_generation;
    position_status.state_hash         = ntn_position_plan_state_hash.empty() ? "none" : ntn_position_plan_state_hash;
    position_status.state_store_status = ntn_position_plan_state_store_status;
    position_status.state_store_error  = ntn_position_plan_state_error.empty() ? "none" : ntn_position_plan_state_error;
    position_status.last_state_save_unix_ms = ntn_position_plan_state_last_save_unix_ms;
    position_status.state_write_blocked     = ntn_position_plan_state_write_blocked;
    if (ntn_onboard_position_plan_ctrl.has_value()) {
      const auto& controller_cfg = ntn_onboard_position_plan_ctrl->config();
      position_status.stage          = to_string(ntn_onboard_position_plan_ctrl->stage());
      position_status.deployment_stage = to_string(ntn_onboard_position_plan_ctrl->deployment_stage());
      position_status.deployment_detail = ntn_onboard_position_plan_ctrl->deployment_detail();
      position_status.recovery_stage              = to_string(ntn_onboard_position_plan_ctrl->recovery_stage());
      position_status.recovery_detail             = ntn_onboard_position_plan_ctrl->recovery_detail();
      position_status.catalog_version_high_water  = ntn_onboard_position_plan_ctrl->highest_catalog_version_seen();
      position_status.schedule_version_high_water = ntn_onboard_position_plan_ctrl->highest_schedule_version_seen();
      position_status.recovery_schedule_version   = ntn_onboard_position_plan_ctrl->recovery_schedule_version();
      position_status.execution_evidence =
          ntn_onboard_position_plan_ctrl->active_has_external_apply_evidence()
              ? "ssb_prach_software_gate_applied_no_position_or_rf_evidence"
              : "intent_or_control_plane_only";
      position_status.clear_queue_depth = ntn_position_plan_clear_queue.size();
      position_status.clear_in_flight   = ntn_position_plan_clear_in_flight;
      if (!ntn_position_plan_clear_queue.empty()) {
        position_status.clear_queue_head_schedule_version =
            ntn_position_plan_clear_queue.front().first.source.schedule_version;
        position_status.clear_queue_head_calendar_hash = ntn_position_plan_clear_queue.front().first.calendar_hash;
        position_status.clear_queue_head_reason        = ntn_position_plan_clear_queue.front().second;
      }
      position_status.last_rejection = to_string(ntn_onboard_position_plan_ctrl->last_rejection_reason());
      position_status.last_rejected_schedule_version =
          ntn_onboard_position_plan_ctrl->last_rejected_schedule_version();
      position_status.received_plan_present   = ntn_onboard_position_plan_ctrl->has_received_plan();
      position_status.received_catalog_version = ntn_onboard_position_plan_ctrl->last_received_catalog_version();
      position_status.received_schedule_version = ntn_onboard_position_plan_ctrl->last_received_schedule_version();
      position_status.received_content_hash = ntn_onboard_position_plan_ctrl->last_received_content_hash().empty()
                                                  ? "none"
                                                  : ntn_onboard_position_plan_ctrl->last_received_content_hash();
      if (position_status.received_plan_present) {
        position_status.received_activation_epoch_unix_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                ntn_onboard_position_plan_ctrl->last_received_activation_epoch().time_since_epoch())
                .count();
      }
      position_status.candidate_l1_positions = ntn_onboard_position_plan_ctrl->candidate_inventory().size();
      for (unsigned i = 0; i != position_status.cells.size(); ++i) {
        position_status.cells[i].nci      = controller_cfg.onboard_cells[i].nci;
        position_status.cells[i].pci      = controller_cfg.onboard_cells[i].pci;
        position_status.cells[i].capacity = controller_cfg.max_l1_positions_per_cell;
        position_status.cells[i].analog_port_capacity      = controller_cfg.max_analog_ports_per_cell;
        position_status.cells[i].digital_planning_capacity = controller_cfg.max_digital_ports_per_cell;
      }
      position_status.static_preflight_schedule_version = ntn_position_plan_static_preflight_schedule_version;
      for (unsigned i = 0; i != position_status.static_opportunities.size(); ++i) {
        const f1ap_ntn_access_calendar_preflight_report& source = ntn_position_plan_static_preflight_reports[i];
        cu_cp_ntn_static_opportunity_status& destination = position_status.static_opportunities[i];
        destination.performed           = source.performed;
        destination.passed              = source.passed;
        destination.numerology          = source.numerology;
        destination.expected_ssb        = source.expected_ssb;
        destination.matched_ssb         = source.matched_ssb;
        destination.expected_prach      = source.expected_prach;
        destination.matched_prach       = source.matched_prach;
        destination.max_ssb_gap_slots   = source.max_ssb_gap_slots;
        destination.max_prach_gap_slots = source.max_prach_gap_slots;
        if (source.first_unmatched.has_value()) {
          destination.first_unmatched =
              fmt::format("{}:{}:{}+{}",
                          source.first_unmatched->position_id,
                          source.first_unmatched->purpose == f1ap_ntn_access_calendar_preflight_purpose::ssb ? "ssb"
                                                                                                             : "prach",
                          source.first_unmatched->start_slot_offset,
                          source.first_unmatched->nof_slots);
        }
      }

      const ntn_activated_position_plan* audited_plan = nullptr;
      if (ntn_onboard_position_plan_ctrl->active_plan().has_value()) {
        const auto& active_plan = *ntn_onboard_position_plan_ctrl->active_plan();
        position_status.active_catalog_version  = active_plan.source.catalog_version;
        position_status.active_schedule_version = active_plan.source.schedule_version;
        position_status.active_content_hash     = active_plan.source.content_hash;
        position_status.active_calendar_hash    = active_plan.calendar_hash;
        position_status.active_activation_epoch_unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                                                  active_plan.source.activation_epoch.time_since_epoch())
                                                                  .count();
        for (unsigned i = 0; i != position_status.cells.size(); ++i) {
          position_status.cells[i].active_l1_positions = active_plan.cell_positions[i].assigned_l1_ids.size();
        }
        audited_plan = &active_plan;
      }
      if (ntn_onboard_position_plan_ctrl->pending_plan().has_value()) {
        const auto& pending_plan = *ntn_onboard_position_plan_ctrl->pending_plan();
        position_status.pending_catalog_version  = pending_plan.source.catalog_version;
        position_status.pending_schedule_version = pending_plan.source.schedule_version;
        position_status.pending_content_hash     = pending_plan.source.content_hash;
        position_status.pending_calendar_hash    = pending_plan.calendar_hash;
        position_status.pending_activation_epoch_unix_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                pending_plan.source.activation_epoch.time_since_epoch())
                .count();
        for (unsigned i = 0; i != position_status.cells.size(); ++i) {
          position_status.cells[i].pending_l1_positions = pending_plan.cell_positions[i].assigned_l1_ids.size();
        }
        audited_plan = &pending_plan;
      }
      if (audited_plan != nullptr) {
        position_status.schema_version  = audited_plan->source.schema_version;
        position_status.planning_run_id = audited_plan->source.planning_run_id.empty()
                                                   ? "none"
                                                   : audited_plan->source.planning_run_id;
        position_status.audited_schedule_version = audited_plan->source.schedule_version;
        position_status.calendar_intents          = audited_plan->calendar_audit.nof_calendar_intents;
        position_status.ssb_intents               = audited_plan->calendar_audit.nof_ssb_intents;
        position_status.prach_ro_intents          = audited_plan->calendar_audit.nof_prach_ro_intents;
        position_status.prach_ul_beam_intents     = audited_plan->calendar_audit.nof_prach_ul_beam_intents;
        position_status.max_ssb_interval_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(audited_plan->calendar_audit.max_ssb_interval)
                .count();
        position_status.max_prach_interval_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(audited_plan->calendar_audit.max_prach_interval)
                .count();
        position_status.prach_ro_without_beam = audited_plan->calendar_audit.prach_ro_without_beam;
        position_status.resource_conflicts    = audited_plan->calendar_audit.resource_conflicts;
        for (unsigned i = 0; i != position_status.cells.size(); ++i) {
          std::set<uint16_t> used_ports;
          for (const ntn_access_calendar_intent& intent : audited_plan->access_calendar) {
            if (intent.nci == position_status.cells[i].nci &&
                intent.port_id != ntn_access_calendar_intent::no_resource_port) {
              used_ports.insert(intent.port_id);
            }
          }
          position_status.cells[i].analog_ports_used = used_ports.size();
        }
      }
    }
  }

  const ntn_assistance_snapshot assistance = get_current_ntn_assistance_snapshot();
  status.assistance_valid                  = assistance.valid;
  status.assistance_invalid_reason         = assistance.invalid_reason;
  status.nof_mobility_eligible_beams       = current_ntn_mobility_eligible_beam_ids.size();
  status.predictive_window_valid           = current_ntn_predictive_window_valid;
  status.nof_ntn_predictive_upcoming_beams = current_ntn_predictive_upcoming_beam_ids.size();
  status.nof_ntn_predictive_drain_soon_beams = current_ntn_predictive_drain_soon_beam_ids.size();
  status.ntn_predictive_service_window_horizon = current_ntn_predictive_horizon;
  status.ntn_predictive_handover_lead_time     = current_ntn_predictive_lead_time;
  status.nof_ntn_predictive_timeline_steps     = current_ntn_predictive_timeline_steps;
  status.earliest_ntn_predictive_upcoming_offset = current_ntn_earliest_predictive_upcoming_offset;
  status.earliest_ntn_predictive_drain_offset    = current_ntn_earliest_predictive_drain_offset;
  for (const auto& timeline : current_ntn_predictive_beam_timeline) {
    if (timeline.second.first_entry_offset.has_value()) {
      ++status.nof_ntn_predictive_timeline_entry_beams;
    }
    if (timeline.second.first_exit_offset.has_value()) {
      ++status.nof_ntn_predictive_timeline_exit_beams;
    }
  }
  std::set<std::string> current_and_next_window_satellite_ids = current_ntn_current_window_satellite_ids;
  current_and_next_window_satellite_ids.insert(current_ntn_next_window_satellite_ids.begin(),
                                               current_ntn_next_window_satellite_ids.end());
  status.multi_satellite_window_valid =
      current_and_next_window_satellite_ids.size() > 1 || current_ntn_satellite_states.size() > 1 ||
      current_ntn_satellite_owner_change_count > 0;
  status.nof_ntn_current_window_satellites    = current_ntn_current_window_satellite_ids.size();
  status.nof_ntn_next_window_satellites       = current_ntn_next_window_satellite_ids.size();
  status.nof_ntn_multi_satellite_visible_beams = current_ntn_beam_satellite_ids.size();
  status.nof_ntn_satellite_owner_changes      = current_ntn_satellite_owner_change_count;
  status.nof_total_analog_access_beams     = ntn_cfg.analog_beams.size();
  for (const auto& analog : ntn_cfg.analog_beams) {
    if (analog.is_edge_partial) {
      ++status.nof_partial_analog_access_beams;
    } else {
      ++status.nof_full_analog_access_beams;
    }
  }

  const std::vector<cu_cp_ntn_beam_status> beam_status = get_current_ntn_beam_status();
  status.nof_total_beams = beam_status.size();
  std::set<std::string> active_analog_access_beam_ids;
  std::set<std::string> active_reuse_group_ids;
  for (const auto& analog_assignment : current_ntn_beam_placement_plan.analog_assignments) {
    if (analog_assignment.selected_access_du_index != du_index_t::invalid) {
      ++status.nof_access_du_assigned_analog_beams;
    } else {
      ++status.nof_access_du_unassigned_analog_beams;
    }
  }
  for (const auto& beam : beam_status) {
    if (beam.derived_tac.has_value()) {
      ++status.nof_valid_service_area_beams;
    } else {
      ++status.nof_invalid_service_area_beams;
    }
    if (beam.paging_recommendable) {
      ++status.nof_paging_recommendable_beams;
    }
    if (beam.downlink_ready) {
      ++status.nof_downlink_ready_beams;
    }
    if (beam.uplink_ready) {
      ++status.nof_uplink_ready_beams;
    }
    if (beam.downlink_visible) {
      ++status.nof_downlink_visible_beams;
    }
    if (beam.uplink_access_ready) {
      ++status.nof_uplink_access_ready_beams;
    }
    if (beam.access_roundtrip_ready) {
      ++status.nof_access_roundtrip_ready_beams;
    }
    if (beam.paired_uplink_access_ready) {
      ++status.nof_paired_uplink_access_ready_beams;
    }
    if (beam.downlink_visible && !beam.uplink_access_ready && !beam.paired_uplink_access_ready) {
      ++status.nof_downlink_only_without_ul_pair_beams;
    }
    if (beam.bidirectional_service_ready) {
      ++status.nof_bidirectional_service_ready_beams;
    }
    if (beam.sib19_broadcast_state == "desired") {
      ++status.nof_sib19_broadcast_desired;
    } else if (beam.sib19_broadcast_state == "sent_to_du") {
      ++status.nof_sib19_broadcast_sent_to_du;
    } else if (beam.sib19_broadcast_state == "applied_by_du") {
      ++status.nof_sib19_broadcast_applied_by_du;
    } else if (beam.sib19_broadcast_state == "rejected_by_du") {
      ++status.nof_sib19_broadcast_rejected_by_du;
    } else if (beam.sib19_broadcast_state == "clear_sent") {
      ++status.nof_sib19_broadcast_clear_sent;
    } else if (beam.sib19_broadcast_state == "cleared_by_du") {
      ++status.nof_sib19_broadcast_cleared_by_du;
    } else if (beam.sib19_broadcast_state == "stale_blocked") {
      ++status.nof_sib19_broadcast_stale_blocked;
    }
    if (!beam.resource_domain_eligible) {
      if (beam.resource_domain_reason.rfind("analog_", 0) == 0) {
        ++status.nof_resource_domain_analog_cap_blocked;
      } else if (beam.resource_domain_reason.rfind("digital_", 0) == 0) {
        ++status.nof_resource_domain_digital_cap_blocked;
      } else if (beam.resource_domain_reason == "conflict_group_blocked") {
        ++status.nof_resource_domain_conflict_blocked;
      }
    }
    if (beam.analog_access_eligible && !beam.analog_beam_id.empty()) {
      active_analog_access_beam_ids.insert(beam.analog_beam_id);
    }
    switch (beam.state) {
      case cu_cp_ntn_beam_assignment_state::active_loaded:
        ++status.nof_active_loaded_beams;
        ++status.nof_loaded_service_beams;
        ++status.nof_loaded_digital_service_beams;
        if (beam.access_du_index != du_index_t::invalid && beam.service_du_index != du_index_t::invalid) {
          if (beam.access_du_index == beam.service_du_index) {
            ++status.nof_same_du_service_beams;
          } else {
            ++status.nof_split_du_service_beams;
          }
        }
        if (beam.qos.has_qos_demand) {
          ++status.nof_qos_prioritized_loaded_beams;
        }
        if (!beam.reuse_group_id.empty()) {
          active_reuse_group_ids.insert(beam.reuse_group_id);
        }
        break;
      case cu_cp_ntn_beam_assignment_state::candidate:
        ++status.nof_candidate_beams;
        break;
      case cu_cp_ntn_beam_assignment_state::draining:
        ++status.nof_draining_beams;
        if (beam.access_du_index != du_index_t::invalid && beam.service_du_index != du_index_t::invalid) {
          if (beam.access_du_index == beam.service_du_index) {
            ++status.nof_same_du_service_beams;
          } else {
            ++status.nof_split_du_service_beams;
          }
        }
        if (!beam.reuse_group_id.empty()) {
          active_reuse_group_ids.insert(beam.reuse_group_id);
        }
        break;
      case cu_cp_ntn_beam_assignment_state::inactive:
        ++status.nof_inactive_beams;
        break;
    }
  }
  status.nof_active_analog_access_beams = active_analog_access_beam_ids.size();
  status.nof_active_reuse_groups        = active_reuse_group_ids.size();

  const ntn_service_switch_over_snapshot service_snapshot = get_current_ntn_service_switch_over_snapshot();
  status.nof_active_switch_over_events                  = service_snapshot.active_events.size();
  status.automatic_source_updates_allowed               = service_snapshot.automatic_source_updates_allowed;
  const std::vector<cu_cp_ntn_ue_status> ntn_ue_status = get_current_ntn_ue_status();
  status.nof_ues_with_ntn_context                       = ntn_ue_status.size();
  for (const cu_cp_ntn_ue_status& ue_status : ntn_ue_status) {
    switch (ue_status.ntn_capability_state) {
      case ntn_ue_capability_state::unknown:
        ++status.nof_ntn_capability_unknown_ues;
        break;
      case ntn_ue_capability_state::supported:
        ++status.nof_ntn_capability_supported_ues;
        if (!ue_status.ntn_capability_profile_match) {
          ++status.nof_ntn_capability_profile_blocked_ues;
        }
        break;
      case ntn_ue_capability_state::unsupported:
        ++status.nof_ntn_capability_unsupported_ues;
        break;
      case ntn_ue_capability_state::parse_failed:
        ++status.nof_ntn_capability_parse_failed_ues;
        break;
    }
    switch (ue_status.ntn_capability_scenario_support) {
      case ntn_ue_capability_scenario_support::ngso:
        ++status.nof_ntn_capability_ngso_ues;
        break;
      case ntn_ue_capability_scenario_support::gso:
        ++status.nof_ntn_capability_gso_ues;
        break;
      case ntn_ue_capability_scenario_support::both:
        ++status.nof_ntn_capability_both_ues;
        break;
      case ntn_ue_capability_scenario_support::implicit_both:
        ++status.nof_ntn_capability_implicit_both_ues;
        break;
      case ntn_ue_capability_scenario_support::absent:
        break;
    }
  }
  for (const auto& layer : ntn_ue_layer_states) {
    if (layer.second.access_state == ntn_access_state_access_active) {
      ++status.nof_ntn_access_active_ues;
      ++status.nof_ntn_access_only_ues;
    }
    if (layer.second.access_state == ntn_access_state_released_after_ics) {
      ++status.nof_ntn_analog_released_ues;
    }

    if (layer.second.service_state == ntn_service_state_service_bound) {
      ++status.nof_ntn_service_bound_ues;
      ++status.nof_ntn_digital_service_bound_ues;
      if (layer.second.service_binding_source == "location") {
        ++status.nof_ntn_location_bound_service_ues;
      } else if (layer.second.service_binding_source == "access_cell_fallback") {
        ++status.nof_ntn_access_cell_fallback_service_ues;
      }
    } else if (layer.second.service_state == ntn_service_state_binding_pending) {
      ++status.nof_ntn_service_binding_pending_ues;
    } else if (layer.second.service_state == ntn_service_state_blocked) {
      ++status.nof_ntn_service_binding_blocked_ues;
    } else if (layer.second.access_state == ntn_access_state_released_after_ics) {
      ++status.nof_ntn_control_only_ues;
    }
  }
  status.nof_ntn_beam_hopping_ues_requested = nof_ntn_beam_hopping_ues_requested;
  status.nof_ntn_beam_hopping_ues_scheduled = nof_ntn_beam_hopping_ues_scheduled;
  status.nof_ntn_beam_hopping_ues_skipped   = nof_ntn_beam_hopping_ues_skipped;
  status.nof_ntn_predictive_beam_hopping_ues_requested = nof_ntn_predictive_beam_hopping_ues_requested;
  status.nof_ntn_predictive_beam_hopping_ues_scheduled = nof_ntn_predictive_beam_hopping_ues_scheduled;
  status.nof_ntn_predictive_beam_hopping_ues_skipped   = nof_ntn_predictive_beam_hopping_ues_skipped;
  status.nof_ntn_handover_preferred_ues_requested = nof_ntn_handover_preferred_ues_requested;
  status.nof_ntn_handover_preferred_ues_scheduled = nof_ntn_handover_preferred_ues_scheduled;
  status.nof_ntn_handover_preferred_ues_skipped   = nof_ntn_handover_preferred_ues_skipped;
  status.nof_ntn_load_balancing_evaluations = nof_ntn_load_balancing_evaluations;
  status.nof_ntn_load_balancing_admission_steered = nof_ntn_load_balancing_admission_steered;
  status.nof_ntn_load_balancing_handover_requested = nof_ntn_load_balancing_handover_requested;
  status.nof_ntn_load_balancing_handover_scheduled = nof_ntn_load_balancing_handover_scheduled;
  status.nof_ntn_load_balancing_handover_skipped = nof_ntn_load_balancing_handover_skipped;
  status.nof_ntn_load_balancing_same_analog_scheduled = nof_ntn_load_balancing_same_analog_scheduled;
  status.nof_ntn_load_balancing_cross_analog_scheduled = nof_ntn_load_balancing_cross_analog_scheduled;
  status.nof_ntn_load_balancing_skipped_projected_capacity =
      nof_ntn_load_balancing_skipped_projected_capacity;
  status.nof_ntn_load_balancing_skipped_cold_analog = nof_ntn_load_balancing_skipped_cold_analog;
  status.last_ntn_load_balancing_reason = last_ntn_load_balancing_reason;
  status.last_ntn_load_balancing_source_beam_id = last_ntn_load_balancing_source_beam_id;
  status.last_ntn_load_balancing_target_beam_id = last_ntn_load_balancing_target_beam_id;
  status.last_ntn_load_balancing_source_analog_id = last_ntn_load_balancing_source_analog_id;
  status.last_ntn_load_balancing_target_analog_id = last_ntn_load_balancing_target_analog_id;
  status.nof_ntn_service_pair_handover_targets = nof_ntn_service_pair_handover_targets;
  status.nof_ntn_service_pair_handover_scheduled = nof_ntn_service_pair_handover_scheduled;
  status.nof_ntn_service_pair_handover_skipped = nof_ntn_service_pair_handover_skipped;
  status.nof_ntn_service_pair_handover_committed = nof_ntn_service_pair_handover_committed;
  status.nof_ntn_service_pair_handover_rolled_back = nof_ntn_service_pair_handover_rolled_back;
  status.nof_ntn_service_pair_handover_context_cleared = nof_ntn_service_pair_handover_context_cleared;
  status.last_ntn_service_pair_handover_reason = last_ntn_service_pair_handover_reason;
  status.last_ntn_service_pair_handover_completion_reason = last_ntn_service_pair_handover_completion_reason;
  status.nof_ntn_preheat_requested = nof_ntn_preheat_requested;
  status.nof_ntn_preheat_sent = nof_ntn_preheat_sent;
  status.nof_ntn_preheat_applied = nof_ntn_preheat_applied;
  status.nof_ntn_preheat_skipped = nof_ntn_preheat_skipped;
  status.nof_ntn_preheat_demoted = nof_ntn_preheat_demoted;
  status.nof_ntn_preheat_skipped_by_capacity = nof_ntn_preheat_skipped_by_capacity;
  status.nof_ntn_preheat_skipped_by_policy = nof_ntn_preheat_skipped_by_policy;
  status.nof_ntn_cold_analog_preheat_requested = nof_ntn_cold_analog_preheat_requested;
  status.last_ntn_preheat_reason = last_ntn_preheat_reason;
  status.last_ntn_preheat_source_analog_id = last_ntn_preheat_source_analog_id;
  status.last_ntn_preheat_target_analog_id = last_ntn_preheat_target_analog_id;
  status.nof_ntn_target_reservation_created = nof_ntn_target_reservation_created;
  status.nof_ntn_target_reservation_held = nof_ntn_target_reservation_held;
  status.nof_ntn_target_reservation_consumed = nof_ntn_target_reservation_consumed;
  status.nof_ntn_target_reservation_expired = nof_ntn_target_reservation_expired;
  status.nof_ntn_admission_blocked_by_target_reservation = nof_ntn_admission_blocked_by_target_reservation;
  status.nof_ntn_handover_skipped_by_pair_cooldown = nof_ntn_handover_skipped_by_pair_cooldown;
  status.nof_ntn_handover_skipped_by_preheat_ready_guard = nof_ntn_handover_skipped_by_preheat_ready_guard;
  status.nof_ntn_preheat_demote_deferred_by_reservation = nof_ntn_preheat_demote_deferred_by_reservation;
  status.last_ntn_scheduling_guard_reason = last_ntn_scheduling_guard_reason;
  status.last_ntn_scheduling_guard_source_beam_id = last_ntn_scheduling_guard_source_beam_id;
  status.last_ntn_scheduling_guard_target_beam_id = last_ntn_scheduling_guard_target_beam_id;
  status.last_ntn_scheduling_guard_source_analog_id = last_ntn_scheduling_guard_source_analog_id;
  status.last_ntn_scheduling_guard_target_analog_id = last_ntn_scheduling_guard_target_analog_id;
  status.nof_ntn_beam_scheduling_evaluations = nof_ntn_beam_scheduling_evaluations;
  status.nof_ntn_beam_scheduling_demand_prioritized_windows =
      nof_ntn_beam_scheduling_demand_prioritized_windows;
  status.nof_ntn_beam_scheduling_legacy_fallback = nof_ntn_beam_scheduling_legacy_fallback;
  status.nof_ntn_beam_scheduling_sticky_kept = nof_ntn_beam_scheduling_sticky_kept;
  status.last_ntn_beam_scheduling_reason = last_ntn_beam_scheduling_reason;
  status.nof_ntn_resource_weighting_evaluations = nof_ntn_resource_weighting_evaluations;
  status.nof_ntn_resource_weighting_weighted_beams = nof_ntn_resource_weighting_weighted_beams;
  status.nof_ntn_resource_weighting_qos_boosted_beams = nof_ntn_resource_weighting_qos_boosted_beams;
  status.nof_ntn_resource_weighting_legacy_fallback = nof_ntn_resource_weighting_legacy_fallback;
  status.last_ntn_resource_weighting_reason = last_ntn_resource_weighting_reason;
  status.nof_ntn_headroom_evaluations = nof_ntn_headroom_evaluations;
  status.nof_ntn_headroom_reserved_beams = get_nof_ntn_headroom_reserved_beams();
  if (status.nof_ntn_headroom_reserved_beams == 0 && has_ntn_headroom_protected_handover_demand()) {
    status.nof_ntn_headroom_reserved_beams = 1;
  }
  status.nof_ntn_headroom_admission_allowed  = nof_ntn_headroom_admission_allowed;
  status.nof_ntn_headroom_admission_blocked  = nof_ntn_headroom_admission_blocked;
  status.nof_ntn_headroom_handover_protected = nof_ntn_headroom_handover_protected;
  status.last_ntn_headroom_reason            = last_ntn_headroom_reason;
  status.nof_ntn_release_allowed_ues_requested = nof_ntn_release_allowed_ues_requested;
  status.nof_ntn_release_allowed_ues_scheduled = nof_ntn_release_allowed_ues_scheduled;
  status.nof_ntn_release_allowed_ues_skipped   = nof_ntn_release_allowed_ues_skipped;
  status.nof_ntn_rrc_location_reports_received = nof_ntn_rrc_location_reports_received;
  status.nof_ntn_rrc_location_reports_decoded = nof_ntn_rrc_location_reports_decoded;
  status.nof_ntn_rrc_location_reports_unsupported = nof_ntn_rrc_location_reports_unsupported;
  status.nof_ntn_rrc_location_reports_decode_failed = nof_ntn_rrc_location_reports_decode_failed;
  status.nof_ntn_location_reports_accepted = nof_ntn_location_reports_accepted;
  status.nof_ntn_location_reports_rejected = nof_ntn_location_reports_rejected;
  for (const auto& location_freshness : ntn_location_freshness_states) {
    if (location_freshness.second.state == ntn_location_freshness_fresh) {
      ++status.nof_ntn_location_fresh_ues;
    } else if (location_freshness.second.state == ntn_location_freshness_missing) {
      ++status.nof_ntn_location_missing_ues;
    } else if (location_freshness.second.state == ntn_location_freshness_stale) {
      ++status.nof_ntn_location_stale_ues;
    } else if (location_freshness.second.state == ntn_location_freshness_release_pending) {
      ++status.nof_ntn_location_release_pending_ues;
    }
  }
  status.nof_ntn_location_watchdog_evaluations = nof_ntn_location_watchdog_evaluations;
  status.nof_ntn_location_watchdog_refresh_requested = nof_ntn_location_watchdog_refresh_requested;
  status.nof_ntn_location_watchdog_release_requested = nof_ntn_location_watchdog_release_requested;
  status.nof_ntn_location_watchdog_release_scheduled = nof_ntn_location_watchdog_release_scheduled;
  status.nof_ntn_location_watchdog_release_skipped   = nof_ntn_location_watchdog_release_skipped;
  status.last_ntn_location_watchdog_release_reason   = last_ntn_location_watchdog_release_reason;
  for (const auto& location_request : ntn_rrc_location_request_states) {
    if (location_request.second.desired) {
      ++status.nof_ntn_rrc_location_request_desired_ues;
    }
    if (location_request.second.configured) {
      ++status.nof_ntn_rrc_location_request_configured_ues;
    }
    if (location_request.second.pending) {
      ++status.nof_ntn_rrc_location_request_pending_ues;
    }
  }
  status.nof_ntn_rrc_location_request_configs_included = nof_ntn_rrc_location_request_configs_included;
  status.nof_ntn_rrc_location_request_configs_removed = nof_ntn_rrc_location_request_configs_removed;
  status.nof_ntn_rrc_location_request_configs_skipped_capability =
      nof_ntn_rrc_location_request_configs_skipped_capability;
  status.nof_ntn_rrc_location_request_configs_skipped_state = nof_ntn_rrc_location_request_configs_skipped_state;
  status.nof_ntn_rrc_location_request_reconfig_sent = nof_ntn_rrc_location_request_reconfig_sent;
  status.nof_ntn_rrc_location_request_reconfig_failed = nof_ntn_rrc_location_request_reconfig_failed;
  status.nof_ntn_nrppa_dl_ue_received                = nof_ntn_nrppa_dl_ue_received;
  status.nof_ntn_nrppa_dl_ue_forwarded               = nof_ntn_nrppa_dl_ue_forwarded;
  status.nof_ntn_nrppa_dl_ue_dropped                 = nof_ntn_nrppa_dl_ue_dropped;
  status.nof_ntn_nrppa_dl_non_ue_received            = nof_ntn_nrppa_dl_non_ue_received;
  status.nof_ntn_nrppa_dl_non_ue_forwarded           = nof_ntn_nrppa_dl_non_ue_forwarded;
  status.nof_ntn_nrppa_dl_non_ue_dropped             = nof_ntn_nrppa_dl_non_ue_dropped;
  status.nof_ntn_nrppa_ul_ue_received                = nof_ntn_nrppa_ul_ue_received;
  status.nof_ntn_nrppa_ul_ue_sent                    = nof_ntn_nrppa_ul_ue_sent;
  status.nof_ntn_nrppa_ul_ue_dropped                 = nof_ntn_nrppa_ul_ue_dropped;
  status.nof_ntn_nrppa_ul_non_ue_received            = nof_ntn_nrppa_ul_non_ue_received;
  status.nof_ntn_nrppa_ul_non_ue_sent                = nof_ntn_nrppa_ul_non_ue_sent;
  status.nof_ntn_nrppa_ul_non_ue_dropped             = nof_ntn_nrppa_ul_non_ue_dropped;
  status.last_ntn_nrppa_dropped_reason               = last_ntn_nrppa_dropped_reason;
  status.nof_ntn_nrppa_trp_requests_received         = nof_ntn_nrppa_trp_requests_received;
  status.nof_ntn_nrppa_trp_requests_decoded          = nof_ntn_nrppa_trp_requests_decoded;
  status.nof_ntn_nrppa_trp_responses_sent            = nof_ntn_nrppa_trp_responses_sent;
  status.nof_ntn_nrppa_trp_failures_sent             = nof_ntn_nrppa_trp_failures_sent;
  status.nof_ntn_nrppa_unsupported_procedures        = nof_ntn_nrppa_unsupported_procedures;
  status.nof_ntn_nrppa_trp_unsupported_info_items    = nof_ntn_nrppa_trp_unsupported_info_items;
  status.nof_ntn_nrppa_trp_empty_results             = nof_ntn_nrppa_trp_empty_results;
  status.last_ntn_nrppa_trp_reason                   = last_ntn_nrppa_trp_reason;
  status.nof_ntn_nrppa_standard_decode_success       = nof_ntn_nrppa_standard_decode_success;
  status.nof_ntn_nrppa_standard_decode_failure       = nof_ntn_nrppa_standard_decode_failure;
  status.nof_ntn_nrppa_standard_encode_responses     = nof_ntn_nrppa_standard_encode_responses;
  status.nof_ntn_nrppa_standard_encode_failures      = nof_ntn_nrppa_standard_encode_failures;
  status.nof_ntn_nrppa_minimal_fallback_decodes      = nof_ntn_nrppa_minimal_fallback_decodes;
  status.last_ntn_nrppa_standard_decode_reason       = last_ntn_nrppa_standard_decode_reason;
  status.nof_ntn_nrppa_positioning_info_requests_received =
      nof_ntn_nrppa_positioning_info_requests_received;
  status.nof_ntn_nrppa_positioning_info_requests_decoded =
      nof_ntn_nrppa_positioning_info_requests_decoded;
  status.nof_ntn_nrppa_positioning_info_requests_forwarded =
      nof_ntn_nrppa_positioning_info_requests_forwarded;
  status.nof_ntn_nrppa_positioning_info_responses_sent = nof_ntn_nrppa_positioning_info_responses_sent;
  status.nof_ntn_nrppa_positioning_info_failures_sent = nof_ntn_nrppa_positioning_info_failures_sent;
  status.nof_ntn_nrppa_positioning_info_dropped = nof_ntn_nrppa_positioning_info_dropped;
  status.last_ntn_nrppa_positioning_info_reason = last_ntn_nrppa_positioning_info_reason;
  status.nof_ntn_nrppa_measurement_requests_received  = nof_ntn_nrppa_measurement_requests_received;
  status.nof_ntn_nrppa_measurement_requests_decoded   = nof_ntn_nrppa_measurement_requests_decoded;
  status.nof_ntn_nrppa_measurement_requests_forwarded = nof_ntn_nrppa_measurement_requests_forwarded;
  status.nof_ntn_nrppa_measurement_responses_sent     = nof_ntn_nrppa_measurement_responses_sent;
  status.nof_ntn_nrppa_measurement_failures_sent      = nof_ntn_nrppa_measurement_failures_sent;
  status.nof_ntn_nrppa_measurement_dropped            = nof_ntn_nrppa_measurement_dropped;
  status.last_ntn_nrppa_measurement_reason            = last_ntn_nrppa_measurement_reason;
  status.nof_ntn_nrppa_activation_requests_received   = nof_ntn_nrppa_activation_requests_received;
  status.nof_ntn_nrppa_activation_requests_decoded    = nof_ntn_nrppa_activation_requests_decoded;
  status.nof_ntn_nrppa_activation_requests_forwarded  = nof_ntn_nrppa_activation_requests_forwarded;
  status.nof_ntn_nrppa_activation_responses_sent      = nof_ntn_nrppa_activation_responses_sent;
  status.nof_ntn_nrppa_activation_failures_sent       = nof_ntn_nrppa_activation_failures_sent;
  status.nof_ntn_nrppa_activation_dropped             = nof_ntn_nrppa_activation_dropped;
  status.last_ntn_nrppa_activation_reason             = last_ntn_nrppa_activation_reason;
  status.nof_ntn_nrppa_deactivation_requests_received = nof_ntn_nrppa_deactivation_requests_received;
  status.nof_ntn_nrppa_deactivation_requests_decoded  = nof_ntn_nrppa_deactivation_requests_decoded;
  status.nof_ntn_nrppa_deactivation_requests_forwarded = nof_ntn_nrppa_deactivation_requests_forwarded;
  status.nof_ntn_nrppa_deactivation_acks_sent         = nof_ntn_nrppa_deactivation_acks_sent;
  status.nof_ntn_nrppa_deactivation_failures_sent     = nof_ntn_nrppa_deactivation_failures_sent;
  status.nof_ntn_nrppa_deactivation_dropped           = nof_ntn_nrppa_deactivation_dropped;
  status.last_ntn_nrppa_deactivation_reason           = last_ntn_nrppa_deactivation_reason;
  status.nof_ntn_nrppa_assistance_control_requests_received =
      nof_ntn_nrppa_assistance_control_requests_received;
  status.nof_ntn_nrppa_assistance_control_requests_decoded = nof_ntn_nrppa_assistance_control_requests_decoded;
  status.nof_ntn_nrppa_assistance_control_requests_forwarded =
      nof_ntn_nrppa_assistance_control_requests_forwarded;
  status.nof_ntn_nrppa_assistance_control_feedbacks_sent = nof_ntn_nrppa_assistance_control_feedbacks_sent;
  status.nof_ntn_nrppa_assistance_control_failures_sent = nof_ntn_nrppa_assistance_control_failures_sent;
  status.nof_ntn_nrppa_assistance_control_dropped = nof_ntn_nrppa_assistance_control_dropped;
  status.nof_ntn_nrppa_assistance_control_unsupported_fields =
      nof_ntn_nrppa_assistance_control_unsupported_fields;
  status.last_ntn_nrppa_assistance_control_reason = last_ntn_nrppa_assistance_control_reason;
  const auto idle_paging_max_age = ntn_cfg.idle_paging_context_max_age;
  const auto idle_paging_now     = std::chrono::steady_clock::now();
  for (const auto& idle_context : ntn_idle_paging_contexts) {
    if (idle_paging_max_age.count() > 0 && idle_context.second.updated_time != std::chrono::steady_clock::time_point{} &&
        idle_paging_now - idle_context.second.updated_time > idle_paging_max_age) {
      ++status.nof_ntn_idle_paging_contexts_expired;
      continue;
    }
    ++status.nof_ntn_idle_paging_contexts;
    if (idle_context.second.five_g_s_tmsi.has_value()) {
      ++status.nof_ntn_idle_paging_contexts_with_5g_s_tmsi;
    }
  }
  status.nof_ntn_idle_paging_contexts_expired += nof_ntn_idle_paging_expired;
  status.nof_ntn_idle_paging_ue_hits         = nof_ntn_idle_paging_ue_hits;
  status.nof_ntn_idle_paging_tac_fallbacks   = nof_ntn_idle_paging_tac_fallbacks;
  status.nof_ntn_idle_paging_recommendations = nof_ntn_idle_paging_recommendations;
  status.nof_ntn_idle_paging_skipped         = nof_ntn_idle_paging_skipped;
  status.last_ntn_idle_paging_reason         = last_ntn_idle_paging_reason;
  status.nof_ntn_paired_access_contexts      = ntn_pending_paired_access_contexts.size();
  for (const auto& layer : ntn_ue_layer_states) {
    if (!layer.second.paired_uplink_access_beam_id.empty()) {
      ++status.nof_ntn_paired_access_contexts;
    }
    if (layer.second.service_state == ntn_service_state_service_bound &&
        !layer.second.service_uplink_resource_beam_id.empty()) {
      ++status.nof_ntn_service_pair_bound_ues;
    }
  }
  status.nof_ntn_paired_access_responses             = nof_ntn_paired_access_responses;
  status.nof_ntn_service_bindings_from_paired_access = nof_ntn_service_bindings_from_paired_access;
  status.nof_ntn_paired_access_blocked               = nof_ntn_paired_access_blocked;
  status.last_ntn_paired_access_reason               = last_ntn_paired_access_reason;
  status.nof_ntn_service_pair_blocked_ues            = nof_ntn_service_pair_blocked;
  status.last_ntn_service_pair_reason                = last_ntn_service_pair_reason;
  for (const auto& inactive_context : ntn_inactive_contexts) {
    if (idle_paging_max_age.count() > 0 &&
        inactive_context.second.updated_time != std::chrono::steady_clock::time_point{} &&
        idle_paging_now - inactive_context.second.updated_time > idle_paging_max_age) {
      ++status.nof_ntn_inactive_contexts_expired;
      continue;
    }
    ++status.nof_ntn_inactive_contexts;
  }
  status.nof_ntn_inactive_contexts_expired += nof_ntn_inactive_contexts_expired;
  status.nof_ntn_inactive_suspend_requested = nof_ntn_inactive_suspend_requested;
  status.nof_ntn_inactive_suspend_succeeded = nof_ntn_inactive_suspend_succeeded;
  status.nof_ntn_inactive_suspend_failed    = nof_ntn_inactive_suspend_failed;
  status.nof_ntn_inactive_resume_requested  = nof_ntn_inactive_resume_requested;
  status.nof_ntn_inactive_resume_succeeded  = nof_ntn_inactive_resume_succeeded;
  status.nof_ntn_inactive_resume_failed     = nof_ntn_inactive_resume_failed;
  status.nof_ntn_inactive_ngap_suspend_responses = nof_ntn_inactive_ngap_suspend_responses;
  status.nof_ntn_inactive_ngap_suspend_failures  = nof_ntn_inactive_ngap_suspend_failures;
  status.nof_ntn_inactive_ngap_resume_responses  = nof_ntn_inactive_ngap_resume_responses;
  status.nof_ntn_inactive_ngap_resume_failures   = nof_ntn_inactive_ngap_resume_failures;
  status.nof_ntn_inactive_paging_hits       = nof_ntn_inactive_paging_hits;
  status.nof_ntn_inactive_fallback_releases = nof_ntn_inactive_fallback_releases;
  status.last_ntn_inactive_reason           = last_ntn_inactive_reason;
  for (const auto& relocation : ntn_pre_service_relocation_states) {
    if (relocation.second.state == "preparing") {
      ++status.nof_pre_service_relocations_active;
    } else if (relocation.second.state == "blocked") {
      ++status.nof_pre_service_relocations_blocked;
    } else if (relocation.second.state == "pending_pre_service" ||
               relocation.second.state == "failed_retryable") {
      ++status.nof_pre_service_relocations_pending;
    }
  }
  for (const auto& handover : ntn_connected_handover_states) {
    if (handover.second.state == "handover_preparing") {
      ++status.nof_connected_handovers_active;
    } else if (handover.second.state == "target_preloading") {
      ++status.nof_connected_handovers_preloaded;
    } else if (handover.second.state == "target_resource_preparing") {
      ++status.nof_connected_handovers_resource_preparing;
    } else if (handover.second.state == "target_resource_applied") {
      ++status.nof_connected_handovers_resource_applied;
    } else if (handover.second.state == "blocked") {
      ++status.nof_connected_handovers_blocked;
    } else if (handover.second.state == "rollback_restored") {
      ++status.nof_connected_handovers_rollback;
    } else if (handover.second.state == "candidate_stable" ||
               handover.second.state == "failed_retryable") {
      ++status.nof_connected_handovers_candidate;
    }
  }
  const ntn_beam_service_resource_snapshot resource_snapshot = ntn_service_resource_mng.get_snapshot();
  status.nof_ntn_access_rnti_owned            = resource_snapshot.nof_access_rnti_owned;
  status.nof_ntn_access_rnti_conflicts        = resource_snapshot.nof_access_rnti_conflicts;
  status.nof_ntn_rnti_leases_reserved         = resource_snapshot.nof_rnti_leases_reserved;
  status.nof_ntn_rnti_leases_available        = resource_snapshot.nof_rnti_leases_available;
  status.nof_ntn_rnti_leases_sent_to_du       = resource_snapshot.nof_rnti_leases_sent_to_du;
  status.nof_ntn_rnti_leases_applied_by_du    = resource_snapshot.nof_rnti_leases_applied_by_du;
  status.nof_ntn_rnti_leases_consumed_by_du   = resource_snapshot.nof_rnti_leases_consumed_by_du;
  status.nof_ntn_rnti_leases_rejected_by_du   = resource_snapshot.nof_rnti_leases_rejected_by_du;
  status.nof_ntn_rnti_leases_offered_in_rar   = resource_snapshot.nof_rnti_leases_offered_in_rar;
  status.nof_ntn_rnti_leases_initial_ul_seen  = resource_snapshot.nof_rnti_leases_initial_ul_seen;
  status.nof_ntn_rnti_leases_committed        = resource_snapshot.nof_rnti_leases_committed;
  status.nof_ntn_rnti_leases_released         = resource_snapshot.nof_rnti_leases_released;
  status.nof_ntn_rnti_leases_expired          = resource_snapshot.nof_rnti_leases_expired;
  status.nof_ntn_rnti_leases_conflict         = resource_snapshot.nof_rnti_leases_conflict;
  status.nof_ntn_digital_slot_intents         = resource_snapshot.nof_digital_slot_active;
  status.nof_ntn_digital_slot_cleared_intents = resource_snapshot.nof_digital_slot_cleared;
  status.nof_ntn_digital_slot_sent_to_du      = resource_snapshot.nof_digital_slot_sent_to_du;
  status.nof_ntn_digital_slot_applied_by_du   = resource_snapshot.nof_digital_slot_applied_by_du;
  status.nof_ntn_digital_slot_rejected_by_du  = resource_snapshot.nof_digital_slot_rejected_by_du;
  status.nof_ntn_digital_slot_cleared_by_du   = resource_snapshot.nof_digital_slot_cleared_by_du;
  status.nof_ntn_digital_slot_rollback        = resource_snapshot.nof_digital_slot_rollback;
  status.ntn_resource_audit_generation         = last_ntn_resource_audit_generation;
  status.nof_ntn_resource_audit_queries_sent   = nof_ntn_resource_audit_queries_sent;
  status.nof_ntn_resource_audit_responses_accepted = nof_ntn_resource_audit_responses_accepted;
  status.nof_ntn_resource_audit_mismatches     = nof_ntn_resource_audit_mismatches;
  status.nof_ntn_resource_audit_repair_actions = nof_ntn_resource_audit_repair_actions;
  status.nof_ntn_resource_audit_failures       = nof_ntn_resource_audit_failures;
  status.nof_ntn_resource_audit_rnti_incomplete = nof_ntn_resource_audit_rnti_incomplete;
  status.nof_ntn_resource_audit_ue_slot_incomplete = nof_ntn_resource_audit_ue_slot_incomplete;
  status.last_ntn_resource_audit_reason = last_ntn_resource_audit_reason;
  status.nof_ntn_service_pair_resource_audit_targets    = nof_ntn_service_pair_resource_audit_targets;
  status.nof_ntn_service_pair_resource_audit_mismatches = nof_ntn_service_pair_resource_audit_mismatches;
  status.nof_ntn_service_pair_resource_audit_repairs    = nof_ntn_service_pair_resource_audit_repairs;
  status.nof_ntn_service_pair_resource_audit_skipped    = nof_ntn_service_pair_resource_audit_skipped;
  status.last_ntn_service_pair_resource_audit_reason    = last_ntn_service_pair_resource_audit_reason;
  status.nof_ntn_resource_repairs_queued       = resource_snapshot.nof_resource_repairs_queued;
  status.nof_ntn_resource_repairs_sent         = resource_snapshot.nof_resource_repairs_sent;
  status.nof_ntn_resource_repairs_applied      = resource_snapshot.nof_resource_repairs_applied;
  status.nof_ntn_resource_repairs_failed       = resource_snapshot.nof_resource_repairs_failed;
  status.nof_ntn_resource_repairs_retry_exhausted = resource_snapshot.nof_resource_repairs_retry_exhausted;
  status.nof_ntn_resource_repairs_blocked_conflict = resource_snapshot.nof_resource_repairs_blocked_conflict;
  return status;
}

std::vector<cu_cp_ntn_ue_status> cu_cp_impl::get_current_ntn_ue_status() const
{
  std::vector<cu_cp_ntn_ue_status> result;
  const std::vector<const cu_cp_ue*> ues = ue_mng.get_ues();
  const ntn_beam_service_resource_snapshot resource_snapshot = ntn_service_resource_mng.get_snapshot();
  std::map<ue_index_t, ntn_access_rnti_ownership> access_ownership_by_ue;
  std::map<ue_index_t, ntn_digital_slot_resource_intent> digital_slot_intent_by_ue;
  for (const auto& ownership : resource_snapshot.access_rnti_ownerships) {
    access_ownership_by_ue[ownership.ue_index] = ownership;
  }
  for (const auto& intent : resource_snapshot.digital_slot_intents) {
    digital_slot_intent_by_ue[intent.ue_index] = intent;
  }
  result.reserve(ues.size());
  for (const cu_cp_ue* ue : ues) {
    if (ue == nullptr) {
      continue;
    }

    const cell_meas_manager_ue_context& meas_context = ue->get_meas_context();
    const bool has_slot_request = ntn_service_resource_mng.has_active_digital_slot_intent(ue->get_ue_index());
    const auto core_location_it = ntn_core_location_reporting_states.find(ue->get_ue_index());
    const bool has_core_location_state =
        core_location_it != ntn_core_location_reporting_states.end() && !core_location_it->second.active_requests.empty();
    const auto location_request_it = ntn_rrc_location_request_states.find(ue->get_ue_index());
    const bool has_location_request_state =
        location_request_it != ntn_rrc_location_request_states.end() &&
        (location_request_it->second.desired || location_request_it->second.configured ||
         location_request_it->second.pending || location_request_it->second.skipped_reason != "none");
    const auto location_freshness_it = ntn_location_freshness_states.find(ue->get_ue_index());
    const bool has_location_freshness_state =
        location_freshness_it != ntn_location_freshness_states.end() &&
        location_freshness_it->second.state != ntn_location_freshness_not_required;
    const auto relocation_it = ntn_pre_service_relocation_states.find(ue->get_ue_index());
    const auto connected_handover_it = ntn_connected_handover_states.find(ue->get_ue_index());
    const auto layer_it = ntn_ue_layer_states.find(ue->get_ue_index());
    const auto access_ownership_it = access_ownership_by_ue.find(ue->get_ue_index());
    const auto digital_slot_it     = digital_slot_intent_by_ue.find(ue->get_ue_index());

    if (!meas_context.last_ntn_location_report.has_value() && !meas_context.ntn_candidate_beam.has_value() &&
        !has_slot_request && !has_core_location_state && !has_location_request_state && !has_location_freshness_state &&
        relocation_it == ntn_pre_service_relocation_states.end() &&
        connected_handover_it == ntn_connected_handover_states.end() && layer_it == ntn_ue_layer_states.end() &&
        access_ownership_it == access_ownership_by_ue.end() && digital_slot_it == digital_slot_intent_by_ue.end()) {
      continue;
    }

    cu_cp_ntn_ue_status status;
    status.ue_index            = ue->get_ue_index();
    status.du_index            = ue->get_du_index();
    status.rnti                = ue->get_c_rnti();
    status.nof_drbs            = ue->get_up_resource_manager().get_nof_drbs();
    status.qos                 = summarize_ntn_qos_demand(ue->get_up_resource_manager().get_up_context());
    const ntn_ue_capability_summary capability = evaluate_ntn_ue_capability_for_ue(*ue);
    status.ntn_capability_state                  = capability.state;
    status.ntn_capability_reason                 = capability.reason;
    status.ntn_capability_parsed                 = capability.parsed;
    status.ntn_capability_has_nr_container       = capability.has_nr_container;
    status.ntn_capability_non_terrestrial_network_r17 = capability.non_terrestrial_network_r17;
    status.ntn_capability_scenario_support_r17   = capability.ntn_scenario_support_r17;
    status.ntn_capability_scenario               = capability.ntn_scenario;
    status.ntn_capability_parameters_r17         = capability.ntn_parameters_r17;
    status.ntn_capability_scenario_support       = capability.scenario_support;
    status.ntn_capability_deployment_profile     = capability.deployment_profile;
    status.ntn_capability_profile_match          = capability.matched_deployment_profile;
    status.ntn_capability_profile_reason         = capability.profile_block_reason;
    status.last_location       = meas_context.last_ntn_location_report;
    status.has_ul_slot_request = has_slot_request;
    if (status.last_location.has_value()) {
      status.serving_nci     = status.last_location->serving_nci;
      status.serving_beam_id = find_ntn_beam_id_by_nci(status.last_location->serving_nci);
    }
    if (meas_context.ntn_candidate_beam.has_value()) {
      status.candidate_beam_id             = meas_context.ntn_candidate_beam->target_beam_id;
      status.candidate_nci                 = meas_context.ntn_candidate_beam->target_nci;
      status.candidate_handover_triggered  = meas_context.ntn_candidate_beam->handover_triggered;
      status.accepted_handover_attempt_id  = meas_context.ntn_candidate_beam->accepted_handover_attempt_id;
    }
    if (core_location_it != ntn_core_location_reporting_states.end()) {
      status.active_core_location_requests = core_location_it->second.active_requests.size();
    }
    if (location_request_it != ntn_rrc_location_request_states.end()) {
      status.ntn_rrc_location_request_desired        = location_request_it->second.desired;
      status.ntn_rrc_location_request_configured     = location_request_it->second.configured;
      status.ntn_rrc_location_request_pending        = location_request_it->second.pending;
      status.ntn_rrc_location_request_skipped_reason = location_request_it->second.skipped_reason;
    }
    if (location_freshness_it != ntn_location_freshness_states.end()) {
      status.ntn_location_freshness_state  = location_freshness_it->second.state;
      status.ntn_location_freshness_reason = location_freshness_it->second.reason;
      status.ntn_location_release_pending  = location_freshness_it->second.release_pending;
      status.ntn_location_age              = location_freshness_it->second.location_age;
    }
    if (relocation_it != ntn_pre_service_relocation_states.end()) {
      if (!status.serving_nci.has_value()) {
        status.serving_nci     = relocation_it->second.serving_nci;
        status.serving_beam_id = relocation_it->second.target_beam_id;
      }
      status.pre_service_relocation_state           = relocation_it->second.state;
      status.pre_service_relocation_reason          = relocation_it->second.reason;
      status.pre_service_relocation_target_du_index = relocation_it->second.target_du_index;
      status.pre_service_relocation_target_beam_id  = relocation_it->second.target_beam_id;
      status.pre_service_relocation_target_nci      = relocation_it->second.target_nci;
      status.pre_service_relocation_attempt_id      = relocation_it->second.attempt_id;
      status.pre_service_relocation_retry_count     = relocation_it->second.retry_count;
    }
    if (connected_handover_it != ntn_connected_handover_states.end()) {
      status.connected_handover_state                 = connected_handover_it->second.state;
      status.connected_handover_reason                = connected_handover_it->second.reason;
      status.connected_handover_source_beam_id        = connected_handover_it->second.source_beam_id;
      status.connected_handover_target_beam_id        = connected_handover_it->second.target_beam_id;
      status.connected_handover_source_analog_beam_id = connected_handover_it->second.source_analog_beam_id;
      status.connected_handover_target_analog_beam_id = connected_handover_it->second.target_analog_beam_id;
      status.connected_handover_target_nci            = connected_handover_it->second.target_nci;
      status.connected_handover_target_du_index       = connected_handover_it->second.target_du_index;
      status.connected_handover_target_c_rnti         = connected_handover_it->second.target_c_rnti;
      if (!connected_handover_it->second.target_uplink_resource_beam_id.empty()) {
        status.connected_handover_target_uplink_resource_beam_id =
            connected_handover_it->second.target_uplink_resource_beam_id;
      }
      if (connected_handover_it->second.has_target_uplink_resource_nci) {
        status.connected_handover_target_uplink_resource_nci =
            connected_handover_it->second.target_uplink_resource_nci;
      }
      status.connected_handover_target_uplink_resource_du_index =
          connected_handover_it->second.target_uplink_resource_du_index;
      status.connected_handover_target_service_pair_reason =
          connected_handover_it->second.target_service_pair_reason;
      status.connected_handover_target_resource_state = connected_handover_it->second.target_resource_state;
      status.connected_handover_target_sr_srs_applied = connected_handover_it->second.target_sr_srs_applied;
      status.connected_handover_attempt_id            = connected_handover_it->second.attempt_id;
      status.connected_handover_retry_count           = connected_handover_it->second.retry_count;
    }
    if (layer_it != ntn_ue_layer_states.end()) {
      if (layer_it->second.service_state == ntn_service_state_service_bound) {
        status.ntn_runtime_state = ntn_runtime_state_service_bound;
      } else if (layer_it->second.service_state == ntn_service_state_binding_pending) {
        status.ntn_runtime_state = ntn_runtime_state_binding_pending;
      } else if (layer_it->second.service_state == ntn_service_state_blocked ||
                 layer_it->second.access_state == ntn_access_state_blocked) {
        status.ntn_runtime_state = ntn_runtime_state_blocked;
      } else if (layer_it->second.access_state == ntn_access_state_released_after_ics) {
        status.ntn_runtime_state = ntn_runtime_state_control_only;
      } else if (layer_it->second.access_state == ntn_access_state_access_active) {
        status.ntn_runtime_state = ntn_runtime_state_access_active;
      }
      status.analog_access_released = layer_it->second.access_state == ntn_access_state_released_after_ics;
      status.access_layer_state  = layer_it->second.access_state;
      status.access_layer_reason = layer_it->second.access_reason;
      if (!layer_it->second.access_analog_beam_id.empty()) {
        status.access_analog_beam_id = layer_it->second.access_analog_beam_id;
      }
      status.access_du_index = layer_it->second.access_du_index;
      if (layer_it->second.has_access_nci) {
        status.access_nci = layer_it->second.access_nci;
        if (!status.serving_nci.has_value()) {
          status.serving_nci     = layer_it->second.access_nci;
          status.serving_beam_id = find_ntn_beam_id_by_nci(layer_it->second.access_nci);
        }
      }
      if (!layer_it->second.last_downlink_wake_beam_id.empty()) {
        status.last_downlink_wake_beam_id = layer_it->second.last_downlink_wake_beam_id;
      }
      if (!layer_it->second.paired_uplink_access_beam_id.empty()) {
        status.paired_uplink_access_beam_id = layer_it->second.paired_uplink_access_beam_id;
      }
      if (layer_it->second.has_paired_uplink_access_nci) {
        status.paired_uplink_access_nci = layer_it->second.paired_uplink_access_nci;
      }
      status.paired_uplink_access_du_index = layer_it->second.paired_uplink_access_du_index;
      status.paired_access_reason          = layer_it->second.paired_access_reason;
      status.service_layer_state  = layer_it->second.service_state;
      status.service_layer_reason = layer_it->second.service_reason;
      if (!layer_it->second.service_digital_beam_id.empty()) {
        status.service_digital_beam_id = layer_it->second.service_digital_beam_id;
        status.service_downlink_beam_id = layer_it->second.service_digital_beam_id;
      }
      status.service_du_index = layer_it->second.service_du_index;
      if (layer_it->second.has_service_nci) {
        status.service_nci = layer_it->second.service_nci;
      }
      if (!layer_it->second.service_uplink_resource_beam_id.empty()) {
        status.service_uplink_resource_beam_id = layer_it->second.service_uplink_resource_beam_id;
      }
      if (layer_it->second.has_service_uplink_resource_nci) {
        status.service_uplink_resource_nci = layer_it->second.service_uplink_resource_nci;
      }
      status.service_uplink_resource_du_index = layer_it->second.service_uplink_resource_du_index;
      status.service_pair_reason              = layer_it->second.service_pair_reason;
      status.service_binding_source = layer_it->second.service_binding_source;
    }
    if (access_ownership_it != access_ownership_by_ue.end()) {
      status.access_rnti_ownership_state  = access_ownership_it->second.state;
      status.access_rnti_ownership_reason = access_ownership_it->second.reason;
    }
    if (digital_slot_it != digital_slot_intent_by_ue.end()) {
      status.digital_slot_intent_state  = digital_slot_it->second.state;
      status.digital_slot_intent_reason = digital_slot_it->second.reason;
    }
    result.push_back(std::move(status));
  }
  return result;
}

cu_cp_ntn_antenna_intent_snapshot cu_cp_impl::get_current_ntn_antenna_intent_snapshot() const
{
  cu_cp_ntn_antenna_intent_snapshot snapshot;

  std::map<std::string, unsigned> nof_access_active_ues_by_analog;
  for (const auto& layer : ntn_ue_layer_states) {
    if (layer.second.access_state == ntn_access_state_access_active &&
        !layer.second.access_analog_beam_id.empty()) {
      ++nof_access_active_ues_by_analog[layer.second.access_analog_beam_id];
    }
  }

  snapshot.analog_access_intents.reserve(current_ntn_beam_placement_plan.analog_assignments.size());
  for (const auto& assignment : current_ntn_beam_placement_plan.analog_assignments) {
    cu_cp_ntn_analog_access_intent intent;
    intent.analog_beam_id        = assignment.analog_beam_id;
    intent.selected_du_index     = assignment.selected_access_du_index;
    intent.nof_access_active_ues = nof_access_active_ues_by_analog[assignment.analog_beam_id];
    intent.reason                = assignment.reason;
    snapshot.analog_access_intents.push_back(std::move(intent));
  }

  for (const auto& assignment : current_ntn_beam_placement_plan.assignments) {
    const bool has_digital_service_intent =
        assignment.bidirectional_service_ready &&
        (assignment.nof_ues != 0 || assignment.nof_drbs != 0 || assignment.nof_antenna_slots != 0);
    if (!has_digital_service_intent) {
      continue;
    }
    cu_cp_ntn_digital_service_intent intent;
    intent.digital_beam_id     = assignment.beam_id;
    intent.service_du_index    = assignment.du_index;
    intent.nof_ues             = assignment.nof_ues;
    intent.nof_drbs            = assignment.nof_drbs;
    intent.nof_antenna_slots   = assignment.nof_antenna_slots;
    intent.antenna_slot_index  = assignment.antenna_slot_index;
    intent.antenna_slot_period = assignment.antenna_slot_period;
    intent.sr_slot_offset      = assignment.sr_slot_offset;
    intent.sr_slot_period      = assignment.sr_slot_period;
    intent.srs_slot_offset     = assignment.srs_slot_offset;
    intent.srs_slot_period     = assignment.srs_slot_period;
    intent.resource_weight     = assignment.resource_weight;
    intent.resource_share      = assignment.resource_share;
    intent.resource_weight_reason = assignment.resource_weight_reason;
    intent.qos                 = assignment.qos;
    intent.reason              = "loaded_service_calendar";
    snapshot.digital_service_intents.push_back(std::move(intent));
  }

  return snapshot;
}

ntn_beam_service_resource_snapshot cu_cp_impl::get_current_ntn_beam_service_resource_snapshot() const
{
  return ntn_service_resource_mng.get_snapshot();
}

bool cu_cp_impl::handle_ntn_service_switch_over_event(const ntn_service_switch_over_event& event)
{
  const auto previous_controller = ntn_service_switch_over_ctrl;
  if (!ntn_service_switch_over_ctrl.apply_event(event)) {
    logger.warning("Rejected NTN service switch-over event id={}", event.event_id);
    return false;
  }
  if (!refresh_ntn_beam_placement_for_service_policy()) {
    ntn_service_switch_over_ctrl = previous_controller;
    return false;
  }
  schedule_ntn_handover_preferred_service_handovers();
  schedule_ntn_release_allowed_service_releases();
  return true;
}

bool cu_cp_impl::is_ntn_release_allowed_service_layer(
    const ntn_service_switch_over_snapshot&  switch_over_snapshot,
    const ntn_ue_access_service_layer_state& layer) const
{
  if (layer.service_state != ntn_service_state_service_bound || layer.service_digital_beam_id.empty()) {
    return false;
  }

  const nr_cell_identity service_nci = layer.has_service_nci ? layer.service_nci : nr_cell_identity::min();
  return get_beam_service_policy(switch_over_snapshot, layer.service_digital_beam_id, service_nci) ==
         ntn_service_beam_policy::release_allowed;
}

bool cu_cp_impl::is_ntn_handover_preferred_service_layer(
    const ntn_service_switch_over_snapshot&  switch_over_snapshot,
    const ntn_ue_access_service_layer_state& layer) const
{
  if (layer.service_state != ntn_service_state_service_bound || layer.service_digital_beam_id.empty()) {
    return false;
  }

  const nr_cell_identity service_nci = layer.has_service_nci ? layer.service_nci : nr_cell_identity::min();
  if (get_beam_service_policy(switch_over_snapshot, layer.service_digital_beam_id, service_nci) ==
      ntn_service_beam_policy::release_allowed) {
    return false;
  }

  const std::optional<nr_cell_identity> optional_service_nci =
      layer.has_service_nci ? std::optional<nr_cell_identity>{layer.service_nci} : std::nullopt;
  return std::any_of(switch_over_snapshot.active_events.begin(),
                     switch_over_snapshot.active_events.end(),
                     [&layer, optional_service_nci](const ntn_service_switch_over_event& event) {
                       return event.type == ntn_service_switch_over_type::hard &&
                              event.policy == ntn_service_switch_over_policy::handover_preferred &&
                              switch_over_event_affects_beam_or_nci(
                                  event, layer.service_digital_beam_id, optional_service_nci);
                     });
}

bool cu_cp_impl::is_ntn_beam_hopping_service_layer(
    const ntn_service_switch_over_snapshot&  switch_over_snapshot,
    const ntn_ue_access_service_layer_state& layer) const
{
  if (layer.service_state != ntn_service_state_service_bound || layer.service_digital_beam_id.empty()) {
    return false;
  }

  const nr_cell_identity service_nci = layer.has_service_nci ? layer.service_nci : nr_cell_identity::min();
  if (get_beam_service_policy(switch_over_snapshot, layer.service_digital_beam_id, service_nci) !=
      ntn_service_beam_policy::normal) {
    return false;
  }
  if (current_ntn_predictive_window_valid &&
      contains_beam_id(current_ntn_predictive_drain_soon_beam_ids, layer.service_digital_beam_id)) {
    return false;
  }

  const ntn_beam_du_assignment* assignment =
      find_assignment_for_beam(current_ntn_beam_placement_plan, layer.service_digital_beam_id);
  return assignment != nullptr && assignment->state == ntn_beam_assignment_state::draining;
}

bool cu_cp_impl::is_ntn_predictive_beam_hopping_service_layer(
    const ntn_ue_access_service_layer_state& layer) const
{
  return current_ntn_predictive_window_valid && layer.service_state == ntn_service_state_service_bound &&
         !layer.service_digital_beam_id.empty() &&
         contains_beam_id(current_ntn_predictive_drain_soon_beam_ids, layer.service_digital_beam_id);
}

bool cu_cp_impl::is_ntn_release_allowed_release_temporarily_blocked(ue_index_t ue_index) const
{
  if (ntn_location_watchdog_release_requested_ues.count(ue_index) != 0) {
    return true;
  }

  const auto relocation_it = ntn_pre_service_relocation_states.find(ue_index);
  if (relocation_it != ntn_pre_service_relocation_states.end() &&
      (relocation_it->second.state == "pending_pre_service" || relocation_it->second.state == "preparing" ||
       relocation_it->second.state == "failed_retryable")) {
    return true;
  }

  const auto handover_it = ntn_connected_handover_states.find(ue_index);
  if (handover_it != ntn_connected_handover_states.end() &&
      (handover_it->second.state == "target_preloading" ||
       handover_it->second.state == "target_resource_preparing" ||
       handover_it->second.state == "target_resource_applied" ||
       handover_it->second.state == "handover_preparing" ||
       handover_it->second.state == "failed_retryable")) {
    return true;
  }

  return false;
}

bool cu_cp_impl::is_ntn_handover_preferred_temporarily_blocked(ue_index_t ue_index) const
{
  if (ntn_release_allowed_release_requested_ues.count(ue_index) != 0 ||
      ntn_location_watchdog_release_requested_ues.count(ue_index) != 0) {
    return true;
  }

  const auto relocation_it = ntn_pre_service_relocation_states.find(ue_index);
  if (relocation_it != ntn_pre_service_relocation_states.end() &&
      (relocation_it->second.state == "pending_pre_service" || relocation_it->second.state == "preparing" ||
       relocation_it->second.state == "failed_retryable" || relocation_it->second.state == "blocked")) {
    return true;
  }

  const auto handover_it = ntn_connected_handover_states.find(ue_index);
  if (handover_it != ntn_connected_handover_states.end() &&
      (handover_it->second.state == "target_preloading" ||
       handover_it->second.state == "target_resource_preparing" ||
       handover_it->second.state == "target_resource_applied" ||
       handover_it->second.state == "handover_preparing" ||
       handover_it->second.state == "failed_retryable" ||
       handover_it->second.state == "blocked")) {
    return true;
  }

  return false;
}

bool cu_cp_impl::is_ntn_load_balancing_handover_temporarily_blocked(ue_index_t ue_index) const
{
  if (is_ntn_handover_preferred_temporarily_blocked(ue_index)) {
    return true;
  }

  const auto& ntn_cfg = cfg.mobility.meas_manager_config.ntn_location_mobility;
  if (ntn_cfg.multi_beam_load_balancing_handover_cooldown.count() == 0) {
    return false;
  }
  const auto last_it = ntn_load_balancing_last_handover_times.find(ue_index);
  if (last_it == ntn_load_balancing_last_handover_times.end()) {
    return false;
  }
  return std::chrono::steady_clock::now() - last_it->second <
         ntn_cfg.multi_beam_load_balancing_handover_cooldown;
}

bool cu_cp_impl::is_ntn_preheated_target_ready(const ntn_beam_preheat_state& state) const
{
  const auto& ntn_cfg = cfg.mobility.meas_manager_config.ntn_location_mobility;
  if (!state.applied || state.applied_time.time_since_epoch().count() == 0) {
    return false;
  }
  return ntn_cfg.preheated_beam_min_ready_time.count() == 0 ||
         std::chrono::steady_clock::now() - state.applied_time >= ntn_cfg.preheated_beam_min_ready_time;
}

bool cu_cp_impl::is_ntn_analog_rebalance_pair_cooldown_active(const std::string& source_analog_beam_id,
                                                              const std::string& target_analog_beam_id) const
{
  const auto& ntn_cfg = cfg.mobility.meas_manager_config.ntn_location_mobility;
  if (source_analog_beam_id.empty() || target_analog_beam_id.empty() || source_analog_beam_id == target_analog_beam_id ||
      ntn_cfg.analog_rebalance_pair_cooldown.count() == 0) {
    return false;
  }
  const std::string pair_key = make_ntn_analog_pair_cooldown_key(source_analog_beam_id, target_analog_beam_id);
  const auto        last_it  = ntn_analog_rebalance_pair_last_times.find(pair_key);
  return last_it != ntn_analog_rebalance_pair_last_times.end() &&
         std::chrono::steady_clock::now() - last_it->second < ntn_cfg.analog_rebalance_pair_cooldown;
}

cu_cp_impl::ntn_service_handover_target_selection cu_cp_impl::select_ntn_service_handover_target_assignment(
    const ntn_service_switch_over_snapshot&  switch_over_snapshot,
    const ntn_ue_access_service_layer_state& layer) const
{
  const auto& ntn_cfg = cfg.mobility.meas_manager_config.ntn_location_mobility;
  const ntn_beam_position* source_beam = find_ntn_beam_cfg(ntn_cfg.beams, layer.service_digital_beam_id);
  const std::string        source_analog_beam_id = source_beam != nullptr ? source_beam->analog_beam_id : "";
  const ntn_beam_du_assignment* selected = nullptr;
  ntn_service_handover_target_selection selection;

  auto get_analog_beam_id = [&](const std::string& beam_id) {
    const ntn_beam_position* beam = find_ntn_beam_cfg(ntn_cfg.beams, beam_id);
    return beam != nullptr ? beam->analog_beam_id : std::string{};
  };

  auto same_derived_tac = [&](const std::string& lhs, const std::string& rhs) {
    const ntn_beam_tac_result lhs_tac = derive_ntn_beam_tac(lhs);
    const ntn_beam_tac_result rhs_tac = derive_ntn_beam_tac(rhs);
    return lhs_tac.tac.has_value() && rhs_tac.tac.has_value() && lhs_tac.tac.value() == rhs_tac.tac.value();
  };

  auto is_assignment_common_eligible = [&](const ntn_beam_du_assignment& assignment,
                                           bool require_mobility_eligible) {
    if (assignment.beam_id.empty() || assignment.beam_id == layer.service_digital_beam_id) {
      return false;
    }
    if (layer.has_service_nci && assignment.nci.value() == layer.service_nci.value()) {
      return false;
    }
    const auto target_timeline_it = current_ntn_predictive_beam_timeline.find(assignment.beam_id);
    if (current_ntn_predictive_window_valid && target_timeline_it != current_ntn_predictive_beam_timeline.end() &&
        target_timeline_it->second.first_exit_offset.has_value() &&
        target_timeline_it->second.first_exit_offset.value() <= current_ntn_predictive_lead_time) {
      return false;
    }
    if (require_mobility_eligible && !contains_beam_id(current_ntn_mobility_eligible_beam_ids, assignment.beam_id)) {
      return false;
    }
    const ntn_service_beam_policy target_policy =
        get_beam_service_policy(switch_over_snapshot, assignment.beam_id, assignment.nci);
    if (ntn_service_policy_blocks_new_demand(target_policy)) {
      return false;
    }
    const bool active_loaded = assignment.state == ntn_beam_assignment_state::active_loaded;
    const bool candidate_in_window =
        assignment.state == ntn_beam_assignment_state::candidate && assignment.in_hopping_window;
    if (!active_loaded && !candidate_in_window) {
      return false;
    }
    if (assignment.du_index == du_index_t::invalid || assignment.du_assignment_reason != "eligible") {
      return false;
    }
    if (!ntn_cfg.analog_beams.empty() && assignment.access_du_index == du_index_t::invalid) {
      return false;
    }
    return assignment.resource_domain_eligible;
  };

  auto is_bidirectional_assignment_eligible = [&](const ntn_beam_du_assignment& assignment) {
    return is_assignment_common_eligible(assignment, true) && assignment.downlink_enabled && assignment.uplink_enabled;
  };

  auto is_downlink_assignment_eligible = [&](const ntn_beam_du_assignment& assignment) {
    return is_assignment_common_eligible(assignment, false) && assignment.downlink_enabled;
  };

  auto is_uplink_assignment_eligible = [&](const ntn_beam_du_assignment& assignment) {
    return is_assignment_common_eligible(assignment, false) && assignment.uplink_enabled;
  };

  auto has_same_source_analog = [&](const ntn_beam_du_assignment& assignment) {
    if (source_analog_beam_id.empty()) {
      return false;
    }
    return get_analog_beam_id(assignment.beam_id) == source_analog_beam_id;
  };

  auto is_better_target = [&](const ntn_beam_du_assignment& candidate, const ntn_beam_du_assignment& current) {
    const bool candidate_same_analog = has_same_source_analog(candidate);
    const bool current_same_analog   = has_same_source_analog(current);
    if (candidate_same_analog != current_same_analog) {
      return candidate_same_analog;
    }

    const bool candidate_active = candidate.state == ntn_beam_assignment_state::active_loaded;
    const bool current_active   = current.state == ntn_beam_assignment_state::active_loaded;
    if (candidate_active != current_active) {
      return candidate_active;
    }
    if (candidate.elevation_deg != current.elevation_deg) {
      return candidate.elevation_deg > current.elevation_deg;
    }

    const unsigned candidate_order = get_served_candidate_order(current_ntn_served_beam_candidates, candidate.beam_id);
    const unsigned current_order   = get_served_candidate_order(current_ntn_served_beam_candidates, current.beam_id);
    if (candidate_order != current_order) {
      return candidate_order < current_order;
    }
    return candidate.beam_id < current.beam_id;
  };

  for (const ntn_beam_du_assignment& assignment : current_ntn_beam_placement_plan.assignments) {
    if (!is_bidirectional_assignment_eligible(assignment)) {
      continue;
    }
    if (selected == nullptr || is_better_target(assignment, *selected)) {
      selected = &assignment;
    }
  }

  if (selected != nullptr) {
    selection.assignment = selected;
    return selection;
  }

  const ntn_beam_du_assignment* selected_downlink = nullptr;
  const ntn_beam_du_assignment* selected_uplink   = nullptr;
  auto is_better_uplink = [&](const ntn_beam_du_assignment& candidate, const ntn_beam_du_assignment& current) {
    const bool candidate_active = candidate.state == ntn_beam_assignment_state::active_loaded;
    const bool current_active   = current.state == ntn_beam_assignment_state::active_loaded;
    if (candidate_active != current_active) {
      return candidate_active;
    }
    if (candidate.elevation_deg != current.elevation_deg) {
      return candidate.elevation_deg > current.elevation_deg;
    }
    return candidate.beam_id < current.beam_id;
  };

  for (const ntn_beam_du_assignment& downlink_assignment : current_ntn_beam_placement_plan.assignments) {
    if (!is_downlink_assignment_eligible(downlink_assignment)) {
      continue;
    }
    const std::string target_analog_beam_id = get_analog_beam_id(downlink_assignment.beam_id);
    if (target_analog_beam_id.empty()) {
      continue;
    }
    const ntn_beam_du_assignment* uplink_match = nullptr;
    for (const ntn_beam_du_assignment& uplink_assignment : current_ntn_beam_placement_plan.assignments) {
      if (!is_uplink_assignment_eligible(uplink_assignment) ||
          uplink_assignment.beam_id == downlink_assignment.beam_id ||
          uplink_assignment.du_index != downlink_assignment.du_index ||
          get_analog_beam_id(uplink_assignment.beam_id) != target_analog_beam_id ||
          !same_derived_tac(downlink_assignment.beam_id, uplink_assignment.beam_id)) {
        continue;
      }
      if (uplink_match == nullptr || is_better_uplink(uplink_assignment, *uplink_match)) {
        uplink_match = &uplink_assignment;
      }
    }
    if (uplink_match == nullptr) {
      continue;
    }
    if (selected_downlink == nullptr || is_better_target(downlink_assignment, *selected_downlink)) {
      selected_downlink = &downlink_assignment;
      selected_uplink   = uplink_match;
    }
  }

  if (selected_downlink != nullptr && selected_uplink != nullptr) {
    selection.assignment                 = selected_downlink;
    selection.uplink_resource_assignment = selected_uplink;
    selection.service_pair_reason        = "same_analog_tac_uplink_resource";
  }
  return selection;
}

cu_cp_impl::ntn_load_balancing_target_selection cu_cp_impl::select_ntn_load_balancing_target_assignment(
    const ntn_service_switch_over_snapshot&  switch_over_snapshot,
    const ntn_ue_access_service_layer_state& layer,
    unsigned                                 moving_nof_drbs)
{
  const auto& ntn_cfg = cfg.mobility.meas_manager_config.ntn_location_mobility;
  const ntn_beam_position* source_beam = find_ntn_beam_cfg(ntn_cfg.beams, layer.service_digital_beam_id);
  const std::string        source_analog_beam_id = source_beam != nullptr ? source_beam->analog_beam_id : "";
  const ntn_beam_du_assignment* source_assignment =
      find_assignment_for_beam(current_ntn_beam_placement_plan, layer.service_digital_beam_id);
  ntn_load_balancing_target_selection selection;
  selection.source_analog_beam_id = source_analog_beam_id.empty() ? "none" : source_analog_beam_id;

  struct projected_load {
    unsigned nof_ues  = 0;
    unsigned nof_drbs = 0;
  };

  auto is_pending_handover_state = [](const ntn_connected_handover_ue_state& state) {
    return state.state == "target_preloading" || state.state == "target_resource_preparing" ||
           state.state == "target_resource_applied" || state.state == "handover_preparing";
  };

  auto get_analog_beam_id = [&](const std::string& beam_id) {
    const ntn_beam_position* beam = find_ntn_beam_cfg(ntn_cfg.beams, beam_id);
    return beam != nullptr ? beam->analog_beam_id : std::string{};
  };

  auto get_pending_target_load = [&](const std::string& beam_id) {
    projected_load load;
    for (const auto& entry : ntn_connected_handover_states) {
      const ntn_connected_handover_ue_state& state = entry.second;
      if (!is_pending_handover_state(state) ||
          (state.target_beam_id != beam_id && state.target_uplink_resource_beam_id != beam_id)) {
        continue;
      }
      ++load.nof_ues;
      load.nof_drbs += state.nof_drbs;
    }
    return load;
  };

  auto get_current_analog_load = [&](const std::string& analog_beam_id) {
    projected_load load;
    if (analog_beam_id.empty()) {
      return load;
    }
    for (const ntn_beam_du_assignment& assignment : current_ntn_beam_placement_plan.assignments) {
      if (get_analog_beam_id(assignment.beam_id) != analog_beam_id) {
        continue;
      }
      load.nof_ues += assignment.nof_ues;
      load.nof_drbs += assignment.nof_drbs;
    }
    return load;
  };

  auto get_pending_incoming_analog_load = [&](const std::string& analog_beam_id) {
    projected_load load;
    if (analog_beam_id.empty()) {
      return load;
    }
    for (const auto& entry : ntn_connected_handover_states) {
      const ntn_connected_handover_ue_state& state = entry.second;
      if (!is_pending_handover_state(state)) {
        continue;
      }
      std::string target_analog = state.target_analog_beam_id;
      if (target_analog.empty()) {
        target_analog = get_analog_beam_id(state.target_beam_id);
      }
      if (target_analog != analog_beam_id) {
        continue;
      }
      std::string source_analog = state.source_analog_beam_id;
      if (source_analog.empty()) {
        source_analog = get_analog_beam_id(state.source_beam_id);
      }
      if (source_analog == target_analog) {
        continue;
      }
      ++load.nof_ues;
      load.nof_drbs += state.nof_drbs;
    }
    return load;
  };

  auto is_analog_active_or_preheated = [&](const std::string& analog_beam_id) {
    if (analog_beam_id.empty()) {
      return true;
    }
    for (const ntn_beam_du_assignment& assignment : current_ntn_beam_placement_plan.assignments) {
      if (get_analog_beam_id(assignment.beam_id) == analog_beam_id &&
          assignment.access_du_index != du_index_t::invalid && assignment.access_du_reason == "eligible") {
        return true;
      }
    }
    return false;
  };

  auto du_capacity_allows = [&](const ntn_beam_du_assignment& assignment) {
    const unsigned loaded_digital_service_beam_cap = get_ntn_loaded_digital_service_beam_cap(ntn_cfg);
    const std::vector<ntn_du_beam_capacity> capacities =
        build_ntn_du_beam_capacities(du_db,
                                      ue_mng,
                                      ntn_cfg.beams,
                                      loaded_digital_service_beam_cap,
                                      cfg.admission.max_nof_ues,
                                      cfg.admission.max_nof_drbs_per_ue);
    auto capacity_it =
        std::find_if(capacities.begin(), capacities.end(), [&](const ntn_du_beam_capacity& capacity) {
          return capacity.du_index == assignment.du_index;
        });
    if (capacity_it == capacities.end()) {
      return false;
    }
    if (capacity_it->max_ues == 0 && capacity_it->max_drbs == 0) {
      return true;
    }
    projected_load load{capacity_it->current_ues, capacity_it->current_drbs};
    for (const ntn_beam_du_assignment& beam_assignment : current_ntn_beam_placement_plan.assignments) {
      if (beam_assignment.du_index != assignment.du_index) {
        continue;
      }
      load.nof_ues += beam_assignment.nof_ues;
      load.nof_drbs += beam_assignment.nof_drbs;
    }
    for (const auto& entry : ntn_connected_handover_states) {
      const ntn_connected_handover_ue_state& state = entry.second;
      if (!is_pending_handover_state(state) || state.target_du_index != assignment.du_index) {
        continue;
      }
      ++load.nof_ues;
      load.nof_drbs += state.nof_drbs;
    }
    if (source_assignment == nullptr || source_assignment->du_index != assignment.du_index) {
      ++load.nof_ues;
      load.nof_drbs += moving_nof_drbs;
    }
    if (capacity_it->max_ues != 0 && load.nof_ues > capacity_it->max_ues) {
      return false;
    }
    if (capacity_it->max_drbs != 0 && load.nof_drbs > capacity_it->max_drbs) {
      return false;
    }
    return true;
  };

  auto projected_capacity_allows = [&](const ntn_beam_du_assignment& assignment) {
    const projected_load pending_target_load = get_pending_target_load(assignment.beam_id);
    const unsigned       projected_digital_ues = assignment.nof_ues + pending_target_load.nof_ues + 1U;
    const unsigned       projected_digital_drbs = assignment.nof_drbs + pending_target_load.nof_drbs + moving_nof_drbs;
    if (assignment.digital_ue_cap != 0 && projected_digital_ues > assignment.digital_ue_cap) {
      return false;
    }
    if (assignment.digital_drb_cap != 0 && projected_digital_drbs > assignment.digital_drb_cap) {
      return false;
    }

    const std::string target_analog_beam_id = get_analog_beam_id(assignment.beam_id);
    if (!target_analog_beam_id.empty()) {
      projected_load projected_analog_load = get_current_analog_load(target_analog_beam_id);
      const projected_load pending_analog_load = get_pending_incoming_analog_load(target_analog_beam_id);
      projected_analog_load.nof_ues += pending_analog_load.nof_ues;
      projected_analog_load.nof_drbs += pending_analog_load.nof_drbs;
      if (target_analog_beam_id != source_analog_beam_id) {
        ++projected_analog_load.nof_ues;
        projected_analog_load.nof_drbs += moving_nof_drbs;
      }
      if (assignment.analog_service_bound_ue_cap != 0 &&
          projected_analog_load.nof_ues > assignment.analog_service_bound_ue_cap) {
        return false;
      }
      if (assignment.analog_drb_cap != 0 && projected_analog_load.nof_drbs > assignment.analog_drb_cap) {
        return false;
      }
    }

    return du_capacity_allows(assignment);
  };

  const projected_load source_analog_load = get_current_analog_load(source_analog_beam_id);
  const bool source_analog_at_capacity =
      source_assignment != nullptr &&
      ((source_assignment->analog_service_bound_ue_cap != 0 &&
        source_analog_load.nof_ues >= source_assignment->analog_service_bound_ue_cap) ||
       (source_assignment->analog_drb_cap != 0 && source_analog_load.nof_drbs >= source_assignment->analog_drb_cap));

  auto is_assignment_eligible = [&](const ntn_beam_du_assignment& assignment) {
    if (assignment.beam_id.empty() || assignment.beam_id == layer.service_digital_beam_id) {
      return false;
    }
    if (layer.has_service_nci && assignment.nci.value() == layer.service_nci.value()) {
      return false;
    }
    if (!contains_beam_id(current_ntn_mobility_eligible_beam_ids, assignment.beam_id)) {
      return false;
    }
    if (!assignment.downlink_enabled || !assignment.uplink_enabled) {
      return false;
    }
    if (current_ntn_predictive_window_valid &&
        contains_beam_id(current_ntn_predictive_drain_soon_beam_ids, assignment.beam_id)) {
      return false;
    }
    const ntn_service_beam_policy target_policy =
        get_beam_service_policy(switch_over_snapshot, assignment.beam_id, assignment.nci);
    if (ntn_service_policy_blocks_new_demand(target_policy)) {
      return false;
    }
    const bool active_loaded = assignment.state == ntn_beam_assignment_state::active_loaded;
    const bool candidate_in_window =
        assignment.state == ntn_beam_assignment_state::candidate && assignment.in_hopping_window;
    if (!active_loaded && !candidate_in_window) {
      return false;
    }
    const std::string target_analog_beam_id = get_analog_beam_id(assignment.beam_id);
    auto preheat_it = ntn_preheat_states.find(assignment.beam_id);
    if (preheat_it != ntn_preheat_states.end() && preheat_it->second.applied && !preheat_it->second.ready) {
      if (is_ntn_preheated_target_ready(preheat_it->second)) {
        preheat_it->second.ready = true;
      } else if (target_analog_beam_id != source_analog_beam_id) {
        selection.skipped_preheat_ready_guard = true;
        selection.preheat_beam_id            = assignment.beam_id;
        selection.preheat_analog_beam_id = target_analog_beam_id.empty() ? "none" : target_analog_beam_id;
        if (selection.reason == "no_eligible_target") {
          selection.reason = "preheat_ready_guard";
        }
        return false;
      }
    }
    if (target_analog_beam_id != source_analog_beam_id && !is_analog_active_or_preheated(target_analog_beam_id)) {
      selection.skipped_cold_analog = true;
      selection.preheat_beam_id = assignment.beam_id;
      selection.preheat_analog_beam_id = target_analog_beam_id.empty() ? "none" : target_analog_beam_id;
      if (selection.reason == "no_eligible_target") {
        selection.reason = "cold_analog";
      }
      return false;
    }
    if (assignment.du_index == du_index_t::invalid || assignment.du_assignment_reason != "eligible") {
      return false;
    }
    if (!ntn_cfg.analog_beams.empty() && assignment.access_du_index == du_index_t::invalid) {
      return false;
    }
    if (!assignment.resource_domain_eligible) {
      return false;
    }
    if (target_analog_beam_id != source_analog_beam_id &&
        is_ntn_analog_rebalance_pair_cooldown_active(source_analog_beam_id, target_analog_beam_id)) {
      selection.skipped_pair_cooldown = true;
      if (selection.reason == "no_eligible_target") {
        selection.reason = "pair_cooldown";
      }
      return false;
    }
    if (!projected_capacity_allows(assignment)) {
      selection.skipped_projected_capacity = true;
      selection.reason = "projected_capacity";
      return false;
    }
    return true;
  };

  auto has_same_source_analog = [&](const ntn_beam_du_assignment& assignment) {
    if (source_analog_beam_id.empty()) {
      return false;
    }
    const ntn_beam_position* beam = find_ntn_beam_cfg(ntn_cfg.beams, assignment.beam_id);
    return beam != nullptr && beam->analog_beam_id == source_analog_beam_id;
  };

  auto is_better_target = [&](const ntn_beam_du_assignment& candidate, const ntn_beam_du_assignment& current) {
    const bool candidate_same_analog = has_same_source_analog(candidate);
    const bool current_same_analog   = has_same_source_analog(current);
    if (candidate_same_analog != current_same_analog) {
      return source_analog_at_capacity ? !candidate_same_analog : candidate_same_analog;
    }

    const projected_load candidate_pending_load = get_pending_target_load(candidate.beam_id);
    const projected_load current_pending_load   = get_pending_target_load(current.beam_id);
    const unsigned candidate_projected_ues      = candidate.nof_ues + candidate_pending_load.nof_ues;
    const unsigned current_projected_ues        = current.nof_ues + current_pending_load.nof_ues;
    if (candidate_projected_ues != current_projected_ues) {
      return candidate_projected_ues < current_projected_ues;
    }
    const unsigned candidate_projected_drbs = candidate.nof_drbs + candidate_pending_load.nof_drbs;
    const unsigned current_projected_drbs   = current.nof_drbs + current_pending_load.nof_drbs;
    if (candidate_projected_drbs != current_projected_drbs) {
      return candidate_projected_drbs < current_projected_drbs;
    }

    const bool candidate_active = candidate.state == ntn_beam_assignment_state::active_loaded;
    const bool current_active   = current.state == ntn_beam_assignment_state::active_loaded;
    if (candidate_active != current_active) {
      return candidate_active;
    }
    if (candidate.elevation_deg != current.elevation_deg) {
      return candidate.elevation_deg > current.elevation_deg;
    }

    const unsigned candidate_order = get_served_candidate_order(current_ntn_served_beam_candidates, candidate.beam_id);
    const unsigned current_order   = get_served_candidate_order(current_ntn_served_beam_candidates, current.beam_id);
    if (candidate_order != current_order) {
      return candidate_order < current_order;
    }
    return candidate.beam_id < current.beam_id;
  };

  for (const ntn_beam_du_assignment& assignment : current_ntn_beam_placement_plan.assignments) {
    if (!is_assignment_eligible(assignment)) {
      continue;
    }
    if (selection.assignment == nullptr || is_better_target(assignment, *selection.assignment)) {
      selection.assignment = &assignment;
    }
  }

  auto same_derived_tac = [&](const std::string& lhs, const std::string& rhs) {
    const ntn_beam_tac_result lhs_tac = derive_ntn_beam_tac(lhs);
    const ntn_beam_tac_result rhs_tac = derive_ntn_beam_tac(rhs);
    return lhs_tac.tac.has_value() && rhs_tac.tac.has_value() && lhs_tac.tac.value() == rhs_tac.tac.value();
  };

  auto is_service_pair_direction_eligible = [&](const ntn_beam_du_assignment& assignment, bool downlink) {
    if (assignment.beam_id.empty() || assignment.beam_id == layer.service_digital_beam_id) {
      return false;
    }
    if (layer.has_service_nci && assignment.nci.value() == layer.service_nci.value()) {
      return false;
    }
    if (downlink && !assignment.downlink_enabled) {
      return false;
    }
    if (!downlink && !assignment.uplink_enabled) {
      return false;
    }
    if (current_ntn_predictive_window_valid &&
        contains_beam_id(current_ntn_predictive_drain_soon_beam_ids, assignment.beam_id)) {
      return false;
    }
    const ntn_service_beam_policy target_policy =
        get_beam_service_policy(switch_over_snapshot, assignment.beam_id, assignment.nci);
    if (ntn_service_policy_blocks_new_demand(target_policy)) {
      return false;
    }
    const bool active_loaded = assignment.state == ntn_beam_assignment_state::active_loaded;
    const bool candidate_in_window =
        assignment.state == ntn_beam_assignment_state::candidate && assignment.in_hopping_window;
    if (!active_loaded && !candidate_in_window) {
      return false;
    }
    if (assignment.du_index == du_index_t::invalid || assignment.du_assignment_reason != "eligible") {
      return false;
    }
    if (!ntn_cfg.analog_beams.empty() && assignment.access_du_index == du_index_t::invalid) {
      return false;
    }
    return assignment.resource_domain_eligible;
  };

  auto is_better_uplink_resource = [&](const ntn_beam_du_assignment& candidate,
                                       const ntn_beam_du_assignment& current) {
    const bool candidate_active = candidate.state == ntn_beam_assignment_state::active_loaded;
    const bool current_active   = current.state == ntn_beam_assignment_state::active_loaded;
    if (candidate_active != current_active) {
      return candidate_active;
    }
    if (candidate.elevation_deg != current.elevation_deg) {
      return candidate.elevation_deg > current.elevation_deg;
    }
    return candidate.beam_id < current.beam_id;
  };

  if (selection.assignment == nullptr) {
    const ntn_beam_du_assignment* selected_downlink = nullptr;
    const ntn_beam_du_assignment* selected_uplink   = nullptr;
    for (const ntn_beam_du_assignment& downlink_assignment : current_ntn_beam_placement_plan.assignments) {
      if (!is_service_pair_direction_eligible(downlink_assignment, true)) {
        continue;
      }
      const std::string target_analog_beam_id = get_analog_beam_id(downlink_assignment.beam_id);
      if (target_analog_beam_id.empty()) {
        continue;
      }
      if (target_analog_beam_id != source_analog_beam_id && !is_analog_active_or_preheated(target_analog_beam_id)) {
        selection.skipped_cold_analog = true;
        selection.reason              = "cold_analog";
        selection.preheat_beam_id     = downlink_assignment.beam_id;
        selection.preheat_analog_beam_id = target_analog_beam_id;
        continue;
      }
      if (target_analog_beam_id != source_analog_beam_id &&
          is_ntn_analog_rebalance_pair_cooldown_active(source_analog_beam_id, target_analog_beam_id)) {
        selection.skipped_pair_cooldown = true;
        if (selection.reason == "no_eligible_target") {
          selection.reason = "pair_cooldown";
        }
        continue;
      }
      if (!projected_capacity_allows(downlink_assignment)) {
        selection.skipped_projected_capacity = true;
        selection.reason = "projected_capacity";
        continue;
      }

      const ntn_beam_du_assignment* uplink_match = nullptr;
      for (const ntn_beam_du_assignment& uplink_assignment : current_ntn_beam_placement_plan.assignments) {
        if (!is_service_pair_direction_eligible(uplink_assignment, false) ||
            uplink_assignment.beam_id == downlink_assignment.beam_id ||
            uplink_assignment.du_index != downlink_assignment.du_index ||
            get_analog_beam_id(uplink_assignment.beam_id) != target_analog_beam_id ||
            !same_derived_tac(downlink_assignment.beam_id, uplink_assignment.beam_id)) {
          continue;
        }
        if (!projected_capacity_allows(uplink_assignment)) {
          selection.skipped_projected_capacity = true;
          selection.reason = "projected_capacity";
          continue;
        }
        if (uplink_match == nullptr || is_better_uplink_resource(uplink_assignment, *uplink_match)) {
          uplink_match = &uplink_assignment;
        }
      }
      if (uplink_match == nullptr) {
        continue;
      }
      if (selected_downlink == nullptr || is_better_target(downlink_assignment, *selected_downlink)) {
        selected_downlink = &downlink_assignment;
        selected_uplink   = uplink_match;
      }
    }
    if (selected_downlink != nullptr && selected_uplink != nullptr) {
      selection.assignment                 = selected_downlink;
      selection.uplink_resource_assignment = selected_uplink;
      selection.service_pair_reason        = "same_analog_tac_uplink_resource";
    }
  }

  if (selection.assignment == nullptr && !source_analog_beam_id.empty()) {
    for (const ntn_served_beam_candidate& candidate : current_ntn_served_beam_candidates) {
      if (candidate.beam_id.empty() || candidate.beam_id == layer.service_digital_beam_id) {
        continue;
      }
      const std::string candidate_analog_beam_id = get_analog_beam_id(candidate.beam_id);
      if (candidate_analog_beam_id.empty() || candidate_analog_beam_id == source_analog_beam_id) {
        continue;
      }
      if (!is_analog_active_or_preheated(candidate_analog_beam_id)) {
        selection.skipped_cold_analog = true;
        selection.reason              = "cold_analog";
        selection.preheat_beam_id     = candidate.beam_id;
        selection.preheat_analog_beam_id = candidate_analog_beam_id;
        break;
      }
    }
  }

  if (selection.assignment != nullptr) {
    selection.reason = "eligible";
    selection.target_analog_beam_id = get_analog_beam_id(selection.assignment->beam_id);
    if (selection.target_analog_beam_id.empty()) {
      selection.target_analog_beam_id = "none";
    }
    selection.same_analog =
        !source_analog_beam_id.empty() && selection.target_analog_beam_id == source_analog_beam_id;
  }
  return selection;
}

void cu_cp_impl::record_ntn_beam_preheat_request(const ntn_ue_access_service_layer_state& layer,
                                                 const ntn_load_balancing_target_selection& target_selection,
                                                 const char* reason)
{
  if (target_selection.preheat_beam_id.empty() || target_selection.preheat_beam_id == "none") {
    return;
  }

  auto [preheat_it, is_new_request] = ntn_preheat_states.try_emplace(target_selection.preheat_beam_id);
  ntn_beam_preheat_state& state = preheat_it->second;
  state.target_beam_id          = target_selection.preheat_beam_id;
  state.source_analog_beam_id =
      target_selection.source_analog_beam_id.empty() ? "none" : target_selection.source_analog_beam_id;
  state.target_analog_beam_id =
      target_selection.preheat_analog_beam_id.empty() ? "none" : target_selection.preheat_analog_beam_id;
  state.reason = reason;
  if (is_new_request) {
    ++nof_ntn_preheat_requested;
    if (std::string_view(reason) == "cold_analog") {
      ++nof_ntn_cold_analog_preheat_requested;
    }
    last_ntn_preheat_reason           = reason;
    last_ntn_preheat_source_analog_id = state.source_analog_beam_id;
    last_ntn_preheat_target_analog_id = state.target_analog_beam_id;
    logger.debug("Requested NTN beam preheat target_beam={} source_beam={} source_analog={} target_analog={} reason={}",
                 target_selection.preheat_beam_id,
                 layer.service_digital_beam_id,
                 last_ntn_preheat_source_analog_id,
                 last_ntn_preheat_target_analog_id,
                 reason);
  }
}

void cu_cp_impl::record_ntn_target_reservation(const std::string& target_beam_id,
                                               const std::string& source_beam_id,
                                               const std::string& source_analog_beam_id,
                                               const std::string& target_analog_beam_id,
                                               const char*        reason)
{
  if (target_beam_id.empty() || target_beam_id == "none") {
    return;
  }

  auto [it, inserted] = ntn_target_reservations.try_emplace(target_beam_id);
  ntn_target_reservation_state& state = it->second;
  state.target_beam_id                = target_beam_id;
  state.source_beam_id                = source_beam_id.empty() ? "none" : source_beam_id;
  state.source_analog_beam_id         = source_analog_beam_id.empty() ? "none" : source_analog_beam_id;
  state.target_analog_beam_id         = target_analog_beam_id.empty() ? "none" : target_analog_beam_id;
  state.reason                        = reason;
  state.last_held_time                = std::chrono::steady_clock::now();
  if (inserted || state.created_time.time_since_epoch().count() == 0) {
    state.created_time = state.last_held_time;
    ++nof_ntn_target_reservation_created;
  }
  last_ntn_scheduling_guard_reason           = reason;
  last_ntn_scheduling_guard_source_beam_id   = state.source_beam_id;
  last_ntn_scheduling_guard_target_beam_id   = state.target_beam_id;
  last_ntn_scheduling_guard_source_analog_id = state.source_analog_beam_id;
  last_ntn_scheduling_guard_target_analog_id = state.target_analog_beam_id;
}

void cu_cp_impl::consume_ntn_target_reservation(const std::string& target_beam_id,
                                                const std::string& source_beam_id,
                                                const std::string& source_analog_beam_id,
                                                const std::string& target_analog_beam_id,
                                                const char*        reason)
{
  if (target_beam_id.empty() || target_beam_id == "none") {
    return;
  }
  if (ntn_target_reservations.erase(target_beam_id) != 0) {
    ++nof_ntn_target_reservation_consumed;
  }
  last_ntn_scheduling_guard_reason           = reason;
  last_ntn_scheduling_guard_source_beam_id   = source_beam_id.empty() ? "none" : source_beam_id;
  last_ntn_scheduling_guard_target_beam_id   = target_beam_id;
  last_ntn_scheduling_guard_source_analog_id = source_analog_beam_id.empty() ? "none" : source_analog_beam_id;
  last_ntn_scheduling_guard_target_analog_id = target_analog_beam_id.empty() ? "none" : target_analog_beam_id;
}

bool cu_cp_impl::is_ntn_target_capacity_reserved_for_beam(const std::string& beam_id) const
{
  if (beam_id.empty()) {
    return false;
  }
  if (is_ntn_headroom_reserved_beam(beam_id)) {
    return true;
  }
  const auto reservation_it = ntn_target_reservations.find(beam_id);
  return reservation_it != ntn_target_reservations.end();
}

bool cu_cp_impl::expire_ntn_target_reservations()
{
  const auto& ntn_cfg = cfg.mobility.meas_manager_config.ntn_location_mobility;
  if (ntn_cfg.digital_target_reservation_hold_time.count() == 0 || ntn_target_reservations.empty()) {
    return false;
  }

  const auto now = std::chrono::steady_clock::now();
  bool       expired = false;
  for (auto it = ntn_target_reservations.begin(); it != ntn_target_reservations.end();) {
    if (is_ntn_headroom_reserved_beam(it->first) ||
        now - it->second.last_held_time < ntn_cfg.digital_target_reservation_hold_time) {
      ++it;
      continue;
    }
    last_ntn_scheduling_guard_reason           = "reservation_expired";
    last_ntn_scheduling_guard_source_beam_id   = it->second.source_beam_id;
    last_ntn_scheduling_guard_target_beam_id   = it->second.target_beam_id;
    last_ntn_scheduling_guard_source_analog_id = it->second.source_analog_beam_id;
    last_ntn_scheduling_guard_target_analog_id = it->second.target_analog_beam_id;
    it = ntn_target_reservations.erase(it);
    ++nof_ntn_target_reservation_expired;
    expired = true;
  }
  return expired;
}

bool cu_cp_impl::update_ntn_preheat_results(const ntn_beam_placement_plan& plan)
{
  bool newly_applied = false;
  for (auto& entry : ntn_preheat_states) {
    ntn_beam_preheat_state& state = entry.second;
    const ntn_beam_du_assignment* assignment = find_assignment_for_beam(plan, state.target_beam_id);
    if (assignment == nullptr || assignment->state == ntn_beam_assignment_state::inactive) {
      continue;
    }

    if (!state.sent) {
      state.sent = true;
      ++nof_ntn_preheat_sent;
    }

    if (assignment->state == ntn_beam_assignment_state::active_loaded && assignment->du_index != du_index_t::invalid &&
        assignment->du_assignment_reason == "eligible" && assignment->resource_domain_eligible &&
        assignment->bidirectional_service_ready) {
      if (!state.applied) {
        state.applied = true;
        state.applied_time = std::chrono::steady_clock::now();
        state.ready   = is_ntn_preheated_target_ready(state);
        newly_applied = true;
        ++nof_ntn_preheat_applied;
        record_ntn_target_reservation(state.target_beam_id,
                                      "none",
                                      state.source_analog_beam_id,
                                      state.target_analog_beam_id,
                                      "preheat_applied");
      } else if (!state.ready && is_ntn_preheated_target_ready(state)) {
        state.ready = true;
      }
      last_ntn_preheat_reason           = "applied";
      last_ntn_preheat_source_analog_id = state.source_analog_beam_id;
      last_ntn_preheat_target_analog_id = state.target_analog_beam_id;
      continue;
    }

    if (state.applied || state.skipped) {
      continue;
    }

    state.skipped = true;
    ++nof_ntn_preheat_skipped;
    if (!assignment->resource_domain_eligible) {
      ++nof_ntn_preheat_skipped_by_policy;
      last_ntn_preheat_reason = "policy";
    } else {
      ++nof_ntn_preheat_skipped_by_capacity;
      last_ntn_preheat_reason = "capacity";
    }
    last_ntn_preheat_source_analog_id = state.source_analog_beam_id;
    last_ntn_preheat_target_analog_id = state.target_analog_beam_id;
  }
  return newly_applied;
}

bool cu_cp_impl::expire_idle_ntn_preheated_beams()
{
  const auto& ntn_cfg = cfg.mobility.meas_manager_config.ntn_location_mobility;
  if (ntn_cfg.preheated_beam_hold_time.count() == 0 || ntn_preheat_states.empty()) {
    return false;
  }

  auto is_pending_handover_state = [](const ntn_connected_handover_ue_state& state) {
    return state.state == "target_preloading" || state.state == "target_resource_preparing" ||
           state.state == "target_resource_applied" || state.state == "handover_preparing";
  };

  auto beam_has_active_use = [&](const std::string& beam_id) {
    for (const auto& layer_entry : ntn_ue_layer_states) {
      const ntn_ue_access_service_layer_state& layer = layer_entry.second;
      if (layer.service_digital_beam_id != beam_id) {
        continue;
      }
      if (layer.service_state == ntn_service_state_service_bound ||
          layer.service_state == ntn_service_state_binding_pending) {
        return true;
      }
    }
    for (const auto& handover_entry : ntn_connected_handover_states) {
      const ntn_connected_handover_ue_state& handover = handover_entry.second;
      if (handover.target_beam_id == beam_id && is_pending_handover_state(handover)) {
        return true;
      }
    }
    const ntn_beam_du_assignment* assignment = find_assignment_for_beam(current_ntn_beam_placement_plan, beam_id);
    return assignment != nullptr && (assignment->nof_ues != 0 || assignment->nof_drbs != 0);
  };

  const auto now = std::chrono::steady_clock::now();
  bool       demoted = false;
  for (auto it = ntn_preheat_states.begin(); it != ntn_preheat_states.end();) {
    ntn_beam_preheat_state& state = it->second;
    if (!state.applied || state.applied_time.time_since_epoch().count() == 0 ||
        now - state.applied_time < ntn_cfg.preheated_beam_hold_time || beam_has_active_use(state.target_beam_id)) {
      ++it;
      continue;
    }
    if (!is_ntn_preheated_target_ready(state) || is_ntn_target_capacity_reserved_for_beam(state.target_beam_id)) {
      ++nof_ntn_preheat_demote_deferred_by_reservation;
      last_ntn_scheduling_guard_reason           = is_ntn_target_capacity_reserved_for_beam(state.target_beam_id)
                                                       ? "reservation_deferred_demote"
                                                       : "preheat_ready_guard";
      last_ntn_scheduling_guard_source_beam_id   = "none";
      last_ntn_scheduling_guard_target_beam_id   = state.target_beam_id;
      last_ntn_scheduling_guard_source_analog_id = state.source_analog_beam_id;
      last_ntn_scheduling_guard_target_analog_id = state.target_analog_beam_id;
      ++it;
      continue;
    }

    last_ntn_preheat_reason           = "idle_demoted";
    last_ntn_preheat_source_analog_id = state.source_analog_beam_id;
    last_ntn_preheat_target_analog_id = state.target_analog_beam_id;
    it = ntn_preheat_states.erase(it);
    ++nof_ntn_preheat_demoted;
    demoted = true;
  }
  return demoted;
}

bool cu_cp_impl::is_ntn_load_balancing_source_hot(const ntn_ue_access_service_layer_state& layer,
                                                  const ntn_load_balancing_target_selection& target_selection) const
{
  const auto& ntn_cfg = cfg.mobility.meas_manager_config.ntn_location_mobility;
  const ntn_beam_du_assignment* source_assignment =
      find_assignment_for_beam(current_ntn_beam_placement_plan, layer.service_digital_beam_id);
  if (source_assignment == nullptr || source_assignment->state != ntn_beam_assignment_state::active_loaded) {
    return false;
  }
  const ntn_beam_du_assignment* target_assignment = target_selection.assignment;

  const bool source_at_ue_cap =
      source_assignment->digital_ue_cap != 0 && source_assignment->digital_ue_load >= source_assignment->digital_ue_cap;
  const bool source_at_drb_cap =
      source_assignment->digital_drb_cap != 0 && source_assignment->digital_drb_load >= source_assignment->digital_drb_cap;
  auto get_analog_beam_id = [&](const std::string& beam_id) {
    const ntn_beam_position* beam = find_ntn_beam_cfg(ntn_cfg.beams, beam_id);
    return beam != nullptr ? beam->analog_beam_id : std::string{};
  };
  auto get_current_analog_load = [&](const std::string& analog_beam_id) {
    unsigned nof_ues = 0;
    unsigned nof_drbs = 0;
    if (analog_beam_id.empty()) {
      return std::make_pair(nof_ues, nof_drbs);
    }
    for (const ntn_beam_du_assignment& assignment : current_ntn_beam_placement_plan.assignments) {
      if (get_analog_beam_id(assignment.beam_id) == analog_beam_id) {
        nof_ues += assignment.nof_ues;
        nof_drbs += assignment.nof_drbs;
      }
    }
    return std::make_pair(nof_ues, nof_drbs);
  };

  const std::string source_analog_beam_id = get_analog_beam_id(layer.service_digital_beam_id);
  const auto        source_analog_load    = get_current_analog_load(source_analog_beam_id);
  const bool source_analog_at_ue_cap = source_assignment->analog_service_bound_ue_cap != 0 &&
                                       source_analog_load.first >= source_assignment->analog_service_bound_ue_cap;
  const bool source_analog_at_drb_cap =
      source_assignment->analog_drb_cap != 0 && source_analog_load.second >= source_assignment->analog_drb_cap;

  const bool digital_hot_without_target =
      source_assignment->nof_ues >= ntn_cfg.multi_beam_load_balancing_min_ue_delta || source_at_ue_cap ||
      source_at_drb_cap;
  if (target_assignment == nullptr) {
    return digital_hot_without_target || source_analog_at_ue_cap || source_analog_at_drb_cap;
  }
  if (source_assignment->nof_ues <= target_assignment->nof_ues) {
    const std::string target_analog_beam_id = target_selection.target_analog_beam_id == "none"
                                                  ? std::string{}
                                                  : target_selection.target_analog_beam_id;
    if (!source_analog_beam_id.empty() && target_analog_beam_id != source_analog_beam_id) {
      const auto target_analog_load = get_current_analog_load(target_analog_beam_id);
      if (source_analog_load.first > target_analog_load.first) {
        const unsigned analog_delta = source_analog_load.first - target_analog_load.first;
        return analog_delta >= ntn_cfg.multi_beam_load_balancing_min_ue_delta || source_analog_at_ue_cap ||
               source_analog_at_drb_cap;
      }
      return source_analog_at_ue_cap || source_analog_at_drb_cap;
    }
    return false;
  }
  const unsigned ue_delta = source_assignment->nof_ues - target_assignment->nof_ues;
  if (ue_delta >= ntn_cfg.multi_beam_load_balancing_min_ue_delta || source_at_ue_cap || source_at_drb_cap) {
    return true;
  }

  const std::string target_analog_beam_id =
      target_selection.target_analog_beam_id == "none" ? std::string{} : target_selection.target_analog_beam_id;
  if (!source_analog_beam_id.empty() && target_analog_beam_id != source_analog_beam_id) {
    const auto target_analog_load = get_current_analog_load(target_analog_beam_id);
    if (source_analog_load.first > target_analog_load.first) {
      const unsigned analog_delta = source_analog_load.first - target_analog_load.first;
      return analog_delta >= ntn_cfg.multi_beam_load_balancing_min_ue_delta || source_analog_at_ue_cap ||
             source_analog_at_drb_cap;
    }
    return source_analog_at_ue_cap || source_analog_at_drb_cap;
  }
  return false;
}

bool cu_cp_impl::schedule_ntn_service_handover(
    ue_index_t                               ue_index,
    const ntn_ue_access_service_layer_state& layer,
    const ntn_service_handover_target_selection& target_selection,
    const char*                              handover_reason)
{
  const auto& ntn_cfg = cfg.mobility.meas_manager_config.ntn_location_mobility;
  const ntn_beam_du_assignment* target_assignment = target_selection.assignment;
  if (target_assignment == nullptr) {
    return false;
  }

  nr_cell_identity serving_nci = layer.has_service_nci ? layer.service_nci : nr_cell_identity::min();
  if (!layer.has_service_nci) {
    const ntn_beam_position* source_beam = find_ntn_beam_cfg(ntn_cfg.beams, layer.service_digital_beam_id);
    if (source_beam == nullptr) {
      return false;
    }
    serving_nci = source_beam->nci;
  }

  const du_cell_configuration* target_cell =
      find_du_cell_by_nci(du_db, target_assignment->du_index, target_assignment->nci);
  if (target_cell == nullptr) {
    return false;
  }

  const bool uses_service_pair = target_selection.uses_service_pair();
  if (uses_service_pair) {
    ++nof_ntn_service_pair_handover_targets;
    last_ntn_service_pair_handover_reason = target_selection.service_pair_reason;
  }

  const auto now = std::chrono::steady_clock::now();
  ntn_location_handover_trigger trigger;
  trigger.ue_index                     = ue_index;
  trigger.serving_nci                  = serving_nci;
  trigger.handover_attempt_id          = next_ntn_service_switch_over_handover_attempt_id++;
  trigger.target_beam_id               = target_assignment->beam_id;
  const unsigned gnb_id_bit_length     = cfg.node.gnb_id.bit_length != 0 ? cfg.node.gnb_id.bit_length : 22;
  trigger.target_gnb_id                = target_cell->cgi.nci.gnb_id(gnb_id_bit_length);
  trigger.target_nci                   = target_assignment->nci;
  trigger.target_pci                   = target_cell->pci;
  trigger.handover_reason              = handover_reason;
  if (uses_service_pair) {
    trigger.target_uplink_resource_beam_id = target_selection.uplink_resource_assignment->beam_id;
    trigger.target_uplink_resource_nci     = target_selection.uplink_resource_assignment->nci;
    trigger.target_uplink_resource_du_index = target_selection.uplink_resource_assignment->du_index;
    trigger.target_service_pair_reason     = target_selection.service_pair_reason;
  }
  trigger.served_beam_ids_snapshot     = current_ntn_mobility_eligible_beam_ids;
  trigger.last_location_report.ue_index = ue_index;
  trigger.last_location_report.serving_nci = serving_nci;
  trigger.last_location_report.received_time = now;
  trigger.candidate_since              = now;
  trigger.last_report_time             = now;
  trigger.consecutive_location_reports = 1;

  const bool scheduled = handle_ntn_location_handover_required(trigger);
  if (uses_service_pair) {
    if (scheduled) {
      ++nof_ntn_service_pair_handover_scheduled;
    } else {
      ++nof_ntn_service_pair_handover_skipped;
      if (last_ntn_service_pair_handover_reason == "none") {
        last_ntn_service_pair_handover_reason = "schedule_failed";
      }
    }
  }
  return scheduled;
}

void cu_cp_impl::schedule_ntn_multi_beam_load_balancing_handovers()
{
  const auto& ntn_cfg = cfg.mobility.meas_manager_config.ntn_location_mobility;
  if (!ntn_cfg.enabled || !ntn_cfg.multi_beam_load_balancing_enabled ||
      ntn_cfg.multi_beam_load_balancing_max_handovers_per_eval == 0 ||
      is_current_ntn_assistance_stale(std::chrono::steady_clock::now())) {
    return;
  }
  for (const auto& handover_entry : ntn_connected_handover_states) {
    const ntn_connected_handover_ue_state& state = handover_entry.second;
    if (state.reason == "load_balancing" &&
        (state.state == "target_preloading" || state.state == "target_resource_preparing" ||
         state.state == "target_resource_applied" || state.state == "handover_preparing" ||
         state.state == "failed_retryable" || state.state == "blocked")) {
      return;
    }
  }

  const ntn_service_switch_over_snapshot switch_over_snapshot = get_current_ntn_service_switch_over_snapshot();
  std::vector<std::pair<ue_index_t, ntn_ue_access_service_layer_state>> service_layers;
  service_layers.reserve(ntn_ue_layer_states.size());
  for (const auto& layer_entry : ntn_ue_layer_states) {
    if (layer_entry.second.service_state == ntn_service_state_binding_pending) {
      return;
    }
    service_layers.emplace_back(layer_entry.first, layer_entry.second);
  }
  std::sort(service_layers.begin(),
            service_layers.end(),
            [](const auto& lhs, const auto& rhs) { return ue_index_to_uint(lhs.first) < ue_index_to_uint(rhs.first); });

  bool     evaluated_hot_source = false;
  unsigned scheduled_this_eval  = 0;
  for (const auto& layer_entry : service_layers) {
    if (scheduled_this_eval >= ntn_cfg.multi_beam_load_balancing_max_handovers_per_eval) {
      break;
    }

    const ue_index_t                         ue_index = layer_entry.first;
    const ntn_ue_access_service_layer_state& layer    = layer_entry.second;
    if (layer.service_state != ntn_service_state_service_bound || layer.service_digital_beam_id.empty()) {
      continue;
    }

    cu_cp_ue* ue = ue_mng.find_du_ue(ue_index);
    if (ue == nullptr) {
      continue;
    }
    const unsigned moving_nof_drbs = ue->get_up_resource_manager().get_nof_drbs();
    const ntn_load_balancing_target_selection target_selection =
        select_ntn_load_balancing_target_assignment(switch_over_snapshot, layer, moving_nof_drbs);
    const ntn_beam_du_assignment* target_assignment = target_selection.assignment;
    if (!is_ntn_load_balancing_source_hot(layer, target_selection)) {
      continue;
    }

    evaluated_hot_source                     = true;
    ++nof_ntn_load_balancing_handover_requested;
    last_ntn_load_balancing_source_beam_id   = layer.service_digital_beam_id;
    last_ntn_load_balancing_target_beam_id   = target_assignment != nullptr ? target_assignment->beam_id : "none";
    last_ntn_load_balancing_source_analog_id = target_selection.source_analog_beam_id;
    last_ntn_load_balancing_target_analog_id = target_selection.target_analog_beam_id;

    if (is_ntn_load_balancing_handover_temporarily_blocked(ue_index)) {
      ++nof_ntn_load_balancing_handover_skipped;
      last_ntn_load_balancing_reason = "temporarily_blocked";
      continue;
    }
    if (target_assignment == nullptr) {
      ++nof_ntn_load_balancing_handover_skipped;
      if (target_selection.skipped_projected_capacity) {
        ++nof_ntn_load_balancing_skipped_projected_capacity;
      }
      if (target_selection.skipped_cold_analog) {
        ++nof_ntn_load_balancing_skipped_cold_analog;
        record_ntn_beam_preheat_request(layer, target_selection, "cold_analog");
      }
      if (target_selection.skipped_preheat_ready_guard) {
        ++nof_ntn_handover_skipped_by_preheat_ready_guard;
        last_ntn_scheduling_guard_reason           = "preheat_ready_guard";
        last_ntn_scheduling_guard_source_beam_id   = layer.service_digital_beam_id;
        last_ntn_scheduling_guard_target_beam_id   = target_selection.preheat_beam_id;
        last_ntn_scheduling_guard_source_analog_id = target_selection.source_analog_beam_id;
        last_ntn_scheduling_guard_target_analog_id = target_selection.preheat_analog_beam_id;
      }
      if (target_selection.skipped_pair_cooldown) {
        ++nof_ntn_handover_skipped_by_pair_cooldown;
        last_ntn_scheduling_guard_reason           = "pair_cooldown";
        last_ntn_scheduling_guard_source_beam_id   = layer.service_digital_beam_id;
        last_ntn_scheduling_guard_target_beam_id   = "none";
        last_ntn_scheduling_guard_source_analog_id = target_selection.source_analog_beam_id;
        last_ntn_scheduling_guard_target_analog_id = target_selection.target_analog_beam_id;
      }
      last_ntn_load_balancing_reason = target_selection.reason;
      continue;
    }

    const std::string scheduled_source_beam_id   = layer.service_digital_beam_id;
    const std::string scheduled_target_beam_id   = target_assignment->beam_id;
    const std::string scheduled_source_analog_id = target_selection.source_analog_beam_id;
    const std::string scheduled_target_analog_id = target_selection.target_analog_beam_id;

    ntn_service_handover_target_selection handover_target;
    handover_target.assignment                 = target_selection.assignment;
    handover_target.uplink_resource_assignment = target_selection.uplink_resource_assignment;
    handover_target.service_pair_reason        = target_selection.service_pair_reason;
    if (schedule_ntn_service_handover(ue_index, layer, handover_target, "load_balancing")) {
      ++nof_ntn_load_balancing_handover_scheduled;
      if (target_selection.same_analog) {
        ++nof_ntn_load_balancing_same_analog_scheduled;
        last_ntn_load_balancing_reason = "same_analog_scheduled";
      } else {
        ++nof_ntn_load_balancing_cross_analog_scheduled;
        last_ntn_load_balancing_reason = "cross_analog_scheduled";
        const std::string cooldown_key =
            make_ntn_analog_pair_cooldown_key(scheduled_source_analog_id, scheduled_target_analog_id);
        ntn_analog_rebalance_pair_last_times[cooldown_key] = std::chrono::steady_clock::now();
      }
      consume_ntn_target_reservation(scheduled_target_beam_id,
                                     scheduled_source_beam_id,
                                     scheduled_source_analog_id,
                                     scheduled_target_analog_id,
                                     "handover_scheduled");
      ++scheduled_this_eval;
      ntn_load_balancing_last_handover_times[ue_index] = std::chrono::steady_clock::now();
    } else {
      ++nof_ntn_load_balancing_handover_skipped;
      last_ntn_load_balancing_reason = "schedule_failed";
    }
  }

  if (evaluated_hot_source) {
    ++nof_ntn_load_balancing_evaluations;
  }
}

void cu_cp_impl::schedule_ntn_beam_hopping_service_handovers()
{
  if (is_current_ntn_assistance_stale(std::chrono::steady_clock::now())) {
    return;
  }

  const ntn_service_switch_over_snapshot switch_over_snapshot = get_current_ntn_service_switch_over_snapshot();

  for (const auto& layer_entry : ntn_ue_layer_states) {
    const ue_index_t                         ue_index = layer_entry.first;
    const ntn_ue_access_service_layer_state& layer    = layer_entry.second;
    if (!is_ntn_beam_hopping_service_layer(switch_over_snapshot, layer)) {
      continue;
    }

    ++nof_ntn_beam_hopping_ues_requested;
    if (is_ntn_handover_preferred_temporarily_blocked(ue_index)) {
      ++nof_ntn_beam_hopping_ues_skipped;
      continue;
    }

    const ntn_service_handover_target_selection target_selection =
        select_ntn_service_handover_target_assignment(switch_over_snapshot, layer);
    if (target_selection.assignment == nullptr) {
      ++nof_ntn_beam_hopping_ues_skipped;
      continue;
    }

    if (schedule_ntn_service_handover(ue_index, layer, target_selection, "beam_hopping")) {
      ++nof_ntn_beam_hopping_ues_scheduled;
    } else {
      ++nof_ntn_beam_hopping_ues_skipped;
    }
  }
}

void cu_cp_impl::schedule_ntn_predictive_beam_hopping_service_handovers()
{
  if (!current_ntn_predictive_window_valid || current_ntn_predictive_drain_soon_beam_ids.empty() ||
      is_current_ntn_assistance_stale(std::chrono::steady_clock::now())) {
    return;
  }

  const ntn_service_switch_over_snapshot switch_over_snapshot = get_current_ntn_service_switch_over_snapshot();

  for (const auto& layer_entry : ntn_ue_layer_states) {
    const ue_index_t                         ue_index = layer_entry.first;
    const ntn_ue_access_service_layer_state& layer    = layer_entry.second;
    if (!is_ntn_predictive_beam_hopping_service_layer(layer)) {
      continue;
    }

    ++nof_ntn_predictive_beam_hopping_ues_requested;
    if (is_ntn_handover_preferred_temporarily_blocked(ue_index)) {
      ++nof_ntn_predictive_beam_hopping_ues_skipped;
      continue;
    }

    const ntn_service_handover_target_selection target_selection =
        select_ntn_service_handover_target_assignment(switch_over_snapshot, layer);
    if (target_selection.assignment == nullptr) {
      ++nof_ntn_predictive_beam_hopping_ues_skipped;
      continue;
    }

    const ntn_beam_du_assignment* source_assignment =
        find_assignment_for_beam(current_ntn_beam_placement_plan, layer.service_digital_beam_id);
    const bool cross_satellite_target =
        source_assignment != nullptr && !source_assignment->serving_satellite_id.empty() &&
        !target_selection.assignment->serving_satellite_id.empty() &&
        source_assignment->serving_satellite_id != target_selection.assignment->serving_satellite_id;
    const char* handover_reason =
        cross_satellite_target ? "multi_satellite_predictive_beam_hopping" : "predictive_beam_hopping";
    if (schedule_ntn_service_handover(ue_index, layer, target_selection, handover_reason)) {
      ++nof_ntn_predictive_beam_hopping_ues_scheduled;
    } else {
      ++nof_ntn_predictive_beam_hopping_ues_skipped;
    }
  }
}

void cu_cp_impl::schedule_ntn_handover_preferred_service_handovers()
{
  const ntn_service_switch_over_snapshot switch_over_snapshot = get_current_ntn_service_switch_over_snapshot();

  for (const auto& layer_entry : ntn_ue_layer_states) {
    const ue_index_t                         ue_index = layer_entry.first;
    const ntn_ue_access_service_layer_state& layer    = layer_entry.second;
    if (!is_ntn_handover_preferred_service_layer(switch_over_snapshot, layer)) {
      continue;
    }

    ++nof_ntn_handover_preferred_ues_requested;
    if (is_ntn_handover_preferred_temporarily_blocked(ue_index)) {
      ++nof_ntn_handover_preferred_ues_skipped;
      continue;
    }

    const ntn_service_handover_target_selection target_selection =
        select_ntn_service_handover_target_assignment(switch_over_snapshot, layer);
    if (target_selection.assignment == nullptr) {
      ++nof_ntn_handover_preferred_ues_skipped;
      continue;
    }

    if (schedule_ntn_service_handover(ue_index, layer, target_selection, "service_switch_over")) {
      ++nof_ntn_handover_preferred_ues_scheduled;
    } else {
      ++nof_ntn_handover_preferred_ues_skipped;
    }
  }
}

void cu_cp_impl::schedule_ntn_release_allowed_service_releases()
{
  const ntn_service_switch_over_snapshot switch_over_snapshot = get_current_ntn_service_switch_over_snapshot();
  cu_cp_ue_context_release_batch_command release_command;
  std::vector<ue_index_t>                release_ues_to_track;

  for (const auto& layer_entry : ntn_ue_layer_states) {
    const ue_index_t                           ue_index = layer_entry.first;
    const ntn_ue_access_service_layer_state& layer    = layer_entry.second;
    if (!is_ntn_release_allowed_service_layer(switch_over_snapshot, layer)) {
      continue;
    }

    ++nof_ntn_release_allowed_ues_requested;
    if (ntn_release_allowed_release_requested_ues.count(ue_index) != 0 ||
        is_ntn_release_allowed_release_temporarily_blocked(ue_index)) {
      ++nof_ntn_release_allowed_ues_skipped;
      continue;
    }

    release_command.ues.push_back({ue_index, ngap_cause_radio_network_t::release_due_to_ngran_generated_reason});
    release_ues_to_track.push_back(ue_index);
  }

  if (release_command.ues.empty()) {
    return;
  }

  for (ue_index_t ue_index : release_ues_to_track) {
    ntn_release_allowed_release_requested_ues.insert(ue_index);
  }
  const unsigned nof_releases_to_schedule = release_command.ues.size();
  auto           completion_task =
      [this, release_command = std::move(release_command)](coro_context<async_task<void>>& ctx) mutable {
        cu_cp_ue_context_release_batch_response response;
        CORO_BEGIN(ctx);
        CORO_AWAIT_VALUE(response, release_ues(release_command));
        for (ue_index_t ue_index : response.ues_not_found) {
          ntn_release_allowed_release_requested_ues.erase(ue_index);
        }
        for (ue_index_t ue_index : response.duplicate_ues) {
          ntn_release_allowed_release_requested_ues.erase(ue_index);
        }
        for (ue_index_t ue_index : response.failed_to_schedule_ues) {
          ntn_release_allowed_release_requested_ues.erase(ue_index);
        }
        CORO_RETURN();
      };

  if (!common_task_sched.schedule_async_task(launch_async(std::move(completion_task)))) {
    for (ue_index_t ue_index : release_ues_to_track) {
      ntn_release_allowed_release_requested_ues.erase(ue_index);
    }
    nof_ntn_release_allowed_ues_skipped += nof_releases_to_schedule;
    return;
  }

  nof_ntn_release_allowed_ues_scheduled += nof_releases_to_schedule;
}

bool cu_cp_impl::clear_ntn_service_switch_over_event(uint64_t event_id)
{
  const auto previous_controller = ntn_service_switch_over_ctrl;
  if (!ntn_service_switch_over_ctrl.clear_event(event_id)) {
    logger.debug("NTN service switch-over event id={} was not active", event_id);
    return false;
  }
  if (!refresh_ntn_beam_placement_for_service_policy()) {
    ntn_service_switch_over_ctrl = previous_controller;
    return false;
  }
  return true;
}

bool cu_cp_impl::handle_ntn_manual_override(const ntn_manual_override_command& command)
{
  switch (command.mode) {
    case ntn_manual_override_mode::none:
    case ntn_manual_override_mode::freeze_current_state:
    case ntn_manual_override_mode::replace_service_state:
    case ntn_manual_override_mode::restore_automatic_source:
      break;
    default:
      logger.warning("Rejected NTN manual override mode={}", static_cast<unsigned>(command.mode));
      return false;
  }

  const auto previous_controller = ntn_service_switch_over_ctrl;
  const auto previous_served_beam_candidates = current_ntn_served_beam_candidates;
  const auto previous_satellite_ecef          = current_ntn_satellite_ecef;
  const auto previous_satellite_states        = current_ntn_satellite_states;
  const auto previous_satellite_epoch         = current_ntn_satellite_epoch;
  const auto previous_satellite_received_time = current_ntn_satellite_received_time;
  if (command.mode == ntn_manual_override_mode::replace_service_state) {
    if (command.replacement_satellite_ecef.has_value()) {
      current_ntn_satellite_ecef          = command.replacement_satellite_ecef.value();
      current_ntn_satellite_states        = {{"sat-0", command.replacement_satellite_ecef.value()}};
      current_ntn_satellite_epoch         = std::chrono::system_clock::now();
      current_ntn_satellite_received_time = std::chrono::steady_clock::now();
    }
    if (!command.replacement_served_beam_ids.empty() && !update_ntn_served_beams(command.replacement_served_beam_ids)) {
      current_ntn_satellite_ecef          = previous_satellite_ecef;
      current_ntn_satellite_states        = previous_satellite_states;
      current_ntn_satellite_epoch         = previous_satellite_epoch;
      current_ntn_satellite_received_time = previous_satellite_received_time;
      return false;
    }
  }

  if (!ntn_service_switch_over_ctrl.apply_manual_override(command)) {
    logger.warning("Rejected NTN manual override mode={}", static_cast<unsigned>(command.mode));
    if (command.mode == ntn_manual_override_mode::replace_service_state) {
      current_ntn_satellite_ecef          = previous_satellite_ecef;
      current_ntn_satellite_states        = previous_satellite_states;
      current_ntn_satellite_epoch         = previous_satellite_epoch;
      current_ntn_satellite_received_time = previous_satellite_received_time;
      if (!update_ntn_served_beam_candidates(previous_served_beam_candidates)) {
        logger.debug("Could not restore previous NTN served beam candidates after manual override rejection");
      }
    }
    return false;
  }

  if (!refresh_ntn_beam_placement_for_service_policy()) {
    ntn_service_switch_over_ctrl = previous_controller;
    if (command.mode == ntn_manual_override_mode::replace_service_state) {
      current_ntn_satellite_ecef          = previous_satellite_ecef;
      current_ntn_satellite_states        = previous_satellite_states;
      current_ntn_satellite_epoch         = previous_satellite_epoch;
      current_ntn_satellite_received_time = previous_satellite_received_time;
      if (!update_ntn_served_beam_candidates(previous_served_beam_candidates)) {
        logger.debug("Could not restore previous NTN served beam candidates after manual override refresh failure");
      }
    }
    return false;
  }
  return true;
}

ntn_repair_response cu_cp_impl::handle_ntn_repair_command(const ntn_repair_command& command)
{
  const auto& ntn_cfg = cfg.mobility.meas_manager_config.ntn_location_mobility;
  ntn_repair_response response;

  const bool include_resources =
      command.scope == ntn_repair_scope::resources || command.scope == ntn_repair_scope::all;
  const bool include_sib19 = command.scope == ntn_repair_scope::sib19 || command.scope == ntn_repair_scope::all;

  const ntn_beam_service_resource_snapshot resource_snapshot = ntn_service_resource_mng.get_snapshot();
  if (include_resources) {
    response.audit_targets = collect_ntn_resource_audit_targets().size();
    response.existing_failed += resource_snapshot.nof_rnti_leases_rejected_by_du +
                                resource_snapshot.nof_digital_slot_rejected_by_du +
                                resource_snapshot.nof_digital_slot_rollback +
                                resource_snapshot.nof_resource_repairs_failed;
    response.existing_blockers += resource_snapshot.nof_resource_repairs_retry_exhausted +
                                  resource_snapshot.nof_resource_repairs_blocked_conflict;
  }
  if (include_sib19) {
    for (const cu_cp_ntn_beam_status& beam_status : get_current_ntn_beam_status()) {
      if (beam_status.sib19_broadcast_state != "stale_blocked" || beam_status.sib19_packed_bytes > 0 ||
          beam_status.sib19_broadcast_generation > 0) {
        ++response.sib19_candidates;
      }
    }
    const cu_cp_ntn_runtime_status runtime_status = get_current_ntn_runtime_status();
    response.existing_failed += runtime_status.nof_sib19_broadcast_rejected_by_du +
                                runtime_status.nof_sib19_broadcast_stale_blocked;
  }

  if (!ntn_cfg.enabled) {
    response.accepted  = false;
    response.reason    = "ntn_disabled";
    response.next_step = "enable_ntn_location_mobility";
    return response;
  }

  if (command.mode == ntn_repair_mode::dry_run) {
    response.accepted  = true;
    response.reason    = "dry_run";
    response.next_step = "rerun_with_apply_to_trigger_refresh";
    return response;
  }

  if (include_resources) {
    schedule_ntn_rnti_lease_pool_updates_for_access_beams();
    schedule_ntn_ul_slot_updates_for_online_ues();
    schedule_ntn_resource_audits();
  }
  if (include_sib19) {
    schedule_ntn_sib19_broadcast_updates();
  }

  response.accepted = true;
  if (command.scope == ntn_repair_scope::resources) {
    response.reason = "resources_repair_requested";
  } else if (command.scope == ntn_repair_scope::sib19) {
    response.reason = "sib19_refresh_requested";
  } else {
    response.reason = "repair_requested";
  }
  response.next_step = "check_ntn_state_for_async_results";
  return response;
}

ntn_service_switch_over_snapshot cu_cp_impl::get_current_ntn_service_switch_over_snapshot() const
{
  return ntn_service_switch_over_ctrl.build_snapshot(get_configured_ntn_beam_cells());
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
  expire_ntn_target_reservations();
  expire_idle_ntn_preheated_beams();
  const unsigned loaded_digital_service_beam_cap = get_ntn_loaded_digital_service_beam_cap(ntn_cfg);
  ntn_beam_placement_request request;
  request.beams                = ntn_cfg.beams;
  request.analog_beams         = ntn_cfg.analog_beams;
  request.visible_beams        = candidates;
  request.max_active_beams     = loaded_digital_service_beam_cap;
  request.resource_policy      = ntn_cfg.resource_policy;
  request.demand_aware_resource_weighting_enabled = ntn_cfg.demand_aware_beam_scheduling_enabled;
  request.du_capacities        = build_ntn_du_beam_capacities(du_db,
                                                              ue_mng,
                                                              ntn_cfg.beams,
                                                              loaded_digital_service_beam_cap,
                                                              cfg.admission.max_nof_ues,
                                                              cfg.admission.max_nof_drbs_per_ue);
  request.beam_loads           = build_ntn_beam_loads_for_current_service_contexts();
  merge_pending_ntn_connected_handover_loads(request.beam_loads);
  request.preheated_beam_ids.reserve(ntn_preheat_states.size());
  for (const auto& entry : ntn_preheat_states) {
    request.preheated_beam_ids.push_back(entry.first);
  }
  request.previous_assignments = current_ntn_beam_placement_plan.assignments;
  const bool has_ntn_service_binding_pending =
      std::any_of(ntn_ue_layer_states.begin(), ntn_ue_layer_states.end(), [](const auto& entry) {
        return entry.second.service_state == ntn_service_state_binding_pending;
      });

  ntn_beam_placement_plan next_plan = ntn_beam_planner.plan(request);
  if (ntn_cfg.demand_aware_beam_scheduling_enabled) {
    ++nof_ntn_resource_weighting_evaluations;
    unsigned nof_weighted_beams = 0;
    unsigned nof_qos_boosted_beams = 0;
    for (const ntn_beam_du_assignment& assignment : next_plan.assignments) {
      if (is_weighted_resource_assignment(assignment)) {
        ++nof_weighted_beams;
      }
      if (is_qos_boosted_resource_assignment(assignment)) {
        ++nof_qos_boosted_beams;
      }
    }
    nof_ntn_resource_weighting_weighted_beams += nof_weighted_beams;
    nof_ntn_resource_weighting_qos_boosted_beams += nof_qos_boosted_beams;
    last_ntn_resource_weighting_reason = nof_weighted_beams != 0 ? "weighted" : "no_service_demand";
  } else {
    ++nof_ntn_resource_weighting_legacy_fallback;
    last_ntn_resource_weighting_reason = "legacy";
  }

  std::vector<std::string> mobility_eligible_beam_ids =
      get_mobility_eligible_ntn_beam_ids(next_plan, candidates);

  if (!cell_meas_mng.update_ntn_served_beams(mobility_eligible_beam_ids)) {
    return false;
  }
  const bool preheat_applied_this_update = update_ntn_preheat_results(next_plan);

  const bool plan_changed = !are_beam_placement_plans_equal(current_ntn_beam_placement_plan, next_plan);
  current_ntn_served_beam_candidates = candidates;
  current_ntn_mobility_eligible_beam_ids = mobility_eligible_beam_ids;
  if (plan_changed) {
    std::set<std::string> previous_active_analog_ids;
    for (const ntn_analog_access_du_assignment& assignment : current_ntn_beam_placement_plan.analog_assignments) {
      if (assignment.selected_access_du_index != du_index_t::invalid && assignment.reason == "eligible") {
        previous_active_analog_ids.insert(assignment.analog_beam_id);
      }
    }
    std::set<std::string> next_active_analog_ids;
    for (const ntn_analog_access_du_assignment& assignment : next_plan.analog_assignments) {
      if (assignment.selected_access_du_index != du_index_t::invalid && assignment.reason == "eligible") {
        next_active_analog_ids.insert(assignment.analog_beam_id);
      }
    }
    for (const std::string& analog_beam_id : previous_active_analog_ids) {
      if (next_active_analog_ids.count(analog_beam_id) == 0) {
        ntn_service_resource_mng.expire_rnti_leases_for_analog_beam(analog_beam_id, "analog_window_closed");
      }
    }

    current_ntn_core_reportable_ncis = get_ntn_core_reportable_ncis(next_plan);
    current_ntn_beam_placement_plan = std::move(next_plan);
    mobility_mng.handle_ntn_beam_placement_plan_updated(current_ntn_beam_placement_plan);
    schedule_ntn_rnti_lease_pool_updates_for_access_beams();
    if (!has_ntn_service_binding_pending) {
      schedule_ntn_ul_slot_updates_for_online_ues();
    }
    logger.debug("Updated NTN beam placement plan [{}]", format_beam_placement_plan(current_ntn_beam_placement_plan));

    if (mobility_eligible_beam_ids.size() != candidates.size()) {
      logger.debug("NTN beam placement kept {}/{} visible beam candidates outside the mobility eligible set",
                   candidates.size() - mobility_eligible_beam_ids.size(),
                   candidates.size());
    }
  }
  schedule_ntn_sib19_broadcast_updates();
  refresh_ntn_location_freshness_watchdog("served_beam_update");
  if (!preheat_applied_this_update) {
    schedule_ntn_multi_beam_load_balancing_handovers();
  }
  return true;
}

std::optional<std::string> cu_cp_impl::find_ntn_beam_id_by_nci(nr_cell_identity nci) const
{
  const auto& ntn_cfg = cfg.mobility.meas_manager_config.ntn_location_mobility;
  const auto  it      = std::find_if(ntn_cfg.beams.begin(), ntn_cfg.beams.end(), [nci](const ntn_beam_position& beam) {
    return beam.nci.value() == nci.value();
  });
  if (it == ntn_cfg.beams.end()) {
    return std::nullopt;
  }
  return it->beam_id;
}

std::vector<std::pair<std::string, nr_cell_identity>> cu_cp_impl::get_configured_ntn_beam_cells() const
{
  std::vector<std::pair<std::string, nr_cell_identity>> result;
  const auto& ntn_cfg = cfg.mobility.meas_manager_config.ntn_location_mobility;
  result.reserve(ntn_cfg.beams.size());
  for (const auto& beam : ntn_cfg.beams) {
    result.emplace_back(beam.beam_id, beam.nci);
  }
  return result;
}

bool cu_cp_impl::refresh_ntn_beam_placement_for_service_policy()
{
  const auto& ntn_cfg = cfg.mobility.meas_manager_config.ntn_location_mobility;
  std::vector<ntn_served_beam_candidate> filtered_candidates;
  filtered_candidates.reserve(current_ntn_served_beam_candidates.size());
  for (const auto& candidate : current_ntn_served_beam_candidates) {
    const auto beam_it = std::find_if(
        ntn_cfg.beams.begin(), ntn_cfg.beams.end(), [&candidate](const ntn_beam_position& beam) {
          return beam.beam_id == candidate.beam_id;
        });
    const bool force_drain = beam_it != ntn_cfg.beams.end()
                                 ? ntn_service_switch_over_ctrl.forces_drain_for_beam(candidate.beam_id, beam_it->nci)
                                 : ntn_service_switch_over_ctrl.forces_drain_for_beam(candidate.beam_id);
    if (!force_drain) {
      filtered_candidates.push_back(candidate);
    }
  }
  return update_ntn_served_beam_candidates(filtered_candidates);
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

  persist_ntn_idle_paging_context_for_ue(ue_index, "ue_removal");

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
  ntn_rrc_location_request_states.erase(ue_index);
  ntn_location_freshness_states.erase(ue_index);
  ntn_connected_ue_five_g_s_tmsi.erase(ue_index);
  ntn_service_resource_mng.remove_ue(ue_index);
  ntn_pre_service_relocation_states.erase(ue_index);
  clear_ntn_connected_handover_state_for_ue_removal(ue_index, "source_ue_removed");
  ntn_ue_layer_states.erase(ue_index);
  ntn_release_allowed_release_requested_ues.erase(ue_index);
  ntn_location_watchdog_release_requested_ues.erase(ue_index);
  ntn_inactive_contexts.erase(ue_index);

  nrppa_ue_context_removal_handler* nrppa_removal_handler = nullptr;
  nrppa_removal_handler                                   = &nrppa_entity->get_nrppa_ue_context_removal_handler();

  async_task<void> removal_task = launch_async<ue_removal_routine>(ue_index,
                                                                   du_db.get_du_processor(du_index).get_rrc_du_handler(),
                                                                   e1ap_removal_handler,
                                                                   du_db.get_du_processor(du_index).get_f1ap_handler(),
                                                                   ngap_removal_handler,
                                                                   nrppa_removal_handler,
                                                                   ue_mng,
                                                                   logger);

  return launch_async([this, removal_task = std::move(removal_task)](coro_context<async_task<void>>& ctx) mutable {
    CORO_BEGIN(ctx);
    CORO_AWAIT(removal_task);
    refresh_ntn_beam_placement_for_current_load();
    CORO_RETURN();
  });
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
  ue_mng.get_rrc_ue_ngap_adapter(ue_index).connect_cu_cp(get_cu_cp_ngap_handler());

  // Connect CU-CP to RRC UE adapter.
  ue_mng.get_rrc_ue_cu_cp_adapter(ue_index).connect_cu_cp(get_cu_cp_rrc_ue_interface(),
                                                          get_cu_cp_ue_removal_handler(),
                                                          controller,
                                                          ue->get_up_resource_manager(),
                                                          get_cu_cp_measurement_handler());
}

void cu_cp_impl::handle_du_connection_established(du_index_t du_index)
{
  if (stopped.load() || !cfg.mobility.onboard_position_plan.du_execution_enabled) {
    return;
  }
  std::lock_guard<std::mutex> lock(ntn_onboard_position_plan_mutex);
  ntn_position_plan_disconnected_dus.erase(du_index);
}

void cu_cp_impl::handle_du_disconnection(du_index_t du_index)
{
  if (stopped.load() || !cfg.mobility.onboard_position_plan.du_execution_enabled) {
    return;
  }

  bool plan_requires_reconciliation = false;
  bool operation_was_invalidated    = false;
  {
    std::lock_guard<std::mutex> lock(ntn_onboard_position_plan_mutex);
    ++ntn_position_plan_du_connection_generations[du_index];
    ntn_position_plan_disconnected_dus.insert(du_index);

    if (ntn_position_plan_query_du_index == du_index) {
      ++ntn_position_plan_query_request_id;
      ntn_position_plan_query_in_flight = false;
      ntn_position_plan_query_du_index.reset();
      operation_was_invalidated = true;
    }
    if (ntn_position_plan_clear_du_index == du_index) {
      ++ntn_position_plan_clear_request_id;
      ntn_position_plan_clear_in_flight = false;
      ntn_position_plan_clear_du_index.reset();
      operation_was_invalidated = true;
    }

    if (!ntn_onboard_position_plan_ctrl.has_value()) {
      return;
    }
    const auto plan_uses_disconnected_du = [this, du_index](const ntn_activated_position_plan& plan) {
      return du_serves_ntn_onboard_cells(du_db, du_index, plan.cell_positions);
    };
    const bool active_uses_du = ntn_onboard_position_plan_ctrl->active_plan().has_value() &&
                                ntn_onboard_position_plan_ctrl->active_has_external_apply_evidence() &&
                                plan_uses_disconnected_du(*ntn_onboard_position_plan_ctrl->active_plan());
    const bool recovery_uses_du = ntn_onboard_position_plan_ctrl->recovery_plan().has_value() &&
                                  plan_uses_disconnected_du(*ntn_onboard_position_plan_ctrl->recovery_plan());
    const auto deployment = ntn_onboard_position_plan_ctrl->deployment_stage();
    const bool pending_may_have_reached_du =
        ntn_onboard_position_plan_ctrl->pending_plan().has_value() &&
        (deployment == ntn_position_plan_deployment_stage::preparing ||
         deployment == ntn_position_plan_deployment_stage::ready ||
         deployment == ntn_position_plan_deployment_stage::applied) &&
        plan_uses_disconnected_du(*ntn_onboard_position_plan_ctrl->pending_plan());

    if (active_uses_du || recovery_uses_du || pending_may_have_reached_du) {
      plan_requires_reconciliation =
          ntn_onboard_position_plan_ctrl->require_du_reconciliation_after_connection_loss(
              "du_connection_lost_awaiting_matching_query");
      ntn_position_plan_prepare_dispatched.reset();
      ntn_position_plan_static_preflight_schedule_version = 0;
      ntn_position_plan_static_preflight_reports          = {};
      if (plan_requires_reconciliation) {
        persist_ntn_onboard_position_plan_state_locked("du_connection_lost");
      }
    }
  }

  if (plan_requires_reconciliation) {
    logger.warning("DU {} disconnected; hidden NTN calendar application evidence until a matching live-DU query "
                   "succeeds",
                   du_index);
  }
  if (plan_requires_reconciliation || operation_was_invalidated) {
    schedule_ntn_onboard_position_plan_activation();
  }
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

void cu_cp_impl::handle_rrc_initial_ue_message(const cu_cp_initial_ue_message& msg)
{
  if (msg.five_g_s_tmsi.has_value()) {
    ntn_connected_ue_five_g_s_tmsi[msg.ue_index] = msg.five_g_s_tmsi.value();
    if (auto layer_it = ntn_ue_layer_states.find(msg.ue_index); layer_it != ntn_ue_layer_states.end()) {
      const bool already_paired = !layer_it->second.paired_uplink_access_beam_id.empty();
      if (apply_ntn_paired_access_context_for_ue(msg.ue_index, layer_it->second)) {
        layer_it->second.access_reason = "paired_uplink_response";
        if (!already_paired) {
          ++nof_ntn_paired_access_responses;
        }
        ntn_pending_paired_access_contexts.erase(msg.five_g_s_tmsi->to_number());
      } else if (ntn_pending_paired_access_contexts.count(msg.five_g_s_tmsi->to_number()) != 0) {
        ++nof_ntn_paired_access_blocked;
      }
    }
  }
}

void cu_cp_impl::handle_paging_message(cu_cp_paging_message& msg)
{
  apply_ntn_idle_paging_recommendation(msg);
  paging_handler.handle_paging_message(msg);
}

bool cu_cp_impl::schedule_ue_task(ue_index_t ue_index, async_task<void> task)
{
  if (ue_mng.find_ue_task_scheduler(ue_index) == nullptr) {
    logger.debug("UE task scheduler not found for UE index={}", ue_index);
    return false;
  }

  return ue_mng.find_ue_task_scheduler(ue_index)->schedule_async_task(std::move(task));
}

void cu_cp_impl::restore_ntn_onboard_position_plan_state()
{
  const auto& source_cfg = cfg.mobility.onboard_position_plan;
  if (!source_cfg.du_execution_enabled) {
    ntn_position_plan_state_store_status = "disabled";
    return;
  }

  ntn_position_plan_state_store_status = "loading";
  auto                        loaded   = load_ntn_onboard_position_plan_state(source_cfg.state_file);
  std::lock_guard<std::mutex> lock(ntn_onboard_position_plan_mutex);
  if (!ntn_onboard_position_plan_ctrl.has_value()) {
    return;
  }
  ntn_position_plan_state_schema_version = 0;
  if (!loaded.has_value()) {
    ntn_position_plan_state_store_status  = "corrupt";
    ntn_position_plan_state_error         = loaded.error();
    ntn_position_plan_state_write_blocked = true;
    ntn_onboard_position_plan_ctrl->record_external_rejection(ntn_position_plan_reject_reason::state_file_corrupt);
    logger.error(
        "Rejected NTN onboard position-plan state file='{}'. Cause: {}", source_cfg.state_file, loaded.error());
    return;
  }
  if (!loaded->has_value()) {
    ntn_position_plan_state_store_status = "first_boot";
    ntn_position_plan_state_error.clear();
    return;
  }

  ntn_position_plan_state_schema_version = (*loaded)->schema_version;

  const auto recovery_time = std::chrono::system_clock::now();
  auto       restored      = ntn_onboard_position_plan_ctrl->restore_persistent_state(**loaded, recovery_time);
  if (!restored.has_value()) {
    ntn_position_plan_state_store_status  = "context_mismatch";
    ntn_position_plan_state_error         = restored.error();
    ntn_position_plan_state_write_blocked = true;
    ntn_onboard_position_plan_ctrl->record_external_rejection(ntn_position_plan_reject_reason::state_context_mismatch,
                                                              (*loaded)->highest_schedule_version);
    logger.error("Rejected NTN onboard position-plan recovery state file='{}'. Cause: {}",
                 source_cfg.state_file,
                 restored.error());
    return;
  }
  ntn_position_plan_clear_queue.clear();
  for (const ntn_onboard_position_plan_clear_obligation& obligation : (*loaded)->outstanding_clears) {
    ntn_activated_position_plan clear_plan;
    clear_plan.source         = obligation.snapshot.source;
    clear_plan.cell_positions = obligation.snapshot.cell_positions;
    clear_plan.calendar_hash  = obligation.snapshot.calendar_hash;
    ntn_position_plan_clear_queue.emplace_back(std::move(clear_plan), obligation.reason);
  }
  bool recovered_expired_clear = false;
  const auto recover_expired_deployment = [&](const std::optional<ntn_onboard_position_plan_state_snapshot>& snapshot,
                                              bool active_snapshot_was_installed) {
    if (!snapshot.has_value() || recovery_time < snapshot->source.valid_until) {
      return;
    }
    if (!active_snapshot_was_installed &&
        (!ntn_deployment_stage_may_have_installed_calendar((*loaded)->recorded_deployment_stage) ||
         !ntn_snapshot_matches_recorded_deployment(*snapshot, **loaded))) {
      return;
    }
    ntn_activated_position_plan clear_plan;
    clear_plan.source         = snapshot->source;
    clear_plan.cell_positions = snapshot->cell_positions;
    clear_plan.calendar_hash  = snapshot->calendar_hash;
    recovered_expired_clear =
        queue_ntn_onboard_position_plan_clear_locked(clear_plan, "expired_deployment_recovered_after_restart") ||
        recovered_expired_clear;
  };
  // An active snapshot reached active only after matching applied feedback. A later not_sent pending plan may own the
  // global recorded deployment fields, but it cannot erase the cleanup obligation of that previously installed active
  // calendar. Pending snapshots still need an explicit historical DU claim before they are cleared.
  recover_expired_deployment((*loaded)->active, true);
  recover_expired_deployment((*loaded)->pending, false);
  ntn_position_plan_state_generation   = (*loaded)->generation;
  ntn_position_plan_state_hash         = (*loaded)->state_hash;
  ntn_position_plan_state_store_status = "loaded_reconciliation_required";
  ntn_position_plan_state_error.clear();
  const ntn_versioned_position_plan* restored_source = nullptr;
  if ((*loaded)->pending.has_value()) {
    restored_source = &(*loaded)->pending->source;
  } else if ((*loaded)->active.has_value()) {
    restored_source = &(*loaded)->active->source;
  }
  if (restored_source != nullptr) {
    last_ntn_position_plan_file_signature = fmt::format("{}|{}|{}",
                                                        restored_source->schedule_version,
                                                        restored_source->content_hash,
                                                        compute_ntn_position_plan_content_hash(*restored_source));
  }
  if (recovered_expired_clear) {
    persist_ntn_onboard_position_plan_state_locked("expired_deployment_recovered_after_restart");
  }
  logger.info("Loaded NTN onboard position-plan recovery state generation={} high_water_schedule={} "
              "recovery={} outstanding_clears={} (historical applied state is not live evidence)",
              ntn_position_plan_state_generation,
              (*loaded)->highest_schedule_version,
              to_string(ntn_onboard_position_plan_ctrl->recovery_stage()),
              ntn_position_plan_clear_queue.size());
}

cu_cp_impl::ntn_state_persist_outcome
cu_cp_impl::persist_ntn_onboard_position_plan_state_locked(const char* reason, bool omit_clear_queue_head)
{
  const auto& source_cfg = cfg.mobility.onboard_position_plan;
  if (!source_cfg.du_execution_enabled) {
    return ntn_state_persist_outcome::durable;
  }
  if (!ntn_onboard_position_plan_ctrl.has_value() || ntn_position_plan_state_write_blocked) {
    return ntn_state_persist_outcome::not_committed;
  }

  if (ntn_position_plan_state_generation == std::numeric_limits<uint64_t>::max()) {
    ntn_position_plan_state_store_status  = "generation_exhausted";
    ntn_position_plan_state_error         = "state_generation_exhausted";
    ntn_position_plan_state_write_blocked = true;
    return ntn_state_persist_outcome::not_committed;
  }
  const uint64_t next_generation = ntn_position_plan_state_generation + 1;
  auto           state           = ntn_onboard_position_plan_ctrl->make_persistent_state(next_generation);
  const size_t first_clear = omit_clear_queue_head && !ntn_position_plan_clear_queue.empty() ? 1U : 0U;
  state.outstanding_clears.reserve(ntn_position_plan_clear_queue.size() - first_clear);
  for (size_t clear_index = first_clear; clear_index != ntn_position_plan_clear_queue.size(); ++clear_index) {
    const auto& [plan, clear_reason] = ntn_position_plan_clear_queue[clear_index];
    state.outstanding_clears.push_back(
        {{plan.source, plan.cell_positions, plan.calendar_hash}, clear_reason});
  }
  auto stored = store_ntn_onboard_position_plan_state_atomic(source_cfg.state_file, state);
  if (!stored.has_value()) {
    ntn_position_plan_state_store_status  = "write_failed";
    ntn_position_plan_state_error         = stored.error();
    ntn_position_plan_state_write_blocked = true;
    logger.error("Failed to persist NTN onboard position-plan state reason={} file='{}'. Cause: {}",
                 reason,
                 source_cfg.state_file,
                 stored.error());
    return ntn_state_persist_outcome::not_committed;
  }
  ntn_position_plan_state_schema_version = state.schema_version;
  ntn_position_plan_state_generation   = next_generation;
  ntn_position_plan_state_hash         = stored->state_hash;
  ntn_position_plan_state_last_save_unix_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count();
  if (!stored->durable) {
    ntn_position_plan_state_store_status  = "committed_not_durable";
    ntn_position_plan_state_error         = stored->durability_error;
    ntn_position_plan_state_write_blocked = true;
    logger.error("Committed NTN onboard position-plan state without confirmed directory durability reason={} "
                 "file='{}'. Cause: {}",
                 reason,
                 source_cfg.state_file,
                 stored->durability_error);
    return ntn_state_persist_outcome::committed_not_durable;
  }
  ntn_position_plan_state_store_status = "stored";
  ntn_position_plan_state_error.clear();
  return ntn_state_persist_outcome::durable;
}

void cu_cp_impl::reload_ntn_onboard_position_plan()
{
  {
    std::lock_guard<std::mutex> lock(ntn_onboard_position_plan_mutex);
    if (!ntn_onboard_position_plan_ctrl.has_value() || ntn_position_plan_state_write_blocked ||
        ntn_onboard_position_plan_ctrl->recovery_plan().has_value() || !ntn_position_plan_clear_queue.empty()) {
      return;
    }
  }
  const auto& source_cfg = cfg.mobility.onboard_position_plan;
  auto plan = load_ntn_position_plan_json_file(source_cfg.plan_json_file);
  if (!plan.has_value()) {
    {
      std::lock_guard<std::mutex> lock(ntn_onboard_position_plan_mutex);
      if (ntn_onboard_position_plan_ctrl.has_value()) {
        ntn_onboard_position_plan_ctrl->record_external_rejection(ntn_position_plan_reject_reason::parse_error);
      }
    }
    logger.warning("Rejected NTN onboard position plan file='{}'. Cause: {}",
                   source_cfg.plan_json_file,
                   plan.error());
    return;
  }

  const std::string signature = fmt::format("{}|{}|{}",
                                            plan->schedule_version,
                                            plan->content_hash,
                                            compute_ntn_position_plan_content_hash(plan.value()));
  ntn_position_plan_submit_result result;
  std::optional<ntn_activated_position_plan> superseded_pending;
  {
    std::lock_guard<std::mutex> lock(ntn_onboard_position_plan_mutex);
    if (!ntn_onboard_position_plan_ctrl.has_value() || signature == last_ntn_position_plan_file_signature) {
      return;
    }
    const ntn_onboard_position_plan_controller checkpoint       = *ntn_onboard_position_plan_ctrl;
    const auto                                  clear_checkpoint = ntn_position_plan_clear_queue;
    if (source_cfg.du_execution_enabled && ntn_onboard_position_plan_ctrl->pending_plan().has_value() &&
        ntn_onboard_position_plan_ctrl->pending_plan()->source.schedule_version != plan->schedule_version &&
        (ntn_onboard_position_plan_ctrl->deployment_stage() == ntn_position_plan_deployment_stage::preparing ||
         ntn_onboard_position_plan_ctrl->deployment_stage() == ntn_position_plan_deployment_stage::ready ||
         ntn_onboard_position_plan_ctrl->deployment_stage() == ntn_position_plan_deployment_stage::applied)) {
      superseded_pending = *ntn_onboard_position_plan_ctrl->pending_plan();
    }
    result = ntn_onboard_position_plan_ctrl->submit(plan.value(), std::chrono::system_clock::now());
    if (result.accepted && superseded_pending.has_value()) {
      queue_ntn_onboard_position_plan_clear_locked(*superseded_pending, "superseded_by_new_checked_plan");
    }
    const ntn_state_persist_outcome persist_outcome =
        source_cfg.du_execution_enabled
            ? persist_ntn_onboard_position_plan_state_locked(result.accepted ? "checked_plan_accepted"
                                                                              : "received_plan_rejected")
            : ntn_state_persist_outcome::durable;
    if (result.accepted && persist_outcome != ntn_state_persist_outcome::durable) {
      if (persist_outcome == ntn_state_persist_outcome::not_committed) {
        *ntn_onboard_position_plan_ctrl = checkpoint;
        ntn_position_plan_clear_queue   = clear_checkpoint;
      }
      ntn_onboard_position_plan_ctrl->record_external_rejection(
          ntn_position_plan_reject_reason::state_persistence_failure, plan->schedule_version);
      result = {false, ntn_position_plan_stage::rejected, ntn_position_plan_reject_reason::state_persistence_failure};
      superseded_pending.reset();
    } else {
      last_ntn_position_plan_file_signature = signature;
    }
  }
  if (!result.accepted) {
    logger.warning("Rejected NTN onboard position plan satellite_id={} catalog_version={} schedule_version={} reason={}",
                   plan->satellite_id,
                   plan->catalog_version,
                   plan->schedule_version,
                   to_string(result.reason));
    return;
  }
  logger.info("Accepted NTN onboard position plan satellite_id={} catalog_version={} schedule_version={} state={} "
              "candidate_l1={}",
              plan->satellite_id,
              plan->catalog_version,
              plan->schedule_version,
              to_string(result.stage),
              plan->visible_l1_positions.size());
  if (superseded_pending.has_value()) {
    try_clear_ntn_onboard_position_plan_deployment();
  }
  if (source_cfg.du_execution_enabled) {
    try_prepare_ntn_onboard_position_plan();
  }
  schedule_ntn_onboard_position_plan_activation();
}

void cu_cp_impl::try_prepare_ntn_onboard_position_plan()
{
  if (!cfg.mobility.onboard_position_plan.du_execution_enabled || stopped.load()) {
    return;
  }

  std::optional<ntn_activated_position_plan> pending_plan;
  std::set<du_index_t>                       disconnected_du_indexes;
  {
    std::lock_guard<std::mutex> lock(ntn_onboard_position_plan_mutex);
    if (!ntn_onboard_position_plan_ctrl.has_value() || ntn_position_plan_state_write_blocked ||
        ntn_onboard_position_plan_ctrl->deployment_stage() != ntn_position_plan_deployment_stage::not_sent ||
        !ntn_onboard_position_plan_ctrl->pending_plan().has_value() || !ntn_position_plan_clear_queue.empty()) {
      return;
    }
    pending_plan = *ntn_onboard_position_plan_ctrl->pending_plan();
    disconnected_du_indexes = ntn_position_plan_disconnected_dus;
  }

  const auto now = std::chrono::system_clock::now();
  const auto prepare_window_open =
      pending_plan->source.activation_epoch - cfg.mobility.onboard_position_plan.du_prepare_horizon;
  if (now < prepare_window_open) {
    return;
  }
  const auto prepare_deadline =
      pending_plan->source.activation_epoch - cfg.mobility.onboard_position_plan.du_prepare_guard;
  if (now >= prepare_deadline) {
    ntn_state_persist_outcome persist_outcome = ntn_state_persist_outcome::durable;
    {
      std::lock_guard<std::mutex> lock(ntn_onboard_position_plan_mutex);
      if (ntn_onboard_position_plan_ctrl.has_value()) {
        const bool rejected = ntn_onboard_position_plan_ctrl->reject_pending_deployment(
            pending_plan->source.schedule_version,
            pending_plan->calendar_hash,
            ntn_position_plan_reject_reason::du_prepare_timeout,
            "du_prepare_guard_reached_before_du_ready");
        if (rejected) {
          queue_ntn_onboard_position_plan_clear_locked(*pending_plan, "du_prepare_guard_reached_before_du_ready");
          persist_outcome = persist_ntn_onboard_position_plan_state_locked("du_prepare_guard_rejection");
        }
      }
    }
    if (persist_outcome == ntn_state_persist_outcome::durable) {
      try_clear_ntn_onboard_position_plan_deployment();
    }
    schedule_ntn_onboard_position_plan_activation();
    return;
  }

  const ntn_onboard_du_cell_resolution resolution =
      resolve_ntn_onboard_du_cells(du_db, pending_plan->cell_positions, disconnected_du_indexes);
  if (resolution.status == ntn_onboard_du_cell_resolution_status::unavailable ||
      resolution.status == ntn_onboard_du_cell_resolution_status::missing ||
      resolution.status == ntn_onboard_du_cell_resolution_status::duplicate) {
    const bool duplicate = resolution.status == ntn_onboard_du_cell_resolution_status::duplicate;
    std::lock_guard<std::mutex> lock(ntn_onboard_position_plan_mutex);
    if (ntn_onboard_position_plan_ctrl.has_value()) {
      if (duplicate) {
        ntn_onboard_position_plan_ctrl->reject_pending_deployment(
            pending_plan->source.schedule_version,
            pending_plan->calendar_hash,
            ntn_position_plan_reject_reason::identity_mismatch,
            get_ntn_onboard_du_cell_resolution_detail(resolution.status));
      } else {
        const bool detail_changed = ntn_onboard_position_plan_ctrl->deployment_detail() !=
                                    get_ntn_onboard_du_cell_resolution_detail(resolution.status);
        ntn_onboard_position_plan_ctrl->mark_deployment_retryable(
            pending_plan->source.schedule_version,
            pending_plan->calendar_hash,
            get_ntn_onboard_du_cell_resolution_detail(resolution.status));
        if (!detail_changed) {
          return;
        }
      }
      persist_ntn_onboard_position_plan_state_locked(duplicate ? "du_identity_rejection" : "du_unavailable_retry");
    }
    return;
  }
  if (resolution.status == ntn_onboard_du_cell_resolution_status::cross_du) {
    std::lock_guard<std::mutex> lock(ntn_onboard_position_plan_mutex);
    if (ntn_onboard_position_plan_ctrl.has_value()) {
      ntn_onboard_position_plan_ctrl->reject_pending_deployment(
          pending_plan->source.schedule_version,
          pending_plan->calendar_hash,
          ntn_position_plan_reject_reason::cross_du_calendar_not_supported,
          get_ntn_onboard_du_cell_resolution_detail(resolution.status));
      persist_ntn_onboard_position_plan_state_locked("cross_du_rejection");
    }
    return;
  }

  du_processor* processor = du_db.find_du_processor(resolution.du_index);
  if (processor == nullptr || processor->get_context() == nullptr) {
    return;
  }

  f1ap_ntn_access_calendar_update update;
  update.operation            = f1ap_ntn_access_calendar_operation::prepare;
  update.satellite_id         = pending_plan->source.satellite_id;
  update.catalog_version      = pending_plan->source.catalog_version;
  update.schedule_version     = pending_plan->source.schedule_version;
  update.source_content_hash  = pending_plan->source.content_hash;
  update.calendar_hash        = pending_plan->calendar_hash;
  update.activation_epoch_unix_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                              pending_plan->source.activation_epoch.time_since_epoch())
                                                              .count());
  update.valid_until_unix_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                         pending_plan->source.valid_until.time_since_epoch())
                                                         .count());
  update.cycle_duration_us =
      static_cast<uint32_t>(cfg.mobility.onboard_position_plan.max_prach_interval.count());
  update.cells.reserve(2);
  for (unsigned i = 0; i != pending_plan->cell_positions.size(); ++i) {
    f1ap_ntn_access_calendar_cell cell;
    cell.du_cell_index = to_f1ap_du_cell_index(resolution.cells[i]->cell_index);
    cell.nci           = pending_plan->cell_positions[i].identity.nci;
    cell.pci           = pending_plan->cell_positions[i].identity.pci;
    for (const ntn_access_calendar_intent& intent : pending_plan->access_calendar) {
      if (intent.nci != cell.nci) {
        continue;
      }
      f1ap_ntn_access_calendar_intent wire_intent;
      wire_intent.position_id  = intent.position_id;
      wire_intent.start_time_us = static_cast<uint32_t>(intent.start_time.count());
      wire_intent.duration_us   = static_cast<uint32_t>(intent.duration.count());
      wire_intent.direction = intent.direction == ntn_access_calendar_direction::downlink
                                  ? f1ap_ntn_access_calendar_direction::downlink
                                  : f1ap_ntn_access_calendar_direction::uplink;
      switch (intent.purpose) {
        case ntn_access_calendar_purpose::ssb_sib_paging:
          wire_intent.purpose = f1ap_ntn_access_calendar_purpose::ssb_sib_paging;
          break;
        case ntn_access_calendar_purpose::ssb_sib_paging_rar:
          wire_intent.purpose = f1ap_ntn_access_calendar_purpose::ssb_sib_paging_rar;
          break;
        case ntn_access_calendar_purpose::prach_ro:
          wire_intent.purpose = f1ap_ntn_access_calendar_purpose::prach_ro;
          break;
        case ntn_access_calendar_purpose::prach_ul_beam:
          wire_intent.purpose = f1ap_ntn_access_calendar_purpose::prach_ul_beam;
          break;
      }
      wire_intent.port_id = intent.port_id;
      cell.intents.push_back(std::move(wire_intent));
    }
    update.cells.push_back(std::move(cell));
  }

  uint64_t du_connection_generation = 0;
  {
    std::lock_guard<std::mutex> lock(ntn_onboard_position_plan_mutex);
    if (ntn_position_plan_disconnected_dus.count(resolution.du_index) != 0 ||
        !ntn_onboard_position_plan_ctrl.has_value() ||
        !ntn_onboard_position_plan_ctrl->mark_deployment_preparing(update.schedule_version, update.calendar_hash)) {
      return;
    }
    if (persist_ntn_onboard_position_plan_state_locked("du_prepare_dispatched") !=
        ntn_state_persist_outcome::durable) {
      return;
    }
    du_connection_generation = ntn_position_plan_du_connection_generations[resolution.du_index];
  }

  f1ap_gnb_du_resource_coordination_request request;
  request.ntn_access_calendar_update = update;
  async_task<f1ap_gnb_du_resource_coordination_response> f1ap_task =
      processor->get_f1ap_handler().handle_gnb_du_resource_coordination_request(request);
  auto completion_task =
      [this,
       prepared_plan = pending_plan.value(),
       update,
       du_index = resolution.du_index,
       du_connection_generation,
       f1ap_task = std::move(f1ap_task)](
          coro_context<async_task<void>>& ctx) mutable {
        f1ap_gnb_du_resource_coordination_response response;
        bool should_clear_orphan = false;
        bool may_start_f1_transaction = false;
        CORO_BEGIN(ctx);
        {
          std::lock_guard<std::mutex> lock(ntn_onboard_position_plan_mutex);
          may_start_f1_transaction =
              ntn_position_plan_du_connection_generations[du_index] == du_connection_generation &&
              ntn_onboard_position_plan_ctrl.has_value() &&
              ntn_onboard_position_plan_ctrl->pending_plan().has_value() &&
              ntn_onboard_position_plan_ctrl->pending_plan()->source.schedule_version == update.schedule_version &&
              ntn_onboard_position_plan_ctrl->pending_plan()->calendar_hash == update.calendar_hash &&
              ntn_onboard_position_plan_ctrl->deployment_stage() == ntn_position_plan_deployment_stage::preparing &&
              std::chrono::system_clock::now() <
                  prepared_plan.source.activation_epoch - cfg.mobility.onboard_position_plan.du_prepare_guard;
          if (may_start_f1_transaction) {
            ntn_position_plan_prepare_dispatched = std::make_pair(update.schedule_version, update.calendar_hash);
          }
        }
        if (!may_start_f1_transaction) {
          CORO_EARLY_RETURN();
        }
        CORO_AWAIT_VALUE(response, f1ap_task);
        {
          std::lock_guard<std::mutex> lock(ntn_onboard_position_plan_mutex);
          if (ntn_position_plan_du_connection_generations[du_index] != du_connection_generation) {
            logger.debug("Ignored stale NTN calendar prepare response schedule_version={} from disconnected DU {}",
                         update.schedule_version,
                         du_index);
          } else if (ntn_onboard_position_plan_ctrl.has_value()) {
            const bool pending_matches =
                ntn_onboard_position_plan_ctrl->pending_plan().has_value() &&
                ntn_onboard_position_plan_ctrl->pending_plan()->source.schedule_version == update.schedule_version &&
                ntn_onboard_position_plan_ctrl->pending_plan()->calendar_hash == update.calendar_hash;
            const bool active_matches =
                ntn_onboard_position_plan_ctrl->active_plan().has_value() &&
                ntn_onboard_position_plan_ctrl->active_plan()->source.schedule_version == update.schedule_version &&
                ntn_onboard_position_plan_ctrl->active_plan()->calendar_hash == update.calendar_hash;
            const auto deployment                  = ntn_onboard_position_plan_ctrl->deployment_stage();
            const bool query_already_advanced_plan = deployment == ntn_position_plan_deployment_stage::ready ||
                                                     deployment == ntn_position_plan_deployment_stage::applied;
            const bool response_accepts_calendar =
                response.calendar_result.has_value() &&
                (response.calendar_result->status == f1ap_ntn_access_calendar_result_status::preparing ||
                 response.calendar_result->status == f1ap_ntn_access_calendar_result_status::ready ||
                 response.calendar_result->status == f1ap_ntn_access_calendar_result_status::applied);
            const bool response_carries_preflight =
                response.calendar_result.has_value() &&
                carries_ntn_static_opportunity_feedback(*response.calendar_result);
            const ntn_static_opportunity_validation preflight =
                response_carries_preflight
                    ? validate_ntn_static_opportunity_preflight(*response.calendar_result, update)
                    : ntn_static_opportunity_validation{};
            const bool response_plan_matches =
                response.calendar_result.has_value() &&
                response.calendar_result->catalog_version == update.catalog_version &&
                response.calendar_result->schedule_version == update.schedule_version &&
                response.calendar_result->source_content_hash == update.source_content_hash &&
                response.calendar_result->calendar_hash == update.calendar_hash;
            if (response_plan_matches && (pending_matches || active_matches) &&
                update.schedule_version >= ntn_position_plan_static_preflight_schedule_version) {
              ntn_position_plan_static_preflight_schedule_version = update.schedule_version;
              ntn_position_plan_static_preflight_reports          = response.calendar_result->preflight_reports;
            }
            const bool response_after_prepare_deadline =
                std::chrono::system_clock::now() >=
                prepared_plan.source.activation_epoch - cfg.mobility.onboard_position_plan.du_prepare_guard;
            if (active_matches && !pending_matches) {
              // A duplicate response for the plan that is already active is stale but not an orphan deployment.
            } else if (!pending_matches) {
              should_clear_orphan = true;
            } else if (query_already_advanced_plan) {
              // A query sent after prepare may complete first and provide newer ready/applied state. Ignore the older
              // prepare completion in full, including a timeout/reject, so it cannot roll back the same plan.
            } else if (response_after_prepare_deadline) {
              ntn_onboard_position_plan_ctrl->reject_pending_deployment(
                  update.schedule_version,
                  update.calendar_hash,
                  ntn_position_plan_reject_reason::du_prepare_timeout,
                  "du_prepare_response_arrived_after_guard");
              should_clear_orphan = true;
            } else if (!response.calendar_result.has_value()) {
              should_clear_orphan = !ntn_onboard_position_plan_ctrl->mark_deployment_retryable(
                  update.schedule_version, update.calendar_hash, "f1ap_prepare_no_response_retrying");
            } else if (response.calendar_result->catalog_version != update.catalog_version ||
                       response.calendar_result->schedule_version != update.schedule_version ||
                       response.calendar_result->source_content_hash != update.source_content_hash ||
                       response.calendar_result->calendar_hash != update.calendar_hash) {
              ntn_onboard_position_plan_ctrl->reject_pending_deployment(
                  update.schedule_version,
                  update.calendar_hash,
                  ntn_position_plan_reject_reason::calendar_hash_mismatch,
                  "du_prepare_response_version_or_hash_mismatch");
              should_clear_orphan = true;
            } else if (response.calendar_result->status == f1ap_ntn_access_calendar_result_status::unsupported) {
              // A preflight capability failure carries an empty report by definition. Preserve its machine-readable
              // unsupported meaning instead of classifying the absent report as a static-opportunity mismatch. The
              // prepare path has not installed a gate, so there is nothing to clear.
              ntn_onboard_position_plan_ctrl->reject_pending_deployment(
                  update.schedule_version,
                  update.calendar_hash,
                  ntn_position_plan_reject_reason::execution_unsupported,
                  response.calendar_result->reject_reason.empty() ? "scheduler_preflight_unsupported"
                                                                  : response.calendar_result->reject_reason);
            } else if (response_carries_preflight && !preflight.accepted) {
              ntn_onboard_position_plan_ctrl->reject_pending_deployment(
                  update.schedule_version,
                  update.calendar_hash,
                  ntn_position_plan_reject_reason::static_opportunity_mismatch,
                  preflight.detail);
              should_clear_orphan = true;
            } else if (response_accepts_calendar &&
                       (static_cast<size_t>(response.calendar_result->accepted_intents_per_cell[0]) !=
                            update.cells[0].intents.size() ||
                        static_cast<size_t>(response.calendar_result->accepted_intents_per_cell[1]) !=
                            update.cells[1].intents.size())) {
              ntn_onboard_position_plan_ctrl->reject_pending_deployment(
                  update.schedule_version,
                  update.calendar_hash,
                  ntn_position_plan_reject_reason::du_prepare_rejected,
                  "du_prepare_accepted_intent_count_mismatch");
              should_clear_orphan = true;
            } else if (response.calendar_result->status == f1ap_ntn_access_calendar_result_status::preparing) {
              // The scheduler command is queued. The activation timer polls until both cell slot threads confirm
              // that the matching version has been consumed and armed before the prepare guard expires.
            } else if (response.calendar_result->status == f1ap_ntn_access_calendar_result_status::ready) {
              should_clear_orphan =
                  !ntn_onboard_position_plan_ctrl->mark_deployment_ready(update.schedule_version, update.calendar_hash);
            } else if (response.calendar_result->status == f1ap_ntn_access_calendar_result_status::applied) {
              if (ntn_onboard_position_plan_ctrl->mark_deployment_applied(update.schedule_version,
                                                                          update.calendar_hash)) {
                ntn_onboard_position_plan_ctrl->advance_time(std::chrono::system_clock::now());
              } else {
                should_clear_orphan = true;
              }
            } else {
              const bool unsupported =
                  response.calendar_result->status == f1ap_ntn_access_calendar_result_status::unsupported;
              ntn_onboard_position_plan_ctrl->reject_pending_deployment(
                  update.schedule_version,
                  update.calendar_hash,
                  unsupported ? ntn_position_plan_reject_reason::execution_unsupported
                              : ntn_position_plan_reject_reason::du_prepare_rejected,
                  response.calendar_result->reject_reason.empty() ? "du_prepare_rejected"
                                                                  : response.calendar_result->reject_reason);
              should_clear_orphan = !unsupported;
            }
            if (ntn_position_plan_prepare_dispatched.has_value() &&
                ntn_position_plan_prepare_dispatched->first == update.schedule_version &&
                ntn_position_plan_prepare_dispatched->second == update.calendar_hash &&
                ntn_onboard_position_plan_ctrl->deployment_stage() !=
                    ntn_position_plan_deployment_stage::preparing) {
              ntn_position_plan_prepare_dispatched.reset();
            }
            if (should_clear_orphan) {
              queue_ntn_onboard_position_plan_clear_locked(prepared_plan, "orphaned_or_late_prepare_response");
            }
            persist_ntn_onboard_position_plan_state_locked("du_prepare_response");
          }
        }
        if (should_clear_orphan) {
          try_clear_ntn_onboard_position_plan_deployment();
        }
        schedule_ntn_onboard_position_plan_activation();
        CORO_RETURN();
      };
  if (!ntn_position_plan_prepare_io_sched.schedule(launch_async(std::move(completion_task)))) {
    std::lock_guard<std::mutex> lock(ntn_onboard_position_plan_mutex);
    if (ntn_onboard_position_plan_ctrl.has_value()) {
      ntn_onboard_position_plan_ctrl->mark_deployment_retryable(
          update.schedule_version, update.calendar_hash, "cu_cp_task_scheduler_unavailable_retrying");
      persist_ntn_onboard_position_plan_state_locked("prepare_task_scheduler_unavailable");
    }
  }
}

void cu_cp_impl::query_ntn_onboard_position_plan_application()
{
  if (!cfg.mobility.onboard_position_plan.du_execution_enabled || stopped.load()) {
    return;
  }

  std::optional<ntn_activated_position_plan> pending_plan;
  bool                                       query_for_prepare = false;
  bool                                       query_for_recovery = false;
  std::set<du_index_t>                       disconnected_du_indexes;
  {
    std::lock_guard<std::mutex> lock(ntn_onboard_position_plan_mutex);
    if (ntn_position_plan_query_in_flight || !ntn_onboard_position_plan_ctrl.has_value() ||
        ntn_position_plan_state_write_blocked ||
        !ntn_position_plan_clear_queue.empty()) {
      return;
    }
    if (ntn_onboard_position_plan_ctrl->recovery_plan().has_value()) {
      pending_plan       = *ntn_onboard_position_plan_ctrl->recovery_plan();
      query_for_recovery = true;
    } else {
      if (!ntn_onboard_position_plan_ctrl->pending_plan().has_value()) {
        return;
      }
      const auto  deployment = ntn_onboard_position_plan_ctrl->deployment_stage();
      const auto& pending    = *ntn_onboard_position_plan_ctrl->pending_plan();
      const bool  prepare_was_dispatched =
          ntn_position_plan_prepare_dispatched.has_value() &&
          ntn_position_plan_prepare_dispatched->first == pending.source.schedule_version &&
          ntn_position_plan_prepare_dispatched->second == pending.calendar_hash;
      if (deployment != ntn_position_plan_deployment_stage::preparing &&
          (deployment != ntn_position_plan_deployment_stage::ready ||
           std::chrono::system_clock::now() <
               ntn_onboard_position_plan_ctrl->pending_plan()->source.activation_epoch)) {
        return;
      }
      if (deployment == ntn_position_plan_deployment_stage::preparing && !prepare_was_dispatched) {
        return;
      }
      query_for_prepare = deployment == ntn_position_plan_deployment_stage::preparing;
      pending_plan      = *ntn_onboard_position_plan_ctrl->pending_plan();
    }
    disconnected_du_indexes = ntn_position_plan_disconnected_dus;
  }

  const ntn_onboard_du_cell_resolution resolution =
      resolve_ntn_onboard_du_cells(du_db, pending_plan->cell_positions, disconnected_du_indexes);
  if (resolution.status != ntn_onboard_du_cell_resolution_status::resolved) {
    if (resolution.status == ntn_onboard_du_cell_resolution_status::unavailable) {
      return;
    }
    if (query_for_recovery) {
      {
        std::lock_guard<std::mutex> lock(ntn_onboard_position_plan_mutex);
        if (ntn_onboard_position_plan_ctrl.has_value()) {
          const bool rejected = ntn_onboard_position_plan_ctrl->reject_recovery(
              pending_plan->source.schedule_version,
              pending_plan->calendar_hash,
              ntn_position_plan_reject_reason::du_reconciliation_failed,
              get_ntn_onboard_du_cell_resolution_detail(resolution.status));
          if (rejected) {
            queue_ntn_onboard_position_plan_clear_locked(*pending_plan, "du_recovery_cell_mapping_rejection");
          }
          persist_ntn_onboard_position_plan_state_locked("du_recovery_cell_mapping_rejection");
        }
      }
      schedule_ntn_onboard_position_plan_activation();
      try_clear_ntn_onboard_position_plan_deployment();
    } else if (resolution.status == ntn_onboard_du_cell_resolution_status::duplicate ||
               resolution.status == ntn_onboard_du_cell_resolution_status::cross_du) {
      {
        std::lock_guard<std::mutex> lock(ntn_onboard_position_plan_mutex);
        if (ntn_onboard_position_plan_ctrl.has_value()) {
          const bool rejected = ntn_onboard_position_plan_ctrl->reject_pending_deployment(
              pending_plan->source.schedule_version,
              pending_plan->calendar_hash,
              resolution.status == ntn_onboard_du_cell_resolution_status::cross_du
                  ? ntn_position_plan_reject_reason::cross_du_calendar_not_supported
                  : ntn_position_plan_reject_reason::identity_mismatch,
              get_ntn_onboard_du_cell_resolution_detail(resolution.status));
          if (rejected) {
            queue_ntn_onboard_position_plan_clear_locked(*pending_plan, "du_query_cell_mapping_rejection");
          }
          persist_ntn_onboard_position_plan_state_locked("du_query_cell_mapping_rejection");
        }
      }
      try_clear_ntn_onboard_position_plan_deployment();
    }
    return;
  }
  du_processor* processor = du_db.find_du_processor(resolution.du_index);
  if (processor == nullptr) {
    return;
  }

  f1ap_ntn_access_calendar_update update;
  update.operation                = f1ap_ntn_access_calendar_operation::query;
  update.satellite_id         = pending_plan->source.satellite_id;
  update.catalog_version      = pending_plan->source.catalog_version;
  update.schedule_version     = pending_plan->source.schedule_version;
  update.source_content_hash  = pending_plan->source.content_hash;
  update.calendar_hash        = pending_plan->calendar_hash;
  update.activation_epoch_unix_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                              pending_plan->source.activation_epoch.time_since_epoch())
                                                              .count());
  update.valid_until_unix_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                         pending_plan->source.valid_until.time_since_epoch())
                                                         .count());
  update.cycle_duration_us =
      static_cast<uint32_t>(cfg.mobility.onboard_position_plan.max_prach_interval.count());

  f1ap_gnb_du_resource_coordination_request request;
  request.ntn_access_calendar_update = update;
  uint64_t du_connection_generation = 0;
  uint64_t query_request_id          = 0;
  {
    std::lock_guard<std::mutex> lock(ntn_onboard_position_plan_mutex);
    if (ntn_position_plan_disconnected_dus.count(resolution.du_index) != 0) {
      return;
    }
    du_connection_generation = ntn_position_plan_du_connection_generations[resolution.du_index];
    query_request_id          = ++ntn_position_plan_query_request_id;
    ntn_position_plan_query_in_flight = true;
    ntn_position_plan_query_du_index  = resolution.du_index;
  }
  async_task<f1ap_gnb_du_resource_coordination_response> f1ap_task =
      processor->get_f1ap_handler().handle_gnb_du_resource_coordination_request(request);
  auto completion_task =
      [this,
       queried_plan = pending_plan.value(),
       query_for_prepare,
       query_for_recovery,
       update,
       du_index = resolution.du_index,
       du_connection_generation,
       query_request_id,
       f1ap_task = std::move(f1ap_task)](coro_context<async_task<void>>& ctx) mutable {
        f1ap_gnb_du_resource_coordination_response response;
        bool should_clear_rejected = false;
        bool clear_was_queued      = false;
        CORO_BEGIN(ctx);
        CORO_AWAIT_VALUE(response, f1ap_task);
        {
          std::lock_guard<std::mutex> lock(ntn_onboard_position_plan_mutex);
          const bool response_from_current_connection =
              query_request_id == ntn_position_plan_query_request_id &&
              ntn_position_plan_query_du_index == du_index &&
              ntn_position_plan_du_connection_generations[du_index] == du_connection_generation;
          if (query_request_id == ntn_position_plan_query_request_id) {
            ntn_position_plan_query_in_flight = false;
            ntn_position_plan_query_du_index.reset();
          }
          const auto response_time = std::chrono::system_clock::now();
          const bool pending_matches =
              ntn_onboard_position_plan_ctrl.has_value() &&
              ntn_onboard_position_plan_ctrl->pending_plan().has_value() &&
              ntn_onboard_position_plan_ctrl->pending_plan()->source.schedule_version == update.schedule_version &&
              ntn_onboard_position_plan_ctrl->pending_plan()->calendar_hash == update.calendar_hash;
          const bool active_matches =
              ntn_onboard_position_plan_ctrl.has_value() &&
              ntn_onboard_position_plan_ctrl->active_plan().has_value() &&
              ntn_onboard_position_plan_ctrl->active_plan()->source.schedule_version == update.schedule_version &&
              ntn_onboard_position_plan_ctrl->active_plan()->calendar_hash == update.calendar_hash;
          const bool recovery_matches =
              ntn_onboard_position_plan_ctrl.has_value() &&
              ntn_onboard_position_plan_ctrl->recovery_plan().has_value() &&
              ntn_onboard_position_plan_ctrl->recovery_plan()->source.schedule_version == update.schedule_version &&
              ntn_onboard_position_plan_ctrl->recovery_plan()->calendar_hash == update.calendar_hash;
          const auto deployment = ntn_onboard_position_plan_ctrl.has_value()
                                      ? ntn_onboard_position_plan_ctrl->deployment_stage()
                                      : ntn_position_plan_deployment_stage::disabled;
          const auto recovery_stage_before = ntn_onboard_position_plan_ctrl.has_value()
                                                 ? ntn_onboard_position_plan_ctrl->recovery_stage()
                                                 : ntn_position_plan_recovery_stage::disabled;
          const bool active_before =
              ntn_onboard_position_plan_ctrl.has_value() && ntn_onboard_position_plan_ctrl->active_plan().has_value();
          const bool pending_before =
              ntn_onboard_position_plan_ctrl.has_value() && ntn_onboard_position_plan_ctrl->pending_plan().has_value();
          const bool recovery_before =
              ntn_onboard_position_plan_ctrl.has_value() && ntn_onboard_position_plan_ctrl->recovery_plan().has_value();
          const uint64_t recovery_version_before =
              ntn_onboard_position_plan_ctrl.has_value()
                  ? ntn_onboard_position_plan_ctrl->recovery_schedule_version()
                  : 0;
          const auto     rejection_before = ntn_onboard_position_plan_ctrl.has_value()
                                                ? ntn_onboard_position_plan_ctrl->last_rejection_reason()
                                                : ntn_position_plan_reject_reason::none;
          const uint64_t rejected_version_before =
              ntn_onboard_position_plan_ctrl.has_value()
                  ? ntn_onboard_position_plan_ctrl->last_rejected_schedule_version()
                  : 0;
          if (!response_from_current_connection) {
            logger.debug("Ignored stale NTN calendar query response schedule_version={} from disconnected DU {}",
                         update.schedule_version,
                         du_index);
          } else if (query_for_recovery && !recovery_matches) {
            // A newer recovery decision won the race; ignore this stale response.
          } else if (!query_for_recovery && active_matches && !pending_matches) {
            // Ignore a duplicate query response for the plan that is already active.
          } else if (!query_for_recovery && ntn_onboard_position_plan_ctrl.has_value() && !pending_matches) {
            should_clear_rejected = true;
          } else if (query_for_prepare && deployment == ntn_position_plan_deployment_stage::applied) {
            // The same plan has already reached stronger applied evidence through a later completion path.
          } else if (ntn_onboard_position_plan_ctrl.has_value() && response_time >= queried_plan.source.valid_until) {
            if (query_for_recovery) {
              ntn_onboard_position_plan_ctrl->reject_recovery(update.schedule_version,
                                                              update.calendar_hash,
                                                              ntn_position_plan_reject_reason::du_reconciliation_failed,
                                                              "du_recovery_query_arrived_after_valid_until");
            } else {
              ntn_onboard_position_plan_ctrl->reject_pending_deployment(
                  update.schedule_version,
                  update.calendar_hash,
                  ntn_position_plan_reject_reason::du_activation_not_applied,
                  "du_query_response_arrived_after_valid_until");
              should_clear_rejected = true;
            }
          } else if (ntn_onboard_position_plan_ctrl.has_value() && !query_for_recovery && !query_for_prepare &&
                     response_time >=
                         queried_plan.source.activation_epoch + cfg.mobility.onboard_position_plan.du_apply_timeout) {
            ntn_onboard_position_plan_ctrl->reject_pending_deployment(
                update.schedule_version,
                update.calendar_hash,
                ntn_position_plan_reject_reason::du_activation_not_applied,
                "du_application_response_arrived_after_deadline");
            should_clear_rejected = true;
          } else if (ntn_onboard_position_plan_ctrl.has_value() && !query_for_recovery && query_for_prepare &&
                     deployment == ntn_position_plan_deployment_stage::preparing &&
                     response_time >=
                         queried_plan.source.activation_epoch - cfg.mobility.onboard_position_plan.du_prepare_guard) {
            ntn_onboard_position_plan_ctrl->reject_pending_deployment(
                update.schedule_version,
                update.calendar_hash,
                ntn_position_plan_reject_reason::du_prepare_timeout,
                "du_armed_response_arrived_after_guard");
            should_clear_rejected = true;
          } else if (ntn_onboard_position_plan_ctrl.has_value() && response.calendar_result.has_value()) {
            std::array<size_t, 2> expected_intents_per_cell{};
            ntn_static_opportunity_expectation_counts expected_static_opportunities{};
            for (const ntn_access_calendar_intent& intent : queried_plan.access_calendar) {
              for (unsigned i = 0; i != queried_plan.cell_positions.size(); ++i) {
                if (intent.nci == queried_plan.cell_positions[i].identity.nci) {
                  ++expected_intents_per_cell[i];
                  if (intent.purpose == ntn_access_calendar_purpose::ssb_sib_paging ||
                      intent.purpose == ntn_access_calendar_purpose::ssb_sib_paging_rar) {
                    ++expected_static_opportunities[i].ssb;
                  } else if (intent.purpose == ntn_access_calendar_purpose::prach_ro) {
                    ++expected_static_opportunities[i].prach;
                  }
                  break;
                }
              }
            }
            const bool response_accepts_calendar =
                response.calendar_result->status == f1ap_ntn_access_calendar_result_status::preparing ||
                response.calendar_result->status == f1ap_ntn_access_calendar_result_status::ready ||
                response.calendar_result->status == f1ap_ntn_access_calendar_result_status::applied;
            const bool response_carries_preflight =
                carries_ntn_static_opportunity_feedback(*response.calendar_result);
            const ntn_static_opportunity_validation preflight =
                response_carries_preflight
                    ? validate_ntn_static_opportunity_preflight(*response.calendar_result,
                                                                expected_static_opportunities)
                    : ntn_static_opportunity_validation{};
            const bool response_plan_matches =
                response.calendar_result->catalog_version == update.catalog_version &&
                response.calendar_result->schedule_version == update.schedule_version &&
                response.calendar_result->source_content_hash == update.source_content_hash &&
                response.calendar_result->calendar_hash == update.calendar_hash;
            if (response_plan_matches && (pending_matches || active_matches || recovery_matches) &&
                update.schedule_version >= ntn_position_plan_static_preflight_schedule_version) {
              ntn_position_plan_static_preflight_schedule_version = update.schedule_version;
              ntn_position_plan_static_preflight_reports          = response.calendar_result->preflight_reports;
            }
            if (response.calendar_result->catalog_version != update.catalog_version ||
                response.calendar_result->schedule_version != update.schedule_version ||
                response.calendar_result->source_content_hash != update.source_content_hash ||
                response.calendar_result->calendar_hash != update.calendar_hash) {
              if (query_for_recovery) {
                ntn_onboard_position_plan_ctrl->reject_recovery(
                    update.schedule_version,
                    update.calendar_hash,
                    ntn_position_plan_reject_reason::du_reconciliation_failed,
                    "du_recovery_response_version_or_hash_mismatch");
              } else {
                ntn_onboard_position_plan_ctrl->reject_pending_deployment(
                    update.schedule_version,
                    update.calendar_hash,
                    ntn_position_plan_reject_reason::calendar_hash_mismatch,
                    "du_query_response_version_or_hash_mismatch");
                should_clear_rejected = true;
              }
            } else if (response.calendar_result->status == f1ap_ntn_access_calendar_result_status::unsupported) {
              if (query_for_recovery) {
                ntn_onboard_position_plan_ctrl->reject_recovery(
                    update.schedule_version,
                    update.calendar_hash,
                    ntn_position_plan_reject_reason::du_reconciliation_failed,
                    response.calendar_result->reject_reason.empty() ? "du_recovery_query_unsupported"
                                                                    : response.calendar_result->reject_reason);
              } else {
                ntn_onboard_position_plan_ctrl->reject_pending_deployment(
                    update.schedule_version,
                    update.calendar_hash,
                    ntn_position_plan_reject_reason::execution_unsupported,
                    response.calendar_result->reject_reason.empty() ? "scheduler_preflight_unsupported"
                                                                    : response.calendar_result->reject_reason);
                // A normal query follows prepare, so a possibly installed gate is cleared best effort.
                should_clear_rejected = true;
              }
            } else if (response_carries_preflight && !preflight.accepted) {
              if (query_for_recovery) {
                ntn_onboard_position_plan_ctrl->reject_recovery(
                    update.schedule_version,
                    update.calendar_hash,
                    ntn_position_plan_reject_reason::du_reconciliation_failed,
                    preflight.detail);
              } else {
                ntn_onboard_position_plan_ctrl->reject_pending_deployment(
                    update.schedule_version,
                    update.calendar_hash,
                    ntn_position_plan_reject_reason::static_opportunity_mismatch,
                    preflight.detail);
                should_clear_rejected = true;
              }
            } else if (response_accepts_calendar &&
                       (static_cast<size_t>(response.calendar_result->accepted_intents_per_cell[0]) !=
                            expected_intents_per_cell[0] ||
                        static_cast<size_t>(response.calendar_result->accepted_intents_per_cell[1]) !=
                            expected_intents_per_cell[1])) {
              if (query_for_recovery) {
                ntn_onboard_position_plan_ctrl->reject_recovery(
                    update.schedule_version,
                    update.calendar_hash,
                    ntn_position_plan_reject_reason::du_reconciliation_failed,
                    "du_recovery_accepted_intent_count_mismatch");
              } else {
                ntn_onboard_position_plan_ctrl->reject_pending_deployment(
                    update.schedule_version,
                    update.calendar_hash,
                    query_for_prepare ? ntn_position_plan_reject_reason::du_prepare_rejected
                                      : ntn_position_plan_reject_reason::du_activation_not_applied,
                    "du_query_accepted_intent_count_mismatch");
                should_clear_rejected = true;
              }
            } else if (response.calendar_result->status == f1ap_ntn_access_calendar_result_status::preparing) {
              // Keep polling until both scheduler slot threads report armed or the prepare guard expires.
            } else if (response.calendar_result->status == f1ap_ntn_access_calendar_result_status::ready) {
              if (!query_for_recovery) {
                ntn_onboard_position_plan_ctrl->mark_deployment_ready(update.schedule_version, update.calendar_hash);
              }
            } else if (response.calendar_result->status == f1ap_ntn_access_calendar_result_status::applied) {
              if (query_for_recovery) {
                const auto recovery_fallback = ntn_onboard_position_plan_ctrl->recovery_fallback_plan();
                const bool recovery_confirmed = ntn_onboard_position_plan_ctrl->confirm_recovery_applied(
                    update.schedule_version, update.calendar_hash, response_time);
                const bool recovered_plan_is_now_active =
                    recovery_confirmed && ntn_onboard_position_plan_ctrl->active_plan().has_value() &&
                    ntn_onboard_position_plan_ctrl->active_plan()->source.schedule_version ==
                        update.schedule_version &&
                    ntn_onboard_position_plan_ctrl->active_plan()->calendar_hash == update.calendar_hash;
                if (recovered_plan_is_now_active && recovery_fallback.has_value()) {
                  clear_was_queued = queue_ntn_onboard_position_plan_clear_locked(
                      *recovery_fallback, "recovered_pending_replaced_historical_active");
                }
              } else {
                ntn_onboard_position_plan_ctrl->mark_deployment_applied(update.schedule_version, update.calendar_hash);
                ntn_onboard_position_plan_ctrl->advance_time(response_time);
              }
            } else if (response.calendar_result->status == f1ap_ntn_access_calendar_result_status::cleared ||
                       response.calendar_result->status == f1ap_ntn_access_calendar_result_status::rejected ||
                       response.calendar_result->status == f1ap_ntn_access_calendar_result_status::unsupported) {
              const std::string detail =
                  response.calendar_result->reject_reason.empty()
                      ? (response.calendar_result->status == f1ap_ntn_access_calendar_result_status::cleared
                             ? "calendar_cleared_before_application"
                             : "du_application_query_rejected")
                      : response.calendar_result->reject_reason;
              if (query_for_recovery) {
                ntn_onboard_position_plan_ctrl->reject_recovery(
                    update.schedule_version,
                    update.calendar_hash,
                    ntn_position_plan_reject_reason::du_reconciliation_failed,
                    detail);
              } else {
                ntn_onboard_position_plan_ctrl->reject_pending_deployment(
                    update.schedule_version,
                    update.calendar_hash,
                    response.calendar_result->status == f1ap_ntn_access_calendar_result_status::unsupported
                        ? ntn_position_plan_reject_reason::execution_unsupported
                        : ntn_position_plan_reject_reason::du_activation_not_applied,
                    detail);
                should_clear_rejected = true;
              }
            }
          }
          const bool recovery_rejection_changed =
              query_for_recovery && ntn_onboard_position_plan_ctrl.has_value() &&
              ntn_onboard_position_plan_ctrl->last_rejected_schedule_version() == update.schedule_version &&
              (rejection_before != ntn_onboard_position_plan_ctrl->last_rejection_reason() ||
               rejected_version_before != ntn_onboard_position_plan_ctrl->last_rejected_schedule_version());
          const bool du_confirms_calendar_absent =
              response.calendar_result.has_value() &&
              (response.calendar_result->status == f1ap_ntn_access_calendar_result_status::cleared ||
               (response.calendar_result->status == f1ap_ntn_access_calendar_result_status::rejected &&
                response.calendar_result->reject_reason == "calendar_not_found"));
          if (du_confirms_calendar_absent) {
            should_clear_rejected = false;
          }
          if (recovery_rejection_changed && !du_confirms_calendar_absent) {
            should_clear_rejected = true;
          }
          if (should_clear_rejected) {
            clear_was_queued = queue_ntn_onboard_position_plan_clear_locked(
                                   queried_plan,
                                   query_for_recovery ? "rejected_recovery_calendar" : "du_application_query_rejected") ||
                               clear_was_queued;
          }
          const bool controller_changed =
              ntn_onboard_position_plan_ctrl.has_value() &&
              (deployment != ntn_onboard_position_plan_ctrl->deployment_stage() ||
               recovery_stage_before != ntn_onboard_position_plan_ctrl->recovery_stage() ||
               active_before != ntn_onboard_position_plan_ctrl->active_plan().has_value() ||
                pending_before != ntn_onboard_position_plan_ctrl->pending_plan().has_value() ||
                recovery_before != ntn_onboard_position_plan_ctrl->recovery_plan().has_value() ||
                recovery_version_before != ntn_onboard_position_plan_ctrl->recovery_schedule_version() ||
                rejection_before != ntn_onboard_position_plan_ctrl->last_rejection_reason() ||
                rejected_version_before != ntn_onboard_position_plan_ctrl->last_rejected_schedule_version());
          if (controller_changed || clear_was_queued) {
            persist_ntn_onboard_position_plan_state_locked(query_for_recovery ? "du_recovery_query_response"
                                                                              : "du_application_query_response");
          }
        }
        if (clear_was_queued) {
          try_clear_ntn_onboard_position_plan_deployment();
        }
        schedule_ntn_onboard_position_plan_activation();
        CORO_RETURN();
      };
  if (!ntn_position_plan_query_io_sched.schedule(launch_async(std::move(completion_task)))) {
    std::lock_guard<std::mutex> lock(ntn_onboard_position_plan_mutex);
    if (query_request_id == ntn_position_plan_query_request_id) {
      ntn_position_plan_query_in_flight = false;
      ntn_position_plan_query_du_index.reset();
    }
  }
}

bool cu_cp_impl::queue_ntn_onboard_position_plan_clear_locked(const ntn_activated_position_plan& plan,
                                                               std::string                        reason)
{
  const bool already_queued = std::any_of(
      ntn_position_plan_clear_queue.begin(), ntn_position_plan_clear_queue.end(), [&plan](const auto& entry) {
        return entry.first.source.schedule_version == plan.source.schedule_version &&
               normalize_ntn_calendar_hash_for_comparison(entry.first.calendar_hash) ==
                   normalize_ntn_calendar_hash_for_comparison(plan.calendar_hash);
      });
  if (already_queued) {
    return false;
  }
  if (ntn_position_plan_clear_queue.size() >= max_ntn_onboard_position_plan_clear_obligations) {
    ntn_position_plan_state_store_status  = "clear_queue_overflow";
    ntn_position_plan_state_error         = "too_many_outstanding_clears";
    ntn_position_plan_state_write_blocked = true;
    logger.error("Cannot queue NTN calendar clear schedule_version={}. Cause: too_many_outstanding_clears",
                 plan.source.schedule_version);
    return false;
  }
  ntn_position_plan_clear_queue.emplace_back(plan, std::move(reason));
  return true;
}

void cu_cp_impl::try_clear_ntn_onboard_position_plan_deployment()
{
  if (!cfg.mobility.onboard_position_plan.du_execution_enabled || stopped.load()) {
    return;
  }

  std::optional<std::pair<ntn_activated_position_plan, std::string>> queued;
  std::set<du_index_t>                                                disconnected_du_indexes;
  {
    std::lock_guard<std::mutex> lock(ntn_onboard_position_plan_mutex);
    if (ntn_position_plan_state_write_blocked || ntn_position_plan_clear_in_flight ||
        ntn_position_plan_clear_queue.empty()) {
      return;
    }
    queued = ntn_position_plan_clear_queue.front();
    disconnected_du_indexes = ntn_position_plan_disconnected_dus;
  }

  const ntn_onboard_du_cell_resolution resolution =
      resolve_ntn_onboard_du_cells(du_db, queued->first.cell_positions, disconnected_du_indexes);
  if (resolution.status != ntn_onboard_du_cell_resolution_status::resolved) {
    logger.warning("Deferred NTN calendar clear schedule_version={}. Cause={}",
                   queued->first.source.schedule_version,
                   get_ntn_onboard_du_cell_resolution_detail(resolution.status));
    return;
  }
  du_processor* processor = du_db.find_du_processor(resolution.du_index);
  if (processor == nullptr) {
    return;
  }

  f1ap_ntn_access_calendar_update update;
  update.operation            = f1ap_ntn_access_calendar_operation::clear;
  update.satellite_id         = queued->first.source.satellite_id;
  update.catalog_version      = queued->first.source.catalog_version;
  update.schedule_version     = queued->first.source.schedule_version;
  update.source_content_hash  = queued->first.source.content_hash;
  update.calendar_hash        = queued->first.calendar_hash;
  update.activation_epoch_unix_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                              queued->first.source.activation_epoch.time_since_epoch())
                                                              .count());
  update.valid_until_unix_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                         queued->first.source.valid_until.time_since_epoch())
                                                         .count());
  update.cycle_duration_us =
      static_cast<uint32_t>(cfg.mobility.onboard_position_plan.max_prach_interval.count());

  f1ap_gnb_du_resource_coordination_request request;
  request.ntn_access_calendar_update = update;
  uint64_t du_connection_generation = 0;
  uint64_t clear_request_id          = 0;
  {
    std::lock_guard<std::mutex> lock(ntn_onboard_position_plan_mutex);
    if (ntn_position_plan_disconnected_dus.count(resolution.du_index) != 0 ||
        ntn_position_plan_clear_queue.empty() ||
        ntn_position_plan_clear_queue.front().first.source.schedule_version != update.schedule_version ||
        ntn_position_plan_clear_queue.front().first.calendar_hash != update.calendar_hash) {
      return;
    }
    du_connection_generation = ntn_position_plan_du_connection_generations[resolution.du_index];
    clear_request_id          = ++ntn_position_plan_clear_request_id;
    ntn_position_plan_clear_in_flight = true;
    ntn_position_plan_clear_du_index  = resolution.du_index;
  }

  async_task<f1ap_gnb_du_resource_coordination_response> f1ap_task =
      processor->get_f1ap_handler().handle_gnb_du_resource_coordination_request(request);
  auto completion_task =
      [this,
       update,
       du_index = resolution.du_index,
       du_connection_generation,
       clear_request_id,
       f1ap_task = std::move(f1ap_task)](coro_context<async_task<void>>& ctx) mutable {
        f1ap_gnb_du_resource_coordination_response response;
        bool advance_clear_queue = false;
        bool resume_plan_work    = false;
        CORO_BEGIN(ctx);
        CORO_AWAIT_VALUE(response, f1ap_task);
        {
          std::lock_guard<std::mutex> lock(ntn_onboard_position_plan_mutex);
          const bool response_from_current_connection =
              clear_request_id == ntn_position_plan_clear_request_id &&
              ntn_position_plan_clear_du_index == du_index &&
              ntn_position_plan_du_connection_generations[du_index] == du_connection_generation;
          if (clear_request_id == ntn_position_plan_clear_request_id) {
            ntn_position_plan_clear_in_flight = false;
            ntn_position_plan_clear_du_index.reset();
          }
          if (!response_from_current_connection) {
            logger.debug("Ignored stale NTN calendar clear response schedule_version={} from disconnected DU {}",
                         update.schedule_version,
                         du_index);
          } else if (!ntn_position_plan_clear_queue.empty() &&
              ntn_position_plan_clear_queue.front().first.source.schedule_version == update.schedule_version &&
              ntn_position_plan_clear_queue.front().first.calendar_hash == update.calendar_hash &&
              response.calendar_result.has_value() &&
              response.calendar_result->catalog_version == update.catalog_version &&
              response.calendar_result->schedule_version == update.schedule_version &&
              response.calendar_result->source_content_hash == update.source_content_hash &&
              response.calendar_result->calendar_hash == update.calendar_hash) {
            const auto status = response.calendar_result->status;
            const bool terminal = status == f1ap_ntn_access_calendar_result_status::cleared ||
                                  (status == f1ap_ntn_access_calendar_result_status::rejected &&
                                   response.calendar_result->reject_reason == "calendar_not_found");
            if (terminal) {
              const std::string confirmed_clear_reason = ntn_position_plan_clear_queue.front().second;
              const ntn_state_persist_outcome persist_outcome =
                  persist_ntn_onboard_position_plan_state_locked("calendar_clear_confirmed", true);
              advance_clear_queue = persist_outcome == ntn_state_persist_outcome::durable;
              if (advance_clear_queue) {
                ntn_position_plan_clear_queue.erase(ntn_position_plan_clear_queue.begin());
                resume_plan_work = ntn_position_plan_clear_queue.empty();
                logger.info("Confirmed and durably recorded NTN calendar clear schedule_version={} reason={}",
                            update.schedule_version,
                            confirmed_clear_reason);
              } else {
                // The live queue is deliberately left unchanged until the snapshot without its head is durable.
                // State writes are blocked by the persistence helper, so this entry cannot be sent again until a safe
                // restart or operator recovery restores a trustworthy state file.
                logger.error("Confirmed NTN calendar clear schedule_version={} but could not durably record it; "
                             "retaining the cleanup obligation in fail-closed state",
                             update.schedule_version);
              }
            }
          }
        }
        if (advance_clear_queue) {
          try_clear_ntn_onboard_position_plan_deployment();
        }
        if (resume_plan_work) {
          reload_ntn_onboard_position_plan();
        }
        schedule_ntn_onboard_position_plan_activation();
        CORO_RETURN();
      };
  if (!ntn_position_plan_clear_io_sched.schedule(launch_async(std::move(completion_task)))) {
    std::lock_guard<std::mutex> lock(ntn_onboard_position_plan_mutex);
    if (clear_request_id == ntn_position_plan_clear_request_id) {
      ntn_position_plan_clear_in_flight = false;
      ntn_position_plan_clear_du_index.reset();
    }
  }
}

void cu_cp_impl::schedule_ntn_onboard_position_plan_reload()
{
  bool controller_available = false;
  {
    std::lock_guard<std::mutex> lock(ntn_onboard_position_plan_mutex);
    controller_available = ntn_onboard_position_plan_ctrl.has_value();
  }
  if (stopped.load() || !controller_available || !ntn_position_plan_reload_deadline.has_value() ||
      cfg.mobility.onboard_position_plan.reload_period.count() <= 0) {
    ntn_position_plan_reload_timer.stop();
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  if (now >= ntn_position_plan_reload_deadline.value()) {
    on_ntn_onboard_position_plan_reload_timer_expired();
    return;
  }
  const auto remaining = ntn_position_plan_reload_deadline.value() - now;
  auto       delay     = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
  if (std::chrono::duration_cast<std::chrono::steady_clock::duration>(delay) < remaining) {
    delay += std::chrono::milliseconds{1};
  }
  delay = limit_ntn_position_plan_timer_delay(delay);
  ntn_position_plan_reload_timer.set(delay,
                                     [this](timer_id_t /*tid*/) {
                                       on_ntn_onboard_position_plan_reload_timer_expired();
                                     });
  ntn_position_plan_reload_timer.run();
}

void cu_cp_impl::schedule_ntn_onboard_position_plan_activation()
{
  std::optional<std::chrono::system_clock::time_point> next_deadline;
  const auto                                            now = std::chrono::system_clock::now();
  {
    std::lock_guard<std::mutex> lock(ntn_onboard_position_plan_mutex);
    if (ntn_onboard_position_plan_ctrl.has_value()) {
      if (ntn_onboard_position_plan_ctrl->recovery_plan().has_value()) {
        const auto& recovering = *ntn_onboard_position_plan_ctrl->recovery_plan();
        next_deadline          = std::min(recovering.source.valid_until, now + std::chrono::milliseconds{500});
      } else if (ntn_onboard_position_plan_ctrl->pending_plan().has_value()) {
        const auto& pending = *ntn_onboard_position_plan_ctrl->pending_plan();
        if (!cfg.mobility.onboard_position_plan.du_execution_enabled) {
          next_deadline = pending.source.activation_epoch;
        } else {
          switch (ntn_onboard_position_plan_ctrl->deployment_stage()) {
            case ntn_position_plan_deployment_stage::applied:
              next_deadline = pending.source.activation_epoch;
              break;
            case ntn_position_plan_deployment_stage::ready:
              next_deadline = now < pending.source.activation_epoch
                                  ? pending.source.activation_epoch
                                  : std::min(pending.source.activation_epoch +
                                                 cfg.mobility.onboard_position_plan.du_apply_timeout,
                                             now + std::chrono::milliseconds{100});
              break;
            case ntn_position_plan_deployment_stage::not_sent:
              if (now < pending.source.activation_epoch - cfg.mobility.onboard_position_plan.du_prepare_horizon) {
                next_deadline = pending.source.activation_epoch -
                                cfg.mobility.onboard_position_plan.du_prepare_horizon;
              } else {
                next_deadline = std::min(pending.source.activation_epoch -
                                             cfg.mobility.onboard_position_plan.du_prepare_guard,
                                         now + std::chrono::milliseconds{500});
              }
              break;
            case ntn_position_plan_deployment_stage::preparing:
              next_deadline = std::min(pending.source.activation_epoch -
                                           cfg.mobility.onboard_position_plan.du_prepare_guard,
                                       now + std::chrono::milliseconds{50});
              break;
            case ntn_position_plan_deployment_stage::disabled:
            case ntn_position_plan_deployment_stage::rejected:
            case ntn_position_plan_deployment_stage::unsupported:
              next_deadline = pending.source.activation_epoch;
              break;
          }
        }
        next_deadline = std::min(next_deadline.value(), pending.source.valid_until);
      }
      if (ntn_onboard_position_plan_ctrl->active_plan().has_value() &&
          (!next_deadline.has_value() ||
           ntn_onboard_position_plan_ctrl->active_plan()->source.valid_until < next_deadline.value())) {
        next_deadline = ntn_onboard_position_plan_ctrl->active_plan()->source.valid_until;
      }
      if (!ntn_position_plan_state_write_blocked && !ntn_position_plan_clear_queue.empty() &&
          !ntn_position_plan_clear_in_flight) {
        const auto clear_retry = now + std::chrono::milliseconds{500};
        next_deadline = next_deadline.has_value() ? std::min(next_deadline.value(), clear_retry) : clear_retry;
      }
    }
  }
  if (stopped.load() || !next_deadline.has_value()) {
    ntn_position_plan_activation_timer.stop();
    return;
  }

  const auto remaining = next_deadline.value() - now;
  if (remaining <= std::chrono::system_clock::duration::zero()) {
    ntn_position_plan_activation_timer.set(std::chrono::milliseconds{1}, [this](timer_id_t /*tid*/) {
      on_ntn_onboard_position_plan_activation_timer_expired();
    });
    ntn_position_plan_activation_timer.run();
    return;
  }

  auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
  if (std::chrono::duration_cast<std::chrono::system_clock::duration>(delay) < remaining) {
    delay += std::chrono::milliseconds{1};
  }
  delay = limit_ntn_position_plan_timer_delay(delay);
  ntn_position_plan_activation_timer.set(delay, [this](timer_id_t /*tid*/) {
    on_ntn_onboard_position_plan_activation_timer_expired();
  });
  ntn_position_plan_activation_timer.run();
}

void cu_cp_impl::on_ntn_onboard_position_plan_activation_timer_expired()
{
  if (stopped.load()) {
    return;
  }

  const auto                                 now                      = std::chrono::system_clock::now();
  bool                                       should_prepare           = false;
  bool                                       should_query             = false;
  bool                                       controller_state_changed = false;
  std::optional<uint64_t>                    activated_version;
  std::optional<ntn_activated_position_plan> plan_to_clear;
  std::optional<ntn_activated_position_plan> expired_active_to_clear;
  std::string                                plan_clear_reason;
  {
    std::lock_guard<std::mutex> lock(ntn_onboard_position_plan_mutex);
    if (!ntn_onboard_position_plan_ctrl.has_value()) {
      return;
    }

    if (!cfg.mobility.onboard_position_plan.du_execution_enabled) {
      if (ntn_onboard_position_plan_ctrl->advance_time(now) &&
          ntn_onboard_position_plan_ctrl->active_plan().has_value()) {
        activated_version = ntn_onboard_position_plan_ctrl->active_plan()->source.schedule_version;
      }
    } else {
      if (ntn_onboard_position_plan_ctrl->recovery_plan().has_value()) {
        const auto recovering = *ntn_onboard_position_plan_ctrl->recovery_plan();
        if (now >= recovering.source.valid_until) {
          plan_to_clear     = recovering;
          plan_clear_reason = "recovery_plan_expired";
          ntn_onboard_position_plan_ctrl->advance_time(now);
          controller_state_changed = true;
        } else {
          should_query = true;
        }
      } else {
        if (ntn_onboard_position_plan_ctrl->active_plan().has_value() &&
            ntn_onboard_position_plan_ctrl->active_has_external_apply_evidence() &&
            now >= ntn_onboard_position_plan_ctrl->active_plan()->source.valid_until) {
          expired_active_to_clear = *ntn_onboard_position_plan_ctrl->active_plan();
        }
        if (ntn_onboard_position_plan_ctrl->pending_plan().has_value() &&
            now >= ntn_onboard_position_plan_ctrl->pending_plan()->source.valid_until &&
            ntn_onboard_position_plan_ctrl->deployment_stage() != ntn_position_plan_deployment_stage::not_sent) {
          plan_to_clear     = *ntn_onboard_position_plan_ctrl->pending_plan();
          plan_clear_reason = "pending_plan_expired";
        }
        // Always let the controller expire an old active plan. External mode still prevents an un-applied pending plan
        // from being promoted.
        std::optional<ntn_activated_position_plan> recovered_fallback_to_clear;
        if (ntn_onboard_position_plan_ctrl->recovery_fallback_plan().has_value() &&
            ntn_onboard_position_plan_ctrl->pending_plan().has_value() &&
            now >= ntn_onboard_position_plan_ctrl->pending_plan()->source.activation_epoch &&
            ntn_onboard_position_plan_ctrl->deployment_stage() == ntn_position_plan_deployment_stage::applied) {
          const ntn_activated_position_plan& fallback = *ntn_onboard_position_plan_ctrl->recovery_fallback_plan();
          recovered_fallback_to_clear.emplace();
          recovered_fallback_to_clear->source         = fallback.source;
          recovered_fallback_to_clear->cell_positions = fallback.cell_positions;
          recovered_fallback_to_clear->calendar_hash  = fallback.calendar_hash;
        }
        const bool had_active_plan  = ntn_onboard_position_plan_ctrl->active_plan().has_value();
        const bool had_pending_plan = ntn_onboard_position_plan_ctrl->pending_plan().has_value();
        if (ntn_onboard_position_plan_ctrl->advance_time(now) &&
            ntn_onboard_position_plan_ctrl->active_plan().has_value()) {
          activated_version = ntn_onboard_position_plan_ctrl->active_plan()->source.schedule_version;
          if (recovered_fallback_to_clear.has_value() &&
              recovered_fallback_to_clear->source.schedule_version != activated_version.value()) {
            plan_to_clear     = std::move(recovered_fallback_to_clear);
            plan_clear_reason = "recovered_pending_replaced_historical_active";
          }
        }
        controller_state_changed = controller_state_changed ||
                                   (had_active_plan != ntn_onboard_position_plan_ctrl->active_plan().has_value()) ||
                                   (had_pending_plan != ntn_onboard_position_plan_ctrl->pending_plan().has_value());
        if (ntn_onboard_position_plan_ctrl->pending_plan().has_value()) {
          const auto& pending    = *ntn_onboard_position_plan_ctrl->pending_plan();
          const auto  deployment = ntn_onboard_position_plan_ctrl->deployment_stage();
          const auto  prepare_deadline =
              pending.source.activation_epoch - cfg.mobility.onboard_position_plan.du_prepare_guard;
          const auto apply_deadline =
              pending.source.activation_epoch + cfg.mobility.onboard_position_plan.du_apply_timeout;
          const bool prepare_was_dispatched =
              ntn_position_plan_prepare_dispatched.has_value() &&
              ntn_position_plan_prepare_dispatched->first == pending.source.schedule_version &&
              ntn_position_plan_prepare_dispatched->second == pending.calendar_hash;
          if (deployment == ntn_position_plan_deployment_stage::ready && now >= apply_deadline) {
            plan_to_clear     = pending;
            plan_clear_reason = "du_application_deadline_missed";
            ntn_onboard_position_plan_ctrl->reject_pending_deployment(
                pending.source.schedule_version,
                pending.calendar_hash,
                ntn_position_plan_reject_reason::du_activation_not_applied,
                "both_cell_application_not_confirmed_before_deadline");
          } else if (now >= prepare_deadline && (deployment == ntn_position_plan_deployment_stage::not_sent ||
                                                 deployment == ntn_position_plan_deployment_stage::preparing)) {
            plan_to_clear     = pending;
            plan_clear_reason = "du_prepare_guard_reached_before_du_ready";
            ntn_onboard_position_plan_ctrl->reject_pending_deployment(
                pending.source.schedule_version,
                pending.calendar_hash,
                ntn_position_plan_reject_reason::du_prepare_timeout,
                "du_prepare_guard_reached_before_du_ready");
          } else {
            should_prepare = deployment == ntn_position_plan_deployment_stage::not_sent;
            should_query =
                (deployment == ntn_position_plan_deployment_stage::preparing && prepare_was_dispatched) ||
                (deployment == ntn_position_plan_deployment_stage::ready && now >= pending.source.activation_epoch);
          }
        }
      }
    }
    if (expired_active_to_clear.has_value()) {
      queue_ntn_onboard_position_plan_clear_locked(*expired_active_to_clear, "active_plan_expired");
    }
    if (plan_to_clear.has_value()) {
      queue_ntn_onboard_position_plan_clear_locked(
          *plan_to_clear,
          plan_clear_reason.empty() ? "pending_deployment_rejected" : plan_clear_reason);
    }
    if (cfg.mobility.onboard_position_plan.du_execution_enabled &&
        (controller_state_changed || activated_version.has_value() || expired_active_to_clear.has_value() ||
         plan_to_clear.has_value())) {
      persist_ntn_onboard_position_plan_state_locked(activated_version.has_value() ? "plan_activated"
                                                     : expired_active_to_clear.has_value() ? "active_plan_expired"
                                                     : plan_to_clear.has_value() ? "pending_plan_rejected"
                                                                                 : "plan_state_advanced");
    }
  }

  if (activated_version.has_value()) {
    logger.info("Activated NTN onboard position plan schedule_version={} evidence={}",
                activated_version.value(),
                cfg.mobility.onboard_position_plan.du_execution_enabled
                    ? "ssb_prach_software_gate_applied_no_position_or_rf_evidence"
                    : "cu_cp_intent_only");
  }
  if (plan_to_clear.has_value()) {
    try_clear_ntn_onboard_position_plan_deployment();
  }
  if (should_prepare) {
    try_prepare_ntn_onboard_position_plan();
  }
  if (should_query) {
    query_ntn_onboard_position_plan_application();
  }
  try_clear_ntn_onboard_position_plan_deployment();
  schedule_ntn_onboard_position_plan_activation();
}

void cu_cp_impl::on_ntn_onboard_position_plan_reload_timer_expired()
{
  if (stopped.load()) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (ntn_position_plan_reload_deadline.has_value() && now < ntn_position_plan_reload_deadline.value()) {
    schedule_ntn_onboard_position_plan_reload();
    return;
  }
  reload_ntn_onboard_position_plan();
  const auto period = cfg.mobility.onboard_position_plan.reload_period;
  if (period.count() > 0) {
    ntn_position_plan_reload_deadline = std::chrono::steady_clock::now() + period;
    schedule_ntn_onboard_position_plan_reload();
  }
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
