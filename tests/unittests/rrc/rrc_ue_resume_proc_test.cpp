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

#include "lib/rrc/ue/rrc_inactive_context_repository.h"
#include "rrc_ue_test_helpers.h"
#include "srsran/asn1/rrc_nr/nr_ue_variables.h"
#include "srsran/asn1/rrc_nr/rrc_nr.h"
#include "srsran/asn1/rrc_nr/ul_ccch_msg.h"
#include "srsran/asn1/rrc_nr/ul_dcch_msg.h"
#include "srsran/security/integrity.h"
#include <array>
#include <gtest/gtest.h>

using namespace srsran;
using namespace srs_cu_cp;

namespace {

uint16_t calculate_resume_mac_i(const rrc_inactive_ue_context& stored, nr_cell_identity target_cell_id)
{
  asn1::rrc_nr::var_resume_mac_input_s var_input;
  var_input.source_pci = stored.old_pci;
  var_input.target_cell_id.from_number(target_cell_id.value());
  var_input.source_c_rnti = to_value(stored.old_c_rnti);

  byte_buffer   packed;
  asn1::bit_ref bref(packed);
  EXPECT_EQ(var_input.pack(bref), asn1::SRSASN_SUCCESS);

  security::sec_as_config source_as = stored.transfer_context.sec_context.get_as_config(security::sec_domain::rrc);
  EXPECT_TRUE(source_as.k_int.has_value());
  EXPECT_TRUE(source_as.integ_algo.has_value());

  security::sec_mac mac = {};
  if (source_as.integ_algo != security::integrity_algorithm::nia0) {
    security::sec_128_key key = security::truncate_key(source_as.k_int.value());
    byte_buffer_view      packed_view{packed};
    switch (source_as.integ_algo.value()) {
      case security::integrity_algorithm::nia1:
        security::security_nia1(mac, key, 0xffffffff, 0x1f, security::security_direction::downlink, packed_view);
        break;
      case security::integrity_algorithm::nia2:
        security::security_nia2(mac, key, 0xffffffff, 0x1f, security::security_direction::downlink, packed_view);
        break;
      case security::integrity_algorithm::nia3:
        security::security_nia3(mac, key, 0xffffffff, 0x1f, security::security_direction::downlink, packed_view);
        break;
      default:
        break;
    }
  }

  return (static_cast<uint16_t>(mac[2]) << 8U) | static_cast<uint16_t>(mac[3]);
}

byte_buffer generate_rrc_resume_request_pdu(uint32_t                                short_i_rnti,
                                            uint16_t                                resume_mac_i,
                                            asn1::rrc_nr::resume_cause_opts::options cause)
{
  byte_buffer            pdu;
  asn1::bit_ref          bref{pdu};
  asn1::rrc_nr::ul_ccch_msg_s ul_ccch_msg{};

  auto& resume = ul_ccch_msg.msg.set_c1().set_rrc_resume_request().rrc_resume_request;
  resume.resume_id.from_number(short_i_rnti);
  resume.resume_mac_i.from_number(resume_mac_i);
  resume.resume_cause = cause;
  resume.spare.from_number(0);

  EXPECT_EQ(ul_ccch_msg.pack(bref), asn1::SRSASN_SUCCESS);
  return pdu;
}

byte_buffer generate_rrc_resume_complete_pdu(uint8_t transaction_id)
{
  byte_buffer            pdu;
  asn1::bit_ref          bref{pdu};
  asn1::rrc_nr::ul_dcch_msg_s ul_dcch_msg{};

  auto& complete              = ul_dcch_msg.msg.set_c1().set_rrc_resume_complete();
  complete.rrc_transaction_id = transaction_id;
  complete.crit_exts.set_rrc_resume_complete();

  EXPECT_EQ(ul_dcch_msg.pack(bref), asn1::SRSASN_SUCCESS);
  return pdu;
}

byte_buffer make_srb1_ul_pdcp_pdu(uint16_t pdcp_sn, byte_buffer rrc_sdu, security::sec_128_as_config sec_cfg)
{
  byte_buffer pdcp_pdu;
  srsran_assert(pdcp_pdu.append(static_cast<uint8_t>((pdcp_sn >> 8U) & 0xffU)), "Failed to append PDCP header");
  srsran_assert(pdcp_pdu.append(static_cast<uint8_t>(pdcp_sn & 0xffU)), "Failed to append PDCP header");
  srsran_assert(pdcp_pdu.append(std::move(rrc_sdu)), "Failed to append RRC SDU");

  srsran_assert(sec_cfg.k_128_int.has_value(), "RRCResumeComplete test requires an RRC integrity key");
  srsran_assert(sec_cfg.integ_algo.has_value(), "RRCResumeComplete test requires an integrity algorithm");
  srsran_assert(sec_cfg.cipher_algo == security::ciphering_algorithm::nea0,
                "RRCResumeComplete test helper does not cipher the RRC SDU");

  security::sec_mac mac = {};
  byte_buffer_view  integrity_input{pdcp_pdu};
  switch (sec_cfg.integ_algo.value()) {
    case security::integrity_algorithm::nia0:
      std::fill(mac.begin(), mac.end(), 0);
      break;
    case security::integrity_algorithm::nia1:
      security::security_nia1(mac,
                              sec_cfg.k_128_int.value(),
                              pdcp_sn,
                              0,
                              security::security_direction::uplink,
                              integrity_input);
      break;
    case security::integrity_algorithm::nia2:
      security::security_nia2(mac,
                              sec_cfg.k_128_int.value(),
                              pdcp_sn,
                              0,
                              security::security_direction::uplink,
                              integrity_input);
      break;
    case security::integrity_algorithm::nia3:
      security::security_nia3(mac,
                              sec_cfg.k_128_int.value(),
                              pdcp_sn,
                              0,
                              security::security_direction::uplink,
                              integrity_input);
      break;
    default:
      break;
  }

  srsran_assert(pdcp_pdu.append(mac), "Failed to append MAC-I");
  return pdcp_pdu;
}

} // namespace

