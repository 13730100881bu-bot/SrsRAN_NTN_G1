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

#include "srsran/adt/byte_buffer.h"
#include "srsran/ran/du_types.h"
#include "srsran/ran/nr_cell_identity.h"
#include "srsran/ran/pci.h"
#include "srsran/ran/slot_point.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace srsran {

/// Private operation carried in the standard opaque F1AP resource-coordination container.
enum class f1ap_ntn_access_calendar_operation : uint8_t { prepare = 0, query = 1, clear = 2, invalid = 255 };

enum class f1ap_ntn_access_calendar_result_status : uint8_t {
  ready       = 0,
  applied     = 1,
  cleared     = 2,
  rejected    = 3,
  unsupported = 4,
  preparing   = 5
};

enum class f1ap_ntn_access_calendar_direction : uint8_t { downlink = 0, uplink = 1, invalid = 255 };

enum class f1ap_ntn_access_calendar_purpose : uint8_t {
  ssb_sib_paging     = 0,
  ssb_sib_paging_rar = 1,
  prach_ro           = 2,
  prach_ul_beam      = 3,
  invalid            = 255
};

enum class f1ap_ntn_access_calendar_preflight_purpose : uint8_t { ssb = 0, prach = 1, invalid = 255 };

struct f1ap_ntn_access_calendar_unmatched_intent {
  std::string                                position_id;
  f1ap_ntn_access_calendar_preflight_purpose purpose =
      f1ap_ntn_access_calendar_preflight_purpose::invalid;
  uint32_t                                   start_slot_offset = 0;
  uint32_t                                   nof_slots         = 0;
};

struct f1ap_ntn_access_calendar_preflight_report {
  bool                                                    performed            = false;
  bool                                                    passed               = false;
  uint8_t                                                 numerology           = 0xffU;
  uint16_t                                                expected_ssb         = 0;
  uint16_t                                                matched_ssb          = 0;
  uint16_t                                                expected_prach       = 0;
  uint16_t                                                matched_prach        = 0;
  uint32_t                                                max_ssb_gap_slots    = 0;
  uint32_t                                                max_prach_gap_slots  = 0;
  std::optional<f1ap_ntn_access_calendar_unmatched_intent> first_unmatched;
};

struct f1ap_ntn_access_calendar_intent {
  static constexpr uint16_t no_resource_port = std::numeric_limits<uint16_t>::max();

  std::string                        position_id;
  uint32_t                           start_time_us = 0;
  uint32_t                           duration_us   = 0;
  f1ap_ntn_access_calendar_direction direction     = f1ap_ntn_access_calendar_direction::invalid;
  f1ap_ntn_access_calendar_purpose   purpose       = f1ap_ntn_access_calendar_purpose::invalid;
  uint16_t                           port_id       = no_resource_port;
};

struct f1ap_ntn_access_calendar_cell {
  du_cell_index_t                             du_cell_index = INVALID_DU_CELL_INDEX;
  nr_cell_identity                           nci        = nr_cell_identity::min();
  pci_t                                      pci        = INVALID_PCI;
  std::vector<f1ap_ntn_access_calendar_intent> intents;
};

/// Versioned CU-to-DU access-calendar request. This is an intent contract and does not claim RF application.
struct f1ap_ntn_access_calendar_update {
  std::string                              satellite_id;
  uint64_t                                 catalog_version          = 0;
  uint64_t                                 schedule_version         = 0;
  std::string                              source_content_hash;
  std::string                              calendar_hash;
  uint64_t                                 activation_epoch_unix_ms = 0;
  uint64_t                                 valid_until_unix_ms      = 0;
  uint32_t                                 cycle_duration_us        = 0;
  f1ap_ntn_access_calendar_operation       operation = f1ap_ntn_access_calendar_operation::invalid;
  std::vector<f1ap_ntn_access_calendar_cell> cells;
};

struct f1ap_ntn_access_calendar_result {
  uint64_t                                      catalog_version  = 0;
  uint64_t                                      schedule_version = 0;
  std::string                                   source_content_hash;
  std::string                                   calendar_hash;
  f1ap_ntn_access_calendar_result_status        status = f1ap_ntn_access_calendar_result_status::rejected;
  std::string                                   reject_reason;
  std::optional<slot_point>                     activation_slot;
  std::array<uint16_t, 2>                       accepted_intents_per_cell{};
  std::array<f1ap_ntn_access_calendar_preflight_report, 2> preflight_reports{};

