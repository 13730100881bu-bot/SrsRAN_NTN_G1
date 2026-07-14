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

#include "srsran/adt/expected.h"
#include "srsran/cu_cp/cell_meas_manager_config.h"
#include "srsran/ran/nr_cgi.h"
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace srsran {
namespace srs_cu_cp {

/// Static information for one neighbor-capable cell known by the CU-CP.
struct neighbor_cell_info {
  serving_cell_meas_config serving_cell_cfg;
  std::string              satellite_id; ///< NTN satellite that currently manages this cell.
  bool                     enabled = true;
};

/// Static Xn adjacency between two NTN satellites.
struct neighbor_satellite_xn_relation_config {
  std::string source_satellite_id;
  std::string target_satellite_id;
  bool        bidirectional    = true;
  bool        enabled          = true;
  bool        handover_allowed = true;
};

/// Static relation between a serving cell and one neighbor cell.
struct neighbor_cell_relation_config {
  nr_cell_identity             serving_nci;
  nr_cell_identity             neighbor_nci;
  std::vector<report_cfg_id_t> report_cfg_ids;
  bool                         enabled          = true;
  bool                         handover_allowed = true;
};

/// Static neighbor-cell information loaded from an external configuration file.
struct neighbor_cell_manager_config {
  unsigned                                    version = 1;
  std::string                                 region;
  std::vector<neighbor_cell_info>                    cells;
  std::vector<neighbor_satellite_xn_relation_config> xn_relations;
  std::vector<neighbor_cell_relation_config>         relations;
};

/// Query and merge helper for CU-CP neighbor-cell configuration.
class neighbor_cell_manager
{
public:
  neighbor_cell_manager() = default;
  explicit neighbor_cell_manager(const neighbor_cell_manager_config& cfg);

  std::optional<neighbor_cell_info> find_cell(nr_cell_identity nci) const;

  std::vector<neighbor_cell_relation_config> get_neighbors(nr_cell_identity serving_nci,
                                                           bool             handover_only = true) const;

  std::optional<neighbor_cell_relation_config> find_neighbor(nr_cell_identity serving_nci, pci_t neighbor_pci) const;

  bool has_xn_handover_relation(const std::string& source_satellite_id,
                                const std::string& target_satellite_id) const;

  /// Merge handover-allowed neighbor relations into the measurement-manager configuration.
  std::optional<std::string> merge_into(cell_meas_manager_cfg& meas_cfg) const;

private:
  bool is_relation_available(const neighbor_cell_relation_config& relation, bool handover_only) const;

  std::map<nr_cell_identity, neighbor_cell_info>     cells;
  std::vector<neighbor_satellite_xn_relation_config> xn_relations;
  std::vector<neighbor_cell_relation_config>         relations;
};

/// Parses a static neighbor-cell JSON document.
expected<neighbor_cell_manager_config, std::string> parse_neighbor_cell_info_json(const std::string& json_text);

/// Loads and parses a static neighbor-cell JSON file.
expected<neighbor_cell_manager_config, std::string> load_neighbor_cell_info_json_file(const std::string& path);

} // namespace srs_cu_cp
} // namespace srsran
