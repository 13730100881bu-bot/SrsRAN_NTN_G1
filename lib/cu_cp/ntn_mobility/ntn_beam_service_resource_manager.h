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

#pragma once

#include "ntn_beam_placement_planner.h"
#include "srsran/cu_cp/ntn_beam_service_resources.h"
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>

namespace srsran {
namespace srs_cu_cp {

/// CU-CP-only manager for NTN access ownership and digital service slot-resource intent.
class ntn_beam_service_resource_manager
{
public:
  void set_authoritative_rnti_lease_validation_enabled(bool enabled);

  ntn_rnti_lease_pool_update_result reserve_rnti_leases(const ntn_rnti_lease_pool_update& update);

  void mark_rnti_lease_pool_sent_to_du(const ntn_rnti_lease_pool_update& update);

  void mark_rnti_lease_pool_distribution_result(const ntn_rnti_lease_pool_update&        update,
                                                const f1ap_ntn_rnti_lease_pool_result& result);

  bool is_access_rnti_pool_ready(du_index_t              du_index,
                                 srsran::du_cell_index_t cell_index,
                                 pci_t                   pci,
                                 const std::string&      analog_beam_id) const;

  bool rnti_pool_below_low_watermark(du_index_t              du_index,
                                     srsran::du_cell_index_t cell_index,
                                     pci_t                   pci,
                                     const std::string&      analog_beam_id,
                                     unsigned                low_watermark) const;

  ntn_handover_target_rnti_reservation_result
  reserve_handover_target_rnti(ue_index_t              source_ue_index,
                               du_index_t              target_du_index,
                               srsran::du_cell_index_t target_cell_index,
                               pci_t                   target_pci,
                               const std::string&      target_analog_beam_id);

  ntn_access_rnti_ownership_result commit_handover_target_rnti(ue_index_t source_ue_index,
                                                               ue_index_t target_ue_index,
                                                               rnti_t     target_rnti);

  void rollback_handover_target_rnti(ue_index_t source_ue_index, std::string reason);

  ntn_access_rnti_ownership_result mark_rnti_offered_in_rar(du_index_t du_index, pci_t pci, rnti_t rnti);

  unsigned expire_rnti_leases_for_analog_beam(const std::string& analog_beam_id, std::string reason);

  ntn_access_rnti_ownership_result validate_access_rnti_ownership(
      const ntn_access_rnti_ownership_update& update) const;

  ntn_access_rnti_ownership_result register_access_rnti_ownership(const ntn_access_rnti_ownership_update& update);

  void release_analog_access_after_ics(ue_index_t ue_index);

  ntn_slot_resource_update_decision
  update_digital_service_slot_intent(const ntn_digital_service_slot_intent_update& update,
                                     const ntn_beam_placement_plan&               plan);

  ntn_slot_resource_update_decision set_digital_slot_intent_from_request(
      ue_index_t ue_index, const f1ap_ntn_ul_slot_resource_request& request, std::string reason = "legacy_ntn_slot");

  ntn_slot_resource_update_decision clear_digital_service_slot_intent(ue_index_t ue_index,
                                                                      std::string reason = "no_digital_service");

  void restore_or_clear_failed_slot_update(ue_index_t                                               ue_index,
                                           const f1ap_ntn_ul_slot_resource_request&                 attempted_request,
                                           const std::optional<f1ap_ntn_ul_slot_resource_request>& restore_request);

  void mark_slot_update_sent_to_du(ue_index_t ue_index, const f1ap_ntn_ul_slot_resource_request& request);

  void mark_slot_update_result(ue_index_t                                               ue_index,
                               const f1ap_ntn_ul_slot_resource_request&                 attempted_request,
                               const std::optional<f1ap_ntn_ul_slot_resource_request>& restore_request,
                               const f1ap_ntn_ul_slot_resource_result&                  result);

  void mark_slot_update_applied(ue_index_t ue_index, const f1ap_ntn_ul_slot_resource_request& request);

  bool has_active_digital_slot_intent(ue_index_t ue_index) const;

  std::optional<f1ap_ntn_ul_slot_resource_request> get_cached_slot_request(ue_index_t ue_index) const;

  ntn_resource_audit_decision handle_resource_audit_report(const ntn_resource_audit_report& report) const;

  ntn_resource_repair_record queue_resource_repair(const ntn_resource_repair& repair, uint32_t generation_id);

  void mark_resource_repair_sent(const ntn_resource_repair& repair);

  void mark_resource_repair_result(const ntn_resource_repair& repair, bool accepted, std::string reason);

  bool has_blocking_resource_repair() const;

  void remove_ue(ue_index_t ue_index);

  void remove_missing_ues(const std::set<ue_index_t>& live_ues);

  ntn_beam_service_resource_snapshot get_snapshot() const;

private:
  using rnti_key = std::tuple<du_index_t, pci_t, rnti_t>;
  using repair_key = std::tuple<ntn_resource_repair_action, ue_index_t, du_index_t, srsran::du_cell_index_t, pci_t, rnti_t, std::string>;

  static bool is_valid_access_ownership_update(const ntn_access_rnti_ownership_update& update);
  static bool is_valid_lease_pool_update(const ntn_rnti_lease_pool_update& update);
  static bool are_slot_requests_equal(const f1ap_ntn_ul_slot_resource_request& lhs,
                                      const f1ap_ntn_ul_slot_resource_request& rhs);
  static repair_key make_repair_key(const ntn_resource_repair& repair);
  static ntn_resource_repair_record make_repair_record(const ntn_resource_repair& repair, uint32_t generation_id);

  std::optional<f1ap_ntn_ul_slot_resource_request>
  make_slot_request_for_nci(nr_cell_identity nci, du_index_t service_du_index, const ntn_beam_placement_plan& plan) const;

  std::map<rnti_key, ue_index_t> access_owner_by_rnti;
  std::map<ue_index_t, rnti_key> access_rnti_key_by_ue;
  std::map<rnti_key, ntn_rnti_lease> rnti_leases_by_key;
  std::map<ue_index_t, ntn_access_rnti_ownership> access_ownership_by_ue;
  std::map<ue_index_t, ntn_digital_slot_resource_intent> digital_slot_intent_by_ue;
  std::map<ue_index_t, f1ap_ntn_ul_slot_resource_request> cached_slot_requests_by_ue;
  std::map<ue_index_t, f1ap_ntn_ul_slot_resource_request> applied_slot_requests_by_ue;
  std::map<repair_key, ntn_resource_repair_record> resource_repairs_by_key;
  bool authoritative_rnti_lease_validation_enabled = false;
};

} // namespace srs_cu_cp
} // namespace srsran
