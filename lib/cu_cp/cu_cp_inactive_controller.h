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

#include "srsran/cu_cp/cu_cp_types.h"
#include "srsran/rrc/rrc_ue.h"
#include "srsran/srslog/srslog.h"

namespace srsran {
namespace srs_cu_cp {

/// \brief Result of a CU-CP-driven suspend attempt.
struct cu_cp_inactive_suspend_result {
  bool                   success = false;
  rrc_ue_release_context release_context; ///< Contains the RRCRelease(suspend) PDU when success == true.
};

/// \brief CU-CP helper that orchestrates the RRC_INACTIVE suspend path.
///
/// The controller is intentionally stateless and exposes a single entry point:
/// it asks the RRC UE object to build an RRCRelease(suspendConfig) and returns
/// the PDU back to the caller. Responsibilities deliberately *not* covered here:
///
///   - Sending the RRCRelease PDU to the DU over F1AP - the caller still owns
///     the F1AP DL RRC Message Transfer.
///   - Sending NGAP UEContextSuspendRequest to the AMF - use
///     ngap_ue_context_suspend_resume_helper for that.
///   - Removing the UE from the CU-CP UE manager once the suspend is complete -
///     the existing UE Context Release routine handles that, the inactive
///     context lives in rrc_inactive_context_repository.
///
/// Future iterations should grow this controller into an async_task that runs:
///   1. get_rrc_ue_inactive_release_context()
///   2. F1AP DL RRC Message Transfer (RRCRelease)
///   3. NGAP UEContextSuspendRequest
///   4. F1AP UE Context Release Command (release C-RNTI but keep gNB-CU UE ID)
///   5. CU-CP UE manager soft-release (mark UE as INACTIVE without freeing the slot)
class cu_cp_inactive_controller
{
public:
  /// \brief Trigger the suspend-to-inactive flow for a single UE.
  ///
  /// \param rrc_ue The RRC UE object to suspend. Must be in CONNECTED state.
  /// \param logger Logger used for diagnostic messages.
  /// \return A result object whose success flag indicates whether the
  ///         RRCRelease(suspend) PDU was generated successfully.
  static cu_cp_inactive_suspend_result trigger_suspend(rrc_ue_control_message_handler& rrc_ue,
                                                       srslog::basic_logger&           logger)
  {
    cu_cp_inactive_suspend_result result;
    result.release_context = rrc_ue.get_rrc_ue_inactive_release_context();
    result.success         = !result.release_context.rrc_release_pdu.empty();
    if (result.success) {
      logger.info("Suspend-to-inactive succeeded; RRCRelease(suspend) PDU is {} B",
                  result.release_context.rrc_release_pdu.length());
    } else {
      logger.warning("Suspend-to-inactive aborted: failed to build RRCRelease(suspend) PDU");
    }
    return result;
  }
};

} // namespace srs_cu_cp
} // namespace srsran
