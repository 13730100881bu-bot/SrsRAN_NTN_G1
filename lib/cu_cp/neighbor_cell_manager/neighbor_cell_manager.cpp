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

#include "srsran/cu_cp/neighbor_cell_manager_config.h"
#include "fmt/format.h"
#include "nlohmann/json.hpp"
#include "srsran/ran/band_helper.h"
#include <algorithm>
#include <fstream>
#include <sstream>

using namespace srsran;
using namespace srs_cu_cp;

namespace {

expected<nr_cell_identity, std::string> parse_nci_value(const nlohmann::json& obj, const char* key, const char* ctx)
{
  const auto it = obj.find(key);
  if (it == obj.end()) {
    return make_unexpected(fmt::format("{}.{} is missing", ctx, key));
  }

  if (it->is_string()) {
    std::string nci_text = it->get<std::string>();
    if (nci_text.size() > 2 && nci_text[0] == '0' && (nci_text[1] == 'x' || nci_text[1] == 'X')) {
      nci_text.erase(0, 2);
    }
    auto nci = nr_cell_identity::parse_hex(nci_text);
    if (!nci.has_value()) {
      return make_unexpected(fmt::format("{}.{} has invalid hex value", ctx, key));
    }
    return nci.value();
  }

  if (it->is_number_unsigned()) {
    auto nci = nr_cell_identity::create(it->get<uint64_t>());
    if (!nci.has_value()) {
      return make_unexpected(fmt::format("{}.{} is outside the 36-bit NR cell identity range", ctx, key));
    }
    return nci.value();
  }

  if (it->is_number_integer() && it->get<int64_t>() >= 0) {
    auto nci = nr_cell_identity::create(static_cast<uint64_t>(it->get<int64_t>()));
    if (!nci.has_value()) {
      return make_unexpected(fmt::format("{}.{} is outside the 36-bit NR cell identity range", ctx, key));
    }
    return nci.value();
  }

  return make_unexpected(fmt::format("{}.{} must be an unsigned integer or hex string", ctx, key));
}

expected<unsigned, std::string> get_required_uint(const nlohmann::json& obj, const char* key, const char* ctx)
{
  const auto it = obj.find(key);
  if (it == obj.end()) {
    return make_unexpected(fmt::format("{}.{} is missing", ctx, key));
  }
  if (it->is_number_unsigned()) {
    return it->get<unsigned>();
  }
  if (it->is_number_integer() && it->get<int64_t>() >= 0) {
    return static_cast<unsigned>(it->get<int64_t>());
  }
  return make_unexpected(fmt::format("{}.{} must be an unsigned integer", ctx, key));
}

expected<std::optional<unsigned>, std::string> get_optional_uint(const nlohmann::json& obj,
                                                                 const char*           key,
                                                                 const char*           ctx)
{
  const auto it = obj.find(key);
  if (it == obj.end() || it->is_null()) {
    return std::optional<unsigned>{};
  }
  if (it->is_number_unsigned()) {
    return std::optional<unsigned>{it->get<unsigned>()};
  }
  if (it->is_number_integer() && it->get<int64_t>() >= 0) {
    return std::optional<unsigned>{static_cast<unsigned>(it->get<int64_t>())};
  }
  return make_unexpected(fmt::format("{}.{} must be an unsigned integer", ctx, key));
}

expected<bool, std::string> get_optional_bool(const nlohmann::json& obj, const char* key, bool default_value, const char* ctx)
{
  const auto it = obj.find(key);
  if (it == obj.end() || it->is_null()) {
    return default_value;
  }
  if (!it->is_boolean()) {
    return make_unexpected(fmt::format("{}.{} must be a boolean", ctx, key));
  }
  return it->get<bool>();
}

expected<std::string, std::string> get_required_string(const nlohmann::json& obj, const char* key, const char* ctx)
{
  const auto it = obj.find(key);
  if (it == obj.end()) {
    return make_unexpected(fmt::format("{}.{} is missing", ctx, key));
  }
  if (!it->is_string()) {
    return make_unexpected(fmt::format("{}.{} must be a string", ctx, key));
  }
  return it->get<std::string>();
}

expected<std::string, std::string> get_optional_string(const nlohmann::json& obj,
                                                       const char*           key,
                                                       const char*           ctx)
{
  const auto it = obj.find(key);
  if (it == obj.end() || it->is_null()) {
    return std::string{};
  }
  if (!it->is_string()) {
    return make_unexpected(fmt::format("{}.{} must be a string", ctx, key));
  }
  return it->get<std::string>();
}

expected<rrc_ssb_mtc, std::string> make_ssb_mtc(unsigned period, unsigned offset, unsigned duration, const char* ctx)
{
  if (period != 5 && period != 10 && period != 20 && period != 40 && period != 80 && period != 160) {
    return make_unexpected(fmt::format("{}.ssb_period must be one of 5, 10, 20, 40, 80 or 160", ctx));
  }
  if (offset >= period) {
    return make_unexpected(fmt::format("{}.ssb_offset must be smaller than ssb_period", ctx));
  }

  rrc_ssb_mtc ssb_mtc;
  ssb_mtc.periodicity_and_offset.periodicity = static_cast<rrc_periodicity_and_offset::periodicity_t>(period);
  ssb_mtc.periodicity_and_offset.offset      = static_cast<uint8_t>(offset);
  ssb_mtc.dur                                = static_cast<uint8_t>(duration);
  return ssb_mtc;
}

expected<std::vector<report_cfg_id_t>, std::string> parse_report_cfg_ids(const nlohmann::json& relation_json,
                                                                         const char*           ctx)
{
  const auto it = relation_json.find("report_cfg_ids");
  if (it == relation_json.end()) {
    return std::vector<report_cfg_id_t>{};
  }
  if (!it->is_array()) {
    return make_unexpected(fmt::format("{}.report_cfg_ids must be an array", ctx));
  }

  std::vector<report_cfg_id_t> report_cfg_ids;
  for (size_t i = 0; i != it->size(); ++i) {
    const auto& value = it->at(i);
    unsigned    report_id;
    if (value.is_number_unsigned()) {
      report_id = value.get<unsigned>();
    } else if (value.is_number_integer() && value.get<int64_t>() >= 0) {
      report_id = static_cast<unsigned>(value.get<int64_t>());
    } else {
      return make_unexpected(fmt::format("{}.report_cfg_ids[{}] must be an unsigned integer", ctx, i));
    }
    if (report_id >= MAX_NOF_REPORT_CFG) {
      return make_unexpected(fmt::format("{}.report_cfg_ids[{}] is outside the supported range [0, {})",
                                         ctx,
                                         i,
                                         MAX_NOF_REPORT_CFG));
    }
    report_cfg_ids.push_back(uint_to_report_cfg_id(static_cast<uint8_t>(report_id)));
  }

  return report_cfg_ids;
}

expected<neighbor_cell_info, std::string> parse_cell(const nlohmann::json& cell_json, size_t index)
{
  if (!cell_json.is_object()) {
    return make_unexpected(fmt::format("cells[{}] must be an object", index));
  }

  const std::string ctx = fmt::format("cells[{}]", index);

  neighbor_cell_info cell;
  auto               nci = parse_nci_value(cell_json, "nci", ctx.c_str());
  if (!nci.has_value()) {
    return make_unexpected(nci.error());
  }
  cell.serving_cell_cfg.nci = nci.value();

  auto satellite_id = get_optional_string(cell_json, "satellite_id", ctx.c_str());
  if (!satellite_id.has_value()) {
    return make_unexpected(satellite_id.error());
  }
  cell.satellite_id = satellite_id.value();

  auto gnb_id_bit_length = get_required_uint(cell_json, "gnb_id_bit_length", ctx.c_str());
  if (!gnb_id_bit_length.has_value()) {
    return make_unexpected(gnb_id_bit_length.error());
  }
  if (gnb_id_bit_length.value() < 22 || gnb_id_bit_length.value() > 32) {
    return make_unexpected(fmt::format("{}.gnb_id_bit_length must be within [22, 32]", ctx));
  }
  cell.serving_cell_cfg.gnb_id_bit_length = gnb_id_bit_length.value();

  if (cell_json.contains("plmn")) {
    if (!cell_json.at("plmn").is_string()) {
      return make_unexpected(fmt::format("{}.plmn must be a string", ctx));
    }
    auto plmn = plmn_identity::parse(cell_json.at("plmn").get<std::string>());
    if (!plmn.has_value()) {
      return make_unexpected(fmt::format("{}.plmn is invalid", ctx));
    }
    cell.serving_cell_cfg.plmn = plmn.value();
  }

  auto pci = get_optional_uint(cell_json, "pci", ctx.c_str());
  if (!pci.has_value()) {
    return make_unexpected(pci.error());
  }
  if (pci.value().has_value()) {
    if (pci.value().value() > MAX_PCI) {
      return make_unexpected(fmt::format("{}.pci must be within [0, {}]", ctx, MAX_PCI));
    }
    cell.serving_cell_cfg.pci = static_cast<pci_t>(pci.value().value());
  }

  auto band = get_optional_uint(cell_json, "band", ctx.c_str());
  if (!band.has_value()) {
    return make_unexpected(band.error());
  }
  if (band.value().has_value()) {
    cell.serving_cell_cfg.band = uint_to_nr_band(band.value().value());
  }

  auto ssb_arfcn = get_optional_uint(cell_json, "ssb_arfcn", ctx.c_str());
  if (!ssb_arfcn.has_value()) {
    return make_unexpected(ssb_arfcn.error());
  }
  cell.serving_cell_cfg.ssb_arfcn = ssb_arfcn.value();

  auto ssb_scs = get_optional_uint(cell_json, "ssb_scs", ctx.c_str());
  if (!ssb_scs.has_value()) {
    return make_unexpected(ssb_scs.error());
  }
  if (ssb_scs.value().has_value()) {
    const subcarrier_spacing scs = to_subcarrier_spacing(std::to_string(ssb_scs.value().value()));
    if (!is_scs_valid(scs)) {
      return make_unexpected(fmt::format("{}.ssb_scs is invalid", ctx));
    }
    cell.serving_cell_cfg.ssb_scs = scs;
  }

  auto ssb_period   = get_optional_uint(cell_json, "ssb_period", ctx.c_str());
  auto ssb_offset   = get_optional_uint(cell_json, "ssb_offset", ctx.c_str());
  auto ssb_duration = get_optional_uint(cell_json, "ssb_duration", ctx.c_str());
  if (!ssb_period.has_value()) {
    return make_unexpected(ssb_period.error());
  }
  if (!ssb_offset.has_value()) {
    return make_unexpected(ssb_offset.error());
  }
  if (!ssb_duration.has_value()) {
    return make_unexpected(ssb_duration.error());
  }
  const bool has_any_ssb_mtc =
      ssb_period.value().has_value() || ssb_offset.value().has_value() || ssb_duration.value().has_value();
  const bool has_all_ssb_mtc =
      ssb_period.value().has_value() && ssb_offset.value().has_value() && ssb_duration.value().has_value();
  if (has_any_ssb_mtc != has_all_ssb_mtc) {
    return make_unexpected(fmt::format(
        "{}.ssb_period, ssb_offset and ssb_duration must be configured together", ctx));
  }
  if (has_all_ssb_mtc) {
    auto ssb_mtc =
        make_ssb_mtc(ssb_period.value().value(), ssb_offset.value().value(), ssb_duration.value().value(), ctx.c_str());
    if (!ssb_mtc.has_value()) {
      return make_unexpected(ssb_mtc.error());
    }
    cell.serving_cell_cfg.ssb_mtc = ssb_mtc.value();
  }

  auto enabled = get_optional_bool(cell_json, "enabled", true, ctx.c_str());
  if (!enabled.has_value()) {
    return make_unexpected(enabled.error());
  }
  cell.enabled = enabled.value();

  return cell;
}

expected<neighbor_satellite_xn_relation_config, std::string> parse_xn_relation(const nlohmann::json& relation_json,
                                                                               size_t               index)
{
  if (!relation_json.is_object()) {
    return make_unexpected(fmt::format("xn_relations[{}] must be an object", index));
  }

  const std::string ctx = fmt::format("xn_relations[{}]", index);

  neighbor_satellite_xn_relation_config relation;
  auto source_satellite_id = get_required_string(relation_json, "source_satellite_id", ctx.c_str());
  if (!source_satellite_id.has_value()) {
    return make_unexpected(source_satellite_id.error());
  }
  relation.source_satellite_id = source_satellite_id.value();

  auto target_satellite_id = get_required_string(relation_json, "target_satellite_id", ctx.c_str());
  if (!target_satellite_id.has_value()) {
    return make_unexpected(target_satellite_id.error());
  }
  relation.target_satellite_id = target_satellite_id.value();

  if (relation.source_satellite_id.empty() || relation.target_satellite_id.empty()) {
    return make_unexpected(fmt::format("{}.source_satellite_id and target_satellite_id must not be empty", ctx));
  }
  if (relation.source_satellite_id == relation.target_satellite_id) {
    return make_unexpected(fmt::format("{}.target_satellite_id must differ from source_satellite_id", ctx));
  }

  auto bidirectional = get_optional_bool(relation_json, "bidirectional", true, ctx.c_str());
  if (!bidirectional.has_value()) {
    return make_unexpected(bidirectional.error());
  }
  relation.bidirectional = bidirectional.value();

  auto enabled = get_optional_bool(relation_json, "enabled", true, ctx.c_str());
  if (!enabled.has_value()) {
    return make_unexpected(enabled.error());
  }
  relation.enabled = enabled.value();

  auto handover_allowed = get_optional_bool(relation_json, "handover_allowed", true, ctx.c_str());
  if (!handover_allowed.has_value()) {
    return make_unexpected(handover_allowed.error());
  }
  relation.handover_allowed = handover_allowed.value();

  return relation;
}

expected<neighbor_cell_relation_config, std::string> parse_relation(const nlohmann::json& relation_json, size_t index)
{
  if (!relation_json.is_object()) {
    return make_unexpected(fmt::format("relations[{}] must be an object", index));
  }

  const std::string ctx = fmt::format("relations[{}]", index);

  neighbor_cell_relation_config relation;
  auto serving_nci = parse_nci_value(relation_json, "serving_nci", ctx.c_str());
  if (!serving_nci.has_value()) {
    return make_unexpected(serving_nci.error());
  }
  relation.serving_nci = serving_nci.value();

  auto neighbor_nci = parse_nci_value(relation_json, "neighbor_nci", ctx.c_str());
  if (!neighbor_nci.has_value()) {
    return make_unexpected(neighbor_nci.error());
  }
  relation.neighbor_nci = neighbor_nci.value();

  if (relation.serving_nci == relation.neighbor_nci) {
    return make_unexpected(fmt::format("{}.neighbor_nci must differ from serving_nci", ctx));
  }

  auto report_cfg_ids = parse_report_cfg_ids(relation_json, ctx.c_str());
  if (!report_cfg_ids.has_value()) {
    return make_unexpected(report_cfg_ids.error());
  }
  relation.report_cfg_ids = report_cfg_ids.value();

  auto enabled = get_optional_bool(relation_json, "enabled", true, ctx.c_str());
  if (!enabled.has_value()) {
    return make_unexpected(enabled.error());
  }
  relation.enabled = enabled.value();

  auto handover_allowed = get_optional_bool(relation_json, "handover_allowed", true, ctx.c_str());
  if (!handover_allowed.has_value()) {
    return make_unexpected(handover_allowed.error());
  }
  relation.handover_allowed = handover_allowed.value();

  return relation;
}

void merge_cell_info(serving_cell_meas_config& target, const serving_cell_meas_config& source)
{
  target.nci = source.nci;
  if (target.gnb_id_bit_length < 22 || target.gnb_id_bit_length > 32) {
    target.gnb_id_bit_length = source.gnb_id_bit_length;
  }
  target.plmn = source.plmn;
  if (!target.pci.has_value()) {
    target.pci = source.pci;
  }
  if (!target.band.has_value()) {
    target.band = source.band;
  }
  if (!target.ssb_mtc.has_value()) {
    target.ssb_mtc = source.ssb_mtc;
  }
  if (!target.ssb_arfcn.has_value()) {
    target.ssb_arfcn = source.ssb_arfcn;
  }
  if (!target.ssb_scs.has_value()) {
    target.ssb_scs = source.ssb_scs;
  }
}

} // namespace

