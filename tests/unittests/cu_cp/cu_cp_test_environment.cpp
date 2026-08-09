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

#include "cu_cp_test_environment.h"
#include "tests/test_doubles/e1ap/e1ap_test_message_validators.h"
#include "tests/test_doubles/f1ap/f1ap_test_message_validators.h"
#include "tests/test_doubles/ngap/ngap_test_message_validators.h"
#include "tests/test_doubles/rrc/rrc_test_message_validators.h"
#include "tests/test_doubles/rrc/rrc_test_messages.h"
#include "tests/unittests/cu_cp/test_doubles/mock_cu_up.h"
#include "tests/unittests/cu_cp/test_helpers.h"
#include "tests/unittests/e1ap/common/e1ap_cu_cp_test_messages.h"
#include "tests/unittests/ngap/ngap_test_messages.h"
#include "srsran/asn1/f1ap/common.h"
#include "srsran/asn1/f1ap/f1ap_pdu_contents.h"
#include "srsran/asn1/f1ap/f1ap_pdu_contents_ue.h"
#include <algorithm>
#include "srsran/asn1/ngap/ngap_pdu_contents.h"
#include "srsran/asn1/rrc_nr/dl_ccch_msg.h"
#include "srsran/asn1/rrc_nr/ul_dcch_msg_ies.h"
#include "srsran/cu_cp/cell_meas_manager_config.h"
#include "srsran/cu_cp/cu_cp_configuration_helpers.h"
#include "srsran/cu_cp/cu_cp_factory.h"
#include "srsran/cu_cp/cu_cp_types.h"
#include "srsran/e1ap/common/e1ap_message.h"
#include "srsran/e1ap/common/e1ap_types.h"
#include "srsran/f1ap/f1ap_message.h"
#include "srsran/f1ap/ntn_access_calendar.h"
#include "srsran/f1ap/ntn_rnti_lease_pool.h"
#include "srsran/ngap/ngap_message.h"
#include "srsran/ran/cu_types.h"
#include "srsran/ran/plmn_identity.h"
#include "srsran/support/executors/task_worker.h"

using namespace srsran;
using namespace srs_cu_cp;

class cu_cp_test_environment::worker_manager
{
  const unsigned WORKER_QUEUE_SIZE = 1024;

public:
  void stop() { worker.stop(); }

  void wait_pending_tasks() { worker.wait_pending_tasks(); }

  task_worker                    worker{"cu_cp_worker", WORKER_QUEUE_SIZE};
  std::unique_ptr<task_executor> exec{std::make_unique<task_worker_executor>(worker)};
};

// ////

static bool is_gnb_du_resource_coordination_request(const f1ap_message& pdu)
{
  return pdu.pdu.type().value == asn1::f1ap::f1ap_pdu_c::types_opts::init_msg &&
         pdu.pdu.init_msg().value.type().value ==
             asn1::f1ap::f1ap_elem_procs_o::init_msg_c::types_opts::gnb_du_res_coordination_request;
}

static bool is_ntn_access_calendar_query(const f1ap_message& pdu)
{
  if (!is_gnb_du_resource_coordination_request(pdu)) {
    return false;
  }
  const auto& request = pdu.pdu.init_msg().value.gnb_du_res_coordination_request();
  const auto  update  =
      decode_f1ap_ntn_access_calendar_update(request->eutra_nr_cell_res_coordination_req_container);
  return update.has_value() && update->operation == f1ap_ntn_access_calendar_operation::query;
}

static void record_ntn_calendar_prepare(const f1ap_message&      request,
                                        bool                     preflight_incomplete,
                                        std::array<uint16_t, 2>& last_ntn_calendar_intents_per_cell,
                                        std::array<f1ap_ntn_access_calendar_preflight_report, 2>&
                                            last_ntn_calendar_preflight_reports)
{
  if (!is_gnb_du_resource_coordination_request(request)) {
    return;
  }

  const auto& asn1_request = request.pdu.init_msg().value.gnb_du_res_coordination_request();
  const auto  update =
      decode_f1ap_ntn_access_calendar_update(asn1_request->eutra_nr_cell_res_coordination_req_container);
  if (!update.has_value() || update->operation != f1ap_ntn_access_calendar_operation::prepare) {
    return;
  }

  last_ntn_calendar_intents_per_cell = {};
  last_ntn_calendar_preflight_reports = {};
  for (unsigned i = 0; i != update->cells.size() && i != last_ntn_calendar_intents_per_cell.size(); ++i) {
    last_ntn_calendar_intents_per_cell[i] = static_cast<uint16_t>(update->cells[i].intents.size());
    auto& report       = last_ntn_calendar_preflight_reports[i];
    report.performed   = !preflight_incomplete || i != 0;
    report.passed      = report.performed;
    report.numerology  = report.performed ? 1U : 0xffU;
    if (!report.performed) {
      continue;
    }
    for (const f1ap_ntn_access_calendar_intent& intent : update->cells[i].intents) {
      if (intent.purpose == f1ap_ntn_access_calendar_purpose::ssb_sib_paging ||
          intent.purpose == f1ap_ntn_access_calendar_purpose::ssb_sib_paging_rar) {
        ++report.expected_ssb;
        ++report.matched_ssb;
      } else if (intent.purpose == f1ap_ntn_access_calendar_purpose::prach_ro) {
        ++report.expected_prach;
        ++report.matched_prach;
      }
    }
    report.max_ssb_gap_slots   = report.expected_ssb == 0 ? 0 : 160;
    report.max_prach_gap_slots = report.expected_prach == 0 ? 0 : 1280;
  }
}

