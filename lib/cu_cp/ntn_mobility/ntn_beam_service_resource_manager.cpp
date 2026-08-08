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

#include "ntn_beam_service_resource_manager.h"
#include <algorithm>

using namespace srsran;
using namespace srs_cu_cp;

static bool is_service_state_slot_eligible(const std::string& state)
{
  return state == "binding_pending" || state == "service_bound";
}

bool ntn_beam_service_resource_manager::is_valid_access_ownership_update(const ntn_access_rnti_ownership_update& update)
{
  return update.ue_index != ue_index_t::invalid && update.du_index != du_index_t::invalid &&
         update.cell_index != srsran::INVALID_DU_CELL_INDEX && update.pci != INVALID_PCI &&
         update.rnti != rnti_t::INVALID_RNTI;
}

bool ntn_beam_service_resource_manager::is_valid_lease_pool_update(const ntn_rnti_lease_pool_update& update)
{
  return update.du_index != du_index_t::invalid && update.cell_index != srsran::INVALID_DU_CELL_INDEX &&
         update.pci != INVALID_PCI && !update.analog_beam_id.empty() && !update.leases.empty();
}

bool ntn_beam_service_resource_manager::are_slot_requests_equal(const f1ap_ntn_ul_slot_resource_request& lhs,
                                                                const f1ap_ntn_ul_slot_resource_request& rhs)
{
  return lhs.sr_slot_offset == rhs.sr_slot_offset && lhs.sr_slot_period == rhs.sr_slot_period &&
         lhs.srs_slot_offset == rhs.srs_slot_offset && lhs.srs_slot_period == rhs.srs_slot_period;
}

ntn_beam_service_resource_manager::repair_key
ntn_beam_service_resource_manager::make_repair_key(const ntn_resource_repair& repair)
{
  return {repair.action,
          repair.ue_index,
          repair.du_index,
          repair.cell_index,
          repair.pci,
          repair.rnti,
          repair.analog_beam_id};
}

ntn_resource_repair_record ntn_beam_service_resource_manager::make_repair_record(const ntn_resource_repair& repair,
                                                                                 uint32_t generation_id)
{
  ntn_resource_repair_record record;
  record.action         = repair.action;
  record.ue_index       = repair.ue_index;
  record.du_index       = repair.du_index;
  record.cell_index     = repair.cell_index;
  record.pci            = repair.pci;
  record.rnti           = repair.rnti;
  record.analog_beam_id = repair.analog_beam_id;
  record.service_beam_id = repair.service_beam_id;
  record.uplink_resource_beam_id  = repair.uplink_resource_beam_id;
  record.uplink_resource_du_index = repair.uplink_resource_du_index;
  record.uplink_resource_nci      = repair.uplink_resource_nci;
  record.has_uplink_resource_nci  = repair.has_uplink_resource_nci;
  record.generation_id  = generation_id;
  record.state          = "queued";
  record.reason         = repair.reason;
  return record;
}

void ntn_beam_service_resource_manager::set_authoritative_rnti_lease_validation_enabled(bool enabled)
{
  authoritative_rnti_lease_validation_enabled = enabled;
}

ntn_rnti_lease_pool_update_result
ntn_beam_service_resource_manager::reserve_rnti_leases(const ntn_rnti_lease_pool_update& update)
{
  if (!is_valid_lease_pool_update(update)) {
    return {};
  }

  ntn_rnti_lease_pool_update_result result;
  std::set<rnti_t>                  leases_in_update;
  for (rnti_t rnti : update.leases) {
    if (!is_crnti(rnti)) {
      result.accepted = false;
      result.state    = "invalid";
      result.reason   = "invalid_rnti";
      return result;
    }
    const rnti_key key{update.du_index, update.cell_index, update.pci, rnti};
    const bool used_in_du_lease_namespace = std::any_of(rnti_leases_by_key.begin(),
                                                        rnti_leases_by_key.end(),
                                                        [du_index = update.du_index, rnti](const auto& entry) {
                                                          return std::get<0>(entry.first) == du_index &&
                                                                 std::get<3>(entry.first) == rnti;
                                                        });
    const bool used_in_du_owner_namespace = std::any_of(access_owner_by_rnti.begin(),
                                                        access_owner_by_rnti.end(),
                                                        [du_index = update.du_index, rnti](const auto& entry) {
                                                          return std::get<0>(entry.first) == du_index &&
                                                                 std::get<3>(entry.first) == rnti;
                                                        });
    const auto     retired_generation         = retired_generation_by_du_rnti.find({update.du_index, rnti});
    if (retired_generation != retired_generation_by_du_rnti.end() &&
        update.generation_id <= retired_generation->second) {
      result.accepted = false;
      result.state    = "conflict";
      result.reason   = "stale_generation";
      return result;
    }
    if (!leases_in_update.emplace(rnti).second || rnti_leases_by_key.find(key) != rnti_leases_by_key.end() ||
        access_owner_by_rnti.find(key) != access_owner_by_rnti.end() || used_in_du_lease_namespace ||
        used_in_du_owner_namespace) {
      result.accepted = false;
      result.state    = "conflict";
      result.reason   = "duplicate_lease";
      return result;
    }
  }

  for (rnti_t rnti : update.leases) {
    ntn_rnti_lease lease;
    lease.du_index       = update.du_index;
    lease.cell_index     = update.cell_index;
    lease.pci            = update.pci;
    lease.rnti           = rnti;
    lease.analog_beam_id = update.analog_beam_id;
    lease.generation_id  = update.generation_id;
    lease.state          = "reserved";
    lease.reason         = "lease_pool";
    lease.distribution_state  = "desired";
    lease.distribution_reason = "local_reservation";
    rnti_leases_by_key.emplace(rnti_key{update.du_index, update.cell_index, update.pci, rnti}, std::move(lease));
    if (retired_generation_by_du_rnti.find({update.du_index, rnti}) != retired_generation_by_du_rnti.end()) {
      ++nof_rnti_leases_reused;
      rnti_retirement_last_reason = "retired_rnti_reused";
    }
    ++result.nof_leases_reserved;
  }

  result.accepted = true;
  result.state    = "reserved";
  result.reason   = "lease_pool";
  return result;
}

void ntn_beam_service_resource_manager::mark_rnti_lease_pool_sent_to_du(const ntn_rnti_lease_pool_update& update)
{
  for (rnti_t rnti : update.leases) {
    auto lease_it = rnti_leases_by_key.find(rnti_key{update.du_index, update.cell_index, update.pci, rnti});
    if (lease_it == rnti_leases_by_key.end() || lease_it->second.generation_id != update.generation_id ||
        lease_it->second.state != "reserved") {
      continue;
    }
    lease_it->second.distribution_state  = "sent_to_du";
    lease_it->second.distribution_reason = "gnb_du_resource_coordination_request";
  }
}

void ntn_beam_service_resource_manager::mark_rnti_lease_pool_ack_unknown(const ntn_rnti_lease_pool_update& update,
                                                                         const std::string& reason)
{
  for (rnti_t rnti : update.leases) {
    auto lease_it = rnti_leases_by_key.find(rnti_key{update.du_index, update.cell_index, update.pci, rnti});
    if (lease_it == rnti_leases_by_key.end() || lease_it->second.generation_id != update.generation_id ||
        lease_it->second.state != "reserved" || lease_it->second.distribution_state != "sent_to_du") {
      continue;
    }
    lease_it->second.distribution_reason = reason.empty() ? "ack_unknown" : "ack_unknown:" + reason;
  }
}

bool ntn_beam_service_resource_manager::mark_rnti_lease_pool_distribution_result(
    const ntn_rnti_lease_pool_update&        update,
    const f1ap_ntn_rnti_lease_pool_result& result)
{
  if (result.generation_id == 0 || result.generation_id != update.generation_id ||
      (result.accepted && !result.rejected_leases.empty()) || (!result.accepted && !result.accepted_leases.empty())) {
    return false;
  }

  const std::set<rnti_t> update_leases(update.leases.begin(), update.leases.end());
  std::set<rnti_t>       result_leases;
  const auto             validate_result_lease = [&](rnti_t rnti) {
    if (update_leases.count(rnti) == 0 || !result_leases.emplace(rnti).second) {
      return false;
    }
    const auto lease_it = rnti_leases_by_key.find(rnti_key{update.du_index, update.cell_index, update.pci, rnti});
    return lease_it != rnti_leases_by_key.end() && lease_it->second.generation_id == update.generation_id;
  };
  if (!std::all_of(result.accepted_leases.begin(), result.accepted_leases.end(), validate_result_lease) ||
      !std::all_of(result.rejected_leases.begin(), result.rejected_leases.end(), validate_result_lease) ||
      result_leases.size() != update_leases.size()) {
    return false;
  }

  for (rnti_t rnti : result.accepted_leases) {
    auto lease_it = rnti_leases_by_key.find(rnti_key{update.du_index, update.cell_index, update.pci, rnti});
    lease_it->second.distribution_state  = "applied_by_du";
    lease_it->second.distribution_reason = result.reject_reason.empty() ? "du_ack" : result.reject_reason;
  }
  for (rnti_t rnti : result.rejected_leases) {
    auto lease_it = rnti_leases_by_key.find(rnti_key{update.du_index, update.cell_index, update.pci, rnti});
    lease_it->second.distribution_state  = "rejected_by_du";
    lease_it->second.distribution_reason = result.reject_reason.empty() ? "du_reject" : result.reject_reason;
  }
  return true;
}

bool ntn_beam_service_resource_manager::is_access_rnti_pool_ready(du_index_t              du_index,
                                                                  srsran::du_cell_index_t cell_index,
                                                                  pci_t                   pci,
                                                                  const std::string&      analog_beam_id) const
{
  return !rnti_pool_below_low_watermark(du_index, cell_index, pci, analog_beam_id, 0);
}

bool ntn_beam_service_resource_manager::rnti_pool_below_low_watermark(du_index_t              du_index,
                                                                      srsran::du_cell_index_t cell_index,
                                                                      pci_t                   pci,
                                                                      const std::string&      analog_beam_id,
                                                                      unsigned                low_watermark) const
{
  unsigned nof_available_leases = 0;
  for (const auto& entry : rnti_leases_by_key) {
    const ntn_rnti_lease& lease = entry.second;
    if (lease.du_index != du_index || lease.cell_index != cell_index || lease.pci != pci ||
        lease.analog_beam_id != analog_beam_id) {
      continue;
    }
    if (lease.distribution_state == "applied_by_du" && lease.state == "reserved") {
      ++nof_available_leases;
    }
  }
  return nof_available_leases <= low_watermark;
}