  bool accepted() const
  {
    if (status == f1ap_ntn_access_calendar_result_status::cleared) {
      return true;
    }
    if (status != f1ap_ntn_access_calendar_result_status::preparing &&
        status != f1ap_ntn_access_calendar_result_status::ready &&
        status != f1ap_ntn_access_calendar_result_status::applied) {
      return false;
    }
    return std::all_of(preflight_reports.begin(), preflight_reports.end(), [](const auto& report) {
      return report.performed && report.passed && report.numerology < 5 &&
             report.matched_ssb == report.expected_ssb && report.matched_prach == report.expected_prach &&
             !report.first_unmatched.has_value();
    });
  }
};

namespace f1ap_ntn_access_calendar_detail {

static constexpr std::array<uint8_t, 6> update_magic = {'N', 'T', 'N', 'A', 'C', 'U'};
static constexpr std::array<uint8_t, 6> result_magic = {'N', 'T', 'N', 'A', 'C', 'R'};
static constexpr uint8_t                update_wire_version        = 1;
static constexpr uint8_t                legacy_result_wire_version = 1;
static constexpr uint8_t                result_wire_version        = 2;

static constexpr size_t max_satellite_id_length = 128;
static constexpr size_t max_hash_length         = 128;
static constexpr size_t max_position_id_length  = 64;
static constexpr size_t max_reject_reason_length = 512;
static constexpr size_t max_unique_positions    = 256;
static constexpr size_t max_total_intents       = 2560;

class writer
{
public:
  void bytes(const uint8_t* begin, const uint8_t* end) { payload.insert(payload.end(), begin, end); }
  void u8(uint8_t value) { payload.push_back(value); }
  void u16(uint16_t value)
  {
    payload.push_back(static_cast<uint8_t>((value >> 8U) & 0xffU));
    payload.push_back(static_cast<uint8_t>(value & 0xffU));
  }
  void u32(uint32_t value)
  {
    payload.push_back(static_cast<uint8_t>((value >> 24U) & 0xffU));
    payload.push_back(static_cast<uint8_t>((value >> 16U) & 0xffU));
    payload.push_back(static_cast<uint8_t>((value >> 8U) & 0xffU));
    payload.push_back(static_cast<uint8_t>(value & 0xffU));
  }
  void u64(uint64_t value)
  {
    for (int shift = 56; shift >= 0; shift -= 8) {
      payload.push_back(static_cast<uint8_t>((value >> static_cast<unsigned>(shift)) & 0xffU));
    }
  }
  void string(const std::string& value)
  {
    u16(static_cast<uint16_t>(value.size()));
    payload.insert(payload.end(), value.begin(), value.end());
  }
  byte_buffer finish() const
  {
    return byte_buffer::create(span<const uint8_t>(payload.data(), payload.size())).value();
  }

private:
  std::vector<uint8_t> payload;
};

class reader
{
public:
  explicit reader(const byte_buffer& container_) : container(container_) {}

  bool bytes(const uint8_t* expected, size_t count)
  {
    if (!has(count)) {
      return false;
    }
    for (size_t i = 0; i != count; ++i) {
      if (container[offset + i] != expected[i]) {
        return false;
      }
    }
    offset += count;
    return true;
  }
  bool u8(uint8_t& value)
  {
    if (!has(1)) {
      return false;
    }
    value = container[offset++];
    return true;
  }
  bool u16(uint16_t& value)
  {
    if (!has(2)) {
      return false;
    }
    value = (static_cast<uint16_t>(container[offset]) << 8U) | static_cast<uint16_t>(container[offset + 1]);
    offset += 2;
    return true;
  }
  bool u32(uint32_t& value)
  {
    if (!has(4)) {
      return false;
    }
    value = (static_cast<uint32_t>(container[offset]) << 24U) |
            (static_cast<uint32_t>(container[offset + 1]) << 16U) |
            (static_cast<uint32_t>(container[offset + 2]) << 8U) | static_cast<uint32_t>(container[offset + 3]);
    offset += 4;
    return true;
  }
  bool u64(uint64_t& value)
  {
    if (!has(8)) {
      return false;
    }
    value = 0;
    for (unsigned i = 0; i != 8; ++i) {
      value = (value << 8U) | static_cast<uint64_t>(container[offset + i]);
    }
    offset += 8;
    return true;
  }
  bool string(std::string& value, size_t max_length, bool allow_empty = false)
  {
    uint16_t length = 0;
    if (!u16(length) || length > max_length || (!allow_empty && length == 0) || !has(length)) {
      return false;
    }
    value.clear();
    value.reserve(length);
    for (unsigned i = 0; i != length; ++i) {
      value.push_back(static_cast<char>(container[offset++]));
    }
    return true;
  }
  bool finished() const { return offset == container.length(); }

private:
  bool has(size_t count) const { return count <= container.length() - std::min(offset, container.length()); }