static f1ap_message make_gnb_du_resource_coordination_response(
    const f1ap_message&                                       request,
    bool                                                      ntn_calendar_query_stays_ready,
    bool                                                      ntn_calendar_query_reports_applied_early,
    bool                                                      ntn_calendar_query_reports_zero_intents,
    bool                                                      ntn_calendar_prepare_rejects,
    bool                                                      ntn_calendar_prepare_reports_ready,
    bool                                                      ntn_calendar_preflight_incomplete,
    bool                                                      ntn_calendar_preflight_unsupported,
    bool                                                      ntn_resource_audit_rejects,
    bool                                                      ntn_resource_audit_rnti_snapshot_complete,
    bool                                                      ntn_resource_audit_retirement_supported,
    bool                                                      ntn_resource_audit_legacy_only,
    bool                                                      ntn_resource_audit_ue_slot_identity_supported,
    bool                                                      ntn_resource_audit_ue_slot_snapshot_complete,
    bool                                                      ntn_rnti_retirement_rejects,
    uint32_t&                                                 ntn_resource_audit_rnti_generation_high_water,
    uint32_t&                                                 ntn_resource_audit_ue_slot_generation_high_water,
    std::vector<f1ap_ntn_resource_audit_rnti_lease>&          ntn_resource_audit_rnti_leases,
    std::vector<f1ap_ntn_resource_audit_ue_slot>&             ntn_resource_audit_ue_slots,
    std::array<uint16_t, 2>&                                  last_ntn_calendar_intents_per_cell,
    std::array<f1ap_ntn_access_calendar_preflight_report, 2>& last_ntn_calendar_preflight_reports,
    unsigned&                                                 ntn_calendar_clear_requests)
{
  record_ntn_calendar_prepare(request,
                              ntn_calendar_preflight_incomplete,
                              last_ntn_calendar_intents_per_cell,
                              last_ntn_calendar_preflight_reports);
  const auto& asn1_req = request.pdu.init_msg().value.gnb_du_res_coordination_request();

  const std::optional<f1ap_ntn_rnti_lease_pool_update> update =
      decode_f1ap_ntn_rnti_lease_pool_update(asn1_req->eutra_nr_cell_res_coordination_req_container);
  const std::optional<f1ap_ntn_access_calendar_update> calendar_update =
      decode_f1ap_ntn_access_calendar_update(asn1_req->eutra_nr_cell_res_coordination_req_container);
  const std::optional<f1ap_ntn_sib19_broadcast_update> sib19_update =
      decode_f1ap_ntn_sib19_broadcast_update(asn1_req->eutra_nr_cell_res_coordination_req_container);
  const std::optional<f1ap_ntn_resource_audit_request> audit_request =
      decode_f1ap_ntn_resource_audit_request(asn1_req->eutra_nr_cell_res_coordination_req_container);

  byte_buffer response_container;
  if (calendar_update.has_value()) {
    if (calendar_update->operation == f1ap_ntn_access_calendar_operation::clear) {
      ++ntn_calendar_clear_requests;
    }
    f1ap_ntn_access_calendar_result result;
    result.catalog_version     = calendar_update->catalog_version;
    result.schedule_version    = calendar_update->schedule_version;
    result.source_content_hash = calendar_update->source_content_hash;
    result.calendar_hash       = calendar_update->calendar_hash;
    const uint64_t now_unix_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                           std::chrono::system_clock::now().time_since_epoch())
                                                           .count());
    const bool prepare_preflight_unsupported =
        calendar_update->operation == f1ap_ntn_access_calendar_operation::prepare &&
        ntn_calendar_preflight_unsupported;
    result.status = calendar_update->operation == f1ap_ntn_access_calendar_operation::prepare
                        ? (ntn_calendar_preflight_unsupported
                               ? f1ap_ntn_access_calendar_result_status::unsupported
                           : ntn_calendar_preflight_incomplete || ntn_calendar_prepare_rejects
                               ? f1ap_ntn_access_calendar_result_status::rejected
                           : ntn_calendar_prepare_reports_ready ? f1ap_ntn_access_calendar_result_status::ready
                                                                : f1ap_ntn_access_calendar_result_status::preparing)
                    : calendar_update->operation == f1ap_ntn_access_calendar_operation::query
                        ? (!ntn_calendar_query_stays_ready &&
                                   (ntn_calendar_query_reports_applied_early ||
                                    now_unix_ms >= calendar_update->activation_epoch_unix_ms)
                               ? f1ap_ntn_access_calendar_result_status::applied
                               : f1ap_ntn_access_calendar_result_status::ready)
                        : f1ap_ntn_access_calendar_result_status::cleared;
    result.reject_reason = prepare_preflight_unsupported ? "scheduler_preflight_not_performed"
                           : ntn_calendar_preflight_incomplete ? "scheduler_preflight_not_performed"
                           : result.status == f1ap_ntn_access_calendar_result_status::rejected ? "rejected_by_mock_du"
                           : result.status == f1ap_ntn_access_calendar_result_status::applied
                               ? "ssb_prach_software_gate_applied_no_position_or_rf_evidence"
                           : result.status == f1ap_ntn_access_calendar_result_status::preparing
                               ? "waiting_for_both_cell_slot_threads_to_arm"
                               : "accepted_by_mock_du";
    if (calendar_update->operation == f1ap_ntn_access_calendar_operation::query &&
        ntn_calendar_query_reports_zero_intents) {
      result.accepted_intents_per_cell = {};
    } else {
      result.accepted_intents_per_cell = last_ntn_calendar_intents_per_cell;
    }
    if (ntn_calendar_preflight_incomplete ||
        result.status == f1ap_ntn_access_calendar_result_status::preparing ||
        result.status == f1ap_ntn_access_calendar_result_status::ready ||
        result.status == f1ap_ntn_access_calendar_result_status::applied) {
      result.preflight_reports = last_ntn_calendar_preflight_reports;
    }
    response_container = encode_f1ap_ntn_access_calendar_result(result);
  } else if (update.has_value()) {
    f1ap_ntn_rnti_lease_pool_result result;
    result.generation_id   = update->generation_id;
    const bool retirement_rejected =
        update->operation == f1ap_ntn_rnti_lease_pool_operation::retire && ntn_rnti_retirement_rejects;
    result.accepted        = !retirement_rejected;
    result.accepted_leases = retirement_rejected ? std::vector<rnti_t>{} : update->leases;
    result.rejected_leases = retirement_rejected ? update->leases : std::vector<rnti_t>{};
    result.reject_reason   = retirement_rejected ? "retirement_rejected_by_mock_du" : "accepted_by_mock_du";
    if (result.accepted) {
      ntn_resource_audit_rnti_generation_high_water =
          std::max(ntn_resource_audit_rnti_generation_high_water, update->generation_id);
      if (update->operation == f1ap_ntn_rnti_lease_pool_operation::retire) {
        for (rnti_t retired : update->leases) {
          ntn_resource_audit_rnti_leases.erase(std::remove_if(ntn_resource_audit_rnti_leases.begin(),
                                                              ntn_resource_audit_rnti_leases.end(),
                                                              [retired, &update](const auto& lease) {
                                                                return lease.rnti == retired &&
                                                                       lease.generation_id == update->generation_id;
                                                              }),
                                               ntn_resource_audit_rnti_leases.end());
        }
      } else if (update->operation == f1ap_ntn_rnti_lease_pool_operation::add ||
                 update->operation == f1ap_ntn_rnti_lease_pool_operation::replace) {
        if (update->operation == f1ap_ntn_rnti_lease_pool_operation::replace) {
          ntn_resource_audit_rnti_leases.clear();
        }
        for (rnti_t lease : update->leases) {
          ntn_resource_audit_rnti_leases.push_back({lease, "pending", "applied_by_du", update->generation_id});
        }
      } else if (update->operation == f1ap_ntn_rnti_lease_pool_operation::clear) {
        ntn_resource_audit_rnti_leases.clear();
      }
    }
    response_container     = encode_f1ap_ntn_rnti_lease_pool_result(result);
  } else if (sib19_update.has_value()) {
    f1ap_ntn_sib19_broadcast_result result;
    result.generation_id = sib19_update->generation_id;
    result.status        = sib19_update->operation == f1ap_ntn_sib19_broadcast_operation::clear
                               ? f1ap_ntn_sib19_broadcast_result_status::clear_applied
                               : f1ap_ntn_sib19_broadcast_result_status::applied;
    result.reject_reason = sib19_update->operation == f1ap_ntn_sib19_broadcast_operation::clear ? "cleared" : "applied";
    response_container   = encode_f1ap_ntn_sib19_broadcast_result(result);
  } else if (audit_request.has_value()) {
    if ((audit_request->request_ue_slot_identity || audit_request->request_retirement_metadata) &&
        ntn_resource_audit_legacy_only) {
      f1ap_ntn_rnti_lease_pool_result result;
      result.accepted      = false;
      result.reject_reason = "malformed_ntn_rnti_lease_pool_update";
      response_container   = encode_f1ap_ntn_rnti_lease_pool_result(result);
    } else {
      f1ap_ntn_resource_audit_result result;
      result.generation_id               = audit_request->generation_id;
      result.accepted                    = !ntn_resource_audit_rejects;
      result.rnti_snapshot_complete      = ntn_resource_audit_rnti_snapshot_complete;
      result.ue_slot_snapshot_complete   = audit_request->request_ue_slot_identity &&
                                           ntn_resource_audit_ue_slot_identity_supported &&
                                           ntn_resource_audit_ue_slot_snapshot_complete;
      result.retirement_metadata_present = audit_request->request_retirement_metadata;
      result.retire_supported = audit_request->request_retirement_metadata && ntn_resource_audit_retirement_supported;
      result.rnti_generation_high_water = ntn_resource_audit_rnti_generation_high_water;
      result.rnti_leases                = ntn_resource_audit_rnti_leases;
      if (audit_request->request_ue_slot_identity) {
        result.ue_slot_identity_metadata_present = true;
        result.ue_slot_identity_supported        = ntn_resource_audit_ue_slot_identity_supported;
        result.gnb_du_id                         = audit_request->gnb_du_id;
        result.du_index                          = audit_request->du_index;
        result.cell_index                        = audit_request->cell_index;
        result.cell_cgi                          = audit_request->cell_cgi;
        result.pci                               = audit_request->pci;
        result.connection_token                  = audit_request->connection_token;
        result.ue_slot_assignment_generation_high_water =
            ntn_resource_audit_ue_slot_generation_high_water;
        result.ue_slots = ntn_resource_audit_ue_slots;
        for (f1ap_ntn_resource_audit_ue_slot& slot : result.ue_slots) {
          // The mock ledger stores UE identity and the applied resources. Cell identity is bound to the
          // connection-scoped audit target, just as the real DU manager does when it joins its resource registry
          // with the current F1 UE context.
          slot.cell_index = audit_request->cell_index;
          slot.cell_cgi   = audit_request->cell_cgi;
          slot.pci        = audit_request->pci;
        }
      }
      result.reject_reason = ntn_resource_audit_rejects                  ? "rejected_by_mock_du"
                             : ntn_resource_audit_rnti_snapshot_complete ? "accepted_by_mock_du"
                                                                         : "snapshot_unavailable_in_mock_du";
      response_container   = encode_f1ap_ntn_resource_audit_result(result);
    }
  } else {
    f1ap_ntn_rnti_lease_pool_result result;
    result.accepted      = false;
    result.reject_reason = "malformed_mock_du_request";
    response_container   = encode_f1ap_ntn_rnti_lease_pool_result(result);
  }

  f1ap_message response;
  response.pdu.set_successful_outcome().load_info_obj(ASN1_F1AP_ID_GNB_DU_RES_COORDINATION);
  auto& asn1_resp = response.pdu.successful_outcome().value.gnb_du_res_coordination_resp();
  asn1_resp->transaction_id = asn1_req->transaction_id;
  asn1_resp->eutra_nr_cell_res_coordination_req_ack_container = std::move(response_container);
  return response;
}