bool ntn_beam_service_resource_manager::has_unresolved_rnti_lease_pool(du_index_t              du_index,
                                                                       srsran::du_cell_index_t cell_index,
                                                                       pci_t                   pci,
                                                                       const std::string&      analog_beam_id) const
{
  return std::any_of(rnti_leases_by_key.begin(), rnti_leases_by_key.end(), [&](const auto& entry) {
    const ntn_rnti_lease& lease = entry.second;
    return lease.du_index == du_index && lease.cell_index == cell_index && lease.pci == pci &&
           lease.analog_beam_id == analog_beam_id && lease.state == "reserved" &&
           (lease.distribution_state == "desired" || lease.distribution_state == "sent_to_du");
  });
}

ntn_handover_target_rnti_reservation_result
ntn_beam_service_resource_manager::reserve_handover_target_rnti(ue_index_t              source_ue_index,
    du_index_t              target_du_index,
    srsran::du_cell_index_t target_cell_index,
    pci_t                   target_pci,
    const std::string&      target_analog_beam_id)
{
  if (source_ue_index == ue_index_t::invalid || target_du_index == du_index_t::invalid ||
      target_cell_index == srsran::INVALID_DU_CELL_INDEX || target_pci == INVALID_PCI ||
      target_analog_beam_id.empty()) {
    return {};
  }

  for (auto& entry : rnti_leases_by_key) {
    ntn_rnti_lease& lease = entry.second;
    if (lease.du_index != target_du_index || lease.cell_index != target_cell_index || lease.pci != target_pci ||
        lease.analog_beam_id != target_analog_beam_id) {
      continue;
    }
    if (lease.distribution_state != "applied_by_du" || lease.state != "reserved") {
      continue;
    }

    lease.state    = "handover_reserved";
    lease.reason   = "connected_handover_target";
    lease.ue_index = source_ue_index;
    return {true, lease.rnti, "handover_reserved", "connected_handover_target"};
  }

  return {false, rnti_t::INVALID_RNTI, "blocked", "target_rnti_unavailable"};
}

ntn_access_rnti_ownership_result
ntn_beam_service_resource_manager::commit_handover_target_rnti(ue_index_t source_ue_index,
                                                               ue_index_t target_ue_index,
                                                               rnti_t     target_rnti)
{
  if (source_ue_index == ue_index_t::invalid || target_ue_index == ue_index_t::invalid ||
      target_rnti == rnti_t::INVALID_RNTI) {
    return {};
  }

  for (auto& entry : rnti_leases_by_key) {
    ntn_rnti_lease& lease = entry.second;
    if (lease.ue_index != source_ue_index || lease.state != "handover_reserved") {
      continue;
    }
    if (lease.rnti != target_rnti) {
      lease.state  = "conflict";
      lease.reason = "target_rnti_mismatch";
      return {false, "conflict", "target_rnti_mismatch"};
    }
    lease.state    = "committed";
    lease.reason   = "connected_handover_success";
    lease.ue_index = target_ue_index;
    return {true, "committed", "connected_handover_success"};
  }

  return {false, "conflict", "target_rnti_unavailable"};
}

void ntn_beam_service_resource_manager::rollback_handover_target_rnti(ue_index_t source_ue_index, std::string reason)
{
  if (source_ue_index == ue_index_t::invalid) {
    return;
  }

  for (auto& entry : rnti_leases_by_key) {
    ntn_rnti_lease& lease = entry.second;
    if (lease.ue_index != source_ue_index || lease.state != "handover_reserved") {
      continue;
    }
    lease.state    = "reserved";
    lease.reason   = reason;
    lease.ue_index = ue_index_t::invalid;
  }
}

ntn_access_rnti_ownership_result
ntn_beam_service_resource_manager::mark_rnti_offered_in_rar(du_index_t              du_index,
                                                            srsran::du_cell_index_t cell_index,
                                                            pci_t                   pci,
                                                            rnti_t                  rnti)
{
  const rnti_key key{du_index, cell_index, pci, rnti};
  auto           lease_it = rnti_leases_by_key.find(key);
  if (lease_it == rnti_leases_by_key.end()) {
    return {false, "conflict", "unexpected_rnti"};
  }
  if (lease_it->second.distribution_state != "applied_by_du") {
    return {false, "conflict", "rnti_pool_unavailable"};
  }
  if (lease_it->second.state == "expired") {
    return {false, "conflict", "expired_rnti"};
  }
  if (lease_it->second.state != "reserved") {
    return {false, "conflict", "duplicate_rnti"};
  }

  lease_it->second.state  = "offered_in_rar";
  lease_it->second.reason = "rar_offer";
  return {true, "offered_in_rar", "rar_offer"};
}

unsigned ntn_beam_service_resource_manager::expire_rnti_leases_for_analog_beam(const std::string& analog_beam_id,
                                                                               std::string        reason)
{
  unsigned nof_expired = 0;
  for (auto& entry : rnti_leases_by_key) {
    ntn_rnti_lease& lease = entry.second;
    // Closing an access window expires only unused leases. RAR, Initial UL, committed and handover records remain
    // quarantined until their own UE lifecycle reaches an explicit terminal state.
    if (lease.analog_beam_id != analog_beam_id || lease.state != "reserved") {
      continue;
    }
    lease.state  = "expired";
    lease.reason = reason;
    lease.ue_index = ue_index_t::invalid;
    ++nof_expired;
  }
  return nof_expired;
}

ntn_access_rnti_ownership_result
ntn_beam_service_resource_manager::validate_access_rnti_ownership(const ntn_access_rnti_ownership_update& update) const
{
  if (!is_valid_access_ownership_update(update)) {
    return {};
  }

  const rnti_key key{update.du_index, update.cell_index, update.pci, update.rnti};
  const auto     lease_it = rnti_leases_by_key.find(key);
  if (authoritative_rnti_lease_validation_enabled && lease_it == rnti_leases_by_key.end()) {
    return {false, "conflict", "unexpected_rnti"};
  }

  if (authoritative_rnti_lease_validation_enabled && lease_it != rnti_leases_by_key.end()) {
    const ntn_rnti_lease& lease = lease_it->second;
    if (lease.analog_beam_id != update.analog_beam_id ||
        (update.cell_index != srsran::INVALID_DU_CELL_INDEX && lease.cell_index != update.cell_index)) {
      return {false, "conflict", "wrong_cell_or_beam"};
    }
    if (lease.state == "expired") {
      return {false, "conflict", "expired_rnti"};
    }
    if (lease.distribution_state != "applied_by_du") {
      return {false, "conflict", "rnti_pool_unavailable"};
    }
    if (lease.state != "reserved" && lease.state != "offered_in_rar" && lease.state != "consumed_by_du") {
      return {false, "conflict", "duplicate_rnti"};
    }
  }

  const auto existing_owner_it = access_owner_by_rnti.find(key);
  if (existing_owner_it != access_owner_by_rnti.end() && existing_owner_it->second != update.ue_index) {
    return {false, "conflict", "duplicate_crnti"};
  }

  return lease_it != rnti_leases_by_key.end()
             ? ntn_access_rnti_ownership_result{true, "initial_ul_seen", "lease_validated"}
                                              : ntn_access_rnti_ownership_result{true, "observed", "access_active"};
}

ntn_access_rnti_ownership_result
ntn_beam_service_resource_manager::register_access_rnti_ownership(const ntn_access_rnti_ownership_update& update)
{
  if (!is_valid_access_ownership_update(update)) {
    return {};
  }

  const rnti_key key{update.du_index, update.cell_index, update.pci, update.rnti};
  auto           lease_it = rnti_leases_by_key.find(key);
  if (authoritative_rnti_lease_validation_enabled && lease_it == rnti_leases_by_key.end()) {
    ntn_access_rnti_ownership conflict;
    conflict.ue_index       = update.ue_index;
    conflict.du_index       = update.du_index;
    conflict.cell_index     = update.cell_index;
    conflict.pci            = update.pci;
    conflict.rnti           = update.rnti;
    conflict.analog_beam_id = update.analog_beam_id;
    conflict.access_nci     = update.access_nci;
    conflict.has_access_nci = update.has_access_nci;
    conflict.state          = "conflict";
    conflict.reason         = "unexpected_rnti";
    access_ownership_by_ue[update.ue_index] = std::move(conflict);
    return {false, "conflict", "unexpected_rnti"};
  }

  if (authoritative_rnti_lease_validation_enabled && lease_it != rnti_leases_by_key.end()) {
    const ntn_rnti_lease& lease = lease_it->second;
    auto make_rejected_access = [&](const char* reason) {
      ntn_access_rnti_ownership conflict;
      conflict.ue_index       = update.ue_index;
      conflict.du_index       = update.du_index;
      conflict.cell_index     = update.cell_index;
      conflict.pci            = update.pci;
      conflict.rnti           = update.rnti;
      conflict.analog_beam_id = update.analog_beam_id;
      conflict.access_nci     = update.access_nci;
      conflict.has_access_nci = update.has_access_nci;
      conflict.state          = "conflict";
      conflict.reason         = reason;
      access_ownership_by_ue[update.ue_index] = std::move(conflict);
      return ntn_access_rnti_ownership_result{false, "conflict", reason};
    };
    if (lease.analog_beam_id != update.analog_beam_id ||
        (update.cell_index != srsran::INVALID_DU_CELL_INDEX && lease.cell_index != update.cell_index)) {
      return make_rejected_access("wrong_cell_or_beam");
    }
    if (lease.state == "expired") {
      return make_rejected_access("expired_rnti");
    }
    if (lease.distribution_state != "applied_by_du") {
      return make_rejected_access("rnti_pool_unavailable");
    }
    if (lease.state != "reserved" && lease.state != "offered_in_rar" && lease.state != "consumed_by_du") {
      return make_rejected_access("duplicate_rnti");
    }
  }

  const auto     existing_owner_it = access_owner_by_rnti.find(key);
  if (existing_owner_it != access_owner_by_rnti.end() && existing_owner_it->second != update.ue_index) {
    ntn_access_rnti_ownership conflict;
    conflict.ue_index       = update.ue_index;
    conflict.du_index       = update.du_index;
    conflict.cell_index     = update.cell_index;
    conflict.pci            = update.pci;
    conflict.rnti           = update.rnti;
    conflict.analog_beam_id = update.analog_beam_id;
    conflict.access_nci     = update.access_nci;
    conflict.has_access_nci = update.has_access_nci;
    conflict.state          = "conflict";
    conflict.reason         = "duplicate_crnti";
    access_ownership_by_ue[update.ue_index] = std::move(conflict);
    return {false, "conflict", "duplicate_crnti"};
  }

  const auto old_key_it = access_rnti_key_by_ue.find(update.ue_index);
  if (old_key_it != access_rnti_key_by_ue.end() && old_key_it->second != key) {
    access_owner_by_rnti.erase(old_key_it->second);
  }

  access_owner_by_rnti[key]       = update.ue_index;
  access_rnti_key_by_ue[update.ue_index] = key;

  ntn_access_rnti_ownership ownership;
  ownership.ue_index       = update.ue_index;
  ownership.du_index       = update.du_index;
  ownership.cell_index     = update.cell_index;
  ownership.pci            = update.pci;
  ownership.rnti           = update.rnti;
  ownership.analog_beam_id = update.analog_beam_id;
  ownership.access_nci     = update.access_nci;
  ownership.has_access_nci = update.has_access_nci;
  ownership.state          = lease_it != rnti_leases_by_key.end() ? "initial_ul_seen" : "observed";
  ownership.reason         = lease_it != rnti_leases_by_key.end() ? "lease_validated" : "access_active";
  access_ownership_by_ue[update.ue_index] = std::move(ownership);

  if (lease_it != rnti_leases_by_key.end()) {
    lease_it->second.state    = "initial_ul_seen";
    lease_it->second.reason   = "lease_validated";
    lease_it->second.ue_index = update.ue_index;
    return {true, "initial_ul_seen", "lease_validated"};
  }
  return {true, "observed", "access_active"};
}

