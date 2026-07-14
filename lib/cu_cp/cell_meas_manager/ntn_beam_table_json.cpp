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
#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <optional>
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

std::optional<std::string> read_optional_unsigned_cap(const nlohmann::json& obj,
                                                      const char*           key,
                                                      const std::string&    path,
                                                      unsigned&             out)
{
  if (!obj.contains(key)) {
    return std::nullopt;
  }
  const auto& value = obj.at(key);
  if (value.is_number_unsigned()) {
    out = value.get<unsigned>();
    return std::nullopt;
  }
  if (value.is_number_integer() && value.get<int64_t>() >= 0) {
    out = static_cast<unsigned>(value.get<int64_t>());
    return std::nullopt;
  }
  return fmt::format("{}.{} must be an unsigned integer", path, key);
}

std::optional<std::string> read_optional_bool(const nlohmann::json& obj,
                                              const char*           key,
                                              const std::string&    path,
                                              bool&                 out)
{
  if (!obj.contains(key)) {
    return std::nullopt;
  }
  const auto& value = obj.at(key);
  if (!value.is_boolean()) {
    return fmt::format("{}.{} must be a boolean", path, key);
  }
  out = value.get<bool>();
  return std::nullopt;
}

std::optional<std::string> read_optional_group_id(const nlohmann::json& obj,
                                                  const char*           key,
                                                  const std::string&    path,
                                                  std::string&          out)
{
  if (!obj.contains(key)) {
    return std::nullopt;
  }
  if (!obj.at(key).is_string()) {
    return fmt::format("{}.{} must be a string", path, key);
  }
  out = obj.at(key).get<std::string>();
  if (out.empty()) {
    return fmt::format("{}.{} must not be empty", path, key);
  }
  return std::nullopt;
}

std::optional<std::string> read_optional_group_ids(const nlohmann::json& obj,
                                                   const char*           key,
                                                   const std::string&    path,
                                                   std::vector<std::string>& out)
{
  if (!obj.contains(key)) {
    return std::nullopt;
  }
  if (!obj.at(key).is_array()) {
    return fmt::format("{}.{} must be an array", path, key);
  }
  std::set<std::string> unique_ids;
  std::vector<std::string> parsed;
  const auto& groups = obj.at(key);
  for (size_t i = 0; i != groups.size(); ++i) {
    if (!groups.at(i).is_string()) {
      return fmt::format("{}.{}[{}] must be a string", path, key, i);
    }
    const std::string group_id = groups.at(i).get<std::string>();
    if (group_id.empty()) {
      return fmt::format("{}.{}[{}] must not be empty", path, key, i);
    }
    if (!unique_ids.emplace(group_id).second) {
      return fmt::format("{}.{} contains duplicate group '{}'", path, key, group_id);
    }
    parsed.push_back(group_id);
  }
  out = std::move(parsed);
  return std::nullopt;
}

expected<ntn_analog_beam_resource_policy, std::string>
parse_analog_resource_policy(const nlohmann::json& policy_json, const std::string& path)
{
  if (!policy_json.is_object()) {
    return make_unexpected(fmt::format("{} must be an object", path));
  }

  ntn_analog_beam_resource_policy policy;
  for (const char* key :
       {"max_access_only_ues", "max_service_bound_ues", "max_loaded_digital_children", "max_drbs"}) {
    unsigned* field = nullptr;
    if (std::string{key} == "max_access_only_ues") {
      field = &policy.max_access_only_ues;
    } else if (std::string{key} == "max_service_bound_ues") {
      field = &policy.max_service_bound_ues;
    } else if (std::string{key} == "max_loaded_digital_children") {
      field = &policy.max_loaded_digital_children;
    } else {
      field = &policy.max_drbs;
    }
    if (std::optional<std::string> error = read_optional_unsigned_cap(policy_json, key, path, *field);
        error.has_value()) {
      return make_unexpected(error.value());
    }
  }
  return policy;
}