cu_cp_test_environment::cu_cp_test_environment(cu_cp_test_env_params params_) :
  params(std::move(params_)),
  cu_cp_workers(std::make_unique<worker_manager>()),
  timers(64),
  amf_configs(std::move(params.amf_configs))
{
  if (params.ntn_recovered_calendar_intents_per_cell.has_value()) {
    last_ntn_calendar_intents_per_cell = *params.ntn_recovered_calendar_intents_per_cell;
  }
  if (params.ntn_recovered_calendar_preflight_reports.has_value()) {
    last_ntn_calendar_preflight_reports = *params.ntn_recovered_calendar_preflight_reports;
  }
  ntn_resource_audit_rnti_generation_high_water = params.ntn_resource_audit_rnti_generation_high_water;
  ntn_resource_audit_ue_slot_generation_high_water = params.ntn_resource_audit_ue_slot_generation_high_water;
  ntn_resource_audit_rnti_leases                = params.ntn_resource_audit_rnti_leases;
  ntn_resource_audit_ue_slots                   = params.ntn_resource_audit_ue_slots;
  // Initialize logging
  test_logger.set_level(srslog::basic_levels::debug);
  cu_cp_logger.set_level(srslog::basic_levels::debug);
  srslog::fetch_basic_logger("NRPPA").set_level(srslog::basic_levels::debug);
  srslog::fetch_basic_logger("PDCP").set_level(srslog::basic_levels::info);
  srslog::fetch_basic_logger("NGAP").set_hex_dump_max_size(32);
  srslog::fetch_basic_logger("RRC").set_hex_dump_max_size(32);
  srslog::fetch_basic_logger("SEC").set_hex_dump_max_size(32);
  srslog::init();

  // Create CU-CP config
  cu_cp_cfg                               = config_helpers::make_default_cu_cp_config();
  cu_cp_cfg.services.cu_cp_executor       = cu_cp_workers->exec.get();
  cu_cp_cfg.services.timers               = &timers;
  cu_cp_cfg.admission.max_nof_dus         = params.max_nof_dus;
  cu_cp_cfg.admission.max_nof_cu_ups      = params.max_nof_cu_ups;
  cu_cp_cfg.admission.max_nof_ues         = params.max_nof_ues;
  cu_cp_cfg.admission.max_nof_drbs_per_ue = params.max_nof_drbs_per_ue;
  cu_cp_cfg.admission.initial_access_watermark  = params.initial_access_watermark;
  cu_cp_cfg.admission.reestablishment_watermark = params.reestablishment_watermark;
  cu_cp_cfg.admission.handover_watermark        = params.handover_watermark;
  cu_cp_cfg.bearers.drb_config            = config_helpers::make_default_cu_cp_qos_config_list();
  // > NGAP config
  for (const auto& [amf_index, amf_config] : amf_configs) {
    cu_cp_cfg.ngap.ngaps.push_back(cu_cp_configuration::ngap_config{&*amf_config.amf_stub, amf_config.supported_tas});
  }
  // > Security config.
  cu_cp_cfg.security.int_algo_pref_list = {security::integrity_algorithm::nia2,
                                           security::integrity_algorithm::nia1,
                                           security::integrity_algorithm::nia3,
                                           security::integrity_algorithm::nia0};
  cu_cp_cfg.security.enc_algo_pref_list = {security::ciphering_algorithm::nea0,
                                           security::ciphering_algorithm::nea2,
                                           security::ciphering_algorithm::nea1,
                                           security::ciphering_algorithm::nea3};

  // > Logging and metrics config.
  cu_cp_cfg.f1ap.json_log_enabled          = true;
  cu_cp_cfg.e1ap.json_log_enabled          = true;
  cu_cp_cfg.metrics.layers_cfg.enable_ngap = true;
  cu_cp_cfg.metrics.layers_cfg.enable_rrc  = true;

  // > Mobility config
  cu_cp_cfg.mobility.mobility_manager_config.trigger_handover_from_measurements = params.trigger_ho_from_measurements;
  if (params.ntn_onboard_position_plan.has_value()) {
    cu_cp_cfg.mobility.onboard_position_plan = *params.ntn_onboard_position_plan;
  }
  {
    // > Meas manager config
    cell_meas_manager_cfg meas_mng_cfg;
    {
      // Generate NCIs.
      gnb_id_t         gnb_id1 = cu_cp_cfg.node.gnb_id;
      nr_cell_identity nci1    = nr_cell_identity::create(gnb_id1, 0).value();
      nr_cell_identity nci2    = nr_cell_identity::create(gnb_id1, 1).value();
      gnb_id_t         gnb_id2 = {cu_cp_cfg.node.gnb_id.id + 1, cu_cp_cfg.node.gnb_id.bit_length};
      nr_cell_identity nci3    = nr_cell_identity::create(gnb_id2, 0).value();

      // Cell 1
      {
        cell_meas_config cell_cfg_1;
        cell_cfg_1.periodic_report_cfg_id             = uint_to_report_cfg_id(1);
        cell_cfg_1.serving_cell_cfg.gnb_id_bit_length = gnb_id1.bit_length;
        cell_cfg_1.serving_cell_cfg.nci               = nci1;
        cell_cfg_1.ncells.push_back({nci2, {uint_to_report_cfg_id(2)}});
        // Add external cell (for inter CU handover tests)
        cell_cfg_1.ncells.push_back({nci3, {uint_to_report_cfg_id(2)}});

        meas_mng_cfg.cells.emplace(nci1, cell_cfg_1);
      }

      // Cell 2
      {
        cell_meas_config cell_cfg_2;
        cell_cfg_2.periodic_report_cfg_id             = uint_to_report_cfg_id(1);
        cell_cfg_2.serving_cell_cfg.gnb_id_bit_length = gnb_id1.bit_length;
        cell_cfg_2.serving_cell_cfg.nci               = nci2;
        cell_cfg_2.ncells.push_back({nci1, {uint_to_report_cfg_id(2)}});
        meas_mng_cfg.cells.emplace(nci2, cell_cfg_2);
      }

      // Add an external cell
      {
        cell_meas_config cell_cfg_3;
        cell_cfg_3.periodic_report_cfg_id             = uint_to_report_cfg_id(1);
        cell_cfg_3.serving_cell_cfg.gnb_id_bit_length = gnb_id2.bit_length;
        cell_cfg_3.serving_cell_cfg.nci               = nci3;
        cell_cfg_3.serving_cell_cfg.pci               = 3;
        cell_cfg_3.serving_cell_cfg.ssb_arfcn         = 632628;
        cell_cfg_3.serving_cell_cfg.band              = nr_band::n78;
        cell_cfg_3.serving_cell_cfg.ssb_scs           = subcarrier_spacing::kHz15;
        cell_cfg_3.serving_cell_cfg.ssb_mtc = rrc_ssb_mtc{{rrc_periodicity_and_offset::periodicity_t::sf20, 0}, 5};

        cell_cfg_3.ncells.push_back({nci1, {uint_to_report_cfg_id(2)}});
        meas_mng_cfg.cells.emplace(nci3, cell_cfg_3);
      }

      // Add periodic event
      {
        rrc_periodical_report_cfg periodical_cfg;
        periodical_cfg.rs_type                 = srs_cu_cp::rrc_nr_rs_type::ssb;
        periodical_cfg.report_interv           = 1024;
        periodical_cfg.report_amount           = -1;
        periodical_cfg.report_quant_cell.rsrp  = true;
        periodical_cfg.report_quant_cell.rsrq  = true;
        periodical_cfg.report_quant_cell.sinr  = true;
        periodical_cfg.max_report_cells        = 4;
        periodical_cfg.include_beam_meass      = true;
        periodical_cfg.use_allowed_cell_list   = false;
        periodical_cfg.periodic_ho_rsrp_offset = 2;

        meas_mng_cfg.report_config_ids.emplace(uint_to_report_cfg_id(1), rrc_report_cfg_nr{periodical_cfg});
      }

      // Add event A3
      {
        rrc_event_trigger_cfg event_trigger_cfg = {};

        rrc_event_id& event_a3 = event_trigger_cfg.event_id;
        event_a3.id            = rrc_event_id::event_id_t::a3;
        event_a3.meas_trigger_quant_thres_or_offset.emplace();
        event_a3.meas_trigger_quant_thres_or_offset.value().rsrp.emplace() = 6;
        event_a3.hysteresis                                                = 0;
        event_a3.time_to_trigger                                           = 100;
        event_a3.use_allowed_cell_list                                     = false;

        event_trigger_cfg.rs_type                = srs_cu_cp::rrc_nr_rs_type::ssb;
        event_trigger_cfg.report_interv          = 1024;
        event_trigger_cfg.report_amount          = -1;
        event_trigger_cfg.report_quant_cell.rsrp = true;
        event_trigger_cfg.report_quant_cell.rsrq = true;
        event_trigger_cfg.report_quant_cell.sinr = true;
        event_trigger_cfg.max_report_cells       = 4;
        event_trigger_cfg.include_beam_meass     = true;

        rrc_meas_report_quant report_quant_rs_idxes;
        report_quant_rs_idxes.rsrp              = true;
        report_quant_rs_idxes.rsrq              = true;
        report_quant_rs_idxes.sinr              = true;
        event_trigger_cfg.report_quant_rs_idxes = report_quant_rs_idxes;

        meas_mng_cfg.report_config_ids.emplace(uint_to_report_cfg_id(2), rrc_report_cfg_nr{event_trigger_cfg});
      }
    }
    if (params.ntn_location_mobility.has_value()) {
      for (const auto& beam : params.ntn_location_mobility->beams) {
        if (meas_mng_cfg.cells.find(beam.nci) != meas_mng_cfg.cells.end()) {
          continue;
        }
        cell_meas_config beam_cell_cfg;
        beam_cell_cfg.periodic_report_cfg_id             = uint_to_report_cfg_id(1);
        beam_cell_cfg.serving_cell_cfg.gnb_id_bit_length = cu_cp_cfg.node.gnb_id.bit_length;
        beam_cell_cfg.serving_cell_cfg.nci               = beam.nci;
        meas_mng_cfg.cells.emplace(beam.nci, beam_cell_cfg);
      }
      meas_mng_cfg.ntn_location_mobility = params.ntn_location_mobility.value();
    }
    cu_cp_cfg.mobility.meas_manager_config = meas_mng_cfg;
  }

  // > RRC config
  cu_cp_cfg.rrc.rrc_procedure_guard_time_ms =
      std::chrono::milliseconds(10000); // procedure timeouts should only occur intentionally

  // > F1AP config
  cu_cp_cfg.f1ap.proc_timeout = params.f1ap_proc_timeout; // procedure timeouts should only occur intentionally

  // > E1AP config
  cu_cp_cfg.e1ap.proc_timeout = std::chrono::milliseconds(10000); // procedure timeouts should only occur intentionally

  // > UE config
  cu_cp_cfg.ue.request_pdu_session_timeout =
      std::chrono::seconds(10); // procedure timeouts should only occur intentionally

  // create CU-CP instance.
  cu_cp_inst = create_cu_cp(cu_cp_cfg);
}

cu_cp_test_environment::~cu_cp_test_environment()
{
  cu_cp_inst->stop();
  dus.clear();
  cu_ups.clear();
  cu_cp_workers->stop();

  srslog::flush();
}

void cu_cp_test_environment::tick()
{
  // Dispatch clock ticking to CU-CP worker
  cu_cp_workers->worker.push_task_blocking([this]() { timers.tick(); });
}

