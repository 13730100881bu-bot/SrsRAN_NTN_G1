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

#include "srsran/f1ap/ntn_access_calendar.h"
#include "srsran/f1ap/ntn_rnti_lease_pool.h"
#include <optional>

namespace srsran::srs_cu_cp {

struct f1ap_gnb_du_resource_coordination_request {
  std::optional<f1ap_ntn_access_calendar_update> ntn_access_calendar_update;
  f1ap_ntn_rnti_lease_pool_update ntn_rnti_lease_update;
  f1ap_ntn_resource_audit_request ntn_resource_audit_request;
  f1ap_ntn_sib19_broadcast_update ntn_sib19_broadcast_update;
};

struct f1ap_gnb_du_resource_coordination_response {
  bool                                           success = false;
  std::optional<f1ap_ntn_access_calendar_result> calendar_result;
  std::optional<f1ap_ntn_rnti_lease_pool_result> result;
  std::optional<f1ap_ntn_resource_audit_result>  audit_result;
  std::optional<f1ap_ntn_sib19_broadcast_result> sib19_result;
};

} // namespace srsran::srs_cu_cp
