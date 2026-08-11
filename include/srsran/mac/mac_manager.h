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

#pragma once

#include "srsran/ran/du_types.h"
#include "srsran/ran/nr_cell_identity.h"
#include "srsran/ran/pci.h"
#include "srsran/ran/rnti.h"
#include "srsran/ran/slot_point.h"
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace srsran {

namespace ofh {
class prach_beam_context_provider;
}

class mac_cell_manager;
class mac_ue_configurator;
class mac_positioning_measurement_handler;

enum class mac_ntn_rnti_lease_pool_operation { replace, add, clear, retire };

struct mac_ntn_rnti_lease_pool_update {
  du_cell_index_t                      cell_index    = INVALID_DU_CELL_INDEX;
  mac_ntn_rnti_lease_pool_operation    operation     = mac_ntn_rnti_lease_pool_operation::replace;
  uint32_t                             generation_id = 0;
  uint32_t                             expiry_ms      = 0;
  std::vector<rnti_t>                  leases;
};

struct mac_ntn_rnti_lease_pool_result {
  bool                accepted = false;
  std::string         reason;
  std::vector<rnti_t> accepted_leases;
  std::vector<rnti_t> rejected_leases;
};

/// Read-only state of one CU-CP-provided NTN RNTI lease in MAC.
struct mac_ntn_rnti_lease_snapshot_entry {
  rnti_t      rnti          = rnti_t::INVALID_RNTI;
  uint32_t    generation_id = 0;
  std::string state;
  std::string distribution_state;
};

/// Authoritative MAC snapshot for one cell's NTN RNTI lease pool.
struct mac_ntn_rnti_lease_pool_snapshot {
  du_cell_index_t                                cell_index         = INVALID_DU_CELL_INDEX;
  bool                                           complete           = false;
  bool                                           lease_mode_enabled = false;
  bool                                           retirement_supported       = false;
  uint32_t                                       rnti_generation_high_water = 0;
  std::vector<mac_ntn_rnti_lease_snapshot_entry> leases;
};

enum class mac_ntn_access_calendar_operation { prepare, query, clear };
enum class mac_ntn_access_calendar_direction { downlink, uplink };
enum class mac_ntn_access_calendar_purpose { ssb_sib_paging, ssb_sib_paging_rar, prach_ro, prach_ul_beam };
enum class mac_ntn_access_calendar_status { preparing, ready, applied, cleared, rejected, unsupported };
enum class mac_ntn_access_calendar_preflight_purpose { ssb, prach, invalid };

/// First raw common-channel intent that could not be matched to an existing scheduler opportunity.
struct mac_ntn_access_calendar_unmatched_intent {
  std::string                               position_id;
  mac_ntn_access_calendar_preflight_purpose purpose = mac_ntn_access_calendar_preflight_purpose::invalid;
  uint32_t                                  start_slot_offset = 0;
  uint32_t                                  nof_slots         = 0;
};

/// Read-only scheduler preflight evidence for one cell. It is not proof of PHY or RF execution.
struct mac_ntn_access_calendar_preflight_report {
  bool                                                    performed           = false;
  bool                                                    passed              = false;
  uint8_t                                                 numerology          = 0xffU;
  uint32_t                                                expected_ssb        = 0;
  uint32_t                                                matched_ssb         = 0;
  uint32_t                                                expected_prach      = 0;
  uint32_t                                                matched_prach       = 0;
  uint32_t                                                max_ssb_gap_slots   = 0;
  uint32_t                                                max_prach_gap_slots = 0;
  std::optional<mac_ntn_access_calendar_unmatched_intent> first_unmatched;
};

struct mac_ntn_access_calendar_intent {
  std::string                       position_id;
  std::chrono::microseconds         start_time{0};
  std::chrono::microseconds         duration{0};
  mac_ntn_access_calendar_direction direction = mac_ntn_access_calendar_direction::downlink;
  mac_ntn_access_calendar_purpose   purpose   = mac_ntn_access_calendar_purpose::ssb_sib_paging;
  uint16_t                          port_id   = std::numeric_limits<uint16_t>::max();
};

struct mac_ntn_access_calendar_cell {
  du_cell_index_t                            cell_index = INVALID_DU_CELL_INDEX;
  nr_cell_identity                          nci        = nr_cell_identity::min();
  pci_t                                     pci        = INVALID_PCI;
  std::vector<mac_ntn_access_calendar_intent> intents;
};

struct mac_ntn_access_calendar_update {
  mac_ntn_access_calendar_operation operation = mac_ntn_access_calendar_operation::query;
  std::string                       satellite_id;
  uint64_t                          catalog_version = 0;
  uint64_t                          schedule_version = 0;
  std::string                       source_content_hash;
  std::string                       calendar_hash;
  std::chrono::system_clock::time_point activation_epoch{};
  std::chrono::system_clock::time_point valid_until{};
  std::chrono::microseconds              cycle_duration{0};
  std::array<mac_ntn_access_calendar_cell, 2> cells{};
};

struct mac_ntn_access_calendar_result {
  mac_ntn_access_calendar_status status = mac_ntn_access_calendar_status::rejected;
  std::string                    reason;
  uint64_t                       schedule_version = 0;
  std::string                    calendar_hash;
  slot_point                     effective_activation_slot;
  unsigned                       activation_numerology = 0;
  unsigned                       minimum_lead_slots = 0;
  std::array<unsigned, 2>        accepted_intents{};
  std::array<mac_ntn_access_calendar_preflight_report, 2> preflight_reports{};

  bool accepted() const
  {
    return status == mac_ntn_access_calendar_status::preparing || status == mac_ntn_access_calendar_status::ready ||
           status == mac_ntn_access_calendar_status::applied || status == mac_ntn_access_calendar_status::cleared;
  }
};

/// Interface used by the management plane of the DU.
class mac_manager
{
public:
  virtual ~mac_manager() = default;

  /// Interface to manage the creation, reconfiguration, deletion, activation and deactivation of cells.
  virtual mac_cell_manager& get_cell_manager() = 0;

  /// Interface to manage the creation, reconfiguration and deletion of UEs in the MAC.
  virtual mac_ue_configurator& get_ue_configurator() = 0;

  /// Fetch positioning measurement handler.
  virtual mac_positioning_measurement_handler& get_positioning_handler() = 0;

  /// Apply CU-CP-authoritative NTN RNTI leases to MAC before PRACH/RAR.
  virtual mac_ntn_rnti_lease_pool_result apply_ntn_rnti_lease_pool_update(
      const mac_ntn_rnti_lease_pool_update& request) = 0;

  /// Returns a read-only snapshot of pending, consumed and expired NTN leases for one cell.
  /// An incomplete result means that this MAC implementation cannot provide authoritative audit evidence.
  virtual mac_ntn_rnti_lease_pool_snapshot get_ntn_rnti_lease_pool_snapshot(du_cell_index_t cell_index)
  {
    mac_ntn_rnti_lease_pool_snapshot result;
    result.cell_index = cell_index;
    return result;
  }

  /// Prepare, query or clear a versioned two-cell access calendar in the MAC scheduler.
  /// "applied" proves scheduler software state only; it is not RU/RF telemetry.
  virtual mac_ntn_access_calendar_result
  apply_ntn_access_calendar_update(const mac_ntn_access_calendar_update& request) = 0;

  /// Returns the optional slot-specific OFH PRACH BeamId provider used by an NTN receive mapping.
  virtual std::shared_ptr<ofh::prach_beam_context_provider> get_ntn_prach_beam_context_provider() { return {}; }
};

} // namespace srsran
