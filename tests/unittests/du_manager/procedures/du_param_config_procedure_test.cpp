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

#include "../du_manager_test_helpers.h"
#include "srsran/du/du_cell_config_helpers.h"
#include "srsran/du/du_high/du_manager/du_manager_factory.h"
#include "srsran/support/async/async_test_utils.h"
#include "srsran/support/executors/task_worker.h"
#include <gtest/gtest.h>

using namespace srsran;
using namespace srs_du;

class du_manager_procedure_tester
{
public:
  du_manager_procedure_tester(
      std::vector<du_cell_config> cfgs = {config_helpers::make_default_du_cell_config()},
      std::optional<unsigned>     nof_cells_to_activate = std::nullopt) :
    cell_cfgs(cfgs), dependencies(cell_cfgs), du_mng(create_du_manager(dependencies.params))
  {
    // Generate automatic responses from F1AP and MAC.
    const unsigned active_cell_count = nof_cells_to_activate.value_or(cfgs.size());
    srsran_assert(active_cell_count <= cfgs.size(),
                  "Requested {} active cells but only {} cells are configured",
                  active_cell_count,
                  cfgs.size());
    dependencies.f1ap.wait_f1_setup.result.value().cells_to_activate.resize(active_cell_count);
    for (unsigned i = 0; i != active_cell_count; ++i) {
      dependencies.f1ap.wait_f1_setup.result.value().cells_to_activate[i].cgi = cell_cfgs[i].nr_cgi;
    }
    dependencies.f1ap.wait_f1_setup.ready_ev.set();
    dependencies.f1ap.wait_f1_removal.ready_ev.set();
    dependencies.mac.mac_cell.wait_start.ready_ev.set();
    dependencies.mac.mac_cell.wait_stop.ready_ev.set();

    // Start DU manager.
    du_mng->start();
  }
  ~du_manager_procedure_tester()
  {
    std::atomic<bool> done{false};
    worker.push_task_blocking([this, &done]() {
      du_mng->stop();
      done = true;
    });
    while (not done) {
      dependencies.worker.run_pending_tasks();
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    worker.wait_pending_tasks();
  }

  task_worker                           worker{"worker", 16};
  std::vector<du_cell_config>           cell_cfgs;
  du_manager_test_bench                 dependencies;
  std::unique_ptr<du_manager_interface> du_mng;
};

static du_param_config_request make_dummy_request(span<const du_cell_config> cell_cfgs)
{
  du_param_config_request req;
  req.cells.resize(1);
  req.cells[0].nr_cgi      = cell_cfgs[0].nr_cgi;
  req.cells[0].ssb_pwr_mod = cell_cfgs[0].ssb_cfg.ssb_block_power + 1;
  return req;
}

class du_manager_du_config_update_test : public du_manager_procedure_tester, public ::testing::Test
{};

TEST_F(du_manager_du_config_update_test, when_sib1_change_required_then_mac_is_reconfigured_and_f1ap_initiates_du_cfg)
{
  // Initiate procedure.
  du_param_config_request  req  = make_dummy_request(cell_cfgs);
  du_param_config_response resp = du_mng->handle_operator_config_request(req);

  // MAC received config request.
  ASSERT_TRUE(dependencies.mac.mac_cell.last_cell_recfg_req.has_value());

  // F1AP received the DU config request.
  ASSERT_TRUE(dependencies.f1ap.last_du_cfg_req);

  ASSERT_TRUE(resp.success);
}

TEST_F(du_manager_du_config_update_test, check_if_slot_time_mapping_is_available)
{
  mac_cell_time_mapper& mapper = du_mng->get_time_mapper();
  auto                  resp   = mapper.get_last_mapping();
  ASSERT_TRUE(resp.has_value());
  ASSERT_EQ(resp.value().sl_tx, slot_point(1, 1));
}

static f1ap_ntn_rnti_lease_pool_update make_ntn_rnti_lease_pool_update(const du_cell_config& cell)
{
  f1ap_ntn_rnti_lease_pool_update request;
  request.gnb_du_id     = int_to_gnb_du_id(1);
  request.cell_index    = to_du_cell_index(0);
  request.cell_cgi      = cell.nr_cgi;
  request.pci           = cell.pci;
  request.generation_id = 77;
  request.expiry_ms     = 30000;
  request.operation     = f1ap_ntn_rnti_lease_pool_operation::replace;
  request.leases        = {to_rnti(0x4701), to_rnti(0x4702)};
  return request;
}

static f1ap_ntn_resource_audit_request make_ntn_resource_audit_request(const du_cell_config& cell)
{
  f1ap_ntn_resource_audit_request request;
  request.cell_index    = to_du_cell_index(0);
  request.pci           = cell.pci;
  request.generation_id = 77;
  return request;
}

class du_manager_ntn_rnti_lease_test : public du_manager_procedure_tester, public ::testing::Test
{};

TEST_F(du_manager_ntn_rnti_lease_test, when_lease_pool_target_is_valid_then_generation_and_expiry_are_forwarded_to_mac)
{
  const f1ap_ntn_rnti_lease_pool_update request = make_ntn_rnti_lease_pool_update(cell_cfgs[0]);

  async_task<f1ap_ntn_rnti_lease_pool_result> procedure = du_mng->handle_ntn_rnti_lease_pool_update_request(request);
  lazy_task_launcher<f1ap_ntn_rnti_lease_pool_result> task(procedure);

  ASSERT_TRUE(task.ready());
  EXPECT_TRUE(task.get().accepted);
  ASSERT_TRUE(dependencies.mac.last_ntn_rnti_lease_pool_update.has_value());
  const mac_ntn_rnti_lease_pool_update& mac_request = *dependencies.mac.last_ntn_rnti_lease_pool_update;
  EXPECT_EQ(mac_request.cell_index, request.cell_index);
  EXPECT_EQ(mac_request.generation_id, request.generation_id);
  EXPECT_EQ(mac_request.expiry_ms, request.expiry_ms);
  EXPECT_EQ(mac_request.leases, request.leases);
}

TEST_F(du_manager_ntn_rnti_lease_test, when_lease_pool_stable_identity_mismatches_then_mac_is_not_updated)
{
  f1ap_ntn_rnti_lease_pool_update request = make_ntn_rnti_lease_pool_update(cell_cfgs[0]);
  request.cell_cgi.nci                    = nr_cell_identity::create(request.cell_cgi.nci.value() + 1).value();

  async_task<f1ap_ntn_rnti_lease_pool_result> procedure = du_mng->handle_ntn_rnti_lease_pool_update_request(request);
  lazy_task_launcher<f1ap_ntn_rnti_lease_pool_result> task(procedure);

  ASSERT_TRUE(task.ready());
  EXPECT_FALSE(task.get().accepted);
  EXPECT_EQ(task.get().reject_reason, "identity_mismatch");
  EXPECT_EQ(task.get().rejected_leases, request.leases);
  EXPECT_FALSE(dependencies.mac.last_ntn_rnti_lease_pool_update.has_value());
}

TEST_F(du_manager_ntn_rnti_lease_test, when_lease_pool_targets_another_du_then_mac_is_not_updated)
{
  f1ap_ntn_rnti_lease_pool_update request = make_ntn_rnti_lease_pool_update(cell_cfgs[0]);
  request.gnb_du_id                       = int_to_gnb_du_id(2);

  async_task<f1ap_ntn_rnti_lease_pool_result> procedure = du_mng->handle_ntn_rnti_lease_pool_update_request(request);
  lazy_task_launcher<f1ap_ntn_rnti_lease_pool_result> task(procedure);

  ASSERT_TRUE(task.ready());
  EXPECT_FALSE(task.get().accepted);
  EXPECT_EQ(task.get().reject_reason, "gnb_du_id_mismatch");
  EXPECT_EQ(task.get().rejected_leases, request.leases);
  EXPECT_FALSE(dependencies.mac.last_ntn_rnti_lease_pool_update.has_value());
}

TEST_F(du_manager_ntn_rnti_lease_test, when_audit_target_is_valid_then_complete_mac_lease_snapshot_is_returned)
{
  mac_ntn_rnti_lease_pool_snapshot& snapshot = dependencies.mac.next_ntn_rnti_lease_pool_snapshot;
  snapshot.cell_index                        = to_du_cell_index(0);
  snapshot.complete                          = true;
  snapshot.lease_mode_enabled                = true;
  snapshot.leases.push_back({to_rnti(0x4701), 77, "pending", "applied_by_du"});
  snapshot.leases.push_back({to_rnti(0x4702), 77, "consumed_by_mac", "applied_by_du"});
  const f1ap_ntn_resource_audit_request request = make_ntn_resource_audit_request(cell_cfgs[0]);

  async_task<f1ap_ntn_resource_audit_result>         procedure = du_mng->handle_ntn_resource_audit_request(request);
  lazy_task_launcher<f1ap_ntn_resource_audit_result> task(procedure);

  ASSERT_TRUE(task.ready());
  const f1ap_ntn_resource_audit_result result = task.get();
  EXPECT_TRUE(result.accepted);
  EXPECT_TRUE(result.rnti_snapshot_complete);
  EXPECT_FALSE(result.ue_slot_snapshot_complete);
  EXPECT_EQ(result.reject_reason, "ue_slot_snapshot_incomplete");
  EXPECT_EQ(result.generation_id, request.generation_id);
  ASSERT_EQ(result.rnti_leases.size(), 2U);
  EXPECT_EQ(result.rnti_leases[0].rnti, to_rnti(0x4701));
  EXPECT_EQ(result.rnti_leases[0].generation_id, 77U);
  EXPECT_EQ(result.rnti_leases[0].state, "pending");
  EXPECT_EQ(result.rnti_leases[1].generation_id, 77U);
  EXPECT_EQ(result.rnti_leases[1].state, "consumed_by_mac");
  EXPECT_EQ(result.rnti_leases[1].distribution_state, "applied_by_du");
  ASSERT_TRUE(dependencies.mac.last_ntn_rnti_lease_pool_snapshot_cell.has_value());
  EXPECT_EQ(*dependencies.mac.last_ntn_rnti_lease_pool_snapshot_cell, request.cell_index);
}

TEST_F(du_manager_ntn_rnti_lease_test, when_mac_snapshot_is_incomplete_then_audit_is_accepted_without_fake_entries)
{
  mac_ntn_rnti_lease_pool_snapshot& snapshot = dependencies.mac.next_ntn_rnti_lease_pool_snapshot;
  snapshot.cell_index                        = to_du_cell_index(0);
  snapshot.complete                          = false;
  snapshot.leases.push_back({to_rnti(0x4701), 77, "pending", "applied_by_du"});
  const f1ap_ntn_resource_audit_request request = make_ntn_resource_audit_request(cell_cfgs[0]);

  async_task<f1ap_ntn_resource_audit_result>         procedure = du_mng->handle_ntn_resource_audit_request(request);
  lazy_task_launcher<f1ap_ntn_resource_audit_result> task(procedure);

  ASSERT_TRUE(task.ready());
  const f1ap_ntn_resource_audit_result result = task.get();
  EXPECT_TRUE(result.accepted);
  EXPECT_FALSE(result.rnti_snapshot_complete);
  EXPECT_FALSE(result.ue_slot_snapshot_complete);
  EXPECT_EQ(result.reject_reason, "rnti_and_ue_slot_snapshots_incomplete");
  EXPECT_TRUE(result.rnti_leases.empty());
  EXPECT_TRUE(result.ue_slots.empty());
}

TEST_F(du_manager_ntn_rnti_lease_test, when_complete_mac_snapshot_targets_another_cell_then_audit_is_rejected)
{
  mac_ntn_rnti_lease_pool_snapshot& snapshot    = dependencies.mac.next_ntn_rnti_lease_pool_snapshot;
  snapshot.cell_index                           = to_du_cell_index(1);
  snapshot.complete                             = true;
  const f1ap_ntn_resource_audit_request request = make_ntn_resource_audit_request(cell_cfgs[0]);

  async_task<f1ap_ntn_resource_audit_result>         procedure = du_mng->handle_ntn_resource_audit_request(request);
  lazy_task_launcher<f1ap_ntn_resource_audit_result> task(procedure);

  ASSERT_TRUE(task.ready());
  const f1ap_ntn_resource_audit_result result = task.get();
  EXPECT_FALSE(result.accepted);
  EXPECT_FALSE(result.rnti_snapshot_complete);
  EXPECT_EQ(result.reject_reason, "snapshot_cell_mismatch");
  EXPECT_TRUE(result.rnti_leases.empty());
}

TEST_F(du_manager_ntn_rnti_lease_test, when_audit_cell_is_unknown_then_request_is_rejected_without_mac_snapshot)
{
  f1ap_ntn_resource_audit_request request = make_ntn_resource_audit_request(cell_cfgs[0]);
  request.cell_index                      = to_du_cell_index(1);

  async_task<f1ap_ntn_resource_audit_result>         procedure = du_mng->handle_ntn_resource_audit_request(request);
  lazy_task_launcher<f1ap_ntn_resource_audit_result> task(procedure);

  ASSERT_TRUE(task.ready());
  const f1ap_ntn_resource_audit_result result = task.get();
  EXPECT_FALSE(result.accepted);
  EXPECT_EQ(result.reject_reason, "unknown_cell");
  EXPECT_FALSE(result.rnti_snapshot_complete);
  EXPECT_FALSE(dependencies.mac.last_ntn_rnti_lease_pool_snapshot_cell.has_value());
}

TEST_F(du_manager_ntn_rnti_lease_test, when_audit_pci_mismatches_then_request_is_rejected_without_mac_snapshot)
{
  f1ap_ntn_resource_audit_request request = make_ntn_resource_audit_request(cell_cfgs[0]);
  request.pci                             = static_cast<pci_t>(request.pci + 1);

  async_task<f1ap_ntn_resource_audit_result>         procedure = du_mng->handle_ntn_resource_audit_request(request);
  lazy_task_launcher<f1ap_ntn_resource_audit_result> task(procedure);

  ASSERT_TRUE(task.ready());
  const f1ap_ntn_resource_audit_result result = task.get();
  EXPECT_FALSE(result.accepted);
  EXPECT_EQ(result.reject_reason, "pci_mismatch");
  EXPECT_FALSE(result.rnti_snapshot_complete);
  EXPECT_FALSE(dependencies.mac.last_ntn_rnti_lease_pool_snapshot_cell.has_value());
}

static std::vector<du_cell_config> make_two_ntn_calendar_cells()
{
  std::vector<du_cell_config> cells(2, config_helpers::make_default_du_cell_config());
  cells[1].nr_cgi.nci = nr_cell_identity::create(cells[0].nr_cgi.nci.value() + 1).value();
  // Deliberately reuse the PCI: identity validation must use NCI + PCI + DU cell index, never PCI alone.
  cells[1].pci = cells[0].pci;
  return cells;
}

static f1ap_ntn_access_calendar_update make_ntn_calendar_prepare(span<const du_cell_config> cells)
{
  f1ap_ntn_access_calendar_update request;
  request.operation                = f1ap_ntn_access_calendar_operation::prepare;
  request.satellite_id             = "P01-S001";
  request.catalog_version          = 10;
  request.schedule_version         = 20;
  request.source_content_hash      = "sha256:source";
  request.calendar_hash            = "sha256:calendar";
  request.activation_epoch_unix_ms = 320000;
  request.valid_until_unix_ms      = 640000;
  request.cycle_duration_us        = 640000;
  for (unsigned i = 0; i != cells.size(); ++i) {
    f1ap_ntn_access_calendar_cell cell;
    cell.du_cell_index = to_du_cell_index(i);
    cell.nci        = cells[i].nr_cgi.nci;
    cell.pci        = cells[i].pci;
    cell.intents.push_back({"G000001",
                            0,
                            2500,
                            f1ap_ntn_access_calendar_direction::downlink,
                            f1ap_ntn_access_calendar_purpose::ssb_sib_paging,
                            0});
    request.cells.push_back(std::move(cell));
  }
  return request;
}

class du_manager_ntn_access_calendar_test : public du_manager_procedure_tester, public ::testing::Test
{
public:
  du_manager_ntn_access_calendar_test() : du_manager_procedure_tester(make_two_ntn_calendar_cells()) {}
};

TEST_F(du_manager_ntn_access_calendar_test, when_two_cell_identity_is_valid_then_prepare_is_forwarded_atomically_to_mac)
{
  dependencies.mac.next_ntn_access_calendar_result.status = mac_ntn_access_calendar_status::ready;
  dependencies.mac.next_ntn_access_calendar_result.reason = "scheduler_ready";
  dependencies.mac.next_ntn_access_calendar_result.accepted_intents = {1, 1};
  dependencies.mac.next_ntn_access_calendar_result.effective_activation_slot = slot_point{1, 123};
  f1ap_ntn_access_calendar_update request = make_ntn_calendar_prepare(cell_cfgs);

  async_task<f1ap_ntn_access_calendar_result>         procedure =
      du_mng->handle_ntn_access_calendar_update_request(request);
  lazy_task_launcher<f1ap_ntn_access_calendar_result> task(procedure);

  ASSERT_TRUE(task.ready());
  const f1ap_ntn_access_calendar_result response = task.get();
  EXPECT_EQ(response.status, f1ap_ntn_access_calendar_result_status::ready);
  EXPECT_EQ(response.catalog_version, request.catalog_version);
  EXPECT_EQ(response.schedule_version, request.schedule_version);
  EXPECT_EQ(response.source_content_hash, request.source_content_hash);
  EXPECT_EQ(response.calendar_hash, request.calendar_hash);
  EXPECT_EQ(response.reject_reason, "scheduler_ready");
  ASSERT_TRUE(response.activation_slot.has_value());
  EXPECT_EQ(response.activation_slot.value(), slot_point(1, 123));
  EXPECT_EQ(response.accepted_intents_per_cell[0], 1);
  EXPECT_EQ(response.accepted_intents_per_cell[1], 1);

  ASSERT_TRUE(dependencies.mac.last_ntn_access_calendar_update.has_value());
  const mac_ntn_access_calendar_update& mac_request = dependencies.mac.last_ntn_access_calendar_update.value();
  EXPECT_EQ(mac_request.operation, mac_ntn_access_calendar_operation::prepare);
  EXPECT_EQ(mac_request.schedule_version, request.schedule_version);
  EXPECT_EQ(mac_request.calendar_hash, request.calendar_hash);
  EXPECT_EQ(mac_request.activation_epoch.time_since_epoch(),
            std::chrono::milliseconds(request.activation_epoch_unix_ms));
  EXPECT_EQ(mac_request.valid_until.time_since_epoch(), std::chrono::milliseconds(request.valid_until_unix_ms));
  EXPECT_EQ(mac_request.cycle_duration, std::chrono::microseconds(request.cycle_duration_us));
  EXPECT_EQ(mac_request.cells[0].nci, cell_cfgs[0].nr_cgi.nci);
  EXPECT_EQ(mac_request.cells[1].nci, cell_cfgs[1].nr_cgi.nci);
  EXPECT_EQ(mac_request.cells[0].pci, mac_request.cells[1].pci);
  ASSERT_EQ(mac_request.cells[0].intents.size(), 1);
  EXPECT_EQ(mac_request.cells[0].intents[0].position_id, "G000001");
  EXPECT_EQ(mac_request.cells[0].intents[0].direction, mac_ntn_access_calendar_direction::downlink);
  EXPECT_EQ(mac_request.cells[0].intents[0].purpose, mac_ntn_access_calendar_purpose::ssb_sib_paging);
  EXPECT_EQ(mac_request.cells[0].intents[0].port_id, 0);
}

TEST_F(du_manager_ntn_access_calendar_test, when_cell_nci_mismatches_config_then_prepare_is_rejected_without_mac_mutation)
{
  f1ap_ntn_access_calendar_update request = make_ntn_calendar_prepare(cell_cfgs);
  request.cells[1].nci = nr_cell_identity::create(request.cells[1].nci.value() + 100).value();

  async_task<f1ap_ntn_access_calendar_result>         procedure =
      du_mng->handle_ntn_access_calendar_update_request(request);
  lazy_task_launcher<f1ap_ntn_access_calendar_result> task(procedure);

  ASSERT_TRUE(task.ready());
  const f1ap_ntn_access_calendar_result response = task.get();
  EXPECT_EQ(response.status, f1ap_ntn_access_calendar_result_status::rejected);
  EXPECT_EQ(response.reject_reason, "identity_mismatch");
  EXPECT_FALSE(dependencies.mac.last_ntn_access_calendar_update.has_value());
}

TEST_F(du_manager_ntn_access_calendar_test, when_cell_pci_mismatches_config_then_prepare_is_rejected_without_mac_mutation)
{
  f1ap_ntn_access_calendar_update request = make_ntn_calendar_prepare(cell_cfgs);
  request.cells[1].pci = static_cast<pci_t>(request.cells[1].pci + 1);

  async_task<f1ap_ntn_access_calendar_result>         procedure =
      du_mng->handle_ntn_access_calendar_update_request(request);
  lazy_task_launcher<f1ap_ntn_access_calendar_result> task(procedure);

  ASSERT_TRUE(task.ready());
  const f1ap_ntn_access_calendar_result response = task.get();
  EXPECT_EQ(response.status, f1ap_ntn_access_calendar_result_status::rejected);
  EXPECT_EQ(response.reject_reason, "identity_mismatch");
  EXPECT_FALSE(dependencies.mac.last_ntn_access_calendar_update.has_value());
}

TEST_F(du_manager_ntn_access_calendar_test, when_cell_index_is_unknown_then_prepare_is_rejected_without_mac_mutation)
{
  f1ap_ntn_access_calendar_update request = make_ntn_calendar_prepare(cell_cfgs);
  request.cells[1].du_cell_index = to_du_cell_index(2);

  async_task<f1ap_ntn_access_calendar_result>         procedure =
      du_mng->handle_ntn_access_calendar_update_request(request);
  lazy_task_launcher<f1ap_ntn_access_calendar_result> task(procedure);

  ASSERT_TRUE(task.ready());
  const f1ap_ntn_access_calendar_result response = task.get();
  EXPECT_EQ(response.status, f1ap_ntn_access_calendar_result_status::rejected);
  EXPECT_EQ(response.reject_reason, "unknown_cell");
  EXPECT_FALSE(dependencies.mac.last_ntn_access_calendar_update.has_value());
}

class du_manager_ntn_access_calendar_inactive_cell_test : public du_manager_procedure_tester, public ::testing::Test
{
public:
  du_manager_ntn_access_calendar_inactive_cell_test() :
    du_manager_procedure_tester(make_two_ntn_calendar_cells(), 1)
  {
  }
};

TEST_F(du_manager_ntn_access_calendar_inactive_cell_test,
       when_configured_cell_is_inactive_then_prepare_is_rejected_without_mac_mutation)
{
  f1ap_ntn_access_calendar_update request = make_ntn_calendar_prepare(cell_cfgs);

  async_task<f1ap_ntn_access_calendar_result>         procedure =
      du_mng->handle_ntn_access_calendar_update_request(request);
  lazy_task_launcher<f1ap_ntn_access_calendar_result> task(procedure);

  ASSERT_TRUE(task.ready());
  const f1ap_ntn_access_calendar_result response = task.get();
  EXPECT_EQ(response.status, f1ap_ntn_access_calendar_result_status::rejected);
  EXPECT_EQ(response.reject_reason, "cell_not_active");
  EXPECT_FALSE(dependencies.mac.last_ntn_access_calendar_update.has_value());
}

TEST_F(du_manager_ntn_access_calendar_test, when_query_cells_mismatch_config_then_query_is_forwarded_without_validation)
{
  dependencies.mac.next_ntn_access_calendar_result.status = mac_ntn_access_calendar_status::preparing;
  f1ap_ntn_access_calendar_update request = make_ntn_calendar_prepare(cell_cfgs);
  request.operation = f1ap_ntn_access_calendar_operation::query;
  request.cells[1].nci = nr_cell_identity::create(request.cells[1].nci.value() + 100).value();

  async_task<f1ap_ntn_access_calendar_result>         procedure =
      du_mng->handle_ntn_access_calendar_update_request(request);
  lazy_task_launcher<f1ap_ntn_access_calendar_result> task(procedure);

  ASSERT_TRUE(task.ready());
  EXPECT_EQ(task.get().status, f1ap_ntn_access_calendar_result_status::preparing);
  ASSERT_TRUE(dependencies.mac.last_ntn_access_calendar_update.has_value());
  EXPECT_EQ(dependencies.mac.last_ntn_access_calendar_update->operation, mac_ntn_access_calendar_operation::query);
}

TEST_F(du_manager_ntn_access_calendar_test, when_clear_cells_mismatch_config_then_clear_is_forwarded_without_validation)
{
  dependencies.mac.next_ntn_access_calendar_result.status = mac_ntn_access_calendar_status::cleared;
  f1ap_ntn_access_calendar_update request = make_ntn_calendar_prepare(cell_cfgs);
  request.operation = f1ap_ntn_access_calendar_operation::clear;
  request.cells[1].du_cell_index = to_du_cell_index(2);

  async_task<f1ap_ntn_access_calendar_result>         procedure =
      du_mng->handle_ntn_access_calendar_update_request(request);
  lazy_task_launcher<f1ap_ntn_access_calendar_result> task(procedure);

  ASSERT_TRUE(task.ready());
  EXPECT_EQ(task.get().status, f1ap_ntn_access_calendar_result_status::cleared);
  ASSERT_TRUE(dependencies.mac.last_ntn_access_calendar_update.has_value());
  EXPECT_EQ(dependencies.mac.last_ntn_access_calendar_update->operation, mac_ntn_access_calendar_operation::clear);
}

struct ntn_access_calendar_status_test_case {
  mac_ntn_access_calendar_status         mac_status;
  f1ap_ntn_access_calendar_result_status f1ap_status;
  bool                                   accepted;
};

class du_manager_ntn_access_calendar_status_test :
  public du_manager_procedure_tester,
  public ::testing::TestWithParam<ntn_access_calendar_status_test_case>
{
public:
  du_manager_ntn_access_calendar_status_test() : du_manager_procedure_tester(make_two_ntn_calendar_cells()) {}
};

TEST_P(du_manager_ntn_access_calendar_status_test, when_mac_returns_status_then_du_maps_complete_result_to_f1)
{
  const ntn_access_calendar_status_test_case& test_case = GetParam();
  dependencies.mac.next_ntn_access_calendar_result.status = test_case.mac_status;
  dependencies.mac.next_ntn_access_calendar_result.reason = "mac_status_reason";
  dependencies.mac.next_ntn_access_calendar_result.accepted_intents = {7, 11};
  dependencies.mac.next_ntn_access_calendar_result.effective_activation_slot = slot_point{1, 321};
  f1ap_ntn_access_calendar_update request = make_ntn_calendar_prepare(cell_cfgs);

  async_task<f1ap_ntn_access_calendar_result>         procedure =
      du_mng->handle_ntn_access_calendar_update_request(request);
  lazy_task_launcher<f1ap_ntn_access_calendar_result> task(procedure);

  ASSERT_TRUE(task.ready());
  const f1ap_ntn_access_calendar_result response = task.get();
  EXPECT_EQ(response.status, test_case.f1ap_status);
  EXPECT_EQ(response.accepted(), test_case.accepted);
  EXPECT_EQ(response.reject_reason, "mac_status_reason");
  EXPECT_EQ(response.accepted_intents_per_cell[0], 7);
  EXPECT_EQ(response.accepted_intents_per_cell[1], 11);
  ASSERT_TRUE(response.activation_slot.has_value());
  EXPECT_EQ(response.activation_slot.value(), slot_point(1, 321));
}

INSTANTIATE_TEST_SUITE_P(
    all_mac_statuses,
    du_manager_ntn_access_calendar_status_test,
    ::testing::Values(
        ntn_access_calendar_status_test_case{mac_ntn_access_calendar_status::preparing,
                                             f1ap_ntn_access_calendar_result_status::preparing,
                                             true},
        ntn_access_calendar_status_test_case{mac_ntn_access_calendar_status::ready,
                                             f1ap_ntn_access_calendar_result_status::ready,
                                             true},
        ntn_access_calendar_status_test_case{mac_ntn_access_calendar_status::applied,
                                             f1ap_ntn_access_calendar_result_status::applied,
                                             true},
        ntn_access_calendar_status_test_case{mac_ntn_access_calendar_status::cleared,
                                             f1ap_ntn_access_calendar_result_status::cleared,
                                             true},
        ntn_access_calendar_status_test_case{mac_ntn_access_calendar_status::rejected,
                                             f1ap_ntn_access_calendar_result_status::rejected,
                                             false},
        ntn_access_calendar_status_test_case{mac_ntn_access_calendar_status::unsupported,
                                             f1ap_ntn_access_calendar_result_status::unsupported,
                                             false}));

static du_param_config_request make_dummy_rrm_request()
{
  du_param_config_request req;
  req.cells.resize(1);
  req.cells[0].nr_cgi = std::nullopt;
  rrm_policy_ratio_group rrm_policy;
  rrm_policy.minimum_ratio   = 30;
  rrm_policy.maximum_ratio   = 90;
  rrm_policy.dedicated_ratio = 0;
  rrm_policy.resource_type   = rrm_policy_ratio_group::resource_type_t::prb;
  rrm_policy.policy_members_list.emplace_back(
      rrm_policy_member{.plmn_id = plmn_identity{mobile_country_code::from_string("001").value(),
                                                 mobile_network_code::from_string("01").value()},
                        .s_nssai = s_nssai_t{.sst = slice_service_type{1}}});
  req.cells[0].rrm_policy_ratio_list.emplace_back(rrm_policy);
  return req;
}

class du_manager_du_rrm_config_update_test : public du_manager_procedure_tester, public ::testing::Test
{
public:
  du_manager_du_rrm_config_update_test() :
    du_manager_procedure_tester([]() {
      std::vector<du_cell_config> cfgs;
      for (unsigned i = 0; i != NOF_CELLS; ++i) {
        cfgs.push_back(config_helpers::make_default_du_cell_config());
        cfgs.back().rrm_policy_members.emplace_back(slice_rrm_policy_config{
            .rrc_member = rrm_policy_member{.plmn_id = plmn_identity{mobile_country_code::from_string("001").value(),
                                                                     mobile_network_code::from_string("01").value()},
                                            .s_nssai = s_nssai_t{.sst = slice_service_type{1}}}});
        cfgs.back().pci        = static_cast<pci_t>(1U + i);
        cfgs.back().nr_cgi.nci = nr_cell_identity::create({411, 22}, 1 + i).value();
        cfgs.back().dl_cfg_common.init_dl_bwp.pdcch_common.coreset0.value().interleaved->shift_index = cfgs.back().pci;
      }
      return cfgs;
    }())
  {
  }

  static constexpr unsigned NOF_CELLS = 3U;
};

TEST_F(du_manager_du_rrm_config_update_test, when_rrm_policy_change_required_then_mac_is_reconfigured)
{
  // Initiate procedure.
  du_param_config_request  req  = make_dummy_rrm_request();
  du_param_config_response resp = du_mng->handle_operator_config_request(req);

  // MAC received config request.
  ASSERT_TRUE(dependencies.mac.mac_cell.last_cell_recfg_req.has_value());

  ASSERT_TRUE(resp.success);
}
