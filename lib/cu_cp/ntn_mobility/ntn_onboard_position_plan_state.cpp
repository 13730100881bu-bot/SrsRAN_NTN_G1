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
 * the LICENSE file in the top-level directory of this distribution.
 *
 */

#include "ntn_onboard_position_plan_state.h"
#include "nlohmann/json.hpp"
#include "srsran/support/io/unique_fd.h"
#include "fmt/format.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <mbedtls/md.h>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

using namespace srsran;
using namespace srs_cu_cp;

namespace {

using json = nlohmann::json;

std::optional<std::string>
validate_exact_object_keys(const json& value, const char* context, std::initializer_list<const char*> required)
{
  if (!value.is_object()) {
    return fmt::format("{} must be an object", context);
  }

  std::set<std::string> allowed;
  for (const char* key : required) {
    allowed.emplace(key);
  }
  for (auto it = value.begin(); it != value.end(); ++it) {
    if (allowed.count(it.key()) == 0) {
      return fmt::format("unknown field '{}.{}'", context, it.key());
    }
  }
  for (const char* key : required) {
    if (!value.contains(key)) {
      return fmt::format("missing field '{}.{}'", context, key);
    }
  }
  return std::nullopt;
}

expected<uint64_t, std::string> parse_uint64(const json& value, const std::string& context)
{
  if (!value.is_number_unsigned()) {
    return make_unexpected(fmt::format("{} must be an unsigned integer", context));
  }
  return value.get<uint64_t>();
}

expected<std::string, std::string> parse_string(const json& value, const std::string& context)
{
  if (!value.is_string()) {
    return make_unexpected(fmt::format("{} must be a string", context));
  }
  return value.get<std::string>();
}

expected<bool, std::string> parse_bool(const json& value, const std::string& context)
{
  if (!value.is_boolean()) {
    return make_unexpected(fmt::format("{} must be a boolean", context));
  }
  return value.get<bool>();
}

std::string lower_ascii(std::string value)
{
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::tolower(c); });
  return value;
}

std::string normalize_hash(std::string value)
{
  value = lower_ascii(std::move(value));
  if (value.rfind("sha256:", 0) != 0) {
    value.insert(0, "sha256:");
  }
  return value;
}

bool is_sha256_digest(const std::string& value)
{
  const std::string normalized = normalize_hash(value);
  return normalized.size() == 71 &&
         std::all_of(normalized.begin() + 7, normalized.end(), [](unsigned char c) { return std::isxdigit(c); });
}

std::string sha256_with_prefix(const std::string& payload)
{
  std::array<unsigned char, 32> digest{};
  const mbedtls_md_info_t*      info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (info == nullptr ||
      mbedtls_md(info, reinterpret_cast<const unsigned char*>(payload.data()), payload.size(), digest.data()) != 0) {
    return {};
  }

  static constexpr char hexadecimal[] = "0123456789abcdef";
  std::string           result        = "sha256:";
  result.reserve(71);
  for (unsigned char byte : digest) {
    result.push_back(hexadecimal[byte >> 4U]);
    result.push_back(hexadecimal[byte & 0x0fU]);
  }
  return result;
}

bool is_valid_satellite_id(const std::string& value)
{
  return value.size() == 7 && value[0] == 'P' && std::isdigit(static_cast<unsigned char>(value[1])) &&
         std::isdigit(static_cast<unsigned char>(value[2])) && value[3] == '-' && value[4] == 'S' &&
         std::isdigit(static_cast<unsigned char>(value[5])) && std::isdigit(static_cast<unsigned char>(value[6]));
}

bool is_valid_l1_id(const std::string& value)
{
  return value.size() == 7 && value[0] == 'G' &&
         std::all_of(value.begin() + 1, value.end(), [](unsigned char c) { return std::isdigit(c); });
}

bool identities_equal(const ntn_onboard_cell_identity& lhs, const ntn_onboard_cell_identity& rhs)
{
  return lhs.nci == rhs.nci && lhs.pci == rhs.pci;
}

bool identity_sets_equal(std::array<ntn_onboard_cell_identity, 2> lhs, std::array<ntn_onboard_cell_identity, 2> rhs)
{
  auto less = [](const auto& left, const auto& right) {
    return left.nci != right.nci ? left.nci < right.nci : left.pci < right.pci;
  };
  std::sort(lhs.begin(), lhs.end(), less);
  std::sort(rhs.begin(), rhs.end(), less);
  return identities_equal(lhs[0], rhs[0]) && identities_equal(lhs[1], rhs[1]);
}

int64_t to_unix_milliseconds(std::chrono::system_clock::time_point value)
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(value.time_since_epoch()).count();
}

json encode_identity(const ntn_onboard_cell_identity& identity)
{
  return {{"nci", identity.nci.value()}, {"pci", identity.pci}};
}

