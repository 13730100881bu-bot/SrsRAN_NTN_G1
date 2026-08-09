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
#include "srsran/cu_cp/cu_cp_types.h"
#include "srsran/f1ap/ntn_ul_slot_resource_request.h"
#include "srsran/f1ap/f1ap_ue_id_types.h"
#include "srsran/ran/du_types.h"
#include "srsran/ran/gnb_du_id.h"
#include "srsran/ran/nr_cgi.h"
#include "srsran/ran/pci.h"
#include "srsran/ran/rnti.h"
#include "srsran/ran/slot_point.h"
#include <array>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace srsran {

enum class f1ap_ntn_rnti_lease_pool_operation : uint8_t { replace = 0, add = 1, clear = 2, retire = 3 };

struct f1ap_ntn_rnti_lease_pool_update {
  gnb_du_id_t                              gnb_du_id = gnb_du_id_t::invalid;
  srs_cu_cp::du_index_t                   du_index  = srs_cu_cp::du_index_t::invalid;
  du_cell_index_t                         cell_index = INVALID_DU_CELL_INDEX;
  nr_cell_global_id_t                     cell_cgi;
  pci_t                                   pci = INVALID_PCI;
  std::string                             analog_beam_id;
  uint32_t                                generation_id = 0;
  uint32_t                                expiry_ms     = 0;
  f1ap_ntn_rnti_lease_pool_operation      operation     = f1ap_ntn_rnti_lease_pool_operation::replace;
  std::vector<rnti_t>                     leases;
};

struct f1ap_ntn_rnti_lease_pool_result {
  uint32_t            generation_id = 0;
  bool                accepted      = false;
  std::string         reject_reason;
  std::vector<rnti_t> accepted_leases;
  std::vector<rnti_t> rejected_leases;
};

struct f1ap_ntn_resource_audit_request {
  srs_cu_cp::du_index_t du_index = srs_cu_cp::du_index_t::invalid;
  du_cell_index_t       cell_index = INVALID_DU_CELL_INDEX;
  pci_t                 pci = INVALID_PCI;
  uint32_t              generation_id = 0;
  /// Requests the DU's safe RNTI retirement capability and generation high-water metadata.
  bool request_retirement_metadata = false;
  /// Requests the connection-bound, F1-identified complete UE SR/SRS snapshot.
  bool                request_ue_slot_identity = false;
  gnb_du_id_t         gnb_du_id = gnb_du_id_t::invalid;
  nr_cell_global_id_t cell_cgi;
  uint64_t            connection_token = 0;
};

struct f1ap_ntn_resource_audit_rnti_lease {
  rnti_t      rnti = rnti_t::INVALID_RNTI;
  std::string state;
  std::string distribution_state;
  uint32_t    generation_id = 0;
};

struct f1ap_ntn_resource_audit_ue_slot {
  /// Legacy codecs used a CU-internal index. It is never encoded by the authoritative identity codec.
  srs_cu_cp::ue_index_t             ue_index = srs_cu_cp::ue_index_t::invalid;
  bool                              identity_present = false;
  gnb_cu_ue_f1ap_id_t               gnb_cu_ue_f1ap_id = gnb_cu_ue_f1ap_id_t::invalid;
  gnb_du_ue_f1ap_id_t               gnb_du_ue_f1ap_id = gnb_du_ue_f1ap_id_t::invalid;
  du_cell_index_t                    cell_index = INVALID_DU_CELL_INDEX;
  nr_cell_global_id_t                cell_cgi;
  pci_t                              pci = INVALID_PCI;
  rnti_t                             c_rnti = rnti_t::INVALID_RNTI;
  uint32_t                           assignment_generation = 0;
  std::string                       state;
  f1ap_ntn_ul_slot_resource_request request;
};

struct f1ap_ntn_resource_audit_result {
  uint32_t                                      generation_id = 0;
  bool                                          accepted = false;
  /// True only when rnti_leases contains the complete DU/MAC lease snapshot for the requested cell.
  bool                                          rnti_snapshot_complete = false;
  /// True only when ue_slots contains the complete DU per-UE SR/SRS snapshot for the requested cell.
  bool                                          ue_slot_snapshot_complete = false;
  /// Present only in the V3 result returned for a V2 retirement-capability request.
  bool                                            retirement_metadata_present = false;
  bool                                            retire_supported            = false;
  uint32_t                                        rnti_generation_high_water  = 0;
  /// Highest UE SR/SRS assignment generation observed in this complete DU snapshot. Independent of the RNTI domain.
  uint32_t                                        ue_slot_assignment_generation_high_water = 0;
  /// Present only in the authoritative UE-slot result. All fields echo the exact request target.
  bool                                          ue_slot_identity_metadata_present = false;
  bool                                          ue_slot_identity_supported = false;
  gnb_du_id_t                                   gnb_du_id = gnb_du_id_t::invalid;
  srs_cu_cp::du_index_t                         du_index = srs_cu_cp::du_index_t::invalid;
  du_cell_index_t                               cell_index = INVALID_DU_CELL_INDEX;
  nr_cell_global_id_t                           cell_cgi;
  pci_t                                         pci = INVALID_PCI;
  uint64_t                                      connection_token = 0;
  std::string                                   reject_reason;
  std::vector<f1ap_ntn_resource_audit_rnti_lease> rnti_leases;
  std::vector<f1ap_ntn_resource_audit_ue_slot>    ue_slots;
};

enum class f1ap_ntn_sib19_broadcast_operation : uint8_t { update = 0, clear = 1, invalid = 255 };

struct f1ap_ntn_sib19_broadcast_update {
  srs_cu_cp::du_index_t              du_index = srs_cu_cp::du_index_t::invalid;
  du_cell_index_t                    cell_index = INVALID_DU_CELL_INDEX;
  pci_t                              pci = INVALID_PCI;
  std::string                        beam_id;
  nr_cell_identity                   nci = nr_cell_identity::min();
  uint32_t                           generation_id = 0;
  f1ap_ntn_sib19_broadcast_operation operation = f1ap_ntn_sib19_broadcast_operation::invalid;
  unsigned                           si_msg_idx = 0;
  unsigned                           sib_idx    = 19;
  slot_point                         valid_from;
  byte_buffer                        packed_sib19;
};

enum class f1ap_ntn_sib19_broadcast_result_status : uint8_t {
  applied = 0,
  clear_applied = 1,
  si_slot_missing = 2,
  packing_failed = 3,
  mac_update_failed = 4,
  malformed_request = 5,
  stale_generation = 6
};

struct f1ap_ntn_sib19_broadcast_result {
  uint32_t                                  generation_id = 0;
  f1ap_ntn_sib19_broadcast_result_status   status =
      f1ap_ntn_sib19_broadcast_result_status::malformed_request;
  std::string reject_reason;

  bool accepted() const
  {
    return status == f1ap_ntn_sib19_broadcast_result_status::applied ||
           status == f1ap_ntn_sib19_broadcast_result_status::clear_applied;
  }
};