void ntn_beam_service_resource_manager::release_analog_access_after_ics(ue_index_t ue_index)
{
  auto ownership_it = access_ownership_by_ue.find(ue_index);
  if (ownership_it == access_ownership_by_ue.end() || ownership_it->second.state == "conflict") {
    return;
  }
  ownership_it->second.state  = "released_after_ics";
  ownership_it->second.reason = "released_after_ics";

  const auto key_it = access_rnti_key_by_ue.find(ue_index);
  if (key_it != access_rnti_key_by_ue.end()) {
    auto lease_it = rnti_leases_by_key.find(key_it->second);
    if (lease_it != rnti_leases_by_key.end() && lease_it->second.ue_index == ue_index) {
      lease_it->second.state  = "committed";
      lease_it->second.reason = "initial_context_setup_complete";
    }
  }
}

std::optional<f1ap_ntn_ul_slot_resource_request>
ntn_beam_service_resource_manager::make_slot_request_for_nci(nr_cell_identity              nci,
                                                             du_index_t                    service_du_index,
                                                             const ntn_beam_placement_plan& plan) const
{
  for (const ntn_beam_du_assignment& assignment : plan.assignments) {
    if (assignment.nci != nci || assignment.du_index != service_du_index) {
      continue;
    }
    if (assignment.state != ntn_beam_assignment_state::active_loaded &&
        assignment.state != ntn_beam_assignment_state::draining) {
      continue;
    }
    if (!assignment.uplink_resource_required || !assignment.uplink_ready) {
      continue;
    }

    f1ap_ntn_ul_slot_resource_request request;
    if (assignment.sr_slot_period != 0) {
      request.sr_slot_offset = assignment.sr_slot_offset;
      request.sr_slot_period = assignment.sr_slot_period;
    }
    if (assignment.srs_slot_period != 0) {
      request.srs_slot_offset = assignment.srs_slot_offset;
      request.srs_slot_period = assignment.srs_slot_period;
    }
    if (!is_empty(request)) {
      return request;
    }
  }

  return std::nullopt;
}

ntn_slot_resource_update_decision ntn_beam_service_resource_manager::update_digital_service_slot_intent(
    const ntn_digital_service_slot_intent_update& update,
    const ntn_beam_placement_plan&               plan)
{
  if (update.ue_index == ue_index_t::invalid || update.service_du_index == du_index_t::invalid ||
      !update.has_service_nci || update.digital_beam_id.empty() ||
      !is_service_state_slot_eligible(update.service_state)) {
    return clear_digital_service_slot_intent(update.ue_index, "not_service_bound");
  }

  const bool       has_paired_uplink_resource = !update.uplink_resource_beam_id.empty() &&
                                                update.uplink_resource_du_index != du_index_t::invalid &&
      update.has_uplink_resource_nci;
  const du_index_t slot_du_index =
      has_paired_uplink_resource ? update.uplink_resource_du_index : update.service_du_index;
  const nr_cell_identity slot_nci      = has_paired_uplink_resource ? update.uplink_resource_nci : update.service_nci;
  const std::optional<f1ap_ntn_ul_slot_resource_request> slot_request =
      make_slot_request_for_nci(slot_nci, slot_du_index, plan);
  if (!slot_request.has_value()) {
    return clear_digital_service_slot_intent(update.ue_index, "no_slot_resources");
  }

  ntn_digital_slot_resource_intent intent;
  intent.ue_index         = update.ue_index;
  intent.digital_beam_id  = update.digital_beam_id;
  intent.service_du_index = update.service_du_index;
  intent.service_cell_index = update.service_cell_index;
  intent.service_pci      = update.service_pci;
  intent.service_nci      = update.service_nci;
  intent.has_service_nci  = true;
  if (has_paired_uplink_resource) {
    intent.uplink_resource_beam_id  = update.uplink_resource_beam_id;
    intent.uplink_resource_du_index = update.uplink_resource_du_index;
    intent.uplink_resource_cell_index = update.uplink_resource_cell_index;
    intent.uplink_resource_pci      = update.uplink_resource_pci;
    intent.uplink_resource_nci      = update.uplink_resource_nci;
    intent.has_uplink_resource_nci  = true;
  }
  intent.state            = "active";
  intent.reason           = "loaded_service_calendar";
  intent.request          = *slot_request;
  digital_slot_intent_by_ue[update.ue_index] = intent;

  ntn_slot_resource_update_decision decision;
  decision.ue_index = update.ue_index;
  decision.request  = *slot_request;
  decision.reason   = "loaded_service_calendar";

  const auto cached_it = cached_slot_requests_by_ue.find(update.ue_index);
  if (cached_it != cached_slot_requests_by_ue.end() && are_slot_requests_equal(cached_it->second, *slot_request)) {
    decision.action = ntn_slot_resource_update_action::none;
    return decision;
  }

  const auto applied_it = applied_slot_requests_by_ue.find(update.ue_index);
  if (applied_it != applied_slot_requests_by_ue.end()) {
    decision.previous_request = applied_it->second;
  }
  cached_slot_requests_by_ue[update.ue_index] = *slot_request;
  decision.action = ntn_slot_resource_update_action::set;
  return decision;
}

ntn_slot_resource_update_decision ntn_beam_service_resource_manager::set_digital_slot_intent_from_request(
    ue_index_t                               ue_index,
    const f1ap_ntn_ul_slot_resource_request& request,
    std::string                              reason)
{
  if (ue_index == ue_index_t::invalid || is_empty(request)) {
    return clear_digital_service_slot_intent(ue_index, "empty_slot_request");
  }

  ntn_digital_slot_resource_intent intent;
  intent.ue_index = ue_index;
  intent.state    = "active";
  intent.reason   = reason;
  intent.request  = request;
  digital_slot_intent_by_ue[ue_index] = intent;

  ntn_slot_resource_update_decision decision;
  decision.ue_index = ue_index;
  decision.request  = request;
  decision.reason   = reason;

  const auto cached_it = cached_slot_requests_by_ue.find(ue_index);
  if (cached_it != cached_slot_requests_by_ue.end() && are_slot_requests_equal(cached_it->second, request)) {
    decision.action = ntn_slot_resource_update_action::none;
    return decision;
  }

  const auto applied_it = applied_slot_requests_by_ue.find(ue_index);
  if (applied_it != applied_slot_requests_by_ue.end()) {
    decision.previous_request = applied_it->second;
  }
  cached_slot_requests_by_ue[ue_index] = request;
  decision.action = ntn_slot_resource_update_action::set;
  return decision;
}

ntn_slot_resource_update_decision
ntn_beam_service_resource_manager::clear_digital_service_slot_intent(ue_index_t ue_index, std::string reason)
{
  ntn_slot_resource_update_decision decision;
  decision.ue_index = ue_index;
  decision.reason   = reason;

  if (ue_index == ue_index_t::invalid) {
    return decision;
  }

  const auto applied_it = applied_slot_requests_by_ue.find(ue_index);
  if (applied_it != applied_slot_requests_by_ue.end() && !is_empty(applied_it->second)) {
    decision.action           = ntn_slot_resource_update_action::clear;
    decision.previous_request = applied_it->second;
    decision.request          = f1ap_ntn_ul_slot_resource_request{};
    cached_slot_requests_by_ue[ue_index] = f1ap_ntn_ul_slot_resource_request{};
  } else {
    cached_slot_requests_by_ue.erase(ue_index);
  }

  auto intent_it = digital_slot_intent_by_ue.find(ue_index);
  if (intent_it == digital_slot_intent_by_ue.end()) {
    ntn_digital_slot_resource_intent intent;
    intent.ue_index = ue_index;
    intent.state    = "cleared";
    intent.reason   = std::move(reason);
    digital_slot_intent_by_ue[ue_index] = std::move(intent);
  } else {
    intent_it->second.state   = "cleared";
    intent_it->second.reason  = std::move(reason);
    intent_it->second.request = {};
  }
  return decision;
}

void ntn_beam_service_resource_manager::restore_or_clear_failed_slot_update(
    ue_index_t                                               ue_index,
    const f1ap_ntn_ul_slot_resource_request&                 attempted_request,
    const std::optional<f1ap_ntn_ul_slot_resource_request>& restore_request)
{
  const auto cached_it = cached_slot_requests_by_ue.find(ue_index);
  if (cached_it == cached_slot_requests_by_ue.end() || !are_slot_requests_equal(cached_it->second, attempted_request)) {
    return;
  }
  if (restore_request.has_value()) {
    cached_it->second = *restore_request;
    applied_slot_requests_by_ue[ue_index] = *restore_request;
    auto intent_it = digital_slot_intent_by_ue.find(ue_index);
    if (intent_it != digital_slot_intent_by_ue.end()) {
      intent_it->second.state           = "rollback_restored";
      intent_it->second.reason          = "du_context_update_failed";
      intent_it->second.request         = *restore_request;
      intent_it->second.applied_request = *restore_request;
    }
    return;
  }
  cached_slot_requests_by_ue.erase(cached_it);
  applied_slot_requests_by_ue.erase(ue_index);
  auto intent_it = digital_slot_intent_by_ue.find(ue_index);
  if (intent_it != digital_slot_intent_by_ue.end()) {
    intent_it->second.state           = "rejected_by_du";
    intent_it->second.reason          = "du_context_update_failed";
    intent_it->second.applied_request = std::nullopt;
  }
}