json encode_source_plan(const ntn_versioned_position_plan& plan)
{
  json root = {
      {"schema_version", plan.schema_version},
      {"planning_run_id", plan.planning_run_id},
      {"catalog", {{"id", plan.catalog_id}, {"sha256", plan.catalog_hash}}},
      {"identity_registry", {{"version", plan.identity_registry_version}, {"sha256", plan.identity_registry_hash}}},
      {"access_profile", {{"id", plan.access_profile_id}, {"sha256", plan.access_profile_hash}}},
      {"satellite_id", plan.satellite_id},
      {"catalog_version", plan.catalog_version},
      {"schedule_version", plan.schedule_version},
      {"content_hash", plan.content_hash},
      {"valid_from_unix_ms", to_unix_milliseconds(plan.valid_from)},
      {"valid_until_unix_ms", to_unix_milliseconds(plan.valid_until)},
      {"activation_epoch_unix_ms", to_unix_milliseconds(plan.activation_epoch)}};

  root["onboard_cells"] = json::array();
  for (const ntn_onboard_cell_identity& identity : plan.onboard_cells) {
    root["onboard_cells"].push_back(encode_identity(identity));
  }
  root["visible_l1_positions"] = json::array();
  for (const ntn_l1_position& position : plan.visible_l1_positions) {
    root["visible_l1_positions"].push_back({{"position_id", position.position_id},
                                            {"latitude_deg", position.latitude_deg},
                                            {"longitude_deg", position.longitude_deg},
                                            {"child_mask", position.child_mask}});
  }
  return root;
}

json encode_cell_position_set(const ntn_onboard_cell_position_set& assignment)
{
  return {{"nci", assignment.identity.nci.value()},
          {"pci", assignment.identity.pci},
          {"assigned_l1_ids", assignment.assigned_l1_ids}};
}

json encode_partition(const std::array<ntn_onboard_cell_position_set, 2>& partition)
{
  json result = json::array();
  for (const ntn_onboard_cell_position_set& assignment : partition) {
    result.push_back(encode_cell_position_set(assignment));
  }
  return result;
}

json encode_snapshot(const ntn_onboard_position_plan_state_snapshot& snapshot)
{
  return {{"source_plan", encode_source_plan(snapshot.source)},
          {"assignments", encode_partition(snapshot.cell_positions)},
          {"calendar_hash", snapshot.calendar_hash}};
}

json encode_clear_obligation(const ntn_onboard_position_plan_clear_obligation& obligation)
{
  return {{"snapshot", encode_snapshot(obligation.snapshot)}, {"reason", obligation.reason}};
}

json encode_state_payload(const ntn_onboard_position_plan_persistent_state& state)
{
  json payload = {
      {"state_schema_version", state.schema_version},
      {"generation", state.generation},
      {"satellite_id", state.satellite_id},
      {"planning_context",
       {{"catalog_id", state.planning_context.catalog_id},
        {"catalog_hash", state.planning_context.catalog_hash},
        {"identity_registry_version", state.planning_context.identity_registry_version},
        {"identity_registry_hash", state.planning_context.identity_registry_hash},
        {"access_profile_id", state.planning_context.access_profile_id},
        {"access_profile_hash", state.planning_context.access_profile_hash}}},
      {"high_water",
       {{"catalog_version", state.highest_catalog_version}, {"schedule_version", state.highest_schedule_version}}},
      {"sticky_partition", encode_partition(state.sticky_partition)},
      {"deployment",
       {{"recorded_stage", to_string(state.recorded_deployment_stage)},
        {"detail", state.recorded_deployment_detail},
        {"schedule_version", state.recorded_deployment_schedule_version},
        {"calendar_hash", state.recorded_deployment_calendar_hash},
        {"du_reconciliation_required", state.du_reconciliation_required}}}};

  payload["onboard_cells"] = json::array();
  for (const ntn_onboard_cell_identity& identity : state.onboard_cells) {
    payload["onboard_cells"].push_back(encode_identity(identity));
  }
  payload["active"]             = state.active.has_value() ? encode_snapshot(*state.active) : json(nullptr);
  payload["pending"]            = state.pending.has_value() ? encode_snapshot(*state.pending) : json(nullptr);
  payload["outstanding_clears"] = json::array();
  for (const ntn_onboard_position_plan_clear_obligation& obligation : state.outstanding_clears) {
    payload["outstanding_clears"].push_back(encode_clear_obligation(obligation));
  }
  return payload;
}

expected<ntn_onboard_cell_identity, std::string> decode_identity(const json& value, const std::string& context)
{
  if (auto error = validate_exact_object_keys(value, context.c_str(), {"nci", "pci"}); error.has_value()) {
    return make_unexpected(std::move(*error));
  }
  auto nci = parse_uint64(value.at("nci"), fmt::format("{}.nci", context));
  auto pci = parse_uint64(value.at("pci"), fmt::format("{}.pci", context));
  if (!nci.has_value()) {
    return make_unexpected(nci.error());
  }
  if (!pci.has_value()) {
    return make_unexpected(pci.error());
  }
  auto parsed_nci = nr_cell_identity::create(nci.value());
  if (!parsed_nci.has_value()) {
    return make_unexpected(fmt::format("{}.nci exceeds 36 bits", context));
  }
  if (pci.value() > MAX_PCI) {
    return make_unexpected(fmt::format("{}.pci exceeds {}", context, MAX_PCI));
  }
  return ntn_onboard_cell_identity{parsed_nci.value(), static_cast<pci_t>(pci.value())};
}

