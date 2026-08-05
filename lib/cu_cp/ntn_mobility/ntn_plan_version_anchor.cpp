/*
 * Copyright 2021-2026 Software Radio Systems Limited
 *
 * This file is part of srsRAN.
 */

#include "ntn_plan_version_anchor.h"
#include "nlohmann/json.hpp"
#include "srsran/support/io/unique_fd.h"
#include "fmt/format.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
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
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (info == nullptr ||
      mbedtls_md(info,
                 reinterpret_cast<const unsigned char*>(payload.data()),
                 payload.size(),
                 digest.data()) != 0) {
    return {};
  }
  static constexpr char hex[] = "0123456789abcdef";
  std::string result = "sha256:";
  result.reserve(71);
  for (unsigned char value : digest) {
    result.push_back(hex[value >> 4U]);
    result.push_back(hex[value & 0x0fU]);
  }
  return result;
}

std::optional<std::string>
validate_exact_keys(const json& value, const char* context, std::initializer_list<const char*> required)
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

bool valid_key_id(const std::string& value)
{
  const auto is_ascii_alnum = [](unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
  };
  return !value.empty() && value.size() <= max_ntn_position_plan_context_identifier_size &&
         is_ascii_alnum(static_cast<unsigned char>(value.front())) &&
         std::all_of(value.begin(), value.end(), [is_ascii_alnum](unsigned char c) {
           return is_ascii_alnum(c) || c == '.' || c == '_' || c == ':' || c == '/' || c == '-';
         });
}

json encode_identity(const ntn_plan_version_identity& identity)
{
  return {{"catalog_version", identity.catalog_version},
          {"schedule_version", identity.schedule_version},
          {"content_hash", normalize_hash(identity.content_hash)},
          {"key_id", identity.key_id}};
}

json encode_context(const ntn_onboard_position_plan_state_context& context)
{
  return {{"catalog_id", context.catalog_id},
          {"catalog_hash", normalize_hash(context.catalog_hash)},
          {"identity_registry_version", context.identity_registry_version},
          {"identity_registry_hash", normalize_hash(context.identity_registry_hash)},
          {"access_profile_id", context.access_profile_id},
          {"access_profile_hash", normalize_hash(context.access_profile_hash)}};
}

json encode_cell(const ntn_onboard_cell_identity& cell)
{
  return {{"nci", cell.nci.value()}, {"pci", cell.pci}};
}

json encode_payload(const ntn_plan_version_anchor_state& state)
{
  json result = {{"anchor_schema_version", state.schema_version},
                 {"generation", state.generation},
                 {"satellite_id", state.satellite_id},
                 {"planning_context", encode_context(state.planning_context)}};
  result["onboard_cells"] = json::array({encode_cell(state.onboard_cells[0]), encode_cell(state.onboard_cells[1])});
  result["committed"] = state.committed.has_value() ? encode_identity(*state.committed) : json(nullptr);
  result["reserved"] =
      state.reserved.has_value()
          ? json{{"identity", encode_identity(state.reserved->identity)},
                 {"target_state_generation", state.reserved->target_state_generation}}
          : json(nullptr);
  return result;
}

std::string compute_anchor_hash(const ntn_plan_version_anchor_state& state)
{
  return sha256_with_prefix(encode_payload(state).dump());
}

std::optional<std::string> validate_identity(const ntn_plan_version_identity& identity)
{
  if (identity.catalog_version == 0 || identity.schedule_version == 0 ||
      identity.catalog_version > max_ntn_position_plan_cross_language_integer ||
      identity.schedule_version > max_ntn_position_plan_cross_language_integer) {
    return std::string{"version identity is outside the supported exact integer range"};
  }
  if (!is_sha256_digest(identity.content_hash) || !valid_key_id(identity.key_id)) {
    return std::string{"version identity hash or key id is invalid"};
  }
  return std::nullopt;
}

