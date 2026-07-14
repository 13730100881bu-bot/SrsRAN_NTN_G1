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

bool ntn_beam_service_resource_manager::is_valid_access_ownership_update(
    const ntn_access_rnti_ownership_update& update)
{
  return update.ue_index != ue_index_t::invalid && update.du_index != du_index_t::invalid &&
         update.pci != INVALID_PCI && update.rnti != rnti_t::INVALID_RNTI;
}

bool ntn_beam_service_resource_manager::is_valid_lease_pool_update(const ntn_rnti_lease_pool_update& update)
{
  return update.du_index != du_index_t::invalid && update.pci != INVALID_PCI && !update.analog_beam_id.empty() &&
         !update.leases.empty();
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
  return {repair.action, repair.ue_index, repair.du_index, repair.cell_index, repair.pci, repair.rnti, repair.analog_beam_id};
}

ntn_resource_repair_record
ntn_beam_service_resource_manager::make_repair_record(const ntn_resource_repair& repair, uint32_t generation_id)
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

ntn_rnti_lease_pool_update_result ntn_beam_service_resource_manager::reserve_rnti_leases(
    const ntn_rnti_lease_pool_update& update)
{
  if (!is_valid_lease_pool_update(update)) {
    return {};
  }

  ntn_rnti_lease_pool_update_result result;
  for (rnti_t rnti : update.leases) {
    if (!is_crnti(rnti)) {
      result.accepted = false;
      result.state    = "invalid";
      result.reason   = "invalid_rnti";
      return result;
    }
    const rnti_key key{update.du_index, update.pci, rnti};
    if (rnti_leases_by_key.find(key) != rnti_leases_by_key.end() ||
        access_owner_by_rnti.find(key) != access_owner_by_rnti.end()) {
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
    rnti_leases_by_key.emplace(rnti_key{update.du_index, update.pci, rnti}, std::move(lease));
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
    auto lease_it = rnti_leases_by_key.find(rnti_key{update.du_index, update.pci, rnti});
    if (lease_it == rnti_leases_by_key.end()) {
      continue;
    }
    lease_it->second.generation_id       = update.generation_id;
    lease_it->second.distribution_state  = "sent_to_du";
    lease_it->second.distribution_reason = "gnb_du_resource_coordination_request";
  }
}

void ntn_beam_service_resource_manager::mark_rnti_lease_pool_distribution_result(
    const ntn_rnti_lease_pool_update&        update,
    const f1ap_ntn_rnti_lease_pool_result& result)
{
  for (rnti_t rnti : result.accepted_leases) {
    auto lease_it = rnti_leases_by_key.find(rnti_key{update.du_index, update.pci, rnti});
    if (lease_it == rnti_leases_by_key.end()) {
      continue;
    }
    lease_it->second.generation_id       = result.generation_id;
    lease_it->second.distribution_state  = "applied_by_du";
    lease_it->second.distribution_reason = result.reject_reason.empty() ? "du_ack" : result.reject_reason;
  }
  for (rnti_t rnti : result.rejected_leases) {
    auto lease_it = rnti_leases_by_key.find(rnti_key{update.du_index, update.pci, rnti});
    if (lease_it == rnti_leases_by_key.end()) {
      continue;
    }
    lease_it->second.generation_id       = result.generation_id;
    lease_it->second.distribution_state  = "rejected_by_du";
    lease_it->second.distribution_reason = result.reject_reason.empty() ? "du_reject" : result.reject_reason;
  }
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

ntn_handover_target_rnti_reservation_result ntn_beam_service_resource_manager::reserve_handover_target_rnti(
    ue_index_t              source_ue_index,
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
ntn_beam_service_resource_manager::mark_rnti_offered_in_rar(du_index_t du_index, pci_t pci, rnti_t rnti)
{
  const rnti_key key{du_index, pci, rnti};
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
    if (lease.analog_beam_id != analog_beam_id || lease.state == "committed") {
      continue;
    }
    lease.state  = "expired";
    lease.reason = reason;
    lease.ue_index = ue_index_t::invalid;
    ++nof_expired;
  }
  return nof_expired;
}

ntn_access_rnti_ownership_result ntn_beam_service_resource_manager::validate_access_rnti_ownership(
    const ntn_access_rnti_ownership_update& update) const
{
  if (!is_valid_access_ownership_update(update)) {
    return {};
  }

  const rnti_key key{update.du_index, update.pci, update.rnti};
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
    if (lease.state != "reserved" && lease.state != "offered_in_rar") {
      return {false, "conflict", "duplicate_rnti"};
    }
  }

  const auto existing_owner_it = access_owner_by_rnti.find(key);
  if (existing_owner_it != access_owner_by_rnti.end() && existing_owner_it->second != update.ue_index) {
    return {false, "conflict", "duplicate_crnti"};
  }

  return lease_it != rnti_leases_by_key.end() ? ntn_access_rnti_ownership_result{true, "initial_ul_seen", "lease_validated"}
                                              : ntn_access_rnti_ownership_result{true, "observed", "access_active"};
}

ntn_access_rnti_ownership_result ntn_beam_service_resource_manager::register_access_rnti_ownership(
    const ntn_access_rnti_ownership_update& update)
{
  if (!is_valid_access_ownership_update(update)) {
    return {};
  }

  const rnti_key key{update.du_index, update.pci, update.rnti};
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
    if (lease.state != "reserved" && lease.state != "offered_in_rar") {
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
      !update.has_service_nci || update.digital_beam_id.empty() || !is_service_state_slot_eligible(update.service_state)) {
    return clear_digital_service_slot_intent(update.ue_index, "not_service_bound");
  }

  const bool has_paired_uplink_resource =
      !update.uplink_resource_beam_id.empty() && update.uplink_resource_du_index != du_index_t::invalid &&
      update.has_uplink_resource_nci;
  const du_index_t       slot_du_index = has_paired_uplink_resource ? update.uplink_resource_du_index : update.service_du_index;
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
    ue_index_t ue_index, const f1ap_ntn_ul_slot_resource_request& request, std::string reason)
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

void ntn_beam_service_resource_manager::mark_slot_update_sent_to_du(
    ue_index_t ue_index, const f1ap_ntn_ul_slot_resource_request& request)
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
    const f1ap_ntn_ul_slot_resource_request applied_request =
        result.applied_request.value_or(attempted_request);
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

void ntn_beam_service_resource_manager::mark_slot_update_applied(
    ue_index_t ue_index, const f1ap_ntn_ul_slot_resource_request& request)
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
ntn_beam_service_resource_manager::handle_resource_audit_report(const ntn_resource_audit_report& report) const
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

  const auto du_has_applied_rnti = [&report](rnti_t rnti) {
    return std::find_if(report.rnti_leases.begin(), report.rnti_leases.end(), [rnti](const auto& lease) {
             return lease.rnti == rnti && lease.distribution_state == "applied_by_du";
           }) != report.rnti_leases.end();
  };

  std::map<std::string, ntn_resource_repair> rnti_repairs_by_analog;
  for (const auto& entry : rnti_leases_by_key) {
    const ntn_rnti_lease& lease = entry.second;
    if (lease.du_index != report.du_index || lease.cell_index != report.cell_index || lease.pci != report.pci ||
        lease.distribution_state != "applied_by_du" || du_has_applied_rnti(lease.rnti)) {
      continue;
    }

    ntn_resource_repair& repair = rnti_repairs_by_analog[lease.analog_beam_id];
    repair.action              = ntn_resource_repair_action::resend_rnti_lease_pool;
    repair.du_index            = lease.du_index;
    repair.cell_index          = lease.cell_index;
    repair.pci                 = lease.pci;
    repair.analog_beam_id      = lease.analog_beam_id;
    repair.reason              = "du_missing_applied_rnti_pool";
    repair.rnti_leases.push_back(lease.rnti);
  }
  for (auto& repair : rnti_repairs_by_analog) {
    decision.repairs.push_back(std::move(repair.second));
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
  return decision;
}

ntn_resource_repair_record
ntn_beam_service_resource_manager::queue_resource_repair(const ntn_resource_repair& repair, uint32_t generation_id)
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
    if (entry.second.ue_index == ue_index) {
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
    if (entry.second.state == "reserved") {
      ++snapshot.nof_rnti_leases_reserved;
      if (entry.second.distribution_state == "applied_by_du") {
        ++snapshot.nof_rnti_leases_available;
      }
    } else if (entry.second.state == "offered_in_rar") {
      ++snapshot.nof_rnti_leases_offered_in_rar;
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
    }
    if (entry.second.distribution_state == "sent_to_du") {
      ++snapshot.nof_rnti_leases_sent_to_du;
    } else if (entry.second.distribution_state == "applied_by_du") {
      ++snapshot.nof_rnti_leases_applied_by_du;
    } else if (entry.second.distribution_state == "rejected_by_du") {
      ++snapshot.nof_rnti_leases_rejected_by_du;
    }
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
