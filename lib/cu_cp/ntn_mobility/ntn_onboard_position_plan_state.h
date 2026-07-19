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
 * the LICENSE file in the top-level directory of this distribution.
 *
 */

#pragma once

#include "ntn_onboard_position_plan.h"
#include "srsran/adt/expected.h"
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace srsran {
namespace srs_cu_cp {

/// On-disk state is deliberately bounded before JSON parsing.
inline constexpr size_t max_ntn_onboard_position_plan_state_file_size   = 4U * 1024U * 1024U;
inline constexpr size_t max_ntn_onboard_position_plan_clear_obligations = 64U;
inline constexpr size_t max_ntn_onboard_position_plan_observed_positions = 65536U;

/// Management-center artifacts that bind the persisted state to one planning context.
struct ntn_onboard_position_plan_state_context {
  std::string catalog_id;
  std::string catalog_hash;
  std::string identity_registry_version;
  std::string identity_registry_hash;
  std::string access_profile_id;
  std::string access_profile_hash;
};

/// Checked plan data that is persisted across CU-CP restarts.
///
/// The access calendar and audit are intentionally not serialized. The controller must rebuild and re-audit them from
/// source and cell_positions, then compare the resulting calendar_hash before accepting recovery.
struct ntn_onboard_position_plan_state_snapshot {
  ntn_versioned_position_plan                  source;
  std::array<ntn_onboard_cell_position_set, 2> cell_positions{};
  std::string                                  calendar_hash;
};

/// Durable best-effort removal of a calendar that must not become authoritative after restart.
struct ntn_onboard_position_plan_clear_obligation {
  ntn_onboard_position_plan_state_snapshot snapshot;
  std::string                              reason;
};

/// Latest management-center input retained for restart-safe read-only observation.
///
/// This record is not a checked deployment snapshot and is never used as identity or DU application authority. It is
/// stored separately so rejected or expired inputs keep their complete candidate inventory after cleanup and restart.
struct ntn_onboard_position_plan_received_observation {
  uint64_t                              catalog_version  = 0;
  uint64_t                              schedule_version = 0;
  std::string                           content_hash;
  std::chrono::system_clock::time_point activation_epoch{};
  std::vector<ntn_l1_position>          candidate_inventory;
};

/// Private recovery record for the onboard position-plan controller.
///
/// recorded_deployment_stage is historical information only. In particular, a persisted value of applied is not live
/// DU evidence after restart. Consumers must reconcile with the DU before exposing active/applied state.
struct ntn_onboard_position_plan_persistent_state {
  static constexpr unsigned current_schema_version = 2;

  unsigned    schema_version = current_schema_version;
  uint64_t    generation     = 0;
  std::string state_hash;

  std::string                                             satellite_id;
  ntn_onboard_position_plan_state_context                 planning_context;
  std::array<ntn_onboard_cell_identity, 2>                onboard_cells{};
  uint64_t                                                highest_catalog_version  = 0;
  uint64_t                                                highest_schedule_version = 0;
  std::optional<ntn_onboard_position_plan_state_snapshot> active;
  std::optional<ntn_onboard_position_plan_state_snapshot> pending;
  std::optional<ntn_onboard_position_plan_received_observation> received_plan;
  std::array<ntn_onboard_cell_position_set, 2>            sticky_partition{};
  std::vector<ntn_onboard_position_plan_clear_obligation> outstanding_clears;

  ntn_position_plan_deployment_stage recorded_deployment_stage = ntn_position_plan_deployment_stage::not_sent;
  std::string                        recorded_deployment_detail;
  uint64_t                           recorded_deployment_schedule_version = 0;
  std::string                        recorded_deployment_calendar_hash;
  /// Schema v1 always stores true. It prevents historical applied state from being interpreted as current evidence.
  bool du_reconciliation_required = true;
};

/// Loads a private recovery state. A missing file is a valid first boot and returns an empty optional.
/// All other I/O, schema, integrity and internal-consistency errors fail closed.
expected<std::optional<ntn_onboard_position_plan_persistent_state>, std::string>
load_ntn_onboard_position_plan_state(const std::string& path);

/// Result returned once rename has committed the new state bytes.
///
/// durable=false means that the target was replaced, but the parent-directory durability barrier failed. Callers must
/// keep the matching in-memory generation, fail closed and never roll back to the pre-rename controller snapshot.
struct ntn_onboard_position_plan_state_store_result {
  std::string state_hash;
  bool        durable = false;
  std::string durability_error;
};

/// Private deterministic failpoints used only by the focused atomic-replacement tests.
enum class ntn_onboard_position_plan_state_store_failpoint { none, before_rename, after_rename };

/// Atomically replaces a private recovery state using a same-directory temporary file and durable POSIX syncs.
/// An unexpected result means no rename was committed. A value always means the target was replaced; inspect durable
/// before treating the directory entry as crash-durable.
expected<ntn_onboard_position_plan_state_store_result, std::string> store_ntn_onboard_position_plan_state_atomic(
    const std::string&                                path,
    const ntn_onboard_position_plan_persistent_state& state,
    ntn_onboard_position_plan_state_store_failpoint failpoint = ntn_onboard_position_plan_state_store_failpoint::none);

} // namespace srs_cu_cp
} // namespace srsran
