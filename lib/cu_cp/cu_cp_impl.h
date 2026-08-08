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

#include "adapters/cell_meas_manager_adapters.h"
#include "adapters/du_processor_adapters.h"
#include "adapters/e1ap_adapters.h"
#include "adapters/mobility_manager_adapters.h"
#include "adapters/ngap_adapters.h"
#include "adapters/nrppa_adapters.h"
#include "cu_configurator_impl.h"
#include "cu_cp_controller/cu_cp_controller.h"
#include "cu_cp_impl_interface.h"
#include "cu_up_processor/cu_up_processor_repository.h"
#include "du_processor/du_processor_repository.h"
#include "ngap_repository.h"
#include "ntn_mobility/ntn_assistance_snapshot_generator.h"
#include "ntn_mobility/ntn_beam_placement_planner.h"
#include "ntn_mobility/ntn_beam_service_resource_manager.h"
#include "ntn_mobility/ntn_beam_tac.h"
#include "ntn_mobility/ntn_initial_ul_position_authorizer.h"
#include "ntn_mobility/ntn_onboard_position_plan.h"
#include "ntn_mobility/ntn_onboard_position_plan_state.h"
#include "ntn_mobility/ntn_onboard_runtime_mapping.h"
#include "ntn_mobility/ntn_plan_version_anchor.h"
#include "ntn_mobility/ntn_satellite_state_updater.h"
#include "ntn_mobility/ntn_served_beam_scheduler.h"
#include "ntn_mobility/ntn_service_switch_over_controller.h"
#include "ntn_mobility/ntn_sib19_assistance_builder.h"
#include "ntn_mobility/ntn_sib19_broadcast_controller.h"
#include "ntn_mobility/ntn_ue_capability_gate.h"
#include "ue_manager/ue_manager_impl.h"
#include "srsran/cu_cp/cu_configurator.h"
#include "srsran/cu_cp/cu_cp_configuration.h"
#include "srsran/cu_cp/cu_cp_types.h"
#include "srsran/e2/e2_cu.h"
#include "srsran/e2/e2_cu_up_factory.h"
#include "srsran/f1ap/cu_cp/f1ap_cu.h"
#include "srsran/f1ap/ntn_access_calendar.h"
#include "srsran/f1ap/ntn_ul_slot_resource_request.h"
#include "srsran/nrppa/nrppa.h"
#include "srsran/ran/plmn_identity.h"
#include <chrono>
#include <dlfcn.h>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <unordered_map>
#include <utility>

namespace srsran {
namespace srs_cu_cp {

namespace ntn_onboard_detail {

enum class ntn_onboard_du_cell_resolution_status { resolved, unavailable, missing, duplicate, cross_du };

struct ntn_onboard_du_cell_candidate {
  du_index_t                   du_index = du_index_t::invalid;
  nr_cell_identity             nci;
  pci_t                        pci  = INVALID_PCI;
  const du_cell_configuration* cell = nullptr;
};

struct ntn_onboard_du_cell_resolution {
  ntn_onboard_du_cell_resolution_status       status   = ntn_onboard_du_cell_resolution_status::missing;
  du_index_t                                  du_index = du_index_t::invalid;
  std::array<const du_cell_configuration*, 2> cells{};
};

const char* get_ntn_onboard_du_cell_resolution_detail(ntn_onboard_du_cell_resolution_status status);

ntn_onboard_du_cell_resolution
resolve_ntn_onboard_du_cells(const std::vector<ntn_onboard_du_cell_candidate>&   candidates,
                             const std::array<ntn_onboard_cell_position_set, 2>& planned_cells,
                             const std::set<du_index_t>& disconnected_du_indexes = {});

} // namespace ntn_onboard_detail

class cu_cp_common_task_scheduler : public common_task_scheduler
{
public:
  cu_cp_common_task_scheduler() : main_ctrl_loop(128) {}

