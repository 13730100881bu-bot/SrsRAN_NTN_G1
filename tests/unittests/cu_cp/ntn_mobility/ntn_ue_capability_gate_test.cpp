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

#include "lib/cu_cp/ntn_mobility/ntn_ue_capability_gate.h"
#include "srsran/asn1/rrc_nr/ue_cap.h"
#include "srsran/asn1/rrc_nr/ul_dcch_msg_ies.h"
#include "srsran/adt/byte_buffer.h"
#include "srsran/asn1/asn1_utils.h"
#include <gtest/gtest.h>

using namespace srsran;
using namespace srsran::srs_cu_cp;
using namespace asn1::rrc_nr;

static byte_buffer pack_nr_ue_capability(bool                                           ntn_supported,
                                         bool                                           scenario_present,
                                         ue_nr_cap_v1700_s::ntn_scenario_support_r17_opts::options scenario,
                                         bool                                           params_present)
{
  ue_nr_cap_s cap;
  cap.access_stratum_release.value = access_stratum_release_opts::rel17;
  cap.pdcp_params.max_num_rohc_context_sessions.value = pdcp_params_s::max_num_rohc_context_sessions_opts::cs2;
  band_nr_s band;
  band.band_nr = 78;
  cap.rf_params.supported_band_list_nr.push_back(band);

  cap.non_crit_ext_present          = true;
  auto& v1530                       = cap.non_crit_ext;
  v1530.non_crit_ext_present        = true;
  auto& v1540                       = v1530.non_crit_ext;
  v1540.non_crit_ext_present        = true;
  auto& v1550                       = v1540.non_crit_ext;
  v1550.non_crit_ext_present        = true;
  auto& v1560                       = v1550.non_crit_ext;
  v1560.non_crit_ext_present        = true;
  auto& v1570                       = v1560.non_crit_ext;
  v1570.non_crit_ext_present        = true;
  auto& v1610                       = v1570.non_crit_ext;
  v1610.non_crit_ext_present        = true;
  auto& v1640                       = v1610.non_crit_ext;
  v1640.non_crit_ext_present        = true;
  auto& v1650                       = v1640.non_crit_ext;
  v1650.non_crit_ext_present        = true;
  auto& v1690                       = v1650.non_crit_ext;
  v1690.non_crit_ext_present        = true;
  ue_nr_cap_v1700_s& v1700          = v1690.non_crit_ext;
  v1700.non_terrestrial_network_r17_present = ntn_supported;
  v1700.ntn_scenario_support_r17_present    = scenario_present;
  v1700.ntn_scenario_support_r17.value      = scenario;
  v1700.ntn_params_r17_present              = params_present;

  byte_buffer cap_pdu;
  asn1::bit_ref cap_bref{cap_pdu};
  report_fatal_error_if_not(cap.pack(cap_bref) == asn1::SRSASN_SUCCESS, "Failed to pack UE NR capability");

  ue_cap_rat_container_list_l list;
  ue_cap_rat_container_s      container;
  container.rat_type.value = rat_type_opts::nr;
  report_fatal_error_if_not(container.ue_cap_rat_container.resize(cap_pdu.length()),
                            "Failed to size NR capability container");
  std::copy(cap_pdu.begin(), cap_pdu.end(), container.ue_cap_rat_container.begin());
  list.push_back(container);

  byte_buffer packed_list;
  asn1::bit_ref list_bref{packed_list};
  report_fatal_error_if_not(asn1::pack_dyn_seq_of(list_bref, list, 0, 8) == asn1::SRSASN_SUCCESS,
                            "Failed to pack UE capability RAT container list");
  return packed_list;
}

static byte_buffer pack_eutra_only_capability()
{
  ue_cap_rat_container_list_l list;
  ue_cap_rat_container_s      container;
  container.rat_type.value = rat_type_opts::eutra;
  report_fatal_error_if_not(container.ue_cap_rat_container.resize(1), "Failed to size EUTRA capability container");
  container.ue_cap_rat_container[0] = 0;
  list.push_back(container);

  byte_buffer packed_list;
  asn1::bit_ref list_bref{packed_list};
  report_fatal_error_if_not(asn1::pack_dyn_seq_of(list_bref, list, 0, 8) == asn1::SRSASN_SUCCESS,
                            "Failed to pack EUTRA capability RAT container list");
  return packed_list;
}