inline byte_buffer encode_f1ap_ntn_sib19_broadcast_update(const f1ap_ntn_sib19_broadcast_update& update)
{
  static constexpr std::array<uint8_t, 8> magic = {'S', 'I', 'B', '1', '9', 'U', '0', '1'};

  std::vector<uint8_t> payload;
  payload.insert(payload.end(), magic.begin(), magic.end());

  const auto write_u8 = [&payload](uint8_t value) { payload.push_back(value); };
  const auto write_u16 = [&payload](uint16_t value) {
    payload.push_back(static_cast<uint8_t>((value >> 8U) & 0xffU));
    payload.push_back(static_cast<uint8_t>(value & 0xffU));
  };
  const auto write_u32 = [&payload](uint32_t value) {
    payload.push_back(static_cast<uint8_t>((value >> 24U) & 0xffU));
    payload.push_back(static_cast<uint8_t>((value >> 16U) & 0xffU));
    payload.push_back(static_cast<uint8_t>((value >> 8U) & 0xffU));
    payload.push_back(static_cast<uint8_t>(value & 0xffU));
  };
  const auto write_u64 = [&payload](uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
      payload.push_back(static_cast<uint8_t>((value >> static_cast<unsigned>(shift)) & 0xffU));
    }
  };

  write_u16(srs_cu_cp::du_index_to_uint(update.du_index));
  write_u16(static_cast<uint16_t>(update.cell_index));
  write_u16(update.pci);
  write_u64(update.nci.value());
  write_u32(update.generation_id);
  write_u8(static_cast<uint8_t>(update.operation));
  write_u16(static_cast<uint16_t>(update.si_msg_idx));
  write_u16(static_cast<uint16_t>(update.sib_idx));
  write_u8(update.valid_from.valid() ? static_cast<uint8_t>(update.valid_from.numerology()) : 0xffU);
  write_u32(update.valid_from.valid() ? update.valid_from.to_uint() : 0U);
  write_u16(static_cast<uint16_t>(update.beam_id.size()));
  payload.insert(payload.end(), update.beam_id.begin(), update.beam_id.end());
  write_u32(static_cast<uint32_t>(update.packed_sib19.length()));
  for (unsigned i = 0; i != update.packed_sib19.length(); ++i) {
    payload.push_back(update.packed_sib19[i]);
  }

  return byte_buffer::create(span<const uint8_t>(payload.data(), payload.size())).value();
}

inline std::optional<f1ap_ntn_sib19_broadcast_update>
decode_f1ap_ntn_sib19_broadcast_update(const byte_buffer& container)
{
  static constexpr std::array<uint8_t, 8> magic = {'S', 'I', 'B', '1', '9', 'U', '0', '1'};

  if (container.length() < magic.size() + 31) {
    return std::nullopt;
  }
  for (unsigned i = 0; i != magic.size(); ++i) {
    if (container[i] != magic[i]) {
      return std::nullopt;
    }
  }

  size_t offset = magic.size();
  const auto read_u8 = [&container, &offset](uint8_t& value) {
    if (offset + 1 > container.length()) {
      return false;
    }
    value = container[offset++];
    return true;
  };
  const auto read_u16 = [&container, &offset](uint16_t& value) {
    if (offset + 2 > container.length()) {
      return false;
    }
    value = (static_cast<uint16_t>(container[offset]) << 8U) | static_cast<uint16_t>(container[offset + 1]);
    offset += 2;
    return true;
  };
  const auto read_u32 = [&container, &offset](uint32_t& value) {
    if (offset + 4 > container.length()) {
      return false;
    }
    value = (static_cast<uint32_t>(container[offset]) << 24U) |
            (static_cast<uint32_t>(container[offset + 1]) << 16U) |
            (static_cast<uint32_t>(container[offset + 2]) << 8U) | static_cast<uint32_t>(container[offset + 3]);
    offset += 4;
    return true;
  };
  const auto read_u64 = [&container, &offset](uint64_t& value) {
    if (offset + 8 > container.length()) {
      return false;
    }
    value = 0;
    for (unsigned i = 0; i != 8; ++i) {
      value = (value << 8U) | static_cast<uint64_t>(container[offset + i]);
    }
    offset += 8;
    return true;
  };

  uint16_t du_idx = 0;
  uint16_t cell_idx = 0;
  uint16_t pci = 0;
  uint64_t nci = 0;
  uint32_t generation = 0;
  uint8_t  operation = 0;
  uint16_t si_msg_idx = 0;
  uint16_t sib_idx = 0;
  uint8_t  numerology = 0;
  uint32_t slot_count = 0;
  uint16_t beam_id_len = 0;
  uint32_t pdu_len = 0;

  if (!read_u16(du_idx) || !read_u16(cell_idx) || !read_u16(pci) || !read_u64(nci) || !read_u32(generation) ||
      !read_u8(operation) || !read_u16(si_msg_idx) || !read_u16(sib_idx) || !read_u8(numerology) ||
      !read_u32(slot_count) || !read_u16(beam_id_len)) {
    return std::nullopt;
  }
  if (operation > static_cast<uint8_t>(f1ap_ntn_sib19_broadcast_operation::clear) || cell_idx >= MAX_NOF_DU_CELLS ||
      !is_valid(static_cast<pci_t>(pci)) || beam_id_len == 0 || (numerology != 0xffU && numerology >= NOF_NUMEROLOGIES)) {
    return std::nullopt;
  }
  auto nci_value = nr_cell_identity::create(nci);
  if (!nci_value.has_value()) {
    return std::nullopt;
  }
  if (offset + beam_id_len > container.length()) {
    return std::nullopt;
  }

  f1ap_ntn_sib19_broadcast_update update;
  update.du_index      = srs_cu_cp::uint_to_du_index(du_idx);
  update.cell_index    = to_du_cell_index(cell_idx);
  update.pci           = static_cast<pci_t>(pci);
  update.nci           = nci_value.value();
  update.generation_id = generation;
  update.operation     = static_cast<f1ap_ntn_sib19_broadcast_operation>(operation);
  update.si_msg_idx    = si_msg_idx;
  update.sib_idx       = sib_idx;
  if (numerology != 0xffU) {
    update.valid_from = slot_point{numerology, slot_count};
  }
  update.beam_id.reserve(beam_id_len);
  for (unsigned i = 0; i != beam_id_len; ++i) {
    update.beam_id.push_back(static_cast<char>(container[offset++]));
  }
  if (!read_u32(pdu_len) || offset + pdu_len != container.length()) {
    return std::nullopt;
  }
  std::vector<uint8_t> pdu;
  pdu.reserve(pdu_len);
  for (unsigned i = 0; i != pdu_len; ++i) {
    pdu.push_back(container[offset++]);
  }
  update.packed_sib19 = byte_buffer::create(span<const uint8_t>(pdu.data(), pdu.size())).value();

  return update;
}

inline byte_buffer encode_f1ap_ntn_sib19_broadcast_result(const f1ap_ntn_sib19_broadcast_result& result)
{
  static constexpr std::array<uint8_t, 8> magic = {'S', 'I', 'B', '1', '9', 'R', '0', '1'};

  std::vector<uint8_t> payload;
  payload.insert(payload.end(), magic.begin(), magic.end());

  const auto write_u8 = [&payload](uint8_t value) { payload.push_back(value); };
  const auto write_u16 = [&payload](uint16_t value) {
    payload.push_back(static_cast<uint8_t>((value >> 8U) & 0xffU));
    payload.push_back(static_cast<uint8_t>(value & 0xffU));
  };
  const auto write_u32 = [&payload](uint32_t value) {
    payload.push_back(static_cast<uint8_t>((value >> 24U) & 0xffU));
    payload.push_back(static_cast<uint8_t>((value >> 16U) & 0xffU));
    payload.push_back(static_cast<uint8_t>((value >> 8U) & 0xffU));
    payload.push_back(static_cast<uint8_t>(value & 0xffU));
  };

  write_u32(result.generation_id);
  write_u8(static_cast<uint8_t>(result.status));
  write_u16(static_cast<uint16_t>(result.reject_reason.size()));
  payload.insert(payload.end(), result.reject_reason.begin(), result.reject_reason.end());

  return byte_buffer::create(span<const uint8_t>(payload.data(), payload.size())).value();
}