expected<std::array<ntn_onboard_cell_identity, 2>, std::string> decode_identities(const json&        value,
                                                                                  const std::string& context)
{
  if (!value.is_array() || value.size() != 2) {
    return make_unexpected(fmt::format("{} must contain exactly two cells", context));
  }
  std::array<ntn_onboard_cell_identity, 2> result{};
  for (size_t i = 0; i != result.size(); ++i) {
    auto identity = decode_identity(value[i], fmt::format("{}[{}]", context, i));
    if (!identity.has_value()) {
      return make_unexpected(identity.error());
    }
    result[i] = identity.value();
  }
  return result;
}

expected<ntn_onboard_cell_position_set, std::string> decode_cell_position_set(const json&        value,
                                                                              const std::string& context)
{
  if (auto error = validate_exact_object_keys(value, context.c_str(), {"nci", "pci", "assigned_l1_ids"});
      error.has_value()) {
    return make_unexpected(std::move(*error));
  }
  auto identity = decode_identity(json{{"nci", value.at("nci")}, {"pci", value.at("pci")}}, context);
  if (!identity.has_value()) {
    return make_unexpected(identity.error());
  }
  const json& ids = value.at("assigned_l1_ids");
  if (!ids.is_array()) {
    return make_unexpected(fmt::format("{}.assigned_l1_ids must be an array", context));
  }

  ntn_onboard_cell_position_set result;
  result.identity = identity.value();
  result.assigned_l1_ids.reserve(ids.size());
  for (size_t i = 0; i != ids.size(); ++i) {
    auto id = parse_string(ids[i], fmt::format("{}.assigned_l1_ids[{}]", context, i));
    if (!id.has_value()) {
      return make_unexpected(id.error());
    }
    result.assigned_l1_ids.push_back(std::move(id.value()));
  }
  return result;
}

expected<std::array<ntn_onboard_cell_position_set, 2>, std::string> decode_partition(const json&        value,
                                                                                     const std::string& context)
{
  if (!value.is_array() || value.size() != 2) {
    return make_unexpected(fmt::format("{} must contain exactly two cell assignments", context));
  }
  std::array<ntn_onboard_cell_position_set, 2> result{};
  for (size_t i = 0; i != result.size(); ++i) {
    auto assignment = decode_cell_position_set(value[i], fmt::format("{}[{}]", context, i));
    if (!assignment.has_value()) {
      return make_unexpected(assignment.error());
    }
    result[i] = std::move(assignment.value());
  }
  return result;
}

expected<ntn_onboard_position_plan_state_snapshot, std::string> decode_snapshot(const json&        value,
                                                                                const std::string& context)
{
  if (auto error = validate_exact_object_keys(value, context.c_str(), {"source_plan", "assignments", "calendar_hash"});
      error.has_value()) {
    return make_unexpected(std::move(*error));
  }
  auto source = parse_ntn_position_plan_json(value.at("source_plan").dump());
  if (!source.has_value()) {
    return make_unexpected(fmt::format("{}.source_plan: {}", context, source.error()));
  }
  auto assignments = decode_partition(value.at("assignments"), fmt::format("{}.assignments", context));
  if (!assignments.has_value()) {
    return make_unexpected(assignments.error());
  }
  auto calendar_hash = parse_string(value.at("calendar_hash"), fmt::format("{}.calendar_hash", context));
  if (!calendar_hash.has_value()) {
    return make_unexpected(calendar_hash.error());
  }
  return ntn_onboard_position_plan_state_snapshot{
      std::move(source.value()), std::move(assignments.value()), std::move(calendar_hash.value())};
}

expected<ntn_onboard_position_plan_clear_obligation, std::string> decode_clear_obligation(const json&        value,
                                                                                          const std::string& context)
{
  if (auto error = validate_exact_object_keys(value, context.c_str(), {"snapshot", "reason"}); error.has_value()) {
    return make_unexpected(std::move(*error));
  }
  auto snapshot = decode_snapshot(value.at("snapshot"), fmt::format("{}.snapshot", context));
  if (!snapshot.has_value()) {
    return make_unexpected(snapshot.error());
  }
  auto reason = parse_string(value.at("reason"), fmt::format("{}.reason", context));
  if (!reason.has_value()) {
    return make_unexpected(reason.error());
  }
  return ntn_onboard_position_plan_clear_obligation{std::move(snapshot.value()), std::move(reason.value())};
}

expected<ntn_position_plan_deployment_stage, std::string> parse_deployment_stage(const json& value)
{
  auto text = parse_string(value, "deployment.recorded_stage");
  if (!text.has_value()) {
    return make_unexpected(text.error());
  }
  static constexpr std::array<ntn_position_plan_deployment_stage, 7> stages{
      ntn_position_plan_deployment_stage::disabled,
      ntn_position_plan_deployment_stage::not_sent,
      ntn_position_plan_deployment_stage::preparing,
      ntn_position_plan_deployment_stage::ready,
      ntn_position_plan_deployment_stage::applied,
      ntn_position_plan_deployment_stage::rejected,
      ntn_position_plan_deployment_stage::unsupported};
  for (ntn_position_plan_deployment_stage stage : stages) {
    if (text.value() == to_string(stage)) {
      return stage;
    }
  }
  return make_unexpected(fmt::format("deployment.recorded_stage '{}' is invalid", text.value()));
}