expected<ntn_digital_beam_resource_policy, std::string>
parse_digital_resource_policy(const nlohmann::json& policy_json, const std::string& path)
{
  if (!policy_json.is_object()) {
    return make_unexpected(fmt::format("{} must be an object", path));
  }

  ntn_digital_beam_resource_policy policy;
  for (const char* key : {"max_ues", "max_drbs", "max_loaded_ues"}) {
    unsigned* field = nullptr;
    if (std::string{key} == "max_ues") {
      field = &policy.max_ues;
    } else if (std::string{key} == "max_drbs") {
      field = &policy.max_drbs;
    } else {
      field = &policy.max_loaded_ues;
    }
    if (std::optional<std::string> error = read_optional_unsigned_cap(policy_json, key, path, *field);
        error.has_value()) {
      return make_unexpected(error.value());
    }
  }
  if (std::optional<std::string> error =
          read_optional_group_id(policy_json, "reuse_group_id", path, policy.reuse_group_id);
      error.has_value()) {
    return make_unexpected(error.value());
  }
  if (std::optional<std::string> error =
          read_optional_group_ids(policy_json, "conflict_group_ids", path, policy.conflict_group_ids);
      error.has_value()) {
    return make_unexpected(error.value());
  }
  return policy;
}

expected<ntn_resource_domain_policy, std::string>
parse_resource_domain_policy(const nlohmann::json& policy_json, const std::string& path)
{
  if (!policy_json.is_object()) {
    return make_unexpected(fmt::format("{} must be an object", path));
  }

  ntn_resource_domain_policy policy;
  if (policy_json.contains("analog")) {
    auto analog = parse_analog_resource_policy(policy_json.at("analog"), path + ".analog");
    if (!analog.has_value()) {
      return make_unexpected(analog.error());
    }
    policy.analog = analog.value();
  }
  if (policy_json.contains("digital")) {
    auto digital = parse_digital_resource_policy(policy_json.at("digital"), path + ".digital");
    if (!digital.has_value()) {
      return make_unexpected(digital.error());
    }
    policy.digital = digital.value();
  }
  return policy;
}

ntn_analog_beam_resource_policy merge_policy(const ntn_analog_beam_resource_policy& defaults,
                                             const ntn_analog_beam_resource_policy& override)
{
  ntn_analog_beam_resource_policy merged = defaults;
  if (override.max_access_only_ues != 0) {
    merged.max_access_only_ues = override.max_access_only_ues;
  }
  if (override.max_service_bound_ues != 0) {
    merged.max_service_bound_ues = override.max_service_bound_ues;
  }
  if (override.max_loaded_digital_children != 0) {
    merged.max_loaded_digital_children = override.max_loaded_digital_children;
  }
  if (override.max_drbs != 0) {
    merged.max_drbs = override.max_drbs;
  }
  return merged;
}

ntn_digital_beam_resource_policy merge_policy(const ntn_digital_beam_resource_policy& defaults,
                                              const ntn_digital_beam_resource_policy& override)
{
  ntn_digital_beam_resource_policy merged = defaults;
  if (override.max_ues != 0) {
    merged.max_ues = override.max_ues;
  }
  if (override.max_drbs != 0) {
    merged.max_drbs = override.max_drbs;
  }
  if (override.max_loaded_ues != 0) {
    merged.max_loaded_ues = override.max_loaded_ues;
  }
  if (!override.reuse_group_id.empty()) {
    merged.reuse_group_id = override.reuse_group_id;
  }
  if (!override.conflict_group_ids.empty()) {
    merged.conflict_group_ids = override.conflict_group_ids;
  }
  return merged;
}

bool is_empty_policy(const ntn_analog_beam_resource_policy& policy)
{
  return policy.max_access_only_ues == 0 && policy.max_service_bound_ues == 0 &&
         policy.max_loaded_digital_children == 0 && policy.max_drbs == 0;
}

bool is_empty_policy(const ntn_digital_beam_resource_policy& policy)
{
  return policy.max_ues == 0 && policy.max_drbs == 0 && policy.max_loaded_ues == 0 &&
         policy.reuse_group_id.empty() && policy.conflict_group_ids.empty();
}

