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

#include "cu_cp_controller.h"
#include "../cu_up_processor/cu_up_processor_repository.h"
#include "../du_processor/du_processor_repository.h"
#include "../ue_manager/ue_manager_impl.h"
#include "srsran/ran/plmn_identity.h"

using namespace srsran;
using namespace srs_cu_cp;

cu_cp_controller::cu_cp_controller(const cu_cp_configuration&      config_,
                                   cu_cp_amf_reconnection_handler& cu_cp_notifier,
                                   common_task_scheduler&          common_task_sched_,
                                   ngap_repository&                ngaps_,
                                   cu_up_processor_repository&     cu_ups_,
                                   du_processor_repository&        dus_,
                                   ue_manager&                     ues_,
                                   task_executor&                  ctrl_exec_) :
  cfg(config_),
  ctrl_exec(ctrl_exec_),
  logger(srslog::fetch_basic_logger("CU-CP")),
  ue_mng(ues_),
  amf_mng(ngaps_, cu_cp_notifier, *cfg.services.timers, ctrl_exec_, common_task_sched_),
  du_mng(cfg.admission.max_nof_dus, dus_, ctrl_exec, common_task_sched_),
  cu_up_mng(cfg.admission.max_nof_cu_ups, cu_ups_, ctrl_exec, common_task_sched_)
{
}

void cu_cp_controller::stop()
{
  // Note: Called from separate outer thread.
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (not running) {
      return;
    }
  }

  // Stop and delete DU connections.
  du_mng.stop();

  // Stop and delete CU-UP connections.
  cu_up_mng.stop();

  // Stop and delete AMF connections.
  amf_mng.stop();
}

bool cu_cp_controller::handle_du_setup_request(du_index_t du_idx, const std::set<plmn_identity>& plmn_ids)
{
  bool success = false;
  for (const auto& plmn : plmn_ids) {
    if (amf_mng.is_amf_connected(plmn)) {
      success = true;
    } else {
      logger.debug("No AMF for PLMN={} is connected", plmn);
    }
  }

  // If AMF is not connected, it either means that the CU-CP is not operational state, there is a CU-CP failure or no
  // AMF for the PLMN of the DU cells was found.
  return success;
}

bool cu_cp_controller::request_ue_setup() const
{
  return request_ue_setup(cu_cp_admission_request_type::initial_access);
}

bool cu_cp_controller::request_ue_setup(cu_cp_admission_request_type request_type,
                                        unsigned                    additional_ues,
                                        unsigned                    additional_drbs) const
{
  if (!ue_admission_enabled.load(std::memory_order_relaxed)) {
    return false;
  }

  if (amf_mng.nof_amfs() == 0) {
    return false;
  }

  if (cu_up_mng.nof_cu_ups() == 0) {
    return false;
  }

  return is_below_admission_watermarks(request_type, additional_ues, additional_drbs);
}

void cu_cp_controller::set_ue_admission_enabled(bool enabled)
{
  bool old_value = ue_admission_enabled.exchange(enabled, std::memory_order_relaxed);
  if (old_value != enabled) {
    logger.info("UE admission {}", enabled ? "enabled" : "disabled");
  }
}

bool cu_cp_controller::is_ue_admission_enabled() const
{
  return ue_admission_enabled.load(std::memory_order_relaxed);
}

static const cu_cp_configuration::admission_params::load_watermark&
get_watermark(const cu_cp_configuration::admission_params& admission, cu_cp_admission_request_type request_type)
{
  switch (request_type) {
    case cu_cp_admission_request_type::initial_access:
      return admission.initial_access_watermark;
    case cu_cp_admission_request_type::reestablishment:
      return admission.reestablishment_watermark;
    case cu_cp_admission_request_type::handover:
      return admission.handover_watermark;
  }
  return admission.initial_access_watermark;
}

static bool is_usage_within_watermark(uint64_t projected, uint64_t maximum, unsigned watermark)
{
  return maximum != 0 && projected <= maximum && projected * 100U <= maximum * watermark;
}

bool cu_cp_controller::is_below_admission_watermarks(cu_cp_admission_request_type request_type,
                                                     unsigned                    additional_ues,
                                                     unsigned                    additional_drbs) const
{
  const auto& watermark    = get_watermark(cfg.admission, request_type);
  const auto  projected_ues = static_cast<uint64_t>(ue_mng.get_nof_ues()) + additional_ues;
  const auto  max_ues       = static_cast<uint64_t>(cfg.admission.max_nof_ues);
  if (!is_usage_within_watermark(projected_ues, max_ues, watermark.max_ue_usage)) {
    logger.debug("UE admission rejected. Cause: UE usage watermark exceeded. request_type={} projected_ues={} "
                 "max_ues={} watermark={}%",
                 static_cast<unsigned>(request_type),
                 projected_ues,
                 max_ues,
                 watermark.max_ue_usage);
    return false;
  }

  const auto projected_drbs = static_cast<uint64_t>(ue_mng.get_nof_drbs()) + additional_drbs;
  const auto max_drbs = static_cast<uint64_t>(cfg.admission.max_nof_ues) * cfg.admission.max_nof_drbs_per_ue;
  if (!is_usage_within_watermark(projected_drbs, max_drbs, watermark.max_drb_usage)) {
    logger.debug("UE admission rejected. Cause: DRB usage watermark exceeded. request_type={} projected_drbs={} "
                 "max_drbs={} watermark={}%",
                 static_cast<unsigned>(request_type),
                 projected_drbs,
                 max_drbs,
                 watermark.max_drb_usage);
    return false;
  }

  return true;
}

bool cu_cp_controller::is_supported_plmn(const plmn_identity& plmn) const
{
  return amf_mng.is_amf_connected(plmn);
}