inline std::optional<f1ap_ntn_sib19_broadcast_result>
decode_f1ap_ntn_sib19_broadcast_result(const byte_buffer& container)
{
  static constexpr std::array<uint8_t, 8> magic = {'S', 'I', 'B', '1', '9', 'R', '0', '1'};

  if (container.length() < magic.size() + 7) {
    return std::nullopt;
  }
  for (unsigned i = 0; i != magic.size(); ++i) {
    if (container[i] != magic[i]) {
      return std::nullopt;
    }
  }

  size_t offset = magic.size();
  const auto read_u8 = [&container, &offset](uint8_t& value) {
    if (offset + 1 > container.length()) {
      return false;
    }
    value = container[offset++];
    return true;
  };
  const auto read_u16 = [&container, &offset](uint16_t& value) {
    if (offset + 2 > container.length()) {
      return false;
    }
    value = (static_cast<uint16_t>(container[offset]) << 8U) | static_cast<uint16_t>(container[offset + 1]);
    offset += 2;
    return true;
  };
  const auto read_u32 = [&container, &offset](uint32_t& value) {
    if (offset + 4 > container.length()) {
      return false;
    }
    value = (static_cast<uint32_t>(container[offset]) << 24U) |
            (static_cast<uint32_t>(container[offset + 1]) << 16U) |
            (static_cast<uint32_t>(container[offset + 2]) << 8U) | static_cast<uint32_t>(container[offset + 3]);
    offset += 4;
    return true;
  };

  f1ap_ntn_sib19_broadcast_result result;
  uint8_t                         status = 0;
  uint16_t                        reason_len = 0;
  if (!read_u32(result.generation_id) || !read_u8(status) ||
      status > static_cast<uint8_t>(f1ap_ntn_sib19_broadcast_result_status::stale_generation) ||
      !read_u16(reason_len) || offset + reason_len != container.length()) {
    return std::nullopt;
  }
  result.status = static_cast<f1ap_ntn_sib19_broadcast_result_status>(status);
  result.reject_reason.reserve(reason_len);
  for (unsigned i = 0; i != reason_len; ++i) {
    result.reject_reason.push_back(static_cast<char>(container[offset++]));
  }

  return result;
}

inline byte_buffer encode_f1ap_ntn_rnti_lease_pool_update(const f1ap_ntn_rnti_lease_pool_update& update)
{
  static constexpr std::array<uint8_t, 8> v1_magic = {'R', 'N', 'T', 'L', 'S', 'E', '0', '1'};
  static constexpr std::array<uint8_t, 8> v2_magic = {'R', 'N', 'T', 'L', 'S', 'E', '0', '2'};

  std::vector<uint8_t> payload;
  const auto&          magic = update.operation == f1ap_ntn_rnti_lease_pool_operation::retire ? v2_magic : v1_magic;
  payload.insert(payload.end(), magic.begin(), magic.end());

  const auto write_u8 = [&payload](uint8_t value) { payload.push_back(value); };
  const auto write_u16 = [&payload](uint16_t value) {
    payload.push_back(static_cast<uint8_t>((value >> 8U) & 0xffU));
    payload.push_back(static_cast<uint8_t>(value & 0xffU));
  };
  const auto write_u32 = [&payload](uint32_t value) {
    payload.push_back(static_cast<uint8_t>((value >> 24U) & 0xffU));
    payload.push_back(static_cast<uint8_t>((value >> 16U) & 0xffU));
    payload.push_back(static_cast<uint8_t>((value >> 8U) & 0xffU));
    payload.push_back(static_cast<uint8_t>(value & 0xffU));
  };
  const auto write_u64 = [&payload](uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
      payload.push_back(static_cast<uint8_t>((value >> static_cast<unsigned>(shift)) & 0xffU));
    }
  };

  write_u64(gnb_du_id_to_int(update.gnb_du_id));
  write_u16(srs_cu_cp::du_index_to_uint(update.du_index));
  write_u16(static_cast<uint16_t>(update.cell_index));
  const auto plmn_bytes = update.cell_cgi.plmn_id.to_bytes();
  payload.insert(payload.end(), plmn_bytes.begin(), plmn_bytes.end());
  write_u64(update.cell_cgi.nci.value());
  write_u16(update.pci);
  write_u32(update.generation_id);
  write_u32(update.expiry_ms);
  write_u8(static_cast<uint8_t>(update.operation));
  write_u16(static_cast<uint16_t>(update.analog_beam_id.size()));
  payload.insert(payload.end(), update.analog_beam_id.begin(), update.analog_beam_id.end());
  write_u16(static_cast<uint16_t>(update.leases.size()));
  for (rnti_t lease : update.leases) {
    write_u16(to_value(lease));
  }

  return byte_buffer::create(span<const uint8_t>(payload.data(), payload.size())).value();
}