  bool schedule_async_task(async_task<void> task) override { return main_ctrl_loop.schedule(std::move(task)); }

private:
  // cu-cp task event loop
  fifo_async_task_scheduler main_ctrl_loop;
};

class cu_cp_impl final : public cu_cp,
                         public cu_cp_impl_interface,
                         public cu_cp_ng_handler,
                         public cu_cp_command_handler,
                         public cu_cp_ntn_command_handler,
                         public cu_cp_ue_command_handler,
                         public cu_cp_admission_command_handler,
                         public ntn_served_beam_update_handler,
                         public mobility_manager_measurement_handler
{
public:
  explicit cu_cp_impl(const cu_cp_configuration& config_);
  ~cu_cp_impl() override;

  bool start() override;
  void stop() override;

  // NGAP interface.
  ngap_message_handler* get_ngap_message_handler(const plmn_identity& plmn) override;

  bool amfs_are_connected() override;

  // NRPPA interface.
  std::unique_ptr<nrppa_interface> create_nrppa_entity(const cu_cp_configuration& cu_cp_cfg,
                                                       nrppa_cu_cp_notifier&      cu_cp_notif,
                                                       common_task_scheduler&     common_task_sched_);

  // CU-UP handler.
  void handle_bearer_context_release_request(const cu_cp_bearer_context_release_request& msg) override;
  void handle_bearer_context_inactivity_notification(const cu_cp_inactivity_notification& msg) override;
  void handle_e1_release_request(cu_up_index_t cu_up_index) override;

  // cu_cp_rrc_ue_interface.
  bool handle_ue_setup_request(ue_index_t ue_index) override;
  bool handle_ue_plmn_selected(ue_index_t ue_index, const plmn_identity& plmn) override;
  rrc_ue_reestablishment_context_response
                   handle_rrc_reestablishment_request(pci_t old_pci, rnti_t old_c_rnti, ue_index_t ue_index) override;
  async_task<bool> handle_rrc_reestablishment_context_modification_required(ue_index_t ue_index) override;

  void             handle_rrc_reestablishment_failure(const cu_cp_ue_context_release_request& request) override;
  void             handle_rrc_reestablishment_complete(ue_index_t old_ue_index) override;
  void             handle_rrc_resume_request(ue_index_t            ue_index,
                                             ue_index_t            old_ue_index,
                                             establishment_cause_t rrc_resume_cause) override;
  void             handle_rrc_reconf_complete_indicator(ue_index_t ue_index) override;
  async_task<bool> handle_ue_context_transfer(ue_index_t ue_index, ue_index_t old_ue_index) override;
  async_task<void> handle_ue_context_release(const cu_cp_ue_context_release_request& request) override;

  // cu_cp_ue_context_manipulation_handler.
  void handle_handover_reconfiguration_sent(const cu_cp_intra_cu_handover_target_request& request) override;
  void handle_handover_ue_context_push(ue_index_t source_ue_index, ue_index_t target_ue_index) override;
  void handle_ntn_handover_result(const ntn_handover_result& result) override;
  void handle_ntn_handover_target_resources_applied(
      ue_index_t                              source_ue_index,
      ue_index_t                              target_ue_index,
      const ntn_handover_context&             context,
      rnti_t                                  target_c_rnti,
      const f1ap_ntn_ul_slot_resource_result& slot_result) override;
  void
  initialize_handover_ue_release_timer(ue_index_t                              ue_index,
                                       std::chrono::milliseconds               handover_ue_release_timeout,
                                       const cu_cp_ue_context_release_request& ue_context_release_request) override;

  // cu_cp_ngap_handler.
  bool handle_handover_request(ue_index_t                        ue_index,
                               const plmn_identity&              selected_plmn,
                               const security::security_context& sec_ctxt) override;
  async_task<expected<ngap_init_context_setup_response, ngap_init_context_setup_failure>>
  handle_new_initial_context_setup_request(const ngap_init_context_setup_request& request) override;
  async_task<cu_cp_pdu_session_resource_setup_response>
  handle_new_pdu_session_resource_setup_request(cu_cp_pdu_session_resource_setup_request& request) override;
  async_task<cu_cp_pdu_session_resource_modify_response>
  handle_new_pdu_session_resource_modify_request(const cu_cp_pdu_session_resource_modify_request& request) override;
  async_task<cu_cp_pdu_session_resource_release_response>
  handle_new_pdu_session_resource_release_command(const cu_cp_pdu_session_resource_release_command& command) override;
  async_task<cu_cp_ue_context_release_complete>
  handle_ue_context_release_command(const cu_cp_ue_context_release_command& command) override;
  async_task<ngap_handover_resource_allocation_response>
                   handle_ngap_handover_request(const ngap_handover_request& request) override;
  void             handle_transmission_of_handover_required() override;
  async_task<bool> handle_new_handover_command(ue_index_t ue_index, byte_buffer command) override;
  ue_index_t handle_ue_index_allocation_request(const nr_cell_global_id_t& cgi, const plmn_identity& plmn) override;
  void       handle_n2_handover_execution(ue_index_t ue_index) override;
  void       handle_dl_ue_associated_nrppa_transport_pdu(ue_index_t ue_index, const byte_buffer& nrppa_pdu) override;
  void handle_dl_non_ue_associated_nrppa_transport_pdu(amf_index_t amf_index, const byte_buffer& nrppa_pdu) override;
  ngap_location_reporting_control_response
       handle_location_reporting_control(const ngap_location_reporting_control& request) override;
  void handle_ue_context_suspend_outcome(ue_index_t ue_index, bool success) override;
  void handle_ue_context_resume_outcome(ue_index_t ue_index, bool success) override;
  void handle_n2_disconnection(amf_index_t amf_index) override;

  // cu_cp_nrppa_handler.
  nrppa_cu_cp_ue_notifier* handle_new_nrppa_ue(ue_index_t ue_index) override;
  void                     handle_ul_nrppa_pdu(const byte_buffer&                    nrppa_pdu,
                                               std::variant<ue_index_t, amf_index_t> ue_or_amf_index) override;
  async_task<trp_information_cu_cp_response_t>
  handle_trp_information_request(const trp_information_request_t& request) override;
  async_task<expected<positioning_information_response_t, positioning_information_failure_t>>
  handle_positioning_information_request(const positioning_information_request_t& request) override;
  async_task<expected<positioning_activation_response_t, positioning_activation_failure_t>>
  handle_positioning_activation_request(const positioning_activation_request_t& request) override;
  async_task<expected<positioning_deactivation_response_t, positioning_deactivation_failure_t>>
  handle_positioning_deactivation_request(const positioning_deactivation_request_t& request) override;
  async_task<expected<positioning_assistance_information_feedback_t, positioning_assistance_information_failure_t>>
  handle_positioning_assistance_information_control(
      const positioning_assistance_information_control_request_t& request) override;
  async_task<expected<measurement_response_t, measurement_failure_t>>
  handle_positioning_measurement_request(const measurement_request_t& request) override;
  void handle_unsupported_nrppa_pdu(std::string_view reason) override;
  void handle_nrppa_standard_codec_event(const nrppa_standard_codec_event& event) override;

  // cu_cp_measurement_handler.
  std::optional<rrc_meas_cfg>
       handle_measurement_config_request(ue_index_t                         ue_index,
                                         nr_cell_identity                   nci,
                                         const std::optional<rrc_meas_cfg>& current_meas_config = std::nullopt) override;
  void handle_measurement_report(const ue_index_t ue_index, const rrc_meas_results& meas_results) override;
  void handle_ue_capability_update(ue_index_t ue_index) override;
  void handle_ue_location_report(const ntn_ue_location_report& location_report) override;
  void handle_rrc_ue_location_report_outcome(ue_index_t ue_index, ntn_rrc_ue_location_report_outcome outcome) override;

  // mobility_manager_measurement_handler.
  void handle_neighbor_better_than_spcell(ue_index_t       ue_index,
                                          gnb_id_t         neighbor_gnb_id,
                                          nr_cell_identity neighbor_nci,
                                          pci_t            neighbor_pci) override;
  bool handle_ntn_location_handover_required(const ntn_location_handover_trigger& trigger) override;
  void handle_ntn_served_beams_updated(const std::vector<std::string>& beam_ids) override;
  void handle_ntn_beam_placement_plan_updated(const ntn_beam_placement_plan& plan) override;

  // cu_cp_ntn_command_handler.
  bool handle_ntn_satellite_state_update(const ecef_coordinates_t&           satellite,
                                         std::optional<ecef_coordinates_t> next_satellite = std::nullopt) override;
  bool handle_ntn_satellite_state_update(const std::vector<ntn_satellite_state>& current_satellites,
                                         const std::vector<ntn_satellite_state>& next_satellites) override;
  bool handle_ntn_satellite_state_update(
      const std::vector<ntn_satellite_state>&              current_satellites,
      const std::vector<ntn_satellite_prediction_step>& future_steps) override;
  std::vector<std::string> get_current_ntn_served_beam_ids() const override;
  std::vector<cu_cp_ntn_beam_status> get_current_ntn_beam_status() const override;
  ntn_assistance_snapshot             get_current_ntn_assistance_snapshot() const override;
  ntn_sib19_assistance_snapshot       get_current_ntn_sib19_assistance_snapshot() const override;
  cu_cp_ntn_runtime_status            get_current_ntn_runtime_status() const override;
  std::vector<cu_cp_ntn_ue_status>    get_current_ntn_ue_status() const override;
  cu_cp_ntn_antenna_intent_snapshot   get_current_ntn_antenna_intent_snapshot() const override;
  ntn_beam_service_resource_snapshot  get_current_ntn_beam_service_resource_snapshot() const override;
  bool handle_ntn_service_switch_over_event(const ntn_service_switch_over_event& event) override;
  bool clear_ntn_service_switch_over_event(uint64_t event_id) override;
  bool handle_ntn_manual_override(const ntn_manual_override_command& command) override;
  ntn_repair_response handle_ntn_repair_command(const ntn_repair_command& command) override;
  ntn_service_switch_over_snapshot get_current_ntn_service_switch_over_snapshot() const override;

  // cu_cp_ue_command_handler.
  async_task<cu_cp_ue_context_release_batch_response>
  release_ues(const cu_cp_ue_context_release_batch_command& command) override;

  // cu_cp_admission_command_handler.
  void set_ue_admission_enabled(bool enabled) override;
  cu_cp_admission_control_status get_admission_control_status() override;

  // cu_cp_measurement_config_handler.
  bool handle_cell_config_update_request(nr_cell_identity nci, const serving_cell_meas_config& serv_cell_cfg) override;

  // cu_cp_mobility_manager_handler.
  async_task<cu_cp_intra_cu_handover_response>
  handle_intra_cu_handover_request(const cu_cp_intra_cu_handover_request& request,
                                   du_index_t&                            source_du_index,
                                   du_index_t&                            target_du_index) override;
  void handle_mobility_ntn_handover_result(const ntn_handover_result& result) override;

  // cu_cp_ue_removal_handler.
  async_task<void> handle_ue_removal_request(ue_index_t ue_index) override;
  void             handle_pending_ue_task_cancellation(ue_index_t ue_index) override;

  cu_cp_mobility_command_handler& get_mobility_command_handler() override { return mobility_mng; }
  cu_cp_ntn_command_handler&      get_ntn_command_handler() override { return *this; }
  cu_cp_ue_command_handler&       get_ue_command_handler() override { return *this; }
  cu_cp_admission_command_handler& get_admission_command_handler() override { return *this; }
  metrics_handler&                get_metrics_handler() override { return *metrics_hdlr; }

  // cu_cp_amf_reconnection_handler.
  void handle_amf_reconnection(amf_index_t amf_index) override;

  // cu_cp public interface.
  cu_cp_f1c_handler&                     get_f1c_handler() override { return controller.get_f1c_handler(); }
  cu_cp_e1_handler&                      get_e1_handler() override { return controller.get_e1_handler(); }
  cu_cp_e1ap_event_handler&              get_cu_cp_e1ap_handler() override { return *this; }
  cu_cp_ng_handler&                      get_ng_handler() override { return *this; }
  cu_cp_ngap_handler&                    get_cu_cp_ngap_handler() override { return *this; }
  cu_cp_nrppa_handler&                   get_cu_cp_nrppa_handler() override { return *this; }
  cu_cp_command_handler&                 get_command_handler() override { return *this; }
  cu_cp_rrc_ue_interface&                get_cu_cp_rrc_ue_interface() override { return *this; }
  cu_cp_measurement_handler&             get_cu_cp_measurement_handler() override { return *this; }
  cu_cp_measurement_config_handler&      get_cu_cp_measurement_config_handler() override { return *this; }
  cu_cp_mobility_manager_handler&        get_cu_cp_mobility_manager_handler() override { return *this; }
  cu_cp_ue_removal_handler&              get_cu_cp_ue_removal_handler() override { return *this; }
  cu_cp_ue_context_manipulation_handler& get_cu_cp_ue_context_handler() override { return *this; }
  cu_cp_amf_reconnection_handler&        get_cu_cp_amf_reconnection_handler() override { return *this; }
  cu_configurator&                       get_cu_configurator() override { return cu_cp_cfgtr; }

private:
  // Handling of DU events.
  void handle_rrc_ue_creation(ue_index_t ue_index, rrc_ue_interface& rrc_ue) override;

  void handle_du_connection_established(du_index_t du_index) override;
  void handle_du_disconnection(du_index_t du_index) override;

  byte_buffer handle_target_cell_sib1_required(du_index_t du_index, nr_cell_global_id_t cgi) override;

  async_task<void> handle_transaction_info_loss(const ue_transaction_info_loss_event& ev) override;

  // NGAP UE creation handler.
  ngap_cu_cp_ue_notifier* handle_new_ngap_ue(ue_index_t ue_index) override;
  void handle_rrc_initial_ue_message(const cu_cp_initial_ue_message& msg) override;
  void handle_paging_message(cu_cp_paging_message& msg) override;

  // cu_cp_task_scheduler_handler.
  bool schedule_ue_task(ue_index_t ue_index, async_task<void> task) override;

  // ntn_served_beam_update_handler.
  bool update_ntn_served_beams(const std::vector<std::string>& beam_ids) override;
  bool update_ntn_served_beam_candidates(const std::vector<ntn_served_beam_candidate>& candidates) override;

  std::optional<cu_cp_user_location_info_nr>
  build_ntn_core_user_location_info(const ntn_ue_location_report& report);
  struct ntn_onboard_ue_cell_view;
  std::optional<ntn_onboard_ue_cell_view>
  resolve_ntn_onboard_ue_cell(ue_index_t ue_index, std::chrono::system_clock::time_point now);
  std::optional<cu_cp_user_location_info_nr> build_ntn_onboard_user_location_info(ue_index_t ue_index);
  std::optional<cu_cp_user_location_info_nr> build_ntn_release_user_location_info(ue_index_t ue_index);
  std::optional<cu_cp_info_on_recommended_cells_and_ran_nodes_for_paging>
  build_ntn_paging_recommendation(ue_index_t ue_index);
  void persist_ntn_idle_paging_context_for_ue(ue_index_t ue_index, const char* reason);
  void prune_expired_ntn_idle_paging_contexts(std::chrono::steady_clock::time_point now);
  bool apply_ntn_idle_paging_recommendation(cu_cp_paging_message& msg);
  bool schedule_ntn_inactive_suspend_if_eligible(ue_index_t ue_index);
  bool is_ntn_inactive_suspend_eligible(const cu_cp_ue& ue, std::string& reason);
  void persist_ntn_inactive_context_for_ue(ue_index_t ue_index,
                                           const rrc_ue_release_context& release_context,
                                           const char*                   reason);
  void schedule_ntn_ul_slot_updates_for_online_ues();
  void schedule_ntn_rnti_lease_pool_updates_for_access_beams();
  void schedule_ntn_rnti_retirement_updates();
  void schedule_ntn_sib19_broadcast_updates();
  void on_ntn_resource_audit_timer_expired();
  struct ntn_resource_audit_target {
    du_index_t              du_index = du_index_t::invalid;
    srsran::du_cell_index_t cell_index = srsran::INVALID_DU_CELL_INDEX;
    pci_t                   pci = INVALID_PCI;
    bool                    service_pair = false;
  };
  std::vector<ntn_resource_audit_target> collect_ntn_resource_audit_targets();
  void schedule_ntn_resource_audits();
  void handle_ntn_resource_audit_decision(const ntn_resource_audit_decision& decision);
  void mark_ntn_service_pair_resource_repair_skipped(const ntn_resource_repair& repair, const char* reason);
  std::optional<std::vector<rnti_t>> allocate_ntn_rnti_leases(du_index_t du_index, unsigned nof_leases);
  std::optional<uint32_t>            allocate_ntn_rnti_lease_generation(du_index_t du_index);
  bool                               is_ntn_rnti_du_reconciled(du_index_t du_index) const;
  void report_ntn_location_to_core_if_required(const ntn_ue_location_report& report);
  bool send_ntn_location_report_to_core(const ngap_location_report& report);
  bool should_throttle_ntn_core_location_report(ue_index_t ue_index);
  bool is_ntn_serving_cell_core_reportable(nr_cell_identity nci) const;
  bool is_ntn_rrc_location_request_capability_allowed(const cu_cp_ue& ue, std::string& skipped_reason) const;
  bool is_ntn_rrc_location_request_desired(ue_index_t ue_index, const cu_cp_ue& ue, std::string& skipped_reason) const;
  void refresh_ntn_rrc_location_request_for_ue(ue_index_t ue_index, const char* reason);
  std::chrono::milliseconds get_ntn_location_lost_release_grace_period() const;
  bool is_ntn_location_watchdog_enabled() const;
  bool is_ntn_location_watchdog_release_temporarily_blocked(ue_index_t ue_index) const;
  void refresh_ntn_location_freshness_watchdog(const char* reason);
  void refresh_ntn_beam_placement_for_current_load();
  void clear_ntn_predictive_service_window();
  std::vector<ntn_served_beam_candidate>
  build_ntn_predictive_service_window_candidates(const ntn_served_beam_schedule&          current_schedule,
                                                 const std::vector<ntn_satellite_prediction_step>& future_steps,
                                                 const std::vector<ntn_served_beam_demand>&        demands);
  std::optional<std::string> find_ntn_beam_id_by_nci(nr_cell_identity nci) const;
  std::vector<std::pair<std::string, nr_cell_identity>> get_configured_ntn_beam_cells() const;
  bool refresh_ntn_beam_placement_for_service_policy();
  ntn_assistance_snapshot build_current_ntn_assistance_snapshot(std::chrono::steady_clock::time_point now) const;
  ntn_sib19_assistance_snapshot build_current_ntn_sib19_assistance_snapshot(std::chrono::steady_clock::time_point now) const;
  ntn_sib19_broadcast_snapshot
  build_current_ntn_sib19_broadcast_snapshot(std::chrono::steady_clock::time_point now) const;
  bool is_current_ntn_assistance_stale(std::chrono::steady_clock::time_point now) const;
  bool block_new_ntn_demand_if_assistance_is_stale(const char* demand_name);
  bool block_new_ntn_demand_if_service_policy_blocks(const char*                demand_name,
                                                     std::optional<std::string> beam_id,
                                                     std::optional<nr_cell_identity> beam_nci = std::nullopt);
  bool block_new_ntn_demand_if_predictive_window_blocks(const char*                demand_name,
                                                        std::optional<std::string> beam_id,
                                                        std::optional<nr_cell_identity> beam_nci = std::nullopt);
  bool block_new_ntn_demand_if_access_du_policy_blocks(const char*                demand_name,
                                                       std::optional<std::string> beam_id,
                                                       du_index_t                 ue_du_index,
                                                       bool                       require_service_du);
  bool block_new_ntn_access_if_resource_domain_blocks(std::optional<std::string> beam_id, ue_index_t ue_index);
  bool block_new_ntn_access_if_rnti_pool_unavailable(std::optional<std::string> beam_id, ue_index_t ue_index);
  bool block_new_ntn_pdu_session_demand_if_access_du_policy_blocks(
      std::optional<std::string>        beam_id,
      du_index_t                        ue_du_index,
      unsigned                          expected_drbs,
      const ntn_qos_demand_summary&     pending_qos);
  bool block_new_ntn_demand_if_ntn_policy_blocks(const char*                demand_name,
                                                 std::optional<std::string> beam_id,
                                                 std::optional<nr_cell_identity> beam_nci = std::nullopt);
  bool block_low_priority_ntn_pdu_session_demand_if_capacity_is_reserved(
      std::optional<std::string>        beam_id,
      unsigned                          expected_drbs,
      const ntn_qos_demand_summary&     pending_qos);
  bool block_low_priority_ntn_pdu_session_demand_if_headroom_is_reserved(
      std::optional<std::string>        beam_id,
      unsigned                          expected_drbs,
      const ntn_qos_demand_summary&     pending_qos);
  bool has_ntn_headroom_protected_handover_demand() const;
  unsigned get_nof_ntn_headroom_reserved_beams() const;
  bool is_ntn_headroom_reserved_beam(const std::string& beam_id) const;
  std::string get_ntn_headroom_reserved_beam_reason(const std::string& beam_id) const;
  bool prepare_ntn_pre_service_relocation_if_access_du_mismatch(ue_index_t                  ue_index,
                                                                std::optional<std::string> beam_id,
                                                                std::optional<nr_cell_identity> beam_nci,
                                                                du_index_t                 ue_du_index);
  bool block_new_ntn_pdu_session_demand_if_pre_service_relocation_pending(ue_index_t ue_index);
  ntn_ue_capability_summary evaluate_ntn_ue_capability_for_ue(const cu_cp_ue& ue) const;
  void schedule_ntn_pre_service_relocation_if_needed(ue_index_t ue_index);
  void handle_ntn_pre_service_relocation_result(const ntn_handover_result& result);
  std::optional<std::string> prepare_ntn_digital_service_binding_for_pdu_session(
      cu_cp_ue&                         ue,
      unsigned                          expected_drbs,
      const ntn_qos_demand_summary&     pending_qos);
  void commit_ntn_digital_service_binding_if_pdu_setup_succeeded(ue_index_t ue_index, bool success);
  void clear_ntn_digital_service_context_if_no_service_remains(ue_index_t ue_index);
  void release_ntn_analog_access_after_initial_context_setup(ue_index_t ue_index);
  struct ntn_initial_ul_position_ue_context;
  struct ntn_initial_ul_position_admission_result;
  ntn_initial_ul_position_admission_result
  evaluate_ntn_initial_ul_position(ue_index_t ue_index, std::optional<nr_cell_identity> serving_nci);
  void store_ntn_initial_ul_position_context(ue_index_t                                      ue_index,
                                             const ntn_initial_ul_position_ue_context& context);
  void erase_ntn_initial_ul_position_context(ue_index_t ue_index);
  void invalidate_ntn_initial_ul_position_state_locked(std::optional<du_index_t> du_index = std::nullopt);
  std::vector<ntn_beam_load> build_ntn_beam_loads_for_current_service_contexts();
  std::vector<ntn_served_beam_demand> build_ntn_served_beam_demands_for_current_service_contexts();
  void merge_pending_ntn_connected_handover_loads(std::vector<ntn_beam_load>& loads) const;
  bool prepare_ntn_connected_handover(ntn_location_handover_trigger& trigger);
  void handle_ntn_connected_handover_result(const ntn_handover_result& result);
  void clear_ntn_connected_handover_state_for_ue_removal(ue_index_t ue_index, const std::string& reason);

  struct ntn_core_location_reporting_ue_state {
    std::vector<ngap_location_reporting_request_type> active_requests;
    std::chrono::steady_clock::time_point             last_sent_time = {};
    bool                                             has_last_sent_time = false;
    std::optional<nr_cell_identity>                   last_reported_serving_nci;
  };

  struct ntn_rrc_location_request_ue_state {
    bool        desired    = false;
    bool        configured = false;
    bool        pending    = false;
    std::string skipped_reason = "none";
  };

  struct ntn_location_freshness_ue_state {
    std::string state  = "not_required";
    std::string reason = "not_required";
    std::optional<std::chrono::steady_clock::time_point> first_unfresh_time;
    bool release_pending = false;
    std::optional<std::chrono::milliseconds> location_age;
  };

  struct ntn_idle_paging_context {
    std::string                        authority = "legacy";
    std::optional<cu_cp_five_g_s_tmsi> five_g_s_tmsi;
    std::optional<std::string>         last_service_beam_id;
    std::optional<std::string>         last_downlink_wake_beam_id;
    std::optional<std::string>         paired_uplink_access_beam_id;
    std::optional<nr_cell_identity>    paired_uplink_access_nci;
    du_index_t                         paired_uplink_access_du_index = du_index_t::invalid;
    std::string                        paired_access_reason = "none";
    std::optional<std::string>         last_access_analog_beam_id;
    std::optional<nr_cell_identity>    last_serving_nci;
    std::optional<nr_cell_identity>    onboard_nci;
    std::optional<nr_cell_global_id_t> onboard_ncgi;
    std::optional<cu_cp_tai>           onboard_tai;
    uint64_t                           schedule_version = 0;
    std::string                        calendar_hash = "none";
    std::chrono::system_clock::time_point plan_valid_until = {};
    std::optional<tac_t>               last_derived_tac;
    std::optional<ntn_ue_location_report> last_location;
    std::chrono::steady_clock::time_point updated_time = {};
    std::string invalid_reason = "none";

    bool matches_onboard_route(const ntn_onboard_runtime_cell_route& route) const
    {
      return onboard_ncgi.has_value() && onboard_tai.has_value() && onboard_ncgi.value() == route.ncgi &&
             onboard_tai->plmn_id == route.ncgi.plmn_id && onboard_tai->tac == route.tac;
    }
  };

  struct ntn_onboard_ue_cell_view {
    std::shared_ptr<const ntn_onboard_runtime_mapping_snapshot> mapping;
    ntn_onboard_runtime_cell_route                              route;
  };

  struct ntn_inactive_context {
    std::optional<cu_cp_five_g_s_tmsi> five_g_s_tmsi;
    std::optional<uint64_t>            full_i_rnti;
    std::optional<uint32_t>            short_i_rnti;
    std::optional<std::string>         last_service_beam_id;
    std::optional<std::string>         last_downlink_wake_beam_id;
    std::optional<std::string>         paired_uplink_access_beam_id;
    std::optional<nr_cell_identity>    paired_uplink_access_nci;
    du_index_t                         paired_uplink_access_du_index = du_index_t::invalid;
    std::string                        paired_access_reason = "none";
    std::optional<std::string>         last_access_analog_beam_id;
    std::optional<nr_cell_identity>    last_serving_nci;
    std::optional<ntn_ue_location_report> last_location;
    std::chrono::steady_clock::time_point updated_time = {};
    std::string state  = "suspend_requested";
    std::string reason = "none";
  };

  struct ntn_predictive_beam_timeline_state {
    std::optional<std::chrono::milliseconds> first_entry_offset;
    std::optional<std::chrono::milliseconds> first_exit_offset;
  };

  struct ntn_pre_service_relocation_ue_state {
    std::string               state  = "none";
    std::string               reason = "none";
    du_index_t                source_du_index = du_index_t::invalid;
    du_index_t                target_du_index = du_index_t::invalid;
    std::string               target_beam_id;
    nr_cell_identity          serving_nci = nr_cell_identity::min();
    nr_cell_identity          target_nci  = nr_cell_identity::min();
    pci_t                     target_pci  = INVALID_PCI;
    uint64_t                  attempt_id  = 0;
    unsigned                  retry_count = 0;
  };

  struct ntn_connected_handover_ue_state {
    std::string               state  = "none";
    std::string               reason = "none";
    std::string               source_beam_id;
    std::string               target_beam_id;
    std::string               source_analog_beam_id;
    std::string               target_analog_beam_id;
    nr_cell_identity          serving_nci = nr_cell_identity::min();
    nr_cell_identity          target_nci  = nr_cell_identity::min();
    du_index_t                target_du_index = du_index_t::invalid;
    rnti_t                    target_c_rnti   = rnti_t::INVALID_RNTI;
    std::string               target_uplink_resource_beam_id;
    nr_cell_identity          target_uplink_resource_nci = nr_cell_identity::min();
    du_index_t                target_uplink_resource_du_index = du_index_t::invalid;
    bool                      has_target_uplink_resource_nci = false;
    std::string               target_service_pair_reason = "none";
    std::optional<f1ap_ntn_ul_slot_resource_request> target_ul_slot_request;
    std::string               target_resource_state = "none";
    bool                      target_sr_srs_applied = false;
    uint64_t                  attempt_id = 0;
    unsigned                  retry_count = 0;
    unsigned                  nof_drbs = 0;
    ntn_qos_demand_summary    qos;
  };

  struct ntn_ue_access_service_layer_state {
    std::string               access_state  = "none";
    std::string               access_reason = "none";
    std::string               access_analog_beam_id;
    du_index_t                access_du_index = du_index_t::invalid;
    nr_cell_identity          access_nci = nr_cell_identity::min();
    bool                      has_access_nci = false;
    std::string               last_downlink_wake_beam_id;
    std::string               paired_uplink_access_beam_id;
    nr_cell_identity          paired_uplink_access_nci = nr_cell_identity::min();
    bool                      has_paired_uplink_access_nci = false;
    du_index_t                paired_uplink_access_du_index = du_index_t::invalid;
    std::string               paired_access_reason = "none";
    std::string               service_state = "none";
    std::string               service_reason = "none";
    std::string               service_digital_beam_id;
    du_index_t                service_du_index = du_index_t::invalid;
    nr_cell_identity          service_nci = nr_cell_identity::min();
    bool                      has_service_nci = false;
    std::string               service_uplink_resource_beam_id;
    du_index_t                service_uplink_resource_du_index = du_index_t::invalid;
    nr_cell_identity          service_uplink_resource_nci = nr_cell_identity::min();
    bool                      has_service_uplink_resource_nci = false;
    std::string               service_pair_reason = "none";
    std::string               service_binding_source = "none";
    unsigned                  pending_drbs = 0;
    ntn_qos_demand_summary    pending_qos;
  };

  struct ntn_sib19_broadcast_record {
    std::string               beam_id;
    nr_cell_identity          nci = nr_cell_identity::min();
    ntn_sib19_broadcast_state state = ntn_sib19_broadcast_state::stale_blocked;
    std::string               reason = "not_configured";
    uint32_t                  generation_id = 0;
    unsigned                  packed_sib19_bytes = 0;
    uint32_t                  packed_sib19_hash = 0;
  };

  struct ntn_service_handover_target_selection {
    const ntn_beam_du_assignment* assignment = nullptr;
    const ntn_beam_du_assignment* uplink_resource_assignment = nullptr;
    std::string                   service_pair_reason = "none";

    bool uses_service_pair() const
    {
      return assignment != nullptr && uplink_resource_assignment != nullptr &&
             uplink_resource_assignment->beam_id != assignment->beam_id;
    }
  };

  struct ntn_load_balancing_target_selection {
    const ntn_beam_du_assignment* assignment = nullptr;
    const ntn_beam_du_assignment* uplink_resource_assignment = nullptr;
    bool                          same_analog = false;
    bool                          skipped_projected_capacity = false;
    bool                          skipped_cold_analog = false;
    bool                          skipped_pair_cooldown = false;
    bool                          skipped_preheat_ready_guard = false;
    std::string                   reason = "no_eligible_target";
    std::string                   source_analog_beam_id = "none";
    std::string                   target_analog_beam_id = "none";
    std::string                   preheat_beam_id = "none";
    std::string                   preheat_analog_beam_id = "none";
    std::string                   service_pair_reason = "none";

    bool uses_service_pair() const
    {
      return assignment != nullptr && uplink_resource_assignment != nullptr &&
             uplink_resource_assignment->beam_id != assignment->beam_id;
    }
  };

  struct ntn_beam_preheat_state {
    std::string target_beam_id = "none";
    std::string source_analog_beam_id = "none";
    std::string target_analog_beam_id = "none";
    std::string reason = "none";
    bool        sent = false;
    bool        applied = false;
    bool        ready = false;
    bool        skipped = false;
    std::chrono::steady_clock::time_point applied_time{};
  };

  struct ntn_target_reservation_state {
    std::string target_beam_id = "none";
    std::string source_beam_id = "none";
    std::string source_analog_beam_id = "none";
    std::string target_analog_beam_id = "none";
    std::string reason = "none";
    std::chrono::steady_clock::time_point created_time{};
    std::chrono::steady_clock::time_point last_held_time{};
  };

  std::optional<ntn_ue_access_service_layer_state> build_ntn_access_layer_state_for_ue(cu_cp_ue& ue);
  bool apply_ntn_paired_access_context_for_ue(ue_index_t ue_index, ntn_ue_access_service_layer_state& layer);
  void mark_ntn_access_released_after_ics(ntn_ue_access_service_layer_state& layer);
  void store_ntn_paired_access_context_for_paging(const cu_cp_five_g_s_tmsi&   five_g_s_tmsi,
                                                  const cu_cp_ntn_beam_status& selected_beam,
                                                  const char*                  reason);
  std::optional<std::string> select_ntn_digital_service_beam_for_ue(
      const cu_cp_ue&                              ue,
      const ntn_ue_access_service_layer_state&     access_state,
      unsigned                                     expected_drbs,
      const ntn_qos_demand_summary&                pending_qos,
      std::string&                                 binding_source,
      std::string&                                 binding_reason,
      std::optional<std::string>&                  service_uplink_resource_beam_id,
      std::string&                                 service_pair_reason) const;
  bool is_ntn_release_allowed_service_layer(
      const ntn_service_switch_over_snapshot&       switch_over_snapshot,
      const ntn_ue_access_service_layer_state&      layer) const;
  bool is_ntn_handover_preferred_service_layer(
      const ntn_service_switch_over_snapshot&       switch_over_snapshot,
      const ntn_ue_access_service_layer_state&      layer) const;
  bool is_ntn_beam_hopping_service_layer(
      const ntn_service_switch_over_snapshot&       switch_over_snapshot,
      const ntn_ue_access_service_layer_state&      layer) const;
  bool is_ntn_predictive_beam_hopping_service_layer(const ntn_ue_access_service_layer_state& layer) const;
  bool is_ntn_release_allowed_release_temporarily_blocked(ue_index_t ue_index) const;
  bool is_ntn_handover_preferred_temporarily_blocked(ue_index_t ue_index) const;
  bool is_ntn_load_balancing_handover_temporarily_blocked(ue_index_t ue_index) const;
  bool is_ntn_preheated_target_ready(const ntn_beam_preheat_state& state) const;
  bool is_ntn_analog_rebalance_pair_cooldown_active(const std::string& source_analog_beam_id,
                                                    const std::string& target_analog_beam_id) const;
  ntn_service_handover_target_selection select_ntn_service_handover_target_assignment(
      const ntn_service_switch_over_snapshot&       switch_over_snapshot,
      const ntn_ue_access_service_layer_state&      layer) const;
  ntn_load_balancing_target_selection select_ntn_load_balancing_target_assignment(
      const ntn_service_switch_over_snapshot&       switch_over_snapshot,
      const ntn_ue_access_service_layer_state&      layer,
      unsigned                                      moving_nof_drbs);
  bool is_ntn_load_balancing_source_hot(const ntn_ue_access_service_layer_state& layer,
                                        const ntn_load_balancing_target_selection& target_selection) const;
  void record_ntn_beam_preheat_request(const ntn_ue_access_service_layer_state& layer,
                                       const ntn_load_balancing_target_selection& target_selection,
                                       const char* reason);
  void record_ntn_target_reservation(const std::string& target_beam_id,
                                     const std::string& source_beam_id,
                                     const std::string& source_analog_beam_id,
                                     const std::string& target_analog_beam_id,
                                     const char* reason);
  void consume_ntn_target_reservation(const std::string& target_beam_id,
                                      const std::string& source_beam_id,
                                      const std::string& source_analog_beam_id,
                                      const std::string& target_analog_beam_id,
                                      const char* reason);
  bool expire_ntn_target_reservations();
  bool is_ntn_target_capacity_reserved_for_beam(const std::string& beam_id) const;
  bool update_ntn_preheat_results(const ntn_beam_placement_plan& plan);
  bool expire_idle_ntn_preheated_beams();
  bool schedule_ntn_service_handover(
      ue_index_t                                    ue_index,
      const ntn_ue_access_service_layer_state&      layer,
      const ntn_service_handover_target_selection&  target_selection,
      const char*                                   handover_reason);
  void schedule_ntn_multi_beam_load_balancing_handovers();
  void schedule_ntn_beam_hopping_service_handovers();
  void schedule_ntn_predictive_beam_hopping_service_handovers();
  void schedule_ntn_handover_preferred_service_handovers();
  void schedule_ntn_release_allowed_service_releases();

  void on_statistics_report_timer_expired();
  enum class ntn_state_persist_outcome { durable, not_committed, committed_not_durable };
  using ntn_position_plan_clear_entry = std::pair<ntn_activated_position_plan, std::string>;
  void reload_ntn_onboard_position_plan();
  void restore_ntn_onboard_position_plan_state();
  ntn_state_persist_outcome persist_new_signed_ntn_position_plan_locked(
      const ntn_versioned_position_plan& plan, const char* reason);
  ntn_state_persist_outcome persist_ntn_onboard_position_plan_state_locked(const char* reason,
                                                                            bool omit_clear_queue_head = false);
  void restore_ntn_onboard_position_plan_checkpoint_fail_closed_locked(
      const ntn_onboard_position_plan_controller&       controller_checkpoint,
      const std::vector<ntn_position_plan_clear_entry>& clear_queue_checkpoint,
      std::chrono::system_clock::time_point             now);
  void refresh_ntn_onboard_runtime_mapping(std::chrono::system_clock::time_point now, const char* reason);
  void invalidate_ntn_onboard_runtime_mapping_locked(ntn_onboard_runtime_mapping_stage stage, const char* detail);
  expected<std::shared_ptr<const ntn_onboard_runtime_mapping_snapshot>, std::string>
  build_ntn_onboard_runtime_mapping_snapshot(const ntn_activated_position_plan&    plan,
                                             const std::set<du_index_t>&           disconnected_dus,
                                             const std::map<du_index_t, uint64_t>& du_generations);
  std::shared_ptr<const ntn_onboard_runtime_mapping_snapshot>
  get_ready_ntn_onboard_runtime_mapping(std::chrono::system_clock::time_point now) const;
  void try_prepare_ntn_onboard_position_plan();
  void query_ntn_onboard_position_plan_application();
  bool queue_ntn_onboard_position_plan_clear_locked(const ntn_activated_position_plan& plan, std::string reason);
  void try_clear_ntn_onboard_position_plan_deployment();
  void schedule_ntn_onboard_position_plan_reload();
  void schedule_ntn_onboard_position_plan_activation();
  void on_ntn_onboard_position_plan_activation_timer_expired();
  void on_ntn_onboard_position_plan_reload_timer_expired();

  cu_cp_configuration cfg;

  // Logger.
  srslog::basic_logger& logger = srslog::fetch_basic_logger("CU-CP");

  // Components.
  // UE manager.
  ue_manager ue_mng;

  // Cell measurement manager.
  cell_meas_manager cell_meas_mng;

  std::optional<ntn_served_beam_scheduler> ntn_served_beam_sched;
  mutable std::mutex                                    ntn_onboard_position_plan_mutex;
  std::optional<ntn_onboard_position_plan_controller> ntn_onboard_position_plan_ctrl;
  std::shared_ptr<const ntn_onboard_runtime_mapping_snapshot> ntn_onboard_runtime_mapping;
  ntn_onboard_runtime_mapping_stage                          current_ntn_onboard_runtime_mapping_stage =
      ntn_onboard_runtime_mapping_stage::disabled;
  std::string current_ntn_onboard_runtime_mapping_detail = "feature_disabled";
  struct ntn_initial_ul_position_ue_context {
    uint64_t            observation_id = 0;
    std::string         position_id;
    nr_cell_identity    nci = nr_cell_identity::min();
    pci_t               pci = INVALID_PCI;
    du_index_t          du_index = du_index_t::invalid;
    du_cell_index_t     du_cell_index = du_cell_index_t::invalid;
    rnti_t              c_rnti = rnti_t::INVALID_RNTI;
    uint64_t            schedule_version = 0;
    std::string         calendar_hash;
    uint64_t            du_connection_generation = 0;
  };
  struct ntn_initial_ul_position_admission_result {
    bool                                               applicable = false;
    bool                                               allowed    = true;
    std::string                                        reason     = "not_applicable";
    std::optional<ntn_initial_ul_position_ue_context> context;
  };
  std::optional<ntn_initial_ul_position_authorizer> initial_ul_position_authorizer;
  std::unordered_map<ue_index_t, ntn_initial_ul_position_ue_context> ntn_initial_ul_position_contexts;
  uint64_t    nof_ntn_initial_ul_position_accepted = 0;
  uint64_t    nof_ntn_initial_ul_position_rejected = 0;
  uint64_t    nof_ntn_initial_ul_position_audited  = 0;
  uint64_t    nof_ntn_initial_ul_position_expired  = 0;
  uint64_t    nof_ntn_initial_ul_position_replayed = 0;
  std::string last_ntn_initial_ul_position_reason  = "none";
  std::string                                           last_ntn_position_plan_file_signature;
  std::optional<std::chrono::steady_clock::time_point>  ntn_position_plan_reload_deadline;
  bool                                                   ntn_position_plan_query_in_flight = false;
  bool                                                   ntn_position_plan_clear_in_flight = false;
  std::optional<du_index_t>                              ntn_position_plan_query_du_index;
  std::optional<du_index_t>                              ntn_position_plan_clear_du_index;
  uint64_t                                               ntn_position_plan_query_request_id = 0;
  uint64_t                                               ntn_position_plan_clear_request_id = 0;
  std::map<du_index_t, uint64_t>                         ntn_position_plan_du_connection_generations;
  std::set<du_index_t>                                   ntn_position_plan_disconnected_dus;
  std::optional<std::pair<uint64_t, std::string>>         ntn_position_plan_prepare_dispatched;
  uint64_t                                                 ntn_position_plan_static_preflight_schedule_version = 0;
  std::array<f1ap_ntn_access_calendar_preflight_report, 2> ntn_position_plan_static_preflight_reports{};
  std::vector<ntn_position_plan_clear_entry>                       ntn_position_plan_clear_queue;
  unsigned                                                         ntn_position_plan_state_schema_version = 0;
  uint64_t                                                         ntn_position_plan_state_generation = 0;
  std::string                                                      ntn_position_plan_state_hash;
  std::string                                                      ntn_position_plan_state_store_status = "disabled";
  std::string                                                      ntn_position_plan_state_error;
  int64_t                                                          ntn_position_plan_state_last_save_unix_ms = -1;
  bool                                                             ntn_position_plan_state_write_blocked     = false;
  std::optional<ntn_plan_version_anchor_state>                      ntn_position_plan_version_anchor;
  std::string                                                       ntn_position_plan_version_anchor_status = "disabled";
  std::string                                                       ntn_position_plan_version_anchor_error  = "none";
  std::string                                                       ntn_position_plan_version_anchor_hash;

  ntn_beam_placement_planner ntn_beam_planner;
  ntn_beam_placement_plan    current_ntn_beam_placement_plan;
  std::vector<ntn_served_beam_candidate> current_ntn_served_beam_candidates;
  std::vector<std::string>   current_ntn_mobility_eligible_beam_ids;
  ntn_service_switch_over_controller ntn_service_switch_over_ctrl;
  std::set<nr_cell_identity> current_ntn_core_reportable_ncis;
  std::optional<ecef_coordinates_t> current_ntn_satellite_ecef;
  std::vector<ntn_satellite_state> current_ntn_satellite_states;
  std::chrono::system_clock::time_point current_ntn_satellite_epoch{};
  std::optional<std::chrono::steady_clock::time_point> current_ntn_satellite_received_time;
  bool current_ntn_predictive_window_valid = false;
  std::set<std::string> current_ntn_predictive_upcoming_beam_ids;
  std::set<std::string> current_ntn_predictive_drain_soon_beam_ids;
  std::map<std::string, ntn_predictive_beam_timeline_state> current_ntn_predictive_beam_timeline;
  std::chrono::milliseconds current_ntn_predictive_horizon{0};
  std::chrono::milliseconds current_ntn_predictive_lead_time{0};
  unsigned current_ntn_predictive_timeline_steps = 0;
  std::optional<std::chrono::milliseconds> current_ntn_earliest_predictive_upcoming_offset;
  std::optional<std::chrono::milliseconds> current_ntn_earliest_predictive_drain_offset;
  std::map<std::string, std::string> current_ntn_beam_satellite_ids;
  std::map<std::string, std::string> current_ntn_next_beam_satellite_ids;
  std::set<std::string> current_ntn_current_window_satellite_ids;
  std::set<std::string> current_ntn_next_window_satellite_ids;
  unsigned current_ntn_satellite_owner_change_count = 0;

  std::unique_ptr<ntn_satellite_state_updater> ntn_satellite_updater;

  std::unordered_map<ue_index_t, ntn_core_location_reporting_ue_state> ntn_core_location_reporting_states;
  std::unordered_map<ue_index_t, ntn_rrc_location_request_ue_state>     ntn_rrc_location_request_states;
  std::unordered_map<ue_index_t, ntn_location_freshness_ue_state>       ntn_location_freshness_states;
  std::unordered_map<ue_index_t, cu_cp_five_g_s_tmsi>                   ntn_connected_ue_five_g_s_tmsi;
  std::unordered_map<uint64_t, ntn_idle_paging_context>                 ntn_idle_paging_contexts;
  std::unordered_map<uint64_t, ntn_idle_paging_context>                 ntn_pending_paired_access_contexts;
  std::unordered_map<ue_index_t, ntn_inactive_context>                  ntn_inactive_contexts;
  ntn_beam_service_resource_manager                                    ntn_service_resource_mng;
  std::unordered_map<ue_index_t, ntn_pre_service_relocation_ue_state>  ntn_pre_service_relocation_states;
  std::unordered_map<ue_index_t, ntn_connected_handover_ue_state>       ntn_connected_handover_states;
  std::unordered_map<ue_index_t, std::chrono::steady_clock::time_point> ntn_load_balancing_last_handover_times;
  std::unordered_map<ue_index_t, ntn_ue_access_service_layer_state>      ntn_ue_layer_states;
  std::unordered_map<std::string, ntn_sib19_broadcast_record>            ntn_sib19_broadcast_records;
  std::unordered_map<std::string, ntn_beam_preheat_state>                 ntn_preheat_states;
  std::unordered_map<std::string, ntn_target_reservation_state>           ntn_target_reservations;
  std::unordered_map<std::string, std::chrono::steady_clock::time_point>  ntn_analog_rebalance_pair_last_times;
  std::set<ue_index_t>                                                  ntn_release_allowed_release_requested_ues;
  std::set<ue_index_t>                                                  ntn_location_watchdog_release_requested_ues;
  uint64_t                                                             next_ntn_pre_service_relocation_attempt_id = 1;
  uint64_t                                                             next_ntn_service_switch_over_handover_attempt_id = 1;
  enum class ntn_rnti_retirement_capability { unknown, supported, unsupported };
  struct ntn_rnti_du_reconciliation_state {
    uint64_t                                            connection_generation = 0;
    bool                                                connected             = false;
    ntn_rnti_retirement_capability                      retirement_capability = ntn_rnti_retirement_capability::unknown;
    uint32_t                                            generation_high_water = 0;
    bool                                                generation_exhausted  = false;
    std::set<std::pair<srsran::du_cell_index_t, pci_t>> expected_targets;
    std::set<std::pair<srsran::du_cell_index_t, pci_t>> reconciled_targets;
  };
  std::map<du_index_t, ntn_rnti_du_reconciliation_state>                     ntn_rnti_du_reconciliation_states;
  std::map<du_index_t, uint16_t>                                             next_ntn_rnti_lease_value_by_du;
  std::map<std::tuple<du_index_t, srsran::du_cell_index_t, pci_t>, uint32_t> ntn_rnti_audits_in_flight;
  uint32_t                                                             next_ntn_resource_audit_generation_id = 1;
  uint32_t                                                             next_ntn_sib19_broadcast_generation_id = 1;
  uint32_t                                                             last_ntn_resource_audit_generation = 0;
  unsigned                                                             nof_ntn_resource_audit_queries_sent = 0;
  unsigned                                                             nof_ntn_resource_audit_responses_accepted = 0;
  unsigned                                                             nof_ntn_resource_audit_mismatches = 0;
  unsigned                                                             nof_ntn_resource_audit_repair_actions = 0;
  unsigned                                                             nof_ntn_resource_audit_failures = 0;
  unsigned                                                             nof_ntn_resource_audit_rnti_incomplete = 0;
  unsigned                                                             nof_ntn_resource_audit_ue_slot_incomplete = 0;
  std::string                                                          last_ntn_resource_audit_reason = "none";
  unsigned                                                             nof_ntn_rnti_retire_unsupported   = 0;
  unsigned                                                             nof_ntn_rnti_namespace_exhausted  = 0;
  unsigned                                                             nof_ntn_rnti_generation_exhausted = 0;
  std::string                                                          last_ntn_rnti_retirement_reason   = "none";
  unsigned                                                             nof_ntn_service_pair_resource_audit_targets = 0;
  unsigned                                                             nof_ntn_service_pair_resource_audit_mismatches = 0;
  unsigned                                                             nof_ntn_service_pair_resource_audit_repairs = 0;
  unsigned                                                             nof_ntn_service_pair_resource_audit_skipped = 0;
  std::string                                                          last_ntn_service_pair_resource_audit_reason = "none";
  unsigned                                                             nof_ntn_release_allowed_ues_requested = 0;
  unsigned                                                             nof_ntn_release_allowed_ues_scheduled = 0;
  unsigned                                                             nof_ntn_release_allowed_ues_skipped = 0;
  unsigned                                                             nof_ntn_beam_hopping_ues_requested = 0;
  unsigned                                                             nof_ntn_beam_hopping_ues_scheduled = 0;
  unsigned                                                             nof_ntn_beam_hopping_ues_skipped = 0;
  unsigned                                                             nof_ntn_predictive_beam_hopping_ues_requested = 0;
  unsigned                                                             nof_ntn_predictive_beam_hopping_ues_scheduled = 0;
  unsigned                                                             nof_ntn_predictive_beam_hopping_ues_skipped = 0;
  unsigned                                                             nof_ntn_handover_preferred_ues_requested = 0;
  unsigned                                                             nof_ntn_handover_preferred_ues_scheduled = 0;
  unsigned                                                             nof_ntn_handover_preferred_ues_skipped = 0;
  unsigned                                                             nof_ntn_load_balancing_evaluations = 0;
  unsigned                                                             nof_ntn_load_balancing_admission_steered = 0;
  unsigned                                                             nof_ntn_load_balancing_handover_requested = 0;
  unsigned                                                             nof_ntn_load_balancing_handover_scheduled = 0;
  unsigned                                                             nof_ntn_load_balancing_handover_skipped = 0;
  unsigned                                                             nof_ntn_load_balancing_same_analog_scheduled = 0;
  unsigned                                                             nof_ntn_load_balancing_cross_analog_scheduled = 0;
  unsigned                                                             nof_ntn_load_balancing_skipped_projected_capacity = 0;
  unsigned                                                             nof_ntn_load_balancing_skipped_cold_analog = 0;
  std::string                                                          last_ntn_load_balancing_reason = "none";
  std::string                                                          last_ntn_load_balancing_source_beam_id = "none";
  std::string                                                          last_ntn_load_balancing_target_beam_id = "none";
  std::string                                                          last_ntn_load_balancing_source_analog_id = "none";
  std::string                                                          last_ntn_load_balancing_target_analog_id = "none";
  unsigned                                                             nof_ntn_service_pair_handover_targets = 0;
  unsigned                                                             nof_ntn_service_pair_handover_scheduled = 0;
  unsigned                                                             nof_ntn_service_pair_handover_skipped = 0;
  unsigned                                                             nof_ntn_service_pair_handover_committed = 0;
  unsigned                                                             nof_ntn_service_pair_handover_rolled_back = 0;
  unsigned                                                             nof_ntn_service_pair_handover_context_cleared = 0;
  std::string                                                          last_ntn_service_pair_handover_reason = "none";
  std::string                                                          last_ntn_service_pair_handover_completion_reason = "none";
  unsigned                                                             nof_ntn_preheat_requested = 0;
  unsigned                                                             nof_ntn_preheat_sent = 0;
  unsigned                                                             nof_ntn_preheat_applied = 0;
  unsigned                                                             nof_ntn_preheat_skipped = 0;
  unsigned                                                             nof_ntn_preheat_demoted = 0;
  unsigned                                                             nof_ntn_preheat_skipped_by_capacity = 0;
  unsigned                                                             nof_ntn_preheat_skipped_by_policy = 0;
  unsigned                                                             nof_ntn_cold_analog_preheat_requested = 0;
  std::string                                                          last_ntn_preheat_reason = "none";
  std::string                                                          last_ntn_preheat_source_analog_id = "none";
  std::string                                                          last_ntn_preheat_target_analog_id = "none";
  unsigned                                                             nof_ntn_target_reservation_created = 0;
  unsigned                                                             nof_ntn_target_reservation_held = 0;
  unsigned                                                             nof_ntn_target_reservation_consumed = 0;
  unsigned                                                             nof_ntn_target_reservation_expired = 0;
  unsigned                                                             nof_ntn_admission_blocked_by_target_reservation = 0;
  unsigned                                                             nof_ntn_handover_skipped_by_pair_cooldown = 0;
  unsigned                                                             nof_ntn_handover_skipped_by_preheat_ready_guard = 0;
  unsigned                                                             nof_ntn_preheat_demote_deferred_by_reservation = 0;
  std::string                                                          last_ntn_scheduling_guard_reason = "none";
  std::string                                                          last_ntn_scheduling_guard_source_beam_id = "none";
  std::string                                                          last_ntn_scheduling_guard_target_beam_id = "none";
  std::string                                                          last_ntn_scheduling_guard_source_analog_id = "none";
  std::string                                                          last_ntn_scheduling_guard_target_analog_id = "none";
  unsigned                                                             nof_ntn_beam_scheduling_evaluations = 0;
  unsigned                                                             nof_ntn_beam_scheduling_demand_prioritized_windows = 0;
  unsigned                                                             nof_ntn_beam_scheduling_legacy_fallback = 0;
  unsigned                                                             nof_ntn_beam_scheduling_sticky_kept = 0;
  std::string                                                          last_ntn_beam_scheduling_reason = "none";
  unsigned                                                             nof_ntn_resource_weighting_evaluations = 0;
  unsigned                                                             nof_ntn_resource_weighting_weighted_beams = 0;
  unsigned                                                             nof_ntn_resource_weighting_qos_boosted_beams = 0;
  unsigned                                                             nof_ntn_resource_weighting_legacy_fallback = 0;
  std::string                                                          last_ntn_resource_weighting_reason = "none";
  unsigned                                                             nof_ntn_headroom_evaluations = 0;
  unsigned                                                             nof_ntn_headroom_admission_allowed = 0;
  unsigned                                                             nof_ntn_headroom_admission_blocked = 0;
  unsigned                                                             nof_ntn_headroom_handover_protected = 0;
  std::string                                                          last_ntn_headroom_reason = "none";
  unsigned                                                             nof_ntn_rrc_location_reports_received = 0;
  unsigned                                                             nof_ntn_rrc_location_reports_decoded = 0;
  unsigned                                                             nof_ntn_rrc_location_reports_unsupported = 0;
  unsigned                                                             nof_ntn_rrc_location_reports_decode_failed = 0;
  unsigned                                                             nof_ntn_location_reports_accepted = 0;
  unsigned                                                             nof_ntn_location_reports_rejected = 0;
  unsigned                                                             nof_ntn_location_watchdog_evaluations = 0;
  unsigned                                                             nof_ntn_location_watchdog_refresh_requested = 0;
  unsigned                                                             nof_ntn_location_watchdog_release_requested = 0;
  unsigned                                                             nof_ntn_location_watchdog_release_scheduled = 0;
  unsigned                                                             nof_ntn_location_watchdog_release_skipped = 0;
  std::string                                                          last_ntn_location_watchdog_release_reason = "none";
  unsigned                                                             nof_ntn_rrc_location_request_configs_included = 0;
  unsigned                                                             nof_ntn_rrc_location_request_configs_removed = 0;
  unsigned                                                             nof_ntn_rrc_location_request_configs_skipped_capability = 0;
  unsigned                                                             nof_ntn_rrc_location_request_configs_skipped_state = 0;
  unsigned                                                             nof_ntn_rrc_location_request_reconfig_sent = 0;
  unsigned                                                             nof_ntn_rrc_location_request_reconfig_failed = 0;
  unsigned                                                             nof_ntn_nrppa_dl_ue_received = 0;
  unsigned                                                             nof_ntn_nrppa_dl_ue_forwarded = 0;
  unsigned                                                             nof_ntn_nrppa_dl_ue_dropped = 0;
  unsigned                                                             nof_ntn_nrppa_dl_non_ue_received = 0;
  unsigned                                                             nof_ntn_nrppa_dl_non_ue_forwarded = 0;
  unsigned                                                             nof_ntn_nrppa_dl_non_ue_dropped = 0;
  unsigned                                                             nof_ntn_nrppa_ul_ue_received = 0;
  unsigned                                                             nof_ntn_nrppa_ul_ue_sent = 0;
  unsigned                                                             nof_ntn_nrppa_ul_ue_dropped = 0;
  unsigned                                                             nof_ntn_nrppa_ul_non_ue_received = 0;
  unsigned                                                             nof_ntn_nrppa_ul_non_ue_sent = 0;
  unsigned                                                             nof_ntn_nrppa_ul_non_ue_dropped = 0;
  std::string                                                          last_ntn_nrppa_dropped_reason = "none";
  unsigned                                                             nof_ntn_nrppa_trp_requests_received = 0;
  unsigned                                                             nof_ntn_nrppa_trp_requests_decoded = 0;
  unsigned                                                             nof_ntn_nrppa_trp_responses_sent = 0;
  unsigned                                                             nof_ntn_nrppa_trp_failures_sent = 0;
  unsigned                                                             nof_ntn_nrppa_unsupported_procedures = 0;
  unsigned                                                             nof_ntn_nrppa_trp_unsupported_info_items = 0;
  unsigned                                                             nof_ntn_nrppa_trp_empty_results = 0;
  std::string                                                          last_ntn_nrppa_trp_reason = "none";
  unsigned                                                             nof_ntn_nrppa_standard_decode_success = 0;
  unsigned                                                             nof_ntn_nrppa_standard_decode_failure = 0;
  unsigned                                                             nof_ntn_nrppa_standard_encode_responses = 0;
  unsigned                                                             nof_ntn_nrppa_standard_encode_failures = 0;
  unsigned                                                             nof_ntn_nrppa_minimal_fallback_decodes = 0;
  std::string                                                          last_ntn_nrppa_standard_decode_reason = "none";
  unsigned                                                             nof_ntn_nrppa_positioning_info_requests_received = 0;
  unsigned                                                             nof_ntn_nrppa_positioning_info_requests_decoded = 0;
  unsigned                                                             nof_ntn_nrppa_positioning_info_requests_forwarded = 0;
  unsigned                                                             nof_ntn_nrppa_positioning_info_responses_sent = 0;
  unsigned                                                             nof_ntn_nrppa_positioning_info_failures_sent = 0;
  unsigned                                                             nof_ntn_nrppa_positioning_info_dropped = 0;
  std::string                                                          last_ntn_nrppa_positioning_info_reason = "none";
  unsigned                                                             nof_ntn_nrppa_measurement_requests_received = 0;
  unsigned                                                             nof_ntn_nrppa_measurement_requests_decoded = 0;
  unsigned                                                             nof_ntn_nrppa_measurement_requests_forwarded = 0;
  unsigned                                                             nof_ntn_nrppa_measurement_responses_sent = 0;
  unsigned                                                             nof_ntn_nrppa_measurement_failures_sent = 0;
  unsigned                                                             nof_ntn_nrppa_measurement_dropped = 0;
  std::string                                                          last_ntn_nrppa_measurement_reason = "none";
  unsigned                                                             nof_ntn_nrppa_activation_requests_received = 0;
  unsigned                                                             nof_ntn_nrppa_activation_requests_decoded = 0;
  unsigned                                                             nof_ntn_nrppa_activation_requests_forwarded = 0;
  unsigned                                                             nof_ntn_nrppa_activation_responses_sent = 0;
  unsigned                                                             nof_ntn_nrppa_activation_failures_sent = 0;
  unsigned                                                             nof_ntn_nrppa_activation_dropped = 0;
  std::string                                                          last_ntn_nrppa_activation_reason = "none";
  unsigned                                                             nof_ntn_nrppa_deactivation_requests_received = 0;
  unsigned                                                             nof_ntn_nrppa_deactivation_requests_decoded = 0;
  unsigned                                                             nof_ntn_nrppa_deactivation_requests_forwarded = 0;
  unsigned                                                             nof_ntn_nrppa_deactivation_acks_sent = 0;
  unsigned                                                             nof_ntn_nrppa_deactivation_failures_sent = 0;
  unsigned                                                             nof_ntn_nrppa_deactivation_dropped = 0;
  std::string                                                          last_ntn_nrppa_deactivation_reason = "none";
  unsigned                                                             nof_ntn_nrppa_assistance_control_requests_received = 0;
  unsigned                                                             nof_ntn_nrppa_assistance_control_requests_decoded = 0;
  unsigned                                                             nof_ntn_nrppa_assistance_control_requests_forwarded = 0;
  unsigned                                                             nof_ntn_nrppa_assistance_control_feedbacks_sent = 0;
  unsigned                                                             nof_ntn_nrppa_assistance_control_failures_sent = 0;
  unsigned                                                             nof_ntn_nrppa_assistance_control_dropped = 0;
  unsigned                                                             nof_ntn_nrppa_assistance_control_unsupported_fields = 0;
  std::string                                                          last_ntn_nrppa_assistance_control_reason = "none";
  unsigned                                                             nof_ntn_idle_paging_ue_hits = 0;
  unsigned                                                             nof_ntn_idle_paging_tac_fallbacks = 0;
  unsigned                                                             nof_ntn_idle_paging_recommendations = 0;
  unsigned                                                             nof_ntn_idle_paging_skipped = 0;
  unsigned                                                             nof_ntn_idle_paging_expired = 0;
  std::string                                                          last_ntn_idle_paging_reason = "none";
  unsigned                                                             nof_ntn_paired_access_responses = 0;
  unsigned                                                             nof_ntn_service_bindings_from_paired_access = 0;
  unsigned                                                             nof_ntn_paired_access_blocked = 0;
  std::string                                                          last_ntn_paired_access_reason = "none";
  unsigned                                                             nof_ntn_service_pair_blocked = 0;
  std::string                                                          last_ntn_service_pair_reason = "none";
  unsigned                                                             nof_ntn_inactive_suspend_requested = 0;
  unsigned                                                             nof_ntn_inactive_suspend_succeeded = 0;
  unsigned                                                             nof_ntn_inactive_suspend_failed = 0;
  unsigned                                                             nof_ntn_inactive_resume_requested = 0;
  unsigned                                                             nof_ntn_inactive_resume_succeeded = 0;
  unsigned                                                             nof_ntn_inactive_resume_failed = 0;
  unsigned                                                             nof_ntn_inactive_ngap_suspend_responses = 0;
  unsigned                                                             nof_ntn_inactive_ngap_suspend_failures = 0;
  unsigned                                                             nof_ntn_inactive_ngap_resume_responses = 0;
  unsigned                                                             nof_ntn_inactive_ngap_resume_failures = 0;
  unsigned                                                             nof_ntn_inactive_contexts_expired = 0;
  unsigned                                                             nof_ntn_inactive_paging_hits = 0;
  unsigned                                                             nof_ntn_inactive_fallback_releases = 0;
  std::string                                                          last_ntn_inactive_reason = "none";

  cu_cp_common_task_scheduler common_task_sched;
  /// Calendar F1 transactions use independent lanes so a lost prepare/query cannot delay rollback clear.
  fifo_async_task_scheduler ntn_position_plan_prepare_io_sched{8};
  fifo_async_task_scheduler ntn_position_plan_query_io_sched{8};
  fifo_async_task_scheduler ntn_position_plan_clear_io_sched{8};

  // DU repository to Node Manager adapter.
  du_processor_cu_cp_connection_adapter conn_notifier;

  // Cell Measurement Manager to mobility manager adapters.
  cell_meas_mobility_manager_adapter cell_meas_mobility_notifier;

  // E1AP to CU-CP adapter.
  e1ap_cu_cp_adapter e1ap_ev_notifier;

  // NGAP to CU-CP adapters.
  ngap_cu_cp_adapter ngap_cu_cp_ev_notifier;

  // Mobility manager to CU-CP adapter.
  mobility_manager_adapter mobility_manager_ev_notifier;

  // DU connections being managed by the CU-CP.
  du_processor_repository du_db;

  // CU-UP connections being managed by the CU-CP.
  cu_up_processor_repository cu_up_db;

  // NRPPa to CU-CP adapter.
  nrppa_cu_cp_adapter nrppa_cu_cp_ev_notifier;

  // NRPPa to F1AP adapter.
  std::map<du_index_t, nrppa_f1ap_adapter> nrppa_f1ap_ev_notifiers;

  // NRPPA entity.
  std::unique_ptr<nrppa_interface> nrppa_entity;

  // Handler of paging messages.
  paging_message_handler paging_handler;

  // AMF connections beeing managed by the CU-CP.
  ngap_repository ngap_db;

  // Mobility manager.
  mobility_manager mobility_mng;

  // Handler of the CU-CP connections to other remote nodes (e.g. AMF, CU-UPs, DUs).
  cu_cp_controller controller;

  std::unique_ptr<metrics_handler> metrics_hdlr;

  unique_timer statistics_report_timer;
  unique_timer ntn_resource_audit_timer;
  unique_timer ntn_position_plan_reload_timer;
  unique_timer ntn_position_plan_activation_timer;

  std::atomic<bool> stopped{false};

  cu_configurator_impl cu_cp_cfgtr;

  // Metrics report session for the lifetime of the CU-CP.
  // Used, e.g., for logging metrics and JSON metrics.
  std::unique_ptr<metrics_report_session> metrics_session;
};

} // namespace srs_cu_cp
} // namespace srsran
