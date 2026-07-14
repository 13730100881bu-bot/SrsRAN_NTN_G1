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

#include "ntn_beam_assignment_repository.h"

using namespace srsran;
using namespace srs_cu_cp;

void ntn_beam_assignment_repository::update(const ntn_beam_placement_plan& plan)
{
  cached_assignments = plan.assignments;
  beam_id_to_assignment.clear();
  nci_to_assignment.clear();

  for (size_t i = 0; i != cached_assignments.size(); ++i) {
    const ntn_beam_du_assignment& assignment = cached_assignments[i];
    beam_id_to_assignment[assignment.beam_id] = i;
    nci_to_assignment[assignment.nci]         = i;
  }
}

std::optional<ntn_beam_du_assignment>
ntn_beam_assignment_repository::find_by_beam_id(const std::string& beam_id) const
{
  auto it = beam_id_to_assignment.find(beam_id);
  if (it == beam_id_to_assignment.end()) {
    return std::nullopt;
  }
  return cached_assignments[it->second];
}

std::optional<ntn_beam_du_assignment> ntn_beam_assignment_repository::find_by_nci(nr_cell_identity nci) const
{
  auto it = nci_to_assignment.find(nci);
  if (it == nci_to_assignment.end()) {
    return std::nullopt;
  }
  return cached_assignments[it->second];
}

bool ntn_beam_assignment_repository::is_beam_active(const std::string& beam_id) const
{
  auto assignment = find_by_beam_id(beam_id);
  return assignment.has_value() && assignment->state == ntn_beam_assignment_state::active_loaded;
}

std::optional<du_index_t> ntn_beam_assignment_repository::get_du_for_beam(const std::string& beam_id) const
{
  auto assignment = find_by_beam_id(beam_id);
  if (!assignment.has_value() || assignment->state != ntn_beam_assignment_state::active_loaded ||
      assignment->du_index == du_index_t::invalid) {
    return std::nullopt;
  }
  return assignment->du_index;
}

std::optional<du_index_t> ntn_beam_assignment_repository::get_du_for_nci(nr_cell_identity nci) const
{
  auto assignment = find_by_nci(nci);
  if (!assignment.has_value() || assignment->state != ntn_beam_assignment_state::active_loaded ||
      assignment->du_index == du_index_t::invalid) {
    return std::nullopt;
  }
  return assignment->du_index;
}