inline std::optional<f1ap_ntn_rnti_lease_pool_update>
decode_f1ap_ntn_rnti_lease_pool_update(const byte_buffer& container)
{
  static constexpr std::array<uint8_t, 7> magic_prefix = {'R', 'N', 'T', 'L', 'S', 'E', '0'};

  if (container.length() < magic_prefix.size() + 1 + 31) {
    return std::nullopt;
  }
  for (unsigned i = 0; i != magic_prefix.size(); ++i) {
    if (container[i] != magic_prefix[i]) {
      return std::nullopt;
    }
  }
  const uint8_t codec_version = container[magic_prefix.size()];
  if (codec_version != '1' && codec_version != '2') {
    return std::nullopt;
  }

  size_t     offset  = magic_prefix.size() + 1;
  const auto read_u8 = [&container, &offset](uint8_t& value) {
    if (offset + 1 > container.length()) {
      return false;
    }
    value = container[offset++];
    return true;
  };
  const auto read_u16 = [&container, &offset](uint16_t& value) {
    if (offset + 2 > container.length()) {
      return false;
    }
    value = (static_cast<uint16_t>(container[offset]) << 8U) | static_cast<uint16_t>(container[offset + 1]);
    offset += 2;
    return true;
  };
  const auto read_u32 = [&container, &offset](uint32_t& value) {
    if (offset + 4 > container.length()) {
      return false;
    }
    value = (static_cast<uint32_t>(container[offset]) << 24U) |
            (static_cast<uint32_t>(container[offset + 1]) << 16U) |
            (static_cast<uint32_t>(container[offset + 2]) << 8U) | static_cast<uint32_t>(container[offset + 3]);
    offset += 4;
    return true;
  };
  const auto read_u64 = [&container, &offset](uint64_t& value) {
    if (offset + 8 > container.length()) {
      return false;
    }
    value = 0;
    for (unsigned i = 0; i != 8; ++i) {
      value = (value << 8U) | static_cast<uint64_t>(container[offset + i]);
    }
    offset += 8;
    return true;
  };

  uint64_t du_id = 0;
  uint16_t du_idx     = 0;
  uint16_t cell_idx = 0;
  uint64_t nci = 0;
  uint16_t pci = 0;
  uint32_t generation = 0;
  uint32_t expiry = 0;
  uint8_t  operation = 0;
  uint16_t analog_len = 0;
  uint16_t nof_leases = 0;

  f1ap_ntn_rnti_lease_pool_update update;
  if (!read_u64(du_id) || !read_u16(du_idx) || !read_u16(cell_idx)) {
    return std::nullopt;
  }
  if (offset + 3 > container.length()) {
    return std::nullopt;
  }
  std::array<uint8_t, 3> plmn_bytes = {container[offset], container[offset + 1], container[offset + 2]};
  offset += 3;
  if (!read_u64(nci) || !read_u16(pci) || !read_u32(generation) || !read_u32(expiry) || !read_u8(operation) ||
      !read_u16(analog_len)) {
    return std::nullopt;
  }
  if ((codec_version == '1' && operation > static_cast<uint8_t>(f1ap_ntn_rnti_lease_pool_operation::clear)) ||
      (codec_version == '2' && operation != static_cast<uint8_t>(f1ap_ntn_rnti_lease_pool_operation::retire))) {
    return std::nullopt;
  }
  if (offset + analog_len > container.length()) {
    return std::nullopt;
  }
  update.analog_beam_id.reserve(analog_len);
  for (unsigned i = 0; i != analog_len; ++i) {
    update.analog_beam_id.push_back(static_cast<char>(container[offset++]));
  }
  if (!read_u16(nof_leases)) {
    return std::nullopt;
  }
  if (offset + static_cast<size_t>(nof_leases) * 2U != container.length()) {
    return std::nullopt;
  }

  auto plmn = plmn_identity::from_bytes(plmn_bytes);
  auto nci_value = nr_cell_identity::create(nci);
  if (!plmn.has_value() || !nci_value.has_value() || !is_valid(pci) || cell_idx >= MAX_NOF_DU_CELLS) {
    return std::nullopt;
  }
  update.gnb_du_id     = int_to_gnb_du_id(du_id);
  update.du_index      = srs_cu_cp::uint_to_du_index(du_idx);
  update.cell_index    = to_du_cell_index(cell_idx);
  update.cell_cgi      = nr_cell_global_id_t{plmn.value(), nci_value.value()};
  update.pci           = pci;
  update.generation_id = generation;
  update.expiry_ms     = expiry;
  update.operation     = static_cast<f1ap_ntn_rnti_lease_pool_operation>(operation);
  update.leases.reserve(nof_leases);
  for (unsigned i = 0; i != nof_leases; ++i) {
    uint16_t rnti_value = 0;
    if (!read_u16(rnti_value)) {
      return std::nullopt;
    }
    const rnti_t rnti = to_rnti(rnti_value);
    if (!is_crnti(rnti)) {
      return std::nullopt;
    }
    update.leases.push_back(rnti);
  }

  return update;
}

inline byte_buffer encode_f1ap_ntn_rnti_lease_pool_result(const f1ap_ntn_rnti_lease_pool_result& result)
{
  static constexpr std::array<uint8_t, 8> magic = {'R', 'N', 'T', 'A', 'C', 'K', '0', '1'};

  std::vector<uint8_t> payload;
  payload.insert(payload.end(), magic.begin(), magic.end());

  const auto write_u8 = [&payload](uint8_t value) { payload.push_back(value); };
  const auto write_u16 = [&payload](uint16_t value) {
    payload.push_back(static_cast<uint8_t>((value >> 8U) & 0xffU));
    payload.push_back(static_cast<uint8_t>(value & 0xffU));
  };
  const auto write_u32 = [&payload](uint32_t value) {
    payload.push_back(static_cast<uint8_t>((value >> 24U) & 0xffU));
    payload.push_back(static_cast<uint8_t>((value >> 16U) & 0xffU));
    payload.push_back(static_cast<uint8_t>((value >> 8U) & 0xffU));
    payload.push_back(static_cast<uint8_t>(value & 0xffU));
  };

  write_u32(result.generation_id);
  write_u8(result.accepted ? 1 : 0);
  write_u16(static_cast<uint16_t>(result.reject_reason.size()));
  payload.insert(payload.end(), result.reject_reason.begin(), result.reject_reason.end());
  write_u16(static_cast<uint16_t>(result.accepted_leases.size()));
  for (rnti_t lease : result.accepted_leases) {
    write_u16(to_value(lease));
  }
  write_u16(static_cast<uint16_t>(result.rejected_leases.size()));
  for (rnti_t lease : result.rejected_leases) {
    write_u16(to_value(lease));
  }

  return byte_buffer::create(span<const uint8_t>(payload.data(), payload.size())).value();
}

inline std::optional<f1ap_ntn_rnti_lease_pool_result>
decode_f1ap_ntn_rnti_lease_pool_result(const byte_buffer& container)
{
  static constexpr std::array<uint8_t, 8> magic = {'R', 'N', 'T', 'A', 'C', 'K', '0', '1'};

  if (container.length() < magic.size() + 9) {
    return std::nullopt;
  }
  for (unsigned i = 0; i != magic.size(); ++i) {
    if (container[i] != magic[i]) {
      return std::nullopt;
    }
  }

  size_t offset = magic.size();
  const auto read_u8 = [&container, &offset](uint8_t& value) {
    if (offset + 1 > container.length()) {
      return false;
    }
    value = container[offset++];
    return true;
  };
  const auto read_u16 = [&container, &offset](uint16_t& value) {
    if (offset + 2 > container.length()) {
      return false;
    }
    value = (static_cast<uint16_t>(container[offset]) << 8U) | static_cast<uint16_t>(container[offset + 1]);
    offset += 2;
    return true;
  };
  const auto read_u32 = [&container, &offset](uint32_t& value) {
    if (offset + 4 > container.length()) {
      return false;
    }
    value = (static_cast<uint32_t>(container[offset]) << 24U) | (static_cast<uint32_t>(container[offset + 1]) << 16U) |
            (static_cast<uint32_t>(container[offset + 2]) << 8U) | static_cast<uint32_t>(container[offset + 3]);
    offset += 4;
    return true;
  };
  const auto read_lease_list = [&container, &offset, &read_u16](std::vector<rnti_t>& leases) {
    uint16_t nof_leases = 0;
    if (!read_u16(nof_leases) || offset + static_cast<size_t>(nof_leases) * 2U > container.length()) {
      return false;
    }
    leases.reserve(nof_leases);
    for (unsigned i = 0; i != nof_leases; ++i) {
      uint16_t rnti_value = 0;
      if (!read_u16(rnti_value)) {
        return false;
      }
      const rnti_t rnti = to_rnti(rnti_value);
      if (!is_crnti(rnti)) {
        return false;
      }
      leases.push_back(rnti);
    }
    return true;
  };

  f1ap_ntn_rnti_lease_pool_result result;
  uint8_t                         accepted = 0;
  uint16_t                        reason_len = 0;
  if (!read_u32(result.generation_id) || !read_u8(accepted) || accepted > 1 || !read_u16(reason_len)) {
    return std::nullopt;
  }
  if (offset + reason_len > container.length()) {
    return std::nullopt;
  }
  result.accepted = accepted == 1;
  result.reject_reason.reserve(reason_len);
  for (unsigned i = 0; i != reason_len; ++i) {
    result.reject_reason.push_back(static_cast<char>(container[offset++]));
  }
  if (!read_lease_list(result.accepted_leases) || !read_lease_list(result.rejected_leases)) {
    return std::nullopt;
  }
  if (offset != container.length()) {
    return std::nullopt;
  }

  return result;
}