neighbor_cell_manager::neighbor_cell_manager(const neighbor_cell_manager_config& cfg)
{
  for (const auto& cell : cfg.cells) {
    if (cell.enabled) {
      cells[cell.serving_cell_cfg.nci] = cell;
    }
  }

  for (const auto& relation : cfg.xn_relations) {
    if (relation.enabled) {
      xn_relations.push_back(relation);
    }
  }

  for (const auto& relation : cfg.relations) {
    if (relation.enabled) {
      relations.push_back(relation);
    }
  }
}

std::optional<neighbor_cell_info> neighbor_cell_manager::find_cell(nr_cell_identity nci) const
{
  const auto it = cells.find(nci);
  if (it == cells.end()) {
    return std::nullopt;
  }
  return it->second;
}

bool neighbor_cell_manager::has_xn_handover_relation(const std::string& source_satellite_id,
                                                     const std::string& target_satellite_id) const
{
  if (source_satellite_id.empty() || target_satellite_id.empty()) {
    return false;
  }
  if (source_satellite_id == target_satellite_id) {
    return true;
  }

  return std::find_if(xn_relations.begin(),
                      xn_relations.end(),
                      [&source_satellite_id, &target_satellite_id](
                          const neighbor_satellite_xn_relation_config& relation) {
                        if (!relation.enabled || !relation.handover_allowed) {
                          return false;
                        }
                        const bool forward_match = relation.source_satellite_id == source_satellite_id &&
                                                   relation.target_satellite_id == target_satellite_id;
                        const bool reverse_match = relation.bidirectional &&
                                                   relation.source_satellite_id == target_satellite_id &&
                                                   relation.target_satellite_id == source_satellite_id;
                        return forward_match || reverse_match;
                      }) != xn_relations.end();
}