expected<ntn_onboard_position_plan_persistent_state, std::string> decode_state(const json& root)
{
  if (auto error = validate_exact_object_keys(root,
                                              "root",
                                              {"state_schema_version",
                                               "generation",
                                               "state_hash",
                                               "satellite_id",
                                               "planning_context",
                                               "onboard_cells",
                                               "high_water",
                                               "active",
                                               "pending",
                                               "sticky_partition",
                                               "outstanding_clears",
                                               "deployment"});
      error.has_value()) {
    return make_unexpected(std::move(*error));
  }

  ntn_onboard_position_plan_persistent_state result;
  auto schema       = parse_uint64(root.at("state_schema_version"), "state_schema_version");
  auto generation   = parse_uint64(root.at("generation"), "generation");
  auto state_hash   = parse_string(root.at("state_hash"), "state_hash");
  auto satellite_id = parse_string(root.at("satellite_id"), "satellite_id");
  if (!schema.has_value()) {
    return make_unexpected(schema.error());
  }
  if (schema.value() > std::numeric_limits<unsigned>::max()) {
    return make_unexpected(std::string{"state_schema_version exceeds unsigned range"});
  }
  if (!generation.has_value()) {
    return make_unexpected(generation.error());
  }
  if (!state_hash.has_value()) {
    return make_unexpected(state_hash.error());
  }
  if (!satellite_id.has_value()) {
    return make_unexpected(satellite_id.error());
  }
  result.schema_version = static_cast<unsigned>(schema.value());
  result.generation     = generation.value();
  result.state_hash     = std::move(state_hash.value());
  result.satellite_id   = std::move(satellite_id.value());

  const json& context = root.at("planning_context");
  if (auto error = validate_exact_object_keys(context,
                                              "planning_context",
                                              {"catalog_id",
                                               "catalog_hash",
                                               "identity_registry_version",
                                               "identity_registry_hash",
                                               "access_profile_id",
                                               "access_profile_hash"});
      error.has_value()) {
    return make_unexpected(std::move(*error));
  }
  auto catalog_id   = parse_string(context.at("catalog_id"), "planning_context.catalog_id");
  auto catalog_hash = parse_string(context.at("catalog_hash"), "planning_context.catalog_hash");
  auto registry_version =
      parse_string(context.at("identity_registry_version"), "planning_context.identity_registry_version");
  auto registry_hash = parse_string(context.at("identity_registry_hash"), "planning_context.identity_registry_hash");
  auto profile_id    = parse_string(context.at("access_profile_id"), "planning_context.access_profile_id");
  auto profile_hash  = parse_string(context.at("access_profile_hash"), "planning_context.access_profile_hash");
  if (!catalog_id.has_value() || !catalog_hash.has_value() || !registry_version.has_value() ||
      !registry_hash.has_value() || !profile_id.has_value() || !profile_hash.has_value()) {
    return make_unexpected(std::string{"planning_context contains an invalid string field"});
  }
  result.planning_context = {std::move(catalog_id.value()),
                             std::move(catalog_hash.value()),
                             std::move(registry_version.value()),
                             std::move(registry_hash.value()),
                             std::move(profile_id.value()),
                             std::move(profile_hash.value())};

  auto onboard_cells = decode_identities(root.at("onboard_cells"), "onboard_cells");
  if (!onboard_cells.has_value()) {
    return make_unexpected(onboard_cells.error());
  }
  result.onboard_cells = onboard_cells.value();

  const json& high_water = root.at("high_water");
  if (auto error = validate_exact_object_keys(high_water, "high_water", {"catalog_version", "schedule_version"});
      error.has_value()) {
    return make_unexpected(std::move(*error));
  }
  auto catalog_version  = parse_uint64(high_water.at("catalog_version"), "high_water.catalog_version");
  auto schedule_version = parse_uint64(high_water.at("schedule_version"), "high_water.schedule_version");
  if (!catalog_version.has_value()) {
    return make_unexpected(catalog_version.error());
  }
  if (!schedule_version.has_value()) {
    return make_unexpected(schedule_version.error());
  }
  result.highest_catalog_version  = catalog_version.value();
  result.highest_schedule_version = schedule_version.value();

  if (!root.at("active").is_null()) {
    auto active = decode_snapshot(root.at("active"), "active");
    if (!active.has_value()) {
      return make_unexpected(active.error());
    }
    result.active = std::move(active.value());
  }
  if (!root.at("pending").is_null()) {
    auto pending = decode_snapshot(root.at("pending"), "pending");
    if (!pending.has_value()) {
      return make_unexpected(pending.error());
    }
    result.pending = std::move(pending.value());
  }
  auto sticky_partition = decode_partition(root.at("sticky_partition"), "sticky_partition");
  if (!sticky_partition.has_value()) {
    return make_unexpected(sticky_partition.error());
  }
  result.sticky_partition = std::move(sticky_partition.value());

  const json& outstanding_clears = root.at("outstanding_clears");
  if (!outstanding_clears.is_array()) {
    return make_unexpected(std::string{"outstanding_clears must be an array"});
  }
  result.outstanding_clears.reserve(outstanding_clears.size());
  for (size_t i = 0; i != outstanding_clears.size(); ++i) {
    auto obligation = decode_clear_obligation(outstanding_clears[i], fmt::format("outstanding_clears[{}]", i));
    if (!obligation.has_value()) {
      return make_unexpected(obligation.error());
    }
    result.outstanding_clears.push_back(std::move(obligation.value()));
  }

  const json& deployment = root.at("deployment");
  if (auto error = validate_exact_object_keys(
          deployment,
          "deployment",
          {"recorded_stage", "detail", "schedule_version", "calendar_hash", "du_reconciliation_required"});
      error.has_value()) {
    return make_unexpected(std::move(*error));
  }
  auto deployment_stage   = parse_deployment_stage(deployment.at("recorded_stage"));
  auto deployment_detail  = parse_string(deployment.at("detail"), "deployment.detail");
  auto deployment_version = parse_uint64(deployment.at("schedule_version"), "deployment.schedule_version");
  auto deployment_hash    = parse_string(deployment.at("calendar_hash"), "deployment.calendar_hash");
  auto reconciliation =
      parse_bool(deployment.at("du_reconciliation_required"), "deployment.du_reconciliation_required");
  if (!deployment_stage.has_value()) {
    return make_unexpected(deployment_stage.error());
  }
  if (!deployment_detail.has_value()) {
    return make_unexpected(deployment_detail.error());
  }
  if (!deployment_version.has_value()) {
    return make_unexpected(deployment_version.error());
  }
  if (!deployment_hash.has_value()) {
    return make_unexpected(deployment_hash.error());
  }
  if (!reconciliation.has_value()) {
    return make_unexpected(reconciliation.error());
  }
  result.recorded_deployment_stage            = deployment_stage.value();
  result.recorded_deployment_detail           = std::move(deployment_detail.value());
  result.recorded_deployment_schedule_version = deployment_version.value();
  result.recorded_deployment_calendar_hash    = std::move(deployment_hash.value());
  result.du_reconciliation_required           = reconciliation.value();
  return result;
}