void ntn_beam_service_resource_manager::mark_slot_update_sent_to_du(ue_index_t                               ue_index,
                                                                    const f1ap_ntn_ul_slot_resource_request& request)
{
  auto intent_it = digital_slot_intent_by_ue.find(ue_index);
  if (intent_it == digital_slot_intent_by_ue.end()) {
    return;
  }
  intent_it->second.state  = is_empty(request) ? "clear_sent" : "sent_to_du";
  intent_it->second.reason = is_empty(request) ? "clear_sent_to_du" : "sent_to_du";
  intent_it->second.request = request;
}

void ntn_beam_service_resource_manager::mark_slot_update_result(
    ue_index_t                                               ue_index,
    const f1ap_ntn_ul_slot_resource_request&                 attempted_request,
    const std::optional<f1ap_ntn_ul_slot_resource_request>& restore_request,
    const f1ap_ntn_ul_slot_resource_result&                  result)
{
  auto intent_it = digital_slot_intent_by_ue.find(ue_index);
  if (intent_it == digital_slot_intent_by_ue.end()) {
    return;
  }

  if (result.accepted) {
    const f1ap_ntn_ul_slot_resource_request applied_request = result.applied_request.value_or(attempted_request);
    if (is_empty(applied_request)) {
      cached_slot_requests_by_ue.erase(ue_index);
      applied_slot_requests_by_ue.erase(ue_index);
      intent_it->second.state           = "cleared_by_du";
      intent_it->second.reason          = to_string(result.reason);
      intent_it->second.request         = {};
      intent_it->second.applied_request = std::nullopt;
      return;
    }

    cached_slot_requests_by_ue[ue_index]  = applied_request;
    applied_slot_requests_by_ue[ue_index] = applied_request;
    intent_it->second.state               = "applied_by_du";
    intent_it->second.reason              = to_string(result.reason);
    intent_it->second.request             = applied_request;
    intent_it->second.applied_request     = applied_request;
    return;
  }

  intent_it->second.state  = "rejected_by_du";
  intent_it->second.reason = to_string(result.reason);
  if (restore_request.has_value()) {
    cached_slot_requests_by_ue[ue_index]  = *restore_request;
    applied_slot_requests_by_ue[ue_index] = *restore_request;
    intent_it->second.request             = *restore_request;
    intent_it->second.applied_request     = *restore_request;
  } else {
    cached_slot_requests_by_ue.erase(ue_index);
    applied_slot_requests_by_ue.erase(ue_index);
    intent_it->second.request         = {};
    intent_it->second.applied_request = std::nullopt;
  }
}

void ntn_beam_service_resource_manager::mark_slot_update_applied(ue_index_t                               ue_index,
                                                                 const f1ap_ntn_ul_slot_resource_request& request)
{
  if (is_empty(request)) {
    cached_slot_requests_by_ue.erase(ue_index);
    applied_slot_requests_by_ue.erase(ue_index);
    auto intent_it = digital_slot_intent_by_ue.find(ue_index);
    if (intent_it != digital_slot_intent_by_ue.end()) {
      intent_it->second.state  = "cleared_by_du";
      intent_it->second.reason = "applied_clear";
      intent_it->second.request = {};
      intent_it->second.applied_request = std::nullopt;
    }
    return;
  }
  cached_slot_requests_by_ue[ue_index] = request;
  applied_slot_requests_by_ue[ue_index] = request;
  auto intent_it = digital_slot_intent_by_ue.find(ue_index);
  if (intent_it != digital_slot_intent_by_ue.end()) {
    intent_it->second.state           = "applied_by_du";
    intent_it->second.reason          = "applied";
    intent_it->second.request         = request;
    intent_it->second.applied_request = request;
  }
}

bool ntn_beam_service_resource_manager::has_active_digital_slot_intent(ue_index_t ue_index) const
{
  const auto intent_it = digital_slot_intent_by_ue.find(ue_index);
  return intent_it != digital_slot_intent_by_ue.end() &&
         (intent_it->second.state == "active" || intent_it->second.state == "sent_to_du" ||
          intent_it->second.state == "applied_by_du" || intent_it->second.state == "rollback_restored") &&
         !is_empty(intent_it->second.request);
}

std::optional<f1ap_ntn_ul_slot_resource_request>
ntn_beam_service_resource_manager::get_cached_slot_request(ue_index_t ue_index) const
{
  const auto cached_it = cached_slot_requests_by_ue.find(ue_index);
  return cached_it != cached_slot_requests_by_ue.end()
             ? std::optional<f1ap_ntn_ul_slot_resource_request>{cached_it->second}
             : std::nullopt;
}

