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

#include <string>

namespace srsran {
namespace srs_cu_cp {

/// CU-CP summary of UE NTN capability derived from the RRC UE capability RAT container list.
enum class ntn_ue_capability_state { unknown, supported, unsupported, parse_failed };

/// CU-CP interpretation of Rel-17 NTN scenario support for the current deployment profile.
enum class ntn_ue_capability_scenario_support { absent, ngso, gso, both, implicit_both };

struct ntn_ue_capability_summary {
  ntn_ue_capability_state state  = ntn_ue_capability_state::unknown;
  std::string             reason = "no_capabilities";
  bool                    parsed = false;

  bool        has_nr_container                  = false;
  bool        non_terrestrial_network_r17       = false;
  bool        ntn_scenario_support_r17          = false;
  std::string ntn_scenario                      = "none";
  bool        ntn_parameters_r17                = false;

  ntn_ue_capability_scenario_support scenario_support          = ntn_ue_capability_scenario_support::absent;
  std::string                         deployment_profile        = "leo_ngso";
  bool                                matched_deployment_profile = false;
  std::string                         profile_block_reason       = "no_capabilities";
};

const char* to_string(ntn_ue_capability_state state);
const char* to_string(ntn_ue_capability_scenario_support scenario);

} // namespace srs_cu_cp
} // namespace srsran