std::optional<std::string> validate_partition(const std::array<ntn_onboard_cell_position_set, 2>& partition,
                                              const std::array<ntn_onboard_cell_identity, 2>&     onboard_cells,
                                              const char*                                         context,
                                              const std::set<std::string>*                        expected_ids)
{
  if (!identity_sets_equal({partition[0].identity, partition[1].identity}, onboard_cells)) {
    return fmt::format("{}_identity_mismatch", context);
  }
  std::set<std::string> ids;
  for (const ntn_onboard_cell_position_set& assignment : partition) {
    for (const std::string& id : assignment.assigned_l1_ids) {
      if (!is_valid_l1_id(id)) {
        return fmt::format("{}_invalid_l1_id", context);
      }
      if (!ids.insert(id).second) {
        return fmt::format("{}_duplicate_l1_id", context);
      }
    }
  }
  if (expected_ids != nullptr && ids != *expected_ids) {
    return fmt::format("{}_inventory_mismatch", context);
  }
  return std::nullopt;
}

bool context_matches_plan(const ntn_onboard_position_plan_state_context& context,
                          const ntn_versioned_position_plan&             plan)
{
  return context.catalog_id == plan.catalog_id &&
         normalize_hash(context.catalog_hash) == normalize_hash(plan.catalog_hash) &&
         context.identity_registry_version == plan.identity_registry_version &&
         normalize_hash(context.identity_registry_hash) == normalize_hash(plan.identity_registry_hash) &&
         context.access_profile_id == plan.access_profile_id &&
         normalize_hash(context.access_profile_hash) == normalize_hash(plan.access_profile_hash);
}

std::optional<std::string> validate_snapshot(const ntn_onboard_position_plan_state_snapshot&   snapshot,
                                             const ntn_onboard_position_plan_persistent_state& state,
                                             const char*                                       context)
{
  const ntn_versioned_position_plan& plan = snapshot.source;
  if (plan.schema_version != 2) {
    return fmt::format("{}_source_schema_unsupported", context);
  }
  if (plan.satellite_id != state.satellite_id || !context_matches_plan(state.planning_context, plan) ||
      !identity_sets_equal(plan.onboard_cells, state.onboard_cells)) {
    return fmt::format("{}_planning_context_mismatch", context);
  }
  if (plan.catalog_version == 0 || plan.schedule_version == 0) {
    return fmt::format("{}_invalid_version", context);
  }
  if (!is_sha256_digest(plan.content_hash) ||
      normalize_hash(plan.content_hash) != normalize_hash(compute_ntn_position_plan_content_hash(plan))) {
    return fmt::format("{}_source_hash_mismatch", context);
  }
  if (!is_sha256_digest(snapshot.calendar_hash)) {
    return fmt::format("{}_calendar_hash_invalid", context);
  }

  std::set<std::string> inventory;
  for (const ntn_l1_position& position : plan.visible_l1_positions) {
    if (!is_valid_l1_id(position.position_id) || !inventory.insert(position.position_id).second ||
        position.child_mask == 0 || position.child_mask > 0x7fU || !std::isfinite(position.latitude_deg) ||
        !std::isfinite(position.longitude_deg) || position.latitude_deg < -90.0 || position.latitude_deg > 90.0 ||
        position.longitude_deg < -180.0 || position.longitude_deg > 180.0) {
      return fmt::format("{}_source_inventory_invalid", context);
    }
  }
  return validate_partition(snapshot.cell_positions, state.onboard_cells, context, &inventory);
}

