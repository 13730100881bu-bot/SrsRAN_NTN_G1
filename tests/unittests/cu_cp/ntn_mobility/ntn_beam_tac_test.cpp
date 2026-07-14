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

#include "lib/cu_cp/ntn_mobility/ntn_beam_tac.h"
#include <gtest/gtest.h>

using namespace srsran;
using namespace srs_cu_cp;

TEST(ntn_beam_tac, derives_tac_from_final_decimal_run)
{
  const ntn_beam_tac_result beam7 = derive_ntn_beam_tac("CN-BEAM-0007");
  ASSERT_TRUE(beam7.tac.has_value());
  EXPECT_EQ(beam7.tac.value(), 7U);
  EXPECT_EQ(beam7.reason, ntn_beam_tac_invalid_reason::none);

  const ntn_beam_tac_result beam42 = derive_ntn_beam_tac("beam-42");
  ASSERT_TRUE(beam42.tac.has_value());
  EXPECT_EQ(beam42.tac.value(), 42U);
  EXPECT_EQ(beam42.reason, ntn_beam_tac_invalid_reason::none);
}

TEST(ntn_beam_tac, rejects_missing_or_out_of_range_decimal_run_without_fatal_error)
{
  const ntn_beam_tac_result missing = derive_ntn_beam_tac("CN-BEAM-A");
  EXPECT_FALSE(missing.tac.has_value());
  EXPECT_EQ(missing.reason, ntn_beam_tac_invalid_reason::missing_decimal_suffix);

  const ntn_beam_tac_result too_large = derive_ntn_beam_tac("CN-BEAM-16777216");
  EXPECT_FALSE(too_large.tac.has_value());
  EXPECT_EQ(too_large.reason, ntn_beam_tac_invalid_reason::out_of_range);
}