void apply_resource_policy_defaults(ntn_beam_table_config& table)
{
  for (auto& analog : table.analog_beams) {
    if (!analog.resource_policy.has_value() && is_empty_policy(table.resource_policy.analog)) {
      continue;
    }
    analog.resource_policy =
        merge_policy(table.resource_policy.analog, analog.resource_policy.value_or(ntn_analog_beam_resource_policy{}));
  }
  for (auto& beam : table.beams) {
    if (!beam.resource_policy.has_value() && is_empty_policy(table.resource_policy.digital)) {
      continue;
    }
    beam.resource_policy =
        merge_policy(table.resource_policy.digital, beam.resource_policy.value_or(ntn_digital_beam_resource_policy{}));
  }
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
  const std::string beam_path = fmt::format("beams[{}]", index);
  if (auto err = read_optional_bool(beam_json, "downlink_enabled", beam_path, beam.downlink_enabled);
      err.has_value()) {
    return make_unexpected(err.value());
  }
  if (auto err = read_optional_bool(beam_json, "uplink_enabled", beam_path, beam.uplink_enabled); err.has_value()) {
    return make_unexpected(err.value());
  }
  if (!beam.downlink_enabled && !beam.uplink_enabled) {
    return make_unexpected(fmt::format("beams[{}] must enable at least one link direction", index));
  }
  if (beam_json.contains("analog_beam_id")) {
    if (!beam_json.at("analog_beam_id").is_string()) {
      return make_unexpected(fmt::format("beams[{}].analog_beam_id must be a string", index));
    }
    beam.analog_beam_id = beam_json.at("analog_beam_id").get<std::string>();
    if (beam.analog_beam_id.empty()) {
      return make_unexpected(fmt::format("beams[{}].analog_beam_id must not be empty", index));
    }
  }
  if (beam_json.contains("hex_q")) {
    if (!beam_json.at("hex_q").is_number_integer()) {
      return make_unexpected(fmt::format("beams[{}].hex_q must be an integer", index));
    }
    beam.hex_q = beam_json.at("hex_q").get<int>();
  }
  if (beam_json.contains("hex_r")) {
    if (!beam_json.at("hex_r").is_number_integer()) {
      return make_unexpected(fmt::format("beams[{}].hex_r must be an integer", index));
    }
    beam.hex_r = beam_json.at("hex_r").get<int>();
  }
  if (beam.hex_q.has_value() != beam.hex_r.has_value()) {
    return make_unexpected(fmt::format("beams[{}] must configure both hex_q and hex_r or neither", index));
  }
  if (beam_json.contains("resource_policy")) {
    auto policy = parse_digital_resource_policy(beam_json.at("resource_policy"),
                                                fmt::format("beams[{}].resource_policy", index));
    if (!policy.has_value()) {
      return make_unexpected(policy.error());
    }
    beam.resource_policy = policy.value();
  }

  return beam;
}

