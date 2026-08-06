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

#include "ntn_onboard_runtime_mapping.h"
#include <algorithm>
#include <set>

using namespace srsran;
using namespace srsran::srs_cu_cp;

namespace {

bool identities_equal(const ntn_onboard_cell_identity& lhs, const ntn_onboard_cell_identity& rhs)
{
  return lhs.nci == rhs.nci && lhs.pci == rhs.pci;
}

std::vector<std::string> effective_assigned_positions(const ntn_versioned_position_plan& plan)
{
  if (plan.schema_version >= 3 || !plan.assigned_l1_position_ids.empty()) {
    return plan.assigned_l1_position_ids;
  }

  std::vector<std::string> result;
  result.reserve(plan.visible_l1_positions.size());
  for (const ntn_l1_position& position : plan.visible_l1_positions) {
    result.push_back(position.position_id);
  }
  return result;
}

} // namespace

const char* srsran::srs_cu_cp::to_string(ntn_onboard_runtime_mapping_stage stage)
{
  switch (stage) {
    case ntn_onboard_runtime_mapping_stage::disabled:
      return "disabled";
    case ntn_onboard_runtime_mapping_stage::awaiting_active_plan:
      return "awaiting_active_plan";
    case ntn_onboard_runtime_mapping_stage::awaiting_live_du:
      return "awaiting_live_du";
    case ntn_onboard_runtime_mapping_stage::ready:
      return "ready";
    case ntn_onboard_runtime_mapping_stage::stale:
      return "stale";
  }
  return "unknown";
}

const char* srsran::srs_cu_cp::to_string(ntn_onboard_tai_status status)
{
  switch (status) {
    case ntn_onboard_tai_status::ready:
      return "ready";
    case ntn_onboard_tai_status::supported_tai_missing:
      return "supported_tai_missing";
    case ntn_onboard_tai_status::supported_tai_duplicate:
      return "supported_tai_duplicate";
    case ntn_onboard_tai_status::plmn_tac_mismatch:
      return "plmn_tac_mismatch";
  }
  return "unknown";
}

const char* srsran::srs_cu_cp::to_string(ntn_position_transition transition)
{
  switch (transition) {
    case ntn_position_transition::unknown:
      return "unknown";
    case ntn_position_transition::no_change:
      return "no_change";
    case ntn_position_transition::same_cell:
      return "same_cell";
    case ntn_position_transition::cell_change:
      return "cell_change";
  }
  return "unknown";
}

ntn_onboard_tai_status
srsran::srs_cu_cp::classify_ntn_onboard_tai(span<const cu_cp_tai> supported_tais, const cu_cp_tai& cell_tai)
{
  unsigned exact_matches = 0;
  bool     plmn_present  = false;
  for (const cu_cp_tai& supported : supported_tais) {
    if (supported.plmn_id == cell_tai.plmn_id) {
      plmn_present = true;
      if (supported.tac == cell_tai.tac) {
        ++exact_matches;
      }
    }
  }
  if (exact_matches == 1) {
    return ntn_onboard_tai_status::ready;
  }
  if (exact_matches > 1) {
    return ntn_onboard_tai_status::supported_tai_duplicate;
  }
  return plmn_present ? ntn_onboard_tai_status::plmn_tac_mismatch
                      : ntn_onboard_tai_status::supported_tai_missing;
}

