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

#include "srsran/ran/du_types.h"
#include "srsran/ran/nr_cell_identity.h"
#include "srsran/ran/pci.h"
#include "srsran/ran/rnti.h"
#include "srsran/ran/slot_point.h"
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace srsran {

enum class mac_ntn_initial_ul_position_authority : uint8_t {
  none,
  software_attributed,
  sdr_rx_port_verified,
  ofh_beam_id_verified,
  invalid = 0xffU
};

enum class mac_ntn_rx_backend : uint8_t { sdr, ofh, invalid = 0xffU };

/// One deployment-local receive mapping. The mapping never changes NR cell identity.
struct mac_ntn_rx_port_mapping {
  nr_cell_identity nci = nr_cell_identity::min();
  uint16_t         cell_local_port = std::numeric_limits<uint16_t>::max();
  mac_ntn_rx_backend backend = mac_ntn_rx_backend::invalid;
  uint16_t            physical_rx_port = std::numeric_limits<uint16_t>::max();
  std::optional<uint16_t> prach_eaxc;
  std::optional<uint16_t> beam_id;
};

/// Optional local receive mapping. It is independent of the management-center position-plan version.
struct mac_ntn_rx_mapping_config {
  bool        enabled = false;
  uint64_t    version = 0;
  std::string hash;
  float       unique_margin_db = 6.0F;
  std::vector<mac_ntn_rx_port_mapping> entries;
};

/// One bounded, one-shot MAC-to-DU record associated with the Initial UL C-RNTI.
struct mac_ntn_initial_ul_position_record {
  uint64_t observation_id = 0;

  du_cell_index_t cell_index = INVALID_DU_CELL_INDEX;
  rnti_t          c_rnti     = rnti_t::INVALID_RNTI;
  uint32_t        rnti_generation = 0;
  nr_cell_identity nci = nr_cell_identity::min();
  pci_t            pci = INVALID_PCI;

  mac_ntn_initial_ul_position_authority authority = mac_ntn_initial_ul_position_authority::none;
  std::string                           reason;
  std::string                           satellite_id;
  uint64_t                              catalog_version  = 0;
  uint64_t                              schedule_version = 0;
  std::string                           source_content_hash;
  std::string                           calendar_hash;
  std::string                           position_id;
  uint16_t                              cell_local_port = std::numeric_limits<uint16_t>::max();
  uint16_t                              physical_rx_port = std::numeric_limits<uint16_t>::max();

  std::optional<uint16_t> prach_eaxc;
  std::optional<uint16_t> beam_id;
  uint64_t                mapping_version = 0;
  std::string             mapping_hash;
  float                   receive_port_margin_db = 0.0F;

  slot_point prach_slot;
  uint64_t   calendar_cycle_index = 0;
  uint32_t   cycle_slot_offset = 0;
  uint32_t   occasion_offset_us = 0;

  bool usable() const
  {
    return observation_id != 0 && reason.empty() && authority != mac_ntn_initial_ul_position_authority::none &&
           authority != mac_ntn_initial_ul_position_authority::invalid;
  }
};

} // namespace srsran