expected<ntn_analog_beam_position, std::string> parse_analog_beam(const nlohmann::json& analog_json, size_t index)
{
  if (!analog_json.is_object()) {
    return make_unexpected(fmt::format("analog_beams[{}] must be an object", index));
  }

  ntn_analog_beam_position analog;
  const auto analog_id_it = analog_json.find("analog_beam_id");
  if (analog_id_it == analog_json.end() || !analog_id_it->is_string()) {
    return make_unexpected(fmt::format("analog_beams[{}].analog_beam_id must be a string", index));
  }
  analog.analog_beam_id = analog_id_it->get<std::string>();
  if (analog.analog_beam_id.empty()) {
    return make_unexpected(fmt::format("analog_beams[{}].analog_beam_id must not be empty", index));
  }

  for (const char* key : {"center_hex_q", "center_hex_r"}) {
    if (!analog_json.contains(key) || !analog_json.at(key).is_number_integer()) {
      return make_unexpected(fmt::format("analog_beams[{}].{} must be an integer", index, key));
    }
  }
  analog.center_hex_q = analog_json.at("center_hex_q").get<int>();
  analog.center_hex_r = analog_json.at("center_hex_r").get<int>();

  const auto center_it = analog_json.find("center_digital_beam_id");
  if (center_it == analog_json.end() || !center_it->is_string()) {
    return make_unexpected(fmt::format("analog_beams[{}].center_digital_beam_id must be a string", index));
  }
  analog.center_digital_beam_id = center_it->get<std::string>();
  if (analog.center_digital_beam_id.empty()) {
    return make_unexpected(fmt::format("analog_beams[{}].center_digital_beam_id must not be empty", index));
  }

  const auto children_it = analog_json.find("child_digital_beam_ids");
  if (children_it == analog_json.end() || !children_it->is_array()) {
    return make_unexpected(fmt::format("analog_beams[{}].child_digital_beam_ids must be an array", index));
  }
  std::set<std::string> child_ids;
  for (size_t child_idx = 0; child_idx != children_it->size(); ++child_idx) {
    if (!children_it->at(child_idx).is_string()) {
      return make_unexpected(
          fmt::format("analog_beams[{}].child_digital_beam_ids[{}] must be a string", index, child_idx));
    }
    const std::string child_id = children_it->at(child_idx).get<std::string>();
    if (child_id.empty()) {
      return make_unexpected(
          fmt::format("analog_beams[{}].child_digital_beam_ids[{}] must not be empty", index, child_idx));
    }
    if (!child_ids.emplace(child_id).second) {
      return make_unexpected(fmt::format("analog_beams[{}] contains duplicate child '{}'", index, child_id));
    }
    analog.child_digital_beam_ids.push_back(child_id);
  }
  if (analog.child_digital_beam_ids.empty() || analog.child_digital_beam_ids.size() > 7) {
    return make_unexpected(fmt::format("analog_beams[{}] must contain between 1 and 7 child digital beams", index));
  }

  if (analog_json.contains("is_edge_partial")) {
    if (!analog_json.at("is_edge_partial").is_boolean()) {
      return make_unexpected(fmt::format("analog_beams[{}].is_edge_partial must be a boolean", index));
    }
    analog.is_edge_partial = analog_json.at("is_edge_partial").get<bool>();
  }
  if (!analog.is_edge_partial && analog.child_digital_beam_ids.size() != 7) {
    return make_unexpected(fmt::format("analog_beams[{}] must have seven children unless is_edge_partial=true", index));
  }
  if (analog_json.contains("enabled")) {
    if (!analog_json.at("enabled").is_boolean()) {
      return make_unexpected(fmt::format("analog_beams[{}].enabled must be a boolean", index));
    }
    analog.enabled = analog_json.at("enabled").get<bool>();
  }
  const std::string analog_path = fmt::format("analog_beams[{}]", index);
  if (auto err = read_optional_bool(analog_json, "downlink_enabled", analog_path, analog.downlink_enabled);
      err.has_value()) {
    return make_unexpected(err.value());
  }
  if (auto err = read_optional_bool(analog_json, "uplink_enabled", analog_path, analog.uplink_enabled);
      err.has_value()) {
    return make_unexpected(err.value());
  }
  if (!analog.downlink_enabled && !analog.uplink_enabled) {
    return make_unexpected(fmt::format("analog_beams[{}] must enable at least one link direction", index));
  }
  if (analog_json.contains("resource_policy")) {
    auto policy = parse_analog_resource_policy(analog_json.at("resource_policy"),
                                               fmt::format("analog_beams[{}].resource_policy", index));
    if (!policy.has_value()) {
      return make_unexpected(policy.error());
    }
    analog.resource_policy = policy.value();
  }
  return analog;
}