std::optional<std::string> validate_state(const ntn_onboard_position_plan_persistent_state& state)
{
  if (state.schema_version != ntn_onboard_position_plan_persistent_state::current_schema_version) {
    return fmt::format("unsupported_state_schema_version_{}", state.schema_version);
  }
  if (state.generation == 0) {
    return std::string{"invalid_state_generation"};
  }
  if (!is_valid_satellite_id(state.satellite_id)) {
    return std::string{"invalid_state_satellite_id"};
  }
  if (state.planning_context.catalog_id.empty() || !is_sha256_digest(state.planning_context.catalog_hash) ||
      state.planning_context.identity_registry_version.empty() ||
      !is_sha256_digest(state.planning_context.identity_registry_hash) ||
      state.planning_context.access_profile_id.empty() ||
      !is_sha256_digest(state.planning_context.access_profile_hash)) {
    return std::string{"invalid_state_planning_context"};
  }
  if (!is_valid(state.onboard_cells[0].pci) || !is_valid(state.onboard_cells[1].pci) ||
      state.onboard_cells[0].nci == state.onboard_cells[1].nci) {
    return std::string{"invalid_state_onboard_cells"};
  }
  if (!state.du_reconciliation_required) {
    return std::string{"du_reconciliation_must_be_required"};
  }
  if (auto error = validate_partition(state.sticky_partition, state.onboard_cells, "sticky_partition", nullptr);
      error.has_value()) {
    return error;
  }
  if (state.outstanding_clears.size() > max_ntn_onboard_position_plan_clear_obligations) {
    return std::string{"too_many_outstanding_clears"};
  }

  if (state.active.has_value()) {
    if (auto error = validate_snapshot(*state.active, state, "active"); error.has_value()) {
      return error;
    }
    if (state.highest_catalog_version < state.active->source.catalog_version ||
        state.highest_schedule_version < state.active->source.schedule_version) {
      return std::string{"active_exceeds_version_high_water"};
    }
  }
  if (state.pending.has_value()) {
    if (auto error = validate_snapshot(*state.pending, state, "pending"); error.has_value()) {
      return error;
    }
    if (state.highest_catalog_version < state.pending->source.catalog_version ||
        state.highest_schedule_version < state.pending->source.schedule_version) {
      return std::string{"pending_exceeds_version_high_water"};
    }
  }
  if (state.active.has_value() && state.pending.has_value() &&
      (state.pending->source.schedule_version <= state.active->source.schedule_version ||
       state.pending->source.catalog_version < state.active->source.catalog_version)) {
    return std::string{"pending_version_does_not_follow_active"};
  }

  const auto snapshot_identity_matches = [](const ntn_onboard_position_plan_state_snapshot& lhs,
                                            const ntn_onboard_position_plan_state_snapshot& rhs) {
    return lhs.source.schedule_version == rhs.source.schedule_version &&
           normalize_hash(lhs.calendar_hash) == normalize_hash(rhs.calendar_hash);
  };
  std::set<std::pair<uint64_t, std::string>> clear_identities;
  for (const ntn_onboard_position_plan_clear_obligation& obligation : state.outstanding_clears) {
    if (obligation.reason.empty() || obligation.reason.size() > 256 ||
        std::any_of(
            obligation.reason.begin(), obligation.reason.end(), [](unsigned char c) { return std::iscntrl(c); })) {
      return std::string{"invalid_outstanding_clear_reason"};
    }
    if (auto error = validate_snapshot(obligation.snapshot, state, "outstanding_clear"); error.has_value()) {
      return error;
    }
    if (state.highest_catalog_version < obligation.snapshot.source.catalog_version ||
        state.highest_schedule_version < obligation.snapshot.source.schedule_version) {
      return std::string{"outstanding_clear_exceeds_version_high_water"};
    }
    const auto identity =
        std::make_pair(obligation.snapshot.source.schedule_version, normalize_hash(obligation.snapshot.calendar_hash));
    if (!clear_identities.insert(identity).second) {
      return std::string{"duplicate_outstanding_clear"};
    }
    if ((state.active.has_value() && snapshot_identity_matches(obligation.snapshot, *state.active)) ||
        (state.pending.has_value() && snapshot_identity_matches(obligation.snapshot, *state.pending))) {
      return std::string{"outstanding_clear_targets_live_snapshot"};
    }
  }

  const bool has_deployment_identity =
      state.recorded_deployment_schedule_version != 0 || !state.recorded_deployment_calendar_hash.empty();
  if (has_deployment_identity &&
      (state.recorded_deployment_schedule_version == 0 || !is_sha256_digest(state.recorded_deployment_calendar_hash))) {
    return std::string{"invalid_recorded_deployment_identity"};
  }
  if ((state.recorded_deployment_stage == ntn_position_plan_deployment_stage::preparing ||
       state.recorded_deployment_stage == ntn_position_plan_deployment_stage::ready ||
       state.recorded_deployment_stage == ntn_position_plan_deployment_stage::applied) &&
      !has_deployment_identity) {
    return std::string{"recorded_deployment_identity_missing"};
  }
  if (state.recorded_deployment_stage == ntn_position_plan_deployment_stage::preparing ||
      state.recorded_deployment_stage == ntn_position_plan_deployment_stage::ready ||
      state.recorded_deployment_stage == ntn_position_plan_deployment_stage::applied) {
    const auto matches_snapshot = [&](const std::optional<ntn_onboard_position_plan_state_snapshot>& snapshot) {
      return snapshot.has_value() && snapshot->source.schedule_version == state.recorded_deployment_schedule_version &&
             normalize_hash(snapshot->calendar_hash) == normalize_hash(state.recorded_deployment_calendar_hash);
    };
    if (!matches_snapshot(state.active) && !matches_snapshot(state.pending)) {
      return std::string{"recorded_deployment_snapshot_mismatch"};
    }
  }
  return std::nullopt;
}