expected<ntn_plan_version_identity, std::string> decode_identity(const json& value, const char* context)
{
  if (auto error = validate_exact_keys(
          value, context, {"catalog_version", "schedule_version", "content_hash", "key_id"});
      error.has_value()) {
    return make_unexpected(*error);
  }
  if (!value.at("catalog_version").is_number_unsigned() || !value.at("schedule_version").is_number_unsigned() ||
      !value.at("content_hash").is_string() || !value.at("key_id").is_string()) {
    return make_unexpected(fmt::format("{} contains an invalid field", context));
  }
  ntn_plan_version_identity result{value.at("catalog_version").get<uint64_t>(),
                                   value.at("schedule_version").get<uint64_t>(),
                                   value.at("content_hash").get<std::string>(),
                                   value.at("key_id").get<std::string>()};
  if (auto error = validate_identity(result); error.has_value()) {
    return make_unexpected(fmt::format("{}: {}", context, *error));
  }
  result.content_hash = normalize_hash(result.content_hash);
  return result;
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
    if (errno == ENOENT) {
      return make_unexpected(std::string{"not_found"});
    }
    return make_unexpected(fmt::format("cannot open anchor '{}': {}", path, std::strerror(errno)));
  }
  struct stat before {};
  if (::fstat(fd.value(), &before) != 0 || !S_ISREG(before.st_mode)) {
    return make_unexpected(std::string{"anchor is not a regular file"});
  }
  if (before.st_size < 0 || static_cast<uint64_t>(before.st_size) > max_ntn_plan_version_anchor_file_size) {
    return make_unexpected(std::string{"anchor input_too_large"});
  }
  const size_t initial_size = static_cast<size_t>(before.st_size);
  std::string result;
  result.reserve(initial_size);
  std::array<char, 4096> buffer{};
  while (true) {
    const ssize_t count = ::read(fd.value(), buffer.data(), buffer.size());
    if (count < 0) {
      if (errno == EINTR) continue;
      return make_unexpected(std::string{"anchor read failed"});
    }
    if (count == 0) break;
    const size_t bytes = static_cast<size_t>(count);
    if (result.size() > max_ntn_plan_version_anchor_file_size - bytes) {
      return make_unexpected(std::string{"anchor input_too_large"});
    }
    result.append(buffer.data(), bytes);
  }
  struct stat after {};
  if (::fstat(fd.value(), &after) != 0 || after.st_size != before.st_size || result.size() != initial_size) {
    return make_unexpected(std::string{"anchor changed while being read"});
  }
  return result;
}

bool context_valid(const ntn_plan_version_anchor_state& state)
{
  return state.generation > 0 && !state.satellite_id.empty() && !state.planning_context.catalog_id.empty() &&
         is_sha256_digest(state.planning_context.catalog_hash) &&
         !state.planning_context.identity_registry_version.empty() &&
         is_sha256_digest(state.planning_context.identity_registry_hash) &&
         !state.planning_context.access_profile_id.empty() &&
         is_sha256_digest(state.planning_context.access_profile_hash) && is_valid(state.onboard_cells[0].pci) &&
         is_valid(state.onboard_cells[1].pci) && state.onboard_cells[0].nci != state.onboard_cells[1].nci;
}

} // namespace

bool srsran::srs_cu_cp::ntn_plan_version_identity_equal(const ntn_plan_version_identity& lhs,
                                                         const ntn_plan_version_identity& rhs)
{
  return lhs.catalog_version == rhs.catalog_version && lhs.schedule_version == rhs.schedule_version &&
         normalize_hash(lhs.content_hash) == normalize_hash(rhs.content_hash) && lhs.key_id == rhs.key_id;
}

ntn_position_plan_version_anchor_snapshot
srsran::srs_cu_cp::make_ntn_plan_version_anchor_snapshot(const ntn_plan_version_identity& identity)
{
  return {"software_only", identity.catalog_version, identity.schedule_version, normalize_hash(identity.content_hash)};
}

expected<ntn_plan_version_reserve_outcome, std::string>
srsran::srs_cu_cp::reserve_ntn_plan_version(ntn_plan_version_anchor_state& state,
                                             const ntn_plan_version_identity& identity,
                                             uint64_t target_state_generation)
{
  if (auto error = validate_identity(identity); error.has_value() || target_state_generation == 0) {
    return make_unexpected(error.has_value() ? *error : std::string{"target state generation must be non-zero"});
  }
  if (state.reserved.has_value()) {
    if (ntn_plan_version_identity_equal(state.reserved->identity, identity) &&
        state.reserved->target_state_generation == target_state_generation) {
      return ntn_plan_version_reserve_outcome::already_reserved;
    }
    return make_unexpected(std::string{"version_anchor_unavailable: another reservation is outstanding"});
  }
  if (state.committed.has_value()) {
    if (ntn_plan_version_identity_equal(*state.committed, identity)) {
      return ntn_plan_version_reserve_outcome::already_committed;
    }
    if (identity.catalog_version < state.committed->catalog_version ||
        identity.schedule_version < state.committed->schedule_version ||
        identity.schedule_version == state.committed->schedule_version) {
      return make_unexpected(std::string{"version_replay"});
    }
  }
  state.reserved = ntn_plan_version_reservation{identity, target_state_generation};
  ++state.generation;
  return ntn_plan_version_reserve_outcome::reserved;
}