bool neighbor_cell_manager::is_relation_available(const neighbor_cell_relation_config& relation, bool handover_only) const
{
  if (handover_only && !relation.handover_allowed) {
    return false;
  }

  const auto serving_cell_it  = cells.find(relation.serving_nci);
  const auto neighbor_cell_it = cells.find(relation.neighbor_nci);
  if (serving_cell_it == cells.end() || neighbor_cell_it == cells.end()) {
    return true;
  }

  const std::string& source_satellite_id = serving_cell_it->second.satellite_id;
  const std::string& target_satellite_id = neighbor_cell_it->second.satellite_id;
  if (source_satellite_id.empty() && target_satellite_id.empty()) {
    return true;
  }
  if (source_satellite_id.empty() || target_satellite_id.empty()) {
    return false;
  }
  if (!handover_only && source_satellite_id == target_satellite_id) {
    return true;
  }

  return has_xn_handover_relation(source_satellite_id, target_satellite_id);
}

std::vector<neighbor_cell_relation_config> neighbor_cell_manager::get_neighbors(nr_cell_identity serving_nci,
                                                                                bool handover_only) const
{
  std::vector<neighbor_cell_relation_config> result;
  for (const auto& relation : relations) {
    if (relation.serving_nci == serving_nci && is_relation_available(relation, handover_only)) {
      result.push_back(relation);
    }
  }
  return result;
}

