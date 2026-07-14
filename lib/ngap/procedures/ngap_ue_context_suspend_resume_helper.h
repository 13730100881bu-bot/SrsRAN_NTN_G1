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

#include "srsran/asn1/ngap/ngap_pdu_contents.h"
#include "srsran/ngap/ngap.h"
#include "srsran/ngap/ngap_message.h"
#include "srsran/ngap/ngap_types.h"
#include "srsran/ran/cu_types.h"

namespace srsran {
namespace srs_cu_cp {

/// \brief Helpers for the NGAP UE Context Suspend/Resume procedures (TS 38.413
/// Sec 8.7.x), used by the RRC_INACTIVE flow.
///
/// This file intentionally provides only the *initiating* message builders.
/// Wiring the response paths (UEContextSuspendResponse / UEContextResumeFailure /
/// UEContextResumeRequest from the AMF) requires extending ngap_impl::handle_message
/// and adding entries to the ngap_transaction_manager. The MVP fires the request
/// once and lets the AMF response either be ignored or routed via the existing
/// generic handler so the build stays green.
class ngap_ue_context_suspend_resume_helper
{
public:
  /// Build a UE Context Suspend Request (initiating message) ready to be sent
  /// to the AMF. The PDU session list is intentionally left empty so the AMF
  /// suspends all sessions of the UE.
  static ngap_message build_ue_context_suspend_request(amf_ue_id_t amf_ue_id, ran_ue_id_t ran_ue_id)
  {
    ngap_message msg = {};
    msg.pdu.set_init_msg();
    msg.pdu.init_msg().load_info_obj(ASN1_NGAP_ID_UE_CONTEXT_SUSPEND);

    auto& ies          = msg.pdu.init_msg().value.ue_context_suspend_request();
    ies->amf_ue_ngap_id = amf_ue_id_to_uint(amf_ue_id);
    ies->ran_ue_ngap_id = ran_ue_id_to_uint(ran_ue_id);
    return msg;
  }

  /// Build a UE Context Resume Request (initiating message). The caller passes
  /// the original RRC establishment cause that maps to the UE's resume cause.
  static ngap_message build_ue_context_resume_request(amf_ue_id_t                          amf_ue_id,
                                                      ran_ue_id_t                          ran_ue_id,
                                                      asn1::ngap::rrc_establishment_cause_e rrc_resume_cause)
  {
    ngap_message msg = {};
    msg.pdu.set_init_msg();
    msg.pdu.init_msg().load_info_obj(ASN1_NGAP_ID_UE_CONTEXT_RESUME);

    auto& ies          = msg.pdu.init_msg().value.ue_context_resume_request();
    ies->amf_ue_ngap_id = amf_ue_id_to_uint(amf_ue_id);
    ies->ran_ue_ngap_id = ran_ue_id_to_uint(ran_ue_id);
    ies->rrc_resume_cause = rrc_resume_cause;
    return msg;
  }

  /// Send a UE Context Suspend Request to the AMF via the given notifier.
  /// Returns false if the notifier rejects the message.
  static bool send_ue_context_suspend_request(ngap_message_notifier& amf_notifier,
                                              amf_ue_id_t            amf_ue_id,
                                              ran_ue_id_t            ran_ue_id)
  {
    return amf_notifier.on_new_message(build_ue_context_suspend_request(amf_ue_id, ran_ue_id));
  }

  /// Send a UE Context Resume Request to the AMF via the given notifier.
  static bool send_ue_context_resume_request(ngap_message_notifier&                amf_notifier,
                                             amf_ue_id_t                           amf_ue_id,
                                             ran_ue_id_t                           ran_ue_id,
                                             asn1::ngap::rrc_establishment_cause_e rrc_resume_cause)
  {
    return amf_notifier.on_new_message(build_ue_context_resume_request(amf_ue_id, ran_ue_id, rrc_resume_cause));
  }
};

} // namespace srs_cu_cp
} // namespace srsran
