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

#include "ntn_onboard_position_plan.h"
#include "srsran/adt/expected.h"
#include "srsran/adt/span.h"
#include "srsran/cu_cp/cu_cp_types.h"
#include "srsran/ran/nr_cgi.h"
#include "srsran/ran/tac.h"
#include <array>
#include <chrono>
#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace srsran {
namespace srs_cu_cp {

/// Availability of the CU-CP-private position-to-cell view.
enum class ntn_onboard_runtime_mapping_stage { disabled, awaiting_active_plan, awaiting_live_du, ready, stale };

/// Result of matching one DU cell's exact PLMN+TAC against the configured NGAP supported tracking areas.
/// This status controls core-location and Paging narrowing independently of the position-to-cell mapping.
enum class ntn_onboard_tai_status {
  ready,
  supported_tai_missing,
  supported_tai_duplicate,
  plmn_tac_mismatch
};

/// Classification of a position change within one immutable runtime snapshot.
enum class ntn_position_transition { unknown, no_change, same_cell, cell_change };

const char* to_string(ntn_onboard_runtime_mapping_stage stage);
const char* to_string(ntn_onboard_tai_status status);
const char* to_string(ntn_position_transition transition);

/// Classifies one cell's complete PLMN+TAC against the configured NGAP supported-TA entries.
ntn_onboard_tai_status classify_ntn_onboard_tai(span<const cu_cp_tai> supported_tais, const cu_cp_tai& cell_tai);

/// Copy of the stable DU cell route used by one runtime snapshot. It does not retain a DU context pointer.
struct ntn_onboard_runtime_cell_route {
  ntn_onboard_cell_identity identity;
  nr_cell_global_id_t       ncgi;
  tac_t                     tac                      = INVALID_TAC;
  du_index_t                du_index                 = du_index_t::invalid;
  du_cell_index_t           du_cell_index            = du_cell_index_t::invalid;
  uint64_t                  du_connection_generation = 0;
  ntn_onboard_tai_status    tai_status               = ntn_onboard_tai_status::supported_tai_missing;
};

/// One assigned Earth-fixed position and the stable onboard cell that currently owns it.
struct ntn_onboard_runtime_position {
  ntn_l1_position           position;
  ntn_onboard_cell_identity owner;
};

/// Immutable, CU-CP-private view built only from an active position plan and live DU reconciliation data.
/// The snapshot stores the assigned subset, not the complete visibility inventory.
class ntn_onboard_runtime_mapping_snapshot
{
public:
  static expected<std::shared_ptr<const ntn_onboard_runtime_mapping_snapshot>, std::string>
  create(const ntn_activated_position_plan&                       active_plan,
         const std::array<ntn_onboard_runtime_cell_route, 2>& live_cell_routes);

  const std::string& satellite_id() const { return satellite; }
  uint64_t           catalog_version() const { return catalog; }
  uint64_t           schedule_version() const { return schedule; }
  const std::string& source_hash() const { return source_content_hash; }
  const std::string& calendar_hash() const { return calendar_content_hash; }

  std::chrono::system_clock::time_point valid_from() const { return validity_start; }
  std::chrono::system_clock::time_point valid_until() const { return validity_end; }
  std::chrono::system_clock::time_point activation_epoch() const { return activation; }

  size_t nof_positions() const { return mapped_positions.size(); }

  span<const ntn_onboard_runtime_position> positions() const { return mapped_positions; }

  /// Returns nullptr when the position is not assigned by this snapshot.
  const ntn_onboard_runtime_position* find_position(std::string_view position_id) const;

  /// Returns all assigned positions for the NCI in deterministic position_id order.
  span<const ntn_onboard_runtime_position> positions_for_nci(nr_cell_identity nci) const;

  /// Returns nullptr when the NCI is not one of this snapshot's two stable cells.
  const ntn_onboard_runtime_cell_route* resolve_cell_route(nr_cell_identity nci) const;

  /// Returns true only when both copied DU cell routes still exactly match the live served-cell view.
  bool matches_cell_routes(const std::array<ntn_onboard_runtime_cell_route, 2>& live_cell_routes) const;

  ntn_position_transition classify_position_transition(std::string_view old_position_id,
                                                       std::string_view new_position_id) const;

  const std::array<ntn_onboard_runtime_cell_route, 2>& cell_routes() const { return routes; }

private:
  ntn_onboard_runtime_mapping_snapshot() = default;

  std::string satellite;
  uint64_t    catalog  = 0;
  uint64_t    schedule = 0;
  std::string source_content_hash;
  std::string calendar_content_hash;

  std::chrono::system_clock::time_point validity_start{};
  std::chrono::system_clock::time_point validity_end{};
  std::chrono::system_clock::time_point activation{};

  std::array<ntn_onboard_runtime_cell_route, 2> routes{};
  std::vector<ntn_onboard_runtime_position>     mapped_positions;
  std::array<std::vector<ntn_onboard_runtime_position>, 2> positions_by_cell;
  std::map<std::string, size_t, std::less<>>                 position_index;
};

} // namespace srs_cu_cp
} // namespace srsran
