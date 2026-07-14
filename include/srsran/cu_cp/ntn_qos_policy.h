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

#include "srsran/ran/qos/five_qi.h"
#include "srsran/ran/s_nssai.h"
#include <optional>

namespace srsran {
namespace srs_cu_cp {

/// CU-CP summary of QoS demand used to order NTN loaded service beams.
struct ntn_qos_demand_summary {
  bool                      has_qos_demand          = false;
  unsigned                  best_arp_priority       = 15;
  unsigned                  best_qos_priority       = 127;
  five_qi_t                 best_five_qi            = five_qi_t::invalid;
  bool                      has_gbr                 = false;
  bool                      has_delay_critical_gbr  = false;
  bool                      may_trigger_preemption  = false;
  bool                      is_preemptable          = false;
  unsigned                  nof_qos_flows           = 0;
  std::optional<s_nssai_t>  primary_s_nssai;

  bool operator==(const ntn_qos_demand_summary& other) const
  {
    return has_qos_demand == other.has_qos_demand && best_arp_priority == other.best_arp_priority &&
           best_qos_priority == other.best_qos_priority && best_five_qi == other.best_five_qi &&
           has_gbr == other.has_gbr && has_delay_critical_gbr == other.has_delay_critical_gbr &&
           may_trigger_preemption == other.may_trigger_preemption && is_preemptable == other.is_preemptable &&
           nof_qos_flows == other.nof_qos_flows && primary_s_nssai == other.primary_s_nssai;
  }
};

/// Returns true when lhs is preferred over rhs for CU-CP NTN service placement.
bool is_ntn_qos_demand_higher_priority(const ntn_qos_demand_summary& lhs, const ntn_qos_demand_summary& rhs);

} // namespace srs_cu_cp
} // namespace srsran