inline byte_buffer encode_f1ap_ntn_resource_audit_request(const f1ap_ntn_resource_audit_request& request)
{
  static constexpr std::array<uint8_t, 8> v1_magic = {'N', 'T', 'A', 'U', 'D', 'Q', '0', '1'};
  static constexpr std::array<uint8_t, 8> v2_magic = {'N', 'T', 'A', 'U', 'D', 'Q', '0', '2'};
  static constexpr std::array<uint8_t, 8> v3_magic = {'N', 'T', 'A', 'U', 'D', 'Q', '0', '3'};

  if (request.request_ue_slot_identity &&
      (srs_cu_cp::du_index_to_uint(request.du_index) >
           srs_cu_cp::du_index_to_uint(srs_cu_cp::du_index_t::max) ||
       static_cast<unsigned>(request.cell_index) >= MAX_NOF_DU_CELLS || !is_valid(request.pci) ||
       request.generation_id == 0 || gnb_du_id_to_int(request.gnb_du_id) > gnb_du_id_to_int(gnb_du_id_t::max) ||
       request.connection_token == 0)) {
    return {};
  }

  std::vector<uint8_t> payload;
  const auto&          magic = request.request_ue_slot_identity
                                   ? v3_magic
                                   : (request.request_retirement_metadata ? v2_magic : v1_magic);
  payload.insert(payload.end(), magic.begin(), magic.end());

  const auto write_u16 = [&payload](uint16_t value) {
    payload.push_back(static_cast<uint8_t>((value >> 8U) & 0xffU));
    payload.push_back(static_cast<uint8_t>(value & 0xffU));
  };
  const auto write_u32 = [&payload](uint32_t value) {
    payload.push_back(static_cast<uint8_t>((value >> 24U) & 0xffU));
    payload.push_back(static_cast<uint8_t>((value >> 16U) & 0xffU));
    payload.push_back(static_cast<uint8_t>((value >> 8U) & 0xffU));
    payload.push_back(static_cast<uint8_t>(value & 0xffU));
  };
  const auto write_u64 = [&payload](uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
      payload.push_back(static_cast<uint8_t>((value >> static_cast<unsigned>(shift)) & 0xffU));
    }
  };

  write_u16(srs_cu_cp::du_index_to_uint(request.du_index));
  write_u16(static_cast<uint16_t>(request.cell_index));
  write_u16(request.pci);
  write_u32(request.generation_id);
  if (request.request_ue_slot_identity) {
    uint8_t flags = 0x02U;
    flags |= request.request_retirement_metadata ? 0x01U : 0U;
    payload.push_back(flags);
    write_u64(gnb_du_id_to_int(request.gnb_du_id));
    const auto plmn_bytes = request.cell_cgi.plmn_id.to_bytes();
    payload.insert(payload.end(), plmn_bytes.begin(), plmn_bytes.end());
    write_u64(request.cell_cgi.nci.value());
    write_u64(request.connection_token);
  }

  return byte_buffer::create(span<const uint8_t>(payload.data(), payload.size())).value();
}

inline std::optional<f1ap_ntn_resource_audit_request>
decode_f1ap_ntn_resource_audit_request(const byte_buffer& container)
{
  static constexpr std::array<uint8_t, 7> magic_prefix = {'N', 'T', 'A', 'U', 'D', 'Q', '0'};

  const bool is_v3 = container.length() == magic_prefix.size() + 1 + 38;
  if (container.length() != magic_prefix.size() + 1 + 10 && !is_v3) {
    return std::nullopt;
  }
  for (unsigned i = 0; i != magic_prefix.size(); ++i) {
    if (container[i] != magic_prefix[i]) {
      return std::nullopt;
    }
  }
  const uint8_t codec_version = container[magic_prefix.size()];
  if (codec_version != '1' && codec_version != '2' && codec_version != '3') {
    return std::nullopt;
  }
  if ((codec_version == '3') != is_v3) {
    return std::nullopt;
  }

  size_t     offset   = magic_prefix.size() + 1;
  const auto read_u16 = [&container, &offset](uint16_t& value) {
    if (offset + 2 > container.length()) {
      return false;
    }
    value = (static_cast<uint16_t>(container[offset]) << 8U) | static_cast<uint16_t>(container[offset + 1]);
    offset += 2;
    return true;
  };
  const auto read_u32 = [&container, &offset](uint32_t& value) {
    if (offset + 4 > container.length()) {
      return false;
    }
    value = (static_cast<uint32_t>(container[offset]) << 24U) | (static_cast<uint32_t>(container[offset + 1]) << 16U) |
            (static_cast<uint32_t>(container[offset + 2]) << 8U) | static_cast<uint32_t>(container[offset + 3]);
    offset += 4;
    return true;
  };
  const auto read_u64 = [&container, &offset](uint64_t& value) {
    if (offset + 8 > container.length()) {
      return false;
    }
    value = 0;
    for (unsigned i = 0; i != 8; ++i) {
      value = (value << 8U) | static_cast<uint64_t>(container[offset + i]);
    }
    offset += 8;
    return true;
  };

  uint16_t du_index = 0;
  uint16_t cell_index = 0;
  uint16_t pci = 0;
  f1ap_ntn_resource_audit_request request;
  if (!read_u16(du_index) || !read_u16(cell_index) || !read_u16(pci) || !read_u32(request.generation_id)) {
    return std::nullopt;
  }
  if (cell_index >= MAX_NOF_DU_CELLS || !is_valid(static_cast<pci_t>(pci)) ||
      (codec_version == '3' && request.generation_id == 0)) {
    return std::nullopt;
  }
  request.du_index   = srs_cu_cp::uint_to_du_index(du_index);
  request.cell_index = to_du_cell_index(cell_index);
  request.pci        = pci;
  request.request_retirement_metadata = codec_version == '2';
  if (codec_version == '3') {
    uint8_t  flags = 0;
    uint64_t du_id = 0;
    if (offset + 1 > container.length()) {
      return std::nullopt;
    }
    flags = container[offset++];
    if ((flags & 0xfcU) != 0 || (flags & 0x02U) == 0 || !read_u64(du_id) ||
        offset + 3 > container.length()) {
      return std::nullopt;
    }
    std::array<uint8_t, 3> plmn_bytes{};
    for (unsigned i = 0; i != plmn_bytes.size(); ++i) {
      plmn_bytes[i] = container[offset++];
    }
    uint64_t nci_value = 0;
    uint64_t connection_token = 0;
    const auto plmn = plmn_identity::from_bytes(plmn_bytes);
    if (!read_u64(nci_value) || !read_u64(connection_token) || offset != container.length() || !plmn.has_value()) {
      return std::nullopt;
    }
    const auto nci = nr_cell_identity::create(nci_value);
    const auto gnb_du_id = int_to_gnb_du_id(du_id);
    if (du_index > srs_cu_cp::du_index_to_uint(srs_cu_cp::du_index_t::max) || !nci.has_value() ||
        du_id > gnb_du_id_to_int(gnb_du_id_t::max) || gnb_du_id == gnb_du_id_t::invalid || connection_token == 0) {
      return std::nullopt;
    }
    request.request_retirement_metadata = (flags & 0x01U) != 0;
    request.request_ue_slot_identity    = true;
    request.gnb_du_id                   = gnb_du_id;
    request.cell_cgi                    = nr_cell_global_id_t{plmn.value(), nci.value()};
    request.connection_token            = connection_token;
  }
  return request;
}