expected<void, std::string>
srsran::srs_cu_cp::commit_ntn_plan_version(ntn_plan_version_anchor_state& state,
                                            const ntn_plan_version_identity& identity,
                                            uint64_t target_state_generation)
{
  if (state.committed.has_value() && ntn_plan_version_identity_equal(*state.committed, identity) &&
      !state.reserved.has_value()) {
    return {};
  }
  if (!state.reserved.has_value() || !ntn_plan_version_identity_equal(state.reserved->identity, identity) ||
      state.reserved->target_state_generation != target_state_generation) {
    return make_unexpected(std::string{"version anchor reservation mismatch"});
  }
  state.committed = identity;
  state.reserved.reset();
  ++state.generation;
  return {};
}

expected<void, std::string>
srsran::srs_cu_cp::cancel_ntn_plan_version_reservation(ntn_plan_version_anchor_state& state,
                                                        const ntn_plan_version_identity& identity,
                                                        uint64_t target_state_generation)
{
  if (!state.reserved.has_value()) {
    return {};
  }
  if (!ntn_plan_version_identity_equal(state.reserved->identity, identity) ||
      state.reserved->target_state_generation != target_state_generation) {
    return make_unexpected(std::string{"version anchor reservation mismatch"});
  }
  state.reserved.reset();
  ++state.generation;
  return {};
}

expected<std::optional<ntn_plan_version_anchor_state>, std::string>
srsran::srs_cu_cp::load_ntn_plan_version_anchor(const std::string& path)
{
  auto text = read_bounded_regular_file(path);
  if (!text.has_value()) {
    if (text.error() == "not_found") return std::optional<ntn_plan_version_anchor_state>{};
    return make_unexpected(text.error());
  }
  try {
    const json root = json::parse(*text);
    if (auto error = validate_exact_keys(root,
                                         "root",
                                         {"anchor_schema_version",
                                          "generation",
                                          "anchor_hash",
                                          "satellite_id",
                                          "planning_context",
                                          "onboard_cells",
                                          "committed",
                                          "reserved"});
        error.has_value()) {
      return make_unexpected(*error);
    }
    if (!root.at("anchor_schema_version").is_number_unsigned() || !root.at("generation").is_number_unsigned() ||
        !root.at("anchor_hash").is_string() || !root.at("satellite_id").is_string()) {
      return make_unexpected(std::string{"anchor header is invalid"});
    }
    ntn_plan_version_anchor_state result;
    result.schema_version = root.at("anchor_schema_version").get<unsigned>();
    result.generation = root.at("generation").get<uint64_t>();
    result.anchor_hash = root.at("anchor_hash").get<std::string>();
    result.satellite_id = root.at("satellite_id").get<std::string>();
    if (result.schema_version != ntn_plan_version_anchor_state::current_schema_version) {
      return make_unexpected(std::string{"unsupported anchor schema"});
    }

    const json& context = root.at("planning_context");
    if (auto error = validate_exact_keys(context,
                                         "planning_context",
                                         {"catalog_id",
                                          "catalog_hash",
                                          "identity_registry_version",
                                          "identity_registry_hash",
                                          "access_profile_id",
                                          "access_profile_hash"});
        error.has_value()) return make_unexpected(*error);
    for (const char* key : {"catalog_id", "catalog_hash", "identity_registry_version", "identity_registry_hash",
                            "access_profile_id", "access_profile_hash"}) {
      if (!context.at(key).is_string()) return make_unexpected(std::string{"anchor planning context is invalid"});
    }
    result.planning_context = {context.at("catalog_id").get<std::string>(),
                               context.at("catalog_hash").get<std::string>(),
                               context.at("identity_registry_version").get<std::string>(),
                               context.at("identity_registry_hash").get<std::string>(),
                               context.at("access_profile_id").get<std::string>(),
                               context.at("access_profile_hash").get<std::string>()};

    const json& cells = root.at("onboard_cells");
    if (!cells.is_array() || cells.size() != 2) return make_unexpected(std::string{"anchor cells are invalid"});
    for (size_t i = 0; i != 2; ++i) {
      if (auto error = validate_exact_keys(cells[i], "onboard_cells", {"nci", "pci"}); error.has_value())
        return make_unexpected(*error);
      if (!cells[i].at("nci").is_number_unsigned() || !cells[i].at("pci").is_number_unsigned())
        return make_unexpected(std::string{"anchor cell identity is invalid"});
      auto nci = nr_cell_identity::create(cells[i].at("nci").get<uint64_t>());
      const uint64_t pci = cells[i].at("pci").get<uint64_t>();
      if (!nci.has_value() || pci > MAX_PCI) return make_unexpected(std::string{"anchor cell identity is invalid"});
      result.onboard_cells[i] = {nci.value(), static_cast<pci_t>(pci)};
    }
    if (!root.at("committed").is_null()) {
      auto committed = decode_identity(root.at("committed"), "committed");
      if (!committed.has_value()) return make_unexpected(committed.error());
      result.committed = std::move(committed.value());
    }
    if (!root.at("reserved").is_null()) {
      const json& reserved = root.at("reserved");
      if (auto error = validate_exact_keys(reserved, "reserved", {"identity", "target_state_generation"});
          error.has_value()) return make_unexpected(*error);
      auto identity = decode_identity(reserved.at("identity"), "reserved.identity");
      if (!identity.has_value() || !reserved.at("target_state_generation").is_number_unsigned())
        return make_unexpected(std::string{"anchor reservation is invalid"});
      result.reserved = ntn_plan_version_reservation{
          std::move(identity.value()), reserved.at("target_state_generation").get<uint64_t>()};
    }
    if (!context_valid(result)) return make_unexpected(std::string{"anchor context is invalid"});
    ntn_plan_version_anchor_state unhashed = result;
    unhashed.anchor_hash.clear();
    if (!is_sha256_digest(result.anchor_hash) || normalize_hash(result.anchor_hash) != compute_anchor_hash(unhashed)) {
      return make_unexpected(std::string{"anchor hash mismatch"});
    }
    return std::optional<ntn_plan_version_anchor_state>{std::move(result)};
  } catch (const std::exception& error) {
    return make_unexpected(fmt::format("invalid anchor JSON: {}", error.what()));
  }
}