ntn_resource_audit_decision
ntn_beam_service_resource_manager::handle_resource_audit_report(const ntn_resource_audit_report& report)
{
  ntn_resource_audit_decision decision;
  decision.generation_id = report.generation_id;

  if (!report.accepted) {
    ntn_resource_repair repair;
    repair.action     = ntn_resource_repair_action::mark_resource_conflict;
    repair.du_index   = report.du_index;
    repair.cell_index = report.cell_index;
    repair.pci        = report.pci;
    repair.reason     = report.reject_reason.empty() ? "du_audit_rejected" : report.reject_reason;
    decision.repairs.push_back(std::move(repair));
    decision.nof_mismatches = 1;
    decision.nof_repairs    = 1;
    return decision;
  }

  const auto resolve_recovered_audit_rejection = [&]() {
    if ((!report.rnti_snapshot_complete && !report.ue_slot_snapshot_complete) ||
        std::any_of(decision.repairs.begin(), decision.repairs.end(), [](const ntn_resource_repair& repair) {
          return repair.action == ntn_resource_repair_action::mark_resource_conflict;
        })) {
      return;
    }
    for (auto& entry : resource_repairs_by_key) {
      ntn_resource_repair_record& record = entry.second;
      const bool generic_audit_rejection =
          record.action == ntn_resource_repair_action::mark_resource_conflict &&
          record.state == "blocked_conflict" && record.du_index == report.du_index &&
          record.cell_index == report.cell_index && record.pci == report.pci &&
          record.ue_index == ue_index_t::invalid && record.rnti == rnti_t::INVALID_RNTI &&
          record.analog_beam_id.empty();
      if (generic_audit_rejection) {
        record.generation_id = report.generation_id;
        record.state         = "resolved";
        record.reason        = "du_audit_recovered";
      }
    }
  };

  // Capability metadata is independent of whether the lease list is complete. Keep observations from an incomplete
  // list separate from the authoritative connection and generation high-water used by the retirement decisions below.
  // This keeps OAM useful without allowing an incomplete response to authorize work or to mask a legitimate ledger
  // reset reported by the first complete audit after a reconnect.
  if (report.rnti_retirement_capability_known && !report.rnti_snapshot_complete) {
    auto& du_retirement = rnti_retirement_by_du[report.du_index];
    const uint64_t known_connection = du_retirement.du_connection_generation != 0
                                          ? du_retirement.du_connection_generation
                                          : du_retirement.observed_connection_generation;
    const bool live_connection =
        (known_connection == 0 || known_connection == report.du_connection_generation) &&
        (du_retirement.invalidated_connection_generation == 0 ||
         report.du_connection_generation > du_retirement.invalidated_connection_generation);
    if (live_connection) {
      du_retirement.observed_connection_generation = report.du_connection_generation;
      du_retirement.observed_capability_known      = true;
      du_retirement.observed_supported             = report.rnti_retirement_supported;
      du_retirement.observed_generation_high_water =
          std::max(du_retirement.observed_generation_high_water, report.rnti_generation_high_water);
    }
  }

  if (report.rnti_snapshot_complete) {
    bool                                                  rnti_snapshot_valid = true;
    std::map<rnti_t, const ntn_resource_audit_rnti_lease*> observed_rntis;
    std::set<rnti_t>                                       rntis_in_other_local_cells;
    for (const auto& entry : rnti_leases_by_key) {
      if (std::get<0>(entry.first) == report.du_index &&
          (std::get<1>(entry.first) != report.cell_index || std::get<2>(entry.first) != report.pci)) {
        rntis_in_other_local_cells.emplace(std::get<3>(entry.first));
      }
    }
    for (const auto& entry : access_owner_by_rnti) {
      if (std::get<0>(entry.first) == report.du_index &&
          (std::get<1>(entry.first) != report.cell_index || std::get<2>(entry.first) != report.pci)) {
        rntis_in_other_local_cells.emplace(std::get<3>(entry.first));
      }
    }
    auto&      du_retirement = rnti_retirement_by_du[report.du_index];
    const bool new_connection_after_invalidation =
        du_retirement.du_connection_generation == 0 && du_retirement.invalidated_connection_generation != 0 &&
        report.du_connection_generation > du_retirement.invalidated_connection_generation;
    const bool ledger_reset_snapshot = new_connection_after_invalidation && report.rnti_leases.empty() &&
                                       report.rnti_retirement_capability_known && report.rnti_retirement_supported &&
                                       report.rnti_generation_high_water == 0;
    const uint64_t known_connection = du_retirement.du_connection_generation != 0
                                          ? du_retirement.du_connection_generation
                                          : du_retirement.observed_connection_generation;
    const bool stale_connection = (known_connection != 0 && report.du_connection_generation != known_connection) ||
                                  (du_retirement.invalidated_connection_generation != 0 &&
                                   report.du_connection_generation <= du_retirement.invalidated_connection_generation);
    const uint32_t known_generation_high_water =
        std::max(du_retirement.generation_high_water, du_retirement.observed_generation_high_water);
    const bool high_water_regressed =
        report.rnti_retirement_capability_known && known_generation_high_water != 0 &&
        report.rnti_generation_high_water < known_generation_high_water && !ledger_reset_snapshot;
    if (stale_connection || high_water_regressed) {
      ntn_resource_repair repair;
      repair.action     = ntn_resource_repair_action::mark_resource_conflict;
      repair.du_index   = report.du_index;
      repair.cell_index = report.cell_index;
      repair.pci        = report.pci;
      repair.reason     = stale_connection ? "stale_du_connection_generation" : "rnti_generation_high_water_regressed";
      decision.repairs.push_back(std::move(repair));
      rnti_snapshot_valid = false;
    }
    for (const ntn_resource_audit_rnti_lease& du_lease : report.rnti_leases) {
      const bool supported_state =
          (du_lease.state == "pending" && du_lease.distribution_state == "applied_by_du") ||
          (du_lease.state == "consumed_by_mac" && du_lease.distribution_state == "applied_by_du") ||
          (du_lease.state == "expired" && du_lease.distribution_state == "expired_by_du");
      const bool duplicate_rnti = !observed_rntis.emplace(du_lease.rnti, &du_lease).second;
      const auto exact_local_lease =
          rnti_leases_by_key.find(rnti_key{report.du_index, report.cell_index, report.pci, du_lease.rnti});
      const bool known_rnti = exact_local_lease != rnti_leases_by_key.end();
      const bool generation_matches =
          !known_rnti ||
          (du_lease.generation_id != 0 && du_lease.generation_id == exact_local_lease->second.generation_id);
      const bool used_in_other_local_cell =
          !known_rnti && rntis_in_other_local_cells.find(du_lease.rnti) != rntis_in_other_local_cells.end();
      if (!rnti_snapshot_valid || duplicate_rnti || !is_crnti(du_lease.rnti) || du_lease.generation_id == 0 ||
          !supported_state || !generation_matches || used_in_other_local_cell) {
        ntn_resource_repair repair;
        repair.action     = ntn_resource_repair_action::mark_resource_conflict;
        repair.du_index   = report.du_index;
        repair.cell_index = report.cell_index;
        repair.pci        = report.pci;
        repair.rnti       = du_lease.rnti;
        repair.reason     = duplicate_rnti                ? "duplicate_du_rnti_snapshot"
                            : !is_crnti(du_lease.rnti)    ? "invalid_du_rnti_snapshot"
                            : du_lease.generation_id == 0 ? "invalid_du_rnti_snapshot_generation"
                            : !supported_state            ? "unsupported_du_rnti_snapshot_state"
                            : !generation_matches         ? "stale_du_rnti_snapshot_generation"
                            : used_in_other_local_cell    ? "du_rnti_cell_mismatch"
                                                          : "stale_du_connection_generation";
        if (decision.repairs.empty()) {
          decision.repairs.push_back(std::move(repair));
        }
        rnti_snapshot_valid = false;
        break;
      }
    }

    if (rnti_snapshot_valid) {
      if (du_retirement.du_connection_generation == 0) {
        du_retirement.du_connection_generation = report.du_connection_generation;
      }
      if (report.rnti_retirement_capability_known) {
        du_retirement.capability_known = true;
        du_retirement.supported        = report.rnti_retirement_supported;
        du_retirement.generation_high_water =
            std::max(du_retirement.generation_high_water, report.rnti_generation_high_water);
      }
      du_retirement.observed_connection_generation = report.du_connection_generation;
      du_retirement.observed_capability_known      = du_retirement.capability_known;
      du_retirement.observed_supported             = du_retirement.supported;
      du_retirement.observed_generation_high_water = du_retirement.generation_high_water;
      du_retirement.complete_audit_seen = true;
      const bool retirement_supported   = du_retirement.capability_known && du_retirement.supported;

      for (const auto& observed : observed_rntis) {
        const ntn_resource_audit_rnti_lease& du_lease = *observed.second;
        const rnti_key                       key{report.du_index, report.cell_index, report.pci, du_lease.rnti};
        if (rnti_leases_by_key.find(key) != rnti_leases_by_key.end()) {
          continue;
        }

        const bool can_retire = retirement_supported && du_retirement.generation_high_water >= du_lease.generation_id;
        ntn_rnti_lease orphan;
        orphan.du_index                 = report.du_index;
        orphan.cell_index               = report.cell_index;
        orphan.pci                      = report.pci;
        orphan.rnti                     = du_lease.rnti;
        orphan.generation_id            = du_lease.generation_id;
        orphan.du_connection_generation = report.du_connection_generation;
        orphan.state              = du_lease.state == "expired" && can_retire ? "retire_pending" : "orphan_quarantined";
        orphan.reason             = du_lease.state == "expired" && can_retire ? "expired_orphan_confirmed_by_du"
                                                                              : "unknown_du_rnti_quarantined";
        orphan.distribution_state = du_lease.distribution_state;
        orphan.distribution_reason = "du_audit_snapshot";
        rnti_leases_by_key.emplace(key, std::move(orphan));
        orphan_rnti_keys.emplace(key);
        ++nof_rnti_orphans_observed;
        rnti_retirement_last_reason = du_lease.state == "expired" && can_retire ? "expired_orphan_ready_for_retirement"
                                                                                : "unknown_du_rnti_quarantined";
      }

      // A retirement response may be lost immediately before a DU disconnect. After reconnect, an authoritative
      // complete snapshot can confirm removal only for the exact previously-sent batch. Partial disappearance keeps
      // every member isolated so one response can never release only part of an atomic retirement request.
      std::vector<retirement_group_key> retirement_batches_confirmed_absent;
      std::map<rnti_key, uint32_t>      partially_absent_retirement_generations;
      for (const auto& attempted_batch : retirement_attempted_batches) {
        const retirement_group_key& group                = attempted_batch.first;
        const du_index_t            attempted_du         = std::get<0>(group);
        const auto                  attempted_cell       = std::get<1>(group);
        const pci_t                 attempted_pci        = std::get<2>(group);
        const uint32_t              attempted_generation = std::get<3>(group);
        const uint64_t              attempted_connection = std::get<4>(group);
        const bool same_live_connection = attempted_connection == report.du_connection_generation &&
                                          du_retirement.du_connection_generation == report.du_connection_generation;
        const bool recovered_connection =
            du_retirement.invalidated_connection_generation != 0 &&
            attempted_connection <= du_retirement.invalidated_connection_generation &&
            report.du_connection_generation > du_retirement.invalidated_connection_generation;
        if (attempted_du != report.du_index || attempted_cell != report.cell_index || attempted_pci != report.pci ||
            attempted_connection == 0 || (!same_live_connection && !recovered_connection) || ledger_reset_snapshot) {
          continue;
        }

        bool any_absent = false;
        bool all_absent = true;
        bool all_safe   = retirement_supported && du_retirement.generation_high_water >= attempted_generation;
        for (const rnti_key& key : attempted_batch.second) {
          const auto lease_it = rnti_leases_by_key.find(key);
          if (lease_it == rnti_leases_by_key.end() || lease_it->second.generation_id != attempted_generation ||
              retirement_attempted_rnti_keys.count(key) == 0) {
            any_absent = true;
            all_safe   = false;
            continue;
          }

          const ntn_rnti_lease& lease  = lease_it->second;
          const bool            absent = observed_rntis.count(lease.rnti) == 0;
          const bool            cu_owned =
              lease.ue_index != ue_index_t::invalid ||
              std::any_of(access_owner_by_rnti.begin(), access_owner_by_rnti.end(), [&](const auto& owner) {
                return std::get<0>(owner.first) == lease.du_index && std::get<3>(owner.first) == lease.rnti;
              });
          any_absent                          = any_absent || absent;
          all_absent                          = all_absent && absent;
          const bool lease_connection_matches = same_live_connection
                                                    ? lease.du_connection_generation == attempted_connection
                                                    : lease.du_connection_generation == 0;
          all_safe = all_safe && !cu_owned && lease_connection_matches &&
                     (lease.state == "retire_waiting_audit" || lease.state == "orphan_quarantined");
        }

        if (all_absent && all_safe) {
          retirement_batches_confirmed_absent.push_back(group);
        } else if (any_absent) {
          for (const rnti_key& key : attempted_batch.second) {
            partially_absent_retirement_generations.emplace(key, attempted_generation);
          }
        }
      }
      for (const retirement_group_key& group : retirement_batches_confirmed_absent) {
        const auto attempted_batch = retirement_attempted_batches.find(group);
        for (const rnti_key& key : attempted_batch->second) {
          const ntn_rnti_lease& lease              = rnti_leases_by_key.at(key);
          auto&                 retired_generation = retired_generation_by_du_rnti[{lease.du_index, lease.rnti}];
          retired_generation                       = std::max(retired_generation, lease.generation_id);
          orphan_rnti_keys.erase(key);
          retirement_attempted_rnti_keys.erase(key);
          rnti_leases_by_key.erase(key);
          ++nof_rnti_leases_retired;
        }
        retirement_attempted_batches.erase(attempted_batch);
        rnti_retirement_last_reason = "retirement_confirmed_absent_by_complete_audit";
      }
      if (!partially_absent_retirement_generations.empty()) {
        rnti_retirement_last_reason = "partial_retirement_visibility_in_complete_audit";
      }
      std::map<std::pair<std::string, uint32_t>, ntn_resource_repair> rnti_repairs_by_analog_generation;
      for (auto& entry : rnti_leases_by_key) {
        ntn_rnti_lease& lease = entry.second;
        if (lease.du_index != report.du_index || lease.cell_index != report.cell_index || lease.pci != report.pci) {
          continue;
        }

        const auto partial_attempt = partially_absent_retirement_generations.find(entry.first);
        if (partial_attempt != partially_absent_retirement_generations.end() &&
            lease.generation_id == partial_attempt->second && retirement_attempted_rnti_keys.count(entry.first) != 0 &&
            (lease.state == "retire_waiting_audit" || lease.state == "orphan_quarantined")) {
          lease.state  = "retire_waiting_audit";
          lease.reason = "partial_retirement_visibility_in_complete_audit";
          continue;
        }

        const auto du_lease_it = observed_rntis.find(lease.rnti);
        const auto* du_lease   = du_lease_it != observed_rntis.end() ? du_lease_it->second : nullptr;
        if (du_lease != nullptr) {
          lease.du_connection_generation = report.du_connection_generation;
          const bool orphan              = orphan_rnti_keys.count(entry.first) != 0;
          const bool can_retire = retirement_supported && du_retirement.generation_high_water >= lease.generation_id;
          if (du_lease->state == "expired") {
            lease.distribution_state  = "expired_by_du";
            lease.distribution_reason = "du_audit_snapshot";
          }
          if (du_lease->state == "pending" && lease.state == "reserved" &&
              (lease.distribution_state == "sent_to_du" || lease.distribution_state == "applied_by_du")) {
            lease.distribution_state  = "applied_by_du";
            lease.distribution_reason = "du_audit_snapshot";
          } else if (du_lease->state == "consumed_by_mac" &&
                     (lease.state == "reserved" || lease.state == "offered_in_rar" ||
                      lease.state == "consumed_by_du")) {
            lease.state               = "consumed_by_du";
            lease.reason              = "du_audit_consumed_by_mac";
            lease.distribution_state  = "applied_by_du";
            lease.distribution_reason = "du_audit_snapshot";
          } else if (du_lease->state == "expired" && lease.state == "reserved") {
            lease.state               = "expired";
            lease.reason              = "du_lease_expired";
          }

          const bool locally_terminal =
              lease.state == "released" || lease.state == "expired" || lease.state == "retire_waiting_audit";
          if (du_lease->state == "expired" && can_retire &&
              (locally_terminal || (orphan && lease.state == "orphan_quarantined"))) {
            lease.state               = "retire_pending";
            lease.reason              = orphan ? "expired_orphan_confirmed_by_du" : "terminal_lease_confirmed_by_du";
            lease.distribution_state  = "expired_by_du";
            lease.distribution_reason = "retirement_audit";
          } else if (orphan && lease.state != "retire_sent" && lease.state != "retire_pending") {
            const bool retirement_was_attempted = retirement_attempted_rnti_keys.count(entry.first) != 0;
            lease.state = retirement_was_attempted ? "retire_waiting_audit" : "orphan_quarantined";
            lease.reason =
                retirement_was_attempted ? "previous_retirement_requires_audit" : "unknown_du_rnti_quarantined";
          } else if (du_lease->state != "expired" &&
                     (lease.state == "retire_pending" || lease.state == "retire_waiting_audit")) {
            lease.state  = "retire_waiting_audit";
            lease.reason = "du_no_longer_reports_expired";
          }
        }

        // Only an unused lease is expected to remain in the MAC pending pool. Once consumed, observed on Initial UL,
        // committed or expired, it must never be reinserted by the audit repair loop.
        const bool ack_unknown = lease.distribution_state == "sent_to_du" &&
                                 lease.distribution_reason.rfind("ack_unknown", 0) == 0;
        if (orphan_rnti_keys.count(entry.first) != 0 || lease.state != "reserved" ||
            (lease.distribution_state != "applied_by_du" && !ack_unknown)) {
          continue;
        }
        const bool du_has_pending_lease = du_lease != nullptr && du_lease->state == "pending" &&
                                          du_lease->distribution_state == "applied_by_du";
        if (du_has_pending_lease) {
          continue;
        }

        ntn_resource_repair& repair =
            rnti_repairs_by_analog_generation[{lease.analog_beam_id, lease.generation_id}];
        repair.action                   = ntn_resource_repair_action::resend_rnti_lease_pool;
        repair.du_index                 = lease.du_index;
        repair.cell_index               = lease.cell_index;
        repair.pci                      = lease.pci;
        repair.analog_beam_id           = lease.analog_beam_id;
        repair.reason                   = ack_unknown ? "du_missing_ack_unknown_rnti_pool" :
                                                        "du_missing_applied_rnti_pool";
        repair.rnti_lease_generation_id = lease.generation_id;
        repair.rnti_leases.push_back(lease.rnti);
      }
      for (auto& repair : rnti_repairs_by_analog_generation) {
        decision.repairs.push_back(std::move(repair.second));
      }
    }
  }

  if (!report.ue_slot_snapshot_complete) {
    decision.nof_mismatches = decision.repairs.size();
    decision.nof_repairs    = decision.repairs.size();
    resolve_recovered_audit_rejection();
    return decision;
  }

  const auto du_has_matching_slot = [&report](ue_index_t                                      ue_index,
                                              const f1ap_ntn_ul_slot_resource_request& request) {
    return std::find_if(report.ue_slots.begin(), report.ue_slots.end(), [ue_index, &request](const auto& slot) {
             return slot.ue_index == ue_index && slot.state == "applied_by_du" &&
                    ntn_beam_service_resource_manager::are_slot_requests_equal(slot.request, request);
           }) != report.ue_slots.end();
  };

  for (const auto& entry : digital_slot_intent_by_ue) {
    const ntn_digital_slot_resource_intent& intent = entry.second;
    if (is_empty(intent.request) ||
        (intent.state != "active" && intent.state != "sent_to_du" && intent.state != "applied_by_du" &&
         intent.state != "rollback_restored")) {
      continue;
    }
    const bool has_service_pair =
        !intent.uplink_resource_beam_id.empty() && intent.uplink_resource_du_index != du_index_t::invalid &&
        intent.has_uplink_resource_nci;
    const du_index_t audit_du_index = has_service_pair ? intent.uplink_resource_du_index : intent.service_du_index;
    const srsran::du_cell_index_t audit_cell_index =
        has_service_pair ? intent.uplink_resource_cell_index : intent.service_cell_index;
    const pci_t audit_pci = has_service_pair ? intent.uplink_resource_pci : intent.service_pci;

    if (audit_du_index != du_index_t::invalid && audit_du_index != report.du_index) {
      continue;
    }
    if (audit_cell_index != srsran::INVALID_DU_CELL_INDEX && audit_cell_index != report.cell_index) {
      continue;
    }
    if (audit_pci != INVALID_PCI && audit_pci != report.pci) {
      continue;
    }
    if (du_has_matching_slot(intent.ue_index, intent.request)) {
      continue;
    }

    ntn_resource_repair repair;
    repair.action       = ntn_resource_repair_action::apply_sr_srs_assignment;
    repair.ue_index     = intent.ue_index;
    repair.du_index     = report.du_index;
    repair.cell_index   = report.cell_index;
    repair.pci          = report.pci;
    repair.reason       = has_service_pair ? "du_missing_service_pair_ul_sr_srs_assignment" :
                                             "du_missing_sr_srs_assignment";
    repair.service_beam_id = intent.digital_beam_id;
    if (has_service_pair) {
      repair.uplink_resource_beam_id  = intent.uplink_resource_beam_id;
      repair.uplink_resource_du_index = intent.uplink_resource_du_index;
      repair.uplink_resource_nci      = intent.uplink_resource_nci;
      repair.has_uplink_resource_nci  = intent.has_uplink_resource_nci;
    }
    repair.slot_request = intent.request;
    decision.repairs.push_back(std::move(repair));
  }

  for (const ntn_resource_audit_ue_slot& du_slot : report.ue_slots) {
    const auto local_it = digital_slot_intent_by_ue.find(du_slot.ue_index);
    if (local_it != digital_slot_intent_by_ue.end() &&
        (local_it->second.state == "sent_to_du" || local_it->second.state == "applied_by_du" ||
         local_it->second.state == "rollback_restored") &&
        are_slot_requests_equal(local_it->second.request, du_slot.request)) {
      continue;
    }

    ntn_resource_repair repair;
    repair.action       = ntn_resource_repair_action::clear_unknown_sr_srs_assignment;
    repair.ue_index     = du_slot.ue_index;
    repair.du_index     = report.du_index;
    repair.cell_index   = report.cell_index;
    repair.pci          = report.pci;
    repair.reason       = "du_unknown_sr_srs_assignment";
    repair.slot_request = f1ap_ntn_ul_slot_resource_request{};
    repair.uplink_resource_du_index = report.du_index;
    if (local_it != digital_slot_intent_by_ue.end()) {
      repair.service_beam_id = local_it->second.digital_beam_id;
      if (!local_it->second.uplink_resource_beam_id.empty()) {
        repair.uplink_resource_beam_id  = local_it->second.uplink_resource_beam_id;
        repair.uplink_resource_du_index = local_it->second.uplink_resource_du_index;
        repair.uplink_resource_nci      = local_it->second.uplink_resource_nci;
        repair.has_uplink_resource_nci  = local_it->second.has_uplink_resource_nci;
      }
    }
    decision.repairs.push_back(std::move(repair));
  }

  decision.nof_mismatches = decision.repairs.size();
  decision.nof_repairs    = decision.repairs.size();
  resolve_recovered_audit_rejection();
  return decision;
}

