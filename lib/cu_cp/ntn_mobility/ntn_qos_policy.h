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

#include "srsran/cu_cp/cu_cp_types.h"
#include "srsran/cu_cp/ntn_qos_policy.h"
#include "srsran/cu_cp/up_context.h"

namespace srsran {
namespace srs_cu_cp {

void merge_ntn_qos_demand(ntn_qos_demand_summary&                  summary,
                          const qos_flow_level_qos_parameters&     qos_params,
                          const std::optional<s_nssai_t>&          s_nssai);

ntn_qos_demand_summary summarize_ntn_qos_demand(const cu_cp_pdu_session_resource_setup_request& request);

ntn_qos_demand_summary summarize_ntn_qos_demand(const up_context& context);

} // namespace srs_cu_cp
} // namespace srsran