  const byte_buffer& container;
  size_t             offset = 0;
};

inline bool has_magic(const byte_buffer& container, const std::array<uint8_t, 6>& magic)
{
  if (container.length() < magic.size()) {
    return false;
  }
  for (unsigned i = 0; i != magic.size(); ++i) {
    if (container[i] != magic[i]) {
      return false;
    }
  }
  return true;
}

inline bool valid_l1_position_id(const std::string& value)
{
  return value.size() == 7 && value.front() == 'G' &&
         std::all_of(value.begin() + 1, value.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

inline bool valid_intent(const f1ap_ntn_access_calendar_intent& intent, uint32_t cycle_duration_us)
{
  if (!valid_l1_position_id(intent.position_id) || intent.position_id.size() > max_position_id_length ||
      intent.duration_us == 0 || intent.start_time_us >= cycle_duration_us ||
      static_cast<uint64_t>(intent.start_time_us) + intent.duration_us > cycle_duration_us) {
    return false;
  }

  switch (intent.purpose) {
    case f1ap_ntn_access_calendar_purpose::ssb_sib_paging:
    case f1ap_ntn_access_calendar_purpose::ssb_sib_paging_rar:
      return intent.direction == f1ap_ntn_access_calendar_direction::downlink &&
             intent.port_id != f1ap_ntn_access_calendar_intent::no_resource_port;
    case f1ap_ntn_access_calendar_purpose::prach_ro:
      return intent.direction == f1ap_ntn_access_calendar_direction::uplink &&
             intent.port_id == f1ap_ntn_access_calendar_intent::no_resource_port;
    case f1ap_ntn_access_calendar_purpose::prach_ul_beam:
      return intent.direction == f1ap_ntn_access_calendar_direction::uplink &&
             intent.port_id != f1ap_ntn_access_calendar_intent::no_resource_port;
    case f1ap_ntn_access_calendar_purpose::invalid:
      return false;
  }
  return false;
}

inline bool valid_update(const f1ap_ntn_access_calendar_update& update)
{
  if (update.operation == f1ap_ntn_access_calendar_operation::invalid ||
      static_cast<uint8_t>(update.operation) > static_cast<uint8_t>(f1ap_ntn_access_calendar_operation::clear) ||
      update.satellite_id.empty() || update.satellite_id.size() > max_satellite_id_length ||
      update.catalog_version == 0 || update.schedule_version == 0 || update.source_content_hash.empty() ||
      update.source_content_hash.size() > max_hash_length || update.calendar_hash.empty() ||
      update.calendar_hash.size() > max_hash_length || update.activation_epoch_unix_ms == 0 ||
      update.activation_epoch_unix_ms > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
      update.valid_until_unix_ms > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
      update.valid_until_unix_ms <= update.activation_epoch_unix_ms || update.cycle_duration_us == 0) {
    return false;
  }
  if (update.operation == f1ap_ntn_access_calendar_operation::prepare) {
    if (update.cells.size() != 2) {
      return false;
    }
  } else if (!update.cells.empty() && update.cells.size() != 2) {
    return false;
  }

  std::map<std::string, du_cell_index_t> position_owners;
  size_t                                 nof_intents = 0;
  for (size_t cell_idx = 0; cell_idx != update.cells.size(); ++cell_idx) {
    const auto& cell = update.cells[cell_idx];
    if (cell.du_cell_index >= MAX_NOF_DU_CELLS || !is_valid(cell.pci)) {
      return false;
    }
    for (size_t other_idx = 0; other_idx != cell_idx; ++other_idx) {
      if (update.cells[other_idx].du_cell_index == cell.du_cell_index || update.cells[other_idx].nci == cell.nci) {
        return false;
      }
    }
    nof_intents += cell.intents.size();
    if (nof_intents > max_total_intents) {
      return false;
    }
    for (const auto& intent : cell.intents) {
      if (!valid_intent(intent, update.cycle_duration_us)) {
        return false;
      }
      const auto owner = position_owners.emplace(intent.position_id, cell.du_cell_index);
      if (!owner.second && owner.first->second != cell.du_cell_index) {
        return false;
      }
      if (position_owners.size() > max_unique_positions) {
        return false;
      }
    }
  }
  return true;
}

inline bool valid_preflight_report(const f1ap_ntn_access_calendar_preflight_report& report)
{
  if (!report.performed) {
    return !report.passed && report.numerology == 0xffU && report.expected_ssb == 0 && report.matched_ssb == 0 &&
           report.expected_prach == 0 && report.matched_prach == 0 && report.max_ssb_gap_slots == 0 &&
           report.max_prach_gap_slots == 0 && !report.first_unmatched.has_value();
  }
  if (report.numerology >= 5 || report.matched_ssb > report.expected_ssb ||
      report.matched_prach > report.expected_prach ||
      (report.passed && (report.matched_ssb != report.expected_ssb || report.matched_prach != report.expected_prach ||
                         report.first_unmatched.has_value()))) {
    return false;
  }
  if (!report.first_unmatched.has_value()) {
    return true;
  }

  const auto& unmatched = *report.first_unmatched;
  return !report.passed && valid_l1_position_id(unmatched.position_id) &&
         unmatched.position_id.size() <= max_position_id_length &&
         unmatched.purpose != f1ap_ntn_access_calendar_preflight_purpose::invalid &&
         static_cast<uint8_t>(unmatched.purpose) <=
             static_cast<uint8_t>(f1ap_ntn_access_calendar_preflight_purpose::prach) &&
         unmatched.nof_slots != 0 &&
         static_cast<uint64_t>(unmatched.start_slot_offset) + unmatched.nof_slots <=
             std::numeric_limits<uint32_t>::max();
}

inline void encode_preflight_report(writer& out, const f1ap_ntn_access_calendar_preflight_report& report)
{
  out.u8(report.performed ? 1U : 0U);
  out.u8(report.passed ? 1U : 0U);
  out.u8(report.numerology);
  out.u16(report.expected_ssb);
  out.u16(report.matched_ssb);
  out.u16(report.expected_prach);
  out.u16(report.matched_prach);
  out.u32(report.max_ssb_gap_slots);
  out.u32(report.max_prach_gap_slots);
  out.u8(report.first_unmatched.has_value() ? 1U : 0U);
  if (report.first_unmatched.has_value()) {
    out.string(report.first_unmatched->position_id);
    out.u8(static_cast<uint8_t>(report.first_unmatched->purpose));
    out.u32(report.first_unmatched->start_slot_offset);
    out.u32(report.first_unmatched->nof_slots);
  }
}

inline bool decode_preflight_report(reader& out, f1ap_ntn_access_calendar_preflight_report& report)
{
  uint8_t performed              = 0;
  uint8_t passed                 = 0;
  uint8_t first_unmatched_present = 0;
  if (!out.u8(performed) || performed > 1 || !out.u8(passed) || passed > 1 || !out.u8(report.numerology) ||
      !out.u16(report.expected_ssb) || !out.u16(report.matched_ssb) || !out.u16(report.expected_prach) ||
      !out.u16(report.matched_prach) || !out.u32(report.max_ssb_gap_slots) ||
      !out.u32(report.max_prach_gap_slots) || !out.u8(first_unmatched_present) || first_unmatched_present > 1) {
    return false;
  }
  report.performed = performed != 0;
  report.passed    = passed != 0;
  if (first_unmatched_present != 0) {
    f1ap_ntn_access_calendar_unmatched_intent unmatched;
    uint8_t                                   purpose = 0;
    if (!out.string(unmatched.position_id, max_position_id_length) || !out.u8(purpose) ||
        purpose > static_cast<uint8_t>(f1ap_ntn_access_calendar_preflight_purpose::prach) ||
        !out.u32(unmatched.start_slot_offset) || !out.u32(unmatched.nof_slots)) {
      return false;
    }
    unmatched.purpose = static_cast<f1ap_ntn_access_calendar_preflight_purpose>(purpose);
    report.first_unmatched.emplace(std::move(unmatched));
  }
  return valid_preflight_report(report);
}

} // namespace f1ap_ntn_access_calendar_detail

/// Identifies this private container even when its body/version is malformed, so it cannot fall through to another codec.
inline bool is_f1ap_ntn_access_calendar_update_container(const byte_buffer& container)
{
  return f1ap_ntn_access_calendar_detail::has_magic(container, f1ap_ntn_access_calendar_detail::update_magic);
}

inline byte_buffer encode_f1ap_ntn_access_calendar_update(const f1ap_ntn_access_calendar_update& update)
{
  using namespace f1ap_ntn_access_calendar_detail;
  writer out;
  out.bytes(update_magic.data(), update_magic.data() + update_magic.size());
  out.u8(update_wire_version);
  out.u8(static_cast<uint8_t>(update.operation));
  out.string(update.satellite_id);
  out.u64(update.catalog_version);
  out.u64(update.schedule_version);
  out.string(update.source_content_hash);
  out.string(update.calendar_hash);
  out.u64(update.activation_epoch_unix_ms);
  out.u64(update.valid_until_unix_ms);
  out.u32(update.cycle_duration_us);
  out.u8(static_cast<uint8_t>(update.cells.size()));
  for (const auto& cell : update.cells) {
    out.u16(static_cast<uint16_t>(cell.du_cell_index));
    out.u64(cell.nci.value());
    out.u16(cell.pci);
    out.u16(static_cast<uint16_t>(cell.intents.size()));
    for (const auto& intent : cell.intents) {
      out.string(intent.position_id);
      out.u32(intent.start_time_us);
      out.u32(intent.duration_us);
      out.u8(static_cast<uint8_t>(intent.direction));
      out.u8(static_cast<uint8_t>(intent.purpose));
      out.u16(intent.port_id);
    }
  }
  return out.finish();
}

inline std::optional<f1ap_ntn_access_calendar_update>
decode_f1ap_ntn_access_calendar_update(const byte_buffer& container)
{
  using namespace f1ap_ntn_access_calendar_detail;
  reader out(container);
  uint8_t version   = 0;
  uint8_t operation = 0;
  uint8_t nof_cells = 0;

  f1ap_ntn_access_calendar_update update;
  if (!out.bytes(update_magic.data(), update_magic.size()) || !out.u8(version) || version != update_wire_version ||
      !out.u8(operation) || operation > static_cast<uint8_t>(f1ap_ntn_access_calendar_operation::clear) ||
      !out.string(update.satellite_id, max_satellite_id_length) || !out.u64(update.catalog_version) ||
      !out.u64(update.schedule_version) || !out.string(update.source_content_hash, max_hash_length) ||
      !out.string(update.calendar_hash, max_hash_length) || !out.u64(update.activation_epoch_unix_ms) ||
      !out.u64(update.valid_until_unix_ms) || !out.u32(update.cycle_duration_us) || !out.u8(nof_cells) ||
      (nof_cells != 0 && nof_cells != 2)) {
    return std::nullopt;
  }
  update.operation = static_cast<f1ap_ntn_access_calendar_operation>(operation);
  update.cells.reserve(nof_cells);
  size_t nof_intents = 0;
  for (unsigned cell_count = 0; cell_count != nof_cells; ++cell_count) {
    uint16_t cell_index = 0;
    uint64_t nci_value  = 0;
    uint16_t pci        = 0;
    uint16_t cell_nof_intents = 0;
    if (!out.u16(cell_index) || !out.u64(nci_value) || !out.u16(pci) || !out.u16(cell_nof_intents) ||
        cell_index >= MAX_NOF_DU_CELLS || !is_valid(static_cast<pci_t>(pci))) {
      return std::nullopt;
    }
    const auto nci = nr_cell_identity::create(nci_value);
    if (!nci.has_value() || nof_intents + cell_nof_intents > max_total_intents) {
      return std::nullopt;
    }
    nof_intents += cell_nof_intents;

    f1ap_ntn_access_calendar_cell cell;
    cell.du_cell_index = to_du_cell_index(cell_index);
    cell.nci        = nci.value();
    cell.pci        = static_cast<pci_t>(pci);
    cell.intents.reserve(cell_nof_intents);
    for (unsigned intent_count = 0; intent_count != cell_nof_intents; ++intent_count) {
      f1ap_ntn_access_calendar_intent intent;
      uint8_t direction = 0;
      uint8_t purpose   = 0;
      if (!out.string(intent.position_id, max_position_id_length) || !out.u32(intent.start_time_us) ||
          !out.u32(intent.duration_us) || !out.u8(direction) || !out.u8(purpose) || !out.u16(intent.port_id) ||
          direction > static_cast<uint8_t>(f1ap_ntn_access_calendar_direction::uplink) ||
          purpose > static_cast<uint8_t>(f1ap_ntn_access_calendar_purpose::prach_ul_beam)) {
        return std::nullopt;
      }
      intent.direction = static_cast<f1ap_ntn_access_calendar_direction>(direction);
      intent.purpose   = static_cast<f1ap_ntn_access_calendar_purpose>(purpose);
      cell.intents.push_back(std::move(intent));
    }
    update.cells.push_back(std::move(cell));
  }

  if (!out.finished() || !valid_update(update)) {
    return std::nullopt;
  }
  return update;
}

inline byte_buffer encode_f1ap_ntn_access_calendar_result(const f1ap_ntn_access_calendar_result& result)
{
  using namespace f1ap_ntn_access_calendar_detail;
  writer out;
  out.bytes(result_magic.data(), result_magic.data() + result_magic.size());
  out.u8(result_wire_version);
  out.u64(result.catalog_version);
  out.u64(result.schedule_version);
  out.string(result.source_content_hash);
  out.string(result.calendar_hash);
  out.u8(static_cast<uint8_t>(result.status));
  out.string(result.reject_reason);
  out.u8(result.activation_slot.has_value() ? 1U : 0U);
  out.u8(result.activation_slot.has_value() ? static_cast<uint8_t>(result.activation_slot->numerology()) : 0xffU);
  out.u32(result.activation_slot.has_value() ? result.activation_slot->to_uint() : 0U);
  out.u16(result.accepted_intents_per_cell[0]);
  out.u16(result.accepted_intents_per_cell[1]);
  out.u8(static_cast<uint8_t>(result.preflight_reports.size()));
  for (const auto& report : result.preflight_reports) {
    encode_preflight_report(out, report);
  }
  return out.finish();
}

inline std::optional<f1ap_ntn_access_calendar_result>
decode_f1ap_ntn_access_calendar_result(const byte_buffer& container)
{
  using namespace f1ap_ntn_access_calendar_detail;
  reader out(container);
  uint8_t  version          = 0;
  uint8_t  status           = 0;
  uint8_t  slot_present     = 0;
  uint8_t  slot_numerology  = 0;
  uint32_t slot_count       = 0;

  f1ap_ntn_access_calendar_result result;
  if (!out.bytes(result_magic.data(), result_magic.size()) || !out.u8(version) ||
      (version != legacy_result_wire_version && version != result_wire_version) ||
      !out.u64(result.catalog_version) || !out.u64(result.schedule_version) ||
      !out.string(result.source_content_hash, max_hash_length, true) ||
      !out.string(result.calendar_hash, max_hash_length, true) || !out.u8(status) ||
      status > static_cast<uint8_t>(f1ap_ntn_access_calendar_result_status::preparing) ||
      !out.string(result.reject_reason, max_reject_reason_length, true) || !out.u8(slot_present) || slot_present > 1 ||
      !out.u8(slot_numerology) || !out.u32(slot_count) || !out.u16(result.accepted_intents_per_cell[0]) ||
      !out.u16(result.accepted_intents_per_cell[1])) {
    return std::nullopt;
  }
  result.status = static_cast<f1ap_ntn_access_calendar_result_status>(status);
  if (slot_present != 0) {
    if (slot_numerology >= 5 || slot_count >= (10U * 1024U * (1U << slot_numerology))) {
      return std::nullopt;
    }
    result.activation_slot = slot_point{slot_numerology, slot_count};
  } else if (slot_numerology != 0xffU || slot_count != 0) {
    return std::nullopt;
  }

  if (version == legacy_result_wire_version) {
    if (!out.finished()) {
      return std::nullopt;
    }
    return result;
  }

  uint8_t nof_preflight_reports = 0;
  if (!out.u8(nof_preflight_reports) ||
      nof_preflight_reports != static_cast<uint8_t>(result.preflight_reports.size())) {
    return std::nullopt;
  }
  for (auto& report : result.preflight_reports) {
    if (!decode_preflight_report(out, report)) {
      return std::nullopt;
    }
  }
  if (!out.finished()) {
    return std::nullopt;
  }
  return result;
}

} // namespace srsran