std::vector<ntn_rnti_retirement_batch> ntn_beam_service_resource_manager::get_pending_rnti_retirement_batches() const
{
  std::map<retirement_group_key, ntn_rnti_retirement_batch> batches_by_group;
  for (const auto& entry : rnti_leases_by_key) {
    const ntn_rnti_lease& lease = entry.second;
    if (lease.state != "retire_pending") {
      continue;
    }
    const retirement_group_key group{
        lease.du_index, lease.cell_index, lease.pci, lease.generation_id, lease.du_connection_generation};
    auto& batch                    = batches_by_group[group];
    batch.du_index                 = lease.du_index;
    batch.cell_index               = lease.cell_index;
    batch.pci                      = lease.pci;
    batch.generation_id            = lease.generation_id;
    batch.du_connection_generation = lease.du_connection_generation;
    batch.leases.push_back(lease.rnti);
  }

  std::vector<ntn_rnti_retirement_batch> batches;
  batches.reserve(batches_by_group.size());
  for (auto& entry : batches_by_group) {
    batches.push_back(std::move(entry.second));
  }
  return batches;
}

bool ntn_beam_service_resource_manager::mark_rnti_retirement_sent(const ntn_rnti_retirement_batch& batch)
{
  if (batch.du_index == du_index_t::invalid || batch.cell_index == srsran::INVALID_DU_CELL_INDEX ||
      batch.pci == INVALID_PCI || batch.generation_id == 0 || batch.leases.empty()) {
    return false;
  }
  const auto du_state = rnti_retirement_by_du.find(batch.du_index);
  if (du_state == rnti_retirement_by_du.end() || !du_state->second.capability_known || !du_state->second.supported ||
      !du_state->second.complete_audit_seen ||
      du_state->second.du_connection_generation != batch.du_connection_generation ||
      du_state->second.generation_high_water < batch.generation_id) {
    return false;
  }

  std::vector<rnti_t> requested = batch.leases;
  std::sort(requested.begin(), requested.end());
  if (std::adjacent_find(requested.begin(), requested.end()) != requested.end() ||
      !std::all_of(requested.begin(), requested.end(), is_crnti)) {
    return false;
  }
  std::vector<rnti_t> pending;
  for (const auto& entry : rnti_leases_by_key) {
    const ntn_rnti_lease& lease = entry.second;
    if (lease.du_index == batch.du_index && lease.cell_index == batch.cell_index && lease.pci == batch.pci &&
        lease.generation_id == batch.generation_id &&
        lease.du_connection_generation == batch.du_connection_generation && lease.state == "retire_pending") {
      pending.push_back(lease.rnti);
    }
  }
  if (requested != pending) {
    return false;
  }

  std::set<rnti_key> requested_keys;
  for (rnti_t rnti : requested) {
    requested_keys.emplace(batch.du_index, batch.cell_index, batch.pci, rnti);
  }
  std::vector<retirement_group_key> previous_attempts;
  for (const auto& attempted_batch : retirement_attempted_batches) {
    const bool intersects = std::any_of(requested_keys.begin(), requested_keys.end(), [&](const rnti_key& key) {
      return attempted_batch.second.count(key) != 0;
    });
    if (!intersects) {
      continue;
    }
    if (attempted_batch.second != requested_keys) {
      return false;
    }
    previous_attempts.push_back(attempted_batch.first);
  }
  for (const retirement_group_key& group : previous_attempts) {
    retirement_attempted_batches.erase(group);
  }

  for (rnti_t rnti : requested) {
    const rnti_key key{batch.du_index, batch.cell_index, batch.pci, rnti};
    auto           lease = rnti_leases_by_key.find(key);
    lease->second.state  = "retire_sent";
    lease->second.reason = "retirement_sent_to_du";
    retirement_attempted_rnti_keys.emplace(key);
  }
  const retirement_group_key group{
      batch.du_index, batch.cell_index, batch.pci, batch.generation_id, batch.du_connection_generation};
  retirement_attempted_batches.emplace(group, std::move(requested_keys));
  rnti_retirement_last_reason = "retirement_sent_to_du";
  return true;
}