inline byte_buffer encode_f1ap_ntn_resource_audit_result(const f1ap_ntn_resource_audit_result& result)
{
  static constexpr std::array<uint8_t, 8> v2_magic = {'N', 'T', 'A', 'U', 'D', 'R', '0', '2'};
  static constexpr std::array<uint8_t, 8> v3_magic = {'N', 'T', 'A', 'U', 'D', 'R', '0', '3'};
  static constexpr std::array<uint8_t, 8> v4_magic = {'N', 'T', 'A', 'U', 'D', 'R', '0', '4'};

  bool authoritative_slot_snapshot_valid = true;
  if (result.ue_slot_identity_metadata_present) {
    if (result.generation_id == 0 ||
        srs_cu_cp::du_index_to_uint(result.du_index) >
            srs_cu_cp::du_index_to_uint(srs_cu_cp::du_index_t::max) ||
        static_cast<unsigned>(result.cell_index) >= MAX_NOF_DU_CELLS || !is_valid(result.pci) ||
        gnb_du_id_to_int(result.gnb_du_id) > gnb_du_id_to_int(gnb_du_id_t::max) || result.connection_token == 0) {
      return {};
    }

    if (result.ue_slot_snapshot_complete) {
      authoritative_slot_snapshot_valid = result.ue_slot_identity_supported && result.ue_slots.size() <= MAX_NOF_DU_UES;
      std::set<uint32_t> observed_cu_f1ap_ids;
      std::set<uint32_t> observed_du_f1ap_ids;
      for (const auto& slot : result.ue_slots) {
        const uint64_t cu_id = gnb_cu_ue_f1ap_id_to_uint(slot.gnb_cu_ue_f1ap_id);
        const uint64_t du_id = gnb_du_ue_f1ap_id_to_uint(slot.gnb_du_ue_f1ap_id);
        if (!authoritative_slot_snapshot_valid || !slot.identity_present || cu_id > UINT32_MAX || du_id > UINT32_MAX ||
            static_cast<unsigned>(slot.cell_index) >= MAX_NOF_DU_CELLS || !is_valid(slot.pci) ||
            !is_crnti(slot.c_rnti) || slot.cell_index != result.cell_index || slot.cell_cgi != result.cell_cgi ||
            slot.pci != result.pci || slot.assignment_generation == 0 ||
            slot.assignment_generation != slot.request.assignment_generation || slot.state != "applied_by_du" ||
            slot.assignment_generation > result.ue_slot_assignment_generation_high_water ||
            !is_valid_f1ap_ntn_authoritative_ul_slot_set(slot.request) ||
            !observed_cu_f1ap_ids.emplace(static_cast<uint32_t>(cu_id)).second ||
            !observed_du_f1ap_ids.emplace(static_cast<uint32_t>(du_id)).second) {
          authoritative_slot_snapshot_valid = false;
          break;
        }
      }
    }
  }

  std::vector<uint8_t> payload;
  const auto&          magic = result.ue_slot_identity_metadata_present
                                   ? v4_magic
                                   : (result.retirement_metadata_present ? v3_magic : v2_magic);
  payload.insert(payload.end(), magic.begin(), magic.end());

  const auto write_u8 = [&payload](uint8_t value) { payload.push_back(value); };
  const auto write_u16 = [&payload](uint16_t value) {
    payload.push_back(static_cast<uint8_t>((value >> 8U) & 0xffU));
    payload.push_back(static_cast<uint8_t>(value & 0xffU));
  };
  const auto write_u32 = [&payload](uint32_t value) {
    payload.push_back(static_cast<uint8_t>((value >> 24U) & 0xffU));
    payload.push_back(static_cast<uint8_t>((value >> 16U) & 0xffU));
    payload.push_back(static_cast<uint8_t>((value >> 8U) & 0xffU));
    payload.push_back(static_cast<uint8_t>(value & 0xffU));
  };
  const auto write_u64 = [&payload](uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
      payload.push_back(static_cast<uint8_t>((value >> static_cast<unsigned>(shift)) & 0xffU));
    }
  };
  const auto write_string = [&payload, &write_u16](const std::string& value) {
    write_u16(static_cast<uint16_t>(value.size()));
    payload.insert(payload.end(), value.begin(), value.end());
  };
  const auto write_request = [&write_u8, &write_u32](const f1ap_ntn_ul_slot_resource_request& request) {
    uint8_t flags = 0;
    flags |= request.sr_slot_offset.has_value() ? 0x01U : 0U;
    flags |= request.sr_slot_period.has_value() ? 0x02U : 0U;
    flags |= request.srs_slot_offset.has_value() ? 0x04U : 0U;
    flags |= request.srs_slot_period.has_value() ? 0x08U : 0U;
    flags |= request.requested_c_rnti.has_value() ? 0x10U : 0U;
    write_u8(flags);
    if (request.sr_slot_offset.has_value()) {
      write_u32(*request.sr_slot_offset);
    }
    if (request.sr_slot_period.has_value()) {
      write_u32(*request.sr_slot_period);
    }
    if (request.srs_slot_offset.has_value()) {
      write_u32(*request.srs_slot_offset);
    }
    if (request.srs_slot_period.has_value()) {
      write_u32(*request.srs_slot_period);
    }
    if (request.requested_c_rnti.has_value()) {
      write_u32(to_value(*request.requested_c_rnti));
    }
  };

  write_u32(result.generation_id);
  write_u8(result.accepted ? 1 : 0);
  uint8_t completeness_flags = 0;
  completeness_flags |= result.rnti_snapshot_complete ? 0x01U : 0U;
  const bool encoded_ue_slot_snapshot_complete =
      result.ue_slot_snapshot_complete &&
      (!result.ue_slot_identity_metadata_present || authoritative_slot_snapshot_valid);
  completeness_flags |= encoded_ue_slot_snapshot_complete ? 0x02U : 0U;
  write_u8(completeness_flags);
  if (result.ue_slot_identity_metadata_present) {
    uint8_t capability_flags = 0x04U;
    capability_flags |= result.retirement_metadata_present ? 0x01U : 0U;
    capability_flags |= result.retire_supported ? 0x02U : 0U;
    capability_flags |= result.ue_slot_identity_supported ? 0x08U : 0U;
    write_u8(capability_flags);
    write_u32(result.rnti_generation_high_water);
    write_u32(result.ue_slot_assignment_generation_high_water);
    write_u16(srs_cu_cp::du_index_to_uint(result.du_index));
    write_u16(static_cast<uint16_t>(result.cell_index));
    write_u16(result.pci);
    write_u64(gnb_du_id_to_int(result.gnb_du_id));
    const auto plmn_bytes = result.cell_cgi.plmn_id.to_bytes();
    payload.insert(payload.end(), plmn_bytes.begin(), plmn_bytes.end());
    write_u64(result.cell_cgi.nci.value());
    write_u64(result.connection_token);
  } else if (result.retirement_metadata_present) {
    uint8_t retirement_flags = 0x01U;
    retirement_flags |= result.retire_supported ? 0x02U : 0U;
    write_u8(retirement_flags);
    write_u32(result.rnti_generation_high_water);
  }
  write_string(result.reject_reason);
  write_u16(static_cast<uint16_t>(result.rnti_leases.size()));
  for (const auto& lease : result.rnti_leases) {
    write_u16(to_value(lease.rnti));
    write_u32(lease.generation_id);
    write_string(lease.state);
    write_string(lease.distribution_state);
  }
  const bool encode_ue_slots = !result.ue_slot_identity_metadata_present || encoded_ue_slot_snapshot_complete;
  write_u16(static_cast<uint16_t>(encode_ue_slots ? result.ue_slots.size() : 0U));
  if (encode_ue_slots) {
    for (const auto& slot : result.ue_slots) {
    if (result.ue_slot_identity_metadata_present) {
      write_u32(gnb_cu_ue_f1ap_id_to_uint(slot.gnb_cu_ue_f1ap_id));
      write_u32(gnb_du_ue_f1ap_id_to_uint(slot.gnb_du_ue_f1ap_id));
      write_u16(static_cast<uint16_t>(slot.cell_index));
      write_u16(slot.pci);
      const auto slot_plmn_bytes = slot.cell_cgi.plmn_id.to_bytes();
      payload.insert(payload.end(), slot_plmn_bytes.begin(), slot_plmn_bytes.end());
      write_u64(slot.cell_cgi.nci.value());
      write_u16(to_value(slot.c_rnti));
      write_u32(slot.assignment_generation);
      write_string(slot.state);
      write_u8(static_cast<uint8_t>(slot.request.operation));
      write_u32(slot.request.assignment_generation);
      write_request(slot.request);
    } else {
      write_u32(static_cast<uint32_t>(srs_cu_cp::ue_index_to_uint(slot.ue_index)));
      write_string(slot.state);
      write_request(slot.request);
    }
    }
  }

  return byte_buffer::create(span<const uint8_t>(payload.data(), payload.size())).value();
}

