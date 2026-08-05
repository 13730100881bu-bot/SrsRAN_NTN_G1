/*
 * Copyright 2021-2026 Software Radio Systems Limited
 *
 * This file is part of srsRAN.
 */

#pragma once

#include "ntn_onboard_position_plan_state.h"
#include "srsran/adt/expected.h"
#include <optional>
#include <string>

namespace srsran {
namespace srs_cu_cp {

inline constexpr size_t max_ntn_plan_version_anchor_file_size = 64U * 1024U;

/// Authenticated management-plan identity protected by the separate software-only anchor file.
struct ntn_plan_version_identity {
  uint64_t    catalog_version  = 0;
  uint64_t    schedule_version = 0;
  std::string content_hash;
  std::string key_id;
};

struct ntn_plan_version_reservation {
  ntn_plan_version_identity identity;
  uint64_t                  target_state_generation = 0;
};

/// Private software-only monotonic record. Its independent file catches ordinary state-file rollback.
struct ntn_plan_version_anchor_state {
  static constexpr unsigned current_schema_version = 1;

  unsigned                                    schema_version = current_schema_version;
  uint64_t                                    generation     = 0;
  std::string                                 anchor_hash;
  std::string                                 satellite_id;
  ntn_onboard_position_plan_state_context     planning_context;
  std::array<ntn_onboard_cell_identity, 2>    onboard_cells{};
  std::optional<ntn_plan_version_identity>    committed;
  std::optional<ntn_plan_version_reservation> reserved;
};

enum class ntn_plan_version_reserve_outcome { reserved, already_reserved, already_committed };

expected<std::optional<ntn_plan_version_anchor_state>, std::string>
load_ntn_plan_version_anchor(const std::string& path);

expected<std::string, std::string>
store_ntn_plan_version_anchor_atomic(const std::string& path, const ntn_plan_version_anchor_state& state);

expected<ntn_plan_version_reserve_outcome, std::string>
reserve_ntn_plan_version(ntn_plan_version_anchor_state& state,
                         const ntn_plan_version_identity& identity,
                         uint64_t target_state_generation);

expected<void, std::string>
commit_ntn_plan_version(ntn_plan_version_anchor_state& state,
                        const ntn_plan_version_identity& identity,
                        uint64_t target_state_generation);

expected<void, std::string>
cancel_ntn_plan_version_reservation(ntn_plan_version_anchor_state& state,
                                    const ntn_plan_version_identity& identity,
                                    uint64_t target_state_generation);

bool ntn_plan_version_identity_equal(const ntn_plan_version_identity& lhs,
                                     const ntn_plan_version_identity& rhs);

ntn_position_plan_version_anchor_snapshot
make_ntn_plan_version_anchor_snapshot(const ntn_plan_version_identity& identity);

} // namespace srs_cu_cp
} // namespace srsran
