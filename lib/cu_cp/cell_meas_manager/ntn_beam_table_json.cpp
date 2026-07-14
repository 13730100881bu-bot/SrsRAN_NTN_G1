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

#include "srsran/cu_cp/cell_meas_manager_config.h"
#include "fmt/format.h"
#include "nlohmann/json.hpp"
#include <cmath>
#include <fstream>
#include <set>
#include <sstream>

using namespace srsran;
using namespace srs_cu_cp;

namespace {

expected<nr_cell_identity, std::string> parse_nci(const nlohmann::json& beam_json, size_t index)
{
  const auto nci_it = beam_json.find("nci");
  if (nci_it == beam_json.end()) {
    return make_unexpected(fmt::format("beams[{}].nci is missing", index));
  }

  if (nci_it->is_string()) {
    std::string nci_text = nci_it->get<std::string>();
    if (nci_text.size() > 2 && nci_text[0] == '0' && (nci_text[1] == 'x' || nci_text[1] == 'X')) {
      nci_text.erase(0, 2);
    }
    auto nci = nr_cell_identity::parse_hex(nci_text);
    if (!nci.has_value()) {
      return make_unexpected(fmt::format("beams[{}].nci has invalid hex value", index));
    }
    return nci.value();
  }

  if (nci_it->is_number_unsigned()) {
    auto nci = nr_cell_identity::create(nci_it->get<uint64_t>());
    if (!nci.has_value()) {
      return make_unexpected(fmt::format("beams[{}].nci is outside the 36-bit NR cell identity range", index));
    }
    return nci.value();
  }

  if (nci_it->is_number_integer() && nci_it->get<int64_t>() >= 0) {
    auto nci = nr_cell_identity::create(static_cast<uint64_t>(nci_it->get<int64_t>()));
    if (!nci.has_value()) {
      return make_unexpected(fmt::format("beams[{}].nci is outside the 36-bit NR cell identity range", index));
    }
    return nci.value();
  }

  return make_unexpected(fmt::format("beams[{}].nci must be an unsigned integer or hex string", index));
}

expected<std::string, std::string> get_required_string(const nlohmann::json& obj, const char* key, size_t index)
{
  const auto it = obj.find(key);
  if (it == obj.end()) {
    return make_unexpected(fmt::format("beams[{}].{} is missing", index, key));
  }
  if (!it->is_string()) {
    return make_unexpected(fmt::format("beams[{}].{} must be a string", index, key));
  }
  return it->get<std::string>();
}

expected<double, std::string> get_required_number(const nlohmann::json& obj, const char* key, size_t index)
{
  const auto it = obj.find(key);
  if (it == obj.end()) {
    return make_unexpected(fmt::format("beams[{}].{} is missing", index, key));
  }
  if (!it->is_number()) {
    return make_unexpected(fmt::format("beams[{}].{} must be a number", index, key));
  }

  const double value = it->get<double>();
  if (!std::isfinite(value)) {
    return make_unexpected(fmt::format("beams[{}].{} must be finite", index, key));
  }
  return value;
}

expected<ntn_beam_position, std::string> parse_beam(const nlohmann::json& beam_json, size_t index)
{
  if (!beam_json.is_object()) {
    return make_unexpected(fmt::format("beams[{}] must be an object", index));
  }

  ntn_beam_position beam;

  auto beam_id = get_required_string(beam_json, "beam_id", index);
  if (!beam_id.has_value()) {
    return make_unexpected(beam_id.error());
  }
  if (beam_id.value().empty()) {
    return make_unexpected(fmt::format("beams[{}].beam_id must not be empty", index));
  }
  beam.beam_id = beam_id.value();

  auto nci = parse_nci(beam_json, index);
  if (!nci.has_value()) {
    return make_unexpected(nci.error());
  }
  beam.nci = nci.value();

  auto latitude = get_required_number(beam_json, "center_latitude_deg", index);
  if (!latitude.has_value()) {
    return make_unexpected(latitude.error());
  }
  auto longitude = get_required_number(beam_json, "center_longitude_deg", index);
  if (!longitude.has_value()) {
    return make_unexpected(longitude.error());
  }
  auto radius = get_required_number(beam_json, "coverage_radius_m", index);
  if (!radius.has_value()) {
    return make_unexpected(radius.error());
  }
  if (latitude.value() < -90.0 || latitude.value() > 90.0) {
    return make_unexpected(fmt::format("beams[{}].center_latitude_deg must be within [-90, 90]", index));
  }
  if (longitude.value() < -180.0 || longitude.value() > 180.0) {
    return make_unexpected(fmt::format("beams[{}].center_longitude_deg must be within [-180, 180]", index));
  }
  if (radius.value() <= 0.0) {
    return make_unexpected(fmt::format("beams[{}].coverage_radius_m must be positive", index));
  }

  beam.center_latitude_deg  = latitude.value();
  beam.center_longitude_deg = longitude.value();
  beam.coverage_radius_m    = radius.value();

  if (beam_json.contains("enabled")) {
    if (!beam_json.at("enabled").is_boolean()) {
      return make_unexpected(fmt::format("beams[{}].enabled must be a boolean", index));
    }
    beam.enabled = beam_json.at("enabled").get<bool>();
  }

  return beam;
}

} // namespace