expected<std::string, std::string> read_bounded_regular_file(const std::string& path)
{
  int flags = O_RDONLY | O_NONBLOCK;
#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  unique_fd fd(::open(path.c_str(), flags));
  if (!fd.is_open()) {
    return make_unexpected(fmt::format("cannot open state file '{}': {}", path, std::strerror(errno)));
  }

  struct stat file_status{};
  if (::fstat(fd.value(), &file_status) != 0) {
    return make_unexpected(fmt::format("cannot stat state file '{}': {}", path, std::strerror(errno)));
  }
  if (!S_ISREG(file_status.st_mode)) {
    return make_unexpected(fmt::format("state file '{}' is not a regular file", path));
  }
  if (file_status.st_size < 0 ||
      static_cast<uint64_t>(file_status.st_size) > max_ntn_onboard_position_plan_state_file_size) {
    return make_unexpected(
        fmt::format("state file '{}' exceeds {} bytes", path, max_ntn_onboard_position_plan_state_file_size));
  }

  std::string text;
  text.reserve(static_cast<size_t>(file_status.st_size));
  std::array<char, 8192> buffer{};
  while (true) {
    const ssize_t count = ::read(fd.value(), buffer.data(), buffer.size());
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      return make_unexpected(fmt::format("cannot read state file '{}': {}", path, std::strerror(errno)));
    }
    if (count == 0) {
      break;
    }
    if (text.size() + static_cast<size_t>(count) > max_ntn_onboard_position_plan_state_file_size) {
      return make_unexpected(
          fmt::format("state file '{}' exceeds {} bytes", path, max_ntn_onboard_position_plan_state_file_size));
    }
    text.append(buffer.data(), static_cast<size_t>(count));
  }
  return text;
}

expected<bool, std::string> write_all(int fd, const std::string& path, const std::string& text)
{
  size_t offset = 0;
  while (offset != text.size()) {
    const ssize_t count = ::write(fd, text.data() + offset, text.size() - offset);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      return make_unexpected(fmt::format("cannot write temporary state file '{}': {}", path, std::strerror(errno)));
    }
    if (count == 0) {
      return make_unexpected(fmt::format("zero-length write to temporary state file '{}'", path));
    }
    offset += static_cast<size_t>(count);
  }
  return true;
}

class temporary_state_file_guard
{
public:
  explicit temporary_state_file_guard(std::filesystem::path path_) : path(std::move(path_)) {}
  ~temporary_state_file_guard()
  {
    if (!path.empty()) {
      (void)::unlink(path.string().c_str());
    }
  }
  void release() { path.clear(); }

private:
  std::filesystem::path path;
};

expected<std::pair<unique_fd, std::filesystem::path>, std::string>
create_same_directory_temporary_file(const std::filesystem::path& target)
{
  static std::atomic<uint64_t> sequence{0};
  const std::filesystem::path parent = target.parent_path().empty() ? std::filesystem::path{"."} : target.parent_path();
  const std::string           filename = target.filename().string();
  if (filename.empty()) {
    return make_unexpected(std::string{"state file path has no filename"});
  }

  int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  for (unsigned attempt = 0; attempt != 128; ++attempt) {
    const uint64_t              token = sequence.fetch_add(1, std::memory_order_relaxed);
    const std::filesystem::path temporary =
        parent / fmt::format(".{}.tmp.{}.{}", filename, static_cast<unsigned long long>(::getpid()), token);
    const int raw_fd = ::open(temporary.string().c_str(), flags, S_IRUSR | S_IWUSR);
    if (raw_fd >= 0) {
      return std::make_pair(unique_fd{raw_fd}, temporary);
    }
    if (errno != EEXIST) {
      return make_unexpected(
          fmt::format("cannot create temporary state file '{}': {}", temporary.string(), std::strerror(errno)));
    }
  }
  return make_unexpected(fmt::format("cannot allocate a unique temporary state file beside '{}'", target.string()));
}

} // namespace