std::optional<neighbor_cell_relation_config> neighbor_cell_manager::find_neighbor(nr_cell_identity serving_nci,
                                                                                  pci_t neighbor_pci) const
{
  for (const auto& relation : get_neighbors(serving_nci)) {
    const auto cell_it = cells.find(relation.neighbor_nci);
    if (cell_it != cells.end() && cell_it->second.serving_cell_cfg.pci.has_value() &&
        cell_it->second.serving_cell_cfg.pci.value() == neighbor_pci) {
      return relation;
    }
  }
  return std::nullopt;
}

std::optional<std::string> neighbor_cell_manager::merge_into(cell_meas_manager_cfg& meas_cfg) const
{
  for (const auto& cell : cells) {
    auto meas_cell_it = meas_cfg.cells.find(cell.first);
    if (meas_cell_it == meas_cfg.cells.end()) {
      cell_meas_config new_cell_cfg;
      new_cell_cfg.serving_cell_cfg = cell.second.serving_cell_cfg;
      meas_cfg.cells.emplace(cell.first, new_cell_cfg);
      continue;
    }
    merge_cell_info(meas_cell_it->second.serving_cell_cfg, cell.second.serving_cell_cfg);
  }

  for (const auto& relation : relations) {
    if (relation.report_cfg_ids.empty() || !is_relation_available(relation, true)) {
      continue;
    }
    if (meas_cfg.cells.find(relation.serving_nci) == meas_cfg.cells.end()) {
      return fmt::format("relation serving_nci={:#x} has no cell configuration", relation.serving_nci);
    }
    if (meas_cfg.cells.find(relation.neighbor_nci) == meas_cfg.cells.end()) {
      return fmt::format("relation neighbor_nci={:#x} has no cell configuration", relation.neighbor_nci);
    }
    for (const auto& report_cfg_id : relation.report_cfg_ids) {
      if (meas_cfg.report_config_ids.find(report_cfg_id) == meas_cfg.report_config_ids.end()) {
        return fmt::format("relation serving_nci={:#x} neighbor_nci={:#x} references unknown report_cfg_id={}",
                           relation.serving_nci,
                           relation.neighbor_nci,
                           report_cfg_id_to_uint(report_cfg_id));
      }
    }

    auto& serving_cell = meas_cfg.cells.at(relation.serving_nci);
    auto  ncell_it =
        std::find_if(serving_cell.ncells.begin(),
                     serving_cell.ncells.end(),
                     [&relation](const neighbor_cell_meas_config& ncell) { return ncell.nci == relation.neighbor_nci; });
    if (ncell_it == serving_cell.ncells.end()) {
      serving_cell.ncells.push_back({relation.neighbor_nci, relation.report_cfg_ids});
      continue;
    }

    for (const auto& report_cfg_id : relation.report_cfg_ids) {
      if (std::find(ncell_it->report_cfg_ids.begin(), ncell_it->report_cfg_ids.end(), report_cfg_id) ==
          ncell_it->report_cfg_ids.end()) {
        ncell_it->report_cfg_ids.push_back(report_cfg_id);
      }
    }
  }

  return std::nullopt;
}

