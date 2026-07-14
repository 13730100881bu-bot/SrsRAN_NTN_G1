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

#include "srsran/adt/expected.h"
#include "srsran/ran/nr_cell_identity.h"
#include "srsran/ran/pci.h"
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace srsran {
namespace srs_cu_cp {

/// Earth-fixed L1 position supplied by the management-center catalog. It intentionally carries no NCI or PCI.
struct ntn_l1_position {
  std::string position_id;
  double      latitude_deg  = 0.0;
  double      longitude_deg = 0.0;
};

/// Stable identity of one long-lived onboard NR logical cell.
struct ntn_onboard_cell_identity {
  nr_cell_identity nci = nr_cell_identity::min();
  pci_t            pci = INVALID_PCI;
};

/// Versioned management-center input for one satellite.
struct ntn_versioned_position_plan {
  std::string satellite_id;
  uint64_t    catalog_version  = 0;
  uint64_t    schedule_version = 0;
  std::string content_hash;
  std::chrono::system_clock::time_point valid_from{};
  std::chrono::system_clock::time_point valid_until{};
  std::chrono::system_clock::time_point activation_epoch{};
  std::array<ntn_onboard_cell_identity, 2> onboard_cells{};
  std::vector<ntn_l1_position>             visible_l1_positions;
};

/// L1 assignment owned by one stable onboard cell identity.
struct ntn_onboard_cell_position_set {
  ntn_onboard_cell_identity identity;
  std::vector<std::string>  assigned_l1_ids;
};

enum class ntn_access_calendar_direction { downlink, uplink };

enum class ntn_access_calendar_purpose {
  ssb_sib_paging,
  ssb_sib_paging_rar,
  prach_ro,
  prach_ul_beam
};

enum class ntn_access_calendar_state { proposed, checked };

/// CU-CP access-calendar intent. It is a plan/audit artifact and does not claim DU/RF execution.
struct ntn_access_calendar_intent {
  static constexpr uint16_t no_resource_port = std::numeric_limits<uint16_t>::max();

  uint64_t                      schedule_version = 0;
  nr_cell_identity              nci              = nr_cell_identity::min();
  std::string                   position_id;
  std::chrono::microseconds     start_time{0};
  std::chrono::microseconds     duration{0};
  ntn_access_calendar_direction direction = ntn_access_calendar_direction::downlink;
  ntn_access_calendar_purpose   purpose   = ntn_access_calendar_purpose::ssb_sib_paging;
  ntn_access_calendar_state     state     = ntn_access_calendar_state::proposed;
  uint16_t                      port_id   = no_resource_port;
};

enum class ntn_position_plan_stage { disabled, received, validated, calendar_checked, pending, active, rejected };

/// Cross-layer deployment progress for a checked access calendar. This is intentionally separate from plan validation.
enum class ntn_position_plan_deployment_stage {
  disabled,
  not_sent,
  preparing,
  ready,
  applied,
  rejected,
  unsupported
};

enum class ntn_position_plan_reject_reason {
  none,
  feature_disabled,
  parse_error,
  invalid_satellite_id,
  non_monotonic_version,
  invalid_hash,
  expired,
  invalid_validity_window,
  invalid_activation_epoch,
  invalid_l1_id,
  duplicate_l1_id,
  invalid_l1_position,
  identity_mismatch,
  schedule_overflow,
  invalid_calendar_position,
  invalid_resource_port,
  ssb_deadline_miss,
  prach_deadline_miss,
  prach_ro_without_beam,
  resource_conflict,
  du_unavailable,
  cross_du_calendar_not_supported,
  du_prepare_rejected,
  du_prepare_timeout,
  du_activation_not_applied,
  calendar_hash_mismatch,
  execution_unsupported
};

const char* to_string(ntn_position_plan_stage stage);
const char* to_string(ntn_position_plan_deployment_stage stage);
const char* to_string(ntn_position_plan_reject_reason reason);
const char* to_string(ntn_access_calendar_direction direction);
const char* to_string(ntn_access_calendar_purpose purpose);
const char* to_string(ntn_access_calendar_state state);

/// Caps one timer arm below the timer backend limit. Longer deadlines are reached through repeated slices.
std::chrono::milliseconds limit_ntn_position_plan_timer_delay(std::chrono::milliseconds requested_delay);

struct ntn_access_calendar_audit {
  bool                            accepted = false;
  ntn_position_plan_reject_reason reason   = ntn_position_plan_reject_reason::none;
  unsigned                        nof_l1_positions = 0;
  unsigned                        nof_calendar_intents = 0;
  unsigned                        max_used_analog_ports_per_cell = 0;
  unsigned                        max_used_analog_ports_per_satellite = 0;
  std::chrono::microseconds       max_ssb_interval{0};
  std::chrono::microseconds       max_prach_interval{0};
  unsigned                        prach_ro_without_beam = 0;
  unsigned                        resource_conflicts   = 0;
};

/// Configurable planning parameters. These are not protocol or hardware constants.
struct ntn_onboard_position_plan_config {
  bool                                      enabled = false;
  /// When enabled, a pending plan may become active only after matching DU/MAC applied feedback.
  bool                                      require_external_apply = false;
  std::string                               satellite_id;
  std::array<ntn_onboard_cell_identity, 2> onboard_cells{};
  unsigned                                  max_l1_positions_per_cell = 128;
  unsigned                                  max_l1_positions_per_satellite = 256;
  unsigned                                  max_analog_ports_per_cell = 16;
  unsigned                                  max_analog_ports_per_satellite = 32;
  std::chrono::microseconds                 access_slot{10000};
  std::chrono::microseconds                 subvisit_duration{2500};
  std::chrono::microseconds                 max_ssb_interval{80000};
  std::chrono::microseconds                 max_prach_interval{640000};
  std::chrono::milliseconds                 activation_alignment{640};
};

struct ntn_activated_position_plan {
  ntn_versioned_position_plan                 source;
  std::array<ntn_onboard_cell_position_set, 2> cell_positions{};
  std::vector<ntn_access_calendar_intent>       access_calendar;
  ntn_access_calendar_audit                     calendar_audit;
  std::string                                   calendar_hash;
};

struct ntn_position_plan_submit_result {
  bool                            accepted = false;
  ntn_position_plan_stage         stage    = ntn_position_plan_stage::rejected;
  ntn_position_plan_reject_reason reason   = ntn_position_plan_reject_reason::none;
};

/// Decision produced by the private Initial UL active-plan auditor. It is not an RF execution result.
enum class ntn_initial_access_plan_decision { accept, reject, audit_only };

enum class ntn_initial_access_plan_reason {
  none,
  feature_disabled,
  incomplete_metadata,
  no_active_plan,
  active_plan_not_valid,
  satellite_mismatch,
  catalog_version_mismatch,
  schedule_version_mismatch,
  source_hash_mismatch,
  calendar_hash_mismatch,
  cell_identity_mismatch,
  position_not_assigned_to_cell,
  missing_external_apply_evidence,
  prach_occasion_not_scheduled,
  prach_ul_beam_missing,
  resource_port_mismatch,
  intent_only_plan
};

const char* to_string(ntn_initial_access_plan_decision decision);
const char* to_string(ntn_initial_access_plan_reason reason);

/// Proposed private sideband metadata for auditing one Initial UL event against the current active plan.
///
/// No production F1AP transport carries this structure yet. In particular, this metadata must not be inferred from
/// the legacy beam-to-NCI mapping.
struct ntn_initial_access_plan_event {
  std::string                           satellite_id;
  uint64_t                              catalog_version  = 0;
  uint64_t                              schedule_version = 0;
  std::string                           source_content_hash;
  std::string                           calendar_hash;
  ntn_onboard_cell_identity             cell;
  std::string                           position_id;
  std::chrono::system_clock::time_point occasion_time{};
  uint16_t                              ul_beam_port_id = ntn_access_calendar_intent::no_resource_port;
};

struct ntn_initial_access_plan_audit {
  ntn_initial_access_plan_decision decision                = ntn_initial_access_plan_decision::reject;
  ntn_initial_access_plan_reason   reason                  = ntn_initial_access_plan_reason::incomplete_metadata;
  uint64_t                         active_schedule_version = 0;
  std::string                      active_calendar_hash;
  std::chrono::microseconds        occasion_offset{0};
  std::string                      evidence = "cu_cp_active_plan_not_evaluated_no_rf_evidence";
};

/// Private CU-CP controller for version validation, deterministic two-cell partition, calendar audit and activation.
class ntn_onboard_position_plan_controller
{
public:
  explicit ntn_onboard_position_plan_controller(ntn_onboard_position_plan_config config_ = {});

  ntn_position_plan_submit_result submit(const ntn_versioned_position_plan&          plan,
                                         std::chrono::system_clock::time_point now);

  /// Atomically promotes the complete pending plan when its activation epoch is due.
  bool advance_time(std::chrono::system_clock::time_point now);

  /// Records that the matching pending calendar has been sent to the DU for preparation.
  bool mark_deployment_preparing(uint64_t schedule_version, const std::string& calendar_hash);

  /// Returns a matching in-flight prepare to retryable not-sent state without disturbing active service.
  bool mark_deployment_retryable(uint64_t           schedule_version,
                                 const std::string& calendar_hash,
                                 std::string        detail);

  /// Records matching DU ready feedback. Stale version/hash feedback is ignored.
  bool mark_deployment_ready(uint64_t schedule_version, const std::string& calendar_hash);

  /// Records matching DU/MAC applied feedback. Stale version/hash feedback is ignored.
  bool mark_deployment_applied(uint64_t schedule_version, const std::string& calendar_hash);

  /// Rejects and removes only the matching pending plan, preserving the old active plan.
  bool reject_pending_deployment(uint64_t                        schedule_version,
                                 const std::string&              calendar_hash,
                                 ntn_position_plan_reject_reason reason,
                                 std::string                     detail);

  /// Records a provider/parser failure without disturbing active or pending state.
  void record_external_rejection(ntn_position_plan_reject_reason reason, uint64_t schedule_version = 0);

  std::vector<ntn_access_calendar_intent>
  build_access_calendar(uint64_t schedule_version,
                        const std::array<ntn_onboard_cell_position_set, 2>& assignments) const;

  ntn_access_calendar_audit
  audit_access_calendar(uint64_t schedule_version,
                        const std::array<ntn_onboard_cell_position_set, 2>& assignments,
                        const std::vector<ntn_access_calendar_intent>&       intents) const;

  /// Audits complete sideband metadata against the currently active calendar without mutating admission state.
  ntn_initial_access_plan_audit audit_initial_access_event(const ntn_initial_access_plan_event& event) const;

  const ntn_onboard_position_plan_config& config() const { return cfg; }
  ntn_position_plan_stage                 stage() const { return current_stage; }
  ntn_position_plan_reject_reason         last_rejection_reason() const { return last_rejection; }
  uint64_t                                last_rejected_schedule_version() const { return last_rejected_version; }
  bool                                    has_received_plan() const { return received_plan_present; }
  uint64_t                                last_received_catalog_version() const { return last_received_catalog; }
  uint64_t                                last_received_schedule_version() const { return last_received_schedule; }
  const std::string&                      last_received_content_hash() const { return last_received_hash; }
  std::chrono::system_clock::time_point   last_received_activation_epoch() const { return last_received_activation; }
  const std::vector<ntn_l1_position>&     candidate_inventory() const { return last_candidate_inventory; }
  const std::optional<ntn_activated_position_plan>& active_plan() const { return active; }
  const std::optional<ntn_activated_position_plan>& pending_plan() const { return pending; }
  /// True only while the current active plan was promoted after matching external applied feedback.
  bool active_has_external_apply_evidence() const { return active_external_apply_evidence; }
  ntn_position_plan_deployment_stage deployment_stage() const { return deployment; }
  const std::string&                  deployment_detail() const { return deployment_reason; }

private:
  ntn_position_plan_reject_reason validate_plan(const ntn_versioned_position_plan&          plan,
                                                std::chrono::system_clock::time_point now) const;
  std::array<ntn_onboard_cell_position_set, 2>
  partition_positions(const std::vector<ntn_l1_position>& positions) const;
  ntn_position_plan_submit_result reject(ntn_position_plan_reject_reason reason, uint64_t schedule_version);

  ntn_onboard_position_plan_config            cfg;
  ntn_position_plan_stage                     current_stage = ntn_position_plan_stage::disabled;
  ntn_position_plan_reject_reason             last_rejection = ntn_position_plan_reject_reason::none;
  uint64_t                                    last_rejected_version = 0;
  bool                                        received_plan_present = false;
  uint64_t                                    last_received_catalog = 0;
  uint64_t                                    last_received_schedule = 0;
  std::string                                 last_received_hash;
  std::chrono::system_clock::time_point       last_received_activation{};
  uint64_t                                    highest_catalog_version = 0;
  uint64_t                                    highest_schedule_version = 0;
  std::vector<ntn_l1_position>                last_candidate_inventory;
  std::optional<ntn_activated_position_plan> active;
  std::optional<ntn_activated_position_plan> pending;
  bool                                      active_external_apply_evidence = false;
  ntn_position_plan_deployment_stage         deployment = ntn_position_plan_deployment_stage::disabled;
  std::string                                deployment_reason = "external_execution_disabled";
};

/// Computes the canonical SHA-256 content hash used by the plan validator.
std::string compute_ntn_position_plan_content_hash(const ntn_versioned_position_plan& plan);

/// Computes a canonical SHA-256 over the checked, executable calendar artifact.
std::string compute_ntn_access_calendar_hash(uint64_t                                      schedule_version,
                                             const std::vector<ntn_access_calendar_intent>& intents);

/// Parses a management-center plan JSON document.
expected<ntn_versioned_position_plan, std::string> parse_ntn_position_plan_json(const std::string& json_text);

/// Loads and parses a management-center plan JSON file.
expected<ntn_versioned_position_plan, std::string> load_ntn_position_plan_json_file(const std::string& path);

} // namespace srs_cu_cp
} // namespace srsran
