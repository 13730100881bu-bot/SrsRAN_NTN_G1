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

#include "lib/cu_cp/ntn_mobility/ntn_beam_placement_planner.h"
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace srsran {
namespace srs_cu_cp {

/// Runtime lookup table for the latest CU-CP NTN beam-to-DU placement plan.
class ntn_beam_assignment_repository
{
public:
  void update(const ntn_beam_placement_plan& plan);

  const std::vector<ntn_beam_du_assignment>& assignments() const { return cached_assignments; }

  std::optional<ntn_beam_du_assignment> find_by_beam_id(const std::string& beam_id) const;
  std::optional<ntn_beam_du_assignment> find_by_nci(nr_cell_identity nci) const;

  bool is_beam_active(const std::string& beam_id) const;

  std::optional<du_index_t> get_du_for_beam(const std::string& beam_id) const;
  std::optional<du_index_t> get_du_for_nci(nr_cell_identity nci) const;

private:
  std::vector<ntn_beam_du_assignment> cached_assignments;
  std::map<std::string, size_t>       beam_id_to_assignment;
  std::map<nr_cell_identity, size_t>  nci_to_assignment;
};

} // namespace srs_cu_cp
} // namespace srsran
