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

#include "dynamic_o_du_unit_config_validator.h"
#include "apps/units/flexible_o_du/o_du_high/o_du_high_unit_config_validator.h"
#include "apps/units/flexible_o_du/o_du_low/du_low_config_validator.h"
#include "apps/units/flexible_o_du/split_7_2/helpers/ru_ofh_config_validator.h"
#include "apps/units/flexible_o_du/split_8/helpers/ru_sdr_config_validator.h"
#include "srsran/ran/prach/prach_configuration.h"
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

static std::vector<ru_sdr_cell_validation_config> get_ru_sdr_validation_dependencies(const du_high_unit_config& config)
{
  std::vector<ru_sdr_cell_validation_config> out_cfg(config.cells_cfg.size());

  for (unsigned i = 0, e = config.cells_cfg.size(); i != e; ++i) {
    ru_sdr_cell_validation_config&       out_cell = out_cfg[i];
    const du_high_unit_base_cell_config& in_cell  = config.cells_cfg[i].cell;

    // Validates the sampling rate is compatible with the PRACH sequence.
    out_cell.common_scs            = in_cell.common_scs;
    prach_configuration prach_info = prach_configuration_get(band_helper::get_freq_range(in_cell.band.value()),
                                                             band_helper::get_duplex_mode(in_cell.band.value()),
                                                             in_cell.prach_cfg.prach_config_index.value());
    out_cell.prach_format          = prach_info.format;
    out_cell.channel_bw_mhz        = in_cell.channel_bw_mhz;
    out_cell.dplx_mode             = band_helper::get_duplex_mode(in_cell.band.value());
    out_cell.preamble_info =
        is_long_preamble(prach_info.format)
            ? get_prach_preamble_long_info(prach_info.format)
            : get_prach_preamble_short_info(prach_info.format, to_ra_subcarrier_spacing(in_cell.common_scs), false);
  }

  return out_cfg;
}

static bool validate_dynamic_ntn_rx_mapping(const du_high_unit_config& du_cfg,
                                            const ru_ofh_unit_config*  ofh_cfg,
                                            bool                       is_sdr)
{
  const auto& mapping = du_cfg.ntn_initial_ul_rx_mapping;
  if (!mapping.enabled) {
    return true;
  }
  const char* expected_backend = is_sdr ? "sdr" : "ofh";
  std::vector<std::set<unsigned>> mapped_buffer_ports(du_cfg.cells_cfg.size());
  for (const auto& entry : mapping.entries) {
    if (entry.backend != expected_backend) {
      fmt::print("Dynamic O-DU NTN Initial UL receive mapping backend must match the selected RU type '{}'.\n",
                 expected_backend);
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
    if (!cell_index.has_value()) {
      fmt::print("Dynamic O-DU NTN Initial UL mapping NCI {} is not served.\n", entry.nci);
      return false;
    }
    const auto& ports = du_cfg.cells_cfg[cell_index.value()].cell.prach_cfg.ports;
    const auto  port  = std::find(ports.begin(), ports.end(), entry.physical_rx_port);
    if (port == ports.end()) {
      fmt::print("Dynamic O-DU NTN Initial UL mapping physical receive port is not configured for PRACH.\n");
      return false;
    }
    if (!is_sdr) {
      if (ofh_cfg == nullptr || cell_index.value() >= ofh_cfg->cells.size()) {
        return false;
      }
      const unsigned buffer_port = static_cast<unsigned>(std::distance(ports.begin(), port));
      const auto&    ru_cell     = ofh_cfg->cells[cell_index.value()];
      if (buffer_port >= ru_cell.ru_prach_port_id.size() || !entry.prach_eaxc.has_value() ||
          entry.prach_eaxc.value() != ru_cell.ru_prach_port_id[buffer_port]) {
        fmt::print("Dynamic OFH NTN Initial UL mapping requires the matching PRACH eAxC.\n");
        return false;
      }
      mapped_buffer_ports[cell_index.value()].insert(buffer_port);
    }
  }
  if (!is_sdr) {
    for (unsigned i = 0; i != mapped_buffer_ports.size(); ++i) {
      if (mapped_buffer_ports[i].empty()) {
        continue;
      }
      const auto& prach_ports = du_cfg.cells_cfg[i].cell.prach_cfg.ports;
      const auto& ru_cell     = ofh_cfg->cells[i];
      if (mapped_buffer_ports[i].size() != prach_ports.size() ||
          mapped_buffer_ports[i].size() != ru_cell.ru_prach_port_id.size()) {
        fmt::print("Dynamic OFH NTN Initial UL mapping for cell {} must cover every PRACH receive port/eAxC.\n", i);
        return false;
      }
    }
  }
  return true;
}

bool srsran::validate_dynamic_o_du_unit_config(const dynamic_o_du_unit_config& config)
{
  if (!validate_o_du_high_config(config.odu_high_cfg)) {
    return false;
  }

  auto du_low_dependencies = get_du_low_validation_dependencies(config.odu_high_cfg.du_high_cfg.config);
  if (!validate_du_low_config(config.du_low_cfg, du_low_dependencies)) {
    return false;
  }

  if (auto* ru_ofh = std::get_if<ru_ofh_unit_parsed_config>(&config.ru_cfg)) {
    auto ru_ofh_dependencies = get_ru_ofh_validation_dependencies(config.odu_high_cfg.du_high_cfg.config);
    return validate_ru_ofh_config(ru_ofh->config, ru_ofh_dependencies) &&
           validate_dynamic_ntn_rx_mapping(config.odu_high_cfg.du_high_cfg.config, &ru_ofh->config, false);
  }

  if (auto* ru_sdr = std::get_if<ru_sdr_unit_config>(&config.ru_cfg)) {
    auto ru_sdr_dependencies = get_ru_sdr_validation_dependencies(config.odu_high_cfg.du_high_cfg.config);
    return validate_ru_sdr_config(*ru_sdr, ru_sdr_dependencies) &&
           validate_dynamic_ntn_rx_mapping(config.odu_high_cfg.du_high_cfg.config, nullptr, true);
  }

  if (config.odu_high_cfg.du_high_cfg.config.ntn_initial_ul_rx_mapping.enabled) {
    fmt::print("Dynamic O-DU NTN Initial UL receive mapping is unavailable with the dummy RU.\n");
    return false;
  }
  return true;
}