class rrc_ue_resume : public rrc_ue_test_helper, public ::testing::Test
{
protected:
  static void SetUpTestSuite() { srslog::init(); }

  void SetUp() override { init(); }

  void TearDown() override
  {
    for (uint64_t full_i_rnti : stored_full_i_rntis) {
      rrc_inactive_context_repository::get_instance().remove(full_i_rnti);
    }
    srslog::flush();
  }

  rrc_ue_release_context suspend_connected_ue()
  {
    receive_setup_request();
    receive_setup_complete();
    init_security_context();

    rrc_ue_release_context release_context =
        rrc_ue->get_rrc_ue_control_message_handler().get_rrc_ue_inactive_release_context();
    EXPECT_FALSE(release_context.rrc_release_pdu.empty());
    EXPECT_TRUE(release_context.full_i_rnti.has_value());
    stored_full_i_rntis.push_back(release_context.full_i_rnti.value());
    return release_context;
  }

  byte_buffer make_valid_resume_request(const rrc_ue_release_context& release_context,
                                        asn1::rrc_nr::resume_cause_opts::options cause)
  {
    const auto stored = rrc_inactive_context_repository::get_instance().lookup_full(release_context.full_i_rnti.value());
    EXPECT_TRUE(stored.has_value());
    const uint16_t resume_mac_i = calculate_resume_mac_i(stored.value(), stored.value().cell.nci);
    return generate_rrc_resume_request_pdu(release_context.short_i_rnti.value(), resume_mac_i, cause);
  }

  std::vector<uint64_t> stored_full_i_rntis;
};

TEST_F(rrc_ue_resume, rrc_resume_request_for_known_inactive_ue_sends_native_rrc_resume)
{
  const rrc_ue_release_context release_context = suspend_connected_ue();

  rrc_ue_f1ap_notifier.reset();
  rrc_ue->get_ul_pdu_handler().handle_ul_ccch_pdu(
      make_valid_resume_request(release_context, asn1::rrc_nr::resume_cause_opts::options::mo_sig));

  EXPECT_EQ(rrc_ue_f1ap_notifier.nof_rrc_pdus, 1U);
  EXPECT_EQ(rrc_ue_f1ap_notifier.last_srb_id, srb_id_t::srb1);
  EXPECT_EQ(rrc_ue_cu_cp_notifier.nof_rrc_resume_requests, 0U);
  check_ue_release_not_requested();
}

TEST_F(rrc_ue_resume, rrc_resume_complete_triggers_connected_resume_notification)
{
  const rrc_ue_release_context release_context = suspend_connected_ue();

  rrc_ue_f1ap_notifier.reset();
  rrc_ue->get_ul_pdu_handler().handle_ul_ccch_pdu(
      make_valid_resume_request(release_context, asn1::rrc_nr::resume_cause_opts::options::mo_sig));
  ASSERT_EQ(rrc_ue_f1ap_notifier.nof_rrc_pdus, 1U);
  ASSERT_EQ(rrc_ue_cu_cp_notifier.nof_rrc_resume_requests, 0U);

  security::sec_128_as_config resume_sec_cfg =
      ue_mng.find_ue(allocated_ue_index)->get_security_manager().get_rrc_128_as_config();
  rrc_ue->get_ul_pdu_handler().handle_ul_dcch_pdu(
      srb_id_t::srb1, make_srb1_ul_pdcp_pdu(1, generate_rrc_resume_complete_pdu(1), resume_sec_cfg));

  EXPECT_EQ(rrc_ue_cu_cp_notifier.nof_rrc_resume_requests, 1U);
  ASSERT_TRUE(rrc_ue_cu_cp_notifier.last_rrc_resume_old_ue_index.has_value());
  EXPECT_EQ(rrc_ue_cu_cp_notifier.last_rrc_resume_old_ue_index.value(), allocated_ue_index);
  ASSERT_TRUE(rrc_ue_cu_cp_notifier.last_rrc_resume_cause.has_value());
  EXPECT_EQ(rrc_ue_cu_cp_notifier.last_rrc_resume_cause.value(), establishment_cause_t::mo_sig);
  check_ue_release_not_requested();
}

TEST_F(rrc_ue_resume, rnau_refresh_sends_fresh_suspend_without_connected_resume_notification)
{
  const rrc_ue_release_context release_context = suspend_connected_ue();

  rrc_ue_f1ap_notifier.reset();
  rrc_ue->get_ul_pdu_handler().handle_ul_ccch_pdu(
      make_valid_resume_request(release_context, asn1::rrc_nr::resume_cause_opts::options::rna_upd));

  EXPECT_EQ(rrc_ue_f1ap_notifier.nof_rrc_pdus, 1U);
  EXPECT_EQ(rrc_ue_f1ap_notifier.last_srb_id, srb_id_t::srb1);
  EXPECT_EQ(rrc_ue_cu_cp_notifier.nof_rrc_resume_requests, 0U);
  check_ue_release_not_requested();
}