expected<std::optional<ntn_onboard_position_plan_persistent_state>, std::string>
srsran::srs_cu_cp::load_ntn_onboard_position_plan_state(const std::string& path)
{
  if (path.empty() || path.find('\0') != std::string::npos) {
    return make_unexpected(std::string{"state file path is empty or contains NUL"});
  }

  struct stat path_status{};
  if (::lstat(path.c_str(), &path_status) != 0) {
    if (errno == ENOENT) {
      return std::optional<ntn_onboard_position_plan_persistent_state>{};
    }
    return make_unexpected(fmt::format("cannot inspect state file '{}': {}", path, std::strerror(errno)));
  }
  if (!S_ISREG(path_status.st_mode)) {
    return make_unexpected(fmt::format("state file '{}' is not a regular file", path));
  }

  auto text = read_bounded_regular_file(path);
  if (!text.has_value()) {
    return make_unexpected(text.error());
  }

  try {
    const json root    = json::parse(text.value());
    auto       decoded = decode_state(root);
    if (!decoded.has_value()) {
      return make_unexpected(decoded.error());
    }

    json payload = root;
    payload.erase("state_hash");
    const std::string computed_hash = sha256_with_prefix(payload.dump());
    if (computed_hash.empty()) {
      return make_unexpected(std::string{"cannot compute state hash"});
    }
    if (!is_sha256_digest(decoded->state_hash) || normalize_hash(decoded->state_hash) != computed_hash) {
      return make_unexpected(std::string{"state_hash_mismatch"});
    }
    if (auto error = validate_state(decoded.value()); error.has_value()) {
      return make_unexpected(std::move(*error));
    }
    decoded->state_hash = computed_hash;
    return std::optional<ntn_onboard_position_plan_persistent_state>{std::move(decoded.value())};
  } catch (const std::exception& error) {
    return make_unexpected(fmt::format("invalid NTN position-plan state JSON: {}", error.what()));
  }
}

expected<ntn_onboard_position_plan_state_store_result, std::string>
srsran::srs_cu_cp::store_ntn_onboard_position_plan_state_atomic(
    const std::string&                                path,
    const ntn_onboard_position_plan_persistent_state& state,
    ntn_onboard_position_plan_state_store_failpoint   failpoint)
{
  if (path.empty() || path.find('\0') != std::string::npos) {
    return make_unexpected(std::string{"state file path is empty or contains NUL"});
  }
  if (auto error = validate_state(state); error.has_value()) {
    return make_unexpected(std::move(*error));
  }

  try {
    json              payload    = encode_state_payload(state);
    const std::string state_hash = sha256_with_prefix(payload.dump());
    if (state_hash.empty()) {
      return make_unexpected(std::string{"cannot compute state hash"});
    }
    payload["state_hash"]     = state_hash;
    const std::string encoded = payload.dump(2) + '\n';
    if (encoded.size() > max_ntn_onboard_position_plan_state_file_size) {
      return make_unexpected(
          fmt::format("encoded state exceeds {} bytes", max_ntn_onboard_position_plan_state_file_size));
    }

    const std::filesystem::path target(path);
    const std::filesystem::path parent =
        target.parent_path().empty() ? std::filesystem::path{"."} : target.parent_path();
    int directory_flags = O_RDONLY;
#ifdef O_CLOEXEC
    directory_flags |= O_CLOEXEC;
#endif
#ifdef O_DIRECTORY
    directory_flags |= O_DIRECTORY;
#endif
    unique_fd directory_fd(::open(parent.string().c_str(), directory_flags));
    if (!directory_fd.is_open()) {
      return make_unexpected(
          fmt::format("cannot open state-file parent directory '{}': {}", parent.string(), std::strerror(errno)));
    }

    auto temporary = create_same_directory_temporary_file(target);
    if (!temporary.has_value()) {
      return make_unexpected(temporary.error());
    }
    unique_fd                   temporary_fd   = std::move(temporary->first);
    const std::filesystem::path temporary_path = std::move(temporary->second);
    temporary_state_file_guard  cleanup(temporary_path);

    auto write_result = write_all(temporary_fd.value(), temporary_path.string(), encoded);
    if (!write_result.has_value()) {
      return make_unexpected(write_result.error());
    }
    if (::fsync(temporary_fd.value()) != 0) {
      return make_unexpected(
          fmt::format("cannot fsync temporary state file '{}': {}", temporary_path.string(), std::strerror(errno)));
    }
    if (!temporary_fd.close()) {
      return make_unexpected(
          fmt::format("cannot close temporary state file '{}': {}", temporary_path.string(), std::strerror(errno)));
    }
    if (failpoint == ntn_onboard_position_plan_state_store_failpoint::before_rename) {
      return make_unexpected(std::string{"injected failure before state-file rename"});
    }
    if (::rename(temporary_path.string().c_str(), target.string().c_str()) != 0) {
      return make_unexpected(
          fmt::format("cannot atomically replace state file '{}': {}", target.string(), std::strerror(errno)));
    }
    cleanup.release();

    if (failpoint == ntn_onboard_position_plan_state_store_failpoint::after_rename) {
      return ntn_onboard_position_plan_state_store_result{
          state_hash, false, "injected failure after committed state-file rename"};
    }
    if (::fsync(directory_fd.value()) != 0) {
      return ntn_onboard_position_plan_state_store_result{
          state_hash,
          false,
          fmt::format("state file replaced but parent directory '{}' cannot be fsynced: {}",
                      parent.string(),
                      std::strerror(errno))};
    }
    if (!directory_fd.close()) {
      return ntn_onboard_position_plan_state_store_result{
          state_hash,
          false,
          fmt::format("state file replaced but parent directory '{}' cannot be closed: {}",
                      parent.string(),
                      std::strerror(errno))};
    }
    return ntn_onboard_position_plan_state_store_result{state_hash, true, {}};
  } catch (const std::exception& error) {
    return make_unexpected(fmt::format("cannot encode NTN position-plan state: {}", error.what()));
  }
}
