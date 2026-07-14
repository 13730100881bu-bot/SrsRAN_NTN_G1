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

#include "ntn_beam_tac.h"
#include <charconv>

using namespace srsran;
using namespace srs_cu_cp;

static bool is_decimal_digit(char c)
{
  return c >= '0' && c <= '9';
}

ntn_beam_tac_result srsran::srs_cu_cp::derive_ntn_beam_tac(std::string_view beam_id)
{
  if (beam_id.empty() || !is_decimal_digit(beam_id.back())) {
    return {{}, ntn_beam_tac_invalid_reason::missing_decimal_suffix};
  }

  size_t suffix_begin = beam_id.size() - 1;
  while (suffix_begin > 0 && is_decimal_digit(beam_id[suffix_begin - 1])) {
    --suffix_begin;
  }

  uint64_t value = 0;
  const auto suffix = beam_id.substr(suffix_begin);
  const auto result = std::from_chars(suffix.data(), suffix.data() + suffix.size(), value);
  if (result.ec != std::errc{} || value >= INVALID_TAC) {
    return {{}, ntn_beam_tac_invalid_reason::out_of_range};
  }

  return {static_cast<tac_t>(value), ntn_beam_tac_invalid_reason::none};
}