expected<std::string, std::string>
srsran::srs_cu_cp::store_ntn_plan_version_anchor_atomic(const std::string& path,
                                                         const ntn_plan_version_anchor_state& state)
{
  if (!context_valid(state) || state.schema_version != ntn_plan_version_anchor_state::current_schema_version ||
      (state.committed.has_value() && validate_identity(*state.committed).has_value()) ||
      (state.reserved.has_value() && (validate_identity(state.reserved->identity).has_value() ||
                                      state.reserved->target_state_generation == 0))) {
    return make_unexpected(std::string{"invalid anchor state"});
  }
  ntn_plan_version_anchor_state stored = state;
  stored.anchor_hash.clear();
  stored.anchor_hash = compute_anchor_hash(stored);
  json output = encode_payload(stored);
  output["anchor_hash"] = stored.anchor_hash;
  const std::string bytes = output.dump(2) + '\n';
  if (bytes.size() > max_ntn_plan_version_anchor_file_size) {
    return make_unexpected(std::string{"anchor output is too large"});
  }

  const std::filesystem::path target(path);
  const std::filesystem::path directory = target.parent_path().empty() ? std::filesystem::path{"."} : target.parent_path();
  const std::filesystem::path temporary =
      directory / fmt::format(".{}.tmp-{}-{}", target.filename().string(), ::getpid(), stored.generation);
  int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
  unique_fd fd(::open(temporary.c_str(), flags, S_IRUSR | S_IWUSR));
  if (!fd.is_open()) return make_unexpected(std::string{"cannot create anchor temporary file"});
  size_t offset = 0;
  while (offset != bytes.size()) {
    const ssize_t count = ::write(fd.value(), bytes.data() + offset, bytes.size() - offset);
    if (count < 0) {
      if (errno == EINTR) continue;
      ::unlink(temporary.c_str());
      return make_unexpected(std::string{"cannot write anchor temporary file"});
    }
    offset += static_cast<size_t>(count);
  }
  if (::fsync(fd.value()) != 0) {
    ::unlink(temporary.c_str());
    return make_unexpected(std::string{"cannot sync anchor temporary file"});
  }
  if (!fd.close()) {
    ::unlink(temporary.c_str());
    return make_unexpected(std::string{"cannot close anchor temporary file"});
  }
  if (::rename(temporary.c_str(), target.c_str()) != 0) {
    ::unlink(temporary.c_str());
    return make_unexpected(std::string{"cannot replace anchor file"});
  }
  unique_fd directory_fd(::open(directory.c_str(), O_RDONLY | O_DIRECTORY));
  if (!directory_fd.is_open() || ::fsync(directory_fd.value()) != 0) {
    return make_unexpected(std::string{"anchor committed_not_durable"});
  }
  return stored.anchor_hash;
}