inline std::optional<f1ap_ntn_resource_audit_result> decode_f1ap_ntn_resource_audit_result(const byte_buffer& container)
{
  static constexpr std::array<uint8_t, 7> magic_prefix = {'N', 'T', 'A', 'U', 'D', 'R', '0'};

  if (container.length() < magic_prefix.size() + 1 + 9) {
    return std::nullopt;
  }
  for (unsigned i = 0; i != magic_prefix.size(); ++i) {
    if (container[i] != magic_prefix[i]) {
      return std::nullopt;
    }
  }
  const uint8_t codec_version = container[magic_prefix.size()];
  if (codec_version != '1' && codec_version != '2' && codec_version != '3' && codec_version != '4') {
    return std::nullopt;
  }

  size_t     offset  = magic_prefix.size() + 1;
  const auto read_u8 = [&container, &offset](uint8_t& value) {
    if (offset + 1 > container.length()) {
      return false;
    }
    value = container[offset++];
    return true;
  };
  const auto read_u16 = [&container, &offset](uint16_t& value) {
    if (offset + 2 > container.length()) {
      return false;
    }
    value = (static_cast<uint16_t>(container[offset]) << 8U) | static_cast<uint16_t>(container[offset + 1]);
    offset += 2;
    return true;
  };
  const auto read_u32 = [&container, &offset](uint32_t& value) {
    if (offset + 4 > container.length()) {
      return false;
    }
    value = (static_cast<uint32_t>(container[offset]) << 24U) | (static_cast<uint32_t>(container[offset + 1]) << 16U) |
            (static_cast<uint32_t>(container[offset + 2]) << 8U) | static_cast<uint32_t>(container[offset + 3]);
    offset += 4;
    return true;
  };
  const auto read_u64 = [&container, &offset](uint64_t& value) {
    if (offset + 8 > container.length()) {
      return false;
    }
    value = 0;
    for (unsigned i = 0; i != 8; ++i) {
      value = (value << 8U) | static_cast<uint64_t>(container[offset + i]);
    }
    offset += 8;
    return true;
  };
  const auto read_string = [&container, &offset, &read_u16](std::string& value) {
    uint16_t size = 0;
    if (!read_u16(size) || offset + size > container.length()) {
      return false;
    }
    value.clear();
    value.reserve(size);
    for (unsigned i = 0; i != size; ++i) {
      value.push_back(static_cast<char>(container[offset++]));
    }
    return true;
  };
  const auto read_request = [&read_u8, &read_u32](f1ap_ntn_ul_slot_resource_request& request,
                                                  bool                                  authoritative,
                                                  bool&                                 semantic_valid) {
    if (authoritative) {
      uint8_t operation = 0;
      if (!read_u8(operation) || !read_u32(request.assignment_generation)) {
        return false;
      }
      if (operation != static_cast<uint8_t>(f1ap_ntn_ul_slot_resource_operation::set) ||
          request.assignment_generation == 0) {
        semantic_valid = false;
      } else {
        request.operation = f1ap_ntn_ul_slot_resource_operation::set;
      }
    }
    uint8_t flags = 0;
    if (!read_u8(flags)) {
      return false;
    }
    semantic_valid = semantic_valid && (flags & 0xe0U) == 0;
    uint32_t value = 0;
    if ((flags & 0x01U) != 0) {
      if (!read_u32(value)) {
        return false;
      }
      request.sr_slot_offset = value;
    }
    if ((flags & 0x02U) != 0) {
      if (!read_u32(value)) {
        return false;
      }
      if (value == 0) {
        semantic_valid = false;
      } else {
        request.sr_slot_period = value;
      }
    }
    if ((flags & 0x04U) != 0) {
      if (!read_u32(value)) {
        return false;
      }
      request.srs_slot_offset = value;
    }
    if ((flags & 0x08U) != 0) {
      if (!read_u32(value)) {
        return false;
      }
      if (value == 0) {
        semantic_valid = false;
      } else {
        request.srs_slot_period = value;
      }
    }
    if ((flags & 0x10U) != 0) {
      if (!read_u32(value)) {
        return false;
      }
      if (value > UINT16_MAX || !is_crnti(to_rnti(static_cast<uint16_t>(value)))) {
        semantic_valid = false;
      } else {
        request.requested_c_rnti = to_rnti(static_cast<uint16_t>(value));
      }
    }
    if (authoritative && !is_valid_f1ap_ntn_authoritative_ul_slot_set(request)) {
      semantic_valid = false;
    }
    return true;
  };

  f1ap_ntn_resource_audit_result result;
  uint8_t  accepted           = 0;
  uint8_t  completeness_flags = 0;
  uint8_t  capability_flags   = 0;
  uint16_t nof_leases         = 0;
  uint16_t nof_slots          = 0;
  if (!read_u32(result.generation_id) || !read_u8(accepted) || accepted > 1) {
    return std::nullopt;
  }
  if (codec_version == '4' && result.generation_id == 0) {
    return std::nullopt;
  }
  if (codec_version != '1' && (!read_u8(completeness_flags) || (completeness_flags & 0xfcU) != 0)) {
    return std::nullopt;
  }
  if (codec_version == '3' && (!read_u8(capability_flags) || (capability_flags & 0xfcU) != 0 ||
                               (capability_flags & 0x01U) == 0 ||
                               !read_u32(result.rnti_generation_high_water))) {
    return std::nullopt;
  }
  if (codec_version == '4') {
    uint16_t du_index = 0;
    uint16_t cell_index = 0;
    uint16_t pci = 0;
    uint64_t du_id = 0;
    uint64_t nci_value = 0;
    if (!read_u8(capability_flags) || (capability_flags & 0xf0U) != 0 || (capability_flags & 0x04U) == 0 ||
        ((capability_flags & 0x02U) != 0 && (capability_flags & 0x01U) == 0) ||
        !read_u32(result.rnti_generation_high_water) ||
        !read_u32(result.ue_slot_assignment_generation_high_water) || !read_u16(du_index) ||
        !read_u16(cell_index) || !read_u16(pci) || !read_u64(du_id) || offset + 3 > container.length()) {
      return std::nullopt;
    }
    std::array<uint8_t, 3> plmn_bytes{};
    for (unsigned i = 0; i != plmn_bytes.size(); ++i) {
      plmn_bytes[i] = container[offset++];
    }
    const auto plmn = plmn_identity::from_bytes(plmn_bytes);
    if (!read_u64(nci_value) || !read_u64(result.connection_token) || !plmn.has_value()) {
      return std::nullopt;
    }
    const auto nci = nr_cell_identity::create(nci_value);
    const auto gnb_du_id = int_to_gnb_du_id(du_id);
    if (du_index > srs_cu_cp::du_index_to_uint(srs_cu_cp::du_index_t::max) || !nci.has_value() ||
        du_id > gnb_du_id_to_int(gnb_du_id_t::max) || gnb_du_id == gnb_du_id_t::invalid ||
        cell_index >= MAX_NOF_DU_CELLS || !is_valid(static_cast<pci_t>(pci)) || result.connection_token == 0) {
      return std::nullopt;
    }
    result.ue_slot_identity_metadata_present = true;
    result.ue_slot_identity_supported        = (capability_flags & 0x08U) != 0;
    result.gnb_du_id                         = gnb_du_id;
    result.du_index                          = srs_cu_cp::uint_to_du_index(du_index);
    result.cell_index                        = to_du_cell_index(cell_index);
    result.cell_cgi                          = nr_cell_global_id_t{plmn.value(), nci.value()};
    result.pci                               = static_cast<pci_t>(pci);
  }
  if (!read_string(result.reject_reason) || !read_u16(nof_leases)) {
    return std::nullopt;
  }
  result.accepted = accepted == 1;
  result.rnti_snapshot_complete      = codec_version != '1' && (completeness_flags & 0x01U) != 0;
  result.ue_slot_snapshot_complete   = codec_version != '1' && (completeness_flags & 0x02U) != 0;
  result.retirement_metadata_present =
      codec_version == '3' || (codec_version == '4' && (capability_flags & 0x01U) != 0);
  result.retire_supported = result.retirement_metadata_present && (capability_flags & 0x02U) != 0;
  bool authoritative_slot_semantic_valid =
      codec_version != '4' || !result.ue_slot_snapshot_complete || result.ue_slot_identity_supported;
  result.rnti_leases.reserve(nof_leases);
  for (unsigned i = 0; i != nof_leases; ++i) {
    uint16_t rnti_value = 0;
    f1ap_ntn_resource_audit_rnti_lease lease;
    if (!read_u16(rnti_value) || !is_crnti(to_rnti(rnti_value))) {
      return std::nullopt;
    }
    lease.rnti = to_rnti(rnti_value);
    if (codec_version != '1' && !read_u32(lease.generation_id)) {
      return std::nullopt;
    }
    if (!read_string(lease.state) || !read_string(lease.distribution_state)) {
      return std::nullopt;
    }
    result.rnti_leases.push_back(std::move(lease));
  }
  if (!read_u16(nof_slots)) {
    return std::nullopt;
  }
  if (codec_version == '4' && nof_slots > MAX_NOF_DU_UES) {
    // Reject before reserving or parsing attacker-controlled entries. A peer can retry with a bounded snapshot.
    return std::nullopt;
  }
  result.ue_slots.reserve(nof_slots);
  std::set<uint32_t> observed_cu_f1ap_ids;
  std::set<uint32_t> observed_du_f1ap_ids;
  for (unsigned i = 0; i != nof_slots; ++i) {
    f1ap_ntn_resource_audit_ue_slot slot;
    if (codec_version == '4') {
      bool     slot_semantic_valid = true;
      uint32_t cu_f1ap_id = 0;
      uint32_t du_f1ap_id = 0;
      uint16_t cell_index = 0;
      uint16_t pci = 0;
      uint64_t nci_value = 0;
      uint16_t c_rnti = 0;
      if (!read_u32(cu_f1ap_id) || !read_u32(du_f1ap_id) || !read_u16(cell_index) || !read_u16(pci) ||
          offset + 3 > container.length()) {
        return std::nullopt;
      }
      std::array<uint8_t, 3> plmn_bytes{};
      for (unsigned j = 0; j != plmn_bytes.size(); ++j) {
        plmn_bytes[j] = container[offset++];
      }
      const auto plmn = plmn_identity::from_bytes(plmn_bytes);
      if (!read_u64(nci_value) || !read_u16(c_rnti) || !read_u32(slot.assignment_generation) ||
          !read_string(slot.state) || !read_request(slot.request, true, slot_semantic_valid)) {
        return std::nullopt;
      }
      const auto nci = nr_cell_identity::create(nci_value);
      const auto cu_id = int_to_gnb_cu_ue_f1ap_id(cu_f1ap_id);
      const auto du_id = int_to_gnb_du_ue_f1ap_id(du_f1ap_id);
      if (!plmn.has_value() || !nci.has_value() || cu_id == gnb_cu_ue_f1ap_id_t::invalid ||
          du_id == gnb_du_ue_f1ap_id_t::invalid || cell_index >= MAX_NOF_DU_CELLS ||
          !is_valid(static_cast<pci_t>(pci)) || !is_crnti(to_rnti(c_rnti)) || slot.assignment_generation == 0 ||
          slot.assignment_generation != slot.request.assignment_generation || slot.state != "applied_by_du" ||
          slot.assignment_generation > result.ue_slot_assignment_generation_high_water ||
          !observed_cu_f1ap_ids.emplace(cu_f1ap_id).second ||
          !observed_du_f1ap_ids.emplace(du_f1ap_id).second) {
        slot_semantic_valid = false;
      }
      if (plmn.has_value() && nci.has_value() && cell_index < MAX_NOF_DU_CELLS &&
          is_valid(static_cast<pci_t>(pci)) && is_crnti(to_rnti(c_rnti))) {
        slot.identity_present   = true;
        slot.gnb_cu_ue_f1ap_id = cu_id;
        slot.gnb_du_ue_f1ap_id = du_id;
        slot.cell_index        = to_du_cell_index(cell_index);
        slot.cell_cgi          = nr_cell_global_id_t{plmn.value(), nci.value()};
        slot.pci               = static_cast<pci_t>(pci);
        slot.c_rnti            = to_rnti(c_rnti);
        if (slot.cell_index != result.cell_index || slot.cell_cgi != result.cell_cgi || slot.pci != result.pci) {
          slot_semantic_valid = false;
        }
      }
      authoritative_slot_semantic_valid = authoritative_slot_semantic_valid && slot_semantic_valid;
      if (nof_slots <= MAX_NOF_DU_UES && slot_semantic_valid) {
        result.ue_slots.push_back(std::move(slot));
      }
    } else {
      uint32_t ue_index = 0;
      bool     request_semantic_valid = true;
      if (!read_u32(ue_index) || ue_index > srs_cu_cp::ue_index_to_uint(srs_cu_cp::ue_index_t::max) ||
          !read_string(slot.state) || !read_request(slot.request, false, request_semantic_valid) ||
          !request_semantic_valid) {
        return std::nullopt;
      }
      slot.ue_index = srs_cu_cp::uint_to_ue_index(ue_index);
      result.ue_slots.push_back(std::move(slot));
    }
  }
  if (offset != container.length()) {
    return std::nullopt;
  }

  if (codec_version == '4' &&
      (!result.ue_slot_snapshot_complete || !authoritative_slot_semantic_valid)) {
    result.ue_slot_snapshot_complete = false;
    result.ue_slots.clear();
  }

  return result;
}

} // namespace srsran
