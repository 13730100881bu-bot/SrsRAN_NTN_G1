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

#include "ntn_onboard_position_plan.h"
#include "nlohmann/json.hpp"
#include "ntn_onboard_position_plan_state.h"
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
#include <iomanip>
#include <locale>
#include <map>
#include <mbedtls/md.h>
#include <set>
#include <sstream>
#include <tuple>
#include <unordered_map>
#include <sys/stat.h>
#include <unistd.h>

using namespace srsran;
using namespace srs_cu_cp;

namespace {

constexpr double pi = 3.14159265358979323846;

constexpr const char* input_too_large_prefix = "input_too_large:";

std::atomic<ntn_position_plan_file_read_test_hook> next_plan_file_read_test_hook{nullptr};

std::string make_input_too_large_error(const std::string& detail)
{
  return fmt::format("{} {}", input_too_large_prefix, detail);
}

bool is_input_too_large_error(const std::string& detail)
{
  return detail.compare(0, std::strlen(input_too_large_prefix), input_too_large_prefix) == 0;
}

int64_t to_unix_milliseconds(std::chrono::system_clock::time_point value)
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(value.time_since_epoch()).count();
}

bool has_millisecond_precision(std::chrono::system_clock::time_point value)
{
  return value.time_since_epoch() == std::chrono::duration_cast<std::chrono::milliseconds>(value.time_since_epoch());
}