bool ntn_beam_service_resource_manager::mark_rnti_retirement_result(const ntn_rnti_retirement_batch& batch,
                                                                    ntn_rnti_retirement_outcome      outcome,
                                                                    std::string                      reason)
{
  if (batch.du_index == du_index_t::invalid || batch.cell_index == srsran::INVALID_DU_CELL_INDEX ||
      batch.pci == INVALID_PCI || batch.generation_id == 0 || batch.leases.empty()) {
    return false;
  }
  const auto du_state = rnti_retirement_by_du.find(batch.du_index);
  if (du_state == rnti_retirement_by_du.end() || !du_state->second.capability_known || !du_state->second.supported ||
      !du_state->second.complete_audit_seen ||
      du_state->second.du_connection_generation != batch.du_connection_generation ||
      du_state->second.generation_high_water < batch.generation_id) {
    return false;
  }

  std::vector<rnti_t> requested = batch.leases;
  std::sort(requested.begin(), requested.end());
  if (std::adjacent_find(requested.begin(), requested.end()) != requested.end()) {
    return false;
  }
  std::vector<rnti_t> sent;
  for (const auto& entry : rnti_leases_by_key) {
    const ntn_rnti_lease& lease = entry.second;
    if (lease.du_index == batch.du_index && lease.cell_index == batch.cell_index && lease.pci == batch.pci &&
        lease.generation_id == batch.generation_id &&
        lease.du_connection_generation == batch.du_connection_generation && lease.state == "retire_sent") {
      sent.push_back(lease.rnti);
    }
  }
  if (requested != sent) {
    return false;
  }

  const retirement_group_key group{
      batch.du_index, batch.cell_index, batch.pci, batch.generation_id, batch.du_connection_generation};
  const auto attempted_batch = retirement_attempted_batches.find(group);
  if (attempted_batch == retirement_attempted_batches.end()) {
    return false;
  }
  const std::set<rnti_key> attempted_keys                   = attempted_batch->second;
  const auto               clear_attempts_for_exact_members = [&]() {
    std::vector<retirement_group_key> attempts_to_clear;
    for (const auto& attempt : retirement_attempted_batches) {
      if (attempt.second == attempted_keys) {
        attempts_to_clear.push_back(attempt.first);
      }
    }
    for (const retirement_group_key& attempt : attempts_to_clear) {
      retirement_attempted_batches.erase(attempt);
    }
  };

  if (outcome == ntn_rnti_retirement_outcome::accepted) {
    for (rnti_t rnti : requested) {
      const rnti_key key{batch.du_index, batch.cell_index, batch.pci, rnti};
      const auto     owner = access_owner_by_rnti.find(key);
      const auto     lease = rnti_leases_by_key.find(key);
      if (owner != access_owner_by_rnti.end() || lease == rnti_leases_by_key.end() ||
          lease->second.ue_index != ue_index_t::invalid) {
        return false;
      }
    }
    for (rnti_t rnti : requested) {
      const rnti_key key{batch.du_index, batch.cell_index, batch.pci, rnti};
      auto&          retired_generation = retired_generation_by_du_rnti[{batch.du_index, rnti}];
      retired_generation                = std::max(retired_generation, batch.generation_id);
      orphan_rnti_keys.erase(key);
      retirement_attempted_rnti_keys.erase(key);
      rnti_leases_by_key.erase(key);
      ++nof_rnti_leases_retired;
    }
    clear_attempts_for_exact_members();
    rnti_retirement_last_reason = reason.empty() ? "retirement_confirmed_by_du" : std::move(reason);
    return true;
  }

  const bool retain_attempt = outcome == ntn_rnti_retirement_outcome::outcome_unknown;
  if (reason.empty()) {
    switch (outcome) {
      case ntn_rnti_retirement_outcome::definitively_rejected:
        reason = "retirement_definitively_rejected";
        break;
      case ntn_rnti_retirement_outcome::outcome_unknown:
        reason = "retirement_result_unknown";
        break;
      case ntn_rnti_retirement_outcome::not_sent:
        reason = "retirement_not_sent";
        break;
      case ntn_rnti_retirement_outcome::accepted:
        break;
    }
  }
  for (rnti_t rnti : requested) {
    const rnti_key key{batch.du_index, batch.cell_index, batch.pci, rnti};
    auto           lease = rnti_leases_by_key.find(key);
    lease->second.state  = outcome == ntn_rnti_retirement_outcome::not_sent ? "retire_pending" : "retire_waiting_audit";
    lease->second.reason = reason;
    if (!retain_attempt) {
      retirement_attempted_rnti_keys.erase(key);
    }
    if (outcome == ntn_rnti_retirement_outcome::definitively_rejected) {
      ++nof_rnti_retirement_rejected;
    }
  }
  if (!retain_attempt) {
    clear_attempts_for_exact_members();
  }
  rnti_retirement_last_reason = std::move(reason);
  return true;
}

void ntn_beam_service_resource_manager::invalidate_rnti_retirement_for_du(du_index_t du_index)
{
  auto& du_state = rnti_retirement_by_du[du_index];
  du_state.invalidated_connection_generation =
      std::max({du_state.invalidated_connection_generation,
                du_state.du_connection_generation,
                du_state.observed_connection_generation});
  du_state.du_connection_generation          = 0;
  du_state.observed_connection_generation    = 0;
  du_state.capability_known                  = false;
  du_state.supported                         = false;
  du_state.observed_capability_known         = false;
  du_state.observed_supported                = false;
  du_state.complete_audit_seen               = false;
  du_state.observed_generation_high_water = 0;
  for (auto& entry : rnti_leases_by_key) {
    ntn_rnti_lease& lease = entry.second;
    if (lease.du_index != du_index) {
      continue;
    }
    if (lease.distribution_state == "sent_to_du" || lease.distribution_state == "applied_by_du") {
      lease.distribution_state       = "sent_to_du";
      lease.distribution_reason      = "ack_unknown:du_disconnected";
      lease.du_connection_generation = 0;
    }
    if (lease.state != "retire_pending" && lease.state != "retire_sent" && lease.state != "retire_waiting_audit") {
      continue;
    }
    lease.du_connection_generation = 0;
    if (orphan_rnti_keys.count(entry.first) != 0) {
      const bool retirement_was_sent = retirement_attempted_rnti_keys.count(entry.first) != 0;
      lease.state                    = retirement_was_sent ? "retire_waiting_audit" : "orphan_quarantined";
      lease.reason                   = retirement_was_sent ? "du_disconnected_orphan_retire_requires_audit"
                                                           : "du_disconnected_orphan_requires_audit";
    } else {
      lease.state  = "retire_waiting_audit";
      lease.reason = "du_disconnected_retirement_requires_audit";
    }
  }
  for (auto& entry : resource_repairs_by_key) {
    ntn_resource_repair_record& repair = entry.second;
    if (repair.du_index != du_index || repair.action != ntn_resource_repair_action::resend_rnti_lease_pool ||
        (repair.state != "queued" && repair.state != "sent")) {
      continue;
    }
    repair.state       = "invalidated";
    repair.reason      = "du_disconnected_repair_invalidated";
    repair.retry_count = 0;
  }
  rnti_retirement_last_reason = "du_disconnected_retirement_invalidated";
}

std::set<rnti_t> ntn_beam_service_resource_manager::get_rnti_allocation_exclusions(du_index_t du_index) const
{
  std::set<rnti_t> exclusions;
  for (const auto& entry : rnti_leases_by_key) {
    if (entry.second.du_index == du_index) {
      exclusions.emplace(entry.second.rnti);
    }
  }
  for (const auto& entry : access_owner_by_rnti) {
    if (std::get<0>(entry.first) == du_index) {
      exclusions.emplace(std::get<3>(entry.first));
    }
  }
  return exclusions;
}

bool ntn_beam_service_resource_manager::is_rnti_excluded_for_du(du_index_t du_index, rnti_t rnti) const
{
  return std::any_of(
             rnti_leases_by_key.begin(),
             rnti_leases_by_key.end(),
             [&](const auto& entry) { return entry.second.du_index == du_index && entry.second.rnti == rnti; }) ||
         std::any_of(access_owner_by_rnti.begin(), access_owner_by_rnti.end(), [&](const auto& entry) {
           return std::get<0>(entry.first) == du_index && std::get<3>(entry.first) == rnti;
         });
}

ntn_resource_repair_record ntn_beam_service_resource_manager::queue_resource_repair(const ntn_resource_repair& repair,
                                                                                    uint32_t generation_id)
{
  static constexpr unsigned max_repair_retries = 1;

  if (repair.action == ntn_resource_repair_action::none) {
    return {};
  }

  const repair_key key = make_repair_key(repair);
  auto             it  = resource_repairs_by_key.find(key);
  if (it == resource_repairs_by_key.end()) {
    ntn_resource_repair_record record = make_repair_record(repair, generation_id);
    if (repair.action == ntn_resource_repair_action::mark_resource_conflict) {
      record.state = "blocked_conflict";
    }
    it = resource_repairs_by_key.emplace(key, std::move(record)).first;
    return it->second;
  }

  ntn_resource_repair_record& record = it->second;
  record.generation_id = generation_id;
  record.reason        = repair.reason;

  if (record.state == "blocked_conflict" || record.state == "retry_exhausted" || record.state == "queued" ||
      record.state == "sent") {
    return record;
  }

  if (repair.action == ntn_resource_repair_action::mark_resource_conflict) {
    record.state = "blocked_conflict";
    return record;
  }

  if (record.state == "failed") {
    if (record.retry_count >= max_repair_retries) {
      record.state = "retry_exhausted";
      return record;
    }
    ++record.retry_count;
  } else {
    record.retry_count = 0;
  }
  record.state = "queued";
  return record;
}