expected<std::shared_ptr<const ntn_onboard_runtime_mapping_snapshot>, std::string>
ntn_onboard_runtime_mapping_snapshot::create(
    const ntn_activated_position_plan&                       active_plan,
    const std::array<ntn_onboard_runtime_cell_route, 2>& live_cell_routes)
{
  const ntn_versioned_position_plan& source = active_plan.source;
  if (source.satellite_id.empty() || source.catalog_version == 0 || source.schedule_version == 0 ||
      source.content_hash.empty() || active_plan.calendar_hash.empty()) {
    return make_unexpected(std::string{"invalid_plan_identity"});
  }
  if (source.valid_from >= source.valid_until || source.activation_epoch < source.valid_from ||
      source.activation_epoch >= source.valid_until) {
    return make_unexpected(std::string{"invalid_plan_validity"});
  }

  auto result = std::shared_ptr<ntn_onboard_runtime_mapping_snapshot>(new ntn_onboard_runtime_mapping_snapshot);
  result->satellite             = source.satellite_id;
  result->catalog               = source.catalog_version;
  result->schedule              = source.schedule_version;
  result->source_content_hash   = source.content_hash;
  result->calendar_content_hash = active_plan.calendar_hash;
  result->validity_start        = source.valid_from;
  result->validity_end          = source.valid_until;
  result->activation            = source.activation_epoch;

  std::array<bool, 2> used_routes{};
  for (size_t cell_idx = 0; cell_idx != active_plan.cell_positions.size(); ++cell_idx) {
    const ntn_onboard_cell_identity& identity = active_plan.cell_positions[cell_idx].identity;

    size_t matching_source_identities = 0;
    for (const ntn_onboard_cell_identity& source_identity : source.onboard_cells) {
      matching_source_identities += identities_equal(source_identity, identity) ? 1U : 0U;
    }
    if (matching_source_identities != 1) {
      return make_unexpected(std::string{"cell_identity_mismatch"});
    }

    size_t route_idx = live_cell_routes.size();
    for (size_t candidate_idx = 0; candidate_idx != live_cell_routes.size(); ++candidate_idx) {
      if (identities_equal(live_cell_routes[candidate_idx].identity, identity)) {
        if (route_idx != live_cell_routes.size()) {
          return make_unexpected(std::string{"duplicate_cell_route"});
        }
        route_idx = candidate_idx;
      }
    }
    if (route_idx == live_cell_routes.size() || used_routes[route_idx]) {
      return make_unexpected(std::string{"cell_route_unavailable"});
    }

    const ntn_onboard_runtime_cell_route& route = live_cell_routes[route_idx];
    if (route.ncgi.nci != identity.nci) {
      return make_unexpected(std::string{"cell_route_ncgi_mismatch"});
    }
    if (!is_valid(identity.pci) || !is_valid(route.tac) || route.du_index == du_index_t::invalid ||
        route.du_cell_index == du_cell_index_t::invalid) {
      return make_unexpected(std::string{"invalid_cell_route"});
    }
    used_routes[route_idx] = true;
    result->routes[cell_idx] = route;
  }
  if (!used_routes[0] || !used_routes[1] || result->routes[0].identity.nci == result->routes[1].identity.nci) {
    return make_unexpected(std::string{"cell_route_unavailable"});
  }
  if (result->routes[0].du_index != result->routes[1].du_index) {
    return make_unexpected(std::string{"cross_du_mapping_not_supported"});
  }
  if (result->routes[0].du_connection_generation != result->routes[1].du_connection_generation) {
    return make_unexpected(std::string{"du_connection_generation_mismatch"});
  }

  std::map<std::string, const ntn_l1_position*, std::less<>> visible_positions;
  for (const ntn_l1_position& position : source.visible_l1_positions) {
    if (!visible_positions.emplace(position.position_id, &position).second) {
      return make_unexpected(std::string{"duplicate_visible_position"});
    }
  }

  const std::vector<std::string> expected_ids = effective_assigned_positions(source);
  std::set<std::string>          expected_unique;
  for (const std::string& position_id : expected_ids) {
    if (!expected_unique.insert(position_id).second) {
      return make_unexpected(std::string{"duplicate_expected_assignment"});
    }
    if (visible_positions.count(position_id) == 0) {
      return make_unexpected(std::string{"expected_assignment_not_visible"});
    }
  }

  std::set<std::string> mapped_ids;
  for (size_t cell_idx = 0; cell_idx != active_plan.cell_positions.size(); ++cell_idx) {
    const ntn_onboard_cell_position_set& assignment = active_plan.cell_positions[cell_idx];
    auto&                                cell_positions = result->positions_by_cell[cell_idx];
    cell_positions.reserve(assignment.assigned_l1_ids.size());
    for (const std::string& position_id : assignment.assigned_l1_ids) {
      auto visible = visible_positions.find(position_id);
      if (visible == visible_positions.end()) {
        return make_unexpected(std::string{"assigned_position_not_visible"});
      }
      if (expected_unique.count(position_id) == 0) {
        return make_unexpected(std::string{"unexpected_position_assignment"});
      }
      if (!mapped_ids.insert(position_id).second) {
        return make_unexpected(std::string{"duplicate_position_assignment"});
      }
      cell_positions.push_back({*visible->second, assignment.identity});
    }
    std::sort(cell_positions.begin(),
              cell_positions.end(),
              [](const ntn_onboard_runtime_position& lhs, const ntn_onboard_runtime_position& rhs) {
                return lhs.position.position_id < rhs.position.position_id;
              });
  }

  if (mapped_ids != expected_unique) {
    return make_unexpected(std::string{"assignment_set_mismatch"});
  }

  result->mapped_positions.reserve(mapped_ids.size());
  for (const auto& cell_positions : result->positions_by_cell) {
    result->mapped_positions.insert(result->mapped_positions.end(), cell_positions.begin(), cell_positions.end());
  }
  std::sort(result->mapped_positions.begin(),
            result->mapped_positions.end(),
            [](const ntn_onboard_runtime_position& lhs, const ntn_onboard_runtime_position& rhs) {
              return lhs.position.position_id < rhs.position.position_id;
            });
  for (size_t position_idx = 0; position_idx != result->mapped_positions.size(); ++position_idx) {
    result->position_index.emplace(result->mapped_positions[position_idx].position.position_id, position_idx);
  }

  return std::shared_ptr<const ntn_onboard_runtime_mapping_snapshot>{std::move(result)};
}

