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

#include "srsran/adt/expected.h"
#include "srsran/asn1/rrc_nr/sys_info.h"
#include "srsran/cu_cp/ntn_location.h"
#include <string>

namespace srsran {
namespace srs_cu_cp {

/// Builds a bounded CU-CP SIB19 assistance packaging contract.
ntn_sib19_assistance_snapshot build_ntn_sib19_assistance_snapshot(const ntn_sib19_assistance_request& request);

/// Converts one CU-CP SIB19 assistance contract entry into an RRC SIB19-r17 ASN.1 value.
expected<asn1::rrc_nr::sib19_r17_s, std::string>
make_asn1_rrc_sib19_from_ntn_assistance(const ntn_sib19_assistance_entry& entry);

} // namespace srs_cu_cp
} // namespace srsran
