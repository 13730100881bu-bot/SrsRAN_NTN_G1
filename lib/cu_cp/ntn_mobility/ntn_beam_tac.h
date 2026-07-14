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

#include "srsran/ran/tac.h"
#include <optional>
#include <string_view>

namespace srsran {
namespace srs_cu_cp {

enum class ntn_beam_tac_invalid_reason { none, missing_decimal_suffix, out_of_range };

struct ntn_beam_tac_result {
  std::optional<tac_t>       tac;
  ntn_beam_tac_invalid_reason reason = ntn_beam_tac_invalid_reason::none;
};

/// Derive a 24-bit TAC from the final contiguous decimal suffix of a beam identifier.
ntn_beam_tac_result derive_ntn_beam_tac(std::string_view beam_id);

} // namespace srs_cu_cp
} // namespace srsran