const ntn_onboard_runtime_position*
ntn_onboard_runtime_mapping_snapshot::find_position(std::string_view position_id) const
{
  auto found = position_index.find(position_id);
  return found == position_index.end() ? nullptr : &mapped_positions[found->second];
}

span<const ntn_onboard_runtime_position>
ntn_onboard_runtime_mapping_snapshot::positions_for_nci(nr_cell_identity nci) const
{
  for (size_t cell_idx = 0; cell_idx != routes.size(); ++cell_idx) {
    if (routes[cell_idx].identity.nci == nci) {
      return positions_by_cell[cell_idx];
    }
  }
  return {};
}

const ntn_onboard_runtime_cell_route*
ntn_onboard_runtime_mapping_snapshot::resolve_cell_route(nr_cell_identity nci) const
{
  for (const ntn_onboard_runtime_cell_route& route : routes) {
    if (route.identity.nci == nci) {
      return &route;
    }
  }
  return nullptr;
}

bool ntn_onboard_runtime_mapping_snapshot::matches_cell_routes(
    const std::array<ntn_onboard_runtime_cell_route, 2>& live_cell_routes) const
{
  for (const ntn_onboard_runtime_cell_route& expected : routes) {
    const auto live = std::find_if(live_cell_routes.begin(), live_cell_routes.end(), [&expected](const auto& route) {
      return identities_equal(route.identity, expected.identity);
    });
    if (live == live_cell_routes.end() || live->ncgi != expected.ncgi || live->tac != expected.tac ||
        live->du_index != expected.du_index || live->du_cell_index != expected.du_cell_index ||
        live->du_connection_generation != expected.du_connection_generation || live->tai_status != expected.tai_status) {
      return false;
    }
  }
  return true;
}

ntn_position_transition
ntn_onboard_runtime_mapping_snapshot::classify_position_transition(std::string_view old_position_id,
                                                                   std::string_view new_position_id) const
{
  const ntn_onboard_runtime_position* old_position = find_position(old_position_id);
  const ntn_onboard_runtime_position* new_position = find_position(new_position_id);
  if (old_position == nullptr || new_position == nullptr) {
    return ntn_position_transition::unknown;
  }
  if (old_position_id == new_position_id) {
    return ntn_position_transition::no_change;
  }
  return old_position->owner.nci == new_position->owner.nci ? ntn_position_transition::same_cell
                                                            : ntn_position_transition::cell_change;
}