std::optional<std::string> validate_analog_hierarchy(const ntn_beam_table_config& table)
{
  if (table.analog_beams.empty()) {
    for (const auto& beam : table.beams) {
      if (!beam.analog_beam_id.empty()) {
        return fmt::format("beam '{}' references analog beam '{}' but analog_beams is empty",
                           beam.beam_id,
                           beam.analog_beam_id);
      }
    }
    return std::nullopt;
  }

  std::map<std::string, const ntn_beam_position*> digital_beams;
  for (const auto& beam : table.beams) {
    digital_beams[beam.beam_id] = &beam;
  }

  std::set<std::string> analog_ids;
  std::map<std::string, const ntn_analog_beam_position*> analog_by_id;
  std::set<std::string> assigned_children;
  for (const auto& analog : table.analog_beams) {
    if (!analog_ids.emplace(analog.analog_beam_id).second) {
      return fmt::format("duplicate NTN analog beam id '{}'", analog.analog_beam_id);
    }
    analog_by_id[analog.analog_beam_id] = &analog;
    if (digital_beams.count(analog.center_digital_beam_id) == 0) {
      return fmt::format("analog beam '{}' center digital beam '{}' is not configured",
                         analog.analog_beam_id,
                         analog.center_digital_beam_id);
    }
    bool has_downlink_child = false;
    bool has_uplink_child   = false;
    for (const auto& child_id : analog.child_digital_beam_ids) {
      if (digital_beams.count(child_id) == 0) {
        return fmt::format("analog beam '{}' child digital beam '{}' is not configured",
                           analog.analog_beam_id,
                           child_id);
      }
      const ntn_beam_position& child = *digital_beams.at(child_id);
      if (child.downlink_enabled) {
        has_downlink_child = true;
      }
      if (child.uplink_enabled) {
        has_uplink_child = true;
      }
      if (!assigned_children.emplace(child_id).second) {
        return fmt::format("digital beam '{}' belongs to more than one analog beam", child_id);
      }
    }
    if (analog.downlink_enabled && !has_downlink_child) {
      return fmt::format("analog beam '{}' enables downlink but has no downlink-enabled child",
                         analog.analog_beam_id);
    }
    if (analog.uplink_enabled && !has_uplink_child) {
      return fmt::format("analog beam '{}' enables uplink but has no uplink-enabled child", analog.analog_beam_id);
    }
  }

  for (const auto& beam : table.beams) {
    if (beam.analog_beam_id.empty()) {
      return fmt::format("beam '{}' is missing analog_beam_id", beam.beam_id);
    }
    const auto analog_it = analog_by_id.find(beam.analog_beam_id);
    if (analog_it == analog_by_id.end()) {
      return fmt::format("beam '{}' references unknown analog beam '{}'", beam.beam_id, beam.analog_beam_id);
    }
    const auto& children = analog_it->second->child_digital_beam_ids;
    if (std::find(children.begin(), children.end(), beam.beam_id) == children.end()) {
      return fmt::format("beam '{}' parent analog beam '{}' does not list it as a child",
                         beam.beam_id,
                         beam.analog_beam_id);
    }
    if (beam.downlink_enabled && !analog_it->second->downlink_enabled) {
      return fmt::format("beam '{}' enables downlink but parent analog beam '{}' has downlink disabled",
                         beam.beam_id,
                         beam.analog_beam_id);
    }
    if (beam.uplink_enabled && !analog_it->second->uplink_enabled) {
      return fmt::format("beam '{}' enables uplink but parent analog beam '{}' has uplink disabled",
                         beam.beam_id,
                         beam.analog_beam_id);
    }
  }

  if (assigned_children.size() != table.beams.size()) {
    return "analog beam child lists do not cover every configured digital beam";
  }
  return std::nullopt;
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
  if (json.contains("resource_policy")) {
    auto policy = parse_resource_domain_policy(json.at("resource_policy"), "resource_policy");
    if (!policy.has_value()) {
      return make_unexpected(policy.error());
    }
    table.resource_policy = policy.value();
  }

  if (json.contains("analog_beams")) {
    if (!json.at("analog_beams").is_array()) {
      return make_unexpected("analog_beams must be an array");
    }
    const auto& analog_beams = json.at("analog_beams");
    for (size_t i = 0; i != analog_beams.size(); ++i) {
      auto analog = parse_analog_beam(analog_beams.at(i), i);
      if (!analog.has_value()) {
        return make_unexpected(analog.error());
      }
      table.analog_beams.push_back(analog.value());
    }
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

  apply_resource_policy_defaults(table);

  const std::optional<std::string> hierarchy_error = validate_analog_hierarchy(table);
  if (hierarchy_error.has_value()) {
    return make_unexpected(hierarchy_error.value());
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