bool cu_cp_test_environment::tick_until(std::chrono::milliseconds    timeout,
                                        const std::function<bool()>& stop_condition,
                                        bool                         real_time)
{
  std::mutex              mutex;
  std::condition_variable cvar;
  bool                    done = false;

  // Tick up to "timeout" times, waiting for stop_condition() to return true.
  for (unsigned i = 0; i != timeout.count(); ++i) {
    if (stop_condition()) {
      return true;
    }

    // Push to CU-CP worker task that checks the state of the condition.
    done = false;
    cu_cp_workers->worker.push_task_blocking([&]() {
      // Need to tick the clock.
      tick();

      std::lock_guard<std::mutex> lock(mutex);
      done = true;
      cvar.notify_one();
    });

    // Wait for tick to be processed.
    {
      std::unique_lock<std::mutex> lock(mutex);
      cvar.wait(lock, [&done]() { return done; });
    }
    if (real_time) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  return stop_condition();
}

bool cu_cp_test_environment::wait_for_ngap_tx_pdu(ngap_message&             pdu,
                                                  std::chrono::milliseconds timeout,
                                                  unsigned                  amf_idx)
{
  return tick_until(timeout, [&]() { return amf_configs.at(amf_idx).amf_stub->try_pop_rx_pdu(pdu); });
}

bool cu_cp_test_environment::wait_for_e1ap_tx_pdu(unsigned                  cu_up_idx,
                                                  e1ap_message&             pdu,
                                                  std::chrono::milliseconds timeout)
{
  return tick_until(timeout, [&]() { return cu_ups.at(cu_up_idx)->try_pop_rx_pdu(pdu); });
}

bool cu_cp_test_environment::wait_for_f1ap_tx_pdu(unsigned du_idx, f1ap_message& pdu, std::chrono::milliseconds timeout)
{
  report_fatal_error_if_not(dus.size() >= du_idx and dus[du_idx] != nullptr, "DU index out of range");

  return tick_until(timeout, [&]() {
    if (du_idx >= dus.size() or dus[du_idx] == nullptr) {
      return false;
    }
    if (!dus[du_idx]->try_pop_dl_pdu(pdu)) {
      return false;
    }
    if (is_gnb_du_resource_coordination_request(pdu)) {
      if (!params.ntn_calendar_drop_query_responses || !is_ntn_access_calendar_query(pdu)) {
        dus[du_idx]->push_ul_pdu(
            make_gnb_du_resource_coordination_response(pdu,
                                                       params.ntn_calendar_query_stays_ready,
                                                       params.ntn_calendar_query_reports_applied_early,
                                                       params.ntn_calendar_query_reports_zero_intents,
                                                       params.ntn_calendar_prepare_rejects,
                                                       params.ntn_calendar_prepare_reports_ready,
                                                       params.ntn_calendar_preflight_incomplete,
                                                       params.ntn_calendar_preflight_unsupported,
                                                       params.ntn_resource_audit_rejects,
                                                       params.ntn_resource_audit_rnti_snapshot_complete,
                                                       params.ntn_resource_audit_retirement_supported,
                                                       params.ntn_resource_audit_legacy_only,
                                                       params.ntn_resource_audit_ue_slot_identity_supported,
                                                       params.ntn_resource_audit_ue_slot_snapshot_complete,
                                                       params.ntn_rnti_retirement_rejects,
                                                       ntn_resource_audit_rnti_generation_high_water,
                                                       ntn_resource_audit_ue_slot_generation_high_water,
                                                       ntn_resource_audit_rnti_leases,
                                                       ntn_resource_audit_ue_slots,
                                                       last_ntn_calendar_intents_per_cell,
                                                       last_ntn_calendar_preflight_reports,
                                                       ntn_calendar_clear_requests));
      }
      return false;
    }
    return true;
  });
}

bool cu_cp_test_environment::wait_for_f1ap_tx_pdu_without_auto_response(unsigned                  du_idx,
                                                                        f1ap_message&             pdu,
                                                                        std::chrono::milliseconds timeout)
{
  report_fatal_error_if_not(dus.size() >= du_idx and dus[du_idx] != nullptr, "DU index out of range");
  return tick_until(timeout, [&]() {
    if (dus[du_idx] == nullptr || !dus[du_idx]->try_pop_dl_pdu(pdu)) {
      return false;
    }
    record_ntn_calendar_prepare(pdu,
                                params.ntn_calendar_preflight_incomplete,
                                last_ntn_calendar_intents_per_cell,
                                last_ntn_calendar_preflight_reports);
    return true;
  });
}

void cu_cp_test_environment::respond_to_f1ap_resource_coordination_request(unsigned            du_idx,
                                                                            const f1ap_message& request)
{
  report_fatal_error_if_not(dus.size() >= du_idx and dus[du_idx] != nullptr, "DU index out of range");
  report_fatal_error_if_not(is_gnb_du_resource_coordination_request(request),
                            "Expected GNB-DU Resource Coordination Request");
  dus[du_idx]->push_ul_pdu(make_gnb_du_resource_coordination_response(request,
                                                                      params.ntn_calendar_query_stays_ready,
                                                                      params.ntn_calendar_query_reports_applied_early,
                                                                      params.ntn_calendar_query_reports_zero_intents,
                                                                      params.ntn_calendar_prepare_rejects,
                                                                      params.ntn_calendar_prepare_reports_ready,
                                                                      params.ntn_calendar_preflight_incomplete,
                                                                      params.ntn_calendar_preflight_unsupported,
                                                                      params.ntn_resource_audit_rejects,
                                                                      params.ntn_resource_audit_rnti_snapshot_complete,
                                                                      params.ntn_resource_audit_retirement_supported,
                                                                      params.ntn_resource_audit_legacy_only,
                                                                      params.ntn_resource_audit_ue_slot_identity_supported,
                                                                      params.ntn_resource_audit_ue_slot_snapshot_complete,
                                                                      params.ntn_rnti_retirement_rejects,
                                                                      ntn_resource_audit_rnti_generation_high_water,
                                                                      ntn_resource_audit_ue_slot_generation_high_water,
                                                                      ntn_resource_audit_rnti_leases,
                                                                      ntn_resource_audit_ue_slots,
                                                                      last_ntn_calendar_intents_per_cell,
                                                                      last_ntn_calendar_preflight_reports,
                                                                      ntn_calendar_clear_requests));
}

void cu_cp_test_environment::apply_f1ap_resource_coordination_request_without_response(unsigned            du_idx,
                                                                                       const f1ap_message& request)
{
  report_fatal_error_if_not(dus.size() >= du_idx and dus[du_idx] != nullptr, "DU index out of range");
  report_fatal_error_if_not(is_gnb_du_resource_coordination_request(request),
                            "Expected GNB-DU Resource Coordination Request");
  (void)make_gnb_du_resource_coordination_response(request,
                                                   params.ntn_calendar_query_stays_ready,
                                                   params.ntn_calendar_query_reports_applied_early,
                                                   params.ntn_calendar_query_reports_zero_intents,
                                                   params.ntn_calendar_prepare_rejects,
                                                   params.ntn_calendar_prepare_reports_ready,
                                                   params.ntn_calendar_preflight_incomplete,
                                                   params.ntn_calendar_preflight_unsupported,
                                                   params.ntn_resource_audit_rejects,
                                                   params.ntn_resource_audit_rnti_snapshot_complete,
                                                   params.ntn_resource_audit_retirement_supported,
                                                   params.ntn_resource_audit_legacy_only,
                                                   params.ntn_resource_audit_ue_slot_identity_supported,
                                                   params.ntn_resource_audit_ue_slot_snapshot_complete,
                                                   params.ntn_rnti_retirement_rejects,
                                                   ntn_resource_audit_rnti_generation_high_water,
                                                   ntn_resource_audit_ue_slot_generation_high_water,
                                                   ntn_resource_audit_rnti_leases,
                                                   ntn_resource_audit_ue_slots,
                                                   last_ntn_calendar_intents_per_cell,
                                                   last_ntn_calendar_preflight_reports,
                                                   ntn_calendar_clear_requests);
}

void cu_cp_test_environment::drain_f1ap_resource_coordination_requests(unsigned du_idx)
{
  auto du_it = dus.find(du_idx);
  report_fatal_error_if_not(du_it != dus.end() and du_it->second != nullptr, "DU index out of range");

  while (true) {
    f1ap_message f1ap_pdu;
    const bool drained_request = tick_until(std::chrono::milliseconds{20}, [&]() {
      if (!du_it->second->try_pop_dl_pdu(f1ap_pdu)) {
        return false;
      }
      report_fatal_error_if_not(is_gnb_du_resource_coordination_request(f1ap_pdu),
                                "there are still F1AP DL messages to pop from DU");
      if (!params.ntn_calendar_drop_query_responses || !is_ntn_access_calendar_query(f1ap_pdu)) {
        du_it->second->push_ul_pdu(
            make_gnb_du_resource_coordination_response(f1ap_pdu,
                                                       params.ntn_calendar_query_stays_ready,
                                                       params.ntn_calendar_query_reports_applied_early,
                                                       params.ntn_calendar_query_reports_zero_intents,
                                                       params.ntn_calendar_prepare_rejects,
                                                       params.ntn_calendar_prepare_reports_ready,
                                                       params.ntn_calendar_preflight_incomplete,
                                                       params.ntn_calendar_preflight_unsupported,
                                                       params.ntn_resource_audit_rejects,
                                                       params.ntn_resource_audit_rnti_snapshot_complete,
                                                       params.ntn_resource_audit_retirement_supported,
                                                       params.ntn_resource_audit_legacy_only,
                                                       params.ntn_resource_audit_ue_slot_identity_supported,
                                                       params.ntn_resource_audit_ue_slot_snapshot_complete,
                                                       params.ntn_rnti_retirement_rejects,
                                                       ntn_resource_audit_rnti_generation_high_water,
                                                       ntn_resource_audit_ue_slot_generation_high_water,
                                                       ntn_resource_audit_rnti_leases,
                                                       ntn_resource_audit_ue_slots,
                                                       last_ntn_calendar_intents_per_cell,
                                                       last_ntn_calendar_preflight_reports,
                                                       ntn_calendar_clear_requests));
      }
      return true;
    });
    if (!drained_request) {
      break;
    }
    cu_cp_workers->wait_pending_tasks();
  }
}

void cu_cp_test_environment::record_last_ntn_ul_slot_request(unsigned du_idx, const f1ap_message& f1ap_pdu)
{
  last_ntn_ul_slot_request_by_du.erase(du_idx);

  if (!test_helpers::is_valid_ue_context_modification_request(f1ap_pdu)) {
    return;
  }

  const auto& mod_req = f1ap_pdu.pdu.init_msg().value.ue_context_mod_request();
  if (!mod_req->res_coordination_transfer_container_present) {
    return;
  }

  std::optional<f1ap_ntn_ul_slot_resource_request> request =
      decode_f1ap_ntn_ul_slot_resource_request(mod_req->res_coordination_transfer_container);
  last_ntn_ul_slot_request_by_du.emplace(du_idx, request);

  if (!request.has_value() || !is_versioned(*request)) {
    return;
  }

  const gnb_cu_ue_f1ap_id_t cu_ue_id = int_to_gnb_cu_ue_f1ap_id(mod_req->gnb_cu_ue_f1ap_id);
  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(mod_req->gnb_du_ue_f1ap_id);
  const ue_context*         ue        = find_ue_context(du_idx, du_ue_id);
  if (ue == nullptr || !ue->cu_ue_id.has_value() || *ue->cu_ue_id != cu_ue_id) {
    return;
  }

  ntn_resource_audit_ue_slot_generation_high_water =
      std::max(ntn_resource_audit_ue_slot_generation_high_water, request->assignment_generation);
  ntn_resource_audit_ue_slots.erase(
      std::remove_if(ntn_resource_audit_ue_slots.begin(),
                     ntn_resource_audit_ue_slots.end(),
                     [cu_ue_id, du_ue_id](const f1ap_ntn_resource_audit_ue_slot& slot) {
                       return slot.gnb_cu_ue_f1ap_id == cu_ue_id || slot.gnb_du_ue_f1ap_id == du_ue_id;
                     }),
      ntn_resource_audit_ue_slots.end());

  if (request->operation != f1ap_ntn_ul_slot_resource_operation::set) {
    return;
  }

  f1ap_ntn_resource_audit_ue_slot applied;
  applied.identity_present      = true;
  applied.gnb_cu_ue_f1ap_id     = cu_ue_id;
  applied.gnb_du_ue_f1ap_id     = du_ue_id;
  applied.c_rnti                = ue->crnti;
  applied.assignment_generation = request->assignment_generation;
  applied.state                 = "applied_by_du";
  applied.request               = *request;
  applied.request.requested_c_rnti.reset();
  ntn_resource_audit_ue_slots.push_back(std::move(applied));
}

std::optional<f1ap_ntn_ul_slot_resource_result>
cu_cp_test_environment::consume_last_ntn_ul_slot_result(unsigned du_idx)
{
  auto it = last_ntn_ul_slot_request_by_du.find(du_idx);
  if (it == last_ntn_ul_slot_request_by_du.end()) {
    return std::nullopt;
  }

  std::optional<f1ap_ntn_ul_slot_resource_result> result =
      make_successful_ntn_ul_slot_result(it->second);
  last_ntn_ul_slot_request_by_du.erase(it);
  return result;
}

const cu_cp_test_environment::ue_context* cu_cp_test_environment::find_ue_context(unsigned            du_idx,
                                                                                  gnb_du_ue_f1ap_id_t du_ue_id) const
{
  auto it = du_ue_id_to_ran_ue_id_map.at(du_idx).find(du_ue_id);
  if (it == du_ue_id_to_ran_ue_id_map.at(du_idx).end()) {
    return nullptr;
  }
  return &attached_ues.at(it->second);
}

void cu_cp_test_environment::run_ng_setup()
{
  for (const auto& [amf_index, amf_config] : amf_configs) {
    get_amf(amf_index).enqueue_next_tx_pdu(srs_cu_cp::generate_ng_setup_response());
  }
  report_fatal_error_if_not(get_cu_cp().start(), "Failed to start CU-CP");

  ngap_message ngap_pdu;
  for (const auto& [amf_index, amf_config] : amf_configs) {
    report_fatal_error_if_not(get_amf(amf_index).try_pop_rx_pdu(ngap_pdu),
                              "CU-CP did not send the NG Setup Request to the AMF {}",
                              amf_index);
    report_fatal_error_if_not(is_pdu_type(ngap_pdu, asn1::ngap::ngap_elem_procs_o::init_msg_c::types::ng_setup_request),
                              "CU-CP did not setup the AMF connection");
  }
}

bool cu_cp_test_environment::drop_amf_connection(unsigned amf_idx)
{
  auto it = amf_configs.find(amf_idx);
  if (it == amf_configs.end()) {
    return false;
  }
  it->second.amf_stub->drop_connection();
  // Wait for the CU-CP to process the disconnection.
  cu_cp_workers->wait_pending_tasks();
  return true;
}

std::optional<unsigned> cu_cp_test_environment::connect_new_du()
{
  auto du_stub = create_mock_du({get_cu_cp().get_f1c_handler()});
  if (not du_stub) {
    return std::nullopt;
  }
  for (; dus.count(next_du_idx) != 0; ++next_du_idx) {
  }
  auto ret = dus.insert(std::make_pair(next_du_idx, std::move(du_stub)));
  report_fatal_error_if_not(ret.second, "Race condition detected");
  return next_du_idx;
}

bool cu_cp_test_environment::drop_du_connection(unsigned du_idx)
{
  auto it = dus.find(du_idx);
  if (it == dus.end()) {
    return false;
  }
  dus.erase(it);
  // Wait for the CU-CP to process the DU disconnection triggered by the mock DU teardown.
  cu_cp_workers->wait_pending_tasks();
  return true;
}

bool cu_cp_test_environment::run_f1_setup(unsigned                                         du_idx,
                                          gnb_du_id_t                                      gnb_du_id,
                                          std::vector<test_helpers::served_cell_item_info> cells)
{
  f1ap_message f1_setup_req = test_helpers::generate_f1_setup_request(gnb_du_id, cells);
  rrc_test_timer_values     = get_timers(f1_setup_req.pdu.init_msg().value.f1_setup_request());
  get_du(du_idx).push_ul_pdu(f1_setup_req);
  f1ap_message f1ap_pdu;
  bool         result = this->wait_for_f1ap_tx_pdu(du_idx, f1ap_pdu);
  return result;
}

std::optional<unsigned> cu_cp_test_environment::connect_new_cu_up()
{
  auto cu_up_obj = create_mock_cu_up(get_cu_cp().get_e1_handler());
  if (not cu_up_obj) {
    return std::nullopt;
  }
  for (; cu_ups.count(next_cu_up_idx) != 0; ++next_cu_up_idx) {
  }
  auto ret = cu_ups.insert(std::make_pair(next_cu_up_idx, std::move(cu_up_obj)));
  report_fatal_error_if_not(ret.second, "Race condition detected");
  return next_cu_up_idx;
}

bool cu_cp_test_environment::drop_cu_up_connection(unsigned cu_up_idx)
{
  auto it = cu_ups.find(cu_up_idx);
  if (it == cu_ups.end()) {
    return false;
  }
  cu_ups.erase(it);
  return true;
}

bool cu_cp_test_environment::run_e1_setup(unsigned cu_up_idx)
{
  get_cu_up(cu_up_idx).push_tx_pdu(generate_valid_cu_up_e1_setup_request());
  e1ap_message e1ap_pdu;
  bool         result = this->wait_for_e1ap_tx_pdu(cu_up_idx, e1ap_pdu, std::chrono::milliseconds{1000});
  return result;
}

bool cu_cp_test_environment::connect_new_ue(unsigned            du_idx,
                                            gnb_du_ue_f1ap_id_t du_ue_id,
                                            rnti_t              crnti,
                                            plmn_identity       plmn,
                                            std::optional<cu_cp_five_g_s_tmsi> five_g_s_tmsi,
                                            std::optional<nr_cell_identity> serving_nci)
{
  ngap_message ngap_pdu;
  srsran_assert(not this->get_amf().try_pop_rx_pdu(ngap_pdu), "there are still NGAP messages to pop from AMF");
  f1ap_message f1ap_pdu;
  drain_f1ap_resource_coordination_requests(du_idx);
  srsran_assert(not this->get_du(du_idx).try_pop_dl_pdu(f1ap_pdu), "there are still F1AP DL messages to pop from DU");

  // Inject Initial UL RRC message
  f1ap_message init_ul_rrc_msg =
      test_helpers::generate_init_ul_rrc_message_transfer(du_ue_id, crnti, plmn, {}, {}, serving_nci);
  test_logger.info("c-rnti={} du_ue={}: Injecting Initial UL RRC message", crnti, fmt::underlying(du_ue_id));
  get_du(du_idx).push_ul_pdu(init_ul_rrc_msg);

  // Wait for DL RRC message transfer (containing RRC Setup)
  bool result = this->wait_for_f1ap_tx_pdu(du_idx, f1ap_pdu, std::chrono::milliseconds{1000});
  if (not result) {
    return false;
  }

  // Check if the DL RRC Message with Msg4 is valid.
  report_error_if_not(test_helpers::is_valid_dl_rrc_message_transfer_with_msg4(f1ap_pdu), "invalid DL RRC message");

  // Check if the UE Id matches.
  auto& dl_rrc_msg = *f1ap_pdu.pdu.init_msg().value.dl_rrc_msg_transfer();
  report_error_if_not(int_to_gnb_du_ue_f1ap_id(dl_rrc_msg.gnb_du_ue_f1ap_id) == du_ue_id, "invalid gNB-DU-UE-F1AP-ID");
  report_error_if_not(int_to_srb_id(dl_rrc_msg.srb_id) == srb_id_t::srb0, "invalid SRB-Id");

  // Send RRC Setup Complete.
  // > Generate UL DCCH message (containing RRC Setup Complete).
  byte_buffer pdu = test_helpers::pack_ul_dcch_msg(test_helpers::create_rrc_setup_complete(1, five_g_s_tmsi));
  // > Generate UL RRC Message (containing RRC Setup Complete) with PDCP SN=0.
  get_du(du_idx).push_rrc_ul_dcch_message(du_ue_id, srb_id_t::srb1, std::move(pdu));

  // CU-CP should send an NGAP Initial UE Message.
  result = this->wait_for_ngap_tx_pdu(ngap_pdu);
  if (not result) {
    return false;
  }
  report_fatal_error_if_not(test_helpers::is_valid_init_ue_message(ngap_pdu), "Invalid init UE message");

  ue_context ue_ctx{};
  ue_ctx.crnti     = crnti;
  ue_ctx.du_ue_id  = du_ue_id;
  ue_ctx.cu_ue_id  = int_to_gnb_cu_ue_f1ap_id(dl_rrc_msg.gnb_cu_ue_f1ap_id);
  ue_ctx.ran_ue_id = uint_to_ran_ue_id(ngap_pdu.pdu.init_msg().value.init_ue_msg()->ran_ue_ngap_id);

  report_fatal_error_if_not(attached_ues.insert(std::make_pair(ue_ctx.ran_ue_id.value(), ue_ctx)).second,
                            "UE already exists");
  report_fatal_error_if_not(
      du_ue_id_to_ran_ue_id_map[du_idx].insert(std::make_pair(du_ue_id, ue_ctx.ran_ue_id.value())).second,
      "DU UE ID already exists");

  return true;
}

bool cu_cp_test_environment::authenticate_ue(unsigned du_idx, gnb_du_ue_f1ap_id_t du_ue_id, amf_ue_id_t amf_ue_id)
{
  ngap_message ngap_pdu;
  srsran_assert(not this->get_amf().try_pop_rx_pdu(ngap_pdu), "there are still NGAP messages to pop from AMF");
  f1ap_message f1ap_pdu;
  drain_f1ap_resource_coordination_requests(du_idx);
  srsran_assert(not this->get_du(du_idx).try_pop_dl_pdu(f1ap_pdu), "there are still F1AP DL messages to pop from DU");

  auto& ue_ctx     = attached_ues.at(du_ue_id_to_ran_ue_id_map.at(du_idx).at(du_ue_id));
  ue_ctx.amf_ue_id = amf_ue_id;

  // Inject NGAP DL message (authentication request)
  ngap_message dl_nas_transport =
      srs_cu_cp::generate_downlink_nas_transport_message(ue_ctx.amf_ue_id.value(), ue_ctx.ran_ue_id.value());
  get_amf().push_tx_pdu(dl_nas_transport);

  // Wait for DL RRC message transfer (containing NAS message)
  bool result = this->wait_for_f1ap_tx_pdu(du_idx, f1ap_pdu);
  if (not result) {
    return false;
  }
  report_fatal_error_if_not(test_helpers::is_valid_dl_rrc_message_transfer(f1ap_pdu),
                            "Invalid DL RRC message transfer");

  // Inject UL RRC msg transfer (authentication response)
  f1ap_message ul_rrc_msg_transfer = test_helpers::generate_ul_rrc_message_transfer(
      du_ue_id,
      ue_ctx.cu_ue_id.value(),
      srb_id_t::srb1,
      make_byte_buffer("00013a0abf002b96882dac46355c4f34464ddaf7b43fde37ae8000000000").value());
  get_du(du_idx).push_ul_pdu(ul_rrc_msg_transfer);

  // Wait for UL NAS Message (containing authentication response)
  result = this->wait_for_ngap_tx_pdu(ngap_pdu);
  if (not result) {
    return false;
  }

  // Inject DL NAS Transport message (ue security mode command)
  dl_nas_transport = generate_downlink_nas_transport_message(amf_ue_id, ue_ctx.ran_ue_id.value());
  get_amf().push_tx_pdu(dl_nas_transport);

  result = this->wait_for_f1ap_tx_pdu(du_idx, f1ap_pdu);
  if (not result) {
    return false;
  }
  report_fatal_error_if_not(test_helpers::is_valid_dl_rrc_message_transfer(f1ap_pdu),
                            "Invalid DL RRC message transfer");

  // Inject UL RRC msg transfer (ue security mode complete)
  ul_rrc_msg_transfer = test_helpers::generate_ul_rrc_message_transfer(
      du_ue_id,
      ue_ctx.cu_ue_id.value(),
      srb_id_t::srb1,
      make_byte_buffer("00023a1cbf0243241cb5003f002f3b80048290a1b283800000f8b880103f0020bc800680807888787f800008192a3b4"
                       "c080080170170700c0080a980808000000000")
          .value());
  get_du(du_idx).push_ul_pdu(ul_rrc_msg_transfer);

  // Wait for UL NAS Message (containing ue security mode complete)
  result = this->wait_for_ngap_tx_pdu(ngap_pdu);
  if (not result) {
    return false;
  }

  return true;
}

bool cu_cp_test_environment::setup_ue_security(unsigned            du_idx,
                                               gnb_du_ue_f1ap_id_t du_ue_id,
                                               byte_buffer         ue_capability_info_pdu)
{
  ngap_message ngap_pdu;
  srsran_assert(not this->get_amf().try_pop_rx_pdu(ngap_pdu), "there are still NGAP messages to pop from AMF");
  drain_f1ap_resource_coordination_requests(du_idx);
  f1ap_message f1ap_pdu;
  srsran_assert(not this->get_du(du_idx).try_pop_dl_pdu(f1ap_pdu), "there are still F1AP DL messages to pop from DU");

  auto& ue_ctx = attached_ues.at(du_ue_id_to_ran_ue_id_map.at(du_idx).at(du_ue_id));

  // Inject NGAP Initial Context Setup Request
  ngap_message init_ctxt_setup_req =
      generate_valid_initial_context_setup_request_message(ue_ctx.amf_ue_id.value(), ue_ctx.ran_ue_id.value());
  get_amf().push_tx_pdu(init_ctxt_setup_req);

  // Wait for F1AP UE Context Setup Request (containing Security Mode Command).
  bool result = this->wait_for_f1ap_tx_pdu(du_idx, f1ap_pdu);
  if (!result || !test_helpers::is_valid_ue_context_setup_request(f1ap_pdu)) {
    return false;
  }
  {
    const byte_buffer& rrc_container = test_helpers::get_rrc_container(f1ap_pdu);
    if (!test_helpers::is_valid_rrc_security_mode_command(test_helpers::extract_dl_dcch_msg(rrc_container))) {
      return false;
    }
  }
  const auto& setup_req = f1ap_pdu.pdu.init_msg().value.ue_context_setup_request();
  std::optional<f1ap_ntn_ul_slot_resource_request> ntn_slot_request;
  if (setup_req->res_coordination_transfer_container_present) {
    ntn_slot_request = decode_f1ap_ntn_ul_slot_resource_request(setup_req->res_coordination_transfer_container);
  }

  // Inject UE Context Setup Response
  f1ap_message ue_ctxt_setup_response =
      test_helpers::generate_ue_context_setup_response(ue_ctx.cu_ue_id.value(), du_ue_id);
  if (ntn_slot_request.has_value()) {
    auto& setup_resp = ue_ctxt_setup_response.pdu.successful_outcome().value.ue_context_setup_resp();
    setup_resp->res_coordination_transfer_container_present = true;
    setup_resp->res_coordination_transfer_container =
        encode_f1ap_ntn_ul_slot_resource_result(*make_successful_ntn_ul_slot_result(ntn_slot_request));
  }
  get_du(du_idx).push_ul_pdu(ue_ctxt_setup_response);

  // Inject RRC Security Mode Complete
  f1ap_message ul_rrc_msg_transfer = test_helpers::generate_ul_rrc_message_transfer(
      du_ue_id, ue_ctx.cu_ue_id.value(), srb_id_t::srb1, make_byte_buffer("00032a00fd5ec7ff").value());
  get_du(du_idx).push_ul_pdu(ul_rrc_msg_transfer);

  // Wait for UE Capability Enquiry
  result = this->wait_for_f1ap_tx_pdu(du_idx, f1ap_pdu);
  if (!result || !test_helpers::is_valid_dl_rrc_message_transfer(f1ap_pdu)) {
    return false;
  }
  {
    const byte_buffer& rrc_container = test_helpers::get_rrc_container(f1ap_pdu);
    if (!test_helpers::is_valid_rrc_ue_capability_enquiry(test_helpers::extract_dl_dcch_msg(rrc_container))) {
      return false;
    }
  }

  if (ue_capability_info_pdu.empty()) {
    ue_capability_info_pdu =
        make_byte_buffer("00044c821930680ce811d1968097e360e1480005824c5c00060fc2c00637fe002e00131401a0000000880058d006007"
                         "a071e439f0000240400e0300000000100186c0000700809df000000000000030368000800004b2ca000a07143c001c0"
                         "03c000000100200409028098a8660c")
            .value();
  }

  // Inject UL RRC Message Transfer (containing UE Capability Info)
  get_du(du_idx).push_ul_pdu(test_helpers::generate_ul_rrc_message_transfer(
      du_ue_id, ue_ctx.cu_ue_id.value(), srb_id_t::srb1, std::move(ue_capability_info_pdu)));

  // Wait for DL RRC Message Transfer (containing NAS Registration Accept)
  result = this->wait_for_f1ap_tx_pdu(du_idx, f1ap_pdu);
  if (!result || !test_helpers::is_valid_dl_rrc_message_transfer(f1ap_pdu)) {
    return false;
  }

  // Wait for Initial Context Setup Response.
  result = this->wait_for_ngap_tx_pdu(ngap_pdu);
  if (!result || !test_helpers::is_valid_initial_context_setup_response(ngap_pdu)) {
    return false;
  }

  // Wait for UE Radio Capability Info Indication.
  result = this->wait_for_ngap_tx_pdu(ngap_pdu);
  if (!result || !test_helpers::is_valid_ue_radio_capability_info_indication(ngap_pdu)) {
    return false;
  }

  drain_f1ap_resource_coordination_requests(du_idx);

  return true;
}

bool cu_cp_test_environment::finish_ue_registration(unsigned            du_idx,
                                                    unsigned            cu_up_idx,
                                                    gnb_du_ue_f1ap_id_t du_ue_id,
                                                    byte_buffer         registration_complete)
{
  ngap_message ngap_pdu;
  srsran_assert(not this->get_amf().try_pop_rx_pdu(ngap_pdu), "there are still NGAP messages to pop from AMF");

  auto& ue_ctx = attached_ues.at(du_ue_id_to_ran_ue_id_map.at(du_idx).at(du_ue_id));

  if (registration_complete.empty()) {
    registration_complete = make_byte_buffer("00053a053f015362c51680bf00218086b09a5b").value();
  }

  // Inject Registration Complete and wait UL NAS message.
  get_du(du_idx).push_ul_pdu(test_helpers::generate_ul_rrc_message_transfer(
      du_ue_id, ue_ctx.cu_ue_id.value(), srb_id_t::srb1, std::move(registration_complete)));
  bool result = this->wait_for_ngap_tx_pdu(ngap_pdu);
  report_fatal_error_if_not(result, "Failed to receive Registration Complete");

  return true;
}

bool cu_cp_test_environment::request_pdu_session_resource_setup(unsigned            du_idx,
                                                                unsigned            cu_up_idx,
                                                                gnb_du_ue_f1ap_id_t du_ue_id)
{
  ngap_message ngap_pdu;
  srsran_assert(not this->get_amf().try_pop_rx_pdu(ngap_pdu), "there are still NGAP messages to pop from AMF");
  f1ap_message f1ap_pdu;
  srsran_assert(not this->get_du(du_idx).try_pop_dl_pdu(f1ap_pdu), "there are still F1AP DL messages to pop from DU");

  auto& ue_ctx = attached_ues.at(du_ue_id_to_ran_ue_id_map.at(du_idx).at(du_ue_id));

  // Inject PDU Session Establishment Request and wait UL NAS message.
  get_du(du_idx).push_ul_pdu(test_helpers::generate_ul_rrc_message_transfer(
      du_ue_id,
      ue_ctx.cu_ue_id.value(),
      srb_id_t::srb1,
      make_byte_buffer("00063a253f011ffa9203013f0033808018970080e0ffffc9d8bd8013404010880080000840830000000041830000000"
                       "00000800001800005000006000006800008800900c092838339b939b0b83700e03a21bb")
          .value()));
  bool result = this->wait_for_ngap_tx_pdu(ngap_pdu);
  report_fatal_error_if_not(result, "Failed to receive Registration Complete");

  // Inject Configuration Update Command
  ngap_message dl_nas_transport_msg = generate_downlink_nas_transport_message(
      ue_ctx.amf_ue_id.value(),
      ue_ctx.ran_ue_id.value(),
      make_byte_buffer("7e0205545bfc027e0054430f90004f00700065006e00350047005346004732800131235200490100").value());
  get_amf().push_tx_pdu(dl_nas_transport_msg);
  result = this->wait_for_f1ap_tx_pdu(du_idx, f1ap_pdu);
  report_fatal_error_if_not(result, "Failed to receive NAS Configuration Update Command");
  report_fatal_error_if_not(test_helpers::is_valid_dl_rrc_message_transfer(f1ap_pdu),
                            "Invalid DL RRC Message Transfer");

  return true;
}

bool cu_cp_test_environment::send_pdu_session_resource_setup_request_and_await_bearer_context_setup_request(
    const ngap_message& pdu_session_resource_setup_request,
    unsigned            du_idx,
    unsigned            cu_up_idx,
    gnb_du_ue_f1ap_id_t du_ue_id)
{
  e1ap_message e1ap_pdu;
  srsran_assert(not this->get_cu_up(cu_up_idx).try_pop_rx_pdu(e1ap_pdu),
                "there are still E1AP messages to pop from CU-UP");

  // Inject PDU Session Resource Setup Request and wait for Bearer Context Setup Request
  get_amf().push_tx_pdu(pdu_session_resource_setup_request);
  bool result = this->wait_for_e1ap_tx_pdu(cu_up_idx, e1ap_pdu);
  report_fatal_error_if_not(result, "Failed to receive Bearer Context Setup Request");
  report_fatal_error_if_not(test_helpers::is_valid_bearer_context_setup_request(e1ap_pdu),
                            "Invalid Bearer Context Setup Request");

  auto& ue_ctx = attached_ues.at(du_ue_id_to_ran_ue_id_map.at(du_idx).at(du_ue_id));
  ue_ctx.cu_cp_e1ap_id =
      int_to_gnb_cu_cp_ue_e1ap_id(e1ap_pdu.pdu.init_msg().value.bearer_context_setup_request()->gnb_cu_cp_ue_e1ap_id);

  return true;
}

bool cu_cp_test_environment::send_pdu_session_resource_setup_request_and_await_bearer_context_modification_request(
    const ngap_message& pdu_session_resource_setup_request,
    unsigned            cu_up_idx)
{
  e1ap_message e1ap_pdu;
  srsran_assert(not this->get_cu_up(cu_up_idx).try_pop_rx_pdu(e1ap_pdu),
                "there are still E1AP messages to pop from CU-UP");

  // Inject PDU Session Resource Setup Request and wait for Bearer Context Setup Request
  get_amf().push_tx_pdu(pdu_session_resource_setup_request);
  bool result = this->wait_for_e1ap_tx_pdu(cu_up_idx, e1ap_pdu);
  report_fatal_error_if_not(result, "Failed to receive Bearer Context Modification Request");
  report_fatal_error_if_not(test_helpers::is_valid_bearer_context_modification_request(e1ap_pdu),
                            "Invalid Bearer Context Modification Request");

  return true;
}

bool cu_cp_test_environment::send_bearer_context_setup_response_and_await_ue_context_modification_request(
    unsigned               du_idx,
    unsigned               cu_up_idx,
    gnb_du_ue_f1ap_id_t    du_ue_id,
    gnb_cu_up_ue_e1ap_id_t cu_up_e1ap_id,
    pdu_session_id_t       psi,
    qos_flow_id_t          qfi)
{
  f1ap_message f1ap_pdu;
  drain_f1ap_resource_coordination_requests(du_idx);
  srsran_assert(not this->get_du(du_idx).try_pop_dl_pdu(f1ap_pdu), "there are still F1AP DL messages to pop from DU");

  auto& ue_ctx         = attached_ues.at(du_ue_id_to_ran_ue_id_map.at(du_idx).at(du_ue_id));
  ue_ctx.cu_up_e1ap_id = cu_up_e1ap_id;

  // Inject Bearer Context Setup Response and wait for UE Context Modification Request
  get_cu_up(cu_up_idx).push_tx_pdu(generate_bearer_context_setup_response(
      ue_ctx.cu_cp_e1ap_id.value(), ue_ctx.cu_up_e1ap_id.value(), {{psi, drb_test_params{drb_id_t::drb1, qfi}}}));
  bool result = this->wait_for_f1ap_tx_pdu(du_idx, f1ap_pdu);
  report_fatal_error_if_not(result, "Failed to receive UE Context Modification Request");
  report_fatal_error_if_not(test_helpers::is_valid_ue_context_modification_request(f1ap_pdu),
                            "Invalid UE Context Modification Request");
  record_last_ntn_ul_slot_request(du_idx, f1ap_pdu);

  return true;
}

bool cu_cp_test_environment::send_bearer_context_modification_response_and_await_ue_context_modification_request(
    unsigned            du_idx,
    unsigned            cu_up_idx,
    gnb_du_ue_f1ap_id_t du_ue_id,
    pdu_session_id_t    psi,
    drb_id_t            drb_id,
    qos_flow_id_t       qfi)
{
  f1ap_message f1ap_pdu;
  srsran_assert(not this->get_du(du_idx).try_pop_dl_pdu(f1ap_pdu), "there are still F1AP DL messages to pop from DU");

  auto& ue_ctx = attached_ues.at(du_ue_id_to_ran_ue_id_map.at(du_idx).at(du_ue_id));

  // Inject Bearer Context Modification Response and wait for UE Context Modification Request
  get_cu_up(cu_up_idx).push_tx_pdu(generate_bearer_context_modification_response(
      ue_ctx.cu_cp_e1ap_id.value(), ue_ctx.cu_up_e1ap_id.value(), {{psi, drb_test_params{drb_id, qfi}}}, {}));
  bool result = this->wait_for_f1ap_tx_pdu(du_idx, f1ap_pdu);
  report_fatal_error_if_not(result, "Failed to receive UE Context Modification Request");
  report_fatal_error_if_not(test_helpers::is_valid_ue_context_modification_request(f1ap_pdu),
                            "Invalid UE Context Modification Request");
  record_last_ntn_ul_slot_request(du_idx, f1ap_pdu);

  return true;
}

bool cu_cp_test_environment::send_ue_context_modification_response_and_await_bearer_context_modification_request(
    unsigned            du_idx,
    unsigned            cu_up_idx,
    gnb_du_ue_f1ap_id_t du_ue_id,
    rnti_t              crnti)
{
  e1ap_message e1ap_pdu;
  srsran_assert(not this->get_cu_up(cu_up_idx).try_pop_rx_pdu(e1ap_pdu),
                "there are still E1AP messages to pop from CU-UP");

  auto& ue_ctx = attached_ues.at(du_ue_id_to_ran_ue_id_map.at(du_idx).at(du_ue_id));

  // Inject UE Context Modification Response and wait for Bearer Context Modification Request
  get_du(du_idx).push_ul_pdu(test_helpers::generate_ue_context_modification_response(
      du_ue_id,
      ue_ctx.cu_ue_id.value(),
      crnti,
      {drb_id_t::drb1},
      {},
      test_helpers::create_cell_group_config(),
      consume_last_ntn_ul_slot_result(du_idx)));
  bool result = this->wait_for_e1ap_tx_pdu(cu_up_idx, e1ap_pdu);
  report_fatal_error_if_not(result, "Failed to receive Bearer Context Modification Request");
  report_fatal_error_if_not(test_helpers::is_valid_bearer_context_modification_request(e1ap_pdu),
                            "Invalid Bearer Context Modification Request");

  return true;
}

bool cu_cp_test_environment::send_bearer_context_modification_response_and_await_rrc_reconfiguration(
    unsigned                                           du_idx,
    unsigned                                           cu_up_idx,
    gnb_du_ue_f1ap_id_t                                du_ue_id,
    const std::map<pdu_session_id_t, drb_test_params>& pdu_sessions_to_add,
    const std::map<pdu_session_id_t, drb_id_t>&        pdu_sessions_to_modify,
    const std::optional<std::vector<srb_id_t>>&        expected_srbs_to_add_mod,
    const std::optional<std::vector<drb_id_t>>&        expected_drbs_to_add_mod,
    const std::vector<pdu_session_id_t>&               pdu_sessions_failed_to_modify)
{
  f1ap_message f1ap_pdu;
  srsran_assert(not this->get_du(du_idx).try_pop_dl_pdu(f1ap_pdu), "there are still F1AP DL messages to pop from DU");

  auto& ue_ctx = attached_ues.at(du_ue_id_to_ran_ue_id_map.at(du_idx).at(du_ue_id));

  // Inject E1AP Bearer Context Modification Response and wait for DL RRC Message (containing RRC Reconfiguration)
  get_cu_up(cu_up_idx).push_tx_pdu(generate_bearer_context_modification_response(ue_ctx.cu_cp_e1ap_id.value(),
                                                                                 ue_ctx.cu_up_e1ap_id.value(),
                                                                                 pdu_sessions_to_add,
                                                                                 pdu_sessions_to_modify,
                                                                                 pdu_sessions_failed_to_modify));
  bool result = this->wait_for_f1ap_tx_pdu(du_idx, f1ap_pdu);
  report_fatal_error_if_not(result, "Failed to receive F1AP DL RRC Message (containing RRC Reconfiguration)");
  report_fatal_error_if_not(test_helpers::is_valid_dl_rrc_message_transfer(f1ap_pdu),
                            "Invalid DL RRC Message Transfer");
  {
    const byte_buffer& rrc_container = test_helpers::get_rrc_container(f1ap_pdu);
    report_fatal_error_if_not(
        test_helpers::is_valid_rrc_reconfiguration(
            test_helpers::extract_dl_dcch_msg(rrc_container), true, expected_srbs_to_add_mod, expected_drbs_to_add_mod),
        "Invalid RRC Reconfiguration");
  }

  return true;
}

bool cu_cp_test_environment::send_rrc_reconfiguration_complete_and_await_pdu_session_setup_response(
    unsigned                             du_idx,
    gnb_du_ue_f1ap_id_t                  du_ue_id,
    byte_buffer                          rrc_reconfiguration_complete,
    const std::vector<pdu_session_id_t>& expected_pdu_sessions_to_setup,
    const std::vector<pdu_session_id_t>& expected_pdu_sessions_failed_to_setup)
{
  ngap_message ngap_pdu;
  srsran_assert(not this->get_amf().try_pop_rx_pdu(ngap_pdu), "there are still NGAP messages to pop from AMF");

  auto& ue_ctx = attached_ues.at(du_ue_id_to_ran_ue_id_map.at(du_idx).at(du_ue_id));

  // Inject UL RRC Message (containing RRC Reconfiguration Complete) and wait for PDU Session Resource Setup Response
  get_du(du_idx).push_ul_pdu(test_helpers::generate_ul_rrc_message_transfer(
      du_ue_id, ue_ctx.cu_ue_id.value(), srb_id_t::srb1, std::move(rrc_reconfiguration_complete)));
  bool result = this->wait_for_ngap_tx_pdu(ngap_pdu);
  report_fatal_error_if_not(result, "Failed to receive PDU Session Resource Setup Response");
  report_fatal_error_if_not(test_helpers::is_valid_pdu_session_resource_setup_response(ngap_pdu),
                            "Invalid PDU Session Resource Setup Response");
  report_fatal_error_if_not(test_helpers::is_expected_pdu_session_resource_setup_response(
                                ngap_pdu, expected_pdu_sessions_to_setup, expected_pdu_sessions_failed_to_setup),
                            "Unsuccessful PDU Session Resource Setup Response");

  return true;
}

bool cu_cp_test_environment::setup_pdu_session(unsigned               du_idx,
                                               unsigned               cu_up_idx,
                                               gnb_du_ue_f1ap_id_t    du_ue_id,
                                               rnti_t                 crnti,
                                               gnb_cu_up_ue_e1ap_id_t cu_up_e1ap_id,
                                               pdu_session_id_t       psi,
                                               drb_id_t               drb_id,
                                               qos_flow_id_t          qfi,
                                               byte_buffer            rrc_reconfiguration_complete,
                                               bool                   is_initial_session)
{
  ngap_message ngap_pdu;
  srsran_assert(not this->get_amf().try_pop_rx_pdu(ngap_pdu), "there are still NGAP messages to pop from AMF");
  f1ap_message f1ap_pdu;
  srsran_assert(not this->get_du(du_idx).try_pop_dl_pdu(f1ap_pdu), "there are still F1AP DL messages to pop from DU");
  e1ap_message e1ap_pdu;
  srsran_assert(not this->get_cu_up(cu_up_idx).try_pop_rx_pdu(e1ap_pdu),
                "there are still E1AP messages to pop from CU-UP");

  auto& ue_ctx = attached_ues.at(du_ue_id_to_ran_ue_id_map.at(du_idx).at(du_ue_id));

  ngap_message pdu_session_resource_setup_request = generate_valid_pdu_session_resource_setup_request_message(
      ue_ctx.amf_ue_id.value(), ue_ctx.ran_ue_id.value(), {{psi, {pdu_session_type_t::ipv4, {{qfi, 9}}}}});

  if (is_initial_session) {
    // Inject PDU Session Resource Setup Request and wait for Bearer Context Setup Request.
    if (not send_pdu_session_resource_setup_request_and_await_bearer_context_setup_request(
            pdu_session_resource_setup_request, du_idx, cu_up_idx, du_ue_id)) {
      return false;
    }

    // Inject Bearer Context Setup Response and wait for F1AP UE Context Modification Request.
    if (not send_bearer_context_setup_response_and_await_ue_context_modification_request(
            du_idx, cu_up_idx, du_ue_id, cu_up_e1ap_id, psi, qfi)) {
      return false;
    }
  } else {
    // Inject PDU Session Resource Setup Request and wait for Bearer Context Modification Request.
    if (not send_pdu_session_resource_setup_request_and_await_bearer_context_modification_request(
            pdu_session_resource_setup_request, du_idx)) {
      return false;
    }

    // Inject Bearer Context Modification Response and wait for F1AP UE Context Modification Request.
    if (not send_bearer_context_modification_response_and_await_ue_context_modification_request(
            du_idx, cu_up_idx, du_ue_id, psi, drb_id, qfi)) {
      return false;
    }
  }

  // Inject UE Context Modification Response and wait for Bearer Context Modification to be sent to CU-UP.
  if (not send_ue_context_modification_response_and_await_bearer_context_modification_request(
          du_idx, cu_up_idx, du_ue_id, crnti)) {
    return false;
  }

  // Inject Bearer Context Modification Response and wait for DL RRC Message (containing RRC Reconfiguration)
  if (not send_bearer_context_modification_response_and_await_rrc_reconfiguration(
          du_idx, cu_up_idx, du_ue_id, {}, {{psi, drb_id}})) {
    return false;
  }

  // Inject RRC Reconfiguration Complete and wait for PDU Session Resource Setup Response to be sent to AMF.
  if (not send_rrc_reconfiguration_complete_and_await_pdu_session_setup_response(
          du_idx, du_ue_id, std::move(rrc_reconfiguration_complete), {psi}, {})) {
    return false;
  }

  return true;
}

bool cu_cp_test_environment::attach_ue(unsigned               du_idx,
                                       unsigned               cu_up_idx,
                                       gnb_du_ue_f1ap_id_t    du_ue_id,
                                       rnti_t                 crnti,
                                       amf_ue_id_t            amf_ue_id,
                                       gnb_cu_up_ue_e1ap_id_t cu_up_e1ap_id,
                                       pdu_session_id_t       psi,
                                       drb_id_t               drb_id,
                                       qos_flow_id_t          qfi,
                                       byte_buffer            rrc_reconfiguration_complete)
{
  if (not connect_new_ue(du_idx, du_ue_id, crnti)) {
    return false;
  }
  if (not authenticate_ue(du_idx, du_ue_id, amf_ue_id)) {
    return false;
  }
  if (not setup_ue_security(du_idx, du_ue_id)) {
    return false;
  }
  if (not finish_ue_registration(du_idx, cu_up_idx, du_ue_id)) {
    return false;
  }
  if (not request_pdu_session_resource_setup(du_idx, cu_up_idx, du_ue_id)) {
    return false;
  }
  if (not setup_pdu_session(du_idx,
                            cu_up_idx,
                            du_ue_id,
                            crnti,
                            cu_up_e1ap_id,
                            psi,
                            drb_id,
                            qfi,
                            std::move(rrc_reconfiguration_complete),
                            true)) {
    return false;
  }

  return true;
}

bool cu_cp_test_environment::reestablish_ue(unsigned            du_idx,
                                            unsigned            cu_up_idx,
                                            gnb_du_ue_f1ap_id_t new_du_ue_id,
                                            rnti_t              new_crnti,
                                            rnti_t              old_crnti,
                                            pci_t               old_pci)
{
  f1ap_message f1ap_pdu;

  // Send Initial UL RRC Message (containing RRC Reestablishment Request) to CU-CP.
  byte_buffer rrc_container = test_helpers::pack_ul_ccch_msg(
      test_helpers::create_rrc_reestablishment_request(old_crnti, old_pci, "1111010001000010"));
  f1ap_message f1ap_init_ul_rrc_msg = test_helpers::generate_init_ul_rrc_message_transfer(
      new_du_ue_id, new_crnti, plmn_identity::test_value(), {}, std::move(rrc_container));
  get_du(du_idx).push_ul_pdu(f1ap_init_ul_rrc_msg);

  // Wait for DL RRC message transfer (with RRC Reestablishment / RRC Setup / RRC Reject).
  bool result = this->wait_for_f1ap_tx_pdu(du_idx, f1ap_pdu);
  report_fatal_error_if_not(result, "F1AP DL RRC Message Transfer with Msg4 not sent to DU");
  report_fatal_error_if_not(test_helpers::is_valid_dl_rrc_message_transfer_with_msg4(f1ap_pdu), "Invalid Msg4");

  auto& dl_rrc_msg = *f1ap_pdu.pdu.init_msg().value.dl_rrc_msg_transfer();
  report_fatal_error_if_not(int_to_gnb_du_ue_f1ap_id(dl_rrc_msg.gnb_du_ue_f1ap_id) == new_du_ue_id,
                            "Invalid gNB-DU-UE-F1AP-ID");

  if (dl_rrc_msg.srb_id == 0) {
    // RRC Setup / RRC Reject.

    // Send RRC Setup Complete.
    // > Generate UL DCCH message (containing RRC Setup Complete).
    byte_buffer pdu = test_helpers::pack_ul_dcch_msg(test_helpers::create_rrc_setup_complete());
    // > Generate UL RRC Message (containing RRC Setup Complete) with PDCP SN=0.
    get_du(du_idx).push_rrc_ul_dcch_message(new_du_ue_id, srb_id_t::srb1, std::move(pdu));

    // CU-CP should send an NGAP Initial UE Message.
    ngap_message ngap_pdu;
    result = this->wait_for_ngap_tx_pdu(ngap_pdu);
    report_fatal_error_if_not(result, "Failed to send NGAP Initial UE Message");
    report_fatal_error_if_not(test_helpers::is_valid_init_ue_message(ngap_pdu), "Invalid init UE message");

    ue_context ue_ctx{};
    ue_ctx.crnti     = new_crnti;
    ue_ctx.du_ue_id  = new_du_ue_id;
    ue_ctx.cu_ue_id  = int_to_gnb_cu_ue_f1ap_id(dl_rrc_msg.gnb_cu_ue_f1ap_id);
    ue_ctx.ran_ue_id = uint_to_ran_ue_id(ngap_pdu.pdu.init_msg().value.init_ue_msg()->ran_ue_ngap_id);

    report_fatal_error_if_not(attached_ues.insert(std::make_pair(ue_ctx.ran_ue_id.value(), ue_ctx)).second,
                              "UE already exists");
    report_fatal_error_if_not(
        du_ue_id_to_ran_ue_id_map[du_idx].insert(std::make_pair(new_du_ue_id, ue_ctx.ran_ue_id.value())).second,
        "DU UE ID already exists");

    return false;
  }

  gnb_du_ue_f1ap_id_t old_du_ue_id = int_to_gnb_du_ue_f1ap_id(dl_rrc_msg.old_gnb_du_ue_f1ap_id);
  ue_context&         old_ue       = attached_ues.at(du_ue_id_to_ran_ue_id_map.at(du_idx).at(old_du_ue_id));
  old_ue.du_ue_id                  = new_du_ue_id;
  old_ue.cu_ue_id                  = int_to_gnb_cu_ue_f1ap_id(dl_rrc_msg.gnb_cu_ue_f1ap_id);
  old_ue.crnti                     = new_crnti;
  ran_ue_id_t ran_ue_id            = *old_ue.ran_ue_id;
  du_ue_id_to_ran_ue_id_map.at(du_idx).erase(old_du_ue_id);
  report_fatal_error_if_not(du_ue_id_to_ran_ue_id_map[du_idx].insert(std::make_pair(new_du_ue_id, ran_ue_id)).second,
                            "DU UE ID already exists");

  // EVENT: Send RRC Reestablishment Complete.
  // > Generate UL-DCCH message (containing RRC Reestablishment Complete).
  byte_buffer pdu = test_helpers::pack_ul_dcch_msg(test_helpers::create_rrc_reestablishment_complete());
  // > Prepend PDCP header and append MAC.
  report_error_if_not(pdu.prepend(std::array<uint8_t, 2>{0x00U, 0x00U}), "bad alloc");
  report_error_if_not(pdu.append(std::array<uint8_t, 4>{0x85, 0xc1, 0x04, 0xf1}), "bad alloc");
  // > Send UL RRC Message to CU-CP.
  get_du(du_idx).push_ul_pdu(
      test_helpers::generate_ul_rrc_message_transfer(new_du_ue_id, *old_ue.cu_ue_id, srb_id_t::srb1, std::move(pdu)));

  // STATUS: CU-CP sends E1AP Bearer Context Modification Request.
  e1ap_message e1ap_pdu;
  report_fatal_error_if_not(this->wait_for_e1ap_tx_pdu(0, e1ap_pdu), "E1AP BearerContextModificationRequest NOT sent");

  gnb_cu_cp_ue_e1ap_id_t cu_cp_e1ap_id =
      int_to_gnb_cu_cp_ue_e1ap_id(e1ap_pdu.pdu.init_msg().value.bearer_context_mod_request()->gnb_cu_cp_ue_e1ap_id);
  gnb_cu_up_ue_e1ap_id_t cu_up_e1ap_id =
      int_to_gnb_cu_up_ue_e1ap_id(e1ap_pdu.pdu.init_msg().value.bearer_context_mod_request()->gnb_cu_up_ue_e1ap_id);

  // EVENT: Inject E1AP Bearer Context Modification Response
  get_cu_up(cu_up_idx).push_tx_pdu(generate_bearer_context_modification_response(cu_cp_e1ap_id, cu_up_e1ap_id));

  // STATUS: CU-CP sends F1AP UE Context Modification Request to DU.
  report_fatal_error_if_not(this->wait_for_f1ap_tx_pdu(du_idx, f1ap_pdu), "F1AP UEContextModificationRequest NOT sent");
  report_fatal_error_if_not(f1ap_pdu.pdu.init_msg().value.ue_context_mod_request()->drbs_to_be_modified_list_present,
                            "UE Context Modification Request for RRC Reestablishment must contain DRBs to be modified");
  report_fatal_error_if_not(
      not f1ap_pdu.pdu.init_msg().value.ue_context_mod_request()->drbs_to_be_setup_mod_list_present,
      "UE Context Modification Request for RRC Reestablishment must not contain DRBs to be setup");

  // EVENT: Inject F1AP UE Context Modification Response
  get_du(du_idx).push_ul_pdu(test_helpers::generate_ue_context_modification_response(
      new_du_ue_id, *this->find_ue_context(du_idx, new_du_ue_id)->cu_ue_id, new_crnti));

  // STATUS: CU-CP sends E1AP Bearer Context Modification Request.
  report_fatal_error_if_not(this->wait_for_e1ap_tx_pdu(0, e1ap_pdu), "E1AP BearerContextModificationRequest NOT sent");

  // EVENT: CU-UP sends E1AP Bearer Context Modification Response
  get_cu_up(cu_up_idx).push_tx_pdu(generate_bearer_context_modification_response(cu_cp_e1ap_id, cu_up_e1ap_id));

  // STATUS: CU-CP sends F1AP DL RRC Message Transfer (containing RRC Reconfiguration).
  report_fatal_error_if_not(this->wait_for_f1ap_tx_pdu(du_idx, f1ap_pdu), "F1AP DL RRC Message NOT sent");
  report_fatal_error_if_not(test_helpers::is_valid_dl_rrc_message_transfer(f1ap_pdu),
                            "Invalid DL RRC Message Transfer");

  // EVENT: DU sends F1AP UL RRC Message Transfer (containing RRC Reconfiguration Complete).
  pdu = test_helpers::pack_ul_dcch_msg(test_helpers::create_rrc_reconfiguration_complete(1U));
  // > Prepend PDCP header and append MAC.
  report_error_if_not(pdu.prepend(std::array<uint8_t, 2>{0x00U, 0x01U}), "bad alloc");
  report_error_if_not(pdu.append(std::array<uint8_t, 4>{0xf1, 0x21, 0x02, 0x5e}), "bad alloc");
  get_du(du_idx).push_ul_pdu(
      test_helpers::generate_ul_rrc_message_transfer(new_du_ue_id, *old_ue.cu_ue_id, srb_id_t::srb1, std::move(pdu)));

  return true;
}
