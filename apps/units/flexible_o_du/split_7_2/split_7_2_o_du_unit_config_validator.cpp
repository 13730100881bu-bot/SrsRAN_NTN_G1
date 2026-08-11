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

#include "split_7_2_o_du_unit_config_validator.h"
#include "apps/units/flexible_o_du/o_du_high/o_du_high_unit_config_validator.h"
#include "apps/units/flexible_o_du/o_du_low/du_low_config_validator.h"
#include "apps/units/flexible_o_du/split_7_2/helpers/ru_ofh_config_validator.h"
#include "srsran/ran/prach/prach_configuration.h"
#include "srsran/ran/prach/prach_preamble_information.h"
#include "fmt/format.h"
#include <algorithm>
#include <iterator>
#include <optional>
#include <set>

using namespace srsran;

static std::vector<du_low_prach_validation_config> get_du_low_validation_dependencies(const du_high_unit_config& config)
{
  std::vector<du_low_prach_validation_config> out_cfg(config.cells_cfg.size());

  for (unsigned i = 0, e = config.cells_cfg.size(); i != e; ++i) {
    du_low_prach_validation_config&      out_cell = out_cfg[i];
    const du_high_unit_base_cell_config& in_cell  = config.cells_cfg[i].cell;

    // Get PRACH info.
    subcarrier_spacing  common_scs = in_cell.common_scs;
    prach_configuration prach_info = prach_configuration_get(band_helper::get_freq_range(in_cell.band.value()),
                                                             band_helper::get_duplex_mode(in_cell.band.value()),
                                                             in_cell.prach_cfg.prach_config_index.value());

    // PRACH format type.
    out_cell.format = prach_info.format;

    // Get preamble info.
    prach_preamble_information preamble_info =
        is_long_preamble(prach_info.format)
            ? get_prach_preamble_long_info(prach_info.format)
            : get_prach_preamble_short_info(prach_info.format, to_ra_subcarrier_spacing(common_scs), false);

    out_cell.prach_scs             = preamble_info.scs;
    out_cell.zero_correlation_zone = in_cell.prach_cfg.zero_correlation_zone;
    out_cell.nof_prach_ports       = in_cell.prach_cfg.ports.size();
    out_cell.nof_antennas_ul       = in_cell.nof_antennas_ul;
  }

  return out_cfg;
}

static std::vector<ru_ofh_cell_validation_config> get_ru_ofh_validation_dependencies(const du_high_unit_config& config)
{
  std::vector<ru_ofh_cell_validation_config> out_cfg(config.cells_cfg.size());

  for (unsigned i = 0, e = config.cells_cfg.size(); i != e; ++i) {
    ru_ofh_cell_validation_config&       out_cell = out_cfg[i];
    const du_high_unit_base_cell_config& in_cell  = config.cells_cfg[i].cell;

    // Validates the sampling rate is compatible with the PRACH sequence.
    out_cell.scs             = in_cell.common_scs;
    out_cell.nof_prach_ports = in_cell.prach_cfg.ports.size();
    out_cell.nof_antennas_dl = in_cell.nof_antennas_dl;
    out_cell.nof_antennas_ul = in_cell.nof_antennas_ul;
  }

  return out_cfg;
}

static bool validate_ntn_initial_ul_ofh_mapping(const du_high_unit_config& du_cfg, const ru_ofh_unit_config& ru_cfg)
{
  const auto& mapping = du_cfg.ntn_initial_ul_rx_mapping;
  if (!mapping.enabled) {
    return true;
  }
  std::vector<std::set<unsigned>> mapped_buffer_ports(du_cfg.cells_cfg.size());
  for (const auto& entry : mapping.entries) {
    if (entry.backend != "ofh") {
      fmt::print("Split 7.2 NTN Initial UL receive mappings must use the 'ofh' backend.\n");
      return false;
    }

    std::optional<unsigned> cell_index;
    for (unsigned i = 0; i != du_cfg.cells_cfg.size(); ++i) {
      const auto& cell = du_cfg.cells_cfg[i].cell;
      if (!cell.sector_id.has_value()) {
        continue;
      }
      const auto nci = nr_cell_identity::create(du_cfg.gnb_id, cell.sector_id.value());
      if (nci.has_value() && nci.value().value() == entry.nci) {
        cell_index = i;
        break;
      }
    }
    if (!cell_index.has_value() || cell_index.value() >= ru_cfg.cells.size()) {
      fmt::print("NTN Initial UL OFH mapping NCI {} does not identify a served DU cell.\n", entry.nci);
      return false;
    }

    const auto& cell_ports = du_cfg.cells_cfg[cell_index.value()].cell.prach_cfg.ports;
    const auto  port_it    = std::find(cell_ports.begin(), cell_ports.end(), entry.physical_rx_port);
    if (port_it == cell_ports.end()) {
      fmt::print("NTN Initial UL physical receive port {} is not a PRACH port of NCI {}.\n",
                 entry.physical_rx_port,
                 entry.nci);
      return false;
    }
    const unsigned buffer_port = static_cast<unsigned>(std::distance(cell_ports.begin(), port_it));
    const auto&    ru_cell     = ru_cfg.cells[cell_index.value()];
    if (buffer_port >= ru_cell.ru_prach_port_id.size() || !entry.prach_eaxc.has_value() ||
        entry.prach_eaxc.value() != ru_cell.ru_prach_port_id[buffer_port]) {
      fmt::print("NTN Initial UL OFH mapping for NCI {} requires the matching PRACH eAxC.\n",
                 entry.nci);
      return false;
    }
    mapped_buffer_ports[cell_index.value()].insert(buffer_port);
  }

  for (unsigned i = 0; i != mapped_buffer_ports.size(); ++i) {
    if (mapped_buffer_ports[i].empty()) {
      continue;
    }
    const auto& prach_ports = du_cfg.cells_cfg[i].cell.prach_cfg.ports;
    const auto& ru_cell     = ru_cfg.cells[i];
    if (mapped_buffer_ports[i].size() != prach_ports.size() ||
        mapped_buffer_ports[i].size() != ru_cell.ru_prach_port_id.size()) {
      fmt::print("NTN Initial UL OFH mapping for cell {} must cover every configured PRACH receive port/eAxC.\n", i);
      return false;
    }
  }
  return true;
}

bool srsran::validate_split_7_2_o_du_unit_config(const split_7_2_o_du_unit_config& config)
{
  if (!validate_o_du_high_config(config.odu_high_cfg)) {
    return false;
  }

  auto du_low_dependencies = get_du_low_validation_dependencies(config.odu_high_cfg.du_high_cfg.config);
  if (!validate_du_low_config(config.du_low_cfg, du_low_dependencies)) {
    return false;
  }

  auto ru_ofh_dependencies = get_ru_ofh_validation_dependencies(config.odu_high_cfg.du_high_cfg.config);
  return validate_ru_ofh_config(config.ru_cfg.config, ru_ofh_dependencies) &&
         validate_ntn_initial_ul_ofh_mapping(config.odu_high_cfg.du_high_cfg.config, config.ru_cfg.config);
}