expected<ntn_beam_table_config, std::string> srsran::srs_cu_cp::parse_ntn_beam_table_json(
    const std::string& json_text)
{
  nlohmann::json json;
  try {
    json = nlohmann::json::parse(json_text);
  } catch (const nlohmann::json::parse_error& e) {
    return make_unexpected(fmt::format("invalid NTN beam table JSON: {}", e.what()));
  }

  if (!json.is_object()) {
    return make_unexpected("NTN beam table root must be an object");
  }

  ntn_beam_table_config table;
  if (json.contains("version")) {
    if (json.at("version").is_number_unsigned()) {
      table.version = json.at("version").get<unsigned>();
    } else if (json.at("version").is_number_integer() && json.at("version").get<int64_t>() >= 0) {
      table.version = static_cast<unsigned>(json.at("version").get<int64_t>());
    } else {
      return make_unexpected("version must be an unsigned integer");
    }
  }
  if (json.contains("region")) {
    if (!json.at("region").is_string()) {
      return make_unexpected("region must be a string");
    }
    table.region = json.at("region").get<std::string>();
  }
  if (json.contains("satellite_height_m")) {
    if (!json.at("satellite_height_m").is_number()) {
      return make_unexpected("satellite_height_m must be a number");
    }
    const double satellite_height_m = json.at("satellite_height_m").get<double>();
    if (!std::isfinite(satellite_height_m) || satellite_height_m <= 0.0) {
      return make_unexpected("satellite_height_m must be positive and finite");
    }
    table.satellite_height_m = satellite_height_m;
  }

  const auto beams_it = json.find("beams");
  if (beams_it == json.end()) {
    return make_unexpected("beams is missing");
  }
  if (!beams_it->is_array()) {
    return make_unexpected("beams must be an array");
  }

  std::set<std::string>     beam_ids;
  std::set<nr_cell_identity> beam_ncis;
  for (size_t i = 0; i != beams_it->size(); ++i) {
    auto beam = parse_beam(beams_it->at(i), i);
    if (!beam.has_value()) {
      return make_unexpected(beam.error());
    }
    if (!beam_ids.emplace(beam->beam_id).second) {
      return make_unexpected(fmt::format("duplicate NTN beam id '{}'", beam->beam_id));
    }
    if (!beam_ncis.emplace(beam->nci).second) {
      return make_unexpected(fmt::format("duplicate NTN beam nci={:#x}", beam->nci));
    }
    table.beams.push_back(beam.value());
  }

  return table;
}

expected<ntn_beam_table_config, std::string> srsran::srs_cu_cp::load_ntn_beam_table_json_file(const std::string& path)
{
  std::ifstream input(path);
  if (!input.is_open()) {
    return make_unexpected(fmt::format("failed to open NTN beam table JSON file '{}'", path));
  }

  std::ostringstream buffer;
  buffer << input.rdbuf();
  return parse_ntn_beam_table_json(buffer.str());
}