void ntn_beam_service_resource_manager::mark_resource_repair_sent(const ntn_resource_repair& repair)
{
  auto repair_it = resource_repairs_by_key.find(make_repair_key(repair));
  if (repair_it == resource_repairs_by_key.end() || repair_it->second.state != "queued") {
    return;
  }
  repair_it->second.state  = "sent";
  repair_it->second.reason = repair.reason.empty() ? "sent" : repair.reason;
}

void ntn_beam_service_resource_manager::mark_resource_repair_result(const ntn_resource_repair& repair,
                                                                    bool                       accepted,
                                                                    std::string                reason)
{
  static constexpr unsigned max_repair_retries = 1;

  auto repair_it = resource_repairs_by_key.find(make_repair_key(repair));
  if (repair_it == resource_repairs_by_key.end()) {
    return;
  }

  ntn_resource_repair_record& record = repair_it->second;
  record.reason = std::move(reason);
  if (accepted) {
    record.state = "applied";
    return;
  }

  record.state = record.retry_count >= max_repair_retries ? "retry_exhausted" : "failed";
}

bool ntn_beam_service_resource_manager::has_blocking_resource_repair() const
{
  return std::any_of(resource_repairs_by_key.begin(), resource_repairs_by_key.end(), [](const auto& entry) {
    return entry.second.state == "retry_exhausted" || entry.second.state == "blocked_conflict";
  });
}

void ntn_beam_service_resource_manager::remove_ue(ue_index_t ue_index)
{
  const auto key_it = access_rnti_key_by_ue.find(ue_index);
  if (key_it != access_rnti_key_by_ue.end()) {
    const auto owner_it = access_owner_by_rnti.find(key_it->second);
    if (owner_it != access_owner_by_rnti.end() && owner_it->second == ue_index) {
      access_owner_by_rnti.erase(owner_it);
    }
    access_rnti_key_by_ue.erase(key_it);
  }
  access_ownership_by_ue.erase(ue_index);
  for (auto& entry : rnti_leases_by_key) {
    if (entry.second.ue_index == ue_index &&
        (entry.second.state == "initial_ul_seen" || entry.second.state == "committed" ||
         entry.second.state == "consumed_by_du" || entry.second.state == "offered_in_rar")) {
      entry.second.state    = "released";
      entry.second.reason   = "ue_removed";
      entry.second.ue_index = ue_index_t::invalid;
    }
  }
  digital_slot_intent_by_ue.erase(ue_index);
  cached_slot_requests_by_ue.erase(ue_index);
  applied_slot_requests_by_ue.erase(ue_index);
}

void ntn_beam_service_resource_manager::remove_missing_ues(const std::set<ue_index_t>& live_ues)
{
  std::vector<ue_index_t> stale_ues;
  for (const auto& entry : access_ownership_by_ue) {
    if (live_ues.find(entry.first) == live_ues.end()) {
      stale_ues.push_back(entry.first);
    }
  }
  for (const auto& entry : digital_slot_intent_by_ue) {
    if (live_ues.find(entry.first) == live_ues.end()) {
      stale_ues.push_back(entry.first);
    }
  }
  for (const auto& entry : cached_slot_requests_by_ue) {
    if (live_ues.find(entry.first) == live_ues.end()) {
      stale_ues.push_back(entry.first);
    }
  }
  for (const auto& entry : applied_slot_requests_by_ue) {
    if (live_ues.find(entry.first) == live_ues.end()) {
      stale_ues.push_back(entry.first);
    }
  }
  std::sort(stale_ues.begin(), stale_ues.end());
  stale_ues.erase(std::unique(stale_ues.begin(), stale_ues.end()), stale_ues.end());
  for (ue_index_t ue_index : stale_ues) {
    remove_ue(ue_index);
  }
}

ntn_beam_service_resource_snapshot ntn_beam_service_resource_manager::get_snapshot() const
{
  ntn_beam_service_resource_snapshot snapshot;
  snapshot.rnti_leases.reserve(rnti_leases_by_key.size());
  for (const auto& entry : rnti_leases_by_key) {
    snapshot.rnti_leases.push_back(entry.second);
    if (orphan_rnti_keys.count(entry.first) != 0) {
      ++snapshot.nof_rnti_orphans_quarantined;
    }
    if (entry.second.state == "reserved") {
      ++snapshot.nof_rnti_leases_reserved;
      if (entry.second.distribution_state == "applied_by_du") {
        ++snapshot.nof_rnti_leases_available;
      }
    } else if (entry.second.state == "offered_in_rar") {
      ++snapshot.nof_rnti_leases_offered_in_rar;
    } else if (entry.second.state == "consumed_by_du") {
      ++snapshot.nof_rnti_leases_consumed_by_du;
    } else if (entry.second.state == "initial_ul_seen") {
      ++snapshot.nof_rnti_leases_initial_ul_seen;
    } else if (entry.second.state == "committed") {
      ++snapshot.nof_rnti_leases_committed;
    } else if (entry.second.state == "released") {
      ++snapshot.nof_rnti_leases_released;
    } else if (entry.second.state == "expired") {
      ++snapshot.nof_rnti_leases_expired;
    } else if (entry.second.state == "conflict") {
      ++snapshot.nof_rnti_leases_conflict;
    } else if (entry.second.state == "retire_pending") {
      ++snapshot.nof_rnti_leases_retire_pending;
    } else if (entry.second.state == "retire_sent") {
      ++snapshot.nof_rnti_leases_retire_sent;
    } else if (entry.second.state == "retire_waiting_audit") {
      ++snapshot.nof_rnti_leases_retire_waiting_audit;
    }
    if (entry.second.distribution_state == "sent_to_du") {
      ++snapshot.nof_rnti_leases_sent_to_du;
    } else if (entry.second.distribution_state == "applied_by_du") {
      ++snapshot.nof_rnti_leases_applied_by_du;
    } else if (entry.second.distribution_state == "rejected_by_du") {
      ++snapshot.nof_rnti_leases_rejected_by_du;
    }
  }

  snapshot.nof_rnti_leases_retired      = nof_rnti_leases_retired;
  snapshot.nof_rnti_leases_reused       = nof_rnti_leases_reused;
  snapshot.nof_rnti_retirement_rejected = nof_rnti_retirement_rejected;
  snapshot.nof_rnti_orphans_observed    = nof_rnti_orphans_observed;
  snapshot.rnti_retirement_last_reason  = rnti_retirement_last_reason;
  snapshot.rnti_retirement_du_statuses.reserve(rnti_retirement_by_du.size());
  for (const auto& entry : rnti_retirement_by_du) {
    ntn_rnti_retirement_du_status status;
    status.du_index = entry.first;
    status.du_connection_generation = entry.second.complete_audit_seen
                                          ? entry.second.du_connection_generation
                                          : entry.second.observed_connection_generation;
    status.capability_known = entry.second.complete_audit_seen ? entry.second.capability_known
                                                               : entry.second.observed_capability_known;
    status.supported = entry.second.complete_audit_seen ? entry.second.supported : entry.second.observed_supported;
    status.complete_audit_seen = entry.second.complete_audit_seen;
    status.generation_high_water = entry.second.complete_audit_seen ? entry.second.generation_high_water
                                                                    : entry.second.observed_generation_high_water;
    snapshot.rnti_retirement_du_statuses.push_back(status);
  }

  snapshot.access_rnti_ownerships.reserve(access_ownership_by_ue.size());
  for (const auto& entry : access_ownership_by_ue) {
    snapshot.access_rnti_ownerships.push_back(entry.second);
    if (entry.second.state == "observed" || entry.second.state == "initial_ul_seen") {
      ++snapshot.nof_access_rnti_owned;
    } else if (entry.second.state == "conflict") {
      ++snapshot.nof_access_rnti_conflicts;
    }
  }

  snapshot.digital_slot_intents.reserve(digital_slot_intent_by_ue.size());
  for (const auto& entry : digital_slot_intent_by_ue) {
    snapshot.digital_slot_intents.push_back(entry.second);
    if (!entry.second.uplink_resource_beam_id.empty()) {
      ++snapshot.nof_service_pair_digital_slot_intents;
    }
    if (entry.second.state == "active" && !is_empty(entry.second.request)) {
      ++snapshot.nof_digital_slot_desired;
      ++snapshot.nof_digital_slot_active;
    } else if (entry.second.state == "sent_to_du") {
      ++snapshot.nof_digital_slot_sent_to_du;
      ++snapshot.nof_digital_slot_active;
    } else if (entry.second.state == "applied_by_du") {
      ++snapshot.nof_digital_slot_applied_by_du;
      ++snapshot.nof_digital_slot_active;
    } else if (entry.second.state == "rejected_by_du") {
      ++snapshot.nof_digital_slot_rejected_by_du;
    } else if (entry.second.state == "clear_sent") {
      ++snapshot.nof_digital_slot_clear_sent;
    } else if (entry.second.state == "cleared" || entry.second.state == "cleared_by_du") {
      if (entry.second.state == "cleared_by_du") {
        ++snapshot.nof_digital_slot_cleared_by_du;
      }
      ++snapshot.nof_digital_slot_cleared;
    } else if (entry.second.state == "rollback_restored") {
      ++snapshot.nof_digital_slot_rollback;
      ++snapshot.nof_digital_slot_active;
    }
  }

  snapshot.resource_repairs.reserve(resource_repairs_by_key.size());
  for (const auto& entry : resource_repairs_by_key) {
    snapshot.resource_repairs.push_back(entry.second);
    if (!entry.second.uplink_resource_beam_id.empty()) {
      ++snapshot.nof_service_pair_resource_repairs;
    }
    if (entry.second.state == "queued") {
      ++snapshot.nof_resource_repairs_queued;
    } else if (entry.second.state == "sent") {
      ++snapshot.nof_resource_repairs_sent;
    } else if (entry.second.state == "applied") {
      ++snapshot.nof_resource_repairs_applied;
    } else if (entry.second.state == "failed") {
      ++snapshot.nof_resource_repairs_failed;
    } else if (entry.second.state == "retry_exhausted") {
      ++snapshot.nof_resource_repairs_retry_exhausted;
    } else if (entry.second.state == "blocked_conflict") {
      ++snapshot.nof_resource_repairs_blocked_conflict;
    }
  }

  return snapshot;
}