TEST(ntn_ue_capability_gate, nr_container_with_non_terrestrial_network_r17_is_supported)
{
  ntn_ue_capability_summary summary =
      evaluate_ntn_ue_capability(pack_nr_ue_capability(true, true, ue_nr_cap_v1700_s::ntn_scenario_support_r17_opts::ngso, true));

  EXPECT_EQ(summary.state, ntn_ue_capability_state::supported);
  EXPECT_EQ(summary.reason, "supported");
  EXPECT_TRUE(summary.has_nr_container);
  EXPECT_TRUE(summary.non_terrestrial_network_r17);
  EXPECT_TRUE(summary.ntn_scenario_support_r17);
  EXPECT_EQ(summary.ntn_scenario, "ngso");
  EXPECT_EQ(summary.scenario_support, ntn_ue_capability_scenario_support::ngso);
  EXPECT_TRUE(summary.matched_deployment_profile);
  EXPECT_EQ(summary.deployment_profile, "leo_ngso");
  EXPECT_EQ(summary.profile_block_reason, "profile_matched");
  EXPECT_TRUE(summary.ntn_parameters_r17);
}

TEST(ntn_ue_capability_gate, gso_only_ntn_ue_is_supported_but_blocked_for_leo_ngso_profile)
{
  ntn_ue_capability_summary summary =
      evaluate_ntn_ue_capability(pack_nr_ue_capability(true, true, ue_nr_cap_v1700_s::ntn_scenario_support_r17_opts::gso, true));

  EXPECT_EQ(summary.state, ntn_ue_capability_state::supported);
  EXPECT_EQ(summary.reason, "supported");
  EXPECT_EQ(summary.ntn_scenario, "gso");
  EXPECT_EQ(summary.scenario_support, ntn_ue_capability_scenario_support::gso);
  EXPECT_FALSE(summary.matched_deployment_profile);
  EXPECT_EQ(summary.deployment_profile, "leo_ngso");
  EXPECT_EQ(summary.profile_block_reason, "scenario_mismatch");
}

TEST(ntn_ue_capability_gate, absent_scenario_support_is_implicit_both_and_matches_leo_ngso_profile)
{
  ntn_ue_capability_summary summary = evaluate_ntn_ue_capability(
      pack_nr_ue_capability(true, false, ue_nr_cap_v1700_s::ntn_scenario_support_r17_opts::ngso, false));

  EXPECT_EQ(summary.state, ntn_ue_capability_state::supported);
  EXPECT_EQ(summary.reason, "supported");
  EXPECT_FALSE(summary.ntn_scenario_support_r17);
  EXPECT_EQ(summary.ntn_scenario, "implicit_both");
  EXPECT_EQ(summary.scenario_support, ntn_ue_capability_scenario_support::implicit_both);
  EXPECT_TRUE(summary.matched_deployment_profile);
  EXPECT_EQ(summary.profile_block_reason, "profile_matched");
}

TEST(ntn_ue_capability_gate, nr_container_without_non_terrestrial_network_r17_is_unsupported)
{
  ntn_ue_capability_summary summary = evaluate_ntn_ue_capability(
      pack_nr_ue_capability(false, false, ue_nr_cap_v1700_s::ntn_scenario_support_r17_opts::ngso, false));

  EXPECT_EQ(summary.state, ntn_ue_capability_state::unsupported);
  EXPECT_EQ(summary.reason, "missing_non_terrestrial_network_r17");
  EXPECT_TRUE(summary.has_nr_container);
}

TEST(ntn_ue_capability_gate, no_nr_container_is_unsupported)
{
  ntn_ue_capability_summary summary = evaluate_ntn_ue_capability(pack_eutra_only_capability());

  EXPECT_EQ(summary.state, ntn_ue_capability_state::unsupported);
  EXPECT_EQ(summary.reason, "no_nr_container");
  EXPECT_FALSE(summary.has_nr_container);
}

TEST(ntn_ue_capability_gate, malformed_capability_returns_parse_failed)
{
  byte_buffer malformed;
  ASSERT_TRUE(malformed.append(0xff));
  ASSERT_TRUE(malformed.append(0x00));

  ntn_ue_capability_summary summary = evaluate_ntn_ue_capability(malformed);

  EXPECT_EQ(summary.state, ntn_ue_capability_state::parse_failed);
  EXPECT_EQ(summary.reason, "parse_failed");
}