std::chrono::system_clock::time_point from_unix_milliseconds(int64_t value)
{
  return std::chrono::system_clock::time_point{std::chrono::milliseconds{value}};
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

std::string sha256_with_prefix(const std::string& payload)
{
  std::array<unsigned char, 32> digest{};
  const mbedtls_md_info_t*      info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (info == nullptr || mbedtls_md(info,
                                    reinterpret_cast<const unsigned char*>(payload.data()),
                                    payload.size(),
                                    digest.data()) != 0) {
    return {};
  }
  std::ostringstream hash;
  hash << "sha256:" << std::hex << std::setfill('0');
  for (unsigned char byte : digest) {
    hash << std::setw(2) << static_cast<unsigned>(byte);
  }
  return hash.str();
}

bool is_valid_l1_id(const std::string& value)
{
  return value.size() == 7 && value[0] == 'G' &&
         std::all_of(value.begin() + 1, value.end(), [](unsigned char c) { return std::isdigit(c); });
}

bool is_valid_satellite_id(const std::string& value)
{
  return value.size() == 7 && value[0] == 'P' && std::isdigit(static_cast<unsigned char>(value[1])) &&
         std::isdigit(static_cast<unsigned char>(value[2])) && value[3] == '-' && value[4] == 'S' &&
         std::isdigit(static_cast<unsigned char>(value[5])) && std::isdigit(static_cast<unsigned char>(value[6]));
}

std::vector<std::string> effective_assigned_l1_ids(const ntn_versioned_position_plan& plan)
{
  if (plan.schema_version >= 3) {
    return plan.assigned_l1_position_ids;
  }

  std::vector<std::string> result;
  result.reserve(plan.visible_l1_positions.size());
  for (const ntn_l1_position& position : plan.visible_l1_positions) {
    result.push_back(position.position_id);
  }
  return result;
}

std::vector<ntn_l1_position> select_assigned_l1_positions(const ntn_versioned_position_plan& plan)
{
  if (plan.schema_version < 3) {
    return plan.visible_l1_positions;
  }

  const std::set<std::string> assigned_ids(plan.assigned_l1_position_ids.begin(),
                                            plan.assigned_l1_position_ids.end());
  std::vector<ntn_l1_position> result;
  result.reserve(assigned_ids.size());
  for (const ntn_l1_position& position : plan.visible_l1_positions) {
    if (assigned_ids.count(position.position_id) != 0) {
      result.push_back(position);
    }
  }
  return result;
}

bool is_sha256_digest(const std::string& value)
{
  const std::string normalized = normalize_hash(value);
  return normalized.size() == 71 &&
         std::all_of(normalized.begin() + 7, normalized.end(), [](unsigned char c) { return std::isxdigit(c); });
}

std::optional<std::string> validate_exact_object_keys(const nlohmann::json&              value,
                                                      const char*                        context,
                                                      std::initializer_list<const char*> required,
                                                      std::initializer_list<const char*> optional = {})
{
  if (!value.is_object()) {
    return fmt::format("{} must be an object", context);
  }

  std::set<std::string> allowed;
  for (const char* key : required) {
    allowed.emplace(key);
  }
  for (const char* key : optional) {
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

expected<uint64_t, std::string> parse_json_uint64(const nlohmann::json& value, const std::string& context)
{
  if (!value.is_number_unsigned()) {
    return make_unexpected(fmt::format("{} must be an unsigned integer", context));
  }
  return value.get<uint64_t>();
}

expected<int64_t, std::string> parse_json_int64(const nlohmann::json& value, const std::string& context)
{
  if (!value.is_number_integer()) {
    return make_unexpected(fmt::format("{} must be an integer", context));
  }
  if (value.is_number_unsigned()) {
    const uint64_t parsed = value.get<uint64_t>();
    if (parsed > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      return make_unexpected(fmt::format("{} exceeds the signed 64-bit range", context));
    }
    return static_cast<int64_t>(parsed);
  }
  return value.get<int64_t>();
}

expected<std::string, std::string> parse_json_string(const nlohmann::json& value, const std::string& context)
{
  if (!value.is_string()) {
    return make_unexpected(fmt::format("{} must be a string", context));
  }
  return value.get<std::string>();
}

expected<std::string, std::string> parse_context_identifier(const nlohmann::json& value,
                                                            const std::string&   context)
{
  auto result = parse_json_string(value, context);
  if (!result.has_value()) {
    return make_unexpected(result.error());
  }
  if (result->size() > max_ntn_position_plan_context_identifier_size) {
    return make_unexpected(make_input_too_large_error(
        fmt::format("{} exceeds {} bytes", context, max_ntn_position_plan_context_identifier_size)));
  }
  return result;
}

bool cell_identity_less(const ntn_onboard_cell_identity& lhs, const ntn_onboard_cell_identity& rhs)
{
  if (lhs.nci != rhs.nci) {
    return lhs.nci < rhs.nci;
  }
  return lhs.pci < rhs.pci;
}

bool cell_identity_equal(const ntn_onboard_cell_identity& lhs, const ntn_onboard_cell_identity& rhs)
{
  return lhs.nci == rhs.nci && lhs.pci == rhs.pci;
}

std::array<ntn_onboard_cell_identity, 2>
sorted_cell_identities(std::array<ntn_onboard_cell_identity, 2> identities)
{
  if (cell_identity_less(identities[1], identities[0])) {
    std::swap(identities[0], identities[1]);
  }
  return identities;
}

struct projected_position {
  const ntn_l1_position* source = nullptr;
  double                 x      = 0.0;
  double                 y      = 0.0;
};

std::vector<projected_position> project_positions(const std::vector<ntn_l1_position>& positions)
{
  if (positions.empty()) {
    return {};
  }

  std::vector<const ntn_l1_position*> canonical_positions;
  canonical_positions.reserve(positions.size());
  for (const ntn_l1_position& position : positions) {
    canonical_positions.push_back(&position);
  }
  std::sort(canonical_positions.begin(), canonical_positions.end(), [](const auto* lhs, const auto* rhs) {
    if (lhs->position_id != rhs->position_id) {
      return lhs->position_id < rhs->position_id;
    }
    if (lhs->latitude_deg != rhs->latitude_deg) {
      return lhs->latitude_deg < rhs->latitude_deg;
    }
    return lhs->longitude_deg < rhs->longitude_deg;
  });

  double sin_sum = 0.0;
  double cos_sum = 0.0;
  double mean_latitude = 0.0;
  for (const ntn_l1_position* position : canonical_positions) {
    const double longitude_rad = position->longitude_deg * pi / 180.0;
    sin_sum += std::sin(longitude_rad);
    cos_sum += std::cos(longitude_rad);
    mean_latitude += position->latitude_deg;
  }
  mean_latitude /= static_cast<double>(positions.size());
  const double reference_longitude = std::atan2(sin_sum, cos_sum) * 180.0 / pi;
  const double longitude_scale = std::max(0.01, std::cos(mean_latitude * pi / 180.0));

  std::vector<projected_position> result;
  result.reserve(positions.size());
  for (const ntn_l1_position* position : canonical_positions) {
    double longitude_delta = position->longitude_deg - reference_longitude;
    while (longitude_delta > 180.0) {
      longitude_delta -= 360.0;
    }
    while (longitude_delta < -180.0) {
      longitude_delta += 360.0;
    }
    result.push_back({position, longitude_delta * longitude_scale, position->latitude_deg - mean_latitude});
  }
  return result;
}

std::vector<projected_position> spatially_sorted_positions(const std::vector<ntn_l1_position>& positions)
{
  std::vector<projected_position> result = project_positions(positions);
  if (result.empty()) {
    return result;
  }

  double mean_x = 0.0;
  double mean_y = 0.0;
  for (const projected_position& position : result) {
    mean_x += position.x;
    mean_y += position.y;
  }
  mean_x /= static_cast<double>(result.size());
  mean_y /= static_cast<double>(result.size());

  double variance_x = 0.0;
  double variance_y = 0.0;
  for (const projected_position& position : result) {
    variance_x += (position.x - mean_x) * (position.x - mean_x);
    variance_y += (position.y - mean_y) * (position.y - mean_y);
  }
  const bool split_on_x = variance_x >= variance_y;
  std::sort(result.begin(), result.end(), [split_on_x](const projected_position& lhs,
                                                       const projected_position& rhs) {
    const double lhs_primary = split_on_x ? lhs.x : lhs.y;
    const double rhs_primary = split_on_x ? rhs.x : rhs.y;
    if (lhs_primary != rhs_primary) {
      return lhs_primary < rhs_primary;
    }
    const double lhs_secondary = split_on_x ? lhs.y : lhs.x;
    const double rhs_secondary = split_on_x ? rhs.y : rhs.x;
    if (lhs_secondary != rhs_secondary) {
      return lhs_secondary < rhs_secondary;
    }
    return lhs.source->position_id < rhs.source->position_id;
  });
  return result;
}

std::pair<double, double> centroid_for_owner(const std::map<std::string, unsigned>& owners,
                                             unsigned                              owner,
                                             const std::map<std::string, projected_position>& projected)
{
  double   x     = 0.0;
  double   y     = 0.0;
  unsigned count = 0;
  for (const auto& entry : owners) {
    if (entry.second != owner) {
      continue;
    }
    const auto position_it = projected.find(entry.first);
    if (position_it == projected.end()) {
      continue;
    }
    x += position_it->second.x;
    y += position_it->second.y;
    ++count;
  }
  if (count == 0) {
    return {0.0, 0.0};
  }
  return {x / static_cast<double>(count), y / static_cast<double>(count)};
}

double squared_distance(const projected_position& position, const std::pair<double, double>& centroid)
{
  const double x_delta = position.x - centroid.first;
  const double y_delta = position.y - centroid.second;
  return x_delta * x_delta + y_delta * y_delta;
}

std::chrono::microseconds max_cyclic_gap(std::vector<std::chrono::microseconds> offsets,
                                         std::chrono::microseconds              period)
{
  if (offsets.empty()) {
    return period + std::chrono::microseconds{1};
  }
  std::sort(offsets.begin(), offsets.end());
  offsets.erase(std::unique(offsets.begin(), offsets.end()), offsets.end());
  if (offsets.size() == 1) {
    return period;
  }

  std::chrono::microseconds result{0};
  for (size_t i = 1; i != offsets.size(); ++i) {
    result = std::max(result, offsets[i] - offsets[i - 1]);
  }
  result = std::max(result, period - offsets.back() + offsets.front());
  return result;
}

bool intervals_overlap(const ntn_access_calendar_intent& lhs, const ntn_access_calendar_intent& rhs)
{
  return lhs.start_time < rhs.start_time + rhs.duration && rhs.start_time < lhs.start_time + lhs.duration;
}

unsigned count_mask_bits(uint16_t mask)
{
  unsigned result = 0;
  while (mask != 0) {
    result += mask & 1U;
    mask >>= 1U;
  }
  return result;
}

std::optional<uint16_t> nth_mask_port(uint16_t mask, unsigned ordinal)
{
  for (uint16_t port = 0; port != 16; ++port) {
    if ((mask & (uint16_t{1} << port)) == 0) {
      continue;
    }
    if (ordinal == 0) {
      return port;
    }
    --ordinal;
  }
  return std::nullopt;
}

bool is_supported_access_calendar_profile(const ntn_onboard_position_plan_config& config)
{
  static constexpr std::array<ntn_access_calendar_phase, 3> expected_phases{{
      {0x07ff, 0xf800, 43, 21},
      {0x07ff, 0xf800, 43, 21},
      {0x03ff, 0xfc00, 42, 22},
  }};
  const bool                                                unbound_legacy_profile =
      config.expected_access_profile_id.empty() && config.expected_access_profile_hash.empty();
  const bool bound_profile =
      config.expected_access_profile_id == "ntn-access-16a-64d-v1" &&
      normalize_hash(config.expected_access_profile_hash) == compute_ntn_access_profile_hash(config);
  if ((!unbound_legacy_profile && !bound_profile) || config.max_l1_positions_per_cell != 128 ||
      config.max_l1_positions_per_satellite != 256 || config.max_analog_ports_per_cell != 16 ||
      config.max_analog_ports_per_satellite != 32 || config.max_digital_ports_per_cell != 64 ||
      config.max_digital_ports_per_satellite != 128 || config.access_slot != std::chrono::milliseconds{10} ||
      config.subvisit_duration != std::chrono::microseconds{2500} ||
      config.max_ssb_interval != std::chrono::milliseconds{80} ||
      config.max_prach_interval != std::chrono::milliseconds{640} ||
      config.activation_alignment != std::chrono::milliseconds{640} || config.cell_access_slot_stride != 2 ||
      config.subvisits_per_access_slot != 4) {
    return false;
  }

  uint16_t common_downlink = std::numeric_limits<uint16_t>::max();
  uint16_t common_uplink   = std::numeric_limits<uint16_t>::max();
  for (size_t index = 0; index != config.access_phases.size(); ++index) {
    const ntn_access_calendar_phase& phase = config.access_phases[index];
    if (phase.downlink_port_mask != expected_phases[index].downlink_port_mask ||
        phase.uplink_port_mask != expected_phases[index].uplink_port_mask ||
        phase.digital_downlink_capacity != expected_phases[index].digital_downlink_capacity ||
        phase.digital_uplink_capacity != expected_phases[index].digital_uplink_capacity ||
        phase.digital_downlink_capacity + phase.digital_uplink_capacity != config.max_digital_ports_per_cell) {
      return false;
    }
    if ((phase.downlink_port_mask & phase.uplink_port_mask) != 0 ||
        (phase.downlink_port_mask | phase.uplink_port_mask) != std::numeric_limits<uint16_t>::max()) {
      return false;
    }
    common_downlink &= phase.downlink_port_mask;
    common_uplink &= phase.uplink_port_mask;
  }
  const uint64_t occasions_per_window =
      config.max_ssb_interval.count() / (config.cell_access_slot_stride * config.access_slot.count());
  const uint64_t stable_capacity =
      occasions_per_window * config.subvisits_per_access_slot * count_mask_bits(common_downlink);
  return common_uplink != 0 && stable_capacity >= config.max_l1_positions_per_cell;
}

expected<nr_cell_identity, std::string> parse_nci(const nlohmann::json& value, const char* context)
{
  uint64_t parsed = 0;
  try {
    if (value.is_number_unsigned()) {
      parsed = value.get<uint64_t>();
    } else if (value.is_string()) {
      std::string text = value.get<std::string>();
      size_t      consumed = 0;
      int         base     = 10;
      if (text.rfind("0x", 0) == 0 || text.rfind("0X", 0) == 0) {
        text = text.substr(2);
        base = 16;
      }
      parsed = std::stoull(text, &consumed, base);
      if (consumed != text.size()) {
        return make_unexpected(fmt::format("{}.nci contains trailing characters", context));
      }
    } else {
      return make_unexpected(fmt::format("{}.nci must be an unsigned integer or string", context));
    }
  } catch (const std::exception& error) {
    return make_unexpected(fmt::format("{}.nci is invalid: {}", context, error.what()));
  }
  auto nci = nr_cell_identity::create(parsed);
  if (!nci.has_value()) {
    return make_unexpected(fmt::format("{}.nci exceeds the 36-bit NR cell identity range", context));
  }
  return nci.value();
}

} // namespace

const char* srsran::srs_cu_cp::to_string(ntn_position_plan_stage stage)
{
  switch (stage) {
    case ntn_position_plan_stage::disabled:
      return "disabled";
    case ntn_position_plan_stage::received:
      return "received";
    case ntn_position_plan_stage::validated:
      return "validated";
    case ntn_position_plan_stage::calendar_checked:
      return "calendar_checked";
    case ntn_position_plan_stage::pending:
      return "pending";
    case ntn_position_plan_stage::active:
      return "active";
    case ntn_position_plan_stage::rejected:
      return "rejected";
  }
  return "unknown";
}

const char* srsran::srs_cu_cp::to_string(ntn_position_plan_deployment_stage stage)
{
  switch (stage) {
    case ntn_position_plan_deployment_stage::disabled:
      return "disabled";
    case ntn_position_plan_deployment_stage::not_sent:
      return "not_sent";
    case ntn_position_plan_deployment_stage::preparing:
      return "preparing";
    case ntn_position_plan_deployment_stage::ready:
      return "ready";
    case ntn_position_plan_deployment_stage::applied:
      return "applied";
    case ntn_position_plan_deployment_stage::rejected:
      return "rejected";
    case ntn_position_plan_deployment_stage::unsupported:
      return "unsupported";
  }
  return "unknown";
}

const char* srsran::srs_cu_cp::to_string(ntn_position_plan_recovery_stage stage)
{
  switch (stage) {
    case ntn_position_plan_recovery_stage::disabled:
      return "disabled";
    case ntn_position_plan_recovery_stage::no_state:
      return "no_state";
    case ntn_position_plan_recovery_stage::loaded:
      return "loaded";
    case ntn_position_plan_recovery_stage::reconciling:
      return "reconciling";
    case ntn_position_plan_recovery_stage::reconciled:
      return "reconciled";
    case ntn_position_plan_recovery_stage::failed:
      return "failed";
  }
  return "unknown";
}

const char* srsran::srs_cu_cp::to_string(ntn_position_plan_reject_reason reason)
{
  switch (reason) {
    case ntn_position_plan_reject_reason::none:
      return "none";
    case ntn_position_plan_reject_reason::feature_disabled:
      return "feature_disabled";
    case ntn_position_plan_reject_reason::parse_error:
      return "parse_error";
    case ntn_position_plan_reject_reason::input_too_large:
      return "input_too_large";
    case ntn_position_plan_reject_reason::unsupported_schema:
      return "unsupported_schema";
    case ntn_position_plan_reject_reason::unbound_planning_context:
      return "unbound_planning_context";
    case ntn_position_plan_reject_reason::planning_context_mismatch:
      return "planning_context_mismatch";
    case ntn_position_plan_reject_reason::invalid_satellite_id:
      return "invalid_satellite_id";
    case ntn_position_plan_reject_reason::non_monotonic_version:
      return "non_monotonic_version";
    case ntn_position_plan_reject_reason::invalid_hash:
      return "invalid_hash";
    case ntn_position_plan_reject_reason::expired:
      return "expired";
    case ntn_position_plan_reject_reason::invalid_validity_window:
      return "invalid_validity_window";
    case ntn_position_plan_reject_reason::invalid_activation_epoch:
      return "invalid_activation_epoch";
    case ntn_position_plan_reject_reason::invalid_l1_id:
      return "invalid_l1_id";
    case ntn_position_plan_reject_reason::duplicate_l1_id:
      return "duplicate_l1_id";
    case ntn_position_plan_reject_reason::duplicate_assigned_l1_id:
      return "duplicate_assigned_l1_id";
    case ntn_position_plan_reject_reason::assigned_l1_not_visible:
      return "assigned_l1_not_visible";
    case ntn_position_plan_reject_reason::invalid_child_mask:
      return "invalid_child_mask";
    case ntn_position_plan_reject_reason::invalid_l1_position:
      return "invalid_l1_position";
    case ntn_position_plan_reject_reason::identity_mismatch:
      return "identity_mismatch";
    case ntn_position_plan_reject_reason::schedule_overflow:
      return "schedule_overflow";
    case ntn_position_plan_reject_reason::invalid_calendar_profile:
      return "invalid_calendar_profile";
    case ntn_position_plan_reject_reason::invalid_calendar_position:
      return "invalid_calendar_position";
    case ntn_position_plan_reject_reason::invalid_calendar_timing:
      return "invalid_calendar_timing";
    case ntn_position_plan_reject_reason::invalid_calendar_direction:
      return "invalid_calendar_direction";
    case ntn_position_plan_reject_reason::invalid_access_phase_port:
      return "invalid_access_phase_port";
    case ntn_position_plan_reject_reason::invalid_resource_port:
      return "invalid_resource_port";
    case ntn_position_plan_reject_reason::ssb_deadline_miss:
      return "ssb_deadline_miss";
    case ntn_position_plan_reject_reason::prach_deadline_miss:
      return "prach_deadline_miss";
    case ntn_position_plan_reject_reason::prach_ro_without_beam:
      return "prach_ro_without_beam";
    case ntn_position_plan_reject_reason::duplicate_prach_ro:
      return "duplicate_prach_ro";
    case ntn_position_plan_reject_reason::duplicate_prach_ul_beam:
      return "duplicate_prach_ul_beam";
    case ntn_position_plan_reject_reason::prach_beam_without_ro:
      return "prach_beam_without_ro";
    case ntn_position_plan_reject_reason::invalid_rar_placement:
      return "invalid_rar_placement";
    case ntn_position_plan_reject_reason::resource_conflict:
      return "resource_conflict";
    case ntn_position_plan_reject_reason::du_unavailable:
      return "du_unavailable";
    case ntn_position_plan_reject_reason::cross_du_calendar_not_supported:
      return "cross_du_calendar_not_supported";
    case ntn_position_plan_reject_reason::static_opportunity_mismatch:
      return "static_opportunity_mismatch";
    case ntn_position_plan_reject_reason::du_prepare_rejected:
      return "du_prepare_rejected";
    case ntn_position_plan_reject_reason::du_prepare_timeout:
      return "du_prepare_timeout";
    case ntn_position_plan_reject_reason::du_activation_not_applied:
      return "du_activation_not_applied";
    case ntn_position_plan_reject_reason::calendar_hash_mismatch:
      return "calendar_hash_mismatch";
    case ntn_position_plan_reject_reason::execution_unsupported:
      return "execution_unsupported";
    case ntn_position_plan_reject_reason::state_file_corrupt:
      return "state_file_corrupt";
    case ntn_position_plan_reject_reason::state_context_mismatch:
      return "state_context_mismatch";
    case ntn_position_plan_reject_reason::state_persistence_failure:
      return "state_persistence_failure";
    case ntn_position_plan_reject_reason::du_reconciliation_failed:
      return "du_reconciliation_failed";
  }
  return "unknown";
}

const char* srsran::srs_cu_cp::to_string(ntn_access_calendar_direction direction)
{
  return direction == ntn_access_calendar_direction::downlink ? "downlink" : "uplink";
}

const char* srsran::srs_cu_cp::to_string(ntn_access_calendar_purpose purpose)
{
  switch (purpose) {
    case ntn_access_calendar_purpose::ssb_sib_paging:
      return "ssb_sib_paging";
    case ntn_access_calendar_purpose::ssb_sib_paging_rar:
      return "ssb_sib_paging_rar";
    case ntn_access_calendar_purpose::prach_ro:
      return "prach_ro";
    case ntn_access_calendar_purpose::prach_ul_beam:
      return "prach_ul_beam";
  }
  return "unknown";
}

const char* srsran::srs_cu_cp::to_string(ntn_access_calendar_state state)
{
  return state == ntn_access_calendar_state::proposed ? "proposed" : "checked";
}

const char* srsran::srs_cu_cp::to_string(ntn_initial_access_plan_decision decision)
{
  switch (decision) {
    case ntn_initial_access_plan_decision::accept:
      return "accept";
    case ntn_initial_access_plan_decision::reject:
      return "reject";
    case ntn_initial_access_plan_decision::audit_only:
      return "audit_only";
  }
  return "unknown";
}

const char* srsran::srs_cu_cp::to_string(ntn_initial_access_plan_reason reason)
{
  switch (reason) {
    case ntn_initial_access_plan_reason::none:
      return "none";
    case ntn_initial_access_plan_reason::feature_disabled:
      return "feature_disabled";
    case ntn_initial_access_plan_reason::incomplete_metadata:
      return "incomplete_metadata";
    case ntn_initial_access_plan_reason::no_active_plan:
      return "no_active_plan";
    case ntn_initial_access_plan_reason::active_plan_not_valid:
      return "active_plan_not_valid";
    case ntn_initial_access_plan_reason::satellite_mismatch:
      return "satellite_mismatch";
    case ntn_initial_access_plan_reason::catalog_version_mismatch:
      return "catalog_version_mismatch";
    case ntn_initial_access_plan_reason::schedule_version_mismatch:
      return "schedule_version_mismatch";
    case ntn_initial_access_plan_reason::source_hash_mismatch:
      return "source_hash_mismatch";
    case ntn_initial_access_plan_reason::calendar_hash_mismatch:
      return "calendar_hash_mismatch";
    case ntn_initial_access_plan_reason::cell_identity_mismatch:
      return "cell_identity_mismatch";
    case ntn_initial_access_plan_reason::position_not_assigned_to_cell:
      return "position_not_assigned_to_cell";
    case ntn_initial_access_plan_reason::missing_external_apply_evidence:
      return "missing_external_apply_evidence";
    case ntn_initial_access_plan_reason::prach_occasion_not_scheduled:
      return "prach_occasion_not_scheduled";
    case ntn_initial_access_plan_reason::prach_ul_beam_missing:
      return "prach_ul_beam_missing";
    case ntn_initial_access_plan_reason::resource_port_mismatch:
      return "resource_port_mismatch";
    case ntn_initial_access_plan_reason::intent_only_plan:
      return "intent_only_plan";
  }
  return "unknown";
}

std::chrono::milliseconds
srsran::srs_cu_cp::limit_ntn_position_plan_timer_delay(std::chrono::milliseconds requested_delay)
{
  // unique_timer reserves half of its uint32_t range for wrap-safe comparisons. A 24-hour slice stays well below it.
  static constexpr std::chrono::milliseconds max_timer_slice = std::chrono::hours{24};
  return std::min(requested_delay, max_timer_slice);
}

ntn_onboard_position_plan_controller::ntn_onboard_position_plan_controller(ntn_onboard_position_plan_config config_) :
  cfg(std::move(config_))
{
  cfg.onboard_cells = sorted_cell_identities(cfg.onboard_cells);
  sticky_partition[0].identity = cfg.onboard_cells[0];
  sticky_partition[1].identity = cfg.onboard_cells[1];
  current_stage     = cfg.enabled ? ntn_position_plan_stage::received : ntn_position_plan_stage::disabled;
  deployment       = cfg.enabled && cfg.require_external_apply ? ntn_position_plan_deployment_stage::not_sent
                                                                : ntn_position_plan_deployment_stage::disabled;
  deployment_reason = cfg.require_external_apply ? "awaiting_checked_plan" : "external_execution_disabled";
  recovery                     = cfg.enabled && cfg.require_external_apply ? ntn_position_plan_recovery_stage::no_state
                                                                           : ntn_position_plan_recovery_stage::disabled;
  recovery_reason              = cfg.require_external_apply ? "state_file_not_loaded" : "state_recovery_disabled";
}

ntn_position_plan_reject_reason ntn_onboard_position_plan_controller::validate_plan(
    const ntn_versioned_position_plan& plan, std::chrono::system_clock::time_point now) const
{
  if (!cfg.enabled) {
    return ntn_position_plan_reject_reason::feature_disabled;
  }
  if (plan.schema_version != 1 && plan.schema_version != 2 && plan.schema_version != 3) {
    return ntn_position_plan_reject_reason::unsupported_schema;
  }
  if (plan.schema_version == 1 && cfg.require_external_apply) {
    return ntn_position_plan_reject_reason::unbound_planning_context;
  }
  if (plan.schema_version >= 2) {
    const bool context_is_complete = !plan.planning_run_id.empty() && !plan.catalog_id.empty() &&
                                     is_sha256_digest(plan.catalog_hash) && !plan.identity_registry_version.empty() &&
                                     is_sha256_digest(plan.identity_registry_hash) && !plan.access_profile_id.empty() &&
                                     is_sha256_digest(plan.access_profile_hash);
    const bool context_matches =
        !cfg.expected_catalog_id.empty() && !cfg.expected_catalog_hash.empty() &&
        !cfg.expected_identity_registry_version.empty() && !cfg.expected_identity_registry_hash.empty() &&
        !cfg.expected_access_profile_id.empty() && !cfg.expected_access_profile_hash.empty() &&
        plan.catalog_id == cfg.expected_catalog_id &&
        normalize_hash(plan.catalog_hash) == normalize_hash(cfg.expected_catalog_hash) &&
        plan.identity_registry_version == cfg.expected_identity_registry_version &&
        normalize_hash(plan.identity_registry_hash) == normalize_hash(cfg.expected_identity_registry_hash) &&
        plan.access_profile_id == cfg.expected_access_profile_id &&
        normalize_hash(plan.access_profile_hash) == normalize_hash(cfg.expected_access_profile_hash);
    const bool local_profile_matches =
        normalize_hash(cfg.expected_access_profile_hash) == compute_ntn_access_profile_hash(cfg);
    if (!context_is_complete || !context_matches || !local_profile_matches) {
      return ntn_position_plan_reject_reason::planning_context_mismatch;
    }
  }
  if (!is_valid_satellite_id(cfg.satellite_id) || !is_valid_satellite_id(plan.satellite_id) ||
      plan.satellite_id != cfg.satellite_id) {
    return ntn_position_plan_reject_reason::invalid_satellite_id;
  }
  if (plan.content_hash.empty() || normalize_hash(plan.content_hash) != compute_ntn_position_plan_content_hash(plan)) {
    return ntn_position_plan_reject_reason::invalid_hash;
  }
  if (plan.catalog_version == 0 || plan.schedule_version == 0) {
    return ntn_position_plan_reject_reason::non_monotonic_version;
  }

  if (plan.catalog_version < highest_catalog_version || plan.schedule_version <= highest_schedule_version) {
    return ntn_position_plan_reject_reason::non_monotonic_version;
  }

  if (!has_millisecond_precision(plan.valid_from) || !has_millisecond_precision(plan.valid_until) ||
      plan.valid_from >= plan.valid_until) {
    return ntn_position_plan_reject_reason::invalid_validity_window;
  }
  if (now >= plan.valid_until) {
    return ntn_position_plan_reject_reason::expired;
  }
  if (!has_millisecond_precision(plan.activation_epoch) || plan.activation_epoch < plan.valid_from ||
      plan.activation_epoch >= plan.valid_until) {
    return ntn_position_plan_reject_reason::invalid_activation_epoch;
  }
  const int64_t activation_ms = to_unix_milliseconds(plan.activation_epoch);
  if (cfg.activation_alignment.count() <= 0 || activation_ms < 0 ||
      activation_ms % cfg.activation_alignment.count() != 0) {
    return ntn_position_plan_reject_reason::invalid_activation_epoch;
  }

  const auto configured_identities = sorted_cell_identities(cfg.onboard_cells);
  const auto supplied_identities   = sorted_cell_identities(plan.onboard_cells);
  if (!is_valid(configured_identities[0].pci) || !is_valid(configured_identities[1].pci) ||
      configured_identities[0].nci == configured_identities[1].nci ||
      !cell_identity_equal(configured_identities[0], supplied_identities[0]) ||
      !cell_identity_equal(configured_identities[1], supplied_identities[1])) {
    return ntn_position_plan_reject_reason::identity_mismatch;
  }

  std::set<std::string> l1_ids;
  for (const ntn_l1_position& position : plan.visible_l1_positions) {
    if (!is_valid_l1_id(position.position_id)) {
      return ntn_position_plan_reject_reason::invalid_l1_id;
    }
    if (!l1_ids.insert(position.position_id).second) {
      return ntn_position_plan_reject_reason::duplicate_l1_id;
    }
    if ((plan.schema_version >= 2 && (position.child_mask == 0 || position.child_mask > 0x7fU)) ||
        (plan.schema_version == 1 && position.child_mask != 0)) {
      return ntn_position_plan_reject_reason::invalid_child_mask;
    }
    if (!std::isfinite(position.latitude_deg) || !std::isfinite(position.longitude_deg) ||
        position.latitude_deg < -90.0 || position.latitude_deg > 90.0 || position.longitude_deg < -180.0 ||
        position.longitude_deg > 180.0) {
      return ntn_position_plan_reject_reason::invalid_l1_position;
    }
  }

  const std::vector<std::string> assigned_ids = effective_assigned_l1_ids(plan);
  std::set<std::string>          unique_assigned_ids;
  for (const std::string& position_id : assigned_ids) {
    if (!is_valid_l1_id(position_id)) {
      return ntn_position_plan_reject_reason::invalid_l1_id;
    }
    if (!unique_assigned_ids.insert(position_id).second) {
      return ntn_position_plan_reject_reason::duplicate_assigned_l1_id;
    }
    if (l1_ids.count(position_id) == 0) {
      return ntn_position_plan_reject_reason::assigned_l1_not_visible;
    }
  }

  const size_t position_count = assigned_ids.size();
  if (position_count > cfg.max_l1_positions_per_satellite ||
      (position_count + 1) / 2 > cfg.max_l1_positions_per_cell) {
    return ntn_position_plan_reject_reason::schedule_overflow;
  }
  return ntn_position_plan_reject_reason::none;
}

std::array<ntn_onboard_cell_position_set, 2>
ntn_onboard_position_plan_controller::partition_positions(const std::vector<ntn_l1_position>& positions) const
{
  std::array<ntn_onboard_cell_position_set, 2> result{};
  result[0].identity = cfg.onboard_cells[0];
  result[1].identity = cfg.onboard_cells[1];
  if (positions.empty()) {
    return result;
  }

  const std::vector<projected_position> ordered = spatially_sorted_positions(positions);
  std::map<std::string, projected_position> projected;
  for (const projected_position& position : ordered) {
    projected.emplace(position.source->position_id, position);
  }

  const std::array<size_t, 2> targets{(positions.size() + 1) / 2, positions.size() / 2};
  std::map<std::string, unsigned> owners;
  const bool                      sticky_available =
      !sticky_partition[0].assigned_l1_ids.empty() || !sticky_partition[1].assigned_l1_ids.empty();
  if (sticky_available) {
    std::set<std::string> current_ids;
    for (const ntn_l1_position& position : positions) {
      current_ids.insert(position.position_id);
    }
    for (unsigned cell_index = 0; cell_index != sticky_partition.size(); ++cell_index) {
      for (const std::string& position_id : sticky_partition[cell_index].assigned_l1_ids) {
        if (current_ids.count(position_id) != 0) {
          owners.emplace(position_id, cell_index);
        }
      }
    }
  }

  if (owners.empty()) {
    for (size_t i = 0; i != ordered.size(); ++i) {
      owners.emplace(ordered[i].source->position_id, i < targets[0] ? 0U : 1U);
    }
  } else {
    std::array<size_t, 2> counts{};
    for (const auto& entry : owners) {
      ++counts[entry.second];
    }

    for (unsigned source = 0; source != 2; ++source) {
      const unsigned target = 1U - source;
      while (counts[source] > targets[source]) {
        const auto source_centroid = centroid_for_owner(owners, source, projected);
        const auto target_centroid = centroid_for_owner(owners, target, projected);
        auto       best            = owners.end();
        double     best_cost       = std::numeric_limits<double>::infinity();
        for (auto it = owners.begin(); it != owners.end(); ++it) {
          if (it->second != source) {
            continue;
          }
          const projected_position& position = projected.at(it->first);
          const double cost = squared_distance(position, target_centroid) - squared_distance(position, source_centroid);
          if (cost < best_cost || (cost == best_cost && (best == owners.end() || it->first < best->first))) {
            best      = it;
            best_cost = cost;
          }
        }
        if (best == owners.end()) {
          break;
        }
        best->second = target;
        --counts[source];
        ++counts[target];
      }
    }

    for (const projected_position& position : ordered) {
      if (owners.count(position.source->position_id) != 0) {
        continue;
      }
      unsigned selected = 0;
      if (counts[0] >= targets[0]) {
        selected = 1;
      } else if (counts[1] >= targets[1]) {
        selected = 0;
      } else if (!owners.empty()) {
        const auto centroid0 = centroid_for_owner(owners, 0, projected);
        const auto centroid1 = centroid_for_owner(owners, 1, projected);
        const double distance0 = squared_distance(position, centroid0);
        const double distance1 = squared_distance(position, centroid1);
        if (distance1 < distance0 || (distance1 == distance0 && counts[1] < counts[0])) {
          selected = 1;
        }
      }
      owners.emplace(position.source->position_id, selected);
      ++counts[selected];
    }
  }

  for (const auto& entry : owners) {
    result[entry.second].assigned_l1_ids.push_back(entry.first);
  }
  return result;
}

std::vector<ntn_access_calendar_intent> ntn_onboard_position_plan_controller::build_access_calendar(
    uint64_t schedule_version, const std::array<ntn_onboard_cell_position_set, 2>& assignments) const
{
  std::vector<ntn_access_calendar_intent> result;
  if (!is_supported_access_calendar_profile(cfg)) {
    return result;
  }

  unsigned nof_stable_downlink_lanes = std::numeric_limits<unsigned>::max();
  unsigned nof_stable_uplink_lanes   = std::numeric_limits<unsigned>::max();
  for (const ntn_access_calendar_phase& phase : cfg.access_phases) {
    nof_stable_downlink_lanes = std::min(nof_stable_downlink_lanes, count_mask_bits(phase.downlink_port_mask));
    nof_stable_uplink_lanes   = std::min(nof_stable_uplink_lanes, count_mask_bits(phase.uplink_port_mask));
  }
  const unsigned ssb_rounds = cfg.max_prach_interval.count() / cfg.max_ssb_interval.count();
  const unsigned occasions_per_ssb_window =
      cfg.max_ssb_interval.count() / (cfg.cell_access_slot_stride * cfg.access_slot.count());
  const unsigned lanes_per_occasion = nof_stable_downlink_lanes * cfg.subvisits_per_access_slot;
  if (nof_stable_downlink_lanes == 0 || nof_stable_uplink_lanes == 0 || ssb_rounds == 0 ||
      occasions_per_ssb_window == 0 || lanes_per_occasion == 0) {
    return result;
  }

  const nr_cell_identity lower_nci     = std::min(assignments[0].identity.nci, assignments[1].identity.nci);
  size_t                 nof_positions = assignments[0].assigned_l1_ids.size() + assignments[1].assigned_l1_ids.size();
  result.reserve(nof_positions * (ssb_rounds + 2));
  for (const ntn_onboard_cell_position_set& cell : assignments) {
    const unsigned cell_slot_parity = cell.identity.nci == lower_nci ? 0U : 1U;
    for (size_t i = 0; i != cell.assigned_l1_ids.size(); ++i) {
      const unsigned occasion_in_ssb_window = i / lanes_per_occasion;
      const unsigned lane_in_occasion       = i % lanes_per_occasion;
      if (occasion_in_ssb_window >= occasions_per_ssb_window) {
        continue;
      }
      const unsigned downlink_port_ordinal = lane_in_occasion / cfg.subvisits_per_access_slot;
      const unsigned subvisit              = lane_in_occasion % cfg.subvisits_per_access_slot;

      const unsigned group       = occasion_in_ssb_window * cfg.subvisits_per_access_slot + subvisit;
      const unsigned prach_round = (group + downlink_port_ordinal / nof_stable_uplink_lanes) % ssb_rounds;
      const unsigned rar_round   = (prach_round + 1) % ssb_rounds;
      for (unsigned round = 0; round != ssb_rounds; ++round) {
        const unsigned cell_occasion           = round * occasions_per_ssb_window + occasion_in_ssb_window;
        const unsigned access_slot_index       = cell_slot_parity + cfg.cell_access_slot_stride * cell_occasion;
        const ntn_access_calendar_phase& phase = cfg.access_phases[access_slot_index % cfg.access_phases.size()];
        const unsigned                   phase_downlink_lanes = count_mask_bits(phase.downlink_port_mask);
        const auto                       downlink_port =
            nth_mask_port(phase.downlink_port_mask, (downlink_port_ordinal + round) % phase_downlink_lanes);
        if (!downlink_port.has_value()) {
          continue;
        }
        ntn_access_calendar_intent intent;
        intent.schedule_version = schedule_version;
        intent.nci              = cell.identity.nci;
        intent.position_id      = cell.assigned_l1_ids[i];
        intent.start_time       = access_slot_index * cfg.access_slot + subvisit * cfg.subvisit_duration;
        intent.duration         = cfg.subvisit_duration;
        intent.direction        = ntn_access_calendar_direction::downlink;
        intent.purpose          = round == rar_round ? ntn_access_calendar_purpose::ssb_sib_paging_rar
                                                     : ntn_access_calendar_purpose::ssb_sib_paging;
        intent.port_id          = downlink_port.value();
        result.push_back(std::move(intent));
      }

      const unsigned prach_cell_occasion     = prach_round * occasions_per_ssb_window + occasion_in_ssb_window;
      const unsigned prach_access_slot_index = cell_slot_parity + cfg.cell_access_slot_stride * prach_cell_occasion;
      const ntn_access_calendar_phase& prach_phase =
          cfg.access_phases[prach_access_slot_index % cfg.access_phases.size()];
      const unsigned phase_uplink_lanes = count_mask_bits(prach_phase.uplink_port_mask);
      const auto     uplink_port =
          nth_mask_port(prach_phase.uplink_port_mask, (downlink_port_ordinal + prach_round) % phase_uplink_lanes);
      if (!uplink_port.has_value()) {
        continue;
      }
      const std::chrono::microseconds prach_start =
          prach_access_slot_index * cfg.access_slot + subvisit * cfg.subvisit_duration;
      ntn_access_calendar_intent ro;
      ro.schedule_version = schedule_version;
      ro.nci              = cell.identity.nci;
      ro.position_id      = cell.assigned_l1_ids[i];
      ro.start_time       = prach_start;
      ro.duration         = cfg.subvisit_duration;
      ro.direction        = ntn_access_calendar_direction::uplink;
      ro.purpose          = ntn_access_calendar_purpose::prach_ro;
      ro.port_id          = ntn_access_calendar_intent::no_resource_port;
      result.push_back(ro);

      ro.purpose = ntn_access_calendar_purpose::prach_ul_beam;
      ro.port_id = uplink_port.value();
      result.push_back(std::move(ro));
    }
  }
  return result;
}

ntn_access_calendar_audit ntn_onboard_position_plan_controller::audit_access_calendar(
    uint64_t schedule_version,
    const std::array<ntn_onboard_cell_position_set, 2>& assignments,
    const std::vector<ntn_access_calendar_intent>&       intents) const
{
  ntn_access_calendar_audit audit;
  audit.nof_calendar_intents = intents.size();

  if (!is_supported_access_calendar_profile(cfg)) {
    audit.reason = ntn_position_plan_reject_reason::invalid_calendar_profile;
    return audit;
  }

  std::map<std::string, nr_cell_identity> expected_positions;
  std::set<nr_cell_identity>               cell_ncis;
  for (const ntn_onboard_cell_position_set& cell : assignments) {
    cell_ncis.insert(cell.identity.nci);
    for (const std::string& position_id : cell.assigned_l1_ids) {
      if (!expected_positions.emplace(position_id, cell.identity.nci).second) {
        audit.reason = ntn_position_plan_reject_reason::invalid_calendar_position;
        return audit;
      }
    }
  }
  audit.nof_l1_positions = expected_positions.size();
  if (cell_ncis.size() != assignments.size()) {
    audit.reason = ntn_position_plan_reject_reason::invalid_calendar_position;
    return audit;
  }

  const nr_cell_identity               lower_nci = *cell_ncis.begin();
  std::map<nr_cell_identity, unsigned> cell_slot_parity;
  for (nr_cell_identity nci : cell_ncis) {
    cell_slot_parity.emplace(nci, nci == lower_nci ? 0U : 1U);
  }

  std::map<std::string, std::vector<std::chrono::microseconds>> ssb_offsets;
  std::map<std::string, std::vector<std::chrono::microseconds>> prach_offsets;
  std::map<std::string, std::vector<std::chrono::microseconds>>                                   rar_offsets;
  std::map<std::string, unsigned>                                                                 ssb_counts;
  std::map<std::string, unsigned>                                                                 prach_ro_counts;
  std::map<std::string, unsigned>                                                                 prach_beam_counts;
  std::map<std::pair<nr_cell_identity, uint16_t>, std::vector<const ntn_access_calendar_intent*>> resources;
  std::map<nr_cell_identity, std::set<uint16_t>> used_ports;
  using prach_pair_key =
      std::tuple<uint64_t, nr_cell_identity, std::string, std::chrono::microseconds, std::chrono::microseconds>;
  std::map<prach_pair_key, std::pair<unsigned, unsigned>> prach_pairs;

  for (const ntn_access_calendar_intent& intent : intents) {
    if (intent.schedule_version != schedule_version || intent.start_time.count() < 0 ||
        intent.start_time + intent.duration > cfg.max_prach_interval || cell_ncis.count(intent.nci) == 0) {
      audit.reason = ntn_position_plan_reject_reason::invalid_calendar_position;
      return audit;
    }
    const auto expected_it = expected_positions.find(intent.position_id);
    if (expected_it == expected_positions.end() || expected_it->second != intent.nci) {
      audit.reason = ntn_position_plan_reject_reason::invalid_calendar_position;
      return audit;
    }

    if (intent.duration != cfg.subvisit_duration || intent.start_time.count() % cfg.subvisit_duration.count() != 0) {
      audit.reason = ntn_position_plan_reject_reason::invalid_calendar_timing;
      return audit;
    }
    const int64_t slot_index  = intent.start_time.count() / cfg.access_slot.count();
    const int64_t slot_offset = intent.start_time.count() % cfg.access_slot.count();
    if (slot_offset + intent.duration.count() > cfg.access_slot.count() ||
        static_cast<unsigned>(slot_index % cfg.cell_access_slot_stride) != cell_slot_parity.at(intent.nci)) {
      audit.reason = ntn_position_plan_reject_reason::invalid_calendar_timing;
      return audit;
    }
    const ntn_access_calendar_phase& phase = cfg.access_phases[slot_index % cfg.access_phases.size()];
    const prach_pair_key             pair_key{
        intent.schedule_version, intent.nci, intent.position_id, intent.start_time, intent.duration};

    if (intent.purpose == ntn_access_calendar_purpose::ssb_sib_paging ||
        intent.purpose == ntn_access_calendar_purpose::ssb_sib_paging_rar) {
      if (intent.direction != ntn_access_calendar_direction::downlink) {
        ++audit.invalid_direction_intents;
      }
      ++audit.nof_ssb_intents;
      ++ssb_counts[intent.position_id];
      ssb_offsets[intent.position_id].push_back(intent.start_time);
      if (intent.purpose == ntn_access_calendar_purpose::ssb_sib_paging_rar) {
        rar_offsets[intent.position_id].push_back(intent.start_time);
      }
    } else if (intent.purpose == ntn_access_calendar_purpose::prach_ro) {
      if (intent.direction != ntn_access_calendar_direction::uplink) {
        ++audit.invalid_direction_intents;
      }
      if (intent.port_id != ntn_access_calendar_intent::no_resource_port) {
        audit.reason = ntn_position_plan_reject_reason::invalid_resource_port;
        return audit;
      }
      ++audit.nof_prach_ro_intents;
      ++prach_ro_counts[intent.position_id];
      prach_offsets[intent.position_id].push_back(intent.start_time);
      ++prach_pairs[pair_key].first;
      continue;
    } else if (intent.purpose == ntn_access_calendar_purpose::prach_ul_beam) {
      if (intent.direction != ntn_access_calendar_direction::uplink) {
        ++audit.invalid_direction_intents;
      }
      ++audit.nof_prach_ul_beam_intents;
      ++prach_beam_counts[intent.position_id];
      ++prach_pairs[pair_key].second;
    } else {
      audit.reason = ntn_position_plan_reject_reason::invalid_calendar_position;
      return audit;
    }

    if (intent.port_id == ntn_access_calendar_intent::no_resource_port ||
        intent.port_id >= cfg.max_analog_ports_per_cell) {
      audit.reason = ntn_position_plan_reject_reason::invalid_resource_port;
      return audit;
    }
    const uint16_t port_bit           = uint16_t{1} << intent.port_id;
    const bool     phase_port_matches = (intent.purpose == ntn_access_calendar_purpose::ssb_sib_paging ||
                                         intent.purpose == ntn_access_calendar_purpose::ssb_sib_paging_rar)
                                            ? (phase.downlink_port_mask & port_bit) != 0
                                            : (phase.uplink_port_mask & port_bit) != 0;
    if (!phase_port_matches) {
      ++audit.invalid_phase_ports;
    }
    resources[{intent.nci, intent.port_id}].push_back(&intent);
    used_ports[intent.nci].insert(intent.port_id);
  }

  for (const auto& entry : used_ports) {
    audit.max_used_analog_ports_per_cell =
        std::max(audit.max_used_analog_ports_per_cell, static_cast<unsigned>(entry.second.size()));
    audit.max_used_analog_ports_per_satellite += entry.second.size();
  }
  if (audit.max_used_analog_ports_per_cell > cfg.max_analog_ports_per_cell ||
      audit.max_used_analog_ports_per_satellite > cfg.max_analog_ports_per_satellite) {
    audit.reason = ntn_position_plan_reject_reason::invalid_resource_port;
    return audit;
  }

  for (auto& entry : resources) {
    auto& entries = entry.second;
    std::sort(entries.begin(), entries.end(), [](const auto* lhs, const auto* rhs) {
      if (lhs->start_time != rhs->start_time) {
        return lhs->start_time < rhs->start_time;
      }
      return lhs->duration < rhs->duration;
    });
    for (size_t i = 1; i != entries.size(); ++i) {
      if (intervals_overlap(*entries[i - 1], *entries[i])) {
        ++audit.resource_conflicts;
      }
    }
  }

  for (const auto& entry : prach_pairs) {
    const unsigned ro_count   = entry.second.first;
    const unsigned beam_count = entry.second.second;
    if (ro_count > 1) {
      audit.duplicate_prach_ros += ro_count - 1;
    }
    if (beam_count > 1) {
      audit.duplicate_prach_ul_beams += beam_count - 1;
    }
    if (ro_count != 0 && beam_count == 0) {
      audit.prach_ro_without_beam += ro_count;
    }
    if (beam_count != 0 && ro_count == 0) {
      audit.prach_beam_without_ro += beam_count;
    }
  }

  const unsigned expected_ssb_count  = cfg.max_prach_interval.count() / cfg.max_ssb_interval.count();
  bool           invalid_ssb_count   = false;
  bool           invalid_prach_count = false;
  for (const auto& entry : expected_positions) {
    const std::string& position_id = entry.first;
    audit.max_ssb_interval =
        std::max(audit.max_ssb_interval, max_cyclic_gap(ssb_offsets[position_id], cfg.max_prach_interval));
    audit.max_prach_interval =
        std::max(audit.max_prach_interval, max_cyclic_gap(prach_offsets[position_id], cfg.max_prach_interval));
    invalid_ssb_count |= ssb_counts[position_id] != expected_ssb_count;
    invalid_prach_count |= prach_ro_counts[position_id] != 1 || prach_beam_counts[position_id] != 1;

    if (prach_ro_counts[position_id] == 1 && rar_offsets[position_id].size() == 1 &&
        !ssb_offsets[position_id].empty()) {
      std::vector<std::chrono::microseconds> ordered_ssb = ssb_offsets[position_id];
      std::sort(ordered_ssb.begin(), ordered_ssb.end());
      const std::chrono::microseconds ro_offset = prach_offsets[position_id].front();
      auto                            next_ssb  = std::upper_bound(ordered_ssb.begin(), ordered_ssb.end(), ro_offset);
      const std::chrono::microseconds expected_rar = next_ssb == ordered_ssb.end() ? ordered_ssb.front() : *next_ssb;
      if (rar_offsets[position_id].front() != expected_rar) {
        ++audit.rar_placement_mismatches;
      }
    } else if (rar_offsets[position_id].size() != 1) {
      ++audit.rar_placement_mismatches;
    }
  }

  if (audit.invalid_direction_intents != 0) {
    audit.reason = ntn_position_plan_reject_reason::invalid_calendar_direction;
  } else if (audit.invalid_phase_ports != 0) {
    audit.reason = ntn_position_plan_reject_reason::invalid_access_phase_port;
  } else if (audit.duplicate_prach_ros != 0) {
    audit.reason = ntn_position_plan_reject_reason::duplicate_prach_ro;
  } else if (audit.duplicate_prach_ul_beams != 0) {
    audit.reason = ntn_position_plan_reject_reason::duplicate_prach_ul_beam;
  } else if (audit.prach_ro_without_beam != 0) {
    audit.reason = ntn_position_plan_reject_reason::prach_ro_without_beam;
  } else if (audit.prach_beam_without_ro != 0) {
    audit.reason = ntn_position_plan_reject_reason::prach_beam_without_ro;
  } else if (audit.rar_placement_mismatches != 0) {
    audit.reason = ntn_position_plan_reject_reason::invalid_rar_placement;
  } else if (audit.resource_conflicts != 0) {
    audit.reason = ntn_position_plan_reject_reason::resource_conflict;
  } else if (invalid_ssb_count || audit.max_ssb_interval > cfg.max_ssb_interval) {
    audit.reason = ntn_position_plan_reject_reason::ssb_deadline_miss;
  } else if (invalid_prach_count || audit.max_prach_interval > cfg.max_prach_interval) {
    audit.reason = ntn_position_plan_reject_reason::prach_deadline_miss;
  } else {
    audit.accepted = true;
    audit.reason   = ntn_position_plan_reject_reason::none;
  }
  return audit;
}

ntn_initial_access_plan_audit
ntn_onboard_position_plan_controller::audit_initial_access_event(const ntn_initial_access_plan_event& event) const
{
  ntn_initial_access_plan_audit result;
  auto                          finish = [&result](ntn_initial_access_plan_decision decision,
                                                   ntn_initial_access_plan_reason   reason,
                                                   const char*                      evidence) {
    result.decision = decision;
    result.reason   = reason;
    result.evidence = evidence;
    return result;
  };

  if (!cfg.enabled) {
    return finish(ntn_initial_access_plan_decision::audit_only,
                  ntn_initial_access_plan_reason::feature_disabled,
                  "not_evaluated_feature_disabled_no_rf_evidence");
  }
  if (event.satellite_id.empty() || event.catalog_version == 0 || event.schedule_version == 0 ||
      event.source_content_hash.empty() || event.calendar_hash.empty() || event.position_id.empty() ||
      event.cell.pci == INVALID_PCI || event.ul_beam_port_id == ntn_access_calendar_intent::no_resource_port) {
    return finish(ntn_initial_access_plan_decision::reject,
                  ntn_initial_access_plan_reason::incomplete_metadata,
                  "rejected_incomplete_sideband_no_rf_evidence");
  }
  if (!active.has_value()) {
    return finish(ntn_initial_access_plan_decision::reject,
                  ntn_initial_access_plan_reason::no_active_plan,
                  "rejected_no_active_plan_no_rf_evidence");
  }

  result.active_schedule_version = active->source.schedule_version;
  result.active_calendar_hash    = active->calendar_hash;
  if (event.occasion_time < active->source.valid_from || event.occasion_time < active->source.activation_epoch ||
      event.occasion_time >= active->source.valid_until) {
    return finish(ntn_initial_access_plan_decision::reject,
                  ntn_initial_access_plan_reason::active_plan_not_valid,
                  "rejected_outside_active_validity_no_rf_evidence");
  }
  if (event.satellite_id != active->source.satellite_id) {
    return finish(ntn_initial_access_plan_decision::reject,
                  ntn_initial_access_plan_reason::satellite_mismatch,
                  "rejected_active_plan_metadata_mismatch_no_rf_evidence");
  }
  if (event.catalog_version != active->source.catalog_version) {
    return finish(ntn_initial_access_plan_decision::reject,
                  ntn_initial_access_plan_reason::catalog_version_mismatch,
                  "rejected_active_plan_metadata_mismatch_no_rf_evidence");
  }
  if (event.schedule_version != active->source.schedule_version) {
    return finish(ntn_initial_access_plan_decision::reject,
                  ntn_initial_access_plan_reason::schedule_version_mismatch,
                  "rejected_active_plan_metadata_mismatch_no_rf_evidence");
  }
  if (normalize_hash(event.source_content_hash) != normalize_hash(active->source.content_hash)) {
    return finish(ntn_initial_access_plan_decision::reject,
                  ntn_initial_access_plan_reason::source_hash_mismatch,
                  "rejected_active_plan_metadata_mismatch_no_rf_evidence");
  }
  if (normalize_hash(event.calendar_hash) != normalize_hash(active->calendar_hash)) {
    return finish(ntn_initial_access_plan_decision::reject,
                  ntn_initial_access_plan_reason::calendar_hash_mismatch,
                  "rejected_active_plan_metadata_mismatch_no_rf_evidence");
  }

  const ntn_onboard_cell_position_set* owner = nullptr;
  for (const ntn_onboard_cell_position_set& cell : active->cell_positions) {
    if (cell_identity_equal(cell.identity, event.cell)) {
      owner = &cell;
      break;
    }
  }
  if (owner == nullptr) {
    return finish(ntn_initial_access_plan_decision::reject,
                  ntn_initial_access_plan_reason::cell_identity_mismatch,
                  "rejected_active_plan_cell_mismatch_no_rf_evidence");
  }
  if (std::find(owner->assigned_l1_ids.begin(), owner->assigned_l1_ids.end(), event.position_id) ==
      owner->assigned_l1_ids.end()) {
    return finish(ntn_initial_access_plan_decision::reject,
                  ntn_initial_access_plan_reason::position_not_assigned_to_cell,
                  "rejected_active_plan_position_owner_mismatch_no_rf_evidence");
  }
  if (cfg.max_prach_interval.count() <= 0) {
    return finish(ntn_initial_access_plan_decision::reject,
                  ntn_initial_access_plan_reason::prach_occasion_not_scheduled,
                  "rejected_invalid_calendar_cycle_no_rf_evidence");
  }

  const std::chrono::microseconds since_activation =
      std::chrono::duration_cast<std::chrono::microseconds>(event.occasion_time - active->source.activation_epoch);
  result.occasion_offset = std::chrono::microseconds{since_activation.count() % cfg.max_prach_interval.count()};
  const auto ro_it =
      std::find_if(active->access_calendar.begin(), active->access_calendar.end(), [&](const auto& intent) {
        return intent.schedule_version == event.schedule_version && intent.nci == event.cell.nci &&
               intent.position_id == event.position_id && intent.purpose == ntn_access_calendar_purpose::prach_ro &&
               result.occasion_offset >= intent.start_time &&
               result.occasion_offset < intent.start_time + intent.duration;
      });
  if (ro_it == active->access_calendar.end()) {
    return finish(ntn_initial_access_plan_decision::reject,
                  ntn_initial_access_plan_reason::prach_occasion_not_scheduled,
                  "rejected_outside_active_prach_window_no_rf_evidence");
  }

  const auto beam_it =
      std::find_if(active->access_calendar.begin(), active->access_calendar.end(), [&](const auto& intent) {
        return intent.schedule_version == ro_it->schedule_version && intent.nci == ro_it->nci &&
               intent.position_id == ro_it->position_id &&
               intent.purpose == ntn_access_calendar_purpose::prach_ul_beam && intent.start_time == ro_it->start_time &&
               intent.duration == ro_it->duration;
      });
  if (beam_it == active->access_calendar.end()) {
    return finish(ntn_initial_access_plan_decision::reject,
                  ntn_initial_access_plan_reason::prach_ul_beam_missing,
                  "rejected_prach_without_ul_beam_no_rf_evidence");
  }
  if (beam_it->port_id != event.ul_beam_port_id) {
    return finish(ntn_initial_access_plan_decision::reject,
                  ntn_initial_access_plan_reason::resource_port_mismatch,
                  "rejected_ul_beam_port_mismatch_no_rf_evidence");
  }
  if (cfg.require_external_apply && !active_external_apply_evidence) {
    return finish(ntn_initial_access_plan_decision::reject,
                  ntn_initial_access_plan_reason::missing_external_apply_evidence,
                  "rejected_without_software_gate_snapshot_no_rf_evidence");
  }
  if (!cfg.require_external_apply) {
    return finish(ntn_initial_access_plan_decision::audit_only,
                  ntn_initial_access_plan_reason::intent_only_plan,
                  "cu_cp_intent_calendar_event_match_no_du_or_rf_evidence");
  }
  return finish(ntn_initial_access_plan_decision::accept,
                ntn_initial_access_plan_reason::none,
                "cu_cp_active_plan_event_match_software_gate_snapshot_no_rf_evidence");
}

ntn_position_plan_submit_result ntn_onboard_position_plan_controller::reject(ntn_position_plan_reject_reason reason,
                                                                             uint64_t schedule_version)
{
  current_stage         = reason == ntn_position_plan_reject_reason::feature_disabled
                              ? ntn_position_plan_stage::disabled
                              : ntn_position_plan_stage::rejected;
  last_rejection        = reason;
  last_rejected_version = schedule_version;
  return {false, current_stage, reason};
}

ntn_position_plan_reject_reason
ntn_onboard_position_plan_controller::validate_plan_for_restore(const ntn_versioned_position_plan&    plan,
                                                                std::chrono::system_clock::time_point now) const
{
  // Restore must validate the complete planning contract while deliberately ignoring the live controller's
  // monotonic gate. The persisted high-water is checked independently before it is installed.
  ntn_onboard_position_plan_controller pristine{cfg};
  return pristine.validate_plan(plan, now);
}

expected<ntn_activated_position_plan, std::string> ntn_onboard_position_plan_controller::rebuild_persisted_snapshot(
    const ntn_versioned_position_plan&                  source,
    const std::array<ntn_onboard_cell_position_set, 2>& cell_positions,
    const std::string&                                  calendar_hash,
    std::chrono::system_clock::time_point               now) const
{
  const ntn_position_plan_reject_reason source_error = validate_plan_for_restore(source, now);
  if (source_error != ntn_position_plan_reject_reason::none) {
    return make_unexpected(fmt::format("persisted source rejected: {}", to_string(source_error)));
  }

  std::array<ntn_onboard_cell_position_set, 2> normalized = cell_positions;
  if (normalized[1].identity.nci < normalized[0].identity.nci) {
    std::swap(normalized[0], normalized[1]);
  }
  for (unsigned i = 0; i != normalized.size(); ++i) {
    if (!cell_identity_equal(normalized[i].identity, cfg.onboard_cells[i])) {
      return make_unexpected(std::string{"persisted cell assignment identity mismatch"});
    }
    std::sort(normalized[i].assigned_l1_ids.begin(), normalized[i].assigned_l1_ids.end());
    if (normalized[i].assigned_l1_ids.size() > cfg.max_l1_positions_per_cell) {
      return make_unexpected(std::string{"persisted cell assignment exceeds capacity"});
    }
  }

  const std::vector<std::string> effective_ids = effective_assigned_l1_ids(source);
  const std::set<std::string>    expected_ids(effective_ids.begin(), effective_ids.end());
  std::set<std::string> assigned_ids;
  for (const ntn_onboard_cell_position_set& cell : normalized) {
    for (const std::string& position_id : cell.assigned_l1_ids) {
      if (!expected_ids.count(position_id) || !assigned_ids.insert(position_id).second) {
        return make_unexpected(std::string{"persisted assignment is not an exact partition"});
      }
    }
  }
  if (assigned_ids != expected_ids) {
    return make_unexpected(std::string{"persisted assignment omits assigned L1 positions"});
  }

  ntn_activated_position_plan rebuilt;
  rebuilt.source          = source;
  rebuilt.cell_positions  = std::move(normalized);
  rebuilt.access_calendar = build_access_calendar(source.schedule_version, rebuilt.cell_positions);
  rebuilt.calendar_audit =
      audit_access_calendar(source.schedule_version, rebuilt.cell_positions, rebuilt.access_calendar);
  if (!rebuilt.calendar_audit.accepted) {
    return make_unexpected(fmt::format("persisted calendar rejected: {}", to_string(rebuilt.calendar_audit.reason)));
  }
  for (ntn_access_calendar_intent& intent : rebuilt.access_calendar) {
    intent.state = ntn_access_calendar_state::checked;
  }
  rebuilt.calendar_hash = compute_ntn_access_calendar_hash(source.schedule_version, rebuilt.access_calendar);
  if (normalize_hash(rebuilt.calendar_hash) != normalize_hash(calendar_hash)) {
    return make_unexpected(std::string{"persisted calendar hash mismatch"});
  }
  return rebuilt;
}

expected<void, std::string>
ntn_onboard_position_plan_controller::restore_persistent_state(const ntn_onboard_position_plan_persistent_state& state,
                                                               std::chrono::system_clock::time_point             now)
{
  if (!cfg.enabled || !cfg.require_external_apply) {
    return make_unexpected(std::string{"state restore requires the execution profile"});
  }
  const auto configured_cells = sorted_cell_identities(cfg.onboard_cells);
  const auto persisted_cells  = sorted_cell_identities(state.onboard_cells);
  const bool identities_match = cell_identity_equal(configured_cells[0], persisted_cells[0]) &&
                                cell_identity_equal(configured_cells[1], persisted_cells[1]);
  const bool context_matches =
      state.satellite_id == cfg.satellite_id && state.planning_context.catalog_id == cfg.expected_catalog_id &&
      normalize_hash(state.planning_context.catalog_hash) == normalize_hash(cfg.expected_catalog_hash) &&
      state.planning_context.identity_registry_version == cfg.expected_identity_registry_version &&
      normalize_hash(state.planning_context.identity_registry_hash) ==
          normalize_hash(cfg.expected_identity_registry_hash) &&
      state.planning_context.access_profile_id == cfg.expected_access_profile_id &&
      normalize_hash(state.planning_context.access_profile_hash) == normalize_hash(cfg.expected_access_profile_hash) &&
      identities_match;
  if (!context_matches || !state.du_reconciliation_required) {
    return make_unexpected(std::string{"persisted planning context or identity mismatch"});
  }

  uint64_t   max_catalog_version  = 0;
  uint64_t   max_schedule_version = 0;
  const auto observe_version      = [&](const std::optional<ntn_onboard_position_plan_state_snapshot>& snapshot) {
    if (snapshot.has_value()) {
      max_catalog_version  = std::max(max_catalog_version, snapshot->source.catalog_version);
      max_schedule_version = std::max(max_schedule_version, snapshot->source.schedule_version);
    }
  };
  observe_version(state.active);
  observe_version(state.pending);
  if (state.generation == 0 || state.highest_catalog_version < max_catalog_version ||
      state.highest_schedule_version < max_schedule_version) {
    return make_unexpected(std::string{"persisted version high-water is inconsistent"});
  }

  std::optional<ntn_activated_position_plan> restored_active;
  std::optional<ntn_activated_position_plan> restored_pending;
  if (state.active.has_value() && now < state.active->source.valid_until) {
    auto rebuilt = rebuild_persisted_snapshot(
        state.active->source, state.active->cell_positions, state.active->calendar_hash, now);
    if (!rebuilt.has_value()) {
      return make_unexpected(fmt::format("active snapshot: {}", rebuilt.error()));
    }
    if (now < rebuilt->source.activation_epoch) {
      return make_unexpected(std::string{"persisted active snapshot has a future activation epoch"});
    }
    restored_active = std::move(rebuilt.value());
  }
  if (state.pending.has_value() && now < state.pending->source.valid_until) {
    auto rebuilt = rebuild_persisted_snapshot(
        state.pending->source, state.pending->cell_positions, state.pending->calendar_hash, now);
    if (!rebuilt.has_value()) {
      return make_unexpected(fmt::format("pending snapshot: {}", rebuilt.error()));
    }
    restored_pending = std::move(rebuilt.value());
  }
  if (restored_active.has_value() && restored_pending.has_value() &&
      restored_pending->source.schedule_version <= restored_active->source.schedule_version) {
    return make_unexpected(std::string{"persisted pending version does not follow active version"});
  }

  std::array<ntn_onboard_cell_position_set, 2> restored_sticky = state.sticky_partition;
  if (restored_sticky[1].identity.nci < restored_sticky[0].identity.nci) {
    std::swap(restored_sticky[0], restored_sticky[1]);
  }
  std::set<std::string> sticky_ids;
  for (unsigned i = 0; i != restored_sticky.size(); ++i) {
    if (!cell_identity_equal(restored_sticky[i].identity, cfg.onboard_cells[i]) ||
        restored_sticky[i].assigned_l1_ids.size() > cfg.max_l1_positions_per_cell) {
      return make_unexpected(std::string{"persisted sticky partition identity or capacity mismatch"});
    }
    std::sort(restored_sticky[i].assigned_l1_ids.begin(), restored_sticky[i].assigned_l1_ids.end());
    for (const std::string& position_id : restored_sticky[i].assigned_l1_ids) {
      if (!is_valid_l1_id(position_id) || !sticky_ids.insert(position_id).second) {
        return make_unexpected(std::string{"persisted sticky partition is invalid"});
      }
    }
  }

  const bool recorded_stage_has_du_claim =
      state.recorded_deployment_stage == ntn_position_plan_deployment_stage::preparing ||
      state.recorded_deployment_stage == ntn_position_plan_deployment_stage::ready ||
      state.recorded_deployment_stage == ntn_position_plan_deployment_stage::applied;
  const auto deployment_matches = [&](const std::optional<ntn_activated_position_plan>& plan) {
    return plan.has_value() && plan->source.schedule_version == state.recorded_deployment_schedule_version &&
           normalize_hash(plan->calendar_hash) == normalize_hash(state.recorded_deployment_calendar_hash);
  };
  const auto persisted_deployment_matches =
      [&](const std::optional<ntn_onboard_position_plan_state_snapshot>& snapshot) {
        return snapshot.has_value() &&
               snapshot->source.schedule_version == state.recorded_deployment_schedule_version &&
               normalize_hash(snapshot->calendar_hash) == normalize_hash(state.recorded_deployment_calendar_hash);
      };
  const bool deployment_matches_pending = deployment_matches(restored_pending);
  if (recorded_stage_has_du_claim && !persisted_deployment_matches(state.active) &&
      !persisted_deployment_matches(state.pending)) {
    return make_unexpected(std::string{"persisted deployment identity does not match a stored snapshot"});
  }

  highest_catalog_version  = state.highest_catalog_version;
  highest_schedule_version = state.highest_schedule_version;
  sticky_partition         = std::move(restored_sticky);
  received_plan_present    = false;
  last_received_catalog    = 0;
  last_received_schedule   = 0;
  last_received_hash.clear();
  last_received_activation = {};
  last_candidate_inventory.clear();
  last_assigned_l1_position_ids.clear();
  const ntn_onboard_position_plan_state_snapshot* latest_snapshot =
      state.pending.has_value() &&
              (!state.active.has_value() ||
               state.pending->source.schedule_version > state.active->source.schedule_version)
          ? &*state.pending
          : (state.active.has_value() ? &*state.active : nullptr);
  if (state.received_plan.has_value()) {
    received_plan_present      = true;
    last_received_catalog      = state.received_plan->catalog_version;
    last_received_schedule     = state.received_plan->schedule_version;
    last_received_hash         = state.received_plan->content_hash;
    last_received_activation   = state.received_plan->activation_epoch;
    last_candidate_inventory   = state.received_plan->candidate_inventory;
    last_assigned_l1_position_ids = state.received_plan->assigned_l1_position_ids;
    if (state.schema_version < 3 && last_assigned_l1_position_ids.empty()) {
      for (const ntn_l1_position& position : last_candidate_inventory) {
        last_assigned_l1_position_ids.push_back(position.position_id);
      }
    }
  } else if (state.schema_version == 1 && latest_snapshot != nullptr) {
    received_plan_present      = true;
    last_received_catalog      = latest_snapshot->source.catalog_version;
    last_received_schedule     = latest_snapshot->source.schedule_version;
    last_received_hash         = latest_snapshot->source.content_hash;
    last_received_activation   = latest_snapshot->source.activation_epoch;
    last_candidate_inventory = latest_snapshot->source.visible_l1_positions;
    last_assigned_l1_position_ids = effective_assigned_l1_ids(latest_snapshot->source);
  }
  active.reset();
  pending.reset();
  recovery_candidate.reset();
  recovery_fallback_active.reset();
  deferred_pending.reset();
  active_external_apply_evidence = false;

  // A crash can occur after B was prepared/applied but before B atomically replaced active A. In that window the
  // recorded deployment identity is authoritative for query ordering: reconcile B first while retaining A as a
  // hidden, fail-closed fallback. Neither persisted plan is exposed as active DU evidence before a matching query.
  if (restored_pending.has_value() && recorded_stage_has_du_claim && deployment_matches_pending) {
    recovery_candidate            = std::move(restored_pending);
    recovery_fallback_active      = std::move(restored_active);
    recovery_candidate_was_active = false;
  } else if (restored_active.has_value()) {
    recovery_candidate            = std::move(restored_active);
    deferred_pending              = std::move(restored_pending);
    recovery_candidate_was_active = true;
  } else if (restored_pending.has_value() &&
             state.recorded_deployment_stage != ntn_position_plan_deployment_stage::not_sent &&
             state.recorded_deployment_stage != ntn_position_plan_deployment_stage::disabled &&
             state.recorded_deployment_stage != ntn_position_plan_deployment_stage::rejected &&
             state.recorded_deployment_stage != ntn_position_plan_deployment_stage::unsupported) {
    recovery_candidate            = std::move(restored_pending);
    recovery_candidate_was_active = false;
  } else {
    pending                       = std::move(restored_pending);
    recovery_candidate_was_active = false;
  }

  if (recovery_candidate.has_value()) {
    last_recovery_schedule_version = recovery_candidate->source.schedule_version;
    recovery                       = ntn_position_plan_recovery_stage::reconciling;
    recovery_reason                = "awaiting_matching_du_query_after_restart";
    // The persisted stage is historical ordering input only. Never expose it as current applied evidence before a
    // matching query from the live DU connection completes.
    deployment        = ntn_position_plan_deployment_stage::preparing;
    deployment_reason = "du_reconciliation_in_progress";
    current_stage = ntn_position_plan_stage::pending;
  } else {
    last_recovery_schedule_version = 0;
    recovery                       = ntn_position_plan_recovery_stage::reconciled;
    recovery_reason                = "persisted_state_has_no_live_du_claim";
    deployment                     = pending.has_value() ? ntn_position_plan_deployment_stage::not_sent
                                                         : ntn_position_plan_deployment_stage::disabled;
    deployment_reason = pending.has_value() ? "restored_pending_requires_new_prepare" : "no_pending_deployment";
    current_stage     = pending.has_value() ? ntn_position_plan_stage::pending : ntn_position_plan_stage::received;
  }
  return {};
}

ntn_onboard_position_plan_persistent_state
ntn_onboard_position_plan_controller::make_persistent_state(uint64_t generation) const
{
  ntn_onboard_position_plan_persistent_state result;
  result.generation                                 = generation;
  result.satellite_id                               = cfg.satellite_id;
  result.planning_context.catalog_id                = cfg.expected_catalog_id;
  result.planning_context.catalog_hash              = cfg.expected_catalog_hash;
  result.planning_context.identity_registry_version = cfg.expected_identity_registry_version;
  result.planning_context.identity_registry_hash    = cfg.expected_identity_registry_hash;
  result.planning_context.access_profile_id         = cfg.expected_access_profile_id;
  result.planning_context.access_profile_hash       = cfg.expected_access_profile_hash;
  result.onboard_cells                              = cfg.onboard_cells;
  result.highest_catalog_version                    = highest_catalog_version;
  result.highest_schedule_version                   = highest_schedule_version;
  result.sticky_partition                           = sticky_partition;
  if (received_plan_present) {
    result.received_plan = ntn_onboard_position_plan_received_observation{last_received_catalog,
                                                                          last_received_schedule,
                                                                          last_received_hash,
                                                                          last_received_activation,
                                                                          last_candidate_inventory,
                                                                          last_assigned_l1_position_ids};
  }
  const auto to_snapshot                            = [](const ntn_activated_position_plan& plan) {
    return ntn_onboard_position_plan_state_snapshot{plan.source, plan.cell_positions, plan.calendar_hash};
  };
  if (recovery_candidate.has_value()) {
    if (recovery_candidate_was_active) {
      result.active = to_snapshot(*recovery_candidate);
      if (deferred_pending.has_value()) {
        result.pending = to_snapshot(*deferred_pending);
      }
    } else {
      result.pending = to_snapshot(*recovery_candidate);
      if (recovery_fallback_active.has_value()) {
        result.active = to_snapshot(*recovery_fallback_active);
      }
    }
  } else {
    if (active.has_value()) {
      result.active = to_snapshot(*active);
    } else if (recovery_fallback_active.has_value()) {
      // A future recovered plan can already have matching DU apply evidence while its activation epoch is still in
      // the future. Keep the hidden historical plan in the durable snapshot until the replacement is activated and
      // its clear obligation is recorded.
      result.active = to_snapshot(*recovery_fallback_active);
    }
    if (pending.has_value()) {
      result.pending = to_snapshot(*pending);
    }
  }
  result.recorded_deployment_stage  = deployment;
  result.recorded_deployment_detail = deployment_reason;
  const ntn_activated_position_plan* deployment_plan =
      recovery_candidate.has_value() ? &*recovery_candidate
                                     : (pending.has_value() ? &*pending : (active.has_value() ? &*active : nullptr));
  if (deployment_plan != nullptr) {
    result.recorded_deployment_schedule_version = deployment_plan->source.schedule_version;
    result.recorded_deployment_calendar_hash    = deployment_plan->calendar_hash;
  }
  result.du_reconciliation_required = true;
  return result;
}

bool ntn_onboard_position_plan_controller::require_du_reconciliation_after_connection_loss(std::string detail)
{
  if (!cfg.enabled || !cfg.require_external_apply) {
    return false;
  }

  const std::string recovery_detail =
      detail.empty() ? "du_connection_lost_awaiting_matching_query" : std::move(detail);
  if (recovery_candidate.has_value()) {
    const bool changed = recovery != ntn_position_plan_recovery_stage::reconciling ||
                         recovery_reason != recovery_detail ||
                         deployment != ntn_position_plan_deployment_stage::preparing ||
                         current_stage != ntn_position_plan_stage::pending || active_external_apply_evidence;
    active_external_apply_evidence = false;
    recovery                       = ntn_position_plan_recovery_stage::reconciling;
    recovery_reason                = recovery_detail;
    deployment                     = ntn_position_plan_deployment_stage::preparing;
    deployment_reason              = "du_connection_lost_reconciliation_required";
    current_stage                  = ntn_position_plan_stage::pending;
    last_recovery_schedule_version = recovery_candidate->source.schedule_version;
    return changed;
  }

  const bool pending_may_have_reached_du = pending.has_value() &&
                                           (deployment == ntn_position_plan_deployment_stage::preparing ||
                                            deployment == ntn_position_plan_deployment_stage::ready ||
                                            deployment == ntn_position_plan_deployment_stage::applied);
  if (!active.has_value() && !pending_may_have_reached_du) {
    return false;
  }

  deferred_pending.reset();
  if (pending_may_have_reached_du) {
    recovery_candidate            = std::move(pending);
    if (active.has_value()) {
      recovery_fallback_active = std::move(active);
    }
    recovery_candidate_was_active = false;
  } else {
    recovery_fallback_active.reset();
    recovery_candidate            = std::move(active);
    deferred_pending              = std::move(pending);
    recovery_candidate_was_active = true;
  }
  active.reset();
  pending.reset();
  active_external_apply_evidence = false;
  recovery                       = ntn_position_plan_recovery_stage::reconciling;
  recovery_reason                = recovery_detail;
  deployment                     = ntn_position_plan_deployment_stage::preparing;
  deployment_reason              = "du_connection_lost_reconciliation_required";
  current_stage                  = ntn_position_plan_stage::pending;
  last_recovery_schedule_version = recovery_candidate->source.schedule_version;
  return true;
}

bool ntn_onboard_position_plan_controller::confirm_recovery_applied(uint64_t           schedule_version,
                                                                    const std::string& calendar_hash,
                                                                    std::chrono::system_clock::time_point now,
                                                                    bool                                   allow_confirmation,
                                                                    bool                                   defer_pending_activation)
{
  if (!recovery_candidate.has_value() || recovery_candidate->source.schedule_version != schedule_version ||
      normalize_hash(recovery_candidate->calendar_hash) != normalize_hash(calendar_hash) ||
      now >= recovery_candidate->source.valid_until) {
    return false;
  }
  if (!allow_confirmation) {
    // The matching response may be retried after a safe restart. Do not expose recovered application evidence that
    // cannot be committed to the state file.
    return false;
  }
  if (recovery_candidate_was_active) {
    active = std::move(recovery_candidate);
    recovery_candidate.reset();
    sticky_partition               = active->cell_positions;
    active_external_apply_evidence = true;
    pending                        = std::move(deferred_pending);
    deferred_pending.reset();
    deployment        = pending.has_value() ? ntn_position_plan_deployment_stage::not_sent
                                            : ntn_position_plan_deployment_stage::applied;
    deployment_reason = pending.has_value() ? "restored_pending_requires_new_prepare" : "du_reconciled_after_restart";
    current_stage = pending.has_value() ? ntn_position_plan_stage::pending : ntn_position_plan_stage::active;
  } else {
    pending = std::move(recovery_candidate);
    recovery_candidate.reset();
    deployment        = ntn_position_plan_deployment_stage::applied;
    deployment_reason = "du_reconciled_after_restart";
    if (!defer_pending_activation) {
      advance_time(now);
    }
  }
  recovery_candidate_was_active = false;
  recovery                      = ntn_position_plan_recovery_stage::reconciled;
  recovery_reason               = "matching_du_applied_query_confirmed";
  return true;
}

bool ntn_onboard_position_plan_controller::reject_recovery(uint64_t                        schedule_version,
                                                           const std::string&              calendar_hash,
                                                           ntn_position_plan_reject_reason reason,
                                                           std::string                     detail)
{
  if (!recovery_candidate.has_value() || recovery_candidate->source.schedule_version != schedule_version ||
      normalize_hash(recovery_candidate->calendar_hash) != normalize_hash(calendar_hash)) {
    return false;
  }
  if (!recovery_candidate_was_active && recovery_fallback_active.has_value()) {
    const uint64_t    failed_pending_version = recovery_candidate->source.schedule_version;
    const std::string failure_detail         = detail.empty() ? to_string(reason) : detail;
    recovery_candidate                       = std::move(recovery_fallback_active);
    recovery_fallback_active.reset();
    recovery_candidate_was_active = true;
    deferred_pending.reset();
    active.reset();
    pending.reset();
    active_external_apply_evidence = false;
    deployment                     = ntn_position_plan_deployment_stage::preparing;
    deployment_reason              = "historical_active_fallback_requires_du_reconciliation";
    recovery                       = ntn_position_plan_recovery_stage::reconciling;
    recovery_reason                = fmt::format("pending_recovery_failed_querying_active_fallback:{}", failure_detail);
    last_recovery_schedule_version = recovery_candidate->source.schedule_version;
    reject(reason, failed_pending_version);
    current_stage = ntn_position_plan_stage::pending;
    return true;
  }
  recovery_candidate.reset();
  recovery_fallback_active.reset();
  recovery_candidate_was_active = false;
  active.reset();
  active_external_apply_evidence = false;
  pending                        = std::move(deferred_pending);
  deferred_pending.reset();
  deployment =
      pending.has_value() ? ntn_position_plan_deployment_stage::not_sent : ntn_position_plan_deployment_stage::rejected;
  deployment_reason = detail.empty() ? to_string(reason) : detail;
  recovery          = ntn_position_plan_recovery_stage::failed;
  recovery_reason   = detail.empty() ? to_string(reason) : std::move(detail);
  reject(reason, schedule_version);
  if (pending.has_value()) {
    current_stage = ntn_position_plan_stage::pending;
  }
  return true;
}

std::optional<ntn_activated_position_plan>
ntn_onboard_position_plan_controller::take_expired_recovery_fallback(
    std::chrono::system_clock::time_point now)
{
  if (!recovery_fallback_active.has_value() || now < recovery_fallback_active->source.valid_until) {
    return std::nullopt;
  }

  std::optional<ntn_activated_position_plan> expired = std::move(recovery_fallback_active);
  recovery_fallback_active.reset();
  return expired;
}

ntn_position_plan_submit_result ntn_onboard_position_plan_controller::submit(
    const ntn_versioned_position_plan& plan, std::chrono::system_clock::time_point now)
{
  current_stage              = cfg.enabled ? ntn_position_plan_stage::received : ntn_position_plan_stage::disabled;
  received_plan_present      = true;
  last_candidate_inventory   = plan.visible_l1_positions;
  last_assigned_l1_position_ids = effective_assigned_l1_ids(plan);
  last_received_catalog      = plan.catalog_version;
  last_received_schedule     = plan.schedule_version;
  last_received_hash         = plan.content_hash;
  last_received_activation   = plan.activation_epoch;
  const auto validation_error = validate_plan(plan, now);
  if (validation_error != ntn_position_plan_reject_reason::none) {
    return reject(validation_error, plan.schedule_version);
  }
  current_stage = ntn_position_plan_stage::validated;

  ntn_activated_position_plan candidate;
  candidate.source         = plan;
  candidate.cell_positions = partition_positions(select_assigned_l1_positions(plan));
  candidate.access_calendar = build_access_calendar(plan.schedule_version, candidate.cell_positions);
  candidate.calendar_audit  = audit_access_calendar(plan.schedule_version,
                                                    candidate.cell_positions,
                                                    candidate.access_calendar);
  if (!candidate.calendar_audit.accepted) {
    return reject(candidate.calendar_audit.reason, plan.schedule_version);
  }
  for (ntn_access_calendar_intent& intent : candidate.access_calendar) {
    intent.state = ntn_access_calendar_state::checked;
  }
  candidate.calendar_hash = compute_ntn_access_calendar_hash(plan.schedule_version, candidate.access_calendar);
  if (candidate.calendar_hash.empty()) {
    return reject(ntn_position_plan_reject_reason::invalid_hash, plan.schedule_version);
  }
  current_stage = ntn_position_plan_stage::calendar_checked;

  // Do not disturb an existing pending plan until the replacement has passed every check.
  pending         = std::move(candidate);
  highest_catalog_version  = std::max(highest_catalog_version, plan.catalog_version);
  highest_schedule_version = std::max(highest_schedule_version, plan.schedule_version);
  current_stage   = ntn_position_plan_stage::pending;
  deployment      = cfg.require_external_apply ? ntn_position_plan_deployment_stage::not_sent
                                                : ntn_position_plan_deployment_stage::disabled;
  deployment_reason = cfg.require_external_apply ? "not_sent" : "external_execution_disabled";
  advance_time(now);
  return {true, current_stage, ntn_position_plan_reject_reason::none};
}

bool ntn_onboard_position_plan_controller::advance_time(std::chrono::system_clock::time_point now,
                                                         bool                                   allow_activation)
{
  if (recovery_candidate.has_value() && now >= recovery_candidate->source.valid_until) {
    const uint64_t    expired_version = recovery_candidate->source.schedule_version;
    const std::string expired_hash    = recovery_candidate->calendar_hash;
    reject_recovery(expired_version,
                    expired_hash,
                    ntn_position_plan_reject_reason::du_reconciliation_failed,
                    "persisted_plan_expired_before_du_reconciliation");
  }
  if (active.has_value() && now >= active->source.valid_until) {
    const uint64_t expired_version = active->source.schedule_version;
    active.reset();
    active_external_apply_evidence = false;
    if (!pending.has_value()) {
      deployment = cfg.require_external_apply ? ntn_position_plan_deployment_stage::rejected
                                              : ntn_position_plan_deployment_stage::disabled;
      deployment_reason = cfg.require_external_apply ? "active_plan_expired" : "external_execution_disabled";
    }
    reject(ntn_position_plan_reject_reason::expired, expired_version);
  }
  if (!pending.has_value() || now < pending->source.activation_epoch) {
    if (pending.has_value()) {
      current_stage = ntn_position_plan_stage::pending;
    }
    return false;
  }
  if (now >= pending->source.valid_until) {
    const uint64_t expired_version = pending->source.schedule_version;
    pending.reset();
    if (recovery_fallback_active.has_value()) {
      recovery_candidate = std::move(recovery_fallback_active);
      recovery_fallback_active.reset();
      recovery_candidate_was_active = true;
      deferred_pending.reset();
      active_external_apply_evidence = false;
      deployment                     = ntn_position_plan_deployment_stage::preparing;
      deployment_reason              = "historical_active_fallback_requires_du_reconciliation";
      recovery                       = ntn_position_plan_recovery_stage::reconciling;
      recovery_reason                = "expired_pending_querying_historical_active_fallback";
      last_recovery_schedule_version = recovery_candidate->source.schedule_version;
      reject(ntn_position_plan_reject_reason::expired, expired_version);
      current_stage = ntn_position_plan_stage::pending;
      return false;
    }
    reject(ntn_position_plan_reject_reason::expired, expired_version);
    return false;
  }

  if (!allow_activation) {
    current_stage = ntn_position_plan_stage::pending;
    return false;
  }

  if (cfg.require_external_apply && deployment != ntn_position_plan_deployment_stage::applied) {
    current_stage = ntn_position_plan_stage::pending;
    return false;
  }

  active        = std::move(pending);
  pending.reset();
  recovery_fallback_active.reset();
  sticky_partition               = active->cell_positions;
  active_external_apply_evidence = cfg.require_external_apply;
  current_stage = ntn_position_plan_stage::active;
  return true;
}

bool ntn_onboard_position_plan_controller::mark_deployment_preparing(uint64_t           schedule_version,
                                                                     const std::string& calendar_hash)
{
  if (!pending.has_value() || pending->source.schedule_version != schedule_version ||
      normalize_hash(pending->calendar_hash) != normalize_hash(calendar_hash)) {
    return false;
  }
  if (deployment == ntn_position_plan_deployment_stage::preparing ||
      deployment == ntn_position_plan_deployment_stage::ready ||
      deployment == ntn_position_plan_deployment_stage::applied) {
    return true;
  }
  if (deployment != ntn_position_plan_deployment_stage::not_sent) {
    return false;
  }
  deployment        = ntn_position_plan_deployment_stage::preparing;
  deployment_reason = "prepare_sent";
  return true;
}

bool ntn_onboard_position_plan_controller::mark_deployment_retryable(uint64_t           schedule_version,
                                                                     const std::string& calendar_hash,
                                                                     std::string        detail)
{
  if (!pending.has_value() || pending->source.schedule_version != schedule_version ||
      normalize_hash(pending->calendar_hash) != normalize_hash(calendar_hash)) {
    return false;
  }
  if (deployment == ntn_position_plan_deployment_stage::ready ||
      deployment == ntn_position_plan_deployment_stage::applied) {
    // A matching lower-state response may arrive after a query has already advanced the same plan. Acknowledge it
    // without regressing state so callers do not treat it as an orphan deployment that must be cleared.
    return true;
  }
  if (deployment != ntn_position_plan_deployment_stage::not_sent &&
      deployment != ntn_position_plan_deployment_stage::preparing) {
    return false;
  }
  deployment        = ntn_position_plan_deployment_stage::not_sent;
  deployment_reason = detail.empty() ? "retry_pending" : std::move(detail);
  return true;
}

bool ntn_onboard_position_plan_controller::mark_deployment_ready(uint64_t           schedule_version,
                                                                 const std::string& calendar_hash)
{
  if (!pending.has_value() || pending->source.schedule_version != schedule_version ||
      normalize_hash(pending->calendar_hash) != normalize_hash(calendar_hash)) {
    return false;
  }
  if (deployment == ntn_position_plan_deployment_stage::ready ||
      deployment == ntn_position_plan_deployment_stage::applied) {
    return true;
  }
  if (deployment != ntn_position_plan_deployment_stage::preparing) {
    return false;
  }
  deployment        = ntn_position_plan_deployment_stage::ready;
  deployment_reason = "du_ready";
  return true;
}

bool ntn_onboard_position_plan_controller::mark_deployment_applied(uint64_t           schedule_version,
                                                                   const std::string& calendar_hash)
{
  if (!pending.has_value() || pending->source.schedule_version != schedule_version ||
      normalize_hash(pending->calendar_hash) != normalize_hash(calendar_hash)) {
    return false;
  }
  if (deployment == ntn_position_plan_deployment_stage::applied) {
    return true;
  }
  if (deployment != ntn_position_plan_deployment_stage::preparing &&
      deployment != ntn_position_plan_deployment_stage::ready) {
    return false;
  }
  deployment        = ntn_position_plan_deployment_stage::applied;
  deployment_reason = "ssb_prach_software_gate_applied_no_position_or_rf_evidence";
  return true;
}

bool ntn_onboard_position_plan_controller::reject_pending_deployment(
    uint64_t                        schedule_version,
    const std::string&              calendar_hash,
    ntn_position_plan_reject_reason reason,
    std::string                     detail)
{
  if (!pending.has_value() || pending->source.schedule_version != schedule_version ||
      normalize_hash(pending->calendar_hash) != normalize_hash(calendar_hash)) {
    return false;
  }
  const std::string failure_detail = detail.empty() ? to_string(reason) : detail;
  pending.reset();
  if (recovery_fallback_active.has_value()) {
    // A newer plan can supersede an early-applied future update while the historical active plan is intentionally
    // hidden. If that newer deployment fails, return to live-DU reconciliation of the historical plan instead of
    // losing the only safe fallback until the next process restart.
    recovery_candidate            = std::move(recovery_fallback_active);
    recovery_fallback_active.reset();
    recovery_candidate_was_active = true;
    deferred_pending.reset();
    active.reset();
    active_external_apply_evidence = false;
    deployment                     = ntn_position_plan_deployment_stage::preparing;
    deployment_reason              = "historical_active_fallback_requires_du_reconciliation";
    recovery                       = ntn_position_plan_recovery_stage::reconciling;
    recovery_reason = fmt::format("pending_deployment_failed_querying_historical_active_fallback:{}", failure_detail);
    last_recovery_schedule_version = recovery_candidate->source.schedule_version;
    reject(reason, schedule_version);
    current_stage = ntn_position_plan_stage::pending;
    return true;
  }
  deployment = reason == ntn_position_plan_reject_reason::execution_unsupported
                   ? ntn_position_plan_deployment_stage::unsupported
                   : ntn_position_plan_deployment_stage::rejected;
  deployment_reason = std::move(failure_detail);
  reject(reason, schedule_version);
  return true;
}

void ntn_onboard_position_plan_controller::record_external_rejection(ntn_position_plan_reject_reason reason,
                                                                      uint64_t schedule_version)
{
  reject(reason, schedule_version);
}

std::string srsran::srs_cu_cp::compute_ntn_access_profile_hash(const ntn_onboard_position_plan_config& config)
{
  std::ostringstream canonical;
  canonical.imbue(std::locale::classic());
  canonical << "access_profile_id=" << config.expected_access_profile_id << '\n';
  canonical << "max_l1_positions_per_cell=" << config.max_l1_positions_per_cell << '\n';
  canonical << "max_l1_positions_per_satellite=" << config.max_l1_positions_per_satellite << '\n';
  canonical << "analog_ports_per_cell=" << config.max_analog_ports_per_cell << '\n';
  canonical << "analog_ports_per_satellite=" << config.max_analog_ports_per_satellite << '\n';
  canonical << "digital_ports_per_cell=" << config.max_digital_ports_per_cell << '\n';
  canonical << "digital_ports_per_satellite=" << config.max_digital_ports_per_satellite << '\n';
  canonical << "access_slot_us=" << config.access_slot.count() << '\n';
  canonical << "subvisit_duration_us=" << config.subvisit_duration.count() << '\n';
  canonical << "max_ssb_interval_us=" << config.max_ssb_interval.count() << '\n';
  canonical << "max_prach_interval_us=" << config.max_prach_interval.count() << '\n';
  canonical << "activation_alignment_ms=" << config.activation_alignment.count() << '\n';
  canonical << "cell_access_slot_stride=" << config.cell_access_slot_stride << '\n';
  canonical << "subvisits_per_access_slot=" << config.subvisits_per_access_slot << '\n';
  for (size_t index = 0; index != config.access_phases.size(); ++index) {
    canonical << "phase=" << index << ',' << config.access_phases[index].downlink_port_mask << ','
              << config.access_phases[index].uplink_port_mask << ','
              << config.access_phases[index].digital_downlink_capacity << ','
              << config.access_phases[index].digital_uplink_capacity << '\n';
  }
  return sha256_with_prefix(canonical.str());
}

std::string srsran::srs_cu_cp::compute_ntn_position_plan_content_hash(const ntn_versioned_position_plan& plan)
{
  std::ostringstream canonical;
  canonical.imbue(std::locale::classic());
  if (plan.schema_version >= 2) {
    canonical << "schema_version=" << plan.schema_version << '\n';
    canonical << "planning_run_id=" << plan.planning_run_id << '\n';
    canonical << "catalog=" << plan.catalog_id << ',' << normalize_hash(plan.catalog_hash) << '\n';
    canonical << "identity_registry=" << plan.identity_registry_version << ','
              << normalize_hash(plan.identity_registry_hash) << '\n';
    canonical << "access_profile=" << plan.access_profile_id << ',' << normalize_hash(plan.access_profile_hash) << '\n';
  }
  canonical << "satellite_id=" << plan.satellite_id << '\n';
  canonical << "catalog_version=" << plan.catalog_version << '\n';
  canonical << "schedule_version=" << plan.schedule_version << '\n';
  canonical << "valid_from_unix_ms=" << to_unix_milliseconds(plan.valid_from) << '\n';
  canonical << "valid_until_unix_ms=" << to_unix_milliseconds(plan.valid_until) << '\n';
  canonical << "activation_epoch_unix_ms=" << to_unix_milliseconds(plan.activation_epoch) << '\n';

  const auto cells = sorted_cell_identities(plan.onboard_cells);
  for (const ntn_onboard_cell_identity& cell : cells) {
    canonical << "cell=" << cell.nci.value() << ',' << cell.pci << '\n';
  }

  std::vector<ntn_l1_position> positions = plan.visible_l1_positions;
  std::sort(positions.begin(), positions.end(), [](const ntn_l1_position& lhs, const ntn_l1_position& rhs) {
    if (lhs.position_id != rhs.position_id) {
      return lhs.position_id < rhs.position_id;
    }
    if (lhs.latitude_deg != rhs.latitude_deg) {
      return lhs.latitude_deg < rhs.latitude_deg;
    }
    return lhs.longitude_deg < rhs.longitude_deg;
  });
  canonical << std::setprecision(std::numeric_limits<double>::max_digits10);
  for (const ntn_l1_position& position : positions) {
    canonical << "l1=" << position.position_id << ',' << position.latitude_deg << ',' << position.longitude_deg;
    if (plan.schema_version >= 2) {
      canonical << ',' << static_cast<unsigned>(position.child_mask);
    }
    canonical << '\n';
  }

  if (plan.schema_version >= 3) {
    std::vector<std::string> assigned_ids = plan.assigned_l1_position_ids;
    std::sort(assigned_ids.begin(), assigned_ids.end());
    for (const std::string& position_id : assigned_ids) {
      canonical << "assigned_l1=" << position_id << '\n';
    }
  }

  return sha256_with_prefix(canonical.str());
}

std::string srsran::srs_cu_cp::compute_ntn_access_calendar_hash(
    uint64_t schedule_version, const std::vector<ntn_access_calendar_intent>& intents)
{
  std::vector<ntn_access_calendar_intent> canonical_intents = intents;
  std::sort(canonical_intents.begin(), canonical_intents.end(), [](const auto& lhs, const auto& rhs) {
    return std::tie(lhs.nci,
                    lhs.position_id,
                    lhs.start_time,
                    lhs.duration,
                    lhs.direction,
                    lhs.purpose,
                    lhs.port_id) <
           std::tie(rhs.nci,
                    rhs.position_id,
                    rhs.start_time,
                    rhs.duration,
                    rhs.direction,
                    rhs.purpose,
                    rhs.port_id);
  });

  std::ostringstream canonical;
  canonical << "schedule_version=" << schedule_version << '\n';
  for (const ntn_access_calendar_intent& intent : canonical_intents) {
    canonical << "intent=" << intent.nci.value() << ',' << intent.position_id << ',' << intent.start_time.count()
              << ',' << intent.duration.count() << ',' << static_cast<unsigned>(intent.direction) << ','
              << static_cast<unsigned>(intent.purpose) << ',' << intent.port_id << '\n';
  }
  return sha256_with_prefix(canonical.str());
}

expected<ntn_versioned_position_plan, std::string>
srsran::srs_cu_cp::parse_ntn_position_plan_json(const std::string& json_text)
{
  if (json_text.size() > max_ntn_position_plan_file_size) {
    return make_unexpected(make_input_too_large_error(
        fmt::format("plan document exceeds {} bytes", max_ntn_position_plan_file_size)));
  }
  try {
    const nlohmann::json root = nlohmann::json::parse(json_text);
    if (!root.is_object()) {
      return make_unexpected(std::string{"root must be an object"});
    }

    ntn_versioned_position_plan result;
    if (root.contains("schema_version")) {
      auto schema_version = parse_json_uint64(root.at("schema_version"), "schema_version");
      if (!schema_version.has_value() || schema_version.value() > std::numeric_limits<unsigned>::max()) {
        return make_unexpected(schema_version.has_value() ? std::string{"schema_version exceeds unsigned range"}
                                                          : schema_version.error());
      }
      result.schema_version = static_cast<unsigned>(schema_version.value());
    }
    if (result.schema_version != 1 && result.schema_version != 2 && result.schema_version != 3) {
      return make_unexpected(fmt::format("unsupported schema_version {}", result.schema_version));
    }

    std::optional<std::string> key_error;
    if (result.schema_version == 1) {
      key_error = validate_exact_object_keys(root,
                                             "root",
                                             {"satellite_id",
                                              "catalog_version",
                                              "schedule_version",
                                              "content_hash",
                                              "valid_from_unix_ms",
                                              "valid_until_unix_ms",
                                              "activation_epoch_unix_ms",
                                              "onboard_cells",
                                              "visible_l1_positions"},
                                             {"schema_version"});
    } else if (result.schema_version == 2) {
      key_error = validate_exact_object_keys(root,
                                             "root",
                                             {"schema_version",
                                              "planning_run_id",
                                              "catalog",
                                              "identity_registry",
                                              "access_profile",
                                              "satellite_id",
                                              "catalog_version",
                                              "schedule_version",
                                              "content_hash",
                                              "valid_from_unix_ms",
                                              "valid_until_unix_ms",
                                              "activation_epoch_unix_ms",
                                              "onboard_cells",
                                              "visible_l1_positions"});
    } else {
      key_error = validate_exact_object_keys(root,
                                             "root",
                                             {"schema_version",
                                              "planning_run_id",
                                              "catalog",
                                              "identity_registry",
                                              "access_profile",
                                              "satellite_id",
                                              "catalog_version",
                                              "schedule_version",
                                              "content_hash",
                                              "valid_from_unix_ms",
                                              "valid_until_unix_ms",
                                              "activation_epoch_unix_ms",
                                              "onboard_cells",
                                              "visible_l1_positions",
                                              "assigned_l1_position_ids"});
    }
    if (key_error.has_value()) {
      return make_unexpected(std::move(key_error.value()));
    }

    if (result.schema_version >= 2) {
      const nlohmann::json& catalog  = root.at("catalog");
      const nlohmann::json& registry = root.at("identity_registry");
      const nlohmann::json& profile  = root.at("access_profile");
      if (auto error = validate_exact_object_keys(catalog, "catalog", {"id", "sha256"}); error.has_value()) {
        return make_unexpected(std::move(error.value()));
      }
      if (auto error = validate_exact_object_keys(registry, "identity_registry", {"version", "sha256"});
          error.has_value()) {
        return make_unexpected(std::move(error.value()));
      }
      if (auto error = validate_exact_object_keys(profile, "access_profile", {"id", "sha256"}); error.has_value()) {
        return make_unexpected(std::move(error.value()));
      }
      auto planning_run_id = parse_context_identifier(root.at("planning_run_id"), "planning_run_id");
      auto catalog_id      = parse_context_identifier(catalog.at("id"), "catalog.id");
      auto registry_version =
          parse_context_identifier(registry.at("version"), "identity_registry.version");
      auto profile_id = parse_context_identifier(profile.at("id"), "access_profile.id");
      if (!planning_run_id.has_value()) {
        return make_unexpected(planning_run_id.error());
      }
      if (!catalog_id.has_value()) {
        return make_unexpected(catalog_id.error());
      }
      if (!registry_version.has_value()) {
        return make_unexpected(registry_version.error());
      }
      if (!profile_id.has_value()) {
        return make_unexpected(profile_id.error());
      }
      result.planning_run_id           = std::move(planning_run_id.value());
      result.catalog_id                = std::move(catalog_id.value());
      result.catalog_hash              = catalog.at("sha256").get<std::string>();
      result.identity_registry_version = std::move(registry_version.value());
      result.identity_registry_hash    = registry.at("sha256").get<std::string>();
      result.access_profile_id         = std::move(profile_id.value());
      result.access_profile_hash       = profile.at("sha256").get<std::string>();
    }
    auto catalog_version  = parse_json_uint64(root.at("catalog_version"), "catalog_version");
    auto schedule_version = parse_json_uint64(root.at("schedule_version"), "schedule_version");
    auto valid_from       = parse_json_int64(root.at("valid_from_unix_ms"), "valid_from_unix_ms");
    auto valid_until      = parse_json_int64(root.at("valid_until_unix_ms"), "valid_until_unix_ms");
    auto activation_epoch = parse_json_int64(root.at("activation_epoch_unix_ms"), "activation_epoch_unix_ms");
    if (!catalog_version.has_value()) {
      return make_unexpected(catalog_version.error());
    }
    if (!schedule_version.has_value()) {
      return make_unexpected(schedule_version.error());
    }
    if (!valid_from.has_value()) {
      return make_unexpected(valid_from.error());
    }
    if (!valid_until.has_value()) {
      return make_unexpected(valid_until.error());
    }
    if (!activation_epoch.has_value()) {
      return make_unexpected(activation_epoch.error());
    }
    auto satellite_id = parse_context_identifier(root.at("satellite_id"), "satellite_id");
    if (!satellite_id.has_value()) {
      return make_unexpected(satellite_id.error());
    }
    result.satellite_id      = std::move(satellite_id.value());
    result.catalog_version   = catalog_version.value();
    result.schedule_version  = schedule_version.value();
    result.content_hash      = root.at("content_hash").get<std::string>();
    result.valid_from        = from_unix_milliseconds(valid_from.value());
    result.valid_until       = from_unix_milliseconds(valid_until.value());
    result.activation_epoch  = from_unix_milliseconds(activation_epoch.value());

    const nlohmann::json& cells = root.at("onboard_cells");
    if (!cells.is_array() || cells.size() != 2) {
      return make_unexpected(std::string{"onboard_cells must contain exactly two identities"});
    }
    for (size_t i = 0; i != cells.size(); ++i) {
      const std::string context = fmt::format("onboard_cells[{}]", i);
      if (auto error = validate_exact_object_keys(cells[i], context.c_str(), {"nci", "pci"}); error.has_value()) {
        return make_unexpected(std::move(error.value()));
      }
      auto nci = parse_nci(cells[i].at("nci"), context.c_str());
      if (!nci.has_value()) {
        return make_unexpected(nci.error());
      }
      auto pci = parse_json_uint64(cells[i].at("pci"), fmt::format("{}.pci", context));
      if (!pci.has_value()) {
        return make_unexpected(pci.error());
      }
      if (pci.value() > MAX_PCI) {
        return make_unexpected(fmt::format("{}.pci is outside 0..{}", context, MAX_PCI));
      }
      result.onboard_cells[i] = {nci.value(), static_cast<pci_t>(pci.value())};
    }

    const nlohmann::json& positions = root.at("visible_l1_positions");
    if (!positions.is_array()) {
      return make_unexpected(std::string{"visible_l1_positions must be an array"});
    }
    if (positions.size() > max_ntn_position_plan_positions) {
      return make_unexpected(make_input_too_large_error(fmt::format(
          "visible_l1_positions contains more than {} entries", max_ntn_position_plan_positions)));
    }
    result.visible_l1_positions.reserve(positions.size());
    for (size_t i = 0; i != positions.size(); ++i) {
      const std::string context = fmt::format("visible_l1_positions[{}]", i);
      const auto        error =
          result.schema_version >= 2
              ? validate_exact_object_keys(
                    positions[i], context.c_str(), {"position_id", "latitude_deg", "longitude_deg", "child_mask"})
              : validate_exact_object_keys(
                    positions[i], context.c_str(), {"position_id", "latitude_deg", "longitude_deg"});
      if (error.has_value()) {
        return make_unexpected(error.value());
      }
      ntn_l1_position position;
      position.position_id  = positions[i].at("position_id").get<std::string>();
      position.latitude_deg = positions[i].at("latitude_deg").get<double>();
      position.longitude_deg = positions[i].at("longitude_deg").get<double>();
      if (result.schema_version >= 2) {
        auto child_mask = parse_json_uint64(positions[i].at("child_mask"), fmt::format("{}.child_mask", context));
        if (!child_mask.has_value()) {
          return make_unexpected(child_mask.error());
        }
        if (child_mask.value() == 0 || child_mask.value() > 0x7fU) {
          return make_unexpected(fmt::format("{}.child_mask must be in 1..127", context));
        }
        position.child_mask = static_cast<uint8_t>(child_mask.value());
      }
      result.visible_l1_positions.push_back(std::move(position));
    }
    if (result.schema_version >= 3) {
      const nlohmann::json& assigned_ids = root.at("assigned_l1_position_ids");
      if (!assigned_ids.is_array()) {
        return make_unexpected(std::string{"assigned_l1_position_ids must be an array"});
      }
      if (assigned_ids.size() > max_ntn_position_plan_positions) {
        return make_unexpected(make_input_too_large_error(fmt::format(
            "assigned_l1_position_ids contains more than {} entries", max_ntn_position_plan_positions)));
      }
      result.assigned_l1_position_ids.reserve(assigned_ids.size());
      for (size_t i = 0; i != assigned_ids.size(); ++i) {
        auto position_id = parse_json_string(assigned_ids[i], fmt::format("assigned_l1_position_ids[{}]", i));
        if (!position_id.has_value()) {
          return make_unexpected(position_id.error());
        }
        result.assigned_l1_position_ids.push_back(std::move(position_id.value()));
      }
    } else {
      result.assigned_l1_position_ids.reserve(result.visible_l1_positions.size());
      for (const ntn_l1_position& position : result.visible_l1_positions) {
        result.assigned_l1_position_ids.push_back(position.position_id);
      }
    }
    return result;
  } catch (const std::exception& error) {
    return make_unexpected(fmt::format("invalid NTN position plan JSON: {}", error.what()));
  }
}

void srsran::srs_cu_cp::set_ntn_position_plan_file_read_test_hook_once_for_test(
    ntn_position_plan_file_read_test_hook hook)
{
  next_plan_file_read_test_hook.store(hook, std::memory_order_release);
}

expected<ntn_versioned_position_plan, ntn_position_plan_input_failure>
srsran::srs_cu_cp::load_ntn_position_plan_json_file(const std::string& path)
{
  const auto fail = [](ntn_position_plan_input_error reason, std::string detail) {
    return make_unexpected(ntn_position_plan_input_failure{reason, std::move(detail)});
  };

  int flags = O_RDONLY | O_NONBLOCK;
#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  unique_fd fd(::open(path.c_str(), flags));
  if (!fd.is_open()) {
    return fail(ntn_position_plan_input_error::parse_error,
                fmt::format("cannot open plan file '{}': {}", path, std::strerror(errno)));
  }

  struct stat initial_status {};
  if (::fstat(fd.value(), &initial_status) != 0) {
    return fail(ntn_position_plan_input_error::parse_error,
                fmt::format("cannot stat plan file '{}': {}", path, std::strerror(errno)));
  }
  if (!S_ISREG(initial_status.st_mode)) {
    return fail(ntn_position_plan_input_error::parse_error,
                fmt::format("plan file '{}' is not a regular file", path));
  }
  if (initial_status.st_size < 0 ||
      static_cast<uint64_t>(initial_status.st_size) > max_ntn_position_plan_file_size) {
    return fail(ntn_position_plan_input_error::input_too_large,
                fmt::format("plan file '{}' exceeds {} bytes", path, max_ntn_position_plan_file_size));
  }

  if (const auto hook = next_plan_file_read_test_hook.exchange(nullptr, std::memory_order_acq_rel); hook != nullptr) {
    hook(path);
  }

  const size_t initial_size = static_cast<size_t>(initial_status.st_size);
  std::string  text;
  text.reserve(initial_size);
  std::array<char, 8192> buffer{};
  while (true) {
    const ssize_t count = ::read(fd.value(), buffer.data(), buffer.size());
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      return fail(ntn_position_plan_input_error::parse_error,
                  fmt::format("cannot read plan file '{}': {}", path, std::strerror(errno)));
    }
    if (count == 0) {
      break;
    }
    const size_t bytes_read = static_cast<size_t>(count);
    if (text.size() > max_ntn_position_plan_file_size - bytes_read) {
      return fail(ntn_position_plan_input_error::input_too_large,
                  fmt::format("plan file '{}' exceeds {} bytes while being read",
                              path,
                              max_ntn_position_plan_file_size));
    }
    text.append(buffer.data(), bytes_read);
  }

  struct stat final_status {};
  if (::fstat(fd.value(), &final_status) != 0) {
    return fail(ntn_position_plan_input_error::parse_error,
                fmt::format("cannot restat plan file '{}': {}", path, std::strerror(errno)));
  }
  if (final_status.st_size > initial_status.st_size || text.size() > initial_size) {
    return fail(ntn_position_plan_input_error::input_too_large,
                fmt::format("plan file '{}' grew while being read", path));
  }
  if (final_status.st_size != initial_status.st_size || text.size() != initial_size) {
    return fail(ntn_position_plan_input_error::parse_error,
                fmt::format("plan file '{}' changed while being read", path));
  }

  auto parsed = parse_ntn_position_plan_json(text);
  if (!parsed.has_value()) {
    return fail(is_input_too_large_error(parsed.error()) ? ntn_position_plan_input_error::input_too_large
                                                         : ntn_position_plan_input_error::parse_error,
                parsed.error());
  }
  return std::move(parsed.value());
}