expected<neighbor_cell_manager_config, std::string> srsran::srs_cu_cp::parse_neighbor_cell_info_json(
    const std::string& json_text)
{
  nlohmann::json json;
  try {
    json = nlohmann::json::parse(json_text);
  } catch (const nlohmann::json::parse_error& e) {
    return make_unexpected(fmt::format("invalid neighbor cell JSON: {}", e.what()));
  }

  if (!json.is_object()) {
    return make_unexpected("neighbor cell JSON root must be an object");
  }

  neighbor_cell_manager_config cfg;
  if (json.contains("version")) {
    if (json.at("version").is_number_unsigned()) {
      cfg.version = json.at("version").get<unsigned>();
    } else if (json.at("version").is_number_integer() && json.at("version").get<int64_t>() >= 0) {
      cfg.version = static_cast<unsigned>(json.at("version").get<int64_t>());
    } else {
      return make_unexpected("version must be an unsigned integer");
    }
  }
  if (json.contains("region")) {
    if (!json.at("region").is_string()) {
      return make_unexpected("region must be a string");
    }
    cfg.region = json.at("region").get<std::string>();
  }

  const auto cells_it = json.find("cells");
  if (cells_it != json.end()) {
    if (!cells_it->is_array()) {
      return make_unexpected("cells must be an array");
    }
    for (size_t i = 0; i != cells_it->size(); ++i) {
      auto cell = parse_cell(cells_it->at(i), i);
      if (!cell.has_value()) {
        return make_unexpected(cell.error());
      }
      if (std::find_if(cfg.cells.begin(),
                       cfg.cells.end(),
                       [&cell](const neighbor_cell_info& existing_cell) {
                         return existing_cell.serving_cell_cfg.nci == cell.value().serving_cell_cfg.nci;
                       }) != cfg.cells.end()) {
        return make_unexpected(fmt::format("duplicate cell nci={:#x}", cell.value().serving_cell_cfg.nci));
      }
      cfg.cells.push_back(cell.value());
    }
  }

  const auto xn_relations_it = json.find("xn_relations");
  if (xn_relations_it != json.end()) {
    if (!xn_relations_it->is_array()) {
      return make_unexpected("xn_relations must be an array");
    }
    for (size_t i = 0; i != xn_relations_it->size(); ++i) {
      auto relation = parse_xn_relation(xn_relations_it->at(i), i);
      if (!relation.has_value()) {
        return make_unexpected(relation.error());
      }
      cfg.xn_relations.push_back(relation.value());
    }
  }

  const auto relations_it = json.find("relations");
  if (relations_it == json.end()) {
    return make_unexpected("relations is missing");
  }
  if (!relations_it->is_array()) {
    return make_unexpected("relations must be an array");
  }
  for (size_t i = 0; i != relations_it->size(); ++i) {
    auto relation = parse_relation(relations_it->at(i), i);
    if (!relation.has_value()) {
      return make_unexpected(relation.error());
    }
    cfg.relations.push_back(relation.value());
  }

  return cfg;
}

expected<neighbor_cell_manager_config, std::string> srsran::srs_cu_cp::load_neighbor_cell_info_json_file(
    const std::string& path)
{
  std::ifstream input(path);
  if (!input.is_open()) {
    return make_unexpected(fmt::format("failed to open neighbor cell JSON file '{}'", path));
  }

  std::ostringstream buffer;
  buffer << input.rdbuf();
  return parse_neighbor_cell_info_json(buffer.str());
}
