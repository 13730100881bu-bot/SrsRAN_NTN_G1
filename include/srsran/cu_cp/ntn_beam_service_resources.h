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

#include "srsran/cu_cp/cu_cp_types.h"
#include "srsran/f1ap/ntn_rnti_lease_pool.h"
#include "srsran/f1ap/ntn_ul_slot_resource_request.h"
#include "srsran/ran/nr_cgi.h"
#include "srsran/ran/pci.h"
#include "srsran/ran/rnti.h"
#include <optional>
#include <string>
#include <vector>

namespace srsran {
namespace srs_cu_cp {

/// CU-CP update that records the DU-reported C-RNTI ownership for one NTN access UE.
struct ntn_access_rnti_ownership_update {
  ue_index_t       ue_index = ue_index_t::invalid;
  du_index_t       du_index = du_index_t::invalid;
  srsran::du_cell_index_t cell_index = srsran::INVALID_DU_CELL_INDEX;
  pci_t            pci      = INVALID_PCI;
  rnti_t           rnti     = rnti_t::INVALID_RNTI;
  std::string      analog_beam_id;
  nr_cell_identity access_nci = nr_cell_identity::min();
  bool             has_access_nci = false;
};

/// Result of a CU-CP access C-RNTI ownership update.
struct ntn_access_rnti_ownership_result {
  bool        accepted = false;
  std::string state    = "invalid";
  std::string reason   = "invalid_identity";
};

/// CU-CP authoritative RNTI lease pool update for one DU/cell/analog access beam.
struct ntn_rnti_lease_pool_update {
  du_index_t          du_index = du_index_t::invalid;
  srsran::du_cell_index_t cell_index = srsran::INVALID_DU_CELL_INDEX;
  pci_t               pci      = INVALID_PCI;
  std::string         analog_beam_id;
  uint32_t            generation_id = 0;
  std::vector<rnti_t> leases;
};

/// Result of a CU-CP RNTI lease pool update.
struct ntn_rnti_lease_pool_update_result {
  bool        accepted = false;
  std::string state    = "invalid";
  std::string reason   = "invalid_lease_pool";
  unsigned    nof_leases_reserved = 0;
};

/// Result of reserving a CU-CP-authoritative RNTI lease for an NTN handover target UE.
struct ntn_handover_target_rnti_reservation_result {
  bool        accepted = false;
  rnti_t      rnti     = rnti_t::INVALID_RNTI;
  std::string state    = "invalid";
  std::string reason   = "target_rnti_unavailable";
};

/// Public read-only CU-CP authoritative RNTI lease state.
struct ntn_rnti_lease {
  du_index_t  du_index = du_index_t::invalid;
  srsran::du_cell_index_t cell_index = srsran::INVALID_DU_CELL_INDEX;
  pci_t       pci      = INVALID_PCI;
  rnti_t      rnti     = rnti_t::INVALID_RNTI;
  std::string analog_beam_id;
  uint32_t    generation_id = 0;
  std::string state  = "none";
  std::string reason = "none";
  std::string distribution_state = "desired";
  std::string distribution_reason = "local_reservation";
  ue_index_t  ue_index = ue_index_t::invalid;
};

/// Public read-only CU-CP ownership record for one DU-reported C-RNTI.
struct ntn_access_rnti_ownership {
  ue_index_t       ue_index = ue_index_t::invalid;
  du_index_t       du_index = du_index_t::invalid;
  srsran::du_cell_index_t cell_index = srsran::INVALID_DU_CELL_INDEX;
  pci_t            pci      = INVALID_PCI;
  rnti_t           rnti     = rnti_t::INVALID_RNTI;
  std::string      analog_beam_id;
  nr_cell_identity access_nci = nr_cell_identity::min();
  bool             has_access_nci = false;
  std::string      state = "none";
  std::string      reason = "none";
};

/// CU-CP update describing one UE's digital service slot-resource need.
struct ntn_digital_service_slot_intent_update {
  ue_index_t       ue_index = ue_index_t::invalid;
  std::string      digital_beam_id;
  du_index_t       service_du_index = du_index_t::invalid;
  srsran::du_cell_index_t service_cell_index = srsran::INVALID_DU_CELL_INDEX;
  pci_t            service_pci = INVALID_PCI;
  nr_cell_identity service_nci = nr_cell_identity::min();
  bool             has_service_nci = false;
  std::string      uplink_resource_beam_id;
  du_index_t       uplink_resource_du_index = du_index_t::invalid;
  srsran::du_cell_index_t uplink_resource_cell_index = srsran::INVALID_DU_CELL_INDEX;
  pci_t            uplink_resource_pci = INVALID_PCI;
  nr_cell_identity uplink_resource_nci = nr_cell_identity::min();
  bool             has_uplink_resource_nci = false;
  std::string      service_state = "none";
};

/// Public read-only CU-CP digital service slot-resource intent.
struct ntn_digital_slot_resource_intent {
  ue_index_t       ue_index = ue_index_t::invalid;
  std::string      digital_beam_id;
  du_index_t       service_du_index = du_index_t::invalid;
  srsran::du_cell_index_t service_cell_index = srsran::INVALID_DU_CELL_INDEX;
  pci_t            service_pci = INVALID_PCI;
  nr_cell_identity service_nci = nr_cell_identity::min();
  bool             has_service_nci = false;
  std::string      uplink_resource_beam_id;
  du_index_t       uplink_resource_du_index = du_index_t::invalid;
  srsran::du_cell_index_t uplink_resource_cell_index = srsran::INVALID_DU_CELL_INDEX;
  pci_t            uplink_resource_pci = INVALID_PCI;
  nr_cell_identity uplink_resource_nci = nr_cell_identity::min();
  bool             has_uplink_resource_nci = false;
  std::string      state = "none";
  std::string      reason = "none";
  f1ap_ntn_ul_slot_resource_request request;
  std::optional<f1ap_ntn_ul_slot_resource_request> applied_request;
};

/// DU-observed RNTI lease state returned by an NTN resource audit.
struct ntn_resource_audit_rnti_lease {
  rnti_t      rnti = rnti_t::INVALID_RNTI;
  std::string state;
  std::string distribution_state;
};

/// DU-observed per-UE SR/SRS slot-resource state returned by an NTN resource audit.
struct ntn_resource_audit_ue_slot {
  ue_index_t                         ue_index = ue_index_t::invalid;
  std::string                        state;
  f1ap_ntn_ul_slot_resource_request  request;
};

/// CU-CP-normalized DU resource audit report.
struct ntn_resource_audit_report {
  du_index_t                            du_index = du_index_t::invalid;
  srsran::du_cell_index_t               cell_index = srsran::INVALID_DU_CELL_INDEX;
  pci_t                                 pci = INVALID_PCI;
  uint32_t                              generation_id = 0;
  bool                                  accepted = true;
  std::string                           reject_reason;
  std::vector<ntn_resource_audit_rnti_lease> rnti_leases;
  std::vector<ntn_resource_audit_ue_slot>    ue_slots;
};

enum class ntn_resource_repair_action {
  none,
  resend_rnti_lease_pool,
  apply_sr_srs_assignment,
  resend_sr_srs_clear,
  clear_unknown_sr_srs_assignment,
  rollback_handover_target_rnti,
  mark_resource_conflict
};

/// CU-CP-authoritative repair action generated after comparing DU audit state.
struct ntn_resource_repair {
  ntn_resource_repair_action action = ntn_resource_repair_action::none;
  ue_index_t                 ue_index = ue_index_t::invalid;
  du_index_t                 du_index = du_index_t::invalid;
  srsran::du_cell_index_t    cell_index = srsran::INVALID_DU_CELL_INDEX;
  pci_t                      pci = INVALID_PCI;
  rnti_t                     rnti = rnti_t::INVALID_RNTI;
  std::string                analog_beam_id;
  std::string                service_beam_id;
  std::string                uplink_resource_beam_id;
  du_index_t                 uplink_resource_du_index = du_index_t::invalid;
  nr_cell_identity           uplink_resource_nci = nr_cell_identity::min();
  bool                       has_uplink_resource_nci = false;
  std::string                reason;
  std::vector<rnti_t>        rnti_leases;
  std::optional<f1ap_ntn_ul_slot_resource_request> slot_request;
};

struct ntn_resource_audit_decision {
  uint32_t                         generation_id = 0;
  unsigned                         nof_mismatches = 0;
  unsigned                         nof_repairs = 0;
  std::vector<ntn_resource_repair> repairs;
};

/// Public read-only CU-CP repair executor state for one authoritative NTN repair action.
struct ntn_resource_repair_record {
  ntn_resource_repair_action action = ntn_resource_repair_action::none;
  ue_index_t                 ue_index = ue_index_t::invalid;
  du_index_t                 du_index = du_index_t::invalid;
  srsran::du_cell_index_t    cell_index = srsran::INVALID_DU_CELL_INDEX;
  pci_t                      pci = INVALID_PCI;
  rnti_t                     rnti = rnti_t::INVALID_RNTI;
  std::string                analog_beam_id;
  std::string                service_beam_id;
  std::string                uplink_resource_beam_id;
  du_index_t                 uplink_resource_du_index = du_index_t::invalid;
  nr_cell_identity           uplink_resource_nci = nr_cell_identity::min();
  bool                       has_uplink_resource_nci = false;
  uint32_t                   generation_id = 0;
  unsigned                   retry_count = 0;
  std::string                state = "none";
  std::string                reason = "none";
};

enum class ntn_slot_resource_update_action { none, set, clear };

/// Decision returned when CU-CP evaluates whether a UE needs a DU-side slot-resource update.
struct ntn_slot_resource_update_decision {
  ntn_slot_resource_update_action action = ntn_slot_resource_update_action::none;
  ue_index_t ue_index = ue_index_t::invalid;
  std::optional<f1ap_ntn_ul_slot_resource_request> request;
  std::optional<f1ap_ntn_ul_slot_resource_request> previous_request;
  std::string reason = "unchanged";
};

/// CU-CP resource-manager observability snapshot.
struct ntn_beam_service_resource_snapshot {
  std::vector<ntn_rnti_lease>                  rnti_leases;
  std::vector<ntn_access_rnti_ownership>       access_rnti_ownerships;
  std::vector<ntn_digital_slot_resource_intent> digital_slot_intents;
  std::vector<ntn_resource_repair_record>       resource_repairs;
  unsigned nof_rnti_leases_reserved        = 0;
  unsigned nof_rnti_leases_sent_to_du      = 0;
  unsigned nof_rnti_leases_applied_by_du   = 0;
  unsigned nof_rnti_leases_rejected_by_du  = 0;
  unsigned nof_rnti_leases_available       = 0;
  unsigned nof_rnti_leases_offered_in_rar  = 0;
  unsigned nof_rnti_leases_initial_ul_seen = 0;
  unsigned nof_rnti_leases_committed       = 0;
  unsigned nof_rnti_leases_released        = 0;
  unsigned nof_rnti_leases_expired         = 0;
  unsigned nof_rnti_leases_conflict        = 0;
  unsigned nof_access_rnti_owned     = 0;
  unsigned nof_access_rnti_conflicts = 0;
  unsigned nof_digital_slot_desired        = 0;
  unsigned nof_digital_slot_sent_to_du     = 0;
  unsigned nof_digital_slot_applied_by_du  = 0;
  unsigned nof_digital_slot_rejected_by_du = 0;
  unsigned nof_digital_slot_clear_sent     = 0;
  unsigned nof_digital_slot_cleared_by_du  = 0;
  unsigned nof_digital_slot_rollback       = 0;
  unsigned nof_digital_slot_active         = 0;
  unsigned nof_digital_slot_cleared        = 0;
  unsigned nof_resource_repairs_queued          = 0;
  unsigned nof_resource_repairs_sent            = 0;
  unsigned nof_resource_repairs_applied         = 0;
  unsigned nof_resource_repairs_failed          = 0;
  unsigned nof_resource_repairs_retry_exhausted = 0;
  unsigned nof_resource_repairs_blocked_conflict = 0;
  unsigned nof_service_pair_digital_slot_intents = 0;
  unsigned nof_service_pair_resource_repairs     = 0;
};

} // namespace srs_cu_cp
} // namespace srsran
