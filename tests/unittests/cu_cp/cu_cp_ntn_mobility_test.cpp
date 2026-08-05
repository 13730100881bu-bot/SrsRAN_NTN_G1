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
#include "lib/cu_cp/cu_cp_impl.h"
#include "lib/cu_cp/ntn_mobility/ntn_onboard_position_plan.h"
#include "lib/cu_cp/ntn_mobility/ntn_onboard_position_plan_state.h"
#include "lib/cu_cp/ntn_mobility/ntn_plan_version_anchor.h"
#include "lib/f1ap/asn1_helpers.h"
#include "lib/ngap/ngap_asn1_converters.h"
#include "lib/rrc/ue/rrc_measurement_types_asn1_converters.h"
#include "nlohmann/json.hpp"
#include "tests/test_doubles/e1ap/e1ap_test_message_validators.h"
#include "tests/test_doubles/f1ap/f1ap_test_message_validators.h"
#include "tests/test_doubles/ngap/ngap_test_message_validators.h"
#include "tests/unittests/cu_cp/test_helpers.h"
#include "tests/unittests/e1ap/common/e1ap_cu_cp_test_messages.h"
#include "tests/unittests/ngap/ngap_test_messages.h"
#include "tests/unittests/rrc/rrc_ue_test_messages.h"
#include "srsran/asn1/asn1_utils.h"
#include "srsran/asn1/f1ap/common.h"
#include "srsran/asn1/rrc_nr/rrc_nr.h"
#include "srsran/asn1/rrc_nr/ue_cap.h"
#include "srsran/asn1/rrc_nr/ul_dcch_msg.h"
#include "srsran/asn1/rrc_nr/ul_dcch_msg_ies.h"
#include "srsran/cu_cp/cu_cp_command_handler.h"
#include "srsran/f1ap/ntn_access_calendar.h"
#include "srsran/f1ap/ntn_ul_slot_resource_request.h"
#include "srsran/ngap/ngap_handover.h"
#include "srsran/nrppa/nrppa_pdu.h"
#include "srsran/ran/gnb_id.h"
#include "srsran/security/integrity.h"
#include "srsran/support/async/async_test_utils.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <mbedtls/base64.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <set>
#include <thread>
#include <utility>
#include <variant>

using namespace srsran;
using namespace srs_cu_cp;

namespace {

nr_cell_identity make_default_env_nci(unsigned sector_id)
{
  return nr_cell_identity::create(gnb_id_t{411, 22}, sector_id).value();
}

ecef_coordinates_t make_ecef(double latitude_deg, double longitude_deg, double altitude_m)
{
  constexpr double wgs84_a_m = 6378137.0;
  constexpr double wgs84_f   = 1.0 / 298.257223563;
  constexpr double wgs84_e2  = 2.0 * wgs84_f - wgs84_f * wgs84_f;
  constexpr double pi        = 3.14159265358979323846;

  const double lat     = latitude_deg * pi / 180.0;
  const double lon     = longitude_deg * pi / 180.0;
  const double sin_lat = std::sin(lat);
  const double cos_lat = std::cos(lat);
  const double n       = wgs84_a_m / std::sqrt(1.0 - wgs84_e2 * sin_lat * sin_lat);

  ecef_coordinates_t ecef{};
  ecef.position_x = (n + altitude_m) * cos_lat * std::cos(lon);
  ecef.position_y = (n + altitude_m) * cos_lat * std::sin(lon);
  ecef.position_z = (n * (1.0 - wgs84_e2) + altitude_m) * sin_lat;
  return ecef;
}

ntn_location_mobility_config make_ntn_mobility_config()
{
  ntn_location_mobility_config cfg;
  cfg.enabled                       = true;
  cfg.served_beam_min_elevation_deg = 80.0;
  cfg.max_nof_served_beams          = 1;
  cfg.beams = {{"CN-BEAM-0001", make_default_env_nci(0), 0.0, 0.0, 50000.0, true},
               {"CN-BEAM-0002", make_default_env_nci(1), 0.0, 1.0, 50000.0, true}};
  return cfg;
}

void bind_onboard_planning_context(ntn_versioned_position_plan& plan)
{
  plan.schema_version            = 2;
  plan.planning_run_id           = "planning-run-2026-07-15";
  plan.catalog_id                = "global-land-l1-v1";
  plan.catalog_hash              = "sha256:b39fe9c3ee9a9355b3546036b7f16e0fb858c953f8558cc4295122f2169fbe7a";
  plan.identity_registry_version = "mc-ntn-onboard-cell-registry-v1";
  plan.identity_registry_hash    = "sha256:7475821350e104b57a70d979d630f4b29a6cecb89ca0eca7b16dddf2ffee6a4a";
  plan.access_profile_id         = "ntn-access-16a-64d-v1";
  plan.access_profile_hash       = "sha256:195786f4161e3b0fad6faa0605144948a7401c067a014bde684c1b29a8087d63";
  for (ntn_l1_position& position : plan.visible_l1_positions) {
    position.child_mask = 0x7f;
  }
}

void bind_onboard_planning_context(ntn_onboard_position_plan_source_config& source)
{
  source.expected_catalog_id                = "global-land-l1-v1";
  source.expected_catalog_hash              = "sha256:b39fe9c3ee9a9355b3546036b7f16e0fb858c953f8558cc4295122f2169fbe7a";
  source.expected_identity_registry_version = "mc-ntn-onboard-cell-registry-v1";
  source.expected_identity_registry_hash    = "sha256:7475821350e104b57a70d979d630f4b29a6cecb89ca0eca7b16dddf2ffee6a4a";
  source.expected_access_profile_id         = "ntn-access-16a-64d-v1";
  source.expected_access_profile_hash       = "sha256:195786f4161e3b0fad6faa0605144948a7401c067a014bde684c1b29a8087d63";
  if (source.du_execution_enabled && source.state_file.empty() && !source.plan_json_file.empty()) {
    source.state_file = source.plan_json_file + ".state";
  }
}

std::filesystem::path write_onboard_position_plan_for_runtime_test(const ntn_versioned_position_plan& plan)
{
  nlohmann::json root;
  if (plan.schema_version >= 2) {
    root["schema_version"]    = plan.schema_version;
    root["planning_run_id"]   = plan.planning_run_id;
    root["catalog"]           = {{"id", plan.catalog_id}, {"sha256", plan.catalog_hash}};
    root["identity_registry"] = {{"version", plan.identity_registry_version}, {"sha256", plan.identity_registry_hash}};
    root["access_profile"]    = {{"id", plan.access_profile_id}, {"sha256", plan.access_profile_hash}};
  }
  root["satellite_id"]             = plan.satellite_id;
  root["catalog_version"]          = plan.catalog_version;
  root["schedule_version"]         = plan.schedule_version;
  root["content_hash"]             = plan.content_hash;
  root["valid_from_unix_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     plan.valid_from.time_since_epoch())
                                     .count();
  root["valid_until_unix_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      plan.valid_until.time_since_epoch())
                                      .count();
  root["activation_epoch_unix_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                                           plan.activation_epoch.time_since_epoch())
                                           .count();
  for (const auto& cell : plan.onboard_cells) {
    root["onboard_cells"].push_back({{"nci", cell.nci.value()}, {"pci", cell.pci}});
  }
  for (const auto& position : plan.visible_l1_positions) {
    nlohmann::json encoded_position = {{"position_id", position.position_id},
                                       {"latitude_deg", position.latitude_deg},
                                       {"longitude_deg", position.longitude_deg}};
    if (plan.schema_version >= 2) {
      encoded_position["child_mask"] = position.child_mask;
    }
    root["visible_l1_positions"].push_back(std::move(encoded_position));
  }
  if (plan.schema_version >= 3) {
    root["assigned_l1_position_ids"] = plan.assigned_l1_position_ids;
  }
  if (plan.schema_version >= 4 && plan.authentication.has_value()) {
    root["authentication"] = {{"algorithm", plan.authentication->algorithm},
                              {"key_id", plan.authentication->key_id},
                              {"signature_base64", plan.authentication->signature_base64}};
  }

  const auto unique_suffix = std::chrono::steady_clock::now().time_since_epoch().count();
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / fmt::format("srsran-ntn-calendar-{}.json", unique_suffix);
  std::ofstream output(path);
  output << root.dump(2);
  output.close();
  return path;
}

std::string read_runtime_test_file(const std::filesystem::path& path)
{
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void write_runtime_test_file(const std::filesystem::path& path, const std::string& contents)
{
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  srsran_assert(output.good(), "Failed to write NTN runtime test file");
}

struct temporary_plan_file_guard {
  explicit temporary_plan_file_guard(std::filesystem::path path_) : path(std::move(path_)) {}
  ~temporary_plan_file_guard()
  {
    std::error_code error;
    std::filesystem::remove(path, error);
    const std::filesystem::path state_path{path.string() + ".state"};
    std::filesystem::remove(state_path, error);
    const std::filesystem::path anchor_path{path.string() + ".anchor"};
    std::filesystem::remove(anchor_path, error);
    const std::string state_temp_prefix  = fmt::format(".{}.tmp.", state_path.filename().string());
    const std::string anchor_temp_prefix = fmt::format(".{}.tmp-", anchor_path.filename().string());
    for (const auto& entry : std::filesystem::directory_iterator(path.parent_path(), error)) {
      const std::string filename = entry.path().filename().string();
      if (filename.rfind(state_temp_prefix, 0) == 0 || filename.rfind(anchor_temp_prefix, 0) == 0) {
        std::filesystem::remove(entry.path(), error);
      }
    }
  }
  std::filesystem::path path;
};

std::vector<ntn_versioned_position_plan>
sign_schema_v4_runtime_plans(std::vector<ntn_versioned_position_plan> plans,
                             const std::filesystem::path&             public_key_path,
                             const std::string&                       key_id)
{
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context random;
  mbedtls_pk_context key;
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&random);
  mbedtls_pk_init(&key);
  const std::string personalization = "srsran-ntn-schema-v4-runtime-test";
  srsran_assert(mbedtls_ctr_drbg_seed(&random,
                                      mbedtls_entropy_func,
                                      &entropy,
                                      reinterpret_cast<const unsigned char*>(personalization.data()),
                                      personalization.size()) == 0,
                "Failed to seed schema-v4 test signer");
  srsran_assert(mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) == 0,
                "Failed to initialize schema-v4 test signer");
  srsran_assert(mbedtls_ecp_gen_key(
                    MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key), mbedtls_ctr_drbg_random, &random) == 0,
                "Failed to generate schema-v4 test key");

  std::array<unsigned char, 2048> public_key_pem{};
  srsran_assert(mbedtls_pk_write_pubkey_pem(&key, public_key_pem.data(), public_key_pem.size()) == 0,
                "Failed to encode schema-v4 test public key");
  {
    std::ofstream output(public_key_path, std::ios::binary | std::ios::trunc);
    output << reinterpret_cast<const char*>(public_key_pem.data());
    srsran_assert(output.good(), "Failed to write schema-v4 test public key");
  }

  const mbedtls_md_info_t* md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  srsran_assert(md_info != nullptr, "SHA-256 is unavailable for schema-v4 runtime signing");
  for (ntn_versioned_position_plan& plan : plans) {
    plan.schema_version = 4;
    plan.content_hash   = compute_ntn_position_plan_content_hash(plan);
    plan.authentication = ntn_position_plan_authentication{ntn_position_plan_signature_algorithm, key_id, {}};

    const std::string payload = compute_ntn_position_plan_signature_payload(plan);
    std::array<unsigned char, 32> digest{};
    srsran_assert(mbedtls_md(md_info,
                             reinterpret_cast<const unsigned char*>(payload.data()),
                             payload.size(),
                             digest.data()) == 0,
                  "Failed to hash schema-v4 test signature payload");
    std::array<unsigned char, max_ntn_position_plan_signature_size> signature{};
    size_t signature_size = 0;
    srsran_assert(mbedtls_pk_sign(&key,
                                  MBEDTLS_MD_SHA256,
                                  digest.data(),
                                  digest.size(),
                                  signature.data(),
                                  &signature_size,
                                  mbedtls_ctr_drbg_random,
                                  &random) == 0,
                  "Failed to sign schema-v4 runtime plan");
    std::array<unsigned char, 512> encoded_signature{};
    size_t encoded_size = 0;
    srsran_assert(mbedtls_base64_encode(encoded_signature.data(),
                                        encoded_signature.size(),
                                        &encoded_size,
                                        signature.data(),
                                        signature_size) == 0,
                  "Failed to encode schema-v4 runtime signature");
    plan.authentication->signature_base64.assign(reinterpret_cast<const char*>(encoded_signature.data()), encoded_size);
  }

  mbedtls_pk_free(&key);
  mbedtls_ctr_drbg_free(&random);
  mbedtls_entropy_free(&entropy);
  return plans;
}

ntn_versioned_position_plan sign_schema_v4_runtime_plan(ntn_versioned_position_plan plan,
                                                        const std::filesystem::path& public_key_path,
                                                        const std::string&           key_id)
{
  auto signed_plans =
      sign_schema_v4_runtime_plans({std::move(plan)}, public_key_path, key_id);
  return std::move(signed_plans.front());
}

ntn_versioned_position_plan make_schema_v3_runtime_plan(unsigned             visible_positions,
                                                        unsigned             assigned_positions,
                                                        uint64_t             catalog_version,
                                                        uint64_t             schedule_version,
                                                        nr_cell_identity     first_nci,
                                                        nr_cell_identity     second_nci,
                                                        pci_t                shared_pci,
                                                        std::chrono::system_clock::time_point activation_epoch,
                                                        std::chrono::system_clock::time_point valid_until)
{
  srsran_assert(assigned_positions <= visible_positions,
                "Schema v3 runtime test assignment cannot exceed the visible inventory");

  ntn_versioned_position_plan plan;
  plan.satellite_id         = "P01-S01";
  plan.catalog_version      = catalog_version;
  plan.schedule_version     = schedule_version;
  plan.valid_from = std::min(activation_epoch - std::chrono::milliseconds{640},
                             std::chrono::system_clock::now() - std::chrono::milliseconds{640});
  plan.activation_epoch     = activation_epoch;
  plan.valid_until          = valid_until;
  plan.onboard_cells[0]     = {first_nci, shared_pci};
  plan.onboard_cells[1]     = {second_nci, shared_pci};
  plan.visible_l1_positions.reserve(visible_positions);
  for (unsigned i = 0; i != visible_positions; ++i) {
    plan.visible_l1_positions.push_back(
        {fmt::format("G{:06}", i + 1), 10.0 + static_cast<double>(i / 32) * 0.01, 20.0 + (i % 32) * 0.01});
  }
  bind_onboard_planning_context(plan);
  plan.schema_version = 3;
  plan.assigned_l1_position_ids.reserve(assigned_positions);
  for (unsigned i = 0; i != assigned_positions; ++i) {
    const unsigned visible_index = assigned_positions == 0 ? 0 : (i * visible_positions) / assigned_positions;
    plan.assigned_l1_position_ids.push_back(plan.visible_l1_positions[visible_index].position_id);
  }
  plan.content_hash = compute_ntn_position_plan_content_hash(plan);
  return plan;
}

ntn_onboard_position_plan_source_config
make_schema_v3_runtime_source(const ntn_versioned_position_plan& plan,
                              const std::filesystem::path&       plan_path,
                              bool                               execution_enabled = true)
{
  ntn_onboard_position_plan_source_config source;
  source.enabled              = true;
  source.du_execution_enabled = execution_enabled;
  source.du_prepare_guard     = std::chrono::milliseconds{100};
  source.satellite_id         = plan.satellite_id;
  source.plan_json_file       = plan_path.string();
  source.cell_ncis            = {plan.onboard_cells[0].nci, plan.onboard_cells[1].nci};
  source.cell_pcis            = {plan.onboard_cells[0].pci, plan.onboard_cells[1].pci};
  bind_onboard_planning_context(source);
  return source;
}

std::vector<test_helpers::served_cell_item_info>
make_onboard_served_cells(nr_cell_identity first_nci, nr_cell_identity second_nci, pci_t shared_pci)
{
  std::vector<test_helpers::served_cell_item_info> served_cells(2);
  served_cells[0].nci = first_nci;
  served_cells[0].pci = shared_pci;
  served_cells[1].nci = second_nci;
  served_cells[1].pci = shared_pci;
  return served_cells;
}

std::optional<f1ap_ntn_access_calendar_update> decode_ntn_calendar_update(const f1ap_message& request)
{
  if (request.pdu.type().value != asn1::f1ap::f1ap_pdu_c::types_opts::init_msg ||
      request.pdu.init_msg().proc_code != ASN1_F1AP_ID_GNB_DU_RES_COORDINATION) {
    return std::nullopt;
  }
  return decode_f1ap_ntn_access_calendar_update(
      request.pdu.init_msg().value.gnb_du_res_coordination_request()->eutra_nr_cell_res_coordination_req_container);
}

void make_recovered_calendar_feedback(
    const f1ap_ntn_access_calendar_update&                            prepare,
    std::array<uint16_t, 2>&                                          intents_per_cell,
    std::array<f1ap_ntn_access_calendar_preflight_report, 2>& preflight_reports)
{
  intents_per_cell = {};
  preflight_reports = {};
  srsran_assert(prepare.cells.size() == 2, "Expected exactly two onboard cells in schema v3 prepare");
  for (unsigned i = 0; i != prepare.cells.size(); ++i) {
    intents_per_cell[i] = static_cast<uint16_t>(prepare.cells[i].intents.size());
    auto& report        = preflight_reports[i];
    report.performed    = true;
    report.passed       = true;
    report.numerology   = 1;
    for (const f1ap_ntn_access_calendar_intent& intent : prepare.cells[i].intents) {
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

struct state_file_write_failure_guard {
  explicit state_file_write_failure_guard(std::filesystem::path target_) :
    target(std::move(target_)), backup(target.string() + ".write-failure-backup")
  {
  }

  ~state_file_write_failure_guard() { (void)restore(); }

  bool replace_with_directory()
  {
    std::error_code error;
    std::filesystem::rename(target, backup, error);
    if (error) {
      return false;
    }
    if (!std::filesystem::create_directory(target, error) || error) {
      std::error_code ignored;
      std::filesystem::rename(backup, target, ignored);
      installed = static_cast<bool>(ignored);
      return false;
    }
    installed = true;
    return true;
  }

  bool restore()
  {
    if (!installed) {
      return true;
    }
    std::error_code error;
    if (std::filesystem::exists(target, error)) {
      if (error || !std::filesystem::remove(target, error) || error) {
        return false;
      }
    } else if (error) {
      return false;
    }
    std::filesystem::rename(backup, target, error);
    if (error) {
      return false;
    }
    installed = false;
    return true;
  }

private:
  std::filesystem::path target;
  std::filesystem::path backup;
  bool                  installed = false;
};

struct state_store_failpoint_guard {
  explicit state_store_failpoint_guard(ntn_onboard_position_plan_state_store_failpoint failpoint)
  {
    set_ntn_onboard_position_plan_state_store_failpoint_once_for_test(failpoint);
  }

  ~state_store_failpoint_guard()
  {
    set_ntn_onboard_position_plan_state_store_failpoint_once_for_test(
        ntn_onboard_position_plan_state_store_failpoint::none);
  }
};

struct persisted_recovery_calendar_test_data {
  ntn_versioned_position_plan                              plan;
  ntn_onboard_position_plan_source_config                  source;
  std::filesystem::path                                    plan_path;
  std::array<uint16_t, 2>                                  recovered_intents{};
  std::array<f1ap_ntn_access_calendar_preflight_report, 2> recovered_preflight{};
};

struct persisted_active_pending_calendar_test_data {
  ntn_versioned_position_plan                              active_plan;
  ntn_versioned_position_plan                              pending_plan;
  ntn_onboard_position_plan_source_config                  source;
  std::filesystem::path                                    plan_path;
  std::string                                              active_calendar_hash;
  std::string                                              pending_calendar_hash;
  std::array<uint16_t, 2>                                  recovered_active_intents{};
  std::array<f1ap_ntn_access_calendar_preflight_report, 2> recovered_active_preflight{};
  std::array<uint16_t, 2>                                  recovered_pending_intents{};
  std::array<f1ap_ntn_access_calendar_preflight_report, 2> recovered_pending_preflight{};
};

persisted_active_pending_calendar_test_data
make_persisted_active_with_future_pending(std::chrono::milliseconds active_valid_for,
                                          bool                      pending_was_applied = false,
                                          std::chrono::milliseconds pending_activation_delay =
                                              std::chrono::milliseconds{20000})
{
  persisted_active_pending_calendar_test_data data;
  const nr_cell_identity                      first_nci  = make_default_env_nci(0);
  const nr_cell_identity                      second_nci = make_default_env_nci(1);
  constexpr pci_t                             shared_pci = 101;
  const int64_t                               wall_now_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count();
  const int64_t controller_now_ms = active_valid_for.count() < 0 ? wall_now_ms - 2000 : wall_now_ms;
  const int64_t active_activation_ms = (controller_now_ms / 640) * 640;
  const int64_t pending_activation_ms = ((wall_now_ms + pending_activation_delay.count() + 639) / 640) * 640;

  data.active_plan.satellite_id     = "P01-S01";
  data.active_plan.catalog_version  = 50;
  data.active_plan.schedule_version = 60;
  data.active_plan.valid_from =
      std::chrono::system_clock::time_point{std::chrono::milliseconds{active_activation_ms - 640}};
  data.active_plan.activation_epoch =
      std::chrono::system_clock::time_point{std::chrono::milliseconds{active_activation_ms}};
  data.active_plan.valid_until =
      std::chrono::system_clock::time_point{std::chrono::milliseconds{wall_now_ms + active_valid_for.count()}};
  data.active_plan.onboard_cells[0]     = {first_nci, shared_pci};
  data.active_plan.onboard_cells[1]     = {second_nci, shared_pci};
  data.active_plan.visible_l1_positions = {{"G000001", 10.0, 20.0}, {"G000002", 10.1, 20.1}};
  bind_onboard_planning_context(data.active_plan);
  data.active_plan.content_hash = compute_ntn_position_plan_content_hash(data.active_plan);

  data.pending_plan                  = data.active_plan;
  data.pending_plan.catalog_version  = 51;
  data.pending_plan.schedule_version = 61;
  data.pending_plan.valid_from =
      std::chrono::system_clock::time_point{std::chrono::milliseconds{controller_now_ms - 640}};
  data.pending_plan.activation_epoch =
      std::chrono::system_clock::time_point{std::chrono::milliseconds{pending_activation_ms}};
  data.pending_plan.valid_until =
      std::chrono::system_clock::time_point{std::chrono::milliseconds{pending_activation_ms + 30000}};
  data.pending_plan.visible_l1_positions.push_back({"G000003", 10.2, 20.2, 0x7f});
  data.pending_plan.content_hash = compute_ntn_position_plan_content_hash(data.pending_plan);
  data.plan_path                 = write_onboard_position_plan_for_runtime_test(data.pending_plan);

  data.source.enabled              = true;
  data.source.du_execution_enabled = true;
  data.source.du_prepare_guard     = std::chrono::milliseconds{100};
  data.source.satellite_id         = data.pending_plan.satellite_id;
  data.source.plan_json_file       = data.plan_path.string();
  data.source.cell_ncis            = {first_nci, second_nci};
  data.source.cell_pcis            = {shared_pci, shared_pci};
  bind_onboard_planning_context(data.source);

  ntn_onboard_position_plan_config controller_cfg;
  controller_cfg.enabled                            = true;
  controller_cfg.require_external_apply             = true;
  controller_cfg.satellite_id                       = data.source.satellite_id;
  controller_cfg.expected_catalog_id                = data.source.expected_catalog_id;
  controller_cfg.expected_catalog_hash              = data.source.expected_catalog_hash;
  controller_cfg.expected_identity_registry_version = data.source.expected_identity_registry_version;
  controller_cfg.expected_identity_registry_hash    = data.source.expected_identity_registry_hash;
  controller_cfg.expected_access_profile_id         = data.source.expected_access_profile_id;
  controller_cfg.expected_access_profile_hash       = data.source.expected_access_profile_hash;
  controller_cfg.onboard_cells                      = {{{first_nci, shared_pci}, {second_nci, shared_pci}}};
  ntn_onboard_position_plan_controller controller(controller_cfg);
  const auto controller_now =
      std::chrono::system_clock::time_point{std::chrono::milliseconds{controller_now_ms}};
  srsran_assert(controller.submit(data.active_plan, controller_now).accepted,
                "Failed to submit active NTN calendar test plan");
  data.active_calendar_hash = controller.pending_plan()->calendar_hash;
  srsran_assert(controller.mark_deployment_preparing(data.active_plan.schedule_version, data.active_calendar_hash),
                "Failed to mark active NTN calendar test plan preparing");
  srsran_assert(controller.mark_deployment_applied(data.active_plan.schedule_version, data.active_calendar_hash),
                "Failed to mark active NTN calendar test plan applied");
  srsran_assert(controller.advance_time(controller_now), "Failed to activate NTN calendar test plan");
  srsran_assert(controller.submit(data.pending_plan, controller_now).accepted,
                 "Failed to submit future NTN calendar test plan");
  data.pending_calendar_hash = controller.pending_plan()->calendar_hash;
  if (pending_was_applied) {
    srsran_assert(controller.mark_deployment_preparing(data.pending_plan.schedule_version, data.pending_calendar_hash),
                  "Failed to mark future NTN calendar test plan preparing");
    srsran_assert(controller.mark_deployment_applied(data.pending_plan.schedule_version, data.pending_calendar_hash),
                  "Failed to mark future NTN calendar test plan applied");
    srsran_assert(controller.deployment_stage() == ntn_position_plan_deployment_stage::applied,
                  "Future NTN calendar test plan must remain applied before activation");
  } else {
    srsran_assert(controller.deployment_stage() == ntn_position_plan_deployment_stage::not_sent,
                  "Future NTN calendar test plan must remain not_sent");
  }

  for (const ntn_access_calendar_intent& intent : controller.active_plan()->access_calendar) {
    const unsigned cell_index = intent.nci == first_nci ? 0U : 1U;
    ++data.recovered_active_intents[cell_index];
    auto& report      = data.recovered_active_preflight[cell_index];
    report.performed  = true;
    report.passed     = true;
    report.numerology = 1;
    if (intent.purpose == ntn_access_calendar_purpose::ssb_sib_paging ||
        intent.purpose == ntn_access_calendar_purpose::ssb_sib_paging_rar) {
      ++report.expected_ssb;
      ++report.matched_ssb;
      report.max_ssb_gap_slots = 160;
    } else if (intent.purpose == ntn_access_calendar_purpose::prach_ro) {
      ++report.expected_prach;
      ++report.matched_prach;
      report.max_prach_gap_slots = 1280;
    }
  }
  for (const ntn_access_calendar_intent& intent : controller.pending_plan()->access_calendar) {
    const unsigned cell_index = intent.nci == first_nci ? 0U : 1U;
    ++data.recovered_pending_intents[cell_index];
    auto& report      = data.recovered_pending_preflight[cell_index];
    report.performed  = true;
    report.passed     = true;
    report.numerology = 1;
    if (intent.purpose == ntn_access_calendar_purpose::ssb_sib_paging ||
        intent.purpose == ntn_access_calendar_purpose::ssb_sib_paging_rar) {
      ++report.expected_ssb;
      ++report.matched_ssb;
      report.max_ssb_gap_slots = 160;
    } else if (intent.purpose == ntn_access_calendar_purpose::prach_ro) {
      ++report.expected_prach;
      ++report.matched_prach;
      report.max_prach_gap_slots = 1280;
    }
  }
  const auto stored =
      store_ntn_onboard_position_plan_state_atomic(data.source.state_file, controller.make_persistent_state(9));
  srsran_assert(stored.has_value(), "Failed to store active/pending NTN calendar test state");
  return data;
}

persisted_recovery_calendar_test_data make_persisted_recovery_calendar_test_data(uint64_t catalog_version,
                                                                                 uint64_t schedule_version)
{
  persisted_recovery_calendar_test_data data;
  const nr_cell_identity                first_nci  = make_default_env_nci(0);
  const nr_cell_identity                second_nci = make_default_env_nci(1);
  constexpr pci_t                       shared_pci = 101;
  const int64_t                         now_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count();
  const int64_t activation_ms = (now_ms / 640) * 640;

  data.plan.satellite_id     = "P01-S01";
  data.plan.catalog_version  = catalog_version;
  data.plan.schedule_version = schedule_version;
  data.plan.valid_from       = std::chrono::system_clock::time_point{std::chrono::milliseconds{activation_ms - 640}};
  data.plan.activation_epoch = std::chrono::system_clock::time_point{std::chrono::milliseconds{activation_ms}};
  data.plan.valid_until      = std::chrono::system_clock::time_point{std::chrono::milliseconds{now_ms + 30000}};
  data.plan.onboard_cells[0] = {first_nci, shared_pci};
  data.plan.onboard_cells[1] = {second_nci, shared_pci};
  data.plan.visible_l1_positions = {{"G000001", 10.0, 20.0}, {"G000002", 10.1, 20.1}};
  bind_onboard_planning_context(data.plan);
  data.plan.content_hash = compute_ntn_position_plan_content_hash(data.plan);
  data.plan_path         = write_onboard_position_plan_for_runtime_test(data.plan);

  data.source.enabled              = true;
  data.source.du_execution_enabled = true;
  data.source.du_prepare_guard     = std::chrono::milliseconds{100};
  data.source.satellite_id         = data.plan.satellite_id;
  data.source.plan_json_file       = data.plan_path.string();
  data.source.cell_ncis            = {first_nci, second_nci};
  data.source.cell_pcis            = {shared_pci, shared_pci};
  bind_onboard_planning_context(data.source);

  ntn_onboard_position_plan_config controller_cfg;
  controller_cfg.enabled                            = true;
  controller_cfg.require_external_apply             = true;
  controller_cfg.satellite_id                       = data.source.satellite_id;
  controller_cfg.expected_catalog_id                = data.source.expected_catalog_id;
  controller_cfg.expected_catalog_hash              = data.source.expected_catalog_hash;
  controller_cfg.expected_identity_registry_version = data.source.expected_identity_registry_version;
  controller_cfg.expected_identity_registry_hash    = data.source.expected_identity_registry_hash;
  controller_cfg.expected_access_profile_id         = data.source.expected_access_profile_id;
  controller_cfg.expected_access_profile_hash       = data.source.expected_access_profile_hash;
  controller_cfg.onboard_cells                      = {{{first_nci, shared_pci}, {second_nci, shared_pci}}};
  ntn_onboard_position_plan_controller persisted_controller(controller_cfg);
  const auto                           submitted =
      persisted_controller.submit(data.plan, std::chrono::system_clock::time_point{std::chrono::milliseconds{now_ms}});
  srsran_assert(submitted.accepted, "Failed to create persisted NTN recovery test plan");
  const std::string calendar_hash = persisted_controller.pending_plan()->calendar_hash;
  srsran_assert(persisted_controller.mark_deployment_preparing(schedule_version, calendar_hash),
                "Failed to mark persisted NTN recovery test plan preparing");
  srsran_assert(persisted_controller.mark_deployment_applied(schedule_version, calendar_hash),
                "Failed to mark persisted NTN recovery test plan applied");
  srsran_assert(
      persisted_controller.advance_time(std::chrono::system_clock::time_point{std::chrono::milliseconds{now_ms}}),
      "Failed to activate persisted NTN recovery test plan");
  const auto stored = store_ntn_onboard_position_plan_state_atomic(data.source.state_file,
                                                                   persisted_controller.make_persistent_state(9));
  srsran_assert(stored.has_value(), "Failed to store persisted NTN recovery test state");

  for (const ntn_access_calendar_intent& intent : persisted_controller.active_plan()->access_calendar) {
    const unsigned cell_index = intent.nci == first_nci ? 0U : 1U;
    ++data.recovered_intents[cell_index];
    auto& report      = data.recovered_preflight[cell_index];
    report.performed  = true;
    report.passed     = true;
    report.numerology = 1;
    if (intent.purpose == ntn_access_calendar_purpose::ssb_sib_paging ||
        intent.purpose == ntn_access_calendar_purpose::ssb_sib_paging_rar) {
      ++report.expected_ssb;
      ++report.matched_ssb;
      report.max_ssb_gap_slots = 160;
    } else if (intent.purpose == ntn_access_calendar_purpose::prach_ro) {
      ++report.expected_prach;
      ++report.matched_prach;
      report.max_prach_gap_slots = 1280;
    }
  }
  return data;
}

ntn_location_mobility_config make_analog_access_du_relocation_ntn_mobility_config()
{
  ntn_location_mobility_config cfg = make_ntn_mobility_config();
  cfg.served_beam_min_elevation_deg = -90.0;
  cfg.max_nof_served_beams          = 3;
  cfg.max_nof_loaded_digital_service_beams = 3;
  cfg.beams.push_back({"CN-BEAM-0003", make_default_env_nci(2), 0.0, 0.0, 50000.0, true});
  cfg.beams[0].analog_beam_id = "ANALOG-ACCESS-001";
  cfg.beams[1].analog_beam_id = "ANALOG-ACCESS-001";
  cfg.beams[2].analog_beam_id = "ANALOG-ACCESS-001";
  cfg.beams[1].center_latitude_deg  = cfg.beams[0].center_latitude_deg;
  cfg.beams[1].center_longitude_deg = cfg.beams[0].center_longitude_deg;

  ntn_analog_beam_position analog;
  analog.analog_beam_id         = "ANALOG-ACCESS-001";
  analog.center_digital_beam_id = "CN-BEAM-0001";
  analog.child_digital_beam_ids = {"CN-BEAM-0001", "CN-BEAM-0002", "CN-BEAM-0003"};
  analog.is_edge_partial        = true;
  cfg.analog_beams              = {analog};
  return cfg;
}

ntn_location_mobility_config make_access_service_layer_ntn_mobility_config()
{
  ntn_location_mobility_config cfg;
  cfg.enabled                                      = true;
  cfg.served_beam_min_elevation_deg                = -90.0;
  cfg.max_nof_served_beams                         = 2;
  cfg.max_nof_loaded_digital_service_beams         = 2;
  cfg.required_consecutive_location_reports        = 2;
  cfg.beams = {{"CN-BEAM-0001", make_default_env_nci(0), 0.0, 0.0, 50000.0, true},
               {"CN-BEAM-0002", make_default_env_nci(1), 0.0, 1.0, 50000.0, true}};
  cfg.beams[0].analog_beam_id = "ANALOG-ACCESS-001";
  cfg.beams[1].analog_beam_id = "ANALOG-ACCESS-001";

  ntn_analog_beam_position analog;
  analog.analog_beam_id         = "ANALOG-ACCESS-001";
  analog.center_digital_beam_id = "CN-BEAM-0001";
  analog.child_digital_beam_ids = {"CN-BEAM-0001", "CN-BEAM-0002"};
  analog.is_edge_partial        = true;
  cfg.analog_beams              = {analog};
  return cfg;
}

ntn_location_mobility_config make_multi_beam_load_balancing_ntn_mobility_config(bool overlapping_children)
{
  ntn_location_mobility_config cfg = make_access_service_layer_ntn_mobility_config();
  cfg.multi_beam_load_balancing_enabled             = true;
  cfg.multi_beam_load_balancing_min_ue_delta        = 2;
  cfg.multi_beam_load_balancing_max_handovers_per_eval = 1;
  cfg.multi_beam_load_balancing_handover_cooldown   = std::chrono::milliseconds{30000};
  if (overlapping_children) {
    cfg.beams[1].center_latitude_deg  = cfg.beams[0].center_latitude_deg;
    cfg.beams[1].center_longitude_deg = cfg.beams[0].center_longitude_deg;
  }
  return cfg;
}

void add_third_digital_service_beam(ntn_location_mobility_config& cfg)
{
  ntn_beam_position third_beam{"CN-BEAM-0003", make_default_env_nci(2), 0.0, 2.0, 50000.0, true};
  third_beam.analog_beam_id = "ANALOG-ACCESS-001";
  cfg.beams.push_back(third_beam);
  cfg.max_nof_served_beams                 = 3;
  cfg.max_nof_loaded_digital_service_beams = 3;
  if (!cfg.analog_beams.empty()) {
    cfg.analog_beams.front().child_digital_beam_ids.push_back(third_beam.beam_id);
  }
}

ntn_location_mobility_config make_analog_aware_load_balancing_ntn_mobility_config(bool keep_second_analog_cold)
{
  ntn_location_mobility_config cfg = make_multi_beam_load_balancing_ntn_mobility_config(false);
  cfg.max_nof_served_beams                         = 3;
  cfg.max_nof_loaded_digital_service_beams         = 3;
  cfg.max_nof_active_analog_access_beams           = keep_second_analog_cold ? 1 : 2;
  cfg.multi_beam_load_balancing_handover_cooldown  = std::chrono::milliseconds{0};
  cfg.beams = {{"CN-BEAM-0001", make_default_env_nci(0), 0.0, 0.0, 50000.0, true},
               {"CN-BEAM-0002", make_default_env_nci(1), 0.0, 1.0, 50000.0, true},
               {"CN-BEAM-0003", make_default_env_nci(2), 0.0, 2.0, 50000.0, true}};
  cfg.beams[0].analog_beam_id = "ANALOG-ACCESS-001";
  cfg.beams[1].analog_beam_id = "ANALOG-ACCESS-001";
  cfg.beams[2].analog_beam_id = "ANALOG-ACCESS-002";

  ntn_analog_beam_position first_analog;
  first_analog.analog_beam_id         = "ANALOG-ACCESS-001";
  first_analog.center_digital_beam_id = "CN-BEAM-0001";
  first_analog.child_digital_beam_ids = {"CN-BEAM-0001", "CN-BEAM-0002"};
  first_analog.is_edge_partial        = true;
  first_analog.resource_policy.emplace();
  first_analog.resource_policy->max_service_bound_ues = 2;

  ntn_analog_beam_position second_analog;
  second_analog.analog_beam_id         = "ANALOG-ACCESS-002";
  second_analog.center_digital_beam_id = "CN-BEAM-0003";
  second_analog.child_digital_beam_ids = {"CN-BEAM-0003"};
  second_analog.is_edge_partial        = true;

  cfg.analog_beams = {first_analog, second_analog};
  return cfg;
}

ntn_location_mobility_config make_idle_paging_ntn_mobility_config()
{
  ntn_location_mobility_config cfg = make_access_service_layer_ntn_mobility_config();
  cfg.beams[0].beam_id             = "CN-BEAM-0007";
  cfg.beams[1].beam_id             = "CN-BEAM-ALT-0007";
  cfg.analog_beams[0].center_digital_beam_id = "CN-BEAM-0007";
  cfg.analog_beams[0].child_digital_beam_ids = {"CN-BEAM-0007", "CN-BEAM-ALT-0007"};
  return cfg;
}

ntn_location_mobility_config make_paired_access_ntn_mobility_config_with_service_sibling()
{
  ntn_location_mobility_config cfg = make_idle_paging_ntn_mobility_config();
  cfg.max_nof_served_beams                 = 3;
  cfg.max_nof_loaded_digital_service_beams = 3;
  cfg.beams[0].uplink_enabled              = false;
  cfg.beams[1].downlink_enabled            = false;
  cfg.beams[1].beam_id                     = "CN-BEAM-ACC-0007";

  ntn_beam_position service_beam{"CN-BEAM-SVC-0007", make_default_env_nci(2), 0.0, 2.0, 50000.0, true};
  service_beam.analog_beam_id = "ANALOG-ACCESS-001";
  cfg.beams.push_back(service_beam);
  cfg.analog_beams[0].child_digital_beam_ids = {"CN-BEAM-0007", "CN-BEAM-ACC-0007", "CN-BEAM-SVC-0007"};
  return cfg;
}

ntn_location_mobility_config make_service_pair_handover_ntn_mobility_config()
{
  ntn_location_mobility_config cfg = make_access_service_layer_ntn_mobility_config();
  cfg.max_nof_served_beams                 = 3;
  cfg.max_nof_loaded_digital_service_beams = 3;
  cfg.beams[1].uplink_enabled              = false;

  ntn_beam_position uplink_beam{"CN-BEAM-UL-0002", make_default_env_nci(2), 0.0, 1.0, 50000.0, true};
  uplink_beam.analog_beam_id  = "ANALOG-ACCESS-001";
  uplink_beam.downlink_enabled = false;
  uplink_beam.uplink_enabled   = true;
  cfg.beams.push_back(uplink_beam);
  cfg.analog_beams[0].child_digital_beam_ids = {"CN-BEAM-0001", "CN-BEAM-0002", "CN-BEAM-UL-0002"};
  return cfg;
}

ntn_location_mobility_config make_orbit_driven_ntn_mobility_config()
{
  ntn_location_mobility_config cfg = make_ntn_mobility_config();
  cfg.served_beam_min_elevation_deg = -90.0;
  cfg.satellite_state_update.source = ntn_satellite_state_source::circular_orbit;
  cfg.satellite_state_update.update_period = std::chrono::milliseconds{10};
  cfg.satellite_state_update.circular_altitude_m = 500000.0;
  cfg.satellite_state_update.circular_inclination_deg = 0.0;
  cfg.satellite_state_update.circular_epoch = std::chrono::system_clock::now();
  return cfg;
}

void enable_predictive_satellite_state_update(ntn_location_mobility_config& cfg)
{
  cfg.satellite_state_update.source = ntn_satellite_state_source::circular_orbit;
  cfg.satellite_state_update.update_period = std::chrono::seconds{30};
}

void enable_predictive_timeline_satellite_state_update(ntn_location_mobility_config& cfg)
{
  cfg.satellite_state_update.source = ntn_satellite_state_source::circular_orbit;
  cfg.satellite_state_update.update_period = std::chrono::seconds{1};
  cfg.satellite_state_update.predictive_service_window_horizon = std::chrono::seconds{3};
  cfg.satellite_state_update.predictive_handover_lead_time     = std::chrono::seconds{1};
}

byte_buffer make_integrity_protected_srb1_ul_pdcp_pdu(uint16_t pdcp_sn, byte_buffer rrc_sdu)
{
  static constexpr security::sec_128_key k_int = {0xf3,
                                                  0xd5,
                                                  0x99,
                                                  0x4a,
                                                  0x3b,
                                                  0x29,
                                                  0x06,
                                                  0xfb,
                                                  0x27,
                                                  0x00,
                                                  0x4a,
                                                  0x44,
                                                  0x90,
                                                  0x6c,
                                                  0x6b,
                                                  0xd1};
  static constexpr uint8_t               srb1_bearer = 0;

  byte_buffer pdcp_pdu;
  report_fatal_error_if_not(pdcp_pdu.append(static_cast<uint8_t>((pdcp_sn >> 8U) & 0xffU)),
                            "Failed to append PDCP header");
  report_fatal_error_if_not(pdcp_pdu.append(static_cast<uint8_t>(pdcp_sn & 0xffU)), "Failed to append PDCP header");
  report_fatal_error_if_not(pdcp_pdu.append(std::move(rrc_sdu)), "Failed to append RRC SDU");

  security::sec_mac mac = {};
  byte_buffer_view  integrity_input{pdcp_pdu};
  security::security_nia2(mac, k_int, pdcp_sn, srb1_bearer, security::security_direction::uplink, integrity_input);
  report_fatal_error_if_not(pdcp_pdu.append(span<const uint8_t>{mac.data(), mac.size()}), "Failed to append MAC-I");
  return pdcp_pdu;
}

asn1::rrc_nr::ue_cap_rat_container_list_l
make_supported_ntn_ue_capability_rat_container_list(asn1::rrc_nr::ue_nr_cap_v1700_s::ntn_scenario_support_r17_opts::options scenario)
{
  using namespace asn1::rrc_nr;

  ue_nr_cap_s cap;
  cap.access_stratum_release.value                    = access_stratum_release_opts::rel17;
  cap.pdcp_params.max_num_rohc_context_sessions.value = pdcp_params_s::max_num_rohc_context_sessions_opts::cs2;
  band_nr_s band;
  band.band_nr = 78;
  cap.rf_params.supported_band_list_nr.push_back(band);

  cap.non_crit_ext_present   = true;
  auto& v1530                = cap.non_crit_ext;
  v1530.non_crit_ext_present = true;
  auto& v1540                = v1530.non_crit_ext;
  v1540.non_crit_ext_present = true;
  auto& v1550                = v1540.non_crit_ext;
  v1550.non_crit_ext_present = true;
  auto& v1560                = v1550.non_crit_ext;
  v1560.non_crit_ext_present = true;
  auto& v1570                = v1560.non_crit_ext;
  v1570.non_crit_ext_present = true;
  auto& v1610                = v1570.non_crit_ext;
  v1610.non_crit_ext_present = true;
  auto& v1640                = v1610.non_crit_ext;
  v1640.non_crit_ext_present = true;
  auto& v1650                = v1640.non_crit_ext;
  v1650.non_crit_ext_present = true;
  auto& v1690                = v1650.non_crit_ext;
  v1690.non_crit_ext_present = true;
  ue_nr_cap_v1700_s& v1700   = v1690.non_crit_ext;
  v1700.non_terrestrial_network_r17_present = true;
  v1700.ntn_scenario_support_r17_present    = true;
  v1700.ntn_scenario_support_r17.value      = scenario;
  v1700.ntn_params_r17_present              = true;

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
  return list;
}

asn1::rrc_nr::ue_cap_rat_container_list_l make_supported_ntn_ngso_ue_capability_rat_container_list()
{
  return make_supported_ntn_ue_capability_rat_container_list(
      asn1::rrc_nr::ue_nr_cap_v1700_s::ntn_scenario_support_r17_opts::ngso);
}

asn1::rrc_nr::ue_cap_rat_container_list_l make_supported_ntn_gso_ue_capability_rat_container_list()
{
  return make_supported_ntn_ue_capability_rat_container_list(
      asn1::rrc_nr::ue_nr_cap_v1700_s::ntn_scenario_support_r17_opts::gso);
}

byte_buffer make_supported_ntn_ngso_ue_capability_info_pdu()
{
  using namespace asn1::rrc_nr;

  ul_dcch_msg_s msg;
  ue_cap_info_s& cap_info     = msg.msg.set_c1().set_ue_cap_info();
  cap_info.rrc_transaction_id = 2;
  ue_cap_info_ies_s& ies      = cap_info.crit_exts.set_ue_cap_info();
  ies.ue_cap_rat_container_list_present = true;
  ies.ue_cap_rat_container_list         = make_supported_ntn_ngso_ue_capability_rat_container_list();

  byte_buffer rrc_sdu = test_helpers::pack_ul_dcch_msg(msg);
  return make_integrity_protected_srb1_ul_pdcp_pdu(4, std::move(rrc_sdu));
}

byte_buffer make_supported_ntn_gso_ue_capability_info_pdu()
{
  using namespace asn1::rrc_nr;

  ul_dcch_msg_s msg;
  ue_cap_info_s& cap_info     = msg.msg.set_c1().set_ue_cap_info();
  cap_info.rrc_transaction_id = 2;
  ue_cap_info_ies_s& ies      = cap_info.crit_exts.set_ue_cap_info();
  ies.ue_cap_rat_container_list_present = true;
  ies.ue_cap_rat_container_list         = make_supported_ntn_gso_ue_capability_rat_container_list();

  byte_buffer rrc_sdu = test_helpers::pack_ul_dcch_msg(msg);
  return make_integrity_protected_srb1_ul_pdcp_pdu(4, std::move(rrc_sdu));
}

bool meas_config_requests_common_location_info(const std::optional<rrc_meas_cfg>& meas_cfg)
{
  if (!meas_cfg.has_value()) {
    return false;
  }
  for (const rrc_report_cfg_to_add_mod& report_cfg : meas_cfg->report_cfg_to_add_mod_list) {
    const asn1::rrc_nr::report_cfg_nr_s asn1_report_cfg = report_cfg_nr_to_rrc_asn1(report_cfg.report_cfg);
    if (asn1_report_cfg.report_type.type().value !=
        asn1::rrc_nr::report_cfg_nr_s::report_type_c_::types_opts::periodical) {
      continue;
    }
    if (asn1_report_cfg.report_type.periodical().include_common_location_info_r16_present) {
      return true;
    }
  }
  return false;
}

bool rrc_reconfiguration_requests_common_location_info(const f1ap_message& f1ap_pdu)
{
  if (!test_helpers::is_valid_dl_rrc_message_transfer(f1ap_pdu)) {
    return false;
  }

  const byte_buffer& rrc_container = test_helpers::get_rrc_container(f1ap_pdu);
  byte_buffer        dl_dcch_pdu   = test_helpers::extract_dl_dcch_msg(rrc_container);

  asn1::cbit_ref              bref{dl_dcch_pdu};
  asn1::rrc_nr::dl_dcch_msg_s dl_dcch;
  if (dl_dcch.unpack(bref) != asn1::SRSASN_SUCCESS) {
    return false;
  }
  if (dl_dcch.msg.type().value != asn1::rrc_nr::dl_dcch_msg_type_c::types_opts::c1 ||
      dl_dcch.msg.c1().type().value != asn1::rrc_nr::dl_dcch_msg_type_c::c1_c_::types_opts::rrc_recfg ||
      dl_dcch.msg.c1().rrc_recfg().crit_exts.type().value !=
          asn1::rrc_nr::rrc_recfg_s::crit_exts_c_::types_opts::rrc_recfg) {
    return false;
  }

  const asn1::rrc_nr::rrc_recfg_ies_s& recfg = dl_dcch.msg.c1().rrc_recfg().crit_exts.rrc_recfg();
  if (!recfg.meas_cfg_present) {
    return false;
  }
  for (const asn1::rrc_nr::report_cfg_to_add_mod_s& report_cfg : recfg.meas_cfg.report_cfg_to_add_mod_list) {
    if (report_cfg.report_cfg.type().value !=
        asn1::rrc_nr::report_cfg_to_add_mod_s::report_cfg_c_::types_opts::report_cfg_nr) {
      continue;
    }
    const asn1::rrc_nr::report_cfg_nr_s& report_cfg_nr = report_cfg.report_cfg.report_cfg_nr();
    if (report_cfg_nr.report_type.type().value !=
        asn1::rrc_nr::report_cfg_nr_s::report_type_c_::types_opts::periodical) {
      continue;
    }
    if (report_cfg_nr.report_type.periodical().include_common_location_info_r16_present) {
      return true;
    }
  }
  return false;
}

std::optional<uint8_t> get_rrc_reconfiguration_transaction_id(const f1ap_message& f1ap_pdu)
{
  if (!test_helpers::is_valid_dl_rrc_message_transfer(f1ap_pdu)) {
    return std::nullopt;
  }

  const byte_buffer& rrc_container = test_helpers::get_rrc_container(f1ap_pdu);
  byte_buffer        dl_dcch_pdu   = test_helpers::extract_dl_dcch_msg(rrc_container);

  asn1::cbit_ref              bref{dl_dcch_pdu};
  asn1::rrc_nr::dl_dcch_msg_s dl_dcch;
  if (dl_dcch.unpack(bref) != asn1::SRSASN_SUCCESS) {
    return std::nullopt;
  }
  if (dl_dcch.msg.type().value != asn1::rrc_nr::dl_dcch_msg_type_c::types_opts::c1 ||
      dl_dcch.msg.c1().type().value != asn1::rrc_nr::dl_dcch_msg_type_c::c1_c_::types_opts::rrc_recfg) {
    return std::nullopt;
  }
  return dl_dcch.msg.c1().rrc_recfg().rrc_transaction_id;
}

ntn_location_mobility_config make_stale_assistance_ntn_mobility_config(unsigned max_nof_served_beams = 1)
{
  ntn_location_mobility_config cfg       = make_ntn_mobility_config();
  cfg.served_beam_min_elevation_deg     = -90.0;
  cfg.max_nof_served_beams              = max_nof_served_beams;
  cfg.satellite_state_update.source     = ntn_satellite_state_source::manual;
  cfg.satellite_state_update.update_period = std::chrono::milliseconds{500};
  return cfg;
}

void expire_ntn_assistance()
{
  std::this_thread::sleep_for(std::chrono::milliseconds{1100});
}

template <typename Predicate>
bool wait_for_test_condition(Predicate&& predicate, std::chrono::milliseconds timeout = std::chrono::milliseconds{1000})
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  return predicate();
}

ntn_location_mobility_config make_multi_beam_hopping_ntn_mobility_config()
{
  ntn_location_mobility_config cfg;
  cfg.enabled                       = true;
  cfg.served_beam_min_elevation_deg = -90.0;
  cfg.max_nof_served_beams          = 3;
  cfg.beams = {{"CN-BEAM-0001", make_default_env_nci(0), 0.0, 0.0, 50000.0, true},
               {"CN-BEAM-0002", make_default_env_nci(1), 0.0, 1.0, 50000.0, true},
               {"CN-BEAM-0003", make_default_env_nci(2), 0.0, 2.0, 50000.0, true}};
  return cfg;
}

ntn_location_mobility_config make_rotating_multi_beam_hopping_ntn_mobility_config()
{
  ntn_location_mobility_config cfg;
  cfg.enabled                       = true;
  cfg.served_beam_min_elevation_deg = -90.0;
  cfg.max_nof_served_beams          = 2;
  cfg.served_beam_hopping_enabled   = true;
  cfg.beams = {{"CN-BEAM-0001", make_default_env_nci(0), 0.0, 0.0, 50000.0, true},
               {"CN-BEAM-0002", make_default_env_nci(1), 0.0, 0.0, 50000.0, true},
               {"CN-BEAM-0003", make_default_env_nci(2), 0.0, 0.0, 50000.0, true},
               {"CN-BEAM-0004", make_default_env_nci(3), 0.0, 0.0, 50000.0, true}};
  return cfg;
}

std::optional<unsigned> connect_du_for_ntn_beam_cells(cu_cp_test_environment&      env,
                                                      const std::vector<unsigned>& sector_ids,
                                                      gnb_du_id_t gnb_du_id = int_to_gnb_du_id(0x11))
{
  const std::optional<unsigned> du_idx = env.connect_new_du();
  if (!du_idx.has_value()) {
    return std::nullopt;
  }

  std::vector<test_helpers::served_cell_item_info> cells;
  cells.reserve(sector_ids.size());
  for (unsigned sector_id : sector_ids) {
    test_helpers::served_cell_item_info cell;
    cell.nci = make_default_env_nci(sector_id);
    cell.pci = sector_id + 1;
    cells.push_back(cell);
  }

  if (!env.run_f1_setup(du_idx.value(), gnb_du_id, cells)) {
    return std::nullopt;
  }
  return du_idx;
}

std::optional<unsigned> connect_du_for_ntn_beams(cu_cp_test_environment& env)
{
  return connect_du_for_ntn_beam_cells(env, {0, 1});
}

f1ap_message
generate_positioning_assistance_information_feedback(unsigned transaction_id, const nr_cell_global_id_t& cgi)
{
  f1ap_message feedback;
  feedback.pdu.set_init_msg().load_info_obj(ASN1_F1AP_ID_POSITIONING_ASSIST_INFO_FEEDBACK);
  auto& asn1_feedback = feedback.pdu.init_msg().value.positioning_assist_info_feedback();
  asn1_feedback->transaction_id = transaction_id;
  asn1_feedback->positioning_broadcast_cells_present = true;
  asn1_feedback->positioning_broadcast_cells.push_back(cgi_to_asn1(cgi));
  asn1_feedback->routing_id_present = true;
  asn1_feedback->routing_id.from_string("ab");
  return feedback;
}

ntn_ue_location_report make_ntn_location_report(ue_index_t                         ue_index,
                                                nr_cell_identity                   serving_nci,
                                                std::chrono::steady_clock::time_point received_time)
{
  ntn_ue_location_report report;
  report.ue_index              = ue_index;
  report.serving_nci           = serving_nci;
  report.latitude_deg          = 0.0;
  report.longitude_deg         = 0.0;
  report.horizontal_accuracy_m = 25.0;
  report.received_time         = received_time;
  return report;
}

ntn_ue_location_report make_ntn_location_report(ue_index_t ue_index, nr_cell_identity serving_nci)
{
  return make_ntn_location_report(ue_index, serving_nci, std::chrono::steady_clock::now());
}

cu_cp_ntn_beam_status find_beam_status(const std::vector<cu_cp_ntn_beam_status>& beam_status,
                                       const std::string&                         beam_id)
{
  auto it = std::find_if(beam_status.begin(),
                         beam_status.end(),
                         [&beam_id](const cu_cp_ntn_beam_status& status) {
                           return status.beam_id == beam_id;
                         });
  EXPECT_NE(it, beam_status.end());
  return it != beam_status.end() ? *it : cu_cp_ntn_beam_status{};
}

bool drain_sib19_broadcast_until_state(cu_cp_test_environment&    env,
                                       unsigned                   du_idx,
                                       cu_cp_ntn_command_handler& ntn_handler,
                                       const std::string&         beam_id,
                                       std::string_view           expected_state,
                                       std::chrono::milliseconds  timeout = std::chrono::milliseconds{1000})
{
  return env.tick_until(timeout,
                        [&]() {
                          env.drain_f1ap_resource_coordination_requests(du_idx);
                          return find_beam_status(ntn_handler.get_current_ntn_beam_status(), beam_id)
                                     .sib19_broadcast_state == expected_state;
                        },
                        false);
}

cu_cp_ntn_ue_status find_ue_status(const std::vector<cu_cp_ntn_ue_status>& ue_status, ue_index_t ue_index)
{
  auto it = std::find_if(ue_status.begin(),
                         ue_status.end(),
                         [ue_index](const cu_cp_ntn_ue_status& status) {
                           return status.ue_index == ue_index;
                         });
  EXPECT_NE(it, ue_status.end());
  return it != ue_status.end() ? *it : cu_cp_ntn_ue_status{};
}

struct connected_ngap_ntn_ue {
  unsigned     du_idx = 0;
  ue_index_t   ue_index = ue_index_t::invalid;
  ran_ue_id_t  ran_ue_id = ran_ue_id_t::invalid;
  amf_ue_id_t  amf_ue_id = amf_ue_id_t::invalid;
};

bool connect_cu_up_for_ue_admission(cu_cp_test_environment& env)
{
  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  return cu_up_idx.has_value() && env.run_e1_setup(cu_up_idx.value());
}

std::optional<connected_ngap_ntn_ue> connect_ngap_ntn_ue(cu_cp_test_environment& env)
{
  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  if (!du_idx.has_value()) {
    return std::nullopt;
  }
  if (!connect_cu_up_for_ue_admission(env)) {
    return std::nullopt;
  }

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  if (!env.connect_new_ue(du_idx.value(), du_ue_id, to_rnti(0x4601))) {
    return std::nullopt;
  }
  if (!env.authenticate_ue(du_idx.value(), du_ue_id, uint_to_amf_ue_id(0))) {
    return std::nullopt;
  }

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  if (ue_ctx == nullptr || !ue_ctx->cu_ue_id.has_value() || !ue_ctx->ran_ue_id.has_value() ||
      !ue_ctx->amf_ue_id.has_value()) {
    return std::nullopt;
  }

  connected_ngap_ntn_ue connected_ue;
  connected_ue.du_idx    = du_idx.value();
  connected_ue.ue_index  = uint_to_ue_index(gnb_cu_ue_f1ap_id_to_uint(ue_ctx->cu_ue_id.value()));
  connected_ue.ran_ue_id = ue_ctx->ran_ue_id.value();
  connected_ue.amf_ue_id = ue_ctx->amf_ue_id.value();
  return connected_ue;
}

ngap_handover_request make_minimal_ngap_handover_request(ue_index_t                 ue_index,
                                                         const nr_cell_global_id_t& target_cgi)
{
  ngap_handover_request request;
  request.ue_index    = ue_index;
  request.handov_type = ngap_handov_type::intra5gs;
  request.cause       = ngap_cause_radio_network_t::ho_desirable_for_radio_reason;
  request.ue_aggr_max_bit_rate.ue_aggr_max_bit_rate_dl = 1'000'000;
  request.ue_aggr_max_bit_rate.ue_aggr_max_bit_rate_ul = 500'000;
  request.guami.plmn                                   = plmn_identity::test_value();
  request.guami.amf_region_id                          = 1;
  request.guami.amf_set_id                             = 1;
  request.guami.amf_pointer                            = 0;
  request.source_to_target_transparent_container.target_cell_id = target_cgi;
  request.source_to_target_transparent_container.rrc_container  = make_byte_buffer("deadbeef").value();

  return request;
}

cu_cp_impl_interface* get_cu_cp_impl(cu_cp_test_environment& env)
{
  return &static_cast<cu_cp_impl&>(env.get_cu_cp());
}

void assert_ngap_radio_cause(const ngap_cause_t& cause, ngap_cause_radio_network_t expected)
{
  ASSERT_TRUE(std::holds_alternative<ngap_cause_radio_network_t>(cause));
  ASSERT_EQ(std::get<ngap_cause_radio_network_t>(cause), expected);
}

void assert_f1ap_radio_network_cause(const asn1::f1ap::cause_c& cause,
                                     asn1::f1ap::cause_radio_network_opts::options expected)
{
  ASSERT_EQ(cause.type(), asn1::f1ap::cause_c::types::radio_network);
  ASSERT_EQ(cause.radio_network().value, expected);
}

bool try_ack_ntn_slot_update(cu_cp_test_environment& env, unsigned du_idx, const f1ap_message& f1ap_pdu);

void expect_and_ack_ntn_slot_update(cu_cp_test_environment&            env,
                                    unsigned                           du_idx,
                                    gnb_du_ue_f1ap_id_t                du_ue_id,
                                    gnb_cu_ue_f1ap_id_t                cu_ue_id,
                                    rnti_t                             crnti,
                                    f1ap_ntn_ul_slot_resource_request* decoded_request = nullptr)
{
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{1000};
  do {
    f1ap_message f1ap_pdu;
    ASSERT_TRUE(env.wait_for_f1ap_tx_pdu(du_idx, f1ap_pdu, std::chrono::milliseconds{20}));
    ASSERT_TRUE(test_helpers::is_valid_ue_context_modification_request(f1ap_pdu));

    const auto& mod_req = f1ap_pdu.pdu.init_msg().value.ue_context_mod_request();
    ASSERT_TRUE(mod_req->res_coordination_transfer_container_present);
    const std::optional<f1ap_ntn_ul_slot_resource_request> slot_request =
        decode_f1ap_ntn_ul_slot_resource_request(mod_req->res_coordination_transfer_container);
    ASSERT_TRUE(slot_request.has_value());

    const gnb_du_ue_f1ap_id_t received_du_ue_id = int_to_gnb_du_ue_f1ap_id(mod_req->gnb_du_ue_f1ap_id);
    const gnb_cu_ue_f1ap_id_t received_cu_ue_id = int_to_gnb_cu_ue_f1ap_id(mod_req->gnb_cu_ue_f1ap_id);
    if (received_du_ue_id != du_ue_id || received_cu_ue_id != cu_ue_id) {
      ASSERT_TRUE(try_ack_ntn_slot_update(env, du_idx, f1ap_pdu));
      continue;
    }

    if (decoded_request != nullptr) {
      *decoded_request = *slot_request;
    }

    env.get_du(du_idx).push_ul_pdu(test_helpers::generate_ue_context_modification_response(
        du_ue_id, cu_ue_id, crnti, {}, {}, byte_buffer{}, make_successful_ntn_ul_slot_result(slot_request)));
    return;
  } while (std::chrono::steady_clock::now() < deadline);
  FAIL() << "Timed out waiting for expected NTN slot update";
}

bool try_ack_ntn_slot_update(cu_cp_test_environment& env, unsigned du_idx, const f1ap_message& f1ap_pdu)
{
  if (!test_helpers::is_valid_ue_context_modification_request(f1ap_pdu)) {
    return false;
  }

  const auto& mod_req = f1ap_pdu.pdu.init_msg().value.ue_context_mod_request();
  if (!mod_req->res_coordination_transfer_container_present) {
    return false;
  }
  const std::optional<f1ap_ntn_ul_slot_resource_request> slot_request =
      decode_f1ap_ntn_ul_slot_resource_request(mod_req->res_coordination_transfer_container);
  if (!slot_request.has_value()) {
    return false;
  }

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(mod_req->gnb_du_ue_f1ap_id);
  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx, du_ue_id);
  if (ue_ctx == nullptr || !ue_ctx->cu_ue_id.has_value() || ue_ctx->crnti == rnti_t::INVALID_RNTI) {
    return false;
  }

  env.get_du(du_idx).push_ul_pdu(test_helpers::generate_ue_context_modification_response(
      du_ue_id, ue_ctx->cu_ue_id.value(), ue_ctx->crnti, {}, {}, byte_buffer{}, make_successful_ntn_ul_slot_result(slot_request)));
  return true;
}

bool wait_for_ue_context_setup_request(cu_cp_test_environment& env,
                                       unsigned                du_idx,
                                       f1ap_message&           setup_request,
                                       std::chrono::milliseconds timeout = std::chrono::milliseconds{1000})
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  do {
    f1ap_message f1ap_pdu;
    if (!env.wait_for_f1ap_tx_pdu(du_idx, f1ap_pdu, std::chrono::milliseconds{20})) {
      continue;
    }
    if (test_helpers::is_valid_ue_context_setup_request_with_ue_capabilities(f1ap_pdu)) {
      setup_request = std::move(f1ap_pdu);
      return true;
    }
    if (try_ack_ntn_slot_update(env, du_idx, f1ap_pdu)) {
      continue;
    }
  } while (std::chrono::steady_clock::now() < deadline);
  return false;
}

bool wait_for_ue_context_modification_request(cu_cp_test_environment& env,
                                              unsigned                du_idx,
                                              f1ap_message&           mod_request,
                                              std::chrono::milliseconds timeout = std::chrono::milliseconds{1000})
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  do {
    f1ap_message f1ap_pdu;
    if (!env.wait_for_f1ap_tx_pdu(du_idx, f1ap_pdu, std::chrono::milliseconds{20})) {
      continue;
    }
    if (try_ack_ntn_slot_update(env, du_idx, f1ap_pdu)) {
      continue;
    }
    if (test_helpers::is_valid_ue_context_modification_request(f1ap_pdu)) {
      mod_request = std::move(f1ap_pdu);
      return true;
    }
  } while (std::chrono::steady_clock::now() < deadline);
  return false;
}

void expect_no_f1ap_ue_setup_or_release(cu_cp_test_environment& env,
                                        unsigned                du_idx,
                                        std::chrono::milliseconds timeout = std::chrono::milliseconds{50})
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  do {
    f1ap_message f1ap_pdu;
    if (!env.wait_for_f1ap_tx_pdu(du_idx, f1ap_pdu, std::chrono::milliseconds{10})) {
      continue;
    }
    if (try_ack_ntn_slot_update(env, du_idx, f1ap_pdu)) {
      continue;
    }
    EXPECT_FALSE(test_helpers::is_valid_ue_context_setup_request_with_ue_capabilities(f1ap_pdu));
    EXPECT_FALSE(test_helpers::is_valid_ue_context_release_command(f1ap_pdu));
  } while (std::chrono::steady_clock::now() < deadline);
}

void expect_service_binding_ready(cu_cp_test_environment&   env,
                                  unsigned                  du_idx,
                                  cu_cp_ntn_command_handler& ntn_handler,
                                  ue_index_t                ue_index)
{
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{20};
  do {
    f1ap_message service_binding_f1ap_pdu;
    if (!env.wait_for_f1ap_tx_pdu(du_idx, service_binding_f1ap_pdu, std::chrono::milliseconds{5})) {
      continue;
    }
    ASSERT_TRUE(try_ack_ntn_slot_update(env, du_idx, service_binding_f1ap_pdu));
  } while (std::chrono::steady_clock::now() < deadline);
  ASSERT_TRUE(env.tick_until(std::chrono::milliseconds{100}, [&]() {
    const std::vector<cu_cp_ntn_ue_status> ue_status = ntn_handler.get_current_ntn_ue_status();
    const auto status_it = std::find_if(ue_status.begin(),
                                        ue_status.end(),
                                        [ue_index](const cu_cp_ntn_ue_status& status) {
                                          return status.ue_index == ue_index;
                                        });
    return status_it != ue_status.end() && status_it->service_layer_state == "service_bound";
  }));
}

ngap_message make_pdu_session_resource_setup_request_with_five_qi(amf_ue_id_t      amf_ue_id,
                                                                  ran_ue_id_t      ran_ue_id,
                                                                  pdu_session_id_t psi,
                                                                  qos_flow_id_t    qfi,
                                                                  uint16_t         five_qi,
                                                                  bool             include_gbr_qos_info)
{
  ngap_message request = generate_valid_pdu_session_resource_setup_request_message(
      amf_ue_id, ran_ue_id, {{psi, {pdu_session_type_t::ipv4, {{qfi, five_qi}}}}});
  if (include_gbr_qos_info) {
    return request;
  }

  auto& setup_req = request.pdu.init_msg().value.pdu_session_res_setup_request();
  for (auto& setup_item : setup_req->pdu_session_res_setup_list_su_req) {
    asn1::ngap::pdu_session_res_setup_request_transfer_s asn1_setup_req_transfer;
    asn1::cbit_ref bref({setup_item.pdu_session_res_setup_request_transfer.begin(),
                         setup_item.pdu_session_res_setup_request_transfer.end()});
    report_fatal_error_if_not(asn1_setup_req_transfer.unpack(bref) == asn1::SRSASN_SUCCESS,
                              "Failed to unpack PDU Session Resource Setup Request Transfer");
    for (auto& qos_flow : asn1_setup_req_transfer->qos_flow_setup_request_list) {
      qos_flow.qos_flow_level_qos_params.gbr_qos_info_present = false;
    }
    setup_item.pdu_session_res_setup_request_transfer = pack_into_pdu(asn1_setup_req_transfer);
  }
  return request;
}

bool setup_pdu_session_with_five_qi(cu_cp_test_environment& env,
                                    unsigned                du_idx,
                                    unsigned                cu_up_idx,
                                    gnb_du_ue_f1ap_id_t     du_ue_id,
                                    rnti_t                  crnti,
                                    gnb_cu_up_ue_e1ap_id_t  cu_up_e1ap_id,
                                    pdu_session_id_t        psi,
                                    drb_id_t                drb_id,
                                    qos_flow_id_t           qfi,
                                    uint16_t                five_qi,
                                    bool                    is_initial_session,
                                    unsigned                rrc_recfg_transaction_id = 3,
                                    uint8_t                 rrc_recfg_count = 7,
                                    bool                    include_gbr_qos_info = true)
{
  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx, du_ue_id);
  if (ue_ctx == nullptr || !ue_ctx->amf_ue_id.has_value() || !ue_ctx->ran_ue_id.has_value()) {
    return false;
  }

  const ngap_message pdu_session_resource_setup_request =
      make_pdu_session_resource_setup_request_with_five_qi(ue_ctx->amf_ue_id.value(),
                                                           ue_ctx->ran_ue_id.value(),
                                                           psi,
                                                           qfi,
                                                           five_qi,
                                                           include_gbr_qos_info);

  if (is_initial_session) {
    if (!env.send_pdu_session_resource_setup_request_and_await_bearer_context_setup_request(
            pdu_session_resource_setup_request, du_idx, cu_up_idx, du_ue_id)) {
      return false;
    }
    if (!env.send_bearer_context_setup_response_and_await_ue_context_modification_request(
            du_idx, cu_up_idx, du_ue_id, cu_up_e1ap_id, psi, qfi)) {
      return false;
    }
  } else {
    if (!env.send_pdu_session_resource_setup_request_and_await_bearer_context_modification_request(
            pdu_session_resource_setup_request, cu_up_idx)) {
      return false;
    }
    if (!env.send_bearer_context_modification_response_and_await_ue_context_modification_request(
            du_idx, cu_up_idx, du_ue_id, psi, drb_id, qfi)) {
      return false;
    }
  }

  if (!env.send_ue_context_modification_response_and_await_bearer_context_modification_request(
          du_idx, cu_up_idx, du_ue_id, crnti)) {
    return false;
  }
  if (!env.send_bearer_context_modification_response_and_await_rrc_reconfiguration(
          du_idx, cu_up_idx, du_ue_id, {}, {{psi, drb_id}})) {
    return false;
  }
  return env.send_rrc_reconfiguration_complete_and_await_pdu_session_setup_response(
      du_idx, du_ue_id, generate_rrc_reconfiguration_complete_pdu(rrc_recfg_transaction_id, rrc_recfg_count), {psi}, {});
}

ue_index_t finish_ntn_access_only_registration(cu_cp_test_environment& env,
                                               unsigned                du_idx,
                                               unsigned                cu_up_idx,
                                               gnb_du_ue_f1ap_id_t     du_ue_id,
                                               rnti_t                  crnti,
                                               amf_ue_id_t             amf_ue_id,
                                               byte_buffer             ue_capability_info_pdu = {},
                                               std::optional<cu_cp_five_g_s_tmsi> five_g_s_tmsi = std::nullopt,
                                               std::optional<nr_cell_identity> serving_nci = std::nullopt)
{
  if (!env.connect_new_ue(du_idx, du_ue_id, crnti, plmn_identity::test_value(), five_g_s_tmsi, serving_nci)) {
    return ue_index_t::invalid;
  }
  if (!env.authenticate_ue(du_idx, du_ue_id, amf_ue_id)) {
    return ue_index_t::invalid;
  }
  if (!env.setup_ue_security(du_idx, du_ue_id, std::move(ue_capability_info_pdu))) {
    return ue_index_t::invalid;
  }
  if (!env.finish_ue_registration(du_idx, cu_up_idx, du_ue_id)) {
    return ue_index_t::invalid;
  }
  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx, du_ue_id);
  EXPECT_NE(ue_ctx, nullptr);
  EXPECT_TRUE(ue_ctx != nullptr && ue_ctx->cu_ue_id.has_value());
  return ue_ctx != nullptr && ue_ctx->cu_ue_id.has_value()
             ? uint_to_ue_index(gnb_cu_ue_f1ap_id_to_uint(ue_ctx->cu_ue_id.value()))
             : ue_index_t::invalid;
}

struct service_bound_ntn_ue_context {
  unsigned              du_idx = 0;
  unsigned              cu_up_idx = 0;
  gnb_du_ue_f1ap_id_t   du_ue_id = gnb_du_ue_f1ap_id_t::invalid;
  gnb_cu_ue_f1ap_id_t   cu_ue_id = gnb_cu_ue_f1ap_id_t::invalid;
  rnti_t                crnti = rnti_t::INVALID_RNTI;
  ue_index_t            ue_index = ue_index_t::invalid;
  amf_ue_id_t           amf_ue_id = amf_ue_id_t::invalid;
  gnb_cu_up_ue_e1ap_id_t cu_up_e1ap_id = gnb_cu_up_ue_e1ap_id_t::invalid;
  gnb_cu_cp_ue_e1ap_id_t cu_cp_e1ap_id = gnb_cu_cp_ue_e1ap_id_t::invalid;
};

ue_index_t complete_ntn_connected_handover_success(cu_cp_test_environment&             env,
                                                   const service_bound_ntn_ue_context& source_ue,
                                                   const f1ap_message&                 target_setup,
                                                   rnti_t                              target_crnti)
{
  const auto& setup_req = target_setup.pdu.init_msg().value.ue_context_setup_request();
  const gnb_cu_ue_f1ap_id_t target_cu_ue_id = int_to_gnb_cu_ue_f1ap_id(setup_req->gnb_cu_ue_f1ap_id);
  const gnb_du_ue_f1ap_id_t target_du_ue_id = int_to_gnb_du_ue_f1ap_id(51);
  std::optional<f1ap_ntn_ul_slot_resource_request> target_slot_request;
  if (setup_req->res_coordination_transfer_container_present) {
    target_slot_request = decode_f1ap_ntn_ul_slot_resource_request(setup_req->res_coordination_transfer_container);
  }

  f1ap_message target_setup_response =
      test_helpers::generate_ue_context_setup_response(target_cu_ue_id, target_du_ue_id, target_crnti);
  if (target_slot_request.has_value()) {
    auto& setup_resp = target_setup_response.pdu.successful_outcome().value.ue_context_setup_resp();
    setup_resp->res_coordination_transfer_container_present = true;
    setup_resp->res_coordination_transfer_container =
        encode_f1ap_ntn_ul_slot_resource_result(*make_successful_ntn_ul_slot_result(target_slot_request));
  }
  env.get_du(source_ue.du_idx).push_ul_pdu(std::move(target_setup_response));

  f1ap_message source_mod;
  EXPECT_TRUE(wait_for_ue_context_modification_request(env, source_ue.du_idx, source_mod));
  env.get_du(source_ue.du_idx)
      .push_ul_pdu(test_helpers::generate_ue_context_modification_response(
          source_ue.du_ue_id, source_ue.cu_ue_id, source_ue.crnti));

  env.get_du(source_ue.du_idx)
      .push_ul_pdu(test_helpers::generate_ul_rrc_message_transfer(
          target_du_ue_id, target_cu_ue_id, srb_id_t::srb1, make_byte_buffer("80000800db659eb2").value()));

  e1ap_message e1ap_pdu;
  EXPECT_TRUE(env.wait_for_e1ap_tx_pdu(source_ue.cu_up_idx, e1ap_pdu, std::chrono::milliseconds{1000}));
  EXPECT_TRUE(test_helpers::is_valid_bearer_context_modification_request(e1ap_pdu));
  const auto& bearer_mod_req = e1ap_pdu.pdu.init_msg().value.bearer_context_mod_request();
  const gnb_cu_cp_ue_e1ap_id_t cu_cp_e1ap_id =
      int_to_gnb_cu_cp_ue_e1ap_id(bearer_mod_req->gnb_cu_cp_ue_e1ap_id);
  const gnb_cu_up_ue_e1ap_id_t cu_up_e1ap_id =
      int_to_gnb_cu_up_ue_e1ap_id(bearer_mod_req->gnb_cu_up_ue_e1ap_id);
  env.get_cu_up(source_ue.cu_up_idx)
      .push_tx_pdu(generate_bearer_context_modification_response(cu_cp_e1ap_id, cu_up_e1ap_id));

  f1ap_message target_mod;
  EXPECT_TRUE(wait_for_ue_context_modification_request(env, source_ue.du_idx, target_mod));
  env.get_du(source_ue.du_idx)
      .push_ul_pdu(
          test_helpers::generate_ue_context_modification_response(target_du_ue_id, target_cu_ue_id, target_crnti));

  f1ap_message release_pdu;
  EXPECT_TRUE(env.wait_for_f1ap_tx_pdu(source_ue.du_idx, release_pdu, std::chrono::milliseconds{1000}));
  EXPECT_TRUE(test_helpers::is_valid_ue_context_release_command(release_pdu));
  const auto& release_cmd = release_pdu.pdu.init_msg().value.ue_context_release_cmd();
  env.get_du(source_ue.du_idx)
      .push_ul_pdu(test_helpers::generate_ue_context_release_complete(
          int_to_gnb_cu_ue_f1ap_id(release_cmd->gnb_cu_ue_f1ap_id),
          int_to_gnb_du_ue_f1ap_id(release_cmd->gnb_du_ue_f1ap_id)));

  EXPECT_TRUE(env.tick_until(std::chrono::milliseconds{1000}, [&]() {
    return env.get_cu_cp().get_metrics_handler().request_metrics_report().ues.size() == 2;
  }));
  return uint_to_ue_index(gnb_cu_ue_f1ap_id_to_uint(target_cu_ue_id));
}

std::optional<service_bound_ntn_ue_context>
setup_service_bound_ntn_ue(cu_cp_test_environment&                  env,
                           cu_cp_ntn_command_handler&               ntn_handler,
                           bool                                      seed_location,
                           std::chrono::steady_clock::time_point     location_time = std::chrono::steady_clock::now(),
                           std::optional<cu_cp_five_g_s_tmsi> five_g_s_tmsi = std::nullopt)
{
  const std::optional<unsigned> du_idx = connect_du_for_ntn_beam_cells(env, {0, 1});
  if (!du_idx.has_value()) {
    return std::nullopt;
  }

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  if (!cu_up_idx.has_value() || !env.run_e1_setup(cu_up_idx.value())) {
    return std::nullopt;
  }

  if (!ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0))) {
    return std::nullopt;
  }

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  const rnti_t              crnti    = to_rnti(0x4601);
  const ue_index_t ue_index = finish_ntn_access_only_registration(env,
                                                                  du_idx.value(),
                                                                  cu_up_idx.value(),
                                                                  du_ue_id,
                                                                  crnti,
                                                                  amf_ue_id_t::min,
                                                                  make_supported_ntn_ngso_ue_capability_info_pdu(),
                                                                  five_g_s_tmsi);
  if (ue_index == ue_index_t::invalid) {
    return std::nullopt;
  }

  if (seed_location) {
    ntn_ue_location_report report = make_ntn_location_report(ue_index, make_default_env_nci(0), location_time);
    get_cu_cp_impl(env)->get_cu_cp_measurement_handler().handle_ue_location_report(report);
    env.drain_f1ap_resource_coordination_requests(du_idx.value());
  }

  if (!env.request_pdu_session_resource_setup(du_idx.value(), cu_up_idx.value(), du_ue_id) ||
      !setup_pdu_session_with_five_qi(env,
                                      du_idx.value(),
                                      cu_up_idx.value(),
                                      du_ue_id,
                                      crnti,
                                      int_to_gnb_cu_up_ue_e1ap_id(0),
                                      pdu_session_id_t::min,
                                      drb_id_t::drb1,
                                      uint_to_qos_flow_id(0),
                                      9,
                                      true)) {
    return std::nullopt;
  }

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  if (ue_ctx == nullptr || !ue_ctx->cu_ue_id.has_value() || !ue_ctx->amf_ue_id.has_value() ||
      !ue_ctx->cu_cp_e1ap_id.has_value() ||
      !ue_ctx->cu_up_e1ap_id.has_value()) {
    return std::nullopt;
  }

  expect_and_ack_ntn_slot_update(env, du_idx.value(), du_ue_id, ue_ctx->cu_ue_id.value(), crnti);
  env.drain_f1ap_resource_coordination_requests(du_idx.value());
  expect_service_binding_ready(env, du_idx.value(), ntn_handler, ue_index);

  return service_bound_ntn_ue_context{du_idx.value(),
                                      cu_up_idx.value(),
                                      du_ue_id,
                                      ue_ctx->cu_ue_id.value(),
                                      crnti,
                                      ue_index,
                                      ue_ctx->amf_ue_id.value(),
                                      ue_ctx->cu_up_e1ap_id.value(),
                                      ue_ctx->cu_cp_e1ap_id.value()};
}

std::optional<service_bound_ntn_ue_context>
setup_service_bound_ntn_ue_on_existing_connections(cu_cp_test_environment&              env,
                                                   cu_cp_ntn_command_handler&           ntn_handler,
                                                   unsigned                             du_idx,
                                                   unsigned                             cu_up_idx,
                                                   unsigned                             ue_ordinal,
                                                   double                               location_longitude_deg,
                                                   bool                                 expect_quiet_after_binding = true,
                                                   unsigned                             location_serving_beam_ordinal = 0)
{
  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(ue_ordinal);
  const rnti_t              crnti    = to_rnti(0x4601 + ue_ordinal);
  const ue_index_t ue_index = finish_ntn_access_only_registration(
      env,
      du_idx,
      cu_up_idx,
      du_ue_id,
      crnti,
      uint_to_amf_ue_id(ue_ordinal),
      make_supported_ntn_ngso_ue_capability_info_pdu());
  if (ue_index == ue_index_t::invalid) {
    return std::nullopt;
  }

  ntn_ue_location_report report = make_ntn_location_report(ue_index, make_default_env_nci(location_serving_beam_ordinal));
  report.longitude_deg          = location_longitude_deg;
  get_cu_cp_impl(env)->get_cu_cp_measurement_handler().handle_ue_location_report(report);
  env.drain_f1ap_resource_coordination_requests(du_idx);

  const pdu_session_id_t psi = ue_ordinal == 0 ? pdu_session_id_t::min : uint_to_pdu_session_id(ue_ordinal + 1);
  const qos_flow_id_t    qfi = ue_ordinal == 0 ? qos_flow_id_t::min : uint_to_qos_flow_id(ue_ordinal + 1);
  const drb_id_t         drb = ue_ordinal == 0 ? drb_id_t::drb1 : uint_to_drb_id(ue_ordinal + 1);
  if (!env.request_pdu_session_resource_setup(du_idx, cu_up_idx, du_ue_id) ||
      !setup_pdu_session_with_five_qi(env,
                                      du_idx,
                                      cu_up_idx,
                                      du_ue_id,
                                      crnti,
                                      int_to_gnb_cu_up_ue_e1ap_id(ue_ordinal),
                                      psi,
                                      drb,
                                      qfi,
                                      9,
                                      true)) {
    return std::nullopt;
  }

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx, du_ue_id);
  if (ue_ctx == nullptr || !ue_ctx->cu_ue_id.has_value() || !ue_ctx->amf_ue_id.has_value() ||
      !ue_ctx->cu_cp_e1ap_id.has_value() || !ue_ctx->cu_up_e1ap_id.has_value()) {
    return std::nullopt;
  }

  if (expect_quiet_after_binding) {
    expect_and_ack_ntn_slot_update(env, du_idx, du_ue_id, ue_ctx->cu_ue_id.value(), crnti);
    env.drain_f1ap_resource_coordination_requests(du_idx);
    expect_service_binding_ready(env, du_idx, ntn_handler, ue_index);
  }

  return service_bound_ntn_ue_context{du_idx,
                                      cu_up_idx,
                                      du_ue_id,
                                      ue_ctx->cu_ue_id.value(),
                                      crnti,
                                      ue_index,
                                      ue_ctx->amf_ue_id.value(),
                                      ue_ctx->cu_up_e1ap_id.value(),
                                      ue_ctx->cu_cp_e1ap_id.value()};
}

cu_cp_five_g_s_tmsi make_test_paging_five_g_s_tmsi()
{
  return cu_cp_five_g_s_tmsi{1, 0, 4211117727};
}

ngap_message make_paging_message_with_recommended_cell(nr_cell_identity nci, unsigned tac = 7)
{
  ngap_message paging_msg = generate_valid_minimal_paging_message();
  auto&        paging     = paging_msg.pdu.init_msg().value.paging();
  paging->tai_list_for_paging[0].tai.tac.from_number(tac);

  paging->assist_data_for_paging_present                                   = true;
  paging->assist_data_for_paging.assist_data_for_recommended_cells_present = true;
  auto& recommended_cells = paging->assist_data_for_paging.assist_data_for_recommended_cells
                                .recommended_cells_for_paging.recommended_cell_list;
  recommended_cells.clear();

  asn1::ngap::recommended_cell_item_s recommended_cell;
  auto&                               nr_cgi = recommended_cell.ngran_cgi.set_nr_cgi();
  nr_cgi.plmn_id.from_string("00f110");
  nr_cgi.nr_cell_id.from_number(nci.value());
  recommended_cells.push_back(recommended_cell);
  return paging_msg;
}

void complete_service_bound_ue_release(cu_cp_test_environment& env, const service_bound_ntn_ue_context& ue)
{
  env.get_amf().push_tx_pdu(generate_valid_ue_context_release_command_with_amf_ue_ngap_id(ue.amf_ue_id));

  e1ap_message e1ap_pdu;
  ASSERT_TRUE(env.wait_for_e1ap_tx_pdu(ue.cu_up_idx, e1ap_pdu, std::chrono::milliseconds{1000}));
  ASSERT_TRUE(test_helpers::is_valid_bearer_context_release_command(e1ap_pdu));
  env.get_cu_up(ue.cu_up_idx).push_tx_pdu(generate_bearer_context_release_complete(ue.cu_cp_e1ap_id, ue.cu_up_e1ap_id));

  f1ap_message f1ap_pdu;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu(ue.du_idx, f1ap_pdu, std::chrono::milliseconds{1000}));
  ASSERT_TRUE(test_helpers::is_valid_ue_context_release_command(f1ap_pdu));
  const auto& rel_cmd = f1ap_pdu.pdu.init_msg().value.ue_context_release_cmd();
  env.get_du(ue.du_idx)
      .push_ul_pdu(test_helpers::generate_ue_context_release_complete(
          int_to_gnb_cu_ue_f1ap_id(rel_cmd->gnb_cu_ue_f1ap_id),
          int_to_gnb_du_ue_f1ap_id(rel_cmd->gnb_du_ue_f1ap_id)));

  ngap_message ngap_pdu;
  ASSERT_TRUE(env.wait_for_ngap_tx_pdu(ngap_pdu, std::chrono::milliseconds{1000}));
  ASSERT_TRUE(test_helpers::is_valid_ue_context_release_complete(ngap_pdu));
  ASSERT_TRUE(env.tick_until(std::chrono::milliseconds{1000}, [&]() {
    return env.get_cu_cp().get_metrics_handler().request_metrics_report().ues.empty();
  }));
  env.drain_f1ap_resource_coordination_requests(ue.du_idx);
}

bool wait_for_f1ap_paging(cu_cp_test_environment& env,
                          unsigned                du_idx,
                          f1ap_message&           paging,
                          std::chrono::milliseconds timeout = std::chrono::milliseconds{1000})
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  do {
    f1ap_message f1ap_pdu;
    if (!env.wait_for_f1ap_tx_pdu(du_idx, f1ap_pdu, std::chrono::milliseconds{20})) {
      continue;
    }
    if (test_helpers::is_valid_paging(f1ap_pdu)) {
      paging = std::move(f1ap_pdu);
      return true;
    }
  } while (std::chrono::steady_clock::now() < deadline);
  return false;
}

void expect_paging_cells(const f1ap_message& paging, span<const nr_cell_identity> expected_ncis)
{
  const auto& asn1_paging = paging.pdu.init_msg().value.paging();
  ASSERT_EQ(asn1_paging->paging_cell_list.size(), expected_ncis.size());
  for (unsigned i = 0; i != expected_ncis.size(); ++i) {
    const auto& paging_cell = asn1_paging->paging_cell_list[i].value().paging_cell_item();
    EXPECT_EQ(paging_cell.nr_cgi.nr_cell_id.to_number(), expected_ncis[i].value());
  }
}

} // namespace

TEST(cu_cp_ntn_mobility_test, default_cu_cp_rejects_ntn_satellite_state_updates)
{
  cu_cp_test_environment env;

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_FALSE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));
  ASSERT_TRUE(ntn_handler.get_current_ntn_served_beam_ids().empty());
}

TEST(cu_cp_ntn_mobility_test, onboard_cell_du_resolution_requires_each_nci_pci_exactly_once_on_the_same_du)
{
  const nr_cell_identity                             first_nci     = make_default_env_nci(0);
  const nr_cell_identity                             second_nci    = make_default_env_nci(1);
  constexpr pci_t                                    shared_pci    = 101;
  const du_index_t                                   first_du      = uint_to_du_index(0);
  const du_index_t                                   second_du     = uint_to_du_index(1);
  const std::array<ntn_onboard_cell_position_set, 2> planned_cells = {
      ntn_onboard_cell_position_set{{first_nci, shared_pci}, {}},
      ntn_onboard_cell_position_set{{second_nci, shared_pci}, {}}};

  using candidate = ntn_onboard_detail::ntn_onboard_du_cell_candidate;
  using status    = ntn_onboard_detail::ntn_onboard_du_cell_resolution_status;

  const auto resolved = ntn_onboard_detail::resolve_ntn_onboard_du_cells(
      std::vector<candidate>{{first_du, first_nci, shared_pci, nullptr}, {first_du, second_nci, shared_pci, nullptr}},
      planned_cells);
  EXPECT_EQ(resolved.status, status::resolved);
  EXPECT_EQ(resolved.du_index, first_du);

  const auto unavailable = ntn_onboard_detail::resolve_ntn_onboard_du_cells({}, planned_cells);
  EXPECT_EQ(unavailable.status, status::unavailable);
  EXPECT_STREQ(ntn_onboard_detail::get_ntn_onboard_du_cell_resolution_detail(unavailable.status),
               "du_or_cells_unavailable");

  const auto missing = ntn_onboard_detail::resolve_ntn_onboard_du_cells(
      std::vector<candidate>{{first_du, first_nci, shared_pci, nullptr}}, planned_cells);
  EXPECT_EQ(missing.status, status::missing);
  EXPECT_STREQ(ntn_onboard_detail::get_ntn_onboard_du_cell_resolution_detail(missing.status),
               "missing_nci_pci_du_cell_mapping");

  // Two copies of the first onboard cell must not be mistaken for one match of each planned cell.
  const auto duplicate = ntn_onboard_detail::resolve_ntn_onboard_du_cells(
      std::vector<candidate>{{first_du, first_nci, shared_pci, nullptr}, {first_du, first_nci, shared_pci, nullptr}},
      planned_cells);
  EXPECT_EQ(duplicate.status, status::duplicate);

  const auto reconnect_overlap = ntn_onboard_detail::resolve_ntn_onboard_du_cells(
      std::vector<candidate>{{first_du, first_nci, shared_pci, nullptr},
                             {first_du, second_nci, shared_pci, nullptr},
                             {second_du, first_nci, shared_pci, nullptr},
                             {second_du, second_nci, shared_pci, nullptr}},
      planned_cells,
      std::set<du_index_t>{first_du});
  EXPECT_EQ(reconnect_overlap.status, status::resolved);
  EXPECT_EQ(reconnect_overlap.du_index, second_du);
  EXPECT_STREQ(ntn_onboard_detail::get_ntn_onboard_du_cell_resolution_detail(duplicate.status),
               "duplicate_nci_pci_du_cell_mapping");

  const auto cross_du = ntn_onboard_detail::resolve_ntn_onboard_du_cells(
      std::vector<candidate>{{first_du, first_nci, shared_pci, nullptr}, {second_du, second_nci, shared_pci, nullptr}},
      planned_cells);
  EXPECT_EQ(cross_du.status, status::cross_du);
  EXPECT_STREQ(ntn_onboard_detail::get_ntn_onboard_du_cell_resolution_detail(cross_du.status),
               "two_onboard_cells_resolve_to_different_dus");
}

TEST(cu_cp_ntn_mobility_test, versioned_two_cell_calendar_is_prepared_and_activated_only_after_du_applied_feedback)
{
  const nr_cell_identity first_nci  = make_default_env_nci(0);
  const nr_cell_identity second_nci = make_default_env_nci(1);
  constexpr pci_t        shared_pci = 101;
  const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
  const int64_t activation_ms = ((now_ms + 1000 + 639) / 640) * 640;

  ntn_versioned_position_plan plan;
  plan.satellite_id         = "P01-S01";
  plan.catalog_version  = 10;
  plan.schedule_version = 20;
  plan.valid_from       = std::chrono::system_clock::time_point{std::chrono::milliseconds{now_ms - 100}};
  plan.activation_epoch = std::chrono::system_clock::time_point{std::chrono::milliseconds{activation_ms}};
  plan.valid_until      = std::chrono::system_clock::time_point{std::chrono::milliseconds{activation_ms + 3200}};
  plan.onboard_cells[0] = {first_nci, shared_pci};
  plan.onboard_cells[1] = {second_nci, shared_pci};
  plan.visible_l1_positions = {{"G000001", 10.0, 20.0}, {"G000002", 10.1, 20.1}};
  bind_onboard_planning_context(plan);
  plan.content_hash          = compute_ntn_position_plan_content_hash(plan);
  const std::filesystem::path plan_path = write_onboard_position_plan_for_runtime_test(plan);
  temporary_plan_file_guard   plan_file_guard(plan_path);

  cu_cp_test_env_params env_params;
  ntn_onboard_position_plan_source_config source;
  source.enabled              = true;
  source.du_execution_enabled = true;
  source.du_prepare_guard     = std::chrono::milliseconds{100};
  source.satellite_id         = plan.satellite_id;
  source.plan_json_file       = plan_path.string();
  source.cell_ncis            = {first_nci, second_nci};
  source.cell_pcis            = {shared_pci, shared_pci};
  bind_onboard_planning_context(source);
  env_params.ntn_onboard_position_plan = source;

  {
    cu_cp_test_environment env(std::move(env_params));
    env.run_ng_setup();
    const auto du_idx = env.connect_new_du();
    ASSERT_TRUE(du_idx.has_value());
    std::vector<test_helpers::served_cell_item_info> served_cells(2);
    served_cells[0].nci = first_nci;
    served_cells[0].pci = shared_pci;
    served_cells[1].nci = second_nci;
    served_cells[1].pci = shared_pci;
    ASSERT_TRUE(env.run_f1_setup(du_idx.value(), int_to_gnb_du_id(0x11), served_cells));

    // The mock DU returns ready for prepare and applied for the post-epoch query. The helper consumes only resource
    // coordination traffic, so a timeout here is expected once both exchanges have completed.
    f1ap_message ignored;
    (void)env.wait_for_f1ap_tx_pdu(du_idx.value(), ignored, std::chrono::milliseconds{2600});

    const cu_cp_ntn_position_plan_status status = env.get_cu_cp()
                                                        .get_command_handler()
                                                        .get_ntn_command_handler()
                                                        .get_current_ntn_runtime_status()
                                                        .onboard_position_plan;
    EXPECT_EQ(status.stage, "active");
    EXPECT_EQ(status.deployment_stage, "applied");
    EXPECT_EQ(status.active_schedule_version, plan.schedule_version);
    EXPECT_EQ(status.execution_evidence, "ssb_prach_software_gate_applied_no_position_or_rf_evidence");
    EXPECT_EQ(status.cells[0].active_l1_positions + status.cells[1].active_l1_positions,
              plan.visible_l1_positions.size());
  }
}

TEST(cu_cp_ntn_mobility_test, schema_v4_signed_calendar_is_applied_persisted_and_reverified_on_restart)
{
  const nr_cell_identity first_nci  = make_default_env_nci(0);
  const nr_cell_identity second_nci = make_default_env_nci(1);
  constexpr pci_t        shared_pci = 101;
  constexpr const char*  signing_key_id = "ntn-runtime-test-key-v1";
  const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
  const int64_t activation_ms = ((now_ms + 1000 + 639) / 640) * 640;
  const auto activation_epoch =
      std::chrono::system_clock::time_point{std::chrono::milliseconds{activation_ms}};
  ntn_versioned_position_plan plan = make_schema_v3_runtime_plan(12,
                                                                  6,
                                                                  70,
                                                                  80,
                                                                  first_nci,
                                                                  second_nci,
                                                                  shared_pci,
                                                                  activation_epoch,
                                                                  activation_epoch + std::chrono::seconds{10});
  const auto unique_suffix = std::chrono::steady_clock::now().time_since_epoch().count();
  const std::filesystem::path public_key_path =
      std::filesystem::temp_directory_path() / fmt::format("srsran-ntn-signing-key-{}.pem", unique_suffix);
  temporary_plan_file_guard public_key_guard(public_key_path);
  plan = sign_schema_v4_runtime_plan(std::move(plan), public_key_path, signing_key_id);
  const std::filesystem::path plan_path = write_onboard_position_plan_for_runtime_test(plan);
  temporary_plan_file_guard   plan_file_guard(plan_path);

  auto source                    = make_schema_v3_runtime_source(plan, plan_path);
  source.require_signed_plan     = true;
  source.version_anchor_file     = plan_path.string() + ".anchor";
  source.trusted_signing_keys    = {{signing_key_id, public_key_path.string()}};

  {
    cu_cp_test_env_params env_params;
    env_params.ntn_onboard_position_plan = source;
    cu_cp_test_environment env(std::move(env_params));
    env.run_ng_setup();
    const auto du_idx = env.connect_new_du();
    ASSERT_TRUE(du_idx.has_value());
    ASSERT_TRUE(env.run_f1_setup(
        du_idx.value(), int_to_gnb_du_id(0x41), make_onboard_served_cells(first_nci, second_nci, shared_pci)));

    f1ap_message ignored;
    (void)env.wait_for_f1ap_tx_pdu(du_idx.value(), ignored, std::chrono::milliseconds{2600});
    const auto status = env.get_cu_cp()
                            .get_command_handler()
                            .get_ntn_command_handler()
                            .get_current_ntn_runtime_status()
                            .onboard_position_plan;
    EXPECT_EQ(status.stage, "active");
    EXPECT_EQ(status.deployment_stage, "applied");
    EXPECT_EQ(status.active_schedule_version, plan.schedule_version);
    EXPECT_TRUE(status.signature_required);
    EXPECT_EQ(status.signature_status, "verified");
    EXPECT_EQ(status.signing_key_id, signing_key_id);
    EXPECT_NE(status.signing_key_fingerprint, "none");
    EXPECT_EQ(status.state_schema_version, 4U);
    EXPECT_EQ(status.version_anchor_mode, "software_only");
    EXPECT_EQ(status.version_anchor_status, "committed");
    EXPECT_EQ(status.version_anchor_schedule_version, plan.schedule_version);
    EXPECT_FALSE(status.state_write_blocked);
  }

  auto persisted_state = load_ntn_onboard_position_plan_state(source.state_file);
  ASSERT_TRUE(persisted_state.has_value()) << persisted_state.error();
  ASSERT_TRUE(persisted_state->has_value());
  ASSERT_TRUE((*persisted_state)->version_anchor_source.has_value());
  EXPECT_EQ((*persisted_state)->version_anchor_source->authentication->key_id, signing_key_id);
  auto persisted_anchor = load_ntn_plan_version_anchor(source.version_anchor_file);
  ASSERT_TRUE(persisted_anchor.has_value()) << persisted_anchor.error();
  ASSERT_TRUE(persisted_anchor->has_value());
  ASSERT_TRUE((*persisted_anchor)->committed.has_value());
  EXPECT_EQ((*persisted_anchor)->committed->schedule_version, plan.schedule_version);

  cu_cp_test_env_params restarted_params;
  restarted_params.ntn_onboard_position_plan = source;
  cu_cp_test_environment restarted(std::move(restarted_params));
  restarted.run_ng_setup();
  const auto restarted_status = restarted.get_cu_cp()
                                    .get_command_handler()
                                    .get_ntn_command_handler()
                                    .get_current_ntn_runtime_status()
                                    .onboard_position_plan;
  EXPECT_EQ(restarted_status.recovery_stage, "reconciling");
  EXPECT_EQ(restarted_status.signature_status, "verified");
  EXPECT_EQ(restarted_status.version_anchor_status, "committed");
  EXPECT_FALSE(restarted_status.state_write_blocked);
}

TEST(cu_cp_ntn_mobility_test, schema_v4_restart_rejects_a_state_file_rolled_back_below_the_version_anchor)
{
  const nr_cell_identity first_nci  = make_default_env_nci(0);
  const nr_cell_identity second_nci = make_default_env_nci(1);
  constexpr pci_t        shared_pci = 101;
  constexpr const char*  signing_key_id = "ntn-runtime-replay-key-v1";
  const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
  const int64_t activation_ms = ((now_ms + 60000 + 639) / 640) * 640;
  const auto activation_epoch =
      std::chrono::system_clock::time_point{std::chrono::milliseconds{activation_ms}};
  ntn_versioned_position_plan older = make_schema_v3_runtime_plan(8,
                                                                   4,
                                                                   90,
                                                                   100,
                                                                   first_nci,
                                                                   second_nci,
                                                                   shared_pci,
                                                                   activation_epoch,
                                                                   activation_epoch + std::chrono::seconds{120});
  ntn_versioned_position_plan newer = make_schema_v3_runtime_plan(10,
                                                                   5,
                                                                   91,
                                                                   101,
                                                                   first_nci,
                                                                   second_nci,
                                                                   shared_pci,
                                                                   activation_epoch + std::chrono::milliseconds{640},
                                                                   activation_epoch + std::chrono::seconds{120});
  const auto unique_suffix = std::chrono::steady_clock::now().time_since_epoch().count();
  const std::filesystem::path public_key_path =
      std::filesystem::temp_directory_path() / fmt::format("srsran-ntn-replay-key-{}.pem", unique_suffix);
  temporary_plan_file_guard public_key_guard(public_key_path);
  auto signed_plans = sign_schema_v4_runtime_plans(
      {std::move(older), std::move(newer)}, public_key_path, signing_key_id);
  older = std::move(signed_plans[0]);
  newer = std::move(signed_plans[1]);
  const std::filesystem::path plan_path = write_onboard_position_plan_for_runtime_test(older);
  temporary_plan_file_guard   plan_file_guard(plan_path);
  const std::filesystem::path newer_path = write_onboard_position_plan_for_runtime_test(newer);
  temporary_plan_file_guard   newer_file_guard(newer_path);

  auto source                 = make_schema_v3_runtime_source(older, plan_path);
  source.require_signed_plan  = true;
  source.version_anchor_file  = plan_path.string() + ".anchor";
  source.trusted_signing_keys = {{signing_key_id, public_key_path.string()}};
  source.reload_period        = std::chrono::milliseconds{50};
  std::string older_state_bytes;
  {
    cu_cp_test_env_params params;
    params.ntn_onboard_position_plan = source;
    cu_cp_test_environment env(std::move(params));
    env.run_ng_setup();
    ASSERT_TRUE(wait_for_test_condition([&env, &older]() {
      return env.get_cu_cp()
                 .get_command_handler()
                 .get_ntn_command_handler()
                 .get_current_ntn_runtime_status()
                 .onboard_position_plan.pending_schedule_version == older.schedule_version;
    }));
    older_state_bytes = read_runtime_test_file(source.state_file);
    ASSERT_FALSE(older_state_bytes.empty());
    write_runtime_test_file(plan_path, read_runtime_test_file(newer_path));
    ASSERT_TRUE(env.tick_until(std::chrono::milliseconds{1000}, [&env, &newer]() {
      return env.get_cu_cp()
                 .get_command_handler()
                 .get_ntn_command_handler()
                 .get_current_ntn_runtime_status()
                 .onboard_position_plan.pending_schedule_version == newer.schedule_version;
    }));
  }

  write_runtime_test_file(source.state_file, older_state_bytes);
  cu_cp_test_env_params restarted_params;
  restarted_params.ntn_onboard_position_plan = source;
  cu_cp_test_environment restarted(std::move(restarted_params));
  restarted.run_ng_setup();
  const auto status = restarted.get_cu_cp()
                          .get_command_handler()
                          .get_ntn_command_handler()
                          .get_current_ntn_runtime_status()
                          .onboard_position_plan;
  EXPECT_TRUE(status.state_write_blocked);
  EXPECT_EQ(status.last_rejection, "version_replay");
  EXPECT_EQ(status.version_anchor_error, "version_replay");
  EXPECT_EQ(status.active_schedule_version, 0U);
  EXPECT_EQ(status.pending_schedule_version, 0U);
}

TEST(cu_cp_ntn_mobility_test, restart_preserves_rejected_257_position_inventory_as_observation_only)
{
  const nr_cell_identity first_nci  = make_default_env_nci(0);
  const nr_cell_identity second_nci = make_default_env_nci(1);
  constexpr pci_t        shared_pci = 101;
  const int64_t          now_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count();
  const int64_t activation_ms = ((now_ms + 639) / 640) * 640;

  ntn_versioned_position_plan plan;
  plan.satellite_id         = "P01-S01";
  plan.catalog_version      = 29;
  plan.schedule_version     = 39;
  plan.valid_from           = std::chrono::system_clock::time_point{std::chrono::milliseconds{now_ms - 640}};
  plan.activation_epoch     = std::chrono::system_clock::time_point{std::chrono::milliseconds{activation_ms}};
  plan.valid_until          = std::chrono::system_clock::time_point{std::chrono::milliseconds{now_ms + 30000}};
  plan.onboard_cells[0]     = {first_nci, shared_pci};
  plan.onboard_cells[1]     = {second_nci, shared_pci};
  plan.visible_l1_positions.reserve(257);
  for (unsigned i = 0; i != 257; ++i) {
    plan.visible_l1_positions.push_back(
        {fmt::format("G{:06}", i + 1), 10.0 + static_cast<double>(i / 32) * 0.01, 20.0});
  }
  bind_onboard_planning_context(plan);
  plan.content_hash                     = compute_ntn_position_plan_content_hash(plan);
  const std::filesystem::path plan_path = write_onboard_position_plan_for_runtime_test(plan);
  temporary_plan_file_guard   plan_file_guard(plan_path);

  ntn_onboard_position_plan_source_config source;
  source.enabled              = true;
  source.du_execution_enabled = true;
  source.satellite_id         = plan.satellite_id;
  source.plan_json_file       = plan_path.string();
  source.cell_ncis            = {first_nci, second_nci};
  source.cell_pcis            = {shared_pci, shared_pci};
  bind_onboard_planning_context(source);

  const auto verify_inventory = [&plan](cu_cp_test_environment& env) {
    ASSERT_TRUE(wait_for_test_condition([&env]() {
      const auto status = env.get_cu_cp()
                              .get_command_handler()
                              .get_ntn_command_handler()
                              .get_current_ntn_runtime_status()
                              .onboard_position_plan;
      return status.candidate_l1_positions == 257U && status.state_generation > 0;
    }));
    const auto status = env.get_cu_cp()
                            .get_command_handler()
                            .get_ntn_command_handler()
                            .get_current_ntn_runtime_status()
                            .onboard_position_plan;
    EXPECT_EQ(status.last_rejection, "schedule_overflow");
    EXPECT_EQ(status.received_schedule_version, plan.schedule_version);
    EXPECT_EQ(status.candidate_l1_positions, 257U);
    EXPECT_EQ(status.assigned_l1_positions, 257U);
    EXPECT_EQ(status.active_schedule_version, 0U);
    EXPECT_EQ(status.pending_schedule_version, 0U);
    EXPECT_EQ(status.state_schema_version, 3U);
    EXPECT_EQ(status.state_store_status, "stored");
    EXPECT_FALSE(status.state_write_blocked);
  };

  {
    cu_cp_test_env_params params;
    params.ntn_onboard_position_plan = source;
    cu_cp_test_environment env(std::move(params));
    env.run_ng_setup();
    verify_inventory(env);

    auto stored = load_ntn_onboard_position_plan_state(source.state_file);
    ASSERT_TRUE(stored.has_value()) << stored.error();
    ASSERT_TRUE(stored->has_value());
    ASSERT_TRUE((*stored)->received_plan.has_value());
    EXPECT_EQ((*stored)->received_plan->candidate_inventory.size(), 257U);
    EXPECT_EQ((*stored)->received_plan->assigned_l1_position_ids.size(), 257U);
    EXPECT_FALSE((*stored)->active.has_value());
    EXPECT_FALSE((*stored)->pending.has_value());
  }

  cu_cp_test_env_params restarted_params;
  restarted_params.ntn_onboard_position_plan = source;
  cu_cp_test_environment restarted(std::move(restarted_params));
  restarted.run_ng_setup();
  verify_inventory(restarted);
}

TEST(cu_cp_ntn_mobility_test,
     when_schema_v3_has_300_visible_and_87_assigned_then_only_assigned_calendar_is_applied_and_recovered)
{
  const nr_cell_identity first_nci  = make_default_env_nci(0);
  const nr_cell_identity second_nci = make_default_env_nci(1);
  constexpr pci_t        shared_pci = 101;
  const int64_t          now_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count();
  const int64_t activation_ms = ((now_ms + 2500 + 639) / 640) * 640;
  const auto    activation_epoch = std::chrono::system_clock::time_point{std::chrono::milliseconds{activation_ms}};
  ntn_versioned_position_plan plan = make_schema_v3_runtime_plan(300,
                                                                 87,
                                                                 30,
                                                                 40,
                                                                 first_nci,
                                                                 second_nci,
                                                                 shared_pci,
                                                                 activation_epoch,
                                                                 activation_epoch + std::chrono::seconds{30});
  const std::filesystem::path plan_path = write_onboard_position_plan_for_runtime_test(plan);
  temporary_plan_file_guard   plan_file_guard(plan_path);
  const auto source = make_schema_v3_runtime_source(plan, plan_path);

  std::array<uint16_t, 2>                                  recovered_intents{};
  std::array<f1ap_ntn_access_calendar_preflight_report, 2> recovered_preflight{};
  {
    cu_cp_test_env_params params;
    params.ntn_onboard_position_plan                 = source;
    params.ntn_calendar_prepare_reports_ready        = true;
    params.ntn_calendar_query_reports_applied_early  = true;
    cu_cp_test_environment env(std::move(params));
    env.run_ng_setup();
    const auto du_idx = env.connect_new_du();
    ASSERT_TRUE(du_idx.has_value());
    ASSERT_TRUE(env.run_f1_setup(
        du_idx.value(), int_to_gnb_du_id(0x31), make_onboard_served_cells(first_nci, second_nci, shared_pci)));

    f1ap_message prepare_request;
    ASSERT_TRUE(env.wait_for_f1ap_tx_pdu_without_auto_response(
        du_idx.value(), prepare_request, std::chrono::milliseconds{1500}));
    const auto prepare = decode_ntn_calendar_update(prepare_request);
    ASSERT_TRUE(prepare.has_value());
    ASSERT_EQ(prepare->operation, f1ap_ntn_access_calendar_operation::prepare);
    ASSERT_EQ(prepare->cells.size(), 2U);
    EXPECT_EQ(prepare->schedule_version, plan.schedule_version);
    EXPECT_EQ(prepare->source_content_hash, plan.content_hash);
    EXPECT_EQ(prepare->cells[0].nci, first_nci);
    EXPECT_EQ(prepare->cells[0].pci, shared_pci);
    EXPECT_EQ(prepare->cells[1].nci, second_nci);
    EXPECT_EQ(prepare->cells[1].pci, shared_pci);

    const std::set<std::string> assigned(plan.assigned_l1_position_ids.begin(), plan.assigned_l1_position_ids.end());
    std::set<std::string>       scheduled_positions;
    unsigned                    nof_intents = 0;
    for (const f1ap_ntn_access_calendar_cell& cell : prepare->cells) {
      nof_intents += cell.intents.size();
      for (const f1ap_ntn_access_calendar_intent& intent : cell.intents) {
        EXPECT_NE(assigned.find(intent.position_id), assigned.end());
        scheduled_positions.insert(intent.position_id);
      }
    }
    EXPECT_EQ(nof_intents, 870U);
    EXPECT_EQ(scheduled_positions, assigned);
    make_recovered_calendar_feedback(*prepare, recovered_intents, recovered_preflight);
    EXPECT_EQ(recovered_intents[0] + recovered_intents[1], 870U);
    env.respond_to_f1ap_resource_coordination_request(du_idx.value(), prepare_request);

    f1ap_message query_request;
    ASSERT_TRUE(env.wait_for_f1ap_tx_pdu_without_auto_response(
        du_idx.value(), query_request, std::chrono::milliseconds{4000}));
    const auto query = decode_ntn_calendar_update(query_request);
    ASSERT_TRUE(query.has_value());
    EXPECT_EQ(query->operation, f1ap_ntn_access_calendar_operation::query);
    EXPECT_EQ(query->schedule_version, plan.schedule_version);
    EXPECT_EQ(query->calendar_hash, prepare->calendar_hash);
    EXPECT_TRUE(query->cells.empty());
    env.respond_to_f1ap_resource_coordination_request(du_idx.value(), query_request);

    ASSERT_TRUE(env.tick_until(std::chrono::milliseconds{4000}, [&]() {
      return env.get_cu_cp()
                 .get_command_handler()
                 .get_ntn_command_handler()
                 .get_current_ntn_runtime_status()
                 .onboard_position_plan.active_schedule_version == plan.schedule_version;
    }));
    const auto status = env.get_cu_cp()
                            .get_command_handler()
                            .get_ntn_command_handler()
                            .get_current_ntn_runtime_status()
                            .onboard_position_plan;
    EXPECT_EQ(status.stage, "active");
    EXPECT_EQ(status.deployment_stage, "applied");
    EXPECT_EQ(status.schema_version, 3U);
    EXPECT_EQ(status.candidate_l1_positions, 300U);
    EXPECT_EQ(status.assigned_l1_positions, 87U);
    EXPECT_EQ(status.cells[0].nci, first_nci);
    EXPECT_EQ(status.cells[0].pci, shared_pci);
    EXPECT_EQ(status.cells[1].nci, second_nci);
    EXPECT_EQ(status.cells[1].pci, shared_pci);
    EXPECT_EQ(status.cells[0].active_l1_positions + status.cells[1].active_l1_positions, 87U);
    EXPECT_EQ(status.calendar_intents, 870U);
    EXPECT_EQ(status.ssb_intents, 87U * 8U);
    EXPECT_EQ(status.prach_ro_intents, 87U);
    EXPECT_EQ(status.prach_ul_beam_intents, 87U);
  }

  cu_cp_test_env_params restarted_params;
  restarted_params.ntn_onboard_position_plan                 = source;
  restarted_params.ntn_calendar_query_reports_applied_early  = true;
  restarted_params.ntn_recovered_calendar_intents_per_cell   = recovered_intents;
  restarted_params.ntn_recovered_calendar_preflight_reports  = recovered_preflight;
  cu_cp_test_environment restarted(std::move(restarted_params));
  restarted.run_ng_setup();
  const auto hidden_status = restarted.get_cu_cp()
                                 .get_command_handler()
                                 .get_ntn_command_handler()
                                 .get_current_ntn_runtime_status()
                                 .onboard_position_plan;
  EXPECT_EQ(hidden_status.active_schedule_version, 0U);
  EXPECT_EQ(hidden_status.recovery_schedule_version, plan.schedule_version);
  EXPECT_EQ(hidden_status.candidate_l1_positions, 300U);
  EXPECT_EQ(hidden_status.assigned_l1_positions, 87U);

  const auto restarted_du = restarted.connect_new_du();
  ASSERT_TRUE(restarted_du.has_value());
  ASSERT_TRUE(restarted.run_f1_setup(restarted_du.value(),
                                    int_to_gnb_du_id(0x31),
                                    make_onboard_served_cells(first_nci, second_nci, shared_pci)));
  f1ap_message recovery_query;
  ASSERT_TRUE(restarted.wait_for_f1ap_tx_pdu_without_auto_response(
      restarted_du.value(), recovery_query, std::chrono::milliseconds{1500}));
  const auto query = decode_ntn_calendar_update(recovery_query);
  ASSERT_TRUE(query.has_value());
  EXPECT_EQ(query->operation, f1ap_ntn_access_calendar_operation::query);
  EXPECT_EQ(query->schedule_version, plan.schedule_version);
  EXPECT_TRUE(query->cells.empty());
  restarted.respond_to_f1ap_resource_coordination_request(restarted_du.value(), recovery_query);

  ASSERT_TRUE(restarted.tick_until(std::chrono::milliseconds{1500}, [&]() {
    return restarted.get_cu_cp()
               .get_command_handler()
               .get_ntn_command_handler()
               .get_current_ntn_runtime_status()
               .onboard_position_plan.recovery_stage == "reconciled";
  }));
  const auto recovered_status = restarted.get_cu_cp()
                                    .get_command_handler()
                                    .get_ntn_command_handler()
                                    .get_current_ntn_runtime_status()
                                    .onboard_position_plan;
  EXPECT_EQ(recovered_status.stage, "active");
  EXPECT_EQ(recovered_status.active_schedule_version, plan.schedule_version);
  EXPECT_EQ(recovered_status.candidate_l1_positions, 300U);
  EXPECT_EQ(recovered_status.assigned_l1_positions, 87U);
  EXPECT_EQ(recovered_status.cells[0].nci, first_nci);
  EXPECT_EQ(recovered_status.cells[0].pci, shared_pci);
  EXPECT_EQ(recovered_status.cells[1].nci, second_nci);
  EXPECT_EQ(recovered_status.cells[1].pci, shared_pci);
  EXPECT_EQ(recovered_status.cells[0].active_l1_positions + recovered_status.cells[1].active_l1_positions, 87U);
  EXPECT_EQ(recovered_status.calendar_intents, 870U);
}

TEST(cu_cp_ntn_mobility_test, when_schema_v3_has_257_assigned_then_old_active_calendar_remains)
{
  const nr_cell_identity first_nci  = make_default_env_nci(0);
  const nr_cell_identity second_nci = make_default_env_nci(1);
  constexpr pci_t        shared_pci = 101;
  const int64_t          now_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count();
  const auto active_epoch =
      std::chrono::system_clock::time_point{std::chrono::milliseconds{(now_ms / 640) * 640}};
  ntn_versioned_position_plan active_plan = make_schema_v3_runtime_plan(8,
                                                                        4,
                                                                        41,
                                                                        51,
                                                                        first_nci,
                                                                        second_nci,
                                                                        shared_pci,
                                                                        active_epoch,
                                                                        active_epoch + std::chrono::seconds{30});
  ntn_versioned_position_plan overflow_plan = make_schema_v3_runtime_plan(300,
                                                                          257,
                                                                          42,
                                                                          52,
                                                                          first_nci,
                                                                          second_nci,
                                                                          shared_pci,
                                                                          active_epoch,
                                                                          active_epoch + std::chrono::seconds{30});
  const std::filesystem::path plan_path = write_onboard_position_plan_for_runtime_test(overflow_plan);
  temporary_plan_file_guard   plan_file_guard(plan_path);
  auto                        source = make_schema_v3_runtime_source(overflow_plan, plan_path);
  source.reload_period               = std::chrono::milliseconds{50};

  ntn_onboard_position_plan_config controller_cfg;
  controller_cfg.enabled                            = true;
  controller_cfg.require_external_apply             = true;
  controller_cfg.satellite_id                       = source.satellite_id;
  controller_cfg.expected_catalog_id                = source.expected_catalog_id;
  controller_cfg.expected_catalog_hash              = source.expected_catalog_hash;
  controller_cfg.expected_identity_registry_version = source.expected_identity_registry_version;
  controller_cfg.expected_identity_registry_hash    = source.expected_identity_registry_hash;
  controller_cfg.expected_access_profile_id         = source.expected_access_profile_id;
  controller_cfg.expected_access_profile_hash       = source.expected_access_profile_hash;
  controller_cfg.onboard_cells                      = {{{first_nci, shared_pci}, {second_nci, shared_pci}}};
  ntn_onboard_position_plan_controller persisted_controller(controller_cfg);
  const auto now = std::chrono::system_clock::time_point{std::chrono::milliseconds{now_ms}};
  ASSERT_TRUE(persisted_controller.submit(active_plan, now).accepted);
  ASSERT_TRUE(persisted_controller.pending_plan().has_value());
  const std::string active_calendar_hash = persisted_controller.pending_plan()->calendar_hash;
  ASSERT_TRUE(persisted_controller.mark_deployment_preparing(active_plan.schedule_version, active_calendar_hash));
  ASSERT_TRUE(persisted_controller.mark_deployment_applied(active_plan.schedule_version, active_calendar_hash));
  ASSERT_TRUE(persisted_controller.advance_time(now));
  ASSERT_TRUE(persisted_controller.active_plan().has_value());
  ASSERT_TRUE(store_ntn_onboard_position_plan_state_atomic(source.state_file,
                                                           persisted_controller.make_persistent_state(9))
                  .has_value());

  std::array<uint16_t, 2>                                  recovered_intents{};
  std::array<f1ap_ntn_access_calendar_preflight_report, 2> recovered_preflight{};
  for (const ntn_access_calendar_intent& intent : persisted_controller.active_plan()->access_calendar) {
    const unsigned cell_index = intent.nci == first_nci ? 0U : 1U;
    ++recovered_intents[cell_index];
    auto& report      = recovered_preflight[cell_index];
    report.performed  = true;
    report.passed     = true;
    report.numerology = 1;
    if (intent.purpose == ntn_access_calendar_purpose::ssb_sib_paging ||
        intent.purpose == ntn_access_calendar_purpose::ssb_sib_paging_rar) {
      ++report.expected_ssb;
      ++report.matched_ssb;
      report.max_ssb_gap_slots = 160;
    } else if (intent.purpose == ntn_access_calendar_purpose::prach_ro) {
      ++report.expected_prach;
      ++report.matched_prach;
      report.max_prach_gap_slots = 1280;
    }
  }

  cu_cp_test_env_params params;
  params.ntn_onboard_position_plan                 = source;
  params.ntn_calendar_query_reports_applied_early  = true;
  params.ntn_recovered_calendar_intents_per_cell   = recovered_intents;
  params.ntn_recovered_calendar_preflight_reports  = recovered_preflight;
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();
  const auto du_idx = env.connect_new_du();
  ASSERT_TRUE(du_idx.has_value());
  ASSERT_TRUE(env.run_f1_setup(
      du_idx.value(), int_to_gnb_du_id(0x32), make_onboard_served_cells(first_nci, second_nci, shared_pci)));
  f1ap_message recovery_query;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu_without_auto_response(
      du_idx.value(), recovery_query, std::chrono::milliseconds{1500}));
  const auto query = decode_ntn_calendar_update(recovery_query);
  ASSERT_TRUE(query.has_value());
  EXPECT_EQ(query->operation, f1ap_ntn_access_calendar_operation::query);
  EXPECT_EQ(query->schedule_version, active_plan.schedule_version);
  EXPECT_EQ(query->calendar_hash, active_calendar_hash);
  env.respond_to_f1ap_resource_coordination_request(du_idx.value(), recovery_query);

  ASSERT_TRUE(env.tick_until(std::chrono::milliseconds{2000}, [&]() {
    const auto status = env.get_cu_cp()
                            .get_command_handler()
                            .get_ntn_command_handler()
                            .get_current_ntn_runtime_status()
                            .onboard_position_plan;
    return status.recovery_stage == "reconciled" && status.last_rejection == "schedule_overflow" &&
           status.candidate_l1_positions == 300U;
  }));
  const auto status = env.get_cu_cp()
                          .get_command_handler()
                          .get_ntn_command_handler()
                          .get_current_ntn_runtime_status()
                          .onboard_position_plan;
  EXPECT_EQ(status.active_schedule_version, active_plan.schedule_version);
  EXPECT_EQ(status.active_calendar_hash, active_calendar_hash);
  EXPECT_EQ(status.pending_schedule_version, 0U);
  EXPECT_EQ(status.received_schedule_version, overflow_plan.schedule_version);
  EXPECT_EQ(status.candidate_l1_positions, 300U);
  EXPECT_EQ(status.assigned_l1_positions, 257U);
  EXPECT_EQ(status.cells[0].active_l1_positions + status.cells[1].active_l1_positions, 4U);
}

TEST(cu_cp_ntn_mobility_test, when_schema_v3_assignment_is_empty_then_two_cell_deny_all_calendar_is_applied)
{
  const nr_cell_identity first_nci  = make_default_env_nci(0);
  const nr_cell_identity second_nci = make_default_env_nci(1);
  constexpr pci_t        shared_pci = 101;
  const int64_t          now_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count();
  const int64_t activation_ms = ((now_ms + 1500 + 639) / 640) * 640;
  const auto    activation_epoch = std::chrono::system_clock::time_point{std::chrono::milliseconds{activation_ms}};
  ntn_versioned_position_plan plan = make_schema_v3_runtime_plan(12,
                                                                 0,
                                                                 43,
                                                                 53,
                                                                 first_nci,
                                                                 second_nci,
                                                                 shared_pci,
                                                                 activation_epoch,
                                                                 activation_epoch + std::chrono::seconds{30});
  const std::filesystem::path plan_path = write_onboard_position_plan_for_runtime_test(plan);
  temporary_plan_file_guard   plan_file_guard(plan_path);
  const auto                  source = make_schema_v3_runtime_source(plan, plan_path);

  cu_cp_test_env_params params;
  params.ntn_onboard_position_plan                = source;
  params.ntn_calendar_prepare_reports_ready       = true;
  params.ntn_calendar_query_reports_applied_early = true;
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();
  const auto du_idx = env.connect_new_du();
  ASSERT_TRUE(du_idx.has_value());
  ASSERT_TRUE(env.run_f1_setup(
      du_idx.value(), int_to_gnb_du_id(0x33), make_onboard_served_cells(first_nci, second_nci, shared_pci)));

  f1ap_message prepare_request;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu_without_auto_response(
      du_idx.value(), prepare_request, std::chrono::milliseconds{1200}));
  const auto prepare = decode_ntn_calendar_update(prepare_request);
  ASSERT_TRUE(prepare.has_value());
  ASSERT_EQ(prepare->operation, f1ap_ntn_access_calendar_operation::prepare);
  ASSERT_EQ(prepare->cells.size(), 2U);
  EXPECT_EQ(prepare->cells[0].nci, first_nci);
  EXPECT_EQ(prepare->cells[0].pci, shared_pci);
  EXPECT_TRUE(prepare->cells[0].intents.empty());
  EXPECT_EQ(prepare->cells[1].nci, second_nci);
  EXPECT_EQ(prepare->cells[1].pci, shared_pci);
  EXPECT_TRUE(prepare->cells[1].intents.empty());
  env.respond_to_f1ap_resource_coordination_request(du_idx.value(), prepare_request);

  f1ap_message query_request;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu_without_auto_response(
      du_idx.value(), query_request, std::chrono::milliseconds{3000}));
  const auto query = decode_ntn_calendar_update(query_request);
  ASSERT_TRUE(query.has_value());
  EXPECT_EQ(query->operation, f1ap_ntn_access_calendar_operation::query);
  EXPECT_EQ(query->schedule_version, plan.schedule_version);
  env.respond_to_f1ap_resource_coordination_request(du_idx.value(), query_request);

  ASSERT_TRUE(env.tick_until(std::chrono::milliseconds{3000}, [&]() {
    return env.get_cu_cp()
               .get_command_handler()
               .get_ntn_command_handler()
               .get_current_ntn_runtime_status()
               .onboard_position_plan.active_schedule_version == plan.schedule_version;
  }));
  const auto status = env.get_cu_cp()
                          .get_command_handler()
                          .get_ntn_command_handler()
                          .get_current_ntn_runtime_status()
                          .onboard_position_plan;
  EXPECT_EQ(status.stage, "active");
  EXPECT_EQ(status.deployment_stage, "applied");
  EXPECT_EQ(status.candidate_l1_positions, 12U);
  EXPECT_EQ(status.assigned_l1_positions, 0U);
  EXPECT_EQ(status.calendar_intents, 0U);
  EXPECT_EQ(status.cells[0].active_l1_positions, 0U);
  EXPECT_EQ(status.cells[1].active_l1_positions, 0U);
}

TEST(cu_cp_ntn_mobility_test, when_position_plan_feature_is_disabled_then_configured_file_is_not_read)
{
  const auto unique_suffix = std::chrono::steady_clock::now().time_since_epoch().count();
  const std::filesystem::path missing_plan_path =
      std::filesystem::temp_directory_path() / fmt::format("srsran-disabled-ntn-plan-{}.json", unique_suffix);
  std::error_code ignored;
  std::filesystem::remove(missing_plan_path, ignored);
  std::filesystem::remove(missing_plan_path.string() + ".state", ignored);

  ntn_onboard_position_plan_source_config source;
  source.enabled              = false;
  source.du_execution_enabled = false;
  source.satellite_id         = "P01-S01";
  source.plan_json_file       = missing_plan_path.string();
  source.state_file           = missing_plan_path.string() + ".state";
  source.cell_ncis            = {make_default_env_nci(0), make_default_env_nci(1)};
  source.cell_pcis            = {101, 101};
  bind_onboard_planning_context(source);

  cu_cp_test_env_params params;
  params.ntn_onboard_position_plan = source;
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const auto status = env.get_cu_cp()
                          .get_command_handler()
                          .get_ntn_command_handler()
                          .get_current_ntn_runtime_status()
                          .onboard_position_plan;
  EXPECT_FALSE(status.enabled);
  EXPECT_EQ(status.stage, "disabled");
  EXPECT_EQ(status.last_rejection, "none");
  EXPECT_FALSE(status.received_plan_present);
  EXPECT_EQ(status.candidate_l1_positions, 0U);
  EXPECT_EQ(status.active_schedule_version, 0U);
  EXPECT_EQ(status.pending_schedule_version, 0U);
  EXPECT_FALSE(std::filesystem::exists(missing_plan_path));
  EXPECT_FALSE(std::filesystem::exists(source.state_file));
}

TEST(cu_cp_ntn_mobility_test, when_plan_reload_exceeds_input_limit_then_last_successful_state_is_unchanged)
{
  const nr_cell_identity first_nci  = make_default_env_nci(0);
  const nr_cell_identity second_nci = make_default_env_nci(1);
  constexpr pci_t        shared_pci = 101;
  const int64_t          now_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count();
  const auto activation_epoch =
      std::chrono::system_clock::time_point{std::chrono::milliseconds{(now_ms / 640) * 640}};
  const ntn_versioned_position_plan plan = make_schema_v3_runtime_plan(12,
                                                                       6,
                                                                       44,
                                                                       54,
                                                                       first_nci,
                                                                       second_nci,
                                                                       shared_pci,
                                                                       activation_epoch,
                                                                       activation_epoch + std::chrono::seconds{30});
  const std::filesystem::path plan_path = write_onboard_position_plan_for_runtime_test(plan);
  temporary_plan_file_guard   plan_file_guard(plan_path);
  auto                        source = make_schema_v3_runtime_source(plan, plan_path, false);
  source.reload_period               = std::chrono::milliseconds{50};

  cu_cp_test_env_params params;
  params.ntn_onboard_position_plan = source;
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();
  ASSERT_TRUE(wait_for_test_condition([&env, &plan]() {
    return env.get_cu_cp()
               .get_command_handler()
               .get_ntn_command_handler()
               .get_current_ntn_runtime_status()
               .onboard_position_plan.active_schedule_version == plan.schedule_version;
  }));
  const auto before = env.get_cu_cp()
                          .get_command_handler()
                          .get_ntn_command_handler()
                          .get_current_ntn_runtime_status()
                          .onboard_position_plan;

  {
    std::ofstream oversized(plan_path, std::ios::binary | std::ios::trunc);
    const std::string payload(max_ntn_position_plan_file_size + 1, 'x');
    oversized.write(payload.data(), static_cast<std::streamsize>(payload.size()));
  }
  ASSERT_TRUE(env.tick_until(std::chrono::milliseconds{1000}, [&env]() {
    return env.get_cu_cp()
               .get_command_handler()
               .get_ntn_command_handler()
               .get_current_ntn_runtime_status()
               .onboard_position_plan.last_rejection == "input_too_large";
  }));
  const auto after = env.get_cu_cp()
                         .get_command_handler()
                         .get_ntn_command_handler()
                         .get_current_ntn_runtime_status()
                         .onboard_position_plan;

  EXPECT_EQ(after.active_schedule_version, before.active_schedule_version);
  EXPECT_EQ(after.pending_schedule_version, before.pending_schedule_version);
  EXPECT_EQ(after.catalog_version_high_water, before.catalog_version_high_water);
  EXPECT_EQ(after.schedule_version_high_water, before.schedule_version_high_water);
  EXPECT_EQ(after.received_schedule_version, before.received_schedule_version);
  EXPECT_EQ(after.candidate_l1_positions, before.candidate_l1_positions);
  EXPECT_EQ(after.assigned_l1_positions, before.assigned_l1_positions);
  EXPECT_EQ(after.cells[0].active_l1_positions, before.cells[0].active_l1_positions);
  EXPECT_EQ(after.cells[1].active_l1_positions, before.cells[1].active_l1_positions);
}

TEST(cu_cp_ntn_mobility_test, restart_queries_du_before_reexposing_persisted_active_calendar)
{
  const nr_cell_identity first_nci  = make_default_env_nci(0);
  const nr_cell_identity second_nci = make_default_env_nci(1);
  constexpr pci_t        shared_pci = 101;
  const int64_t          now_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count();
  const int64_t activation_ms = (now_ms / 640) * 640;

  ntn_versioned_position_plan plan;
  plan.satellite_id         = "P01-S01";
  plan.catalog_version      = 30;
  plan.schedule_version     = 40;
  plan.valid_from           = std::chrono::system_clock::time_point{std::chrono::milliseconds{activation_ms - 640}};
  plan.activation_epoch     = std::chrono::system_clock::time_point{std::chrono::milliseconds{activation_ms}};
  plan.valid_until          = std::chrono::system_clock::time_point{std::chrono::milliseconds{now_ms + 6000}};
  plan.onboard_cells[0]     = {first_nci, shared_pci};
  plan.onboard_cells[1]     = {second_nci, shared_pci};
  plan.visible_l1_positions = {{"G000001", 10.0, 20.0}, {"G000002", 10.1, 20.1}};
  bind_onboard_planning_context(plan);
  plan.content_hash                     = compute_ntn_position_plan_content_hash(plan);
  const std::filesystem::path plan_path = write_onboard_position_plan_for_runtime_test(plan);
  temporary_plan_file_guard   plan_file_guard(plan_path);

  ntn_onboard_position_plan_source_config source;
  source.enabled              = true;
  source.du_execution_enabled = true;
  source.du_prepare_guard     = std::chrono::milliseconds{100};
  source.satellite_id         = plan.satellite_id;
  source.plan_json_file       = plan_path.string();
  source.cell_ncis            = {first_nci, second_nci};
  source.cell_pcis            = {shared_pci, shared_pci};
  bind_onboard_planning_context(source);

  ntn_onboard_position_plan_config controller_cfg;
  controller_cfg.enabled                            = true;
  controller_cfg.require_external_apply             = true;
  controller_cfg.satellite_id                       = source.satellite_id;
  controller_cfg.expected_catalog_id                = source.expected_catalog_id;
  controller_cfg.expected_catalog_hash              = source.expected_catalog_hash;
  controller_cfg.expected_identity_registry_version = source.expected_identity_registry_version;
  controller_cfg.expected_identity_registry_hash    = source.expected_identity_registry_hash;
  controller_cfg.expected_access_profile_id         = source.expected_access_profile_id;
  controller_cfg.expected_access_profile_hash       = source.expected_access_profile_hash;
  controller_cfg.onboard_cells                      = {{{first_nci, shared_pci}, {second_nci, shared_pci}}};
  ntn_onboard_position_plan_controller persisted_controller(controller_cfg);
  ASSERT_TRUE(
      persisted_controller.submit(plan, std::chrono::system_clock::time_point{std::chrono::milliseconds{now_ms}})
          .accepted);
  const std::string calendar_hash = persisted_controller.pending_plan()->calendar_hash;
  ASSERT_TRUE(persisted_controller.mark_deployment_preparing(plan.schedule_version, calendar_hash));
  ASSERT_TRUE(persisted_controller.mark_deployment_applied(plan.schedule_version, calendar_hash));
  ASSERT_TRUE(
      persisted_controller.advance_time(std::chrono::system_clock::time_point{std::chrono::milliseconds{now_ms}}));
  ntn_onboard_position_plan_persistent_state legacy_state = persisted_controller.make_persistent_state(9);
  legacy_state.schema_version                            = 1;
  legacy_state.received_plan.reset();
  ASSERT_TRUE(store_ntn_onboard_position_plan_state_atomic(source.state_file, legacy_state).has_value());

  cu_cp_test_env_params env_params;
  env_params.ntn_onboard_position_plan    = source;
  env_params.ntn_calendar_prepare_rejects = true;
  std::array<uint16_t, 2>                                  recovered_intents{};
  std::array<f1ap_ntn_access_calendar_preflight_report, 2> recovered_preflight{};
  ASSERT_TRUE(persisted_controller.active_plan().has_value());
  for (const ntn_access_calendar_intent& intent : persisted_controller.active_plan()->access_calendar) {
    const unsigned cell_index = intent.nci == first_nci ? 0U : 1U;
    ++recovered_intents[cell_index];
    auto& report      = recovered_preflight[cell_index];
    report.performed  = true;
    report.passed     = true;
    report.numerology = 1;
    if (intent.purpose == ntn_access_calendar_purpose::ssb_sib_paging ||
        intent.purpose == ntn_access_calendar_purpose::ssb_sib_paging_rar) {
      ++report.expected_ssb;
      ++report.matched_ssb;
      report.max_ssb_gap_slots = 160;
    } else if (intent.purpose == ntn_access_calendar_purpose::prach_ro) {
      ++report.expected_prach;
      ++report.matched_prach;
      report.max_prach_gap_slots = 1280;
    }
  }
  env_params.ntn_recovered_calendar_intents_per_cell  = recovered_intents;
  env_params.ntn_recovered_calendar_preflight_reports = recovered_preflight;

  cu_cp_test_environment env(std::move(env_params));
  env.run_ng_setup();
  const auto loaded_status = env.get_cu_cp()
                                 .get_command_handler()
                                 .get_ntn_command_handler()
                                 .get_current_ntn_runtime_status()
                                 .onboard_position_plan;
  EXPECT_EQ(loaded_status.state_schema_version, 1U);
  EXPECT_EQ(loaded_status.state_generation, 9U);
  EXPECT_EQ(loaded_status.state_store_status, "loaded_reconciliation_required");
  const auto du_idx = env.connect_new_du();
  ASSERT_TRUE(du_idx.has_value());
  std::vector<test_helpers::served_cell_item_info> served_cells(2);
  served_cells[0].nci = first_nci;
  served_cells[0].pci = shared_pci;
  served_cells[1].nci = second_nci;
  served_cells[1].pci = shared_pci;
  ASSERT_TRUE(env.run_f1_setup(du_idx.value(), int_to_gnb_du_id(0x11), served_cells));
  f1ap_message ignored;
  (void)env.wait_for_f1ap_tx_pdu(du_idx.value(), ignored, std::chrono::milliseconds{1600});

  const auto status = env.get_cu_cp()
                          .get_command_handler()
                          .get_ntn_command_handler()
                          .get_current_ntn_runtime_status()
                          .onboard_position_plan;
  EXPECT_EQ(status.recovery_stage, "reconciled");
  EXPECT_EQ(status.stage, "active");
  EXPECT_EQ(status.active_schedule_version, plan.schedule_version);
  EXPECT_EQ(status.active_calendar_hash, calendar_hash);
  EXPECT_EQ(status.deployment_stage, "applied");
  EXPECT_EQ(status.execution_evidence, "ssb_prach_software_gate_applied_no_position_or_rf_evidence");
  EXPECT_TRUE(status.state_file_configured);
  EXPECT_TRUE(status.state_file_required);
  EXPECT_EQ(status.state_schema_version, 3U);
  EXPECT_GT(status.state_generation, 9U);
  EXPECT_EQ(status.state_hash.rfind("sha256:", 0), 0U);
  EXPECT_EQ(status.state_store_status, "stored");
  EXPECT_EQ(status.state_store_error, "none");
  EXPECT_FALSE(status.state_write_blocked);
  EXPECT_GT(status.last_state_save_unix_ms, 0);
  EXPECT_EQ(status.recovery_schedule_version, plan.schedule_version);
  EXPECT_EQ(status.catalog_version_high_water, plan.catalog_version);
  EXPECT_EQ(status.schedule_version_high_water, plan.schedule_version);
}

TEST(cu_cp_ntn_mobility_test, du_disconnect_hides_applied_calendar_until_the_reconnected_du_confirms_it)
{
  persisted_recovery_calendar_test_data data = make_persisted_recovery_calendar_test_data(34, 44);
  temporary_plan_file_guard             plan_file_guard(data.plan_path);

  cu_cp_test_env_params env_params;
  env_params.ntn_onboard_position_plan                = data.source;
  env_params.ntn_recovered_calendar_intents_per_cell  = data.recovered_intents;
  env_params.ntn_recovered_calendar_preflight_reports = data.recovered_preflight;
  cu_cp_test_environment env(std::move(env_params));
  env.run_ng_setup();

  std::vector<test_helpers::served_cell_item_info> served_cells(2);
  served_cells[0].nci = data.plan.onboard_cells[0].nci;
  served_cells[0].pci = data.plan.onboard_cells[0].pci;
  served_cells[1].nci = data.plan.onboard_cells[1].nci;
  served_cells[1].pci = data.plan.onboard_cells[1].pci;

  const auto first_du = env.connect_new_du();
  ASSERT_TRUE(first_du.has_value());
  ASSERT_TRUE(env.run_f1_setup(first_du.value(), int_to_gnb_du_id(0x21), served_cells));
  f1ap_message ignored;
  (void)env.wait_for_f1ap_tx_pdu(first_du.value(), ignored, std::chrono::milliseconds{1600});
  ASSERT_TRUE(wait_for_test_condition([&env, &data]() {
    return env.get_cu_cp()
               .get_command_handler()
               .get_ntn_command_handler()
               .get_current_ntn_runtime_status()
               .onboard_position_plan.active_schedule_version == data.plan.schedule_version;
  }));

  ASSERT_TRUE(env.drop_du_connection(first_du.value()));
  const auto disconnected = env.get_cu_cp()
                                .get_command_handler()
                                .get_ntn_command_handler()
                                .get_current_ntn_runtime_status()
                                .onboard_position_plan;
  EXPECT_EQ(disconnected.stage, "pending");
  EXPECT_EQ(disconnected.recovery_stage, "reconciling");
  EXPECT_EQ(disconnected.recovery_schedule_version, data.plan.schedule_version);
  EXPECT_EQ(disconnected.active_schedule_version, 0U);
  EXPECT_EQ(disconnected.active_calendar_hash, "none");
  EXPECT_EQ(disconnected.execution_evidence, "intent_or_control_plane_only");

  const auto reconnected_du = env.connect_new_du();
  ASSERT_TRUE(reconnected_du.has_value());
  ASSERT_TRUE(env.run_f1_setup(reconnected_du.value(), int_to_gnb_du_id(0x21), served_cells));
  f1ap_message query_request;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu_without_auto_response(
      reconnected_du.value(), query_request, std::chrono::milliseconds{1500}));
  const auto query_update = decode_f1ap_ntn_access_calendar_update(
      query_request.pdu.init_msg().value.gnb_du_res_coordination_request()->eutra_nr_cell_res_coordination_req_container);
  ASSERT_TRUE(query_update.has_value());
  EXPECT_EQ(query_update->operation, f1ap_ntn_access_calendar_operation::query);
  EXPECT_EQ(query_update->schedule_version, data.plan.schedule_version);
  EXPECT_EQ(env.get_cu_cp()
                .get_command_handler()
                .get_ntn_command_handler()
                .get_current_ntn_runtime_status()
                .onboard_position_plan.active_schedule_version,
            0U);

  env.respond_to_f1ap_resource_coordination_request(reconnected_du.value(), query_request);
  ASSERT_TRUE(wait_for_test_condition([&env, &data]() {
    const auto status = env.get_cu_cp()
                            .get_command_handler()
                            .get_ntn_command_handler()
                            .get_current_ntn_runtime_status()
                            .onboard_position_plan;
    return status.active_schedule_version == data.plan.schedule_version && status.recovery_stage == "reconciled";
  }));
  const auto reconnected = env.get_cu_cp()
                               .get_command_handler()
                               .get_ntn_command_handler()
                               .get_current_ntn_runtime_status()
                               .onboard_position_plan;
  EXPECT_EQ(reconnected.active_calendar_hash, query_update->calendar_hash);
  EXPECT_EQ(reconnected.execution_evidence, "ssb_prach_software_gate_applied_no_position_or_rf_evidence");
}

TEST(cu_cp_ntn_mobility_test, disconnect_during_recovery_query_keeps_plan_hidden_and_retries_on_new_connection)
{
  persisted_recovery_calendar_test_data data = make_persisted_recovery_calendar_test_data(35, 45);
  temporary_plan_file_guard             plan_file_guard(data.plan_path);

  cu_cp_test_env_params env_params;
  env_params.ntn_onboard_position_plan                = data.source;
  env_params.ntn_recovered_calendar_intents_per_cell  = data.recovered_intents;
  env_params.ntn_recovered_calendar_preflight_reports = data.recovered_preflight;
  cu_cp_test_environment env(std::move(env_params));
  env.run_ng_setup();

  std::vector<test_helpers::served_cell_item_info> served_cells(2);
  served_cells[0].nci = data.plan.onboard_cells[0].nci;
  served_cells[0].pci = data.plan.onboard_cells[0].pci;
  served_cells[1].nci = data.plan.onboard_cells[1].nci;
  served_cells[1].pci = data.plan.onboard_cells[1].pci;

  const auto first_du = env.connect_new_du();
  ASSERT_TRUE(first_du.has_value());
  ASSERT_TRUE(env.run_f1_setup(first_du.value(), int_to_gnb_du_id(0x22), served_cells));
  f1ap_message first_query;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu_without_auto_response(
      first_du.value(), first_query, std::chrono::milliseconds{1500}));
  ASSERT_TRUE(env.drop_du_connection(first_du.value()));
  EXPECT_EQ(env.get_cu_cp()
                .get_command_handler()
                .get_ntn_command_handler()
                .get_current_ntn_runtime_status()
                .onboard_position_plan.active_schedule_version,
            0U);

  const auto reconnected_du = env.connect_new_du();
  ASSERT_TRUE(reconnected_du.has_value());
  ASSERT_TRUE(env.run_f1_setup(reconnected_du.value(), int_to_gnb_du_id(0x22), served_cells));
  f1ap_message second_query;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu_without_auto_response(
      reconnected_du.value(), second_query, std::chrono::milliseconds{1500}));
  const auto second_update = decode_f1ap_ntn_access_calendar_update(
      second_query.pdu.init_msg().value.gnb_du_res_coordination_request()->eutra_nr_cell_res_coordination_req_container);
  ASSERT_TRUE(second_update.has_value());
  EXPECT_EQ(second_update->operation, f1ap_ntn_access_calendar_operation::query);
  EXPECT_EQ(second_update->schedule_version, data.plan.schedule_version);
  EXPECT_EQ(env.get_cu_cp()
                .get_command_handler()
                .get_ntn_command_handler()
                .get_current_ntn_runtime_status()
                .onboard_position_plan.execution_evidence,
            "intent_or_control_plane_only");

  env.respond_to_f1ap_resource_coordination_request(reconnected_du.value(), second_query);
  ASSERT_TRUE(wait_for_test_condition([&env, &data]() {
    return env.get_cu_cp()
               .get_command_handler()
               .get_ntn_command_handler()
               .get_current_ntn_runtime_status()
               .onboard_position_plan.active_schedule_version == data.plan.schedule_version;
  }));
}

TEST(cu_cp_ntn_mobility_test, future_recovered_update_does_not_clear_active_calendar_before_activation_epoch)
{
  const nr_cell_identity first_nci  = make_default_env_nci(0);
  const nr_cell_identity second_nci = make_default_env_nci(1);
  constexpr pci_t        shared_pci = 101;
  const int64_t          now_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count();
  const int64_t active_epoch_ms  = (now_ms / 640) * 640;
  const int64_t pending_epoch_ms = active_epoch_ms + 3200;

  ntn_versioned_position_plan active_plan;
  active_plan.satellite_id     = "P01-S01";
  active_plan.catalog_version  = 36;
  active_plan.schedule_version = 46;
  active_plan.valid_from = std::chrono::system_clock::time_point{std::chrono::milliseconds{active_epoch_ms - 640}};
  active_plan.activation_epoch =
      std::chrono::system_clock::time_point{std::chrono::milliseconds{active_epoch_ms}};
  active_plan.valid_until = std::chrono::system_clock::time_point{std::chrono::milliseconds{now_ms + 30000}};
  active_plan.onboard_cells[0] = {first_nci, shared_pci};
  active_plan.onboard_cells[1] = {second_nci, shared_pci};
  active_plan.visible_l1_positions = {{"G000001", 10.0, 20.0}, {"G000002", 10.1, 20.1}};
  bind_onboard_planning_context(active_plan);
  active_plan.content_hash = compute_ntn_position_plan_content_hash(active_plan);

  ntn_versioned_position_plan pending_plan = active_plan;
  pending_plan.catalog_version              = 37;
  pending_plan.schedule_version             = 47;
  pending_plan.activation_epoch =
      std::chrono::system_clock::time_point{std::chrono::milliseconds{pending_epoch_ms}};
  pending_plan.content_hash = compute_ntn_position_plan_content_hash(pending_plan);
  const std::filesystem::path plan_path = write_onboard_position_plan_for_runtime_test(pending_plan);
  temporary_plan_file_guard   plan_file_guard(plan_path);

  ntn_onboard_position_plan_source_config source;
  source.enabled              = true;
  source.du_execution_enabled = true;
  source.du_prepare_guard     = std::chrono::milliseconds{100};
  source.satellite_id         = pending_plan.satellite_id;
  source.plan_json_file       = plan_path.string();
  source.cell_ncis            = {first_nci, second_nci};
  source.cell_pcis            = {shared_pci, shared_pci};
  bind_onboard_planning_context(source);

  ntn_onboard_position_plan_config controller_cfg;
  controller_cfg.enabled                            = true;
  controller_cfg.require_external_apply             = true;
  controller_cfg.satellite_id                       = source.satellite_id;
  controller_cfg.expected_catalog_id                = source.expected_catalog_id;
  controller_cfg.expected_catalog_hash              = source.expected_catalog_hash;
  controller_cfg.expected_identity_registry_version = source.expected_identity_registry_version;
  controller_cfg.expected_identity_registry_hash    = source.expected_identity_registry_hash;
  controller_cfg.expected_access_profile_id         = source.expected_access_profile_id;
  controller_cfg.expected_access_profile_hash       = source.expected_access_profile_hash;
  controller_cfg.onboard_cells                      = {{{first_nci, shared_pci}, {second_nci, shared_pci}}};
  ntn_onboard_position_plan_controller persisted_controller(controller_cfg);
  const auto now = std::chrono::system_clock::time_point{std::chrono::milliseconds{now_ms}};
  ASSERT_TRUE(persisted_controller.submit(active_plan, now).accepted);
  const std::string active_calendar_hash = persisted_controller.pending_plan()->calendar_hash;
  ASSERT_TRUE(persisted_controller.mark_deployment_preparing(active_plan.schedule_version, active_calendar_hash));
  ASSERT_TRUE(persisted_controller.mark_deployment_applied(active_plan.schedule_version, active_calendar_hash));
  ASSERT_TRUE(persisted_controller.advance_time(now));
  ASSERT_TRUE(persisted_controller.submit(pending_plan, now).accepted);
  const std::string pending_calendar_hash = persisted_controller.pending_plan()->calendar_hash;
  ASSERT_TRUE(persisted_controller.mark_deployment_preparing(pending_plan.schedule_version, pending_calendar_hash));
  ASSERT_TRUE(persisted_controller.mark_deployment_applied(pending_plan.schedule_version, pending_calendar_hash));
  ASSERT_TRUE(
      store_ntn_onboard_position_plan_state_atomic(source.state_file, persisted_controller.make_persistent_state(9))
          .has_value());

  std::array<uint16_t, 2>                                  recovered_intents{};
  std::array<f1ap_ntn_access_calendar_preflight_report, 2> recovered_preflight{};
  for (const ntn_access_calendar_intent& intent : persisted_controller.pending_plan()->access_calendar) {
    const unsigned cell_index = intent.nci == first_nci ? 0U : 1U;
    ++recovered_intents[cell_index];
    auto& report      = recovered_preflight[cell_index];
    report.performed  = true;
    report.passed     = true;
    report.numerology = 1;
    if (intent.purpose == ntn_access_calendar_purpose::ssb_sib_paging ||
        intent.purpose == ntn_access_calendar_purpose::ssb_sib_paging_rar) {
      ++report.expected_ssb;
      ++report.matched_ssb;
      report.max_ssb_gap_slots = 160;
    } else if (intent.purpose == ntn_access_calendar_purpose::prach_ro) {
      ++report.expected_prach;
      ++report.matched_prach;
      report.max_prach_gap_slots = 1280;
    }
  }

  cu_cp_test_env_params env_params;
  env_params.ntn_onboard_position_plan                = source;
  env_params.ntn_calendar_query_reports_applied_early = true;
  env_params.ntn_recovered_calendar_intents_per_cell  = recovered_intents;
  env_params.ntn_recovered_calendar_preflight_reports = recovered_preflight;
  cu_cp_test_environment env(std::move(env_params));
  env.run_ng_setup();

  const auto du_idx = env.connect_new_du();
  ASSERT_TRUE(du_idx.has_value());
  std::vector<test_helpers::served_cell_item_info> served_cells(2);
  served_cells[0].nci = first_nci;
  served_cells[0].pci = shared_pci;
  served_cells[1].nci = second_nci;
  served_cells[1].pci = shared_pci;
  ASSERT_TRUE(env.run_f1_setup(du_idx.value(), int_to_gnb_du_id(0x23), served_cells));

  f1ap_message pending_query;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu_without_auto_response(
      du_idx.value(), pending_query, std::chrono::milliseconds{1500}));
  const auto pending_update = decode_f1ap_ntn_access_calendar_update(
      pending_query.pdu.init_msg().value.gnb_du_res_coordination_request()->eutra_nr_cell_res_coordination_req_container);
  ASSERT_TRUE(pending_update.has_value());
  EXPECT_EQ(pending_update->operation, f1ap_ntn_access_calendar_operation::query);
  EXPECT_EQ(pending_update->schedule_version, pending_plan.schedule_version);
  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count(),
            pending_epoch_ms);
  env.respond_to_f1ap_resource_coordination_request(du_idx.value(), pending_query);

  ASSERT_TRUE(wait_for_test_condition([&env, &pending_plan]() {
    const auto before_epoch = env.get_cu_cp()
                                  .get_command_handler()
                                  .get_ntn_command_handler()
                                  .get_current_ntn_runtime_status()
                                  .onboard_position_plan;
    return before_epoch.recovery_stage == "reconciled" &&
           before_epoch.pending_schedule_version == pending_plan.schedule_version;
  }));
  const auto before_epoch = env.get_cu_cp()
                                .get_command_handler()
                                .get_ntn_command_handler()
                                .get_current_ntn_runtime_status()
                                .onboard_position_plan;
  EXPECT_EQ(before_epoch.active_schedule_version, 0U);
  EXPECT_EQ(before_epoch.pending_schedule_version, pending_plan.schedule_version);
  EXPECT_EQ(before_epoch.recovery_schedule_version, pending_plan.schedule_version);
  EXPECT_EQ(before_epoch.clear_queue_depth, 0U);
  EXPECT_EQ(env.nof_ntn_calendar_clear_requests(), 0U);

  f1ap_message clear_request;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu_without_auto_response(
      du_idx.value(), clear_request, std::chrono::milliseconds{5000}));
  EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count(),
            pending_epoch_ms);
  const auto clear_update = decode_f1ap_ntn_access_calendar_update(
      clear_request.pdu.init_msg().value.gnb_du_res_coordination_request()->eutra_nr_cell_res_coordination_req_container);
  ASSERT_TRUE(clear_update.has_value());
  EXPECT_EQ(clear_update->operation, f1ap_ntn_access_calendar_operation::clear);
  EXPECT_EQ(clear_update->schedule_version, active_plan.schedule_version);
  EXPECT_EQ(clear_update->calendar_hash, active_calendar_hash);
  auto status = env.get_cu_cp()
                    .get_command_handler()
                    .get_ntn_command_handler()
                    .get_current_ntn_runtime_status()
                    .onboard_position_plan;
  EXPECT_EQ(status.active_schedule_version, pending_plan.schedule_version);
  EXPECT_EQ(status.active_calendar_hash, pending_calendar_hash);
  EXPECT_EQ(status.clear_queue_depth, 1U);

  // Make the next state replacement fail after the DU has confirmed the clear. The obligation must remain visible in
  // memory because the durable file still cannot prove that cleanup was recorded.
  std::error_code state_path_error;
  ASSERT_TRUE(std::filesystem::remove(source.state_file, state_path_error)) << state_path_error.message();
  state_path_error.clear();
  ASSERT_TRUE(std::filesystem::create_directory(source.state_file, state_path_error)) << state_path_error.message();
  env.respond_to_f1ap_resource_coordination_request(du_idx.value(), clear_request);

  ASSERT_TRUE(wait_for_test_condition([&env, &pending_plan]() {
    const auto current = env.get_cu_cp()
                             .get_command_handler()
                             .get_ntn_command_handler()
                             .get_current_ntn_runtime_status()
                             .onboard_position_plan;
    return current.active_schedule_version == pending_plan.schedule_version && current.state_write_blocked;
  }));
  status = env.get_cu_cp()
               .get_command_handler()
               .get_ntn_command_handler()
               .get_current_ntn_runtime_status()
               .onboard_position_plan;
  EXPECT_EQ(status.pending_schedule_version, 0U);
  EXPECT_EQ(status.state_store_status, "write_failed");
  EXPECT_EQ(status.clear_queue_depth, 1U);
  EXPECT_FALSE(status.clear_in_flight);
  EXPECT_EQ(status.clear_queue_head_schedule_version, active_plan.schedule_version);
  EXPECT_EQ(status.clear_queue_head_calendar_hash, active_calendar_hash);
  EXPECT_EQ(status.clear_queue_head_reason, "recovered_pending_replaced_historical_active");
  EXPECT_EQ(env.nof_ntn_calendar_clear_requests(), 1U);
}

TEST(cu_cp_ntn_mobility_test, hidden_fallback_expiry_clears_active_before_recovered_pending_activation_epoch)
{
  const nr_cell_identity first_nci  = make_default_env_nci(0);
  const nr_cell_identity second_nci = make_default_env_nci(1);
  constexpr pci_t        shared_pci = 101;
  const int64_t          now_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count();
  const int64_t active_epoch_ms       = (now_ms / 640) * 640;
  const int64_t active_valid_until_ms = now_ms + 2500;
  const int64_t pending_epoch_ms      = active_epoch_ms + 7680;

  ASSERT_LT(active_valid_until_ms, pending_epoch_ms);

  ntn_versioned_position_plan active_plan;
  active_plan.satellite_id     = "P01-S01";
  active_plan.catalog_version  = 38;
  active_plan.schedule_version = 48;
  active_plan.valid_from = std::chrono::system_clock::time_point{std::chrono::milliseconds{active_epoch_ms - 640}};
  active_plan.activation_epoch =
      std::chrono::system_clock::time_point{std::chrono::milliseconds{active_epoch_ms}};
  active_plan.valid_until =
      std::chrono::system_clock::time_point{std::chrono::milliseconds{active_valid_until_ms}};
  active_plan.onboard_cells[0]       = {first_nci, shared_pci};
  active_plan.onboard_cells[1]       = {second_nci, shared_pci};
  active_plan.visible_l1_positions   = {{"G000001", 10.0, 20.0}, {"G000002", 10.1, 20.1}};
  bind_onboard_planning_context(active_plan);
  active_plan.content_hash = compute_ntn_position_plan_content_hash(active_plan);

  ntn_versioned_position_plan pending_plan = active_plan;
  pending_plan.catalog_version              = 39;
  pending_plan.schedule_version             = 49;
  pending_plan.activation_epoch =
      std::chrono::system_clock::time_point{std::chrono::milliseconds{pending_epoch_ms}};
  pending_plan.valid_until =
      std::chrono::system_clock::time_point{std::chrono::milliseconds{now_ms + 30000}};
  pending_plan.content_hash = compute_ntn_position_plan_content_hash(pending_plan);
  const std::filesystem::path plan_path = write_onboard_position_plan_for_runtime_test(pending_plan);
  temporary_plan_file_guard   plan_file_guard(plan_path);

  ntn_onboard_position_plan_source_config source;
  source.enabled              = true;
  source.du_execution_enabled = true;
  source.du_prepare_guard     = std::chrono::milliseconds{100};
  source.satellite_id         = pending_plan.satellite_id;
  source.plan_json_file       = plan_path.string();
  source.cell_ncis            = {first_nci, second_nci};
  source.cell_pcis            = {shared_pci, shared_pci};
  bind_onboard_planning_context(source);

  ntn_onboard_position_plan_config controller_cfg;
  controller_cfg.enabled                            = true;
  controller_cfg.require_external_apply             = true;
  controller_cfg.satellite_id                       = source.satellite_id;
  controller_cfg.expected_catalog_id                = source.expected_catalog_id;
  controller_cfg.expected_catalog_hash              = source.expected_catalog_hash;
  controller_cfg.expected_identity_registry_version = source.expected_identity_registry_version;
  controller_cfg.expected_identity_registry_hash    = source.expected_identity_registry_hash;
  controller_cfg.expected_access_profile_id         = source.expected_access_profile_id;
  controller_cfg.expected_access_profile_hash       = source.expected_access_profile_hash;
  controller_cfg.onboard_cells                      = {{{first_nci, shared_pci}, {second_nci, shared_pci}}};
  ntn_onboard_position_plan_controller persisted_controller(controller_cfg);
  const auto now = std::chrono::system_clock::time_point{std::chrono::milliseconds{now_ms}};
  ASSERT_TRUE(persisted_controller.submit(active_plan, now).accepted);
  const std::string active_calendar_hash = persisted_controller.pending_plan()->calendar_hash;
  ASSERT_TRUE(persisted_controller.mark_deployment_preparing(active_plan.schedule_version, active_calendar_hash));
  ASSERT_TRUE(persisted_controller.mark_deployment_applied(active_plan.schedule_version, active_calendar_hash));
  ASSERT_TRUE(persisted_controller.advance_time(now));
  ASSERT_TRUE(persisted_controller.submit(pending_plan, now).accepted);
  const std::string pending_calendar_hash = persisted_controller.pending_plan()->calendar_hash;
  ASSERT_TRUE(persisted_controller.mark_deployment_preparing(pending_plan.schedule_version, pending_calendar_hash));
  ASSERT_TRUE(persisted_controller.mark_deployment_applied(pending_plan.schedule_version, pending_calendar_hash));
  ASSERT_TRUE(
      store_ntn_onboard_position_plan_state_atomic(source.state_file, persisted_controller.make_persistent_state(10))
          .has_value());

  std::array<uint16_t, 2>                                  recovered_intents{};
  std::array<f1ap_ntn_access_calendar_preflight_report, 2> recovered_preflight{};
  for (const ntn_access_calendar_intent& intent : persisted_controller.pending_plan()->access_calendar) {
    const unsigned cell_index = intent.nci == first_nci ? 0U : 1U;
    ++recovered_intents[cell_index];
    auto& report      = recovered_preflight[cell_index];
    report.performed  = true;
    report.passed     = true;
    report.numerology = 1;
    if (intent.purpose == ntn_access_calendar_purpose::ssb_sib_paging ||
        intent.purpose == ntn_access_calendar_purpose::ssb_sib_paging_rar) {
      ++report.expected_ssb;
      ++report.matched_ssb;
      report.max_ssb_gap_slots = 160;
    } else if (intent.purpose == ntn_access_calendar_purpose::prach_ro) {
      ++report.expected_prach;
      ++report.matched_prach;
      report.max_prach_gap_slots = 1280;
    }
  }

  cu_cp_test_env_params env_params;
  env_params.ntn_onboard_position_plan                = source;
  env_params.ntn_calendar_query_reports_applied_early = true;
  env_params.ntn_recovered_calendar_intents_per_cell  = recovered_intents;
  env_params.ntn_recovered_calendar_preflight_reports = recovered_preflight;
  cu_cp_test_environment env(std::move(env_params));
  env.run_ng_setup();

  const auto du_idx = env.connect_new_du();
  ASSERT_TRUE(du_idx.has_value());
  std::vector<test_helpers::served_cell_item_info> served_cells(2);
  served_cells[0].nci = first_nci;
  served_cells[0].pci = shared_pci;
  served_cells[1].nci = second_nci;
  served_cells[1].pci = shared_pci;
  ASSERT_TRUE(env.run_f1_setup(du_idx.value(), int_to_gnb_du_id(0x24), served_cells));

  f1ap_message pending_query;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu_without_auto_response(
      du_idx.value(), pending_query, std::chrono::milliseconds{2000}));
  const auto pending_update = decode_f1ap_ntn_access_calendar_update(
      pending_query.pdu.init_msg().value.gnb_du_res_coordination_request()->eutra_nr_cell_res_coordination_req_container);
  ASSERT_TRUE(pending_update.has_value());
  EXPECT_EQ(pending_update->operation, f1ap_ntn_access_calendar_operation::query);
  EXPECT_EQ(pending_update->schedule_version, pending_plan.schedule_version);
  env.respond_to_f1ap_resource_coordination_request(du_idx.value(), pending_query);

  ASSERT_TRUE(wait_for_test_condition([&env, &pending_plan]() {
    const auto before_expiry = env.get_cu_cp()
                                   .get_command_handler()
                                   .get_ntn_command_handler()
                                   .get_current_ntn_runtime_status()
                                   .onboard_position_plan;
    return before_expiry.recovery_stage == "reconciled" &&
           before_expiry.pending_schedule_version == pending_plan.schedule_version;
  }));
  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count(),
            active_valid_until_ms);

  // Lose the DU before the fallback expires. The deadline must still remove the fallback and durably queue its exact
  // cleanup; transmission can wait until a DU with the same two cells reconnects.
  ASSERT_TRUE(env.drop_du_connection(du_idx.value()));
  ASSERT_TRUE(env.tick_until(std::chrono::milliseconds{5000}, [&env]() {
    return env.get_cu_cp()
               .get_command_handler()
               .get_ntn_command_handler()
               .get_current_ntn_runtime_status()
               .onboard_position_plan.clear_queue_depth == 1U;
  }));
  const int64_t fallback_expiry_observed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count();
  EXPECT_GE(fallback_expiry_observed_ms, active_valid_until_ms);
  EXPECT_LT(fallback_expiry_observed_ms, pending_epoch_ms);

  auto status = env.get_cu_cp()
                    .get_command_handler()
                    .get_ntn_command_handler()
                    .get_current_ntn_runtime_status()
                    .onboard_position_plan;
  EXPECT_EQ(status.stage, "pending");
  EXPECT_EQ(status.active_schedule_version, 0U);
  EXPECT_EQ(status.pending_schedule_version, 0U);
  EXPECT_EQ(status.recovery_stage, "reconciling");
  EXPECT_EQ(status.recovery_schedule_version, pending_plan.schedule_version);
  EXPECT_EQ(status.clear_queue_depth, 1U);
  EXPECT_EQ(status.clear_queue_head_schedule_version, active_plan.schedule_version);
  EXPECT_EQ(status.clear_queue_head_calendar_hash, active_calendar_hash);
  EXPECT_EQ(status.clear_queue_head_reason, "historical_fallback_expired");
  // The counter is advanced by the mock when it decodes and answers the request below.
  EXPECT_EQ(env.nof_ntn_calendar_clear_requests(), 0U);

  const auto persisted_after_expiry = load_ntn_onboard_position_plan_state(source.state_file);
  ASSERT_TRUE(persisted_after_expiry.has_value());
  ASSERT_TRUE(persisted_after_expiry->has_value());
  EXPECT_FALSE((*persisted_after_expiry)->active.has_value());
  ASSERT_TRUE((*persisted_after_expiry)->pending.has_value());
  EXPECT_EQ((*persisted_after_expiry)->pending->source.schedule_version, pending_plan.schedule_version);
  ASSERT_EQ((*persisted_after_expiry)->outstanding_clears.size(), 1U);
  EXPECT_EQ((*persisted_after_expiry)->outstanding_clears.front().snapshot.source.schedule_version,
            active_plan.schedule_version);
  EXPECT_EQ((*persisted_after_expiry)->outstanding_clears.front().snapshot.calendar_hash, active_calendar_hash);
  EXPECT_EQ((*persisted_after_expiry)->outstanding_clears.front().reason, "historical_fallback_expired");

  const auto reconnected_du = env.connect_new_du();
  ASSERT_TRUE(reconnected_du.has_value());
  ASSERT_TRUE(env.run_f1_setup(reconnected_du.value(), int_to_gnb_du_id(0x25), served_cells));

  f1ap_message reconnect_request;
  f1ap_message clear_request;
  bool         replacement_query_answered = false;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu_without_auto_response(
      reconnected_du.value(), reconnect_request, std::chrono::milliseconds{2000}));
  const auto reconnect_update = decode_f1ap_ntn_access_calendar_update(
      reconnect_request.pdu.init_msg().value.gnb_du_res_coordination_request()
          ->eutra_nr_cell_res_coordination_req_container);
  ASSERT_TRUE(reconnect_update.has_value());
  if (reconnect_update->operation == f1ap_ntn_access_calendar_operation::query) {
    EXPECT_EQ(reconnect_update->schedule_version, pending_plan.schedule_version);
    env.respond_to_f1ap_resource_coordination_request(reconnected_du.value(), reconnect_request);
    replacement_query_answered = true;
    ASSERT_TRUE(env.wait_for_f1ap_tx_pdu_without_auto_response(
        reconnected_du.value(), clear_request, std::chrono::milliseconds{2000}));
  } else {
    clear_request = std::move(reconnect_request);
  }
  const auto clear_update = decode_f1ap_ntn_access_calendar_update(
      clear_request.pdu.init_msg().value.gnb_du_res_coordination_request()->eutra_nr_cell_res_coordination_req_container);
  ASSERT_TRUE(clear_update.has_value());
  EXPECT_EQ(clear_update->operation, f1ap_ntn_access_calendar_operation::clear);
  EXPECT_EQ(clear_update->schedule_version, active_plan.schedule_version);
  EXPECT_EQ(clear_update->calendar_hash, active_calendar_hash);
  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count(),
            pending_epoch_ms);

  env.respond_to_f1ap_resource_coordination_request(reconnected_du.value(), clear_request);
  ASSERT_TRUE(env.tick_until(std::chrono::milliseconds{1000}, [&]() {
    return env.get_cu_cp()
               .get_command_handler()
               .get_ntn_command_handler()
               .get_current_ntn_runtime_status()
               .onboard_position_plan.clear_queue_depth == 0U;
  }));

  if (!replacement_query_answered) {
    f1ap_message replacement_query;
    ASSERT_TRUE(env.wait_for_f1ap_tx_pdu_without_auto_response(
        reconnected_du.value(), replacement_query, std::chrono::milliseconds{2000}));
    const auto replacement_update = decode_f1ap_ntn_access_calendar_update(
        replacement_query.pdu.init_msg().value.gnb_du_res_coordination_request()
            ->eutra_nr_cell_res_coordination_req_container);
    ASSERT_TRUE(replacement_update.has_value());
    EXPECT_EQ(replacement_update->operation, f1ap_ntn_access_calendar_operation::query);
    EXPECT_EQ(replacement_update->schedule_version, pending_plan.schedule_version);
    env.respond_to_f1ap_resource_coordination_request(reconnected_du.value(), replacement_query);
  }
  ASSERT_TRUE(wait_for_test_condition([&env, &pending_plan]() {
    const auto recovered = env.get_cu_cp()
                               .get_command_handler()
                               .get_ntn_command_handler()
                               .get_current_ntn_runtime_status()
                               .onboard_position_plan;
    return recovered.recovery_stage == "reconciled" &&
           recovered.pending_schedule_version == pending_plan.schedule_version;
  }));
  status = env.get_cu_cp()
               .get_command_handler()
               .get_ntn_command_handler()
               .get_current_ntn_runtime_status()
               .onboard_position_plan;
  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count(),
            pending_epoch_ms);
  EXPECT_EQ(status.stage, "pending");
  EXPECT_EQ(status.active_schedule_version, 0U);
  EXPECT_EQ(status.pending_schedule_version, pending_plan.schedule_version);
  EXPECT_EQ(status.clear_queue_depth, 0U);
  EXPECT_EQ(env.nof_ntn_calendar_clear_requests(), 1U);
}

TEST(cu_cp_ntn_mobility_test,
     hidden_fallback_expiry_persist_failure_blocks_future_activation_and_rebuilds_clear_after_restart)
{
  persisted_active_pending_calendar_test_data data = make_persisted_active_with_future_pending(
      std::chrono::milliseconds{6000}, true, std::chrono::milliseconds{12000});
  temporary_plan_file_guard plan_file_guard(data.plan_path);
  const int64_t active_valid_until_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                            data.active_plan.valid_until.time_since_epoch())
                                            .count();
  const int64_t pending_epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       data.pending_plan.activation_epoch.time_since_epoch())
                                       .count();
  ASSERT_LT(active_valid_until_ms, pending_epoch_ms);

  std::vector<test_helpers::served_cell_item_info> served_cells(2);
  served_cells[0].nci = data.active_plan.onboard_cells[0].nci;
  served_cells[0].pci = data.active_plan.onboard_cells[0].pci;
  served_cells[1].nci = data.active_plan.onboard_cells[1].nci;
  served_cells[1].pci = data.active_plan.onboard_cells[1].pci;

  {
    cu_cp_test_env_params env_params;
    env_params.ntn_onboard_position_plan                = data.source;
    env_params.ntn_calendar_query_reports_applied_early = true;
    env_params.ntn_recovered_calendar_intents_per_cell  = data.recovered_pending_intents;
    env_params.ntn_recovered_calendar_preflight_reports = data.recovered_pending_preflight;
    cu_cp_test_environment env(std::move(env_params));
    env.run_ng_setup();

    const auto du_idx = env.connect_new_du();
    ASSERT_TRUE(du_idx.has_value());
    ASSERT_TRUE(env.run_f1_setup(du_idx.value(), int_to_gnb_du_id(0x26), served_cells));

    f1ap_message pending_query;
    ASSERT_TRUE(env.wait_for_f1ap_tx_pdu_without_auto_response(
        du_idx.value(), pending_query, std::chrono::milliseconds{2000}));
    ASSERT_EQ(pending_query.pdu.type().value, asn1::f1ap::f1ap_pdu_c::types_opts::init_msg);
    ASSERT_EQ(pending_query.pdu.init_msg().proc_code, ASN1_F1AP_ID_GNB_DU_RES_COORDINATION);
    const auto pending_update = decode_f1ap_ntn_access_calendar_update(
        pending_query.pdu.init_msg().value.gnb_du_res_coordination_request()
            ->eutra_nr_cell_res_coordination_req_container);
    ASSERT_TRUE(pending_update.has_value());
    EXPECT_EQ(pending_update->operation, f1ap_ntn_access_calendar_operation::query);
    EXPECT_EQ(pending_update->schedule_version, data.pending_plan.schedule_version);
    EXPECT_EQ(pending_update->calendar_hash, data.pending_calendar_hash);
    env.respond_to_f1ap_resource_coordination_request(du_idx.value(), pending_query);

    ASSERT_TRUE(wait_for_test_condition([&]() {
      const auto before_expiry = env.get_cu_cp()
                                     .get_command_handler()
                                     .get_ntn_command_handler()
                                     .get_current_ntn_runtime_status()
                                     .onboard_position_plan;
      return before_expiry.recovery_stage == "reconciled" &&
             before_expiry.pending_schedule_version == data.pending_plan.schedule_version &&
             before_expiry.deployment_stage == "applied";
    }));
    const int64_t before_guard_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::system_clock::now().time_since_epoch())
                                        .count();
    ASSERT_GE(active_valid_until_ms - before_guard_ms, 500);

    // The durable file still contains A as the historical fallback and B as an early-applied future plan. Make the
    // replacement that records A's expiry fail. The in-memory cleanup obligation must remain exact, and the write
    // failure must prevent B from becoming active when its activation epoch arrives.
    state_file_write_failure_guard write_failure(data.source.state_file);
    ASSERT_TRUE(write_failure.replace_with_directory());
    ASSERT_TRUE(env.tick_until(std::chrono::milliseconds{8000}, [&]() {
      const auto current = env.get_cu_cp()
                               .get_command_handler()
                               .get_ntn_command_handler()
                               .get_current_ntn_runtime_status()
                               .onboard_position_plan;
      return current.state_write_blocked && current.clear_queue_depth == 1U;
    }));

    auto status = env.get_cu_cp()
                      .get_command_handler()
                      .get_ntn_command_handler()
                      .get_current_ntn_runtime_status()
                      .onboard_position_plan;
    EXPECT_EQ(status.state_store_status, "write_failed");
    EXPECT_EQ(status.active_schedule_version, 0U);
    EXPECT_EQ(status.pending_schedule_version, data.pending_plan.schedule_version);
    EXPECT_EQ(status.deployment_stage, "applied");
    EXPECT_EQ(status.clear_queue_depth, 1U);
    EXPECT_EQ(status.clear_queue_head_schedule_version, data.active_plan.schedule_version);
    EXPECT_EQ(status.clear_queue_head_calendar_hash, data.active_calendar_hash);
    EXPECT_EQ(status.clear_queue_head_reason, "historical_fallback_expired");
    EXPECT_FALSE(status.clear_in_flight);
    EXPECT_EQ(env.nof_ntn_calendar_clear_requests(), 0U);
    const int64_t after_fallback_expiry_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                                 std::chrono::system_clock::now().time_since_epoch())
                                                 .count();
    ASSERT_GE(pending_epoch_ms - after_fallback_expiry_ms, 500);

    ASSERT_TRUE(env.tick_until(std::chrono::milliseconds{8000}, [&]() {
      return std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::system_clock::now().time_since_epoch())
                 .count() >= pending_epoch_ms;
    }));
    status = env.get_cu_cp()
                 .get_command_handler()
                 .get_ntn_command_handler()
                 .get_current_ntn_runtime_status()
                 .onboard_position_plan;
    EXPECT_TRUE(status.state_write_blocked);
    EXPECT_EQ(status.active_schedule_version, 0U);
    EXPECT_EQ(status.pending_schedule_version, data.pending_plan.schedule_version);
    EXPECT_EQ(status.deployment_stage, "applied");
    EXPECT_EQ(status.clear_queue_depth, 1U);
    EXPECT_EQ(status.clear_queue_head_schedule_version, data.active_plan.schedule_version);
    EXPECT_EQ(status.clear_queue_head_calendar_hash, data.active_calendar_hash);
    EXPECT_EQ(status.clear_queue_head_reason, "historical_fallback_expired");
    EXPECT_FALSE(status.clear_in_flight);
    EXPECT_EQ(env.nof_ntn_calendar_clear_requests(), 0U);

    // Restore the last durable snapshot, then destroy this CU-CP before constructing the restart instance below.
    ASSERT_TRUE(write_failure.restore());
  }

  cu_cp_test_env_params restarted_params;
  restarted_params.ntn_onboard_position_plan = data.source;
  cu_cp_test_environment restarted_env(std::move(restarted_params));
  restarted_env.run_ng_setup();

  const auto restarted_status = restarted_env.get_cu_cp()
                                    .get_command_handler()
                                    .get_ntn_command_handler()
                                    .get_current_ntn_runtime_status()
                                    .onboard_position_plan;
  EXPECT_FALSE(restarted_status.state_write_blocked);
  EXPECT_EQ(restarted_status.active_schedule_version, 0U);
  EXPECT_EQ(restarted_status.recovery_schedule_version, data.pending_plan.schedule_version);
  EXPECT_EQ(restarted_status.clear_queue_depth, 1U);
  EXPECT_EQ(restarted_status.clear_queue_head_schedule_version, data.active_plan.schedule_version);
  EXPECT_EQ(restarted_status.clear_queue_head_calendar_hash, data.active_calendar_hash);
  EXPECT_EQ(restarted_status.clear_queue_head_reason, "expired_deployment_recovered_after_restart");
  EXPECT_GT(restarted_status.state_generation, 9U);

  const auto restarted_state = load_ntn_onboard_position_plan_state(data.source.state_file);
  ASSERT_TRUE(restarted_state.has_value()) << restarted_state.error();
  ASSERT_TRUE(restarted_state->has_value());
  EXPECT_FALSE(restarted_state->value().active.has_value());
  ASSERT_TRUE(restarted_state->value().pending.has_value());
  EXPECT_EQ(restarted_state->value().pending->source.schedule_version, data.pending_plan.schedule_version);
  ASSERT_EQ(restarted_state->value().outstanding_clears.size(), 1U);
  EXPECT_EQ(restarted_state->value().outstanding_clears.front().snapshot.source.schedule_version,
            data.active_plan.schedule_version);
  EXPECT_EQ(restarted_state->value().outstanding_clears.front().snapshot.calendar_hash, data.active_calendar_hash);
  EXPECT_EQ(restarted_state->value().outstanding_clears.front().reason,
            "expired_deployment_recovered_after_restart");
}

TEST(cu_cp_ntn_mobility_test, plan_activation_persist_failure_keeps_future_plan_hidden_until_restart_reconciliation)
{
  persisted_active_pending_calendar_test_data data = make_persisted_active_with_future_pending(
      std::chrono::milliseconds{60000}, true, std::chrono::milliseconds{9000});
  temporary_plan_file_guard plan_file_guard(data.plan_path);
  const int64_t pending_epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       data.pending_plan.activation_epoch.time_since_epoch())
                                       .count();

  std::vector<test_helpers::served_cell_item_info> served_cells(2);
  served_cells[0].nci = data.active_plan.onboard_cells[0].nci;
  served_cells[0].pci = data.active_plan.onboard_cells[0].pci;
  served_cells[1].nci = data.active_plan.onboard_cells[1].nci;
  served_cells[1].pci = data.active_plan.onboard_cells[1].pci;

  {
    cu_cp_test_env_params env_params;
    env_params.ntn_onboard_position_plan                = data.source;
    env_params.ntn_calendar_query_reports_applied_early = true;
    env_params.ntn_recovered_calendar_intents_per_cell  = data.recovered_pending_intents;
    env_params.ntn_recovered_calendar_preflight_reports = data.recovered_pending_preflight;
    cu_cp_test_environment env(std::move(env_params));
    env.run_ng_setup();

    const auto du_idx = env.connect_new_du();
    ASSERT_TRUE(du_idx.has_value());
    ASSERT_TRUE(env.run_f1_setup(du_idx.value(), int_to_gnb_du_id(0x27), served_cells));

    f1ap_message pending_query;
    ASSERT_TRUE(env.wait_for_f1ap_tx_pdu_without_auto_response(
        du_idx.value(), pending_query, std::chrono::milliseconds{2000}));
    ASSERT_EQ(pending_query.pdu.type().value, asn1::f1ap::f1ap_pdu_c::types_opts::init_msg);
    ASSERT_EQ(pending_query.pdu.init_msg().proc_code, ASN1_F1AP_ID_GNB_DU_RES_COORDINATION);
    const auto pending_update = decode_f1ap_ntn_access_calendar_update(
        pending_query.pdu.init_msg().value.gnb_du_res_coordination_request()
            ->eutra_nr_cell_res_coordination_req_container);
    ASSERT_TRUE(pending_update.has_value());
    EXPECT_EQ(pending_update->operation, f1ap_ntn_access_calendar_operation::query);
    EXPECT_EQ(pending_update->schedule_version, data.pending_plan.schedule_version);
    EXPECT_EQ(pending_update->calendar_hash, data.pending_calendar_hash);
    env.respond_to_f1ap_resource_coordination_request(du_idx.value(), pending_query);

    ASSERT_TRUE(wait_for_test_condition([&]() {
      const auto current = env.get_cu_cp()
                               .get_command_handler()
                               .get_ntn_command_handler()
                               .get_current_ntn_runtime_status()
                               .onboard_position_plan;
      return current.recovery_stage == "reconciled" && current.deployment_stage == "applied" &&
             current.pending_schedule_version == data.pending_plan.schedule_version;
    }));
    const int64_t before_guard_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::system_clock::now().time_since_epoch())
                                        .count();
    ASSERT_GE(pending_epoch_ms - before_guard_ms, 500);

    state_file_write_failure_guard write_failure(data.source.state_file);
    ASSERT_TRUE(write_failure.replace_with_directory());
    ASSERT_TRUE(env.tick_until(std::chrono::milliseconds{11000}, [&]() {
      const auto current = env.get_cu_cp()
                               .get_command_handler()
                               .get_ntn_command_handler()
                               .get_current_ntn_runtime_status()
                               .onboard_position_plan;
      return current.state_write_blocked &&
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::system_clock::now().time_since_epoch())
                     .count() >= pending_epoch_ms;
    }));

    const auto blocked_status = env.get_cu_cp()
                                    .get_command_handler()
                                    .get_ntn_command_handler()
                                    .get_current_ntn_runtime_status()
                                    .onboard_position_plan;
    EXPECT_EQ(blocked_status.state_store_status, "write_failed");
    EXPECT_EQ(blocked_status.active_schedule_version, 0U);
    EXPECT_EQ(blocked_status.pending_schedule_version, data.pending_plan.schedule_version);
    EXPECT_EQ(blocked_status.deployment_stage, "applied");
    EXPECT_EQ(blocked_status.clear_queue_depth, 0U);
    EXPECT_FALSE(blocked_status.clear_in_flight);
    EXPECT_EQ(env.nof_ntn_calendar_clear_requests(), 0U);

    ASSERT_TRUE(write_failure.restore());
  }

  cu_cp_test_env_params restarted_params;
  restarted_params.ntn_onboard_position_plan                = data.source;
  restarted_params.ntn_calendar_query_reports_applied_early = true;
  restarted_params.ntn_recovered_calendar_intents_per_cell  = data.recovered_pending_intents;
  restarted_params.ntn_recovered_calendar_preflight_reports = data.recovered_pending_preflight;
  cu_cp_test_environment restarted_env(std::move(restarted_params));
  restarted_env.run_ng_setup();

  auto restarted_status = restarted_env.get_cu_cp()
                              .get_command_handler()
                              .get_ntn_command_handler()
                              .get_current_ntn_runtime_status()
                              .onboard_position_plan;
  EXPECT_FALSE(restarted_status.state_write_blocked);
  EXPECT_EQ(restarted_status.active_schedule_version, 0U);
  EXPECT_EQ(restarted_status.recovery_schedule_version, data.pending_plan.schedule_version);

  const auto restarted_du = restarted_env.connect_new_du();
  ASSERT_TRUE(restarted_du.has_value());
  ASSERT_TRUE(restarted_env.run_f1_setup(restarted_du.value(), int_to_gnb_du_id(0x28), served_cells));
  f1ap_message restarted_query;
  ASSERT_TRUE(restarted_env.wait_for_f1ap_tx_pdu_without_auto_response(
      restarted_du.value(), restarted_query, std::chrono::milliseconds{2000}));
  ASSERT_EQ(restarted_query.pdu.type().value, asn1::f1ap::f1ap_pdu_c::types_opts::init_msg);
  ASSERT_EQ(restarted_query.pdu.init_msg().proc_code, ASN1_F1AP_ID_GNB_DU_RES_COORDINATION);
  const auto restarted_update = decode_f1ap_ntn_access_calendar_update(
      restarted_query.pdu.init_msg().value.gnb_du_res_coordination_request()
          ->eutra_nr_cell_res_coordination_req_container);
  ASSERT_TRUE(restarted_update.has_value());
  EXPECT_EQ(restarted_update->operation, f1ap_ntn_access_calendar_operation::query);
  EXPECT_EQ(restarted_update->schedule_version, data.pending_plan.schedule_version);
  EXPECT_EQ(restarted_update->calendar_hash, data.pending_calendar_hash);
  restarted_env.respond_to_f1ap_resource_coordination_request(restarted_du.value(), restarted_query);

  ASSERT_TRUE(restarted_env.tick_until(std::chrono::milliseconds{2000}, [&]() {
    restarted_status = restarted_env.get_cu_cp()
                           .get_command_handler()
                           .get_ntn_command_handler()
                           .get_current_ntn_runtime_status()
                           .onboard_position_plan;
    return restarted_status.active_schedule_version == data.pending_plan.schedule_version;
  }));
  EXPECT_FALSE(restarted_status.state_write_blocked);
  EXPECT_EQ(restarted_status.pending_schedule_version, 0U);
  EXPECT_EQ(restarted_status.clear_queue_depth, 1U);
  EXPECT_EQ(restarted_status.clear_queue_head_schedule_version, data.active_plan.schedule_version);
  EXPECT_EQ(restarted_status.clear_queue_head_calendar_hash, data.active_calendar_hash);
  EXPECT_EQ(restarted_status.clear_queue_head_reason, "recovered_pending_replaced_historical_active");

  const auto activated_state = load_ntn_onboard_position_plan_state(data.source.state_file);
  ASSERT_TRUE(activated_state.has_value()) << activated_state.error();
  ASSERT_TRUE(activated_state->has_value());
  ASSERT_TRUE(activated_state->value().active.has_value());
  EXPECT_EQ(activated_state->value().active->source.schedule_version, data.pending_plan.schedule_version);
  EXPECT_FALSE(activated_state->value().pending.has_value());
  ASSERT_EQ(activated_state->value().outstanding_clears.size(), 1U);
  EXPECT_EQ(activated_state->value().outstanding_clears.front().snapshot.source.schedule_version,
            data.active_plan.schedule_version);
  EXPECT_EQ(activated_state->value().outstanding_clears.front().snapshot.calendar_hash, data.active_calendar_hash);
}

TEST(cu_cp_ntn_mobility_test, recovery_applied_persist_failure_does_not_expose_active_plan)
{
  persisted_recovery_calendar_test_data data = make_persisted_recovery_calendar_test_data(52, 62);
  temporary_plan_file_guard             plan_file_guard(data.plan_path);

  cu_cp_test_env_params env_params;
  env_params.ntn_onboard_position_plan                = data.source;
  env_params.ntn_recovered_calendar_intents_per_cell  = data.recovered_intents;
  env_params.ntn_recovered_calendar_preflight_reports = data.recovered_preflight;
  cu_cp_test_environment env(std::move(env_params));
  env.run_ng_setup();

  state_file_write_failure_guard write_failure(data.source.state_file);
  ASSERT_TRUE(write_failure.replace_with_directory());

  const auto du_idx = env.connect_new_du();
  ASSERT_TRUE(du_idx.has_value());
  std::vector<test_helpers::served_cell_item_info> served_cells(2);
  served_cells[0].nci = data.plan.onboard_cells[0].nci;
  served_cells[0].pci = data.plan.onboard_cells[0].pci;
  served_cells[1].nci = data.plan.onboard_cells[1].nci;
  served_cells[1].pci = data.plan.onboard_cells[1].pci;
  ASSERT_TRUE(env.run_f1_setup(du_idx.value(), int_to_gnb_du_id(0x29), served_cells));

  f1ap_message recovery_query;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu_without_auto_response(
      du_idx.value(), recovery_query, std::chrono::milliseconds{2000}));
  ASSERT_EQ(recovery_query.pdu.type().value, asn1::f1ap::f1ap_pdu_c::types_opts::init_msg);
  ASSERT_EQ(recovery_query.pdu.init_msg().proc_code, ASN1_F1AP_ID_GNB_DU_RES_COORDINATION);
  const auto update = decode_f1ap_ntn_access_calendar_update(
      recovery_query.pdu.init_msg().value.gnb_du_res_coordination_request()
          ->eutra_nr_cell_res_coordination_req_container);
  ASSERT_TRUE(update.has_value());
  EXPECT_EQ(update->operation, f1ap_ntn_access_calendar_operation::query);
  EXPECT_EQ(update->schedule_version, data.plan.schedule_version);
  env.respond_to_f1ap_resource_coordination_request(du_idx.value(), recovery_query);

  ASSERT_TRUE(wait_for_test_condition([&]() {
    return env.get_cu_cp()
        .get_command_handler()
        .get_ntn_command_handler()
        .get_current_ntn_runtime_status()
        .onboard_position_plan.state_write_blocked;
  }));
  const auto status = env.get_cu_cp()
                          .get_command_handler()
                          .get_ntn_command_handler()
                          .get_current_ntn_runtime_status()
                          .onboard_position_plan;
  EXPECT_EQ(status.state_store_status, "write_failed");
  EXPECT_EQ(status.active_schedule_version, 0U);
  EXPECT_EQ(status.recovery_schedule_version, data.plan.schedule_version);
  EXPECT_EQ(status.recovery_stage, "reconciling");
  EXPECT_EQ(status.deployment_stage, "preparing");
  EXPECT_EQ(status.clear_queue_depth, 0U);

  ASSERT_TRUE(write_failure.restore());
}

TEST(cu_cp_ntn_mobility_test, committed_but_not_durable_recovery_keeps_active_hidden_and_disk_generation_aligned)
{
  persisted_recovery_calendar_test_data data = make_persisted_recovery_calendar_test_data(53, 63);
  temporary_plan_file_guard             plan_file_guard(data.plan_path);

  {
    cu_cp_test_env_params env_params;
    env_params.ntn_onboard_position_plan                = data.source;
    env_params.ntn_recovered_calendar_intents_per_cell  = data.recovered_intents;
    env_params.ntn_recovered_calendar_preflight_reports = data.recovered_preflight;
    cu_cp_test_environment env(std::move(env_params));
    env.run_ng_setup();

    const auto du_idx = env.connect_new_du();
    ASSERT_TRUE(du_idx.has_value());
    std::vector<test_helpers::served_cell_item_info> served_cells(2);
    served_cells[0].nci = data.plan.onboard_cells[0].nci;
    served_cells[0].pci = data.plan.onboard_cells[0].pci;
    served_cells[1].nci = data.plan.onboard_cells[1].nci;
    served_cells[1].pci = data.plan.onboard_cells[1].pci;
    ASSERT_TRUE(env.run_f1_setup(du_idx.value(), int_to_gnb_du_id(0x2a), served_cells));

    f1ap_message recovery_query;
    ASSERT_TRUE(env.wait_for_f1ap_tx_pdu_without_auto_response(
        du_idx.value(), recovery_query, std::chrono::milliseconds{2000}));
    const auto update = decode_f1ap_ntn_access_calendar_update(
        recovery_query.pdu.init_msg().value.gnb_du_res_coordination_request()
            ->eutra_nr_cell_res_coordination_req_container);
    ASSERT_TRUE(update.has_value());
    EXPECT_EQ(update->operation, f1ap_ntn_access_calendar_operation::query);
    EXPECT_EQ(update->schedule_version, data.plan.schedule_version);

    state_store_failpoint_guard failpoint(
        ntn_onboard_position_plan_state_store_failpoint::after_rename);
    env.respond_to_f1ap_resource_coordination_request(du_idx.value(), recovery_query);
    ASSERT_TRUE(wait_for_test_condition([&]() {
      return env.get_cu_cp()
          .get_command_handler()
          .get_ntn_command_handler()
          .get_current_ntn_runtime_status()
          .onboard_position_plan.state_write_blocked;
    }));

    const auto status = env.get_cu_cp()
                            .get_command_handler()
                            .get_ntn_command_handler()
                            .get_current_ntn_runtime_status()
                            .onboard_position_plan;
    EXPECT_EQ(status.state_store_status, "committed_not_durable");
    EXPECT_EQ(status.active_schedule_version, 0U);
    EXPECT_EQ(status.recovery_schedule_version, data.plan.schedule_version);
    EXPECT_EQ(status.recovery_stage, "reconciling");
    EXPECT_EQ(status.deployment_stage, "preparing");
    EXPECT_EQ(status.state_generation, 10U);

    const auto committed_state = load_ntn_onboard_position_plan_state(data.source.state_file);
    ASSERT_TRUE(committed_state.has_value()) << committed_state.error();
    ASSERT_TRUE(committed_state->has_value());
    ASSERT_TRUE(committed_state->value().active.has_value());
    EXPECT_EQ(committed_state->value().active->source.schedule_version, data.plan.schedule_version);
    EXPECT_EQ(committed_state->value().generation, status.state_generation);
    EXPECT_EQ(committed_state->value().state_hash, status.state_hash);
  }

  cu_cp_test_env_params restarted_params;
  restarted_params.ntn_onboard_position_plan = data.source;
  cu_cp_test_environment restarted_env(std::move(restarted_params));
  restarted_env.run_ng_setup();
  const auto restarted_status = restarted_env.get_cu_cp()
                                    .get_command_handler()
                                    .get_ntn_command_handler()
                                    .get_current_ntn_runtime_status()
                                    .onboard_position_plan;
  EXPECT_FALSE(restarted_status.state_write_blocked);
  EXPECT_EQ(restarted_status.active_schedule_version, 0U);
  EXPECT_EQ(restarted_status.recovery_schedule_version, data.plan.schedule_version);
  EXPECT_EQ(restarted_status.recovery_stage, "reconciling");
  EXPECT_EQ(restarted_status.state_generation, 10U);
}

TEST(cu_cp_ntn_mobility_test, restart_recovery_rejects_incomplete_du_query_feedback_without_exposing_active_plan)
{
  persisted_recovery_calendar_test_data data = make_persisted_recovery_calendar_test_data(31, 41);
  temporary_plan_file_guard             plan_file_guard(data.plan_path);

  auto persisted_state = load_ntn_onboard_position_plan_state(data.source.state_file);
  ASSERT_TRUE(persisted_state.has_value()) << persisted_state.error();
  ASSERT_TRUE(persisted_state->has_value());
  ASSERT_TRUE(persisted_state->value().active.has_value());
  const std::string expected_calendar_hash = persisted_state->value().active->calendar_hash;
  ASSERT_GT(data.recovered_intents[0] + data.recovered_intents[1], 0U);

  cu_cp_test_env_params env_params;
  env_params.ntn_onboard_position_plan                = data.source;
  env_params.ntn_calendar_query_reports_zero_intents  = true;
  env_params.ntn_recovered_calendar_intents_per_cell  = data.recovered_intents;
  env_params.ntn_recovered_calendar_preflight_reports = data.recovered_preflight;
  cu_cp_test_environment env(std::move(env_params));
  env.run_ng_setup();

  const auto du_idx = env.connect_new_du();
  ASSERT_TRUE(du_idx.has_value());
  std::vector<test_helpers::served_cell_item_info> served_cells(2);
  served_cells[0].nci = data.plan.onboard_cells[0].nci;
  served_cells[0].pci = data.plan.onboard_cells[0].pci;
  served_cells[1].nci = data.plan.onboard_cells[1].nci;
  served_cells[1].pci = data.plan.onboard_cells[1].pci;
  ASSERT_TRUE(env.run_f1_setup(du_idx.value(), int_to_gnb_du_id(0x15), served_cells));

  f1ap_message query_request;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu_without_auto_response(
      du_idx.value(), query_request, std::chrono::milliseconds{1500}));
  const auto& query_asn1 = query_request.pdu.init_msg().value.gnb_du_res_coordination_request();
  const auto  query_update =
      decode_f1ap_ntn_access_calendar_update(query_asn1->eutra_nr_cell_res_coordination_req_container);
  ASSERT_TRUE(query_update.has_value());
  ASSERT_EQ(query_update->operation, f1ap_ntn_access_calendar_operation::query);
  EXPECT_EQ(query_update->catalog_version, data.plan.catalog_version);
  EXPECT_EQ(query_update->schedule_version, data.plan.schedule_version);
  EXPECT_EQ(query_update->source_content_hash, data.plan.content_hash);
  EXPECT_EQ(query_update->calendar_hash, expected_calendar_hash);

  const auto status_before_response = env.get_cu_cp()
                                          .get_command_handler()
                                          .get_ntn_command_handler()
                                          .get_current_ntn_runtime_status()
                                          .onboard_position_plan;
  EXPECT_EQ(status_before_response.recovery_stage, "reconciling");
  EXPECT_EQ(status_before_response.active_schedule_version, 0U);
  EXPECT_EQ(status_before_response.execution_evidence, "intent_or_control_plane_only");

  env.respond_to_f1ap_resource_coordination_request(du_idx.value(), query_request);
  ASSERT_TRUE(env.tick_until(std::chrono::milliseconds{1000}, [&]() {
    return env.get_cu_cp()
               .get_command_handler()
               .get_ntn_command_handler()
               .get_current_ntn_runtime_status()
               .onboard_position_plan.recovery_stage == "failed";
  }));

  const auto status = env.get_cu_cp()
                          .get_command_handler()
                          .get_ntn_command_handler()
                          .get_current_ntn_runtime_status()
                          .onboard_position_plan;
  EXPECT_EQ(status.stage, "rejected");
  EXPECT_EQ(status.recovery_stage, "failed");
  EXPECT_EQ(status.recovery_detail, "du_recovery_accepted_intent_count_mismatch");
  EXPECT_EQ(status.deployment_stage, "rejected");
  EXPECT_EQ(status.deployment_detail, "du_recovery_accepted_intent_count_mismatch");
  EXPECT_EQ(status.last_rejection, "du_reconciliation_failed");
  EXPECT_EQ(status.last_rejected_schedule_version, data.plan.schedule_version);
  EXPECT_EQ(status.active_schedule_version, 0U);
  EXPECT_EQ(status.active_content_hash, "none");
  EXPECT_EQ(status.active_calendar_hash, "none");
  EXPECT_EQ(status.execution_evidence, "intent_or_control_plane_only");
}

TEST(cu_cp_ntn_mobility_test, restart_clears_expired_active_even_when_future_pending_was_not_sent)
{
  persisted_active_pending_calendar_test_data data =
      make_persisted_active_with_future_pending(std::chrono::milliseconds{-1000});
  temporary_plan_file_guard plan_file_guard(data.plan_path);

  cu_cp_test_env_params env_params;
  env_params.ntn_onboard_position_plan = data.source;
  cu_cp_test_environment env(std::move(env_params));
  env.run_ng_setup();

  auto status = env.get_cu_cp()
                    .get_command_handler()
                    .get_ntn_command_handler()
                    .get_current_ntn_runtime_status()
                    .onboard_position_plan;
  EXPECT_EQ(status.active_schedule_version, 0U);
  EXPECT_EQ(status.pending_schedule_version, data.pending_plan.schedule_version);
  EXPECT_EQ(status.deployment_stage, "not_sent");
  EXPECT_EQ(status.clear_queue_depth, 1U);
  EXPECT_EQ(status.clear_queue_head_schedule_version, data.active_plan.schedule_version);
  EXPECT_EQ(status.clear_queue_head_calendar_hash, data.active_calendar_hash);
  EXPECT_EQ(status.clear_queue_head_reason, "expired_deployment_recovered_after_restart");
  EXPECT_GT(status.state_generation, 9U);

  const auto du_idx = env.connect_new_du();
  ASSERT_TRUE(du_idx.has_value());
  std::vector<test_helpers::served_cell_item_info> served_cells(2);
  served_cells[0].nci = data.active_plan.onboard_cells[0].nci;
  served_cells[0].pci = data.active_plan.onboard_cells[0].pci;
  served_cells[1].nci = data.active_plan.onboard_cells[1].nci;
  served_cells[1].pci = data.active_plan.onboard_cells[1].pci;
  ASSERT_TRUE(env.run_f1_setup(du_idx.value(), int_to_gnb_du_id(0x17), served_cells));

  f1ap_message clear_request;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu_without_auto_response(
      du_idx.value(), clear_request, std::chrono::milliseconds{1500}));
  const auto clear_update = decode_f1ap_ntn_access_calendar_update(
      clear_request.pdu.init_msg().value.gnb_du_res_coordination_request()->eutra_nr_cell_res_coordination_req_container);
  ASSERT_TRUE(clear_update.has_value());
  EXPECT_EQ(clear_update->operation, f1ap_ntn_access_calendar_operation::clear);
  EXPECT_EQ(clear_update->schedule_version, data.active_plan.schedule_version);
  EXPECT_EQ(clear_update->calendar_hash, data.active_calendar_hash);

  env.respond_to_f1ap_resource_coordination_request(du_idx.value(), clear_request);
  ASSERT_TRUE(env.tick_until(std::chrono::milliseconds{1000}, [&]() {
    return env.get_cu_cp()
               .get_command_handler()
               .get_ntn_command_handler()
               .get_current_ntn_runtime_status()
               .onboard_position_plan.clear_queue_depth == 0U;
  }));
  status = env.get_cu_cp()
               .get_command_handler()
               .get_ntn_command_handler()
               .get_current_ntn_runtime_status()
               .onboard_position_plan;
  EXPECT_EQ(status.active_schedule_version, 0U);
  EXPECT_EQ(status.pending_schedule_version, data.pending_plan.schedule_version);
  EXPECT_EQ(status.deployment_stage, "not_sent");
  EXPECT_EQ(env.nof_ntn_calendar_clear_requests(), 1U);

  auto stored_state = load_ntn_onboard_position_plan_state(data.source.state_file);
  ASSERT_TRUE(stored_state.has_value()) << stored_state.error();
  ASSERT_TRUE(stored_state->has_value());
  EXPECT_FALSE(stored_state->value().active.has_value());
  ASSERT_TRUE(stored_state->value().pending.has_value());
  EXPECT_EQ(stored_state->value().pending->source.schedule_version, data.pending_plan.schedule_version);
  EXPECT_TRUE(stored_state->value().outstanding_clears.empty());
}

TEST(cu_cp_ntn_mobility_test, restart_retains_expired_active_cleanup_when_sixty_four_tasks_are_already_queued)
{
  persisted_active_pending_calendar_test_data data =
      make_persisted_active_with_future_pending(std::chrono::milliseconds{-1000});
  temporary_plan_file_guard plan_file_guard(data.plan_path);

  auto loaded_state = load_ntn_onboard_position_plan_state(data.source.state_file);
  ASSERT_TRUE(loaded_state.has_value()) << loaded_state.error();
  ASSERT_TRUE(loaded_state->has_value());
  auto state = std::move(loaded_state->value());
  ASSERT_TRUE(state.active.has_value());
  ASSERT_TRUE(state.pending.has_value());
  state.outstanding_clears.clear();
  for (unsigned i = 0; i != 64; ++i) {
    ntn_onboard_position_plan_state_snapshot snapshot = *state.active;
    snapshot.source.schedule_version                  = 1000 + i;
    snapshot.source.planning_run_id                   = fmt::format("cleanup-capacity-{}", i);
    snapshot.source.content_hash = compute_ntn_position_plan_content_hash(snapshot.source);
    snapshot.calendar_hash = compute_ntn_access_calendar_hash(snapshot.source.schedule_version, {});
    state.outstanding_clears.push_back({std::move(snapshot), "capacity_fixture"});
  }
  state.highest_schedule_version = 1063;
  const auto fixture_store = store_ntn_onboard_position_plan_state_atomic(data.source.state_file, state);
  ASSERT_TRUE(fixture_store.has_value()) << fixture_store.error();
  ASSERT_TRUE(fixture_store->durable) << fixture_store->durability_error;

  cu_cp_test_env_params env_params;
  env_params.ntn_onboard_position_plan = data.source;
  cu_cp_test_environment env(std::move(env_params));
  env.run_ng_setup();

  const auto status = env.get_cu_cp()
                          .get_command_handler()
                          .get_ntn_command_handler()
                          .get_current_ntn_runtime_status()
                          .onboard_position_plan;
  EXPECT_FALSE(status.state_write_blocked);
  EXPECT_EQ(status.state_store_status, "stored");
  EXPECT_EQ(status.active_schedule_version, 0U);
  EXPECT_EQ(status.pending_schedule_version, data.pending_plan.schedule_version);
  EXPECT_EQ(status.clear_queue_depth, 65U);

  auto recovered_state = load_ntn_onboard_position_plan_state(data.source.state_file);
  ASSERT_TRUE(recovered_state.has_value()) << recovered_state.error();
  ASSERT_TRUE(recovered_state->has_value());
  EXPECT_FALSE(recovered_state->value().active.has_value());
  ASSERT_TRUE(recovered_state->value().pending.has_value());
  EXPECT_EQ(recovered_state->value().pending->source.schedule_version, data.pending_plan.schedule_version);
  ASSERT_EQ(recovered_state->value().outstanding_clears.size(), 65U);
  const auto recovered_active_clear = std::find_if(
      recovered_state->value().outstanding_clears.begin(),
      recovered_state->value().outstanding_clears.end(),
      [&data](const ntn_onboard_position_plan_clear_obligation& obligation) {
        return obligation.snapshot.source.schedule_version == data.active_plan.schedule_version &&
               obligation.snapshot.calendar_hash == data.active_calendar_hash &&
               obligation.reason == "expired_deployment_recovered_after_restart";
      });
  EXPECT_NE(recovered_active_clear, recovered_state->value().outstanding_clears.end());
}

TEST(cu_cp_ntn_mobility_test, live_active_expiry_queues_clear_while_future_pending_remains_not_sent)
{
  persisted_active_pending_calendar_test_data data =
      make_persisted_active_with_future_pending(std::chrono::milliseconds{6000});
  temporary_plan_file_guard plan_file_guard(data.plan_path);

  cu_cp_test_env_params env_params;
  env_params.ntn_onboard_position_plan                = data.source;
  env_params.ntn_recovered_calendar_intents_per_cell  = data.recovered_active_intents;
  env_params.ntn_recovered_calendar_preflight_reports = data.recovered_active_preflight;
  cu_cp_test_environment env(std::move(env_params));
  env.run_ng_setup();

  const auto du_idx = env.connect_new_du();
  ASSERT_TRUE(du_idx.has_value());
  std::vector<test_helpers::served_cell_item_info> served_cells(2);
  served_cells[0].nci = data.active_plan.onboard_cells[0].nci;
  served_cells[0].pci = data.active_plan.onboard_cells[0].pci;
  served_cells[1].nci = data.active_plan.onboard_cells[1].nci;
  served_cells[1].pci = data.active_plan.onboard_cells[1].pci;
  ASSERT_TRUE(env.run_f1_setup(du_idx.value(), int_to_gnb_du_id(0x18), served_cells));

  f1ap_message query_request;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu_without_auto_response(
      du_idx.value(), query_request, std::chrono::milliseconds{1500}));
  const auto query_update = decode_f1ap_ntn_access_calendar_update(
      query_request.pdu.init_msg().value.gnb_du_res_coordination_request()->eutra_nr_cell_res_coordination_req_container);
  ASSERT_TRUE(query_update.has_value());
  EXPECT_EQ(query_update->operation, f1ap_ntn_access_calendar_operation::query);
  EXPECT_EQ(query_update->schedule_version, data.active_plan.schedule_version);
  env.respond_to_f1ap_resource_coordination_request(du_idx.value(), query_request);

  ASSERT_TRUE(wait_for_test_condition([&]() {
    const auto current = env.get_cu_cp()
                             .get_command_handler()
                             .get_ntn_command_handler()
                             .get_current_ntn_runtime_status()
                             .onboard_position_plan;
    return current.active_schedule_version == data.active_plan.schedule_version &&
           current.pending_schedule_version == data.pending_plan.schedule_version &&
           current.deployment_stage == "not_sent";
  }));

  f1ap_message clear_request;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu_without_auto_response(
      du_idx.value(), clear_request, std::chrono::milliseconds{8000}));
  const auto clear_update = decode_f1ap_ntn_access_calendar_update(
      clear_request.pdu.init_msg().value.gnb_du_res_coordination_request()->eutra_nr_cell_res_coordination_req_container);
  ASSERT_TRUE(clear_update.has_value());
  EXPECT_EQ(clear_update->operation, f1ap_ntn_access_calendar_operation::clear);
  EXPECT_EQ(clear_update->schedule_version, data.active_plan.schedule_version);
  EXPECT_EQ(clear_update->calendar_hash, data.active_calendar_hash);

  auto status = env.get_cu_cp()
                    .get_command_handler()
                    .get_ntn_command_handler()
                    .get_current_ntn_runtime_status()
                    .onboard_position_plan;
  EXPECT_EQ(status.active_schedule_version, 0U);
  EXPECT_EQ(status.pending_schedule_version, data.pending_plan.schedule_version);
  EXPECT_EQ(status.deployment_stage, "not_sent");
  EXPECT_EQ(status.clear_queue_depth, 1U);
  EXPECT_EQ(status.clear_queue_head_schedule_version, data.active_plan.schedule_version);
  EXPECT_EQ(status.clear_queue_head_calendar_hash, data.active_calendar_hash);
  EXPECT_EQ(status.clear_queue_head_reason, "active_plan_expired");

  env.respond_to_f1ap_resource_coordination_request(du_idx.value(), clear_request);
  ASSERT_TRUE(env.tick_until(std::chrono::milliseconds{1000}, [&]() {
    return env.get_cu_cp()
               .get_command_handler()
               .get_ntn_command_handler()
               .get_current_ntn_runtime_status()
               .onboard_position_plan.clear_queue_depth == 0U;
  }));
  EXPECT_EQ(env.nof_ntn_calendar_clear_requests(), 1U);
}

TEST(cu_cp_ntn_mobility_test, restart_drops_expired_not_sent_plan_without_sending_clear)
{
  const nr_cell_identity first_nci  = make_default_env_nci(0);
  const nr_cell_identity second_nci = make_default_env_nci(1);
  constexpr pci_t        shared_pci = 101;
  const int64_t          wall_now_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count();
  const int64_t historical_now_ms = wall_now_ms - 2000;
  const int64_t activation_ms     = (historical_now_ms / 640) * 640;

  ntn_versioned_position_plan plan;
  plan.satellite_id         = "P01-S01";
  plan.catalog_version      = 33;
  plan.schedule_version     = 43;
  plan.valid_from           = std::chrono::system_clock::time_point{std::chrono::milliseconds{activation_ms - 640}};
  plan.activation_epoch     = std::chrono::system_clock::time_point{std::chrono::milliseconds{activation_ms}};
  plan.valid_until          = std::chrono::system_clock::time_point{std::chrono::milliseconds{wall_now_ms - 1000}};
  plan.onboard_cells[0]     = {first_nci, shared_pci};
  plan.onboard_cells[1]     = {second_nci, shared_pci};
  plan.visible_l1_positions = {{"G000001", 10.0, 20.0}, {"G000002", 10.1, 20.1}};
  bind_onboard_planning_context(plan);
  plan.content_hash                     = compute_ntn_position_plan_content_hash(plan);
  const std::filesystem::path plan_path = write_onboard_position_plan_for_runtime_test(plan);
  temporary_plan_file_guard   plan_file_guard(plan_path);

  ntn_onboard_position_plan_source_config source;
  source.enabled              = true;
  source.du_execution_enabled = true;
  source.du_prepare_guard     = std::chrono::milliseconds{100};
  source.satellite_id         = plan.satellite_id;
  source.plan_json_file       = plan_path.string();
  source.cell_ncis            = {first_nci, second_nci};
  source.cell_pcis            = {shared_pci, shared_pci};
  bind_onboard_planning_context(source);

  ntn_onboard_position_plan_config controller_cfg;
  controller_cfg.enabled                            = true;
  controller_cfg.require_external_apply             = true;
  controller_cfg.satellite_id                       = source.satellite_id;
  controller_cfg.expected_catalog_id                = source.expected_catalog_id;
  controller_cfg.expected_catalog_hash              = source.expected_catalog_hash;
  controller_cfg.expected_identity_registry_version = source.expected_identity_registry_version;
  controller_cfg.expected_identity_registry_hash    = source.expected_identity_registry_hash;
  controller_cfg.expected_access_profile_id         = source.expected_access_profile_id;
  controller_cfg.expected_access_profile_hash       = source.expected_access_profile_hash;
  controller_cfg.onboard_cells                      = {{{first_nci, shared_pci}, {second_nci, shared_pci}}};
  ntn_onboard_position_plan_controller persisted_controller(controller_cfg);
  const auto historical_now =
      std::chrono::system_clock::time_point{std::chrono::milliseconds{historical_now_ms}};
  ASSERT_TRUE(persisted_controller.submit(plan, historical_now).accepted);
  ASSERT_TRUE(persisted_controller.pending_plan().has_value());
  ASSERT_EQ(persisted_controller.deployment_stage(), ntn_position_plan_deployment_stage::not_sent);
  ASSERT_TRUE(
      store_ntn_onboard_position_plan_state_atomic(source.state_file, persisted_controller.make_persistent_state(9))
          .has_value());

  cu_cp_test_env_params env_params;
  env_params.ntn_onboard_position_plan = source;
  cu_cp_test_environment env(std::move(env_params));
  env.run_ng_setup();
  auto status = env.get_cu_cp()
                    .get_command_handler()
                    .get_ntn_command_handler()
                    .get_current_ntn_runtime_status()
                    .onboard_position_plan;
  EXPECT_EQ(status.active_schedule_version, 0U);
  EXPECT_EQ(status.pending_schedule_version, 0U);
  EXPECT_EQ(status.clear_queue_depth, 0U);
  EXPECT_EQ(status.schedule_version_high_water, plan.schedule_version);

  const auto du_idx = env.connect_new_du();
  ASSERT_TRUE(du_idx.has_value());
  std::vector<test_helpers::served_cell_item_info> served_cells(2);
  served_cells[0].nci = first_nci;
  served_cells[0].pci = shared_pci;
  served_cells[1].nci = second_nci;
  served_cells[1].pci = shared_pci;
  ASSERT_TRUE(env.run_f1_setup(du_idx.value(), int_to_gnb_du_id(0x15), served_cells));
  f1ap_message unexpected_request;
  EXPECT_FALSE(env.wait_for_f1ap_tx_pdu_without_auto_response(
      du_idx.value(), unexpected_request, std::chrono::milliseconds{700}));
  EXPECT_EQ(env.nof_ntn_calendar_clear_requests(), 0U);
  status = env.get_cu_cp()
               .get_command_handler()
               .get_ntn_command_handler()
               .get_current_ntn_runtime_status()
               .onboard_position_plan;
  EXPECT_EQ(status.clear_queue_depth, 0U);
  EXPECT_EQ(status.clear_queue_head_schedule_version, 0U);
  EXPECT_EQ(status.clear_queue_head_calendar_hash, "none");
  EXPECT_EQ(status.clear_queue_head_reason, "none");
  EXPECT_EQ(status.schedule_version_high_water, plan.schedule_version);
}

TEST(cu_cp_ntn_mobility_test, restart_clears_an_expired_deployed_calendar_before_accepting_new_work)
{
  const nr_cell_identity first_nci  = make_default_env_nci(0);
  const nr_cell_identity second_nci = make_default_env_nci(1);
  constexpr pci_t        shared_pci = 101;
  const int64_t          wall_now_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count();
  const int64_t historical_now_ms = wall_now_ms - 2000;
  const int64_t activation_ms     = (historical_now_ms / 640) * 640;

  ntn_versioned_position_plan plan;
  plan.satellite_id         = "P01-S01";
  plan.catalog_version      = 34;
  plan.schedule_version     = 44;
  plan.valid_from           = std::chrono::system_clock::time_point{std::chrono::milliseconds{activation_ms - 640}};
  plan.activation_epoch     = std::chrono::system_clock::time_point{std::chrono::milliseconds{activation_ms}};
  plan.valid_until          = std::chrono::system_clock::time_point{std::chrono::milliseconds{wall_now_ms - 1000}};
  plan.onboard_cells[0]     = {first_nci, shared_pci};
  plan.onboard_cells[1]     = {second_nci, shared_pci};
  plan.visible_l1_positions = {{"G000001", 10.0, 20.0}, {"G000002", 10.1, 20.1}};
  bind_onboard_planning_context(plan);
  plan.content_hash                     = compute_ntn_position_plan_content_hash(plan);
  const std::filesystem::path plan_path = write_onboard_position_plan_for_runtime_test(plan);
  temporary_plan_file_guard   plan_file_guard(plan_path);

  ntn_onboard_position_plan_source_config source;
  source.enabled              = true;
  source.du_execution_enabled = true;
  source.du_prepare_guard     = std::chrono::milliseconds{100};
  source.satellite_id         = plan.satellite_id;
  source.plan_json_file       = plan_path.string();
  source.cell_ncis            = {first_nci, second_nci};
  source.cell_pcis            = {shared_pci, shared_pci};
  bind_onboard_planning_context(source);

  ntn_onboard_position_plan_config controller_cfg;
  controller_cfg.enabled                            = true;
  controller_cfg.require_external_apply             = true;
  controller_cfg.satellite_id                       = source.satellite_id;
  controller_cfg.expected_catalog_id                = source.expected_catalog_id;
  controller_cfg.expected_catalog_hash              = source.expected_catalog_hash;
  controller_cfg.expected_identity_registry_version = source.expected_identity_registry_version;
  controller_cfg.expected_identity_registry_hash    = source.expected_identity_registry_hash;
  controller_cfg.expected_access_profile_id         = source.expected_access_profile_id;
  controller_cfg.expected_access_profile_hash       = source.expected_access_profile_hash;
  controller_cfg.onboard_cells                      = {{{first_nci, shared_pci}, {second_nci, shared_pci}}};
  ntn_onboard_position_plan_controller persisted_controller(controller_cfg);
  const auto historical_now =
      std::chrono::system_clock::time_point{std::chrono::milliseconds{historical_now_ms}};
  ASSERT_TRUE(persisted_controller.submit(plan, historical_now).accepted);
  const std::string calendar_hash = persisted_controller.pending_plan()->calendar_hash;
  ASSERT_TRUE(persisted_controller.mark_deployment_preparing(plan.schedule_version, calendar_hash));
  ASSERT_TRUE(persisted_controller.mark_deployment_applied(plan.schedule_version, calendar_hash));
  ASSERT_TRUE(persisted_controller.advance_time(historical_now));

  // Keep the complete latest management-center input as observation even though it exceeds the executable
  // two-cell capacity. It must survive cleanup of the older, expired software calendar without becoming active.
  ntn_versioned_position_plan overflow_plan = plan;
  overflow_plan.catalog_version              = plan.catalog_version + 1;
  overflow_plan.schedule_version             = plan.schedule_version + 1;
  overflow_plan.visible_l1_positions.clear();
  overflow_plan.visible_l1_positions.reserve(257);
  for (unsigned i = 0; i != 257; ++i) {
    overflow_plan.visible_l1_positions.push_back(
        {fmt::format("G{:06}", i + 1), 10.0 + static_cast<double>(i / 32) * 0.01, 20.0, 0x7f});
  }
  overflow_plan.content_hash = compute_ntn_position_plan_content_hash(overflow_plan);
  const auto overflow_result = persisted_controller.submit(overflow_plan, historical_now);
  EXPECT_FALSE(overflow_result.accepted);
  EXPECT_EQ(overflow_result.reason, ntn_position_plan_reject_reason::schedule_overflow);
  EXPECT_EQ(persisted_controller.active_plan()->source.schedule_version, plan.schedule_version);
  const std::filesystem::path overflow_plan_path = write_onboard_position_plan_for_runtime_test(overflow_plan);
  temporary_plan_file_guard   overflow_plan_file_guard(overflow_plan_path);
  // The state file remains tied to the original source path, while the management-center input visible after restart
  // is the newer rejected plan whose complete 257-position inventory must remain observable.
  source.plan_json_file = overflow_plan_path.string();
  ASSERT_TRUE(
      store_ntn_onboard_position_plan_state_atomic(source.state_file, persisted_controller.make_persistent_state(9))
          .has_value());

  // If the DU confirms the clear but the state file cannot be updated, retain the exact obligation in memory and
  // block further writes. Restoring the original file below models a safe operator repair before the next restart.
  {
    cu_cp_test_env_params failed_persist_params;
    failed_persist_params.ntn_onboard_position_plan = source;
    cu_cp_test_environment failed_persist_env(std::move(failed_persist_params));
    failed_persist_env.run_ng_setup();

    const auto failed_persist_du = failed_persist_env.connect_new_du();
    ASSERT_TRUE(failed_persist_du.has_value());
    std::vector<test_helpers::served_cell_item_info> failed_persist_cells(2);
    failed_persist_cells[0].nci = first_nci;
    failed_persist_cells[0].pci = shared_pci;
    failed_persist_cells[1].nci = second_nci;
    failed_persist_cells[1].pci = shared_pci;
    ASSERT_TRUE(
        failed_persist_env.run_f1_setup(failed_persist_du.value(), int_to_gnb_du_id(0x16), failed_persist_cells));
    f1ap_message failed_persist_clear_request;
    ASSERT_TRUE(failed_persist_env.wait_for_f1ap_tx_pdu_without_auto_response(
        failed_persist_du.value(), failed_persist_clear_request, std::chrono::milliseconds{1500}));

    state_file_write_failure_guard write_failure(source.state_file);
    ASSERT_TRUE(write_failure.replace_with_directory());
    failed_persist_env.respond_to_f1ap_resource_coordination_request(failed_persist_du.value(),
                                                                     failed_persist_clear_request);
    ASSERT_TRUE(wait_for_test_condition([&failed_persist_env]() {
      return failed_persist_env.get_cu_cp()
                 .get_command_handler()
                 .get_ntn_command_handler()
                 .get_current_ntn_runtime_status()
                 .onboard_position_plan.state_write_blocked;
    }));
    const auto failed_persist_status = failed_persist_env.get_cu_cp()
                                           .get_command_handler()
                                           .get_ntn_command_handler()
                                           .get_current_ntn_runtime_status()
                                           .onboard_position_plan;
    EXPECT_EQ(failed_persist_status.state_store_status, "write_failed");
    EXPECT_EQ(failed_persist_status.clear_queue_depth, 1U);
    EXPECT_EQ(failed_persist_status.clear_queue_head_schedule_version, plan.schedule_version);
    EXPECT_EQ(failed_persist_status.clear_queue_head_calendar_hash, calendar_hash);
    EXPECT_EQ(failed_persist_status.clear_queue_head_reason, "expired_deployment_recovered_after_restart");
    EXPECT_EQ(failed_persist_env.nof_ntn_calendar_clear_requests(), 1U);
    ASSERT_TRUE(write_failure.restore());
  }

  {
    cu_cp_test_env_params env_params;
    env_params.ntn_onboard_position_plan = source;
    cu_cp_test_environment env(std::move(env_params));
    env.run_ng_setup();

    auto status = env.get_cu_cp()
                      .get_command_handler()
                      .get_ntn_command_handler()
                      .get_current_ntn_runtime_status()
                      .onboard_position_plan;
    EXPECT_EQ(status.recovery_stage, "reconciled");
    EXPECT_EQ(status.active_schedule_version, 0U);
    EXPECT_EQ(status.clear_queue_depth, 1U);
    EXPECT_EQ(status.clear_queue_head_schedule_version, plan.schedule_version);
    EXPECT_EQ(status.clear_queue_head_calendar_hash, calendar_hash);
    EXPECT_EQ(status.clear_queue_head_reason, "expired_deployment_recovered_after_restart");
    EXPECT_GT(status.state_generation, 9U);
    EXPECT_EQ(status.execution_evidence, "intent_or_control_plane_only");
    EXPECT_EQ(status.received_schedule_version, overflow_plan.schedule_version);
    EXPECT_EQ(status.candidate_l1_positions, 257U);

    const auto du_idx = env.connect_new_du();
    ASSERT_TRUE(du_idx.has_value());
    std::vector<test_helpers::served_cell_item_info> served_cells(2);
    served_cells[0].nci = first_nci;
    served_cells[0].pci = shared_pci;
    served_cells[1].nci = second_nci;
    served_cells[1].pci = shared_pci;
    ASSERT_TRUE(env.run_f1_setup(du_idx.value(), int_to_gnb_du_id(0x16), served_cells));
    f1ap_message clear_request;
    ASSERT_TRUE(env.wait_for_f1ap_tx_pdu_without_auto_response(
        du_idx.value(), clear_request, std::chrono::milliseconds{1500}));
    const auto& clear_asn1 = clear_request.pdu.init_msg().value.gnb_du_res_coordination_request();
    const auto  clear_update =
        decode_f1ap_ntn_access_calendar_update(clear_asn1->eutra_nr_cell_res_coordination_req_container);
    ASSERT_TRUE(clear_update.has_value());
    EXPECT_EQ(clear_update->operation, f1ap_ntn_access_calendar_operation::clear);
    EXPECT_EQ(clear_update->schedule_version, plan.schedule_version);
    env.respond_to_f1ap_resource_coordination_request(du_idx.value(), clear_request);
    ASSERT_TRUE(env.tick_until(std::chrono::milliseconds{1000}, [&]() {
      return env.get_cu_cp()
                 .get_command_handler()
                 .get_ntn_command_handler()
                 .get_current_ntn_runtime_status()
                 .onboard_position_plan.clear_queue_depth == 0U;
    }));
    EXPECT_EQ(env.nof_ntn_calendar_clear_requests(), 1U);

    status = env.get_cu_cp()
                 .get_command_handler()
                 .get_ntn_command_handler()
                 .get_current_ntn_runtime_status()
                 .onboard_position_plan;
    EXPECT_EQ(status.active_schedule_version, 0U);
    EXPECT_EQ(status.clear_queue_depth, 0U);
    EXPECT_EQ(status.clear_queue_head_schedule_version, 0U);
    EXPECT_EQ(status.clear_queue_head_calendar_hash, "none");
    EXPECT_EQ(status.clear_queue_head_reason, "none");
    EXPECT_EQ(status.execution_evidence, "intent_or_control_plane_only");

    auto stored_state = load_ntn_onboard_position_plan_state(source.state_file);
    ASSERT_TRUE(stored_state.has_value()) << stored_state.error();
    ASSERT_TRUE(stored_state->has_value());
    EXPECT_FALSE(stored_state->value().active.has_value());
    EXPECT_FALSE(stored_state->value().pending.has_value());
    EXPECT_TRUE(stored_state->value().outstanding_clears.empty());
    EXPECT_GT(stored_state->value().generation, 10U);
    ASSERT_TRUE(stored_state->value().received_plan.has_value());
    EXPECT_EQ(stored_state->value().received_plan->schedule_version, overflow_plan.schedule_version);
    EXPECT_EQ(stored_state->value().received_plan->candidate_inventory.size(), 257U);
  }

  // A confirmed clear is removed from durable state and must not be sent again after another restart.
  cu_cp_test_env_params restarted_params;
  restarted_params.ntn_onboard_position_plan = source;
  cu_cp_test_environment restarted_env(std::move(restarted_params));
  restarted_env.run_ng_setup();
  const auto restarted_du = restarted_env.connect_new_du();
  ASSERT_TRUE(restarted_du.has_value());
  std::vector<test_helpers::served_cell_item_info> restarted_cells(2);
  restarted_cells[0].nci = first_nci;
  restarted_cells[0].pci = shared_pci;
  restarted_cells[1].nci = second_nci;
  restarted_cells[1].pci = shared_pci;
  ASSERT_TRUE(restarted_env.run_f1_setup(restarted_du.value(), int_to_gnb_du_id(0x16), restarted_cells));
  f1ap_message unexpected_request;
  EXPECT_FALSE(restarted_env.wait_for_f1ap_tx_pdu_without_auto_response(
      restarted_du.value(), unexpected_request, std::chrono::milliseconds{700}));
  EXPECT_EQ(restarted_env.nof_ntn_calendar_clear_requests(), 0U);
  const auto restarted_status = restarted_env.get_cu_cp()
                                    .get_command_handler()
                                    .get_ntn_command_handler()
                                    .get_current_ntn_runtime_status()
                                    .onboard_position_plan;
  EXPECT_EQ(restarted_status.clear_queue_depth, 0U);
  EXPECT_EQ(restarted_status.clear_queue_head_schedule_version, 0U);
  EXPECT_EQ(restarted_status.clear_queue_head_calendar_hash, "none");
  EXPECT_EQ(restarted_status.clear_queue_head_reason, "none");
  EXPECT_EQ(restarted_status.received_schedule_version, overflow_plan.schedule_version);
  EXPECT_EQ(restarted_status.candidate_l1_positions, 257U);
}

TEST(cu_cp_ntn_mobility_test, restart_clears_only_the_expired_update_before_restoring_the_valid_active_plan)
{
  const nr_cell_identity first_nci  = make_default_env_nci(0);
  const nr_cell_identity second_nci = make_default_env_nci(1);
  constexpr pci_t        shared_pci = 101;
  const int64_t          wall_now_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count();
  const int64_t historical_now_ms = wall_now_ms - 2000;
  const int64_t activation_ms     = (historical_now_ms / 640) * 640;
  const auto    historical_now =
      std::chrono::system_clock::time_point{std::chrono::milliseconds{historical_now_ms}};

  ntn_versioned_position_plan active_plan;
  active_plan.satellite_id     = "P01-S01";
  active_plan.catalog_version  = 35;
  active_plan.schedule_version = 45;
  active_plan.valid_from = std::chrono::system_clock::time_point{std::chrono::milliseconds{activation_ms - 640}};
  active_plan.activation_epoch = std::chrono::system_clock::time_point{std::chrono::milliseconds{activation_ms}};
  active_plan.valid_until = std::chrono::system_clock::time_point{std::chrono::milliseconds{wall_now_ms + 30000}};
  active_plan.onboard_cells[0]     = {first_nci, shared_pci};
  active_plan.onboard_cells[1]     = {second_nci, shared_pci};
  active_plan.visible_l1_positions = {{"G000001", 10.0, 20.0}, {"G000002", 10.1, 20.1}};
  bind_onboard_planning_context(active_plan);
  active_plan.content_hash = compute_ntn_position_plan_content_hash(active_plan);

  ntn_versioned_position_plan expired_update = active_plan;
  expired_update.catalog_version              = 36;
  expired_update.schedule_version             = 46;
  expired_update.valid_until =
      std::chrono::system_clock::time_point{std::chrono::milliseconds{wall_now_ms - 1000}};
  expired_update.visible_l1_positions.push_back({"G000003", 10.2, 20.2});
  bind_onboard_planning_context(expired_update);
  expired_update.content_hash = compute_ntn_position_plan_content_hash(expired_update);
  const std::filesystem::path plan_path = write_onboard_position_plan_for_runtime_test(expired_update);
  temporary_plan_file_guard   plan_file_guard(plan_path);

  ntn_onboard_position_plan_source_config source;
  source.enabled              = true;
  source.du_execution_enabled = true;
  source.du_prepare_guard     = std::chrono::milliseconds{100};
  source.satellite_id         = active_plan.satellite_id;
  source.plan_json_file       = plan_path.string();
  source.cell_ncis            = {first_nci, second_nci};
  source.cell_pcis            = {shared_pci, shared_pci};
  bind_onboard_planning_context(source);

  ntn_onboard_position_plan_config controller_cfg;
  controller_cfg.enabled                            = true;
  controller_cfg.require_external_apply             = true;
  controller_cfg.satellite_id                       = source.satellite_id;
  controller_cfg.expected_catalog_id                = source.expected_catalog_id;
  controller_cfg.expected_catalog_hash              = source.expected_catalog_hash;
  controller_cfg.expected_identity_registry_version = source.expected_identity_registry_version;
  controller_cfg.expected_identity_registry_hash    = source.expected_identity_registry_hash;
  controller_cfg.expected_access_profile_id         = source.expected_access_profile_id;
  controller_cfg.expected_access_profile_hash       = source.expected_access_profile_hash;
  controller_cfg.onboard_cells                      = {{{first_nci, shared_pci}, {second_nci, shared_pci}}};
  ntn_onboard_position_plan_controller persisted_controller(controller_cfg);
  ASSERT_TRUE(persisted_controller.submit(active_plan, historical_now).accepted);
  const std::string active_calendar_hash = persisted_controller.pending_plan()->calendar_hash;
  ASSERT_TRUE(persisted_controller.mark_deployment_preparing(active_plan.schedule_version, active_calendar_hash));
  ASSERT_TRUE(persisted_controller.mark_deployment_applied(active_plan.schedule_version, active_calendar_hash));
  ASSERT_TRUE(persisted_controller.advance_time(historical_now));
  ASSERT_TRUE(persisted_controller.submit(expired_update, historical_now).accepted);
  const std::string expired_calendar_hash = persisted_controller.pending_plan()->calendar_hash;
  ASSERT_TRUE(
      persisted_controller.mark_deployment_preparing(expired_update.schedule_version, expired_calendar_hash));
  ASSERT_TRUE(
      store_ntn_onboard_position_plan_state_atomic(source.state_file, persisted_controller.make_persistent_state(9))
          .has_value());

  std::array<uint16_t, 2>                                  recovered_intents{};
  std::array<f1ap_ntn_access_calendar_preflight_report, 2> recovered_preflight{};
  ASSERT_TRUE(persisted_controller.active_plan().has_value());
  for (const ntn_access_calendar_intent& intent : persisted_controller.active_plan()->access_calendar) {
    const unsigned cell_index = intent.nci == first_nci ? 0U : 1U;
    ++recovered_intents[cell_index];
    auto& report      = recovered_preflight[cell_index];
    report.performed  = true;
    report.passed     = true;
    report.numerology = 1;
    if (intent.purpose == ntn_access_calendar_purpose::ssb_sib_paging ||
        intent.purpose == ntn_access_calendar_purpose::ssb_sib_paging_rar) {
      ++report.expected_ssb;
      ++report.matched_ssb;
      report.max_ssb_gap_slots = 160;
    } else if (intent.purpose == ntn_access_calendar_purpose::prach_ro) {
      ++report.expected_prach;
      ++report.matched_prach;
      report.max_prach_gap_slots = 1280;
    }
  }

  cu_cp_test_env_params env_params;
  env_params.ntn_onboard_position_plan                = source;
  env_params.ntn_recovered_calendar_intents_per_cell  = recovered_intents;
  env_params.ntn_recovered_calendar_preflight_reports = recovered_preflight;
  cu_cp_test_environment env(std::move(env_params));
  env.run_ng_setup();

  auto status = env.get_cu_cp()
                    .get_command_handler()
                    .get_ntn_command_handler()
                    .get_current_ntn_runtime_status()
                    .onboard_position_plan;
  EXPECT_EQ(status.recovery_stage, "reconciling");
  EXPECT_EQ(status.recovery_schedule_version, active_plan.schedule_version);
  EXPECT_EQ(status.active_schedule_version, 0U);
  EXPECT_EQ(status.clear_queue_depth, 1U);
  EXPECT_EQ(status.clear_queue_head_schedule_version, expired_update.schedule_version);
  EXPECT_EQ(status.clear_queue_head_calendar_hash, expired_calendar_hash);
  EXPECT_EQ(status.clear_queue_head_reason, "expired_deployment_recovered_after_restart");

  auto normalized_state = load_ntn_onboard_position_plan_state(source.state_file);
  ASSERT_TRUE(normalized_state.has_value()) << normalized_state.error();
  ASSERT_TRUE(normalized_state->has_value());
  ASSERT_TRUE(normalized_state->value().active.has_value());
  EXPECT_EQ(normalized_state->value().active->source.schedule_version, active_plan.schedule_version);
  EXPECT_FALSE(normalized_state->value().pending.has_value());
  ASSERT_EQ(normalized_state->value().outstanding_clears.size(), 1U);
  EXPECT_EQ(normalized_state->value().outstanding_clears[0].snapshot.source.schedule_version,
            expired_update.schedule_version);
  EXPECT_EQ(normalized_state->value().outstanding_clears[0].snapshot.calendar_hash, expired_calendar_hash);

  const auto du_idx = env.connect_new_du();
  ASSERT_TRUE(du_idx.has_value());
  std::vector<test_helpers::served_cell_item_info> served_cells(2);
  served_cells[0].nci = first_nci;
  served_cells[0].pci = shared_pci;
  served_cells[1].nci = second_nci;
  served_cells[1].pci = shared_pci;
  ASSERT_TRUE(env.run_f1_setup(du_idx.value(), int_to_gnb_du_id(0x17), served_cells));

  f1ap_message clear_request;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu_without_auto_response(
      du_idx.value(), clear_request, std::chrono::milliseconds{1500}));
  const auto& clear_asn1 = clear_request.pdu.init_msg().value.gnb_du_res_coordination_request();
  const auto  clear_update =
      decode_f1ap_ntn_access_calendar_update(clear_asn1->eutra_nr_cell_res_coordination_req_container);
  ASSERT_TRUE(clear_update.has_value());
  EXPECT_EQ(clear_update->operation, f1ap_ntn_access_calendar_operation::clear);
  EXPECT_EQ(clear_update->schedule_version, expired_update.schedule_version);
  EXPECT_EQ(clear_update->calendar_hash, expired_calendar_hash);
  EXPECT_NE(clear_update->calendar_hash, active_calendar_hash);

  env.respond_to_f1ap_resource_coordination_request(du_idx.value(), clear_request);
  f1ap_message query_request;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu_without_auto_response(
      du_idx.value(), query_request, std::chrono::milliseconds{1500}));
  const auto& query_asn1 = query_request.pdu.init_msg().value.gnb_du_res_coordination_request();
  const auto  query_update =
      decode_f1ap_ntn_access_calendar_update(query_asn1->eutra_nr_cell_res_coordination_req_container);
  ASSERT_TRUE(query_update.has_value());
  EXPECT_EQ(query_update->operation, f1ap_ntn_access_calendar_operation::query);
  EXPECT_EQ(query_update->schedule_version, active_plan.schedule_version);
  EXPECT_EQ(query_update->calendar_hash, active_calendar_hash);

  status = env.get_cu_cp()
               .get_command_handler()
               .get_ntn_command_handler()
               .get_current_ntn_runtime_status()
               .onboard_position_plan;
  EXPECT_EQ(status.active_schedule_version, 0U);
  EXPECT_EQ(status.clear_queue_depth, 0U);
  EXPECT_EQ(status.clear_queue_head_schedule_version, 0U);
  EXPECT_EQ(status.clear_queue_head_calendar_hash, "none");
  EXPECT_EQ(status.clear_queue_head_reason, "none");
  EXPECT_EQ(status.recovery_stage, "reconciling");

  env.respond_to_f1ap_resource_coordination_request(du_idx.value(), query_request);
  ASSERT_TRUE(env.tick_until(std::chrono::milliseconds{1000}, [&]() {
    return env.get_cu_cp()
                   .get_command_handler()
                   .get_ntn_command_handler()
                   .get_current_ntn_runtime_status()
                   .onboard_position_plan.active_schedule_version == active_plan.schedule_version;
  }));
  status = env.get_cu_cp()
               .get_command_handler()
               .get_ntn_command_handler()
               .get_current_ntn_runtime_status()
               .onboard_position_plan;
  EXPECT_EQ(status.recovery_stage, "reconciled");
  EXPECT_EQ(status.active_schedule_version, active_plan.schedule_version);
  EXPECT_EQ(status.active_calendar_hash, active_calendar_hash);
  EXPECT_EQ(status.deployment_stage, "applied");
  EXPECT_EQ(status.clear_queue_depth, 0U);
  EXPECT_EQ(status.clear_queue_head_schedule_version, 0U);
  EXPECT_EQ(status.clear_queue_head_calendar_hash, "none");
  EXPECT_EQ(status.clear_queue_head_reason, "none");
}

TEST(cu_cp_ntn_mobility_test, restart_recovery_fails_closed_when_an_onboard_nci_pci_cell_mapping_is_missing)
{
  persisted_recovery_calendar_test_data data = make_persisted_recovery_calendar_test_data(32, 42);
  temporary_plan_file_guard             plan_file_guard(data.plan_path);

  cu_cp_test_env_params env_params;
  env_params.ntn_onboard_position_plan                = data.source;
  env_params.ntn_recovered_calendar_intents_per_cell  = data.recovered_intents;
  env_params.ntn_recovered_calendar_preflight_reports = data.recovered_preflight;
  cu_cp_test_environment env(std::move(env_params));
  env.run_ng_setup();

  const auto du_idx = env.connect_new_du();
  ASSERT_TRUE(du_idx.has_value());
  std::vector<test_helpers::served_cell_item_info> served_cells(1);
  served_cells[0].nci = data.plan.onboard_cells[0].nci;
  served_cells[0].pci = data.plan.onboard_cells[0].pci;
  ASSERT_TRUE(env.run_f1_setup(du_idx.value(), int_to_gnb_du_id(0x12), served_cells));

  ASSERT_TRUE(env.tick_until(std::chrono::milliseconds{1500}, [&]() {
    return env.get_cu_cp()
               .get_command_handler()
               .get_ntn_command_handler()
               .get_current_ntn_runtime_status()
               .onboard_position_plan.recovery_stage == "failed";
  }));
  const auto status = env.get_cu_cp()
                          .get_command_handler()
                          .get_ntn_command_handler()
                          .get_current_ntn_runtime_status()
                          .onboard_position_plan;
  EXPECT_EQ(status.recovery_detail, "missing_nci_pci_du_cell_mapping");
  EXPECT_EQ(status.deployment_detail, "missing_nci_pci_du_cell_mapping");
  EXPECT_EQ(status.last_rejection, "du_reconciliation_failed");
  EXPECT_EQ(status.last_rejected_schedule_version, data.plan.schedule_version);
  EXPECT_EQ(status.active_schedule_version, 0U);
  EXPECT_NE(status.execution_evidence, "ssb_prach_software_gate_applied_no_position_or_rf_evidence");
}

TEST(cu_cp_ntn_mobility_test, restart_recovery_fails_closed_when_the_two_onboard_cells_resolve_to_different_dus)
{
  persisted_recovery_calendar_test_data data = make_persisted_recovery_calendar_test_data(33, 43);
  temporary_plan_file_guard             plan_file_guard(data.plan_path);

  cu_cp_test_env_params env_params;
  env_params.ntn_onboard_position_plan                = data.source;
  env_params.ntn_recovered_calendar_intents_per_cell  = data.recovered_intents;
  env_params.ntn_recovered_calendar_preflight_reports = data.recovered_preflight;
  cu_cp_test_environment env(std::move(env_params));
  env.run_ng_setup();

  const auto first_du  = env.connect_new_du();
  const auto second_du = env.connect_new_du();
  ASSERT_TRUE(first_du.has_value());
  ASSERT_TRUE(second_du.has_value());
  std::vector<test_helpers::served_cell_item_info> first_cells(1);
  first_cells[0].nci = data.plan.onboard_cells[0].nci;
  first_cells[0].pci = data.plan.onboard_cells[0].pci;
  std::vector<test_helpers::served_cell_item_info> second_cells(1);
  second_cells[0].nci = data.plan.onboard_cells[1].nci;
  second_cells[0].pci = data.plan.onboard_cells[1].pci;

  // Queue both setups before waiting for either response, so recovery observes the complete cross-DU mapping
  // atomically.
  env.get_du(first_du.value())
      .push_ul_pdu(test_helpers::generate_f1_setup_request(int_to_gnb_du_id(0x13), first_cells));
  env.get_du(second_du.value())
      .push_ul_pdu(test_helpers::generate_f1_setup_request(int_to_gnb_du_id(0x14), second_cells));
  f1ap_message ignored;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu(first_du.value(), ignored));
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu(second_du.value(), ignored));

  ASSERT_TRUE(env.tick_until(std::chrono::milliseconds{1500}, [&]() {
    return env.get_cu_cp()
               .get_command_handler()
               .get_ntn_command_handler()
               .get_current_ntn_runtime_status()
               .onboard_position_plan.recovery_stage == "failed";
  }));
  const auto status = env.get_cu_cp()
                          .get_command_handler()
                          .get_ntn_command_handler()
                          .get_current_ntn_runtime_status()
                          .onboard_position_plan;
  EXPECT_EQ(status.recovery_detail, "two_onboard_cells_resolve_to_different_dus");
  EXPECT_EQ(status.deployment_detail, "two_onboard_cells_resolve_to_different_dus");
  EXPECT_EQ(status.last_rejection, "du_reconciliation_failed");
  EXPECT_EQ(status.active_schedule_version, 0U);
}

TEST(cu_cp_ntn_mobility_test, corrupt_restart_state_fails_closed_without_overwriting_the_file_or_loading_the_plan)
{
  const nr_cell_identity first_nci  = make_default_env_nci(0);
  const nr_cell_identity second_nci = make_default_env_nci(1);
  constexpr pci_t        shared_pci = 101;
  const int64_t          now_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count();
  const int64_t activation_ms = ((now_ms + 1000 + 639) / 640) * 640;

  ntn_versioned_position_plan plan;
  plan.satellite_id         = "P01-S01";
  plan.catalog_version      = 31;
  plan.schedule_version     = 41;
  plan.valid_from           = std::chrono::system_clock::time_point{std::chrono::milliseconds{now_ms - 100}};
  plan.activation_epoch     = std::chrono::system_clock::time_point{std::chrono::milliseconds{activation_ms}};
  plan.valid_until          = std::chrono::system_clock::time_point{std::chrono::milliseconds{activation_ms + 3200}};
  plan.onboard_cells[0]     = {first_nci, shared_pci};
  plan.onboard_cells[1]     = {second_nci, shared_pci};
  plan.visible_l1_positions = {{"G000001", 10.0, 20.0}, {"G000002", 10.1, 20.1}};
  bind_onboard_planning_context(plan);
  plan.content_hash                     = compute_ntn_position_plan_content_hash(plan);
  const std::filesystem::path plan_path = write_onboard_position_plan_for_runtime_test(plan);
  temporary_plan_file_guard   plan_file_guard(plan_path);

  ntn_onboard_position_plan_source_config source;
  source.enabled              = true;
  source.du_execution_enabled = true;
  source.satellite_id         = plan.satellite_id;
  source.plan_json_file       = plan_path.string();
  source.cell_ncis            = {first_nci, second_nci};
  source.cell_pcis            = {shared_pci, shared_pci};
  bind_onboard_planning_context(source);
  const std::string corrupt_bytes = R"({"state_schema_version":1,"truncated":)";
  {
    std::ofstream state_output(source.state_file, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(state_output.is_open());
    state_output << corrupt_bytes;
  }

  cu_cp_test_env_params env_params;
  env_params.ntn_onboard_position_plan = source;
  cu_cp_test_environment env(std::move(env_params));
  const auto             status = env.get_cu_cp()
                                      .get_command_handler()
                                      .get_ntn_command_handler()
                                      .get_current_ntn_runtime_status()
                                      .onboard_position_plan;
  EXPECT_EQ(status.stage, "rejected");
  EXPECT_EQ(status.last_rejection, "state_file_corrupt");
  EXPECT_EQ(status.state_store_status, "corrupt");
  EXPECT_TRUE(status.state_write_blocked);
  EXPECT_EQ(status.state_generation, 0U);
  EXPECT_EQ(status.active_schedule_version, 0U);
  EXPECT_EQ(status.pending_schedule_version, 0U);
  EXPECT_FALSE(status.received_plan_present);

  std::ifstream state_input(source.state_file, std::ios::binary);
  ASSERT_TRUE(state_input.is_open());
  const std::string after((std::istreambuf_iterator<char>(state_input)), std::istreambuf_iterator<char>());
  EXPECT_EQ(after, corrupt_bytes);
}

TEST(cu_cp_ntn_mobility_test, missing_static_opportunity_preflight_is_rejected_before_ready_or_activation)
{
  const nr_cell_identity first_nci  = make_default_env_nci(0);
  const nr_cell_identity second_nci = make_default_env_nci(1);
  constexpr pci_t        shared_pci = 101;
  const int64_t          now_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count();
  const int64_t activation_ms = ((now_ms + 3000 + 639) / 640) * 640;

  ntn_versioned_position_plan plan;
  plan.satellite_id         = "P01-S01";
  plan.catalog_version      = 16;
  plan.schedule_version     = 26;
  plan.valid_from           = std::chrono::system_clock::time_point{std::chrono::milliseconds{now_ms - 100}};
  plan.activation_epoch     = std::chrono::system_clock::time_point{std::chrono::milliseconds{activation_ms}};
  plan.valid_until          = std::chrono::system_clock::time_point{std::chrono::milliseconds{activation_ms + 3200}};
  plan.onboard_cells[0]     = {first_nci, shared_pci};
  plan.onboard_cells[1]     = {second_nci, shared_pci};
  plan.visible_l1_positions = {{"G000001", 10.0, 20.0}, {"G000002", 10.1, 20.1}};
  bind_onboard_planning_context(plan);
  plan.content_hash = compute_ntn_position_plan_content_hash(plan);
  const std::filesystem::path plan_path = write_onboard_position_plan_for_runtime_test(plan);
  temporary_plan_file_guard   plan_file_guard(plan_path);

  cu_cp_test_env_params env_params;
  env_params.ntn_calendar_preflight_incomplete = true;
  ntn_onboard_position_plan_source_config source;
  source.enabled                       = true;
  source.du_execution_enabled          = true;
  source.du_prepare_guard              = std::chrono::milliseconds{100};
  source.satellite_id                  = plan.satellite_id;
  source.plan_json_file                = plan_path.string();
  source.cell_ncis                     = {first_nci, second_nci};
  source.cell_pcis                     = {shared_pci, shared_pci};
  bind_onboard_planning_context(source);
  env_params.ntn_onboard_position_plan = source;

  cu_cp_test_environment env(std::move(env_params));
  env.run_ng_setup();
  const auto du_idx = env.connect_new_du();
  ASSERT_TRUE(du_idx.has_value());
  std::vector<test_helpers::served_cell_item_info> served_cells(2);
  served_cells[0].nci = first_nci;
  served_cells[0].pci = shared_pci;
  served_cells[1].nci = second_nci;
  served_cells[1].pci = shared_pci;
  ASSERT_TRUE(env.run_f1_setup(du_idx.value(), int_to_gnb_du_id(0x11), served_cells));

  f1ap_message ignored;
  (void)env.wait_for_f1ap_tx_pdu(du_idx.value(), ignored, std::chrono::milliseconds{1200});

  const cu_cp_ntn_position_plan_status status = env.get_cu_cp()
                                                    .get_command_handler()
                                                    .get_ntn_command_handler()
                                                    .get_current_ntn_runtime_status()
                                                    .onboard_position_plan;
  EXPECT_EQ(status.stage, "rejected");
  EXPECT_EQ(status.deployment_stage, "rejected");
  EXPECT_EQ(status.last_rejection, "static_opportunity_mismatch");
  EXPECT_EQ(status.deployment_detail, "du_static_opportunity_preflight_missing_cell_0");
  EXPECT_EQ(status.active_schedule_version, 0U);
  EXPECT_EQ(status.static_preflight_schedule_version, plan.schedule_version);
  EXPECT_FALSE(status.static_opportunities[0].performed);
  EXPECT_TRUE(status.static_opportunities[1].performed);
  EXPECT_TRUE(status.static_opportunities[1].passed);
}

TEST(cu_cp_ntn_mobility_test, unsupported_static_opportunity_preflight_keeps_capability_reason_and_sends_no_clear)
{
  const nr_cell_identity first_nci  = make_default_env_nci(0);
  const nr_cell_identity second_nci = make_default_env_nci(1);
  constexpr pci_t        shared_pci = 101;
  const int64_t          now_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count();
  const int64_t activation_ms = ((now_ms + 3000 + 639) / 640) * 640;

  ntn_versioned_position_plan plan;
  plan.satellite_id         = "P01-S01";
  plan.catalog_version      = 17;
  plan.schedule_version     = 27;
  plan.valid_from           = std::chrono::system_clock::time_point{std::chrono::milliseconds{now_ms - 100}};
  plan.activation_epoch     = std::chrono::system_clock::time_point{std::chrono::milliseconds{activation_ms}};
  plan.valid_until          = std::chrono::system_clock::time_point{std::chrono::milliseconds{activation_ms + 3200}};
  plan.onboard_cells[0]     = {first_nci, shared_pci};
  plan.onboard_cells[1]     = {second_nci, shared_pci};
  plan.visible_l1_positions = {{"G000001", 10.0, 20.0}, {"G000002", 10.1, 20.1}};
  bind_onboard_planning_context(plan);
  plan.content_hash = compute_ntn_position_plan_content_hash(plan);
  const std::filesystem::path plan_path = write_onboard_position_plan_for_runtime_test(plan);
  temporary_plan_file_guard   plan_file_guard(plan_path);

  cu_cp_test_env_params env_params;
  env_params.ntn_calendar_preflight_unsupported = true;
  ntn_onboard_position_plan_source_config source;
  source.enabled                       = true;
  source.du_execution_enabled          = true;
  source.du_prepare_guard              = std::chrono::milliseconds{100};
  source.satellite_id                  = plan.satellite_id;
  source.plan_json_file                = plan_path.string();
  source.cell_ncis                     = {first_nci, second_nci};
  source.cell_pcis                     = {shared_pci, shared_pci};
  bind_onboard_planning_context(source);
  env_params.ntn_onboard_position_plan = source;

  cu_cp_test_environment env(std::move(env_params));
  env.run_ng_setup();
  const auto du_idx = env.connect_new_du();
  ASSERT_TRUE(du_idx.has_value());
  std::vector<test_helpers::served_cell_item_info> served_cells(2);
  served_cells[0].nci = first_nci;
  served_cells[0].pci = shared_pci;
  served_cells[1].nci = second_nci;
  served_cells[1].pci = shared_pci;
  ASSERT_TRUE(env.run_f1_setup(du_idx.value(), int_to_gnb_du_id(0x11), served_cells));

  f1ap_message ignored;
  (void)env.wait_for_f1ap_tx_pdu(du_idx.value(), ignored, std::chrono::milliseconds{1200});

  const cu_cp_ntn_position_plan_status status = env.get_cu_cp()
                                                    .get_command_handler()
                                                    .get_ntn_command_handler()
                                                    .get_current_ntn_runtime_status()
                                                    .onboard_position_plan;
  EXPECT_EQ(status.stage, "rejected");
  EXPECT_EQ(status.deployment_stage, "unsupported");
  EXPECT_EQ(status.last_rejection, "execution_unsupported");
  EXPECT_EQ(status.deployment_detail, "scheduler_preflight_not_performed");
  EXPECT_EQ(status.active_schedule_version, 0U);
  EXPECT_EQ(env.nof_ntn_calendar_clear_requests(), 0U);
}

TEST(cu_cp_ntn_mobility_test, query_with_incomplete_calendar_feedback_is_rejected_before_activation)
{
  const nr_cell_identity first_nci  = make_default_env_nci(0);
  const nr_cell_identity second_nci = make_default_env_nci(1);
  constexpr pci_t        shared_pci = 101;
  const int64_t          now_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count();
  const int64_t activation_ms = ((now_ms + 3000 + 639) / 640) * 640;

  ntn_versioned_position_plan plan;
  plan.satellite_id         = "P01-S01";
  plan.catalog_version      = 13;
  plan.schedule_version     = 23;
  plan.valid_from           = std::chrono::system_clock::time_point{std::chrono::milliseconds{now_ms - 100}};
  plan.activation_epoch     = std::chrono::system_clock::time_point{std::chrono::milliseconds{activation_ms}};
  plan.valid_until          = std::chrono::system_clock::time_point{std::chrono::milliseconds{activation_ms + 3200}};
  plan.onboard_cells[0]     = {first_nci, shared_pci};
  plan.onboard_cells[1]     = {second_nci, shared_pci};
  plan.visible_l1_positions = {{"G000001", 10.0, 20.0}, {"G000002", 10.1, 20.1}};
  bind_onboard_planning_context(plan);
  plan.content_hash         = compute_ntn_position_plan_content_hash(plan);
  const std::filesystem::path plan_path = write_onboard_position_plan_for_runtime_test(plan);
  temporary_plan_file_guard   plan_file_guard(plan_path);

  cu_cp_test_env_params env_params;
  env_params.ntn_calendar_query_reports_zero_intents = true;
  ntn_onboard_position_plan_source_config source;
  source.enabled                       = true;
  source.du_execution_enabled          = true;
  source.du_prepare_guard              = std::chrono::milliseconds{100};
  source.satellite_id                  = plan.satellite_id;
  source.plan_json_file                = plan_path.string();
  source.cell_ncis                     = {first_nci, second_nci};
  source.cell_pcis                     = {shared_pci, shared_pci};
  bind_onboard_planning_context(source);
  env_params.ntn_onboard_position_plan = source;

  cu_cp_test_environment env(std::move(env_params));
  env.run_ng_setup();
  const auto du_idx = env.connect_new_du();
  ASSERT_TRUE(du_idx.has_value());
  std::vector<test_helpers::served_cell_item_info> served_cells(2);
  served_cells[0].nci = first_nci;
  served_cells[0].pci = shared_pci;
  served_cells[1].nci = second_nci;
  served_cells[1].pci = shared_pci;
  ASSERT_TRUE(env.run_f1_setup(du_idx.value(), int_to_gnb_du_id(0x11), served_cells));

  f1ap_message ignored;
  (void)env.wait_for_f1ap_tx_pdu(du_idx.value(), ignored, std::chrono::milliseconds{1200});

  const cu_cp_ntn_position_plan_status status = env.get_cu_cp()
                                                    .get_command_handler()
                                                    .get_ntn_command_handler()
                                                    .get_current_ntn_runtime_status()
                                                    .onboard_position_plan;
  EXPECT_EQ(status.stage, "rejected");
  EXPECT_EQ(status.deployment_stage, "rejected");
  EXPECT_EQ(status.last_rejection, "du_prepare_rejected");
  EXPECT_EQ(status.deployment_detail, "du_query_accepted_intent_count_mismatch");
  EXPECT_EQ(status.active_schedule_version, 0U);
}

TEST(cu_cp_ntn_mobility_test, ready_query_feedback_is_not_rolled_back_by_late_rejected_prepare_response)
{
  const nr_cell_identity first_nci  = make_default_env_nci(0);
  const nr_cell_identity second_nci = make_default_env_nci(1);
  constexpr pci_t        shared_pci = 101;
  const int64_t          now_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count();
  const int64_t activation_ms = ((now_ms + 4000 + 639) / 640) * 640;

  ntn_versioned_position_plan plan;
  plan.satellite_id         = "P01-S01";
  plan.catalog_version      = 14;
  plan.schedule_version     = 24;
  plan.valid_from           = std::chrono::system_clock::time_point{std::chrono::milliseconds{now_ms - 100}};
  plan.activation_epoch     = std::chrono::system_clock::time_point{std::chrono::milliseconds{activation_ms}};
  plan.valid_until          = std::chrono::system_clock::time_point{std::chrono::milliseconds{activation_ms + 5000}};
  plan.onboard_cells[0]     = {first_nci, shared_pci};
  plan.onboard_cells[1]     = {second_nci, shared_pci};
  plan.visible_l1_positions = {{"G000001", 10.0, 20.0}, {"G000002", 10.1, 20.1}};
  bind_onboard_planning_context(plan);
  plan.content_hash         = compute_ntn_position_plan_content_hash(plan);
  const std::filesystem::path plan_path = write_onboard_position_plan_for_runtime_test(plan);
  temporary_plan_file_guard   plan_file_guard(plan_path);

  cu_cp_test_env_params env_params;
  env_params.ntn_calendar_prepare_rejects = true;
  ntn_onboard_position_plan_source_config source;
  source.enabled                       = true;
  source.du_execution_enabled          = true;
  source.du_prepare_guard              = std::chrono::milliseconds{1000};
  source.satellite_id                  = plan.satellite_id;
  source.plan_json_file                = plan_path.string();
  source.cell_ncis                     = {first_nci, second_nci};
  source.cell_pcis                     = {shared_pci, shared_pci};
  bind_onboard_planning_context(source);
  env_params.ntn_onboard_position_plan = source;

  cu_cp_test_environment env(std::move(env_params));
  env.run_ng_setup();
  const auto du_idx = env.connect_new_du();
  ASSERT_TRUE(du_idx.has_value());
  std::vector<test_helpers::served_cell_item_info> served_cells(2);
  served_cells[0].nci = first_nci;
  served_cells[0].pci = shared_pci;
  served_cells[1].nci = second_nci;
  served_cells[1].pci = shared_pci;
  ASSERT_TRUE(env.run_f1_setup(du_idx.value(), int_to_gnb_du_id(0x11), served_cells));

  f1ap_message prepare_request;
  ASSERT_TRUE(
      env.wait_for_f1ap_tx_pdu_without_auto_response(du_idx.value(), prepare_request, std::chrono::milliseconds{1200}));
  const auto& prepare_asn1 = prepare_request.pdu.init_msg().value.gnb_du_res_coordination_request();
  const auto  prepare_update =
      decode_f1ap_ntn_access_calendar_update(prepare_asn1->eutra_nr_cell_res_coordination_req_container);
  ASSERT_TRUE(prepare_update.has_value());
  ASSERT_EQ(prepare_update->operation, f1ap_ntn_access_calendar_operation::prepare);

  f1ap_message query_request;
  ASSERT_TRUE(
      env.wait_for_f1ap_tx_pdu_without_auto_response(du_idx.value(), query_request, std::chrono::milliseconds{800}));
  const auto& query_asn1 = query_request.pdu.init_msg().value.gnb_du_res_coordination_request();
  const auto  query_update =
      decode_f1ap_ntn_access_calendar_update(query_asn1->eutra_nr_cell_res_coordination_req_container);
  ASSERT_TRUE(query_update.has_value());
  ASSERT_EQ(query_update->operation, f1ap_ntn_access_calendar_operation::query);

  env.respond_to_f1ap_resource_coordination_request(du_idx.value(), query_request);
  ASSERT_TRUE(env.tick_until(std::chrono::milliseconds{500}, [&]() {
    return env.get_cu_cp()
               .get_command_handler()
               .get_ntn_command_handler()
               .get_current_ntn_runtime_status()
               .onboard_position_plan.deployment_stage == "ready";
  }));

  env.respond_to_f1ap_resource_coordination_request(du_idx.value(), prepare_request);
  (void)env.tick_until(std::chrono::milliseconds{100}, []() { return false; });

  const auto status = env.get_cu_cp()
                          .get_command_handler()
                          .get_ntn_command_handler()
                          .get_current_ntn_runtime_status()
                          .onboard_position_plan;
  EXPECT_EQ(status.stage, "pending");
  EXPECT_EQ(status.deployment_stage, "ready");
  EXPECT_EQ(status.pending_schedule_version, plan.schedule_version);
  EXPECT_EQ(status.active_schedule_version, 0U);
  EXPECT_EQ(status.last_rejection, "none");
}

TEST(cu_cp_ntn_mobility_test, late_ready_prepare_query_does_not_timeout_plan_that_is_already_ready)
{
  const nr_cell_identity first_nci  = make_default_env_nci(0);
  const nr_cell_identity second_nci = make_default_env_nci(1);
  constexpr pci_t        shared_pci = 101;
  const int64_t          now_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count();
  const int64_t     activation_ms    = ((now_ms + 3000 + 639) / 640) * 640;
  constexpr int64_t prepare_guard_ms = 1500;

  ntn_versioned_position_plan plan;
  plan.satellite_id         = "P01-S01";
  plan.catalog_version      = 15;
  plan.schedule_version     = 25;
  plan.valid_from           = std::chrono::system_clock::time_point{std::chrono::milliseconds{now_ms - 100}};
  plan.activation_epoch     = std::chrono::system_clock::time_point{std::chrono::milliseconds{activation_ms}};
  plan.valid_until          = std::chrono::system_clock::time_point{std::chrono::milliseconds{activation_ms + 5000}};
  plan.onboard_cells[0]     = {first_nci, shared_pci};
  plan.onboard_cells[1]     = {second_nci, shared_pci};
  plan.visible_l1_positions = {{"G000001", 10.0, 20.0}, {"G000002", 10.1, 20.1}};
  bind_onboard_planning_context(plan);
  plan.content_hash         = compute_ntn_position_plan_content_hash(plan);
  const std::filesystem::path plan_path = write_onboard_position_plan_for_runtime_test(plan);
  temporary_plan_file_guard   plan_file_guard(plan_path);

  cu_cp_test_env_params env_params;
  env_params.ntn_calendar_prepare_reports_ready = true;
  ntn_onboard_position_plan_source_config source;
  source.enabled                       = true;
  source.du_execution_enabled          = true;
  source.du_prepare_guard              = std::chrono::milliseconds{prepare_guard_ms};
  source.satellite_id                  = plan.satellite_id;
  source.plan_json_file                = plan_path.string();
  source.cell_ncis                     = {first_nci, second_nci};
  source.cell_pcis                     = {shared_pci, shared_pci};
  bind_onboard_planning_context(source);
  env_params.ntn_onboard_position_plan = source;

  cu_cp_test_environment env(std::move(env_params));
  env.run_ng_setup();
  const auto du_idx = env.connect_new_du();
  ASSERT_TRUE(du_idx.has_value());
  std::vector<test_helpers::served_cell_item_info> served_cells(2);
  served_cells[0].nci = first_nci;
  served_cells[0].pci = shared_pci;
  served_cells[1].nci = second_nci;
  served_cells[1].pci = shared_pci;
  ASSERT_TRUE(env.run_f1_setup(du_idx.value(), int_to_gnb_du_id(0x11), served_cells));

  f1ap_message prepare_request;
  ASSERT_TRUE(
      env.wait_for_f1ap_tx_pdu_without_auto_response(du_idx.value(), prepare_request, std::chrono::milliseconds{1200}));
  const auto& prepare_asn1 = prepare_request.pdu.init_msg().value.gnb_du_res_coordination_request();
  const auto  prepare_update =
      decode_f1ap_ntn_access_calendar_update(prepare_asn1->eutra_nr_cell_res_coordination_req_container);
  ASSERT_TRUE(prepare_update.has_value());
  ASSERT_EQ(prepare_update->operation, f1ap_ntn_access_calendar_operation::prepare);

  f1ap_message query_request;
  ASSERT_TRUE(
      env.wait_for_f1ap_tx_pdu_without_auto_response(du_idx.value(), query_request, std::chrono::milliseconds{800}));
  const auto& query_asn1 = query_request.pdu.init_msg().value.gnb_du_res_coordination_request();
  const auto  query_update =
      decode_f1ap_ntn_access_calendar_update(query_asn1->eutra_nr_cell_res_coordination_req_container);
  ASSERT_TRUE(query_update.has_value());
  ASSERT_EQ(query_update->operation, f1ap_ntn_access_calendar_operation::query);

  env.respond_to_f1ap_resource_coordination_request(du_idx.value(), prepare_request);
  ASSERT_TRUE(env.tick_until(std::chrono::milliseconds{500}, [&]() {
    return env.get_cu_cp()
               .get_command_handler()
               .get_ntn_command_handler()
               .get_current_ntn_runtime_status()
               .onboard_position_plan.deployment_stage == "ready";
  }));

  const auto prepare_deadline =
      std::chrono::system_clock::time_point{std::chrono::milliseconds{activation_ms - prepare_guard_ms}};
  if (std::chrono::system_clock::now() <= prepare_deadline) {
    std::this_thread::sleep_until(prepare_deadline + std::chrono::milliseconds{20});
  }
  env.respond_to_f1ap_resource_coordination_request(du_idx.value(), query_request);
  (void)env.tick_until(std::chrono::milliseconds{100}, []() { return false; });

  const auto status = env.get_cu_cp()
                          .get_command_handler()
                          .get_ntn_command_handler()
                          .get_current_ntn_runtime_status()
                          .onboard_position_plan;
  EXPECT_EQ(status.stage, "pending");
  EXPECT_EQ(status.deployment_stage, "ready");
  EXPECT_EQ(status.pending_schedule_version, plan.schedule_version);
  EXPECT_EQ(status.active_schedule_version, 0U);
  EXPECT_EQ(status.last_rejection, "none");
}

TEST(cu_cp_ntn_mobility_test, du_ready_response_after_prepare_guard_is_rejected_and_never_activates)
{
  const nr_cell_identity first_nci  = make_default_env_nci(0);
  const nr_cell_identity second_nci = make_default_env_nci(1);
  constexpr pci_t        shared_pci = 101;
  const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
  const int64_t activation_ms = ((now_ms + 2500 + 639) / 640) * 640;
  constexpr int64_t prepare_guard_ms = 1000;

  ntn_versioned_position_plan plan;
  plan.satellite_id         = "P01-S01";
  plan.catalog_version  = 11;
  plan.schedule_version = 21;
  plan.valid_from       = std::chrono::system_clock::time_point{std::chrono::milliseconds{now_ms - 100}};
  plan.activation_epoch = std::chrono::system_clock::time_point{std::chrono::milliseconds{activation_ms}};
  plan.valid_until      = std::chrono::system_clock::time_point{std::chrono::milliseconds{activation_ms + 3200}};
  plan.onboard_cells[0] = {first_nci, shared_pci};
  plan.onboard_cells[1] = {second_nci, shared_pci};
  plan.visible_l1_positions = {{"G000001", 10.0, 20.0}, {"G000002", 10.1, 20.1}};
  bind_onboard_planning_context(plan);
  plan.content_hash          = compute_ntn_position_plan_content_hash(plan);
  const std::filesystem::path plan_path = write_onboard_position_plan_for_runtime_test(plan);
  temporary_plan_file_guard   plan_file_guard(plan_path);

  cu_cp_test_env_params env_params;
  ntn_onboard_position_plan_source_config source;
  source.enabled              = true;
  source.du_execution_enabled = true;
  source.du_prepare_guard     = std::chrono::milliseconds{prepare_guard_ms};
  source.satellite_id         = plan.satellite_id;
  source.plan_json_file       = plan_path.string();
  source.cell_ncis            = {first_nci, second_nci};
  source.cell_pcis            = {shared_pci, shared_pci};
  bind_onboard_planning_context(source);
  env_params.ntn_onboard_position_plan = source;

  cu_cp_test_environment env(std::move(env_params));
  env.run_ng_setup();
  const auto du_idx = env.connect_new_du();
  ASSERT_TRUE(du_idx.has_value());
  std::vector<test_helpers::served_cell_item_info> served_cells(2);
  served_cells[0].nci = first_nci;
  served_cells[0].pci = shared_pci;
  served_cells[1].nci = second_nci;
  served_cells[1].pci = shared_pci;
  ASSERT_TRUE(env.run_f1_setup(du_idx.value(), int_to_gnb_du_id(0x11), served_cells));

  f1ap_message prepare_request;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu_without_auto_response(
      du_idx.value(), prepare_request, std::chrono::milliseconds{1200}));
  ASSERT_EQ(prepare_request.pdu.type().value, asn1::f1ap::f1ap_pdu_c::types_opts::init_msg);
  const auto& asn1_request = prepare_request.pdu.init_msg().value.gnb_du_res_coordination_request();
  const auto  decoded_update =
      decode_f1ap_ntn_access_calendar_update(asn1_request->eutra_nr_cell_res_coordination_req_container);
  ASSERT_TRUE(decoded_update.has_value());
  ASSERT_EQ(decoded_update->operation, f1ap_ntn_access_calendar_operation::prepare);

  const auto prepare_deadline =
      std::chrono::system_clock::time_point{std::chrono::milliseconds{activation_ms - prepare_guard_ms}};
  if (std::chrono::system_clock::now() <= prepare_deadline) {
    std::this_thread::sleep_until(prepare_deadline + std::chrono::milliseconds{20});
  }
  env.respond_to_f1ap_resource_coordination_request(du_idx.value(), prepare_request);

  ASSERT_TRUE(env.tick_until(std::chrono::milliseconds{500}, [&]() {
    const auto status = env.get_cu_cp()
                            .get_command_handler()
                            .get_ntn_command_handler()
                            .get_current_ntn_runtime_status()
                            .onboard_position_plan;
    return status.last_rejection == "du_prepare_timeout" && status.active_schedule_version == 0;
  }));
  const auto status = env.get_cu_cp()
                          .get_command_handler()
                          .get_ntn_command_handler()
                          .get_current_ntn_runtime_status()
                          .onboard_position_plan;
  EXPECT_EQ(status.stage, "rejected");
  EXPECT_EQ(status.deployment_stage, "rejected");
  EXPECT_EQ(status.active_schedule_version, 0U);
  EXPECT_EQ(status.execution_evidence, "intent_or_control_plane_only");
}

TEST(cu_cp_ntn_mobility_test, lost_application_query_does_not_block_apply_deadline_rollback_clear)
{
  const nr_cell_identity first_nci  = make_default_env_nci(0);
  const nr_cell_identity second_nci = make_default_env_nci(1);
  constexpr pci_t        shared_pci = 101;
  const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
  const int64_t activation_ms = ((now_ms + 1200 + 639) / 640) * 640;

  ntn_versioned_position_plan plan;
  plan.satellite_id         = "P01-S01";
  plan.catalog_version  = 12;
  plan.schedule_version = 22;
  plan.valid_from       = std::chrono::system_clock::time_point{std::chrono::milliseconds{now_ms - 100}};
  plan.activation_epoch = std::chrono::system_clock::time_point{std::chrono::milliseconds{activation_ms}};
  plan.valid_until      = std::chrono::system_clock::time_point{std::chrono::milliseconds{activation_ms + 3200}};
  plan.onboard_cells[0] = {first_nci, shared_pci};
  plan.onboard_cells[1] = {second_nci, shared_pci};
  plan.visible_l1_positions = {{"G000001", 10.0, 20.0}, {"G000002", 10.1, 20.1}};
  bind_onboard_planning_context(plan);
  plan.content_hash          = compute_ntn_position_plan_content_hash(plan);
  const std::filesystem::path plan_path = write_onboard_position_plan_for_runtime_test(plan);
  temporary_plan_file_guard   plan_file_guard(plan_path);

  cu_cp_test_env_params env_params;
  env_params.ntn_calendar_drop_query_responses = true;
  env_params.ntn_calendar_prepare_reports_ready = true;
  ntn_onboard_position_plan_source_config source;
  source.enabled              = true;
  source.du_execution_enabled = true;
  source.du_prepare_guard     = std::chrono::milliseconds{100};
  source.du_apply_timeout     = std::chrono::milliseconds{200};
  source.satellite_id         = plan.satellite_id;
  source.plan_json_file       = plan_path.string();
  source.cell_ncis            = {first_nci, second_nci};
  source.cell_pcis            = {shared_pci, shared_pci};
  bind_onboard_planning_context(source);
  env_params.ntn_onboard_position_plan = source;

  cu_cp_test_environment env(std::move(env_params));
  env.run_ng_setup();
  const auto du_idx = env.connect_new_du();
  ASSERT_TRUE(du_idx.has_value());
  std::vector<test_helpers::served_cell_item_info> served_cells(2);
  served_cells[0].nci = first_nci;
  served_cells[0].pci = shared_pci;
  served_cells[1].nci = second_nci;
  served_cells[1].pci = shared_pci;
  ASSERT_TRUE(env.run_f1_setup(du_idx.value(), int_to_gnb_du_id(0x11), served_cells));

  f1ap_message ignored;
  (void)env.wait_for_f1ap_tx_pdu(du_idx.value(), ignored, std::chrono::milliseconds{2600});

  const auto status = env.get_cu_cp()
                          .get_command_handler()
                          .get_ntn_command_handler()
                          .get_current_ntn_runtime_status()
                          .onboard_position_plan;
  EXPECT_EQ(status.stage, "rejected");
  EXPECT_EQ(status.deployment_stage, "rejected");
  EXPECT_EQ(status.last_rejection, "du_activation_not_applied");
  EXPECT_EQ(status.active_schedule_version, 0U);
  EXPECT_EQ(status.execution_evidence, "intent_or_control_plane_only");
  EXPECT_EQ(status.clear_queue_depth, 0U);
}

TEST(cu_cp_ntn_mobility_test, cu_cp_dl_nrppa_transport_updates_runtime_counters)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<connected_ngap_ntn_ue> ue = connect_ngap_ntn_ue(env);
  ASSERT_TRUE(ue.has_value());

  cu_cp_impl_interface* cu_cp_impl = get_cu_cp_impl(env);
  cu_cp_impl->get_cu_cp_ngap_handler().handle_dl_ue_associated_nrppa_transport_pdu(
      ue->ue_index, byte_buffer::create({0xde, 0xad, 0xbe, 0xef}).value());
  cu_cp_impl->get_cu_cp_ngap_handler().handle_dl_non_ue_associated_nrppa_transport_pdu(
      amf_index_t::min, byte_buffer::create({0xca, 0xfe}).value());

  const cu_cp_ntn_runtime_status runtime =
      env.get_cu_cp().get_command_handler().get_ntn_command_handler().get_current_ntn_runtime_status();
  ASSERT_EQ(runtime.nof_ntn_nrppa_dl_ue_received, 1U);
  ASSERT_EQ(runtime.nof_ntn_nrppa_dl_ue_forwarded, 1U);
  ASSERT_EQ(runtime.nof_ntn_nrppa_dl_ue_dropped, 0U);
  ASSERT_EQ(runtime.nof_ntn_nrppa_dl_non_ue_received, 1U);
  ASSERT_EQ(runtime.nof_ntn_nrppa_dl_non_ue_forwarded, 1U);
  ASSERT_EQ(runtime.nof_ntn_nrppa_dl_non_ue_dropped, 0U);
  ASSERT_EQ(runtime.nof_ntn_nrppa_unsupported_procedures, 2U);
  ASSERT_EQ(runtime.nof_ntn_nrppa_standard_decode_failure, 2U);
  ASSERT_EQ(runtime.nof_ntn_nrppa_minimal_fallback_decodes, 0U);
  ASSERT_EQ(runtime.last_ntn_nrppa_trp_reason, "standard_nrppa_truncated_header");
  ASSERT_EQ(runtime.last_ntn_nrppa_standard_decode_reason, "standard_nrppa_truncated_header");
  ASSERT_EQ(runtime.last_ntn_nrppa_dropped_reason, "none");
}

TEST(cu_cp_ntn_mobility_test, cu_cp_standard_trp_information_transport_updates_codec_runtime_counters)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  params.ntn_location_mobility->served_beam_min_elevation_deg = -90.0;
  params.ntn_location_mobility->max_nof_served_beams          = 1;
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();
  ASSERT_TRUE(connect_du_for_ntn_beams(env).has_value());

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  trp_information_request_t request;
  request.transaction_id = 23;
  request.trp_info_type_list_trp_req.push_back(trp_information_type_item_t::nr_pci);
  request.trp_info_type_list_trp_req.push_back(trp_information_type_item_t::ng_ran_cgi);

  get_cu_cp_impl(env)->get_cu_cp_ngap_handler().handle_dl_non_ue_associated_nrppa_transport_pdu(
      amf_index_t::min, encode_nrppa_standard_trp_information_request(request));

  ASSERT_TRUE(env.tick_until(std::chrono::milliseconds{1000}, [&ntn_handler]() {
    return ntn_handler.get_current_ntn_runtime_status().nof_ntn_nrppa_standard_encode_responses == 1U;
  }));

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  ASSERT_EQ(runtime.nof_ntn_nrppa_dl_non_ue_received, 1U);
  ASSERT_EQ(runtime.nof_ntn_nrppa_dl_non_ue_forwarded, 1U);
  ASSERT_EQ(runtime.nof_ntn_nrppa_standard_decode_success, 1U);
  ASSERT_EQ(runtime.nof_ntn_nrppa_standard_decode_failure, 0U);
  ASSERT_EQ(runtime.nof_ntn_nrppa_standard_encode_responses, 1U);
  ASSERT_EQ(runtime.nof_ntn_nrppa_standard_encode_failures, 0U);
  ASSERT_EQ(runtime.nof_ntn_nrppa_minimal_fallback_decodes, 0U);
  ASSERT_EQ(runtime.last_ntn_nrppa_standard_decode_reason, "trp_information_response");
  ASSERT_EQ(runtime.nof_ntn_nrppa_trp_requests_received, 1U);
  ASSERT_EQ(runtime.nof_ntn_nrppa_trp_requests_decoded, 1U);
  ASSERT_EQ(runtime.nof_ntn_nrppa_trp_responses_sent, 1U);
  ASSERT_EQ(runtime.nof_ntn_nrppa_unsupported_procedures, 0U);
  ASSERT_EQ(runtime.nof_ntn_nrppa_ul_non_ue_received, 1U);
  ASSERT_EQ(runtime.nof_ntn_nrppa_ul_non_ue_sent, 1U);
}

TEST(cu_cp_ntn_mobility_test, cu_cp_trp_information_response_uses_current_ntn_beam_inventory)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  params.ntn_location_mobility->served_beam_min_elevation_deg = -90.0;
  params.ntn_location_mobility->max_nof_served_beams          = 2;
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();
  ASSERT_TRUE(connect_du_for_ntn_beams(env).has_value());

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  trp_information_request_t request;
  request.transaction_id = 19;
  request.trp_info_type_list_trp_req.push_back(trp_information_type_item_t::nr_pci);
  request.trp_info_type_list_trp_req.push_back(trp_information_type_item_t::ng_ran_cgi);
  request.trp_info_type_list_trp_req.push_back(trp_information_type_item_t::arfcn);
  request.trp_info_type_list_trp_req.push_back(trp_information_type_item_t::geo_coord);
  request.trp_info_type_list_trp_req.push_back(trp_information_type_item_t::trp_type);

  async_task<trp_information_cu_cp_response_t> task =
      get_cu_cp_impl(env)->get_cu_cp_nrppa_handler().handle_trp_information_request(request);
  lazy_task_launcher<trp_information_cu_cp_response_t> launcher(task);
  ASSERT_TRUE(task.ready());
  const trp_information_cu_cp_response_t response = task.get();

  ASSERT_EQ(response.transaction_id, 19);
  ASSERT_EQ(response.trp_info_responses.size(), 1);
  const auto& trp_items = response.trp_info_responses.begin()->second.trp_info_list_trp_resp;
  ASSERT_EQ(trp_items.size(), 2);
  EXPECT_EQ(trp_items.front().trp_info.trp_id,
            uint_to_trp_id(static_cast<uint32_t>(make_default_env_nci(0).value() & 0xffffU)));
  const auto& first_item_responses = trp_items.front().trp_info.trp_info_type_resp_list;
  EXPECT_TRUE(std::any_of(first_item_responses.begin(), first_item_responses.end(), [](const auto& item) {
    return std::holds_alternative<pci_t>(item) && std::get<pci_t>(item) == 1;
  }));
  EXPECT_TRUE(std::any_of(first_item_responses.begin(), first_item_responses.end(), [](const auto& item) {
    return std::holds_alternative<nr_cell_global_id_t>(item) &&
           std::get<nr_cell_global_id_t>(item).nci == make_default_env_nci(0);
  }));
  EXPECT_TRUE(std::any_of(first_item_responses.begin(), first_item_responses.end(), [](const auto& item) {
    return std::holds_alternative<uint32_t>(item) && std::get<uint32_t>(item) == 620928;
  }));
  EXPECT_TRUE(std::any_of(first_item_responses.begin(), first_item_responses.end(), [](const auto& item) {
    return std::holds_alternative<geographical_coordinates_t>(item);
  }));
  EXPECT_TRUE(std::any_of(first_item_responses.begin(), first_item_responses.end(), [](const auto& item) {
    return std::holds_alternative<trp_type_t>(item) && std::get<trp_type_t>(item) == trp_type_t::trp;
  }));

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_nrppa_trp_requests_received, 1U);
  EXPECT_EQ(runtime.nof_ntn_nrppa_trp_requests_decoded, 1U);
  EXPECT_EQ(runtime.nof_ntn_nrppa_trp_responses_sent, 1U);
  EXPECT_EQ(runtime.nof_ntn_nrppa_trp_empty_results, 0U);
  EXPECT_EQ(runtime.last_ntn_nrppa_trp_reason, "responded");
}

TEST(cu_cp_ntn_mobility_test, cu_cp_positioning_information_request_forwards_to_f1ap_and_updates_runtime)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<connected_ngap_ntn_ue> ue = connect_ngap_ntn_ue(env);
  ASSERT_TRUE(ue.has_value());
  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(ue->du_idx, du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());

  positioning_information_request_t request;
  request.ue_index = ue->ue_index;
  async_task<expected<positioning_information_response_t, positioning_information_failure_t>> task =
      get_cu_cp_impl(env)->get_cu_cp_nrppa_handler().handle_positioning_information_request(request);
  lazy_task_launcher<expected<positioning_information_response_t, positioning_information_failure_t>> launcher(task);

  f1ap_message f1ap_pdu;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu(ue->du_idx, f1ap_pdu, std::chrono::milliseconds{1000}));
  ASSERT_EQ(f1ap_pdu.pdu.type().value, asn1::f1ap::f1ap_pdu_c::types_opts::init_msg);
  ASSERT_EQ(f1ap_pdu.pdu.init_msg().value.type().value,
            asn1::f1ap::f1ap_elem_procs_o::init_msg_c::types_opts::positioning_info_request);

  env.get_du(ue->du_idx).push_ul_pdu(
      test_helpers::generate_positioning_information_response(du_ue_id, ue_ctx->cu_ue_id.value()));
  ASSERT_TRUE(env.tick_until(std::chrono::milliseconds{1000}, [&task]() { return task.ready(); }));
  ASSERT_TRUE(task.get().has_value());

  const cu_cp_ntn_runtime_status runtime =
      env.get_cu_cp().get_command_handler().get_ntn_command_handler().get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_nrppa_positioning_info_requests_received, 1U);
  EXPECT_EQ(runtime.nof_ntn_nrppa_positioning_info_requests_decoded, 1U);
  EXPECT_EQ(runtime.nof_ntn_nrppa_positioning_info_requests_forwarded, 1U);
  EXPECT_EQ(runtime.nof_ntn_nrppa_positioning_info_responses_sent, 1U);
  EXPECT_EQ(runtime.nof_ntn_nrppa_positioning_info_failures_sent, 0U);
  EXPECT_EQ(runtime.nof_ntn_nrppa_positioning_info_dropped, 0U);
  EXPECT_EQ(runtime.last_ntn_nrppa_positioning_info_reason, "responded");
}

TEST(cu_cp_ntn_mobility_test, cu_cp_nrppa_measurement_request_forwards_to_f1ap_and_updates_runtime)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<connected_ngap_ntn_ue> ue = connect_ngap_ntn_ue(env);
  ASSERT_TRUE(ue.has_value());

  measurement_request_t request;
  request.ue_index    = ue->ue_index;
  request.lmf_meas_id = uint_to_lmf_meas_id(51);
  request.ran_meas_id = uint_to_ran_meas_id(52);
  request.trp_meas_request_list.push_back({uint_to_trp_id(0x101)});
  request.report_characteristics = report_characteristics_t::on_demand;
  request.trp_meas_quantities.push_back({trp_meas_quantities_item_t::ul_rtoa});

  async_task<expected<measurement_response_t, measurement_failure_t>> task =
      get_cu_cp_impl(env)->get_cu_cp_nrppa_handler().handle_positioning_measurement_request(request);
  lazy_task_launcher<expected<measurement_response_t, measurement_failure_t>> launcher(task);

  f1ap_message f1ap_pdu;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu(ue->du_idx, f1ap_pdu, std::chrono::milliseconds{1000}));
  ASSERT_EQ(f1ap_pdu.pdu.type().value, asn1::f1ap::f1ap_pdu_c::types_opts::init_msg);
  ASSERT_EQ(f1ap_pdu.pdu.init_msg().value.type().value,
            asn1::f1ap::f1ap_elem_procs_o::init_msg_c::types_opts::positioning_meas_request);
  const auto& asn1_request = f1ap_pdu.pdu.init_msg().value.positioning_meas_request();

  env.get_du(ue->du_idx)
      .push_ul_pdu(test_helpers::generate_positioning_measurement_response(uint_to_lmf_meas_id(asn1_request->lmf_meas_id),
                                                                           uint_to_ran_meas_id(asn1_request->ran_meas_id),
                                                                           {uint_to_trp_id(0x101)},
                                                                           asn1_request->transaction_id));
  ASSERT_TRUE(env.tick_until(std::chrono::milliseconds{1000}, [&task]() { return task.ready(); }));
  ASSERT_TRUE(task.get().has_value());

  const cu_cp_ntn_runtime_status runtime =
      env.get_cu_cp().get_command_handler().get_ntn_command_handler().get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_nrppa_measurement_requests_received, 1U);
  EXPECT_EQ(runtime.nof_ntn_nrppa_measurement_requests_decoded, 1U);
  EXPECT_EQ(runtime.nof_ntn_nrppa_measurement_requests_forwarded, 1U);
  EXPECT_EQ(runtime.nof_ntn_nrppa_measurement_responses_sent, 1U);
  EXPECT_EQ(runtime.nof_ntn_nrppa_measurement_failures_sent, 0U);
  EXPECT_EQ(runtime.nof_ntn_nrppa_measurement_dropped, 0U);
  EXPECT_EQ(runtime.last_ntn_nrppa_measurement_reason, "responded");
}

TEST(cu_cp_ntn_mobility_test, cu_cp_nrppa_activation_request_forwards_to_f1ap_and_updates_runtime)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<connected_ngap_ntn_ue> ue = connect_ngap_ntn_ue(env);
  ASSERT_TRUE(ue.has_value());
  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(ue->du_idx, du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());

  positioning_activation_request_t request;
  request.ue_index = ue->ue_index;
  request.srs_type = aperiodic_srs_t{true, std::nullopt};

  async_task<expected<positioning_activation_response_t, positioning_activation_failure_t>> task =
      get_cu_cp_impl(env)->get_cu_cp_nrppa_handler().handle_positioning_activation_request(request);
  lazy_task_launcher<expected<positioning_activation_response_t, positioning_activation_failure_t>> launcher(task);

  f1ap_message f1ap_pdu;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu(ue->du_idx, f1ap_pdu, std::chrono::milliseconds{1000}));
  ASSERT_EQ(f1ap_pdu.pdu.type().value, asn1::f1ap::f1ap_pdu_c::types_opts::init_msg);
  ASSERT_EQ(f1ap_pdu.pdu.init_msg().value.type().value,
            asn1::f1ap::f1ap_elem_procs_o::init_msg_c::types_opts::positioning_activation_request);

  env.get_du(ue->du_idx).push_ul_pdu(
      test_helpers::generate_positioning_activation_response(du_ue_id, ue_ctx->cu_ue_id.value()));
  ASSERT_TRUE(env.tick_until(std::chrono::milliseconds{1000}, [&task]() { return task.ready(); }));
  ASSERT_TRUE(task.get().has_value());

  const cu_cp_ntn_runtime_status runtime =
      env.get_cu_cp().get_command_handler().get_ntn_command_handler().get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_nrppa_activation_requests_received, 1U);
  EXPECT_EQ(runtime.nof_ntn_nrppa_activation_requests_decoded, 1U);
  EXPECT_EQ(runtime.nof_ntn_nrppa_activation_requests_forwarded, 1U);
  EXPECT_EQ(runtime.nof_ntn_nrppa_activation_responses_sent, 1U);
  EXPECT_EQ(runtime.nof_ntn_nrppa_activation_failures_sent, 0U);
  EXPECT_EQ(runtime.nof_ntn_nrppa_activation_dropped, 0U);
  EXPECT_EQ(runtime.last_ntn_nrppa_activation_reason, "responded");
}

TEST(cu_cp_ntn_mobility_test, cu_cp_nrppa_deactivation_request_forwards_to_f1ap_and_updates_runtime)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<connected_ngap_ntn_ue> ue = connect_ngap_ntn_ue(env);
  ASSERT_TRUE(ue.has_value());

  positioning_deactivation_request_t request;
  request.ue_index = ue->ue_index;

  async_task<expected<positioning_deactivation_response_t, positioning_deactivation_failure_t>> task =
      get_cu_cp_impl(env)->get_cu_cp_nrppa_handler().handle_positioning_deactivation_request(request);
  lazy_task_launcher<expected<positioning_deactivation_response_t, positioning_deactivation_failure_t>> launcher(task);

  f1ap_message f1ap_pdu;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu(ue->du_idx, f1ap_pdu, std::chrono::milliseconds{1000}));
  ASSERT_EQ(f1ap_pdu.pdu.type().value, asn1::f1ap::f1ap_pdu_c::types_opts::init_msg);
  ASSERT_EQ(f1ap_pdu.pdu.init_msg().value.type().value,
            asn1::f1ap::f1ap_elem_procs_o::init_msg_c::types_opts::positioning_deactivation);
  ASSERT_TRUE(task.ready());
  ASSERT_TRUE(task.get().has_value());

  const cu_cp_ntn_runtime_status runtime =
      env.get_cu_cp().get_command_handler().get_ntn_command_handler().get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_nrppa_deactivation_requests_received, 1U);
  EXPECT_EQ(runtime.nof_ntn_nrppa_deactivation_requests_decoded, 1U);
  EXPECT_EQ(runtime.nof_ntn_nrppa_deactivation_requests_forwarded, 1U);
  EXPECT_EQ(runtime.nof_ntn_nrppa_deactivation_acks_sent, 1U);
  EXPECT_EQ(runtime.nof_ntn_nrppa_deactivation_failures_sent, 0U);
  EXPECT_EQ(runtime.nof_ntn_nrppa_deactivation_dropped, 0U);
  EXPECT_EQ(runtime.last_ntn_nrppa_deactivation_reason, "acked");
}

TEST(cu_cp_ntn_mobility_test, cu_cp_nrppa_assistance_control_forwards_to_matching_du_and_updates_runtime)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  ASSERT_TRUE(du_idx.has_value());

  const nr_cell_global_id_t cgi{plmn_identity::test_value(), make_default_env_nci(0)};
  positioning_assistance_information_control_request_t request;
  request.transaction_id = 55;
  request.pos_broadcast = positioning_assistance_broadcast_action::start;
  request.positioning_broadcast_cells.push_back(cgi);
  request.routing_id = byte_buffer::create({0xab}).value();

  async_task<expected<positioning_assistance_information_feedback_t, positioning_assistance_information_failure_t>>
      task = get_cu_cp_impl(env)->get_cu_cp_nrppa_handler().handle_positioning_assistance_information_control(request);
  lazy_task_launcher<expected<positioning_assistance_information_feedback_t,
                              positioning_assistance_information_failure_t>>
      launcher(task);

  f1ap_message f1ap_pdu;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu(du_idx.value(), f1ap_pdu, std::chrono::milliseconds{1000}));
  ASSERT_EQ(f1ap_pdu.pdu.type().value, asn1::f1ap::f1ap_pdu_c::types_opts::init_msg);
  ASSERT_EQ(f1ap_pdu.pdu.init_msg().value.type().value,
            asn1::f1ap::f1ap_elem_procs_o::init_msg_c::types_opts::positioning_assist_info_ctrl);
  ASSERT_EQ(f1ap_pdu.pdu.init_msg().value.positioning_assist_info_ctrl()->transaction_id, 55U);

  env.get_du(du_idx.value()).push_ul_pdu(generate_positioning_assistance_information_feedback(55, cgi));
  ASSERT_TRUE(env.tick_until(std::chrono::milliseconds{1000}, [&task]() { return task.ready(); }));
  ASSERT_TRUE(task.get().has_value());
  ASSERT_EQ(task.get().value().transaction_id, 55U);
  ASSERT_EQ(task.get().value().positioning_broadcast_cells.size(), 1U);

  const cu_cp_ntn_runtime_status runtime =
      env.get_cu_cp().get_command_handler().get_ntn_command_handler().get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_nrppa_assistance_control_requests_received, 1U);
  EXPECT_EQ(runtime.nof_ntn_nrppa_assistance_control_requests_decoded, 1U);
  EXPECT_EQ(runtime.nof_ntn_nrppa_assistance_control_requests_forwarded, 1U);
  EXPECT_EQ(runtime.nof_ntn_nrppa_assistance_control_feedbacks_sent, 1U);
  EXPECT_EQ(runtime.nof_ntn_nrppa_assistance_control_failures_sent, 0U);
  EXPECT_EQ(runtime.nof_ntn_nrppa_assistance_control_dropped, 0U);
  EXPECT_EQ(runtime.last_ntn_nrppa_assistance_control_reason, "responded");
}

TEST(cu_cp_ntn_mobility_test, cu_cp_ul_nrppa_transport_is_forwarded_to_matching_ngap)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<connected_ngap_ntn_ue> ue = connect_ngap_ntn_ue(env);
  ASSERT_TRUE(ue.has_value());

  cu_cp_impl_interface* cu_cp_impl = get_cu_cp_impl(env);
  const std::array<uint8_t, 3> expected_pdu = {0x01, 0x02, 0x03};
  cu_cp_impl->get_cu_cp_nrppa_handler().handle_ul_nrppa_pdu(
      byte_buffer::create(span<const uint8_t>{expected_pdu}).value(), ue->ue_index);

  ngap_message ngap_pdu;
  ASSERT_TRUE(env.wait_for_ngap_tx_pdu(ngap_pdu));
  ASSERT_EQ(ngap_pdu.pdu.init_msg().value.type(),
            asn1::ngap::ngap_elem_procs_o::init_msg_c::types_opts::ul_ue_associated_nrppa_transport);
  ASSERT_EQ(ngap_pdu.pdu.init_msg().value.ul_ue_associated_nrppa_transport()->nrppa_pdu,
            span<const uint8_t>{expected_pdu});

  const cu_cp_ntn_runtime_status runtime =
      env.get_cu_cp().get_command_handler().get_ntn_command_handler().get_current_ntn_runtime_status();
  ASSERT_EQ(runtime.nof_ntn_nrppa_ul_ue_received, 1U);
  ASSERT_EQ(runtime.nof_ntn_nrppa_ul_ue_sent, 1U);
  ASSERT_EQ(runtime.nof_ntn_nrppa_ul_ue_dropped, 0U);
}

TEST(cu_cp_ntn_mobility_test, cu_cp_ul_non_ue_nrppa_transport_is_forwarded_to_matching_ngap)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  cu_cp_impl_interface* cu_cp_impl = get_cu_cp_impl(env);
  const std::array<uint8_t, 3> expected_pdu = {0x04, 0x05, 0x06};
  cu_cp_impl->get_cu_cp_nrppa_handler().handle_ul_nrppa_pdu(
      byte_buffer::create(span<const uint8_t>{expected_pdu}).value(), amf_index_t::min);

  ngap_message ngap_pdu;
  ASSERT_TRUE(env.wait_for_ngap_tx_pdu(ngap_pdu));
  ASSERT_EQ(ngap_pdu.pdu.init_msg().value.type(),
            asn1::ngap::ngap_elem_procs_o::init_msg_c::types_opts::ul_non_ue_associated_nrppa_transport);
  ASSERT_EQ(ngap_pdu.pdu.init_msg().value.ul_non_ue_associated_nrppa_transport()->nrppa_pdu,
            span<const uint8_t>{expected_pdu});

  const cu_cp_ntn_runtime_status runtime =
      env.get_cu_cp().get_command_handler().get_ntn_command_handler().get_current_ntn_runtime_status();
  ASSERT_EQ(runtime.nof_ntn_nrppa_ul_non_ue_received, 1U);
  ASSERT_EQ(runtime.nof_ntn_nrppa_ul_non_ue_sent, 1U);
  ASSERT_EQ(runtime.nof_ntn_nrppa_ul_non_ue_dropped, 0U);
}

TEST(cu_cp_ntn_mobility_test, satellite_state_update_selects_runtime_served_beam)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();
  ASSERT_TRUE(connect_du_for_ntn_beams(env).has_value());

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));
  ASSERT_EQ(ntn_handler.get_current_ntn_served_beam_ids(), std::vector<std::string>({"CN-BEAM-0001"}));

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));
  ASSERT_EQ(ntn_handler.get_current_ntn_served_beam_ids(), std::vector<std::string>({"CN-BEAM-0001"}));

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 1.0, 500000.0)));
  ASSERT_EQ(ntn_handler.get_current_ntn_served_beam_ids(), std::vector<std::string>({"CN-BEAM-0002"}));
}

TEST(cu_cp_ntn_mobility_test, manual_freeze_ignores_satellite_updates_until_restore)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();
  ASSERT_TRUE(connect_du_for_ntn_beams(env).has_value());

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));
  ASSERT_EQ(ntn_handler.get_current_ntn_served_beam_ids(), std::vector<std::string>({"CN-BEAM-0001"}));

  ntn_manual_override_command freeze;
  freeze.mode = ntn_manual_override_mode::freeze_current_state;
  ASSERT_TRUE(ntn_handler.handle_ntn_manual_override(freeze));
  ASSERT_FALSE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 1.0, 500000.0)));
  ASSERT_EQ(ntn_handler.get_current_ntn_served_beam_ids(), std::vector<std::string>({"CN-BEAM-0001"}));

  ntn_manual_override_command restore;
  restore.mode = ntn_manual_override_mode::restore_automatic_source;
  ASSERT_TRUE(ntn_handler.handle_ntn_manual_override(restore));
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 1.0, 500000.0)));
  ASSERT_EQ(ntn_handler.get_current_ntn_served_beam_ids(), std::vector<std::string>({"CN-BEAM-0002"}));
}

TEST(cu_cp_ntn_mobility_test, soft_switch_over_marks_snapshot_without_releasing_existing_ue)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));

  env.run_ng_setup();
  const std::optional<connected_ngap_ntn_ue> ue = connect_ngap_ntn_ue(env);
  ASSERT_TRUE(ue.has_value());

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  ntn_service_switch_over_event event;
  event.event_id          = 201;
  event.type              = ntn_service_switch_over_type::soft;
  event.source            = ntn_service_switch_over_source::operator_command;
  event.policy            = ntn_service_switch_over_policy::prepare;
  event.affected_beam_ids = {"CN-BEAM-0001"};
  ASSERT_TRUE(ntn_handler.handle_ntn_service_switch_over_event(event));

  ntn_service_switch_over_snapshot snapshot = ntn_handler.get_current_ntn_service_switch_over_snapshot();
  ASSERT_EQ(snapshot.active_events.size(), 1);
  const auto beam_it =
      std::find_if(snapshot.beams.begin(),
                   snapshot.beams.end(),
                   [](const ntn_service_switch_over_beam_snapshot& beam) { return beam.beam_id == "CN-BEAM-0001"; });
  ASSERT_NE(beam_it, snapshot.beams.end());
  EXPECT_EQ(beam_it->policy, ntn_service_beam_policy::prepare);

  ngap_message ngap_pdu;
  ASSERT_FALSE(env.wait_for_ngap_tx_pdu(ngap_pdu, std::chrono::milliseconds{20}));
}

TEST(cu_cp_ntn_mobility_test, manual_update_drives_runtime_served_beam_without_auto_updater)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();
  ASSERT_TRUE(connect_du_for_ntn_beams(env).has_value());

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();

  for (unsigned i = 0; i != 20; ++i) {
    env.tick();
  }
  ASSERT_TRUE(ntn_handler.get_current_ntn_served_beam_ids().empty());

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));
  ASSERT_EQ(ntn_handler.get_current_ntn_served_beam_ids(), std::vector<std::string>({"CN-BEAM-0001"}));
}

TEST(cu_cp_ntn_mobility_test, ntn_assistance_snapshot_tracks_satellite_and_beam_runtime_state)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();
  ASSERT_TRUE(connect_du_for_ntn_beams(env).has_value());

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();

  ntn_assistance_snapshot snapshot = ntn_handler.get_current_ntn_assistance_snapshot();
  ASSERT_FALSE(snapshot.valid);
  EXPECT_EQ(snapshot.invalid_reason, ntn_assistance_invalid_reason::no_satellite_state);

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  snapshot = ntn_handler.get_current_ntn_assistance_snapshot();
  ASSERT_TRUE(snapshot.valid);
  ASSERT_TRUE(snapshot.satellite_ecef.has_value());
  ASSERT_EQ(snapshot.beams.size(), 1U);
  EXPECT_EQ(snapshot.beams.front().beam_id, "CN-BEAM-0001");
  EXPECT_EQ(snapshot.beams.front().state, ntn_assistance_beam_state::candidate);
  EXPECT_EQ(snapshot.beams.front().cell_specific_koffset, 0U);
  ASSERT_TRUE(snapshot.beams.front().ta_info.has_value());
}

TEST(cu_cp_ntn_mobility_test, ntn_sib19_assistance_contract_tracks_command_handler_snapshot)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_stale_assistance_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();
  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  ASSERT_TRUE(du_idx.has_value());

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();

  ntn_sib19_assistance_snapshot contract = ntn_handler.get_current_ntn_sib19_assistance_snapshot();
  ASSERT_FALSE(contract.valid);
  EXPECT_EQ(contract.invalid_reason, ntn_assistance_invalid_reason::no_satellite_state);
  EXPECT_TRUE(contract.entries.empty());

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));
  env.drain_f1ap_resource_coordination_requests(du_idx.value());

  contract = ntn_handler.get_current_ntn_sib19_assistance_snapshot();
  ASSERT_TRUE(contract.valid);
  ASSERT_EQ(contract.entries.size(), 2U);

  const auto entry_it = std::find_if(contract.entries.begin(), contract.entries.end(), [](const auto& entry) {
    return entry.beam_id == "CN-BEAM-0001";
  });
  ASSERT_NE(entry_it, contract.entries.end());
  EXPECT_EQ(entry_it->state, ntn_assistance_beam_state::candidate);
  EXPECT_TRUE(entry_it->satellite_ecef.has_value());

  expire_ntn_assistance();
  contract = ntn_handler.get_current_ntn_sib19_assistance_snapshot();
  ASSERT_FALSE(contract.valid);
  EXPECT_EQ(contract.invalid_reason, ntn_assistance_invalid_reason::stale_satellite_state);
  EXPECT_TRUE(contract.entries.empty());
}

TEST(cu_cp_ntn_mobility_test, sib19_broadcast_update_reaches_du_and_records_applied)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();
  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  ASSERT_TRUE(du_idx.has_value());

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));
  ASSERT_TRUE(drain_sib19_broadcast_until_state(
      env, du_idx.value(), ntn_handler, "CN-BEAM-0001", "applied_by_du"));

  const cu_cp_ntn_beam_status beam_status =
      find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0001");
  EXPECT_EQ(beam_status.sib19_broadcast_state, "applied_by_du");
  EXPECT_EQ(beam_status.sib19_broadcast_reason, "applied");
  EXPECT_GT(beam_status.sib19_broadcast_generation, 0U);
  EXPECT_GT(beam_status.sib19_packed_bytes, 0U);
  EXPECT_NE(beam_status.sib19_packed_hash, 0U);

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_GE(runtime.nof_sib19_broadcast_applied_by_du, 1U);
}

TEST(cu_cp_ntn_mobility_test, downlink_disabled_beam_is_not_recommended_for_paging_or_sib19)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  params.ntn_location_mobility->beams[0].downlink_enabled = false;
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();
  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  ASSERT_TRUE(du_idx.has_value());

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));
  env.drain_f1ap_resource_coordination_requests(du_idx.value());

  const cu_cp_ntn_beam_status beam_status =
      find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0001");
  EXPECT_FALSE(beam_status.downlink_enabled);
  EXPECT_TRUE(beam_status.uplink_enabled);
  EXPECT_FALSE(beam_status.downlink_visible);
  EXPECT_TRUE(beam_status.uplink_access_ready);
  EXPECT_FALSE(beam_status.access_roundtrip_ready);
  EXPECT_FALSE(beam_status.downlink_ready);
  EXPECT_FALSE(beam_status.paging_recommendable);
  EXPECT_EQ(beam_status.paging_recommendation_reason, "downlink_unavailable");
  EXPECT_EQ(beam_status.sib19_broadcast_state, "stale_blocked");
  EXPECT_EQ(beam_status.sib19_broadcast_reason, "downlink_unavailable");
  EXPECT_EQ(beam_status.sib19_packed_bytes, 0U);
  EXPECT_EQ(beam_status.sib19_packed_hash, 0U);

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_downlink_ready_beams, 0U);
  EXPECT_EQ(runtime.nof_downlink_visible_beams, 0U);
  EXPECT_EQ(runtime.nof_uplink_access_ready_beams, 1U);
  EXPECT_EQ(runtime.nof_access_roundtrip_ready_beams, 0U);
  EXPECT_EQ(runtime.nof_paging_recommendable_beams, 0U);
}

TEST(cu_cp_ntn_mobility_test, sib19_broadcast_does_not_resend_unchanged_payload)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();
  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  ASSERT_TRUE(du_idx.has_value());

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  const ecef_coordinates_t   satellite   = make_ecef(0.0, 0.0, 500000.0);
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(satellite));
  ASSERT_TRUE(drain_sib19_broadcast_until_state(
      env, du_idx.value(), ntn_handler, "CN-BEAM-0001", "applied_by_du"));

  const cu_cp_ntn_beam_status first_status =
      find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0001");
  ASSERT_GT(first_status.sib19_broadcast_generation, 0U);
  ASSERT_NE(first_status.sib19_packed_hash, 0U);

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(satellite));
  env.drain_f1ap_resource_coordination_requests(du_idx.value());

  const cu_cp_ntn_beam_status second_status =
      find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0001");
  EXPECT_EQ(second_status.sib19_broadcast_state, "applied_by_du");
  EXPECT_EQ(second_status.sib19_broadcast_generation, first_status.sib19_broadcast_generation);
  EXPECT_EQ(second_status.sib19_packed_hash, first_status.sib19_packed_hash);
}

TEST(cu_cp_ntn_mobility_test, ntn_repair_dry_run_resources_counts_audit_targets_without_sending_f1ap)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_access_service_layer_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  const std::optional<service_bound_ntn_ue_context> ue = setup_service_bound_ntn_ue(env, ntn_handler, true);
  ASSERT_TRUE(ue.has_value());

  const cu_cp_ntn_runtime_status before = ntn_handler.get_current_ntn_runtime_status();
  ntn_repair_command             command;
  command.mode  = ntn_repair_mode::dry_run;
  command.scope = ntn_repair_scope::resources;

  const ntn_repair_response response = ntn_handler.handle_ntn_repair_command(command);
  const cu_cp_ntn_runtime_status after = ntn_handler.get_current_ntn_runtime_status();

  ASSERT_TRUE(response.accepted);
  EXPECT_EQ(response.reason, "dry_run");
  EXPECT_GT(response.audit_targets, 0U);
  EXPECT_EQ(response.sib19_candidates, 0U);
  EXPECT_EQ(after.nof_ntn_resource_audit_queries_sent, before.nof_ntn_resource_audit_queries_sent);
}

TEST(cu_cp_ntn_mobility_test, ntn_repair_apply_resources_triggers_one_shot_audit_and_existing_repair_executor)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_access_service_layer_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  const std::optional<service_bound_ntn_ue_context> ue = setup_service_bound_ntn_ue(env, ntn_handler, true);
  ASSERT_TRUE(ue.has_value());

  const cu_cp_ntn_runtime_status before = ntn_handler.get_current_ntn_runtime_status();
  ntn_repair_command             command;
  command.mode  = ntn_repair_mode::apply;
  command.scope = ntn_repair_scope::resources;

  const ntn_repair_response response = ntn_handler.handle_ntn_repair_command(command);
  env.drain_f1ap_resource_coordination_requests(ue->du_idx);
  const cu_cp_ntn_runtime_status after = ntn_handler.get_current_ntn_runtime_status();

  ASSERT_TRUE(response.accepted);
  EXPECT_EQ(response.reason, "resources_repair_requested");
  EXPECT_GT(response.audit_targets, 0U);
  EXPECT_GT(after.nof_ntn_resource_audit_queries_sent, before.nof_ntn_resource_audit_queries_sent);
  EXPECT_GT(after.nof_ntn_resource_audit_responses_accepted, before.nof_ntn_resource_audit_responses_accepted);
  EXPECT_GT(after.nof_ntn_resource_audit_rnti_incomplete, before.nof_ntn_resource_audit_rnti_incomplete);
  EXPECT_GT(after.nof_ntn_resource_audit_ue_slot_incomplete, before.nof_ntn_resource_audit_ue_slot_incomplete);
  EXPECT_EQ(after.nof_ntn_resource_audit_mismatches, before.nof_ntn_resource_audit_mismatches);
}

TEST(cu_cp_ntn_mobility_test, explicit_du_audit_rejection_reaches_conflict_accounting)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility     = make_access_service_layer_ntn_mobility_config();
  params.ntn_resource_audit_rejects = true;
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  const std::optional<service_bound_ntn_ue_context> ue = setup_service_bound_ntn_ue(env, ntn_handler, true);
  ASSERT_TRUE(ue.has_value());

  const cu_cp_ntn_runtime_status before = ntn_handler.get_current_ntn_runtime_status();
  ntn_repair_command             command;
  command.mode  = ntn_repair_mode::apply;
  command.scope = ntn_repair_scope::resources;
  ASSERT_TRUE(ntn_handler.handle_ntn_repair_command(command).accepted);
  env.drain_f1ap_resource_coordination_requests(ue->du_idx);

  const cu_cp_ntn_runtime_status after = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_GT(after.nof_ntn_resource_audit_failures, before.nof_ntn_resource_audit_failures);
  EXPECT_GT(after.nof_ntn_resource_audit_mismatches, before.nof_ntn_resource_audit_mismatches);
  EXPECT_GT(after.nof_ntn_resource_audit_repair_actions, before.nof_ntn_resource_audit_repair_actions);
  EXPECT_GT(after.nof_ntn_resource_repairs_blocked_conflict, before.nof_ntn_resource_repairs_blocked_conflict);
  EXPECT_EQ(after.last_ntn_resource_audit_reason, "rejected_by_mock_du");
}

TEST(cu_cp_ntn_mobility_test, ntn_repair_apply_sib19_refresh_keeps_unchanged_payload_deduplicated)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();
  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  ASSERT_TRUE(du_idx.has_value());

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  const ecef_coordinates_t   satellite   = make_ecef(0.0, 0.0, 500000.0);
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(satellite));
  ASSERT_TRUE(drain_sib19_broadcast_until_state(
      env, du_idx.value(), ntn_handler, "CN-BEAM-0001", "applied_by_du"));

  const cu_cp_ntn_beam_status first_status =
      find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0001");
  ASSERT_GT(first_status.sib19_broadcast_generation, 0U);

  ntn_repair_command command;
  command.mode  = ntn_repair_mode::apply;
  command.scope = ntn_repair_scope::sib19;
  const ntn_repair_response response = ntn_handler.handle_ntn_repair_command(command);
  env.drain_f1ap_resource_coordination_requests(du_idx.value());

  const cu_cp_ntn_beam_status second_status =
      find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0001");
  ASSERT_TRUE(response.accepted);
  EXPECT_EQ(response.reason, "sib19_refresh_requested");
  EXPECT_GT(response.sib19_candidates, 0U);
  EXPECT_EQ(second_status.sib19_broadcast_state, "applied_by_du");
  EXPECT_EQ(second_status.sib19_broadcast_generation, first_status.sib19_broadcast_generation);
  EXPECT_EQ(second_status.sib19_packed_hash, first_status.sib19_packed_hash);
}

TEST(cu_cp_ntn_mobility_test, ntn_runtime_snapshot_exposes_candidate_loaded_and_assistance_state)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  ASSERT_TRUE(runtime.enabled);
  ASSERT_TRUE(runtime.satellite_state_available);
  ASSERT_TRUE(runtime.assistance_valid);
  ASSERT_EQ(runtime.nof_candidate_beams, 1U);
  ASSERT_EQ(runtime.nof_active_loaded_beams, 0U);
  ASSERT_EQ(runtime.nof_loaded_service_beams, 0U);
  ASSERT_EQ(runtime.nof_draining_beams, 0U);
  ASSERT_EQ(runtime.nof_ues_with_ntn_context, 0U);
  ASSERT_EQ(runtime.nof_valid_service_area_beams, 2U);
  ASSERT_EQ(runtime.nof_invalid_service_area_beams, 0U);
  ASSERT_EQ(runtime.nof_paging_recommendable_beams, 1U);

  const cu_cp_ntn_beam_status candidate_beam =
      find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0001");
  ASSERT_EQ(candidate_beam.state, cu_cp_ntn_beam_assignment_state::candidate);
  ASSERT_TRUE(candidate_beam.derived_tac.has_value());
  EXPECT_EQ(candidate_beam.derived_tac.value(), 1U);
  EXPECT_EQ(candidate_beam.derived_tac_reason, "valid");
  EXPECT_TRUE(candidate_beam.paging_recommendable);
  EXPECT_EQ(candidate_beam.paging_recommendation_reason, "eligible");

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  ASSERT_TRUE(env.attach_ue(du_idx.value(),
                            cu_up_idx.value(),
                            du_ue_id,
                            to_rnti(0x4601),
                            uint_to_amf_ue_id(0),
                            int_to_gnb_cu_up_ue_e1ap_id(0)));

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());
  expect_and_ack_ntn_slot_update(env, du_idx.value(), du_ue_id, ue_ctx->cu_ue_id.value(), to_rnti(0x4601));

  runtime = ntn_handler.get_current_ntn_runtime_status();
  ASSERT_EQ(runtime.nof_candidate_beams, 0U);
  ASSERT_EQ(runtime.nof_active_loaded_beams, 1U);
  ASSERT_EQ(runtime.nof_loaded_service_beams, 1U);
  ASSERT_EQ(runtime.nof_draining_beams, 0U);
  ASSERT_EQ(runtime.nof_ues_with_ntn_context, 1U);
  ASSERT_EQ(runtime.nof_valid_service_area_beams, 2U);
  ASSERT_EQ(runtime.nof_invalid_service_area_beams, 0U);
  ASSERT_EQ(runtime.nof_paging_recommendable_beams, 1U);

  const cu_cp_ntn_beam_status loaded_beam =
      find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0001");
  ASSERT_EQ(loaded_beam.state, cu_cp_ntn_beam_assignment_state::active_loaded);
  ASSERT_EQ(loaded_beam.state_reason, "loaded_service_calendar");
  ASSERT_FALSE(loaded_beam.new_demand_blocked);
  ASSERT_FALSE(loaded_beam.drain_forced);
  ASSERT_TRUE(loaded_beam.derived_tac.has_value());
  EXPECT_EQ(loaded_beam.derived_tac.value(), 1U);
  EXPECT_EQ(loaded_beam.derived_tac_reason, "valid");
  EXPECT_TRUE(loaded_beam.paging_recommendable);
  EXPECT_EQ(loaded_beam.paging_recommendation_reason, "eligible");
}

TEST(cu_cp_ntn_mobility_test, qos_summary_is_visible_after_loaded_service_setup)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  ASSERT_TRUE(env.connect_new_ue(du_idx.value(), du_ue_id, to_rnti(0x4601)));
  ASSERT_TRUE(env.authenticate_ue(du_idx.value(), du_ue_id, uint_to_amf_ue_id(0)));
  ASSERT_TRUE(env.setup_ue_security(du_idx.value(), du_ue_id));
  ASSERT_TRUE(env.finish_ue_registration(du_idx.value(), cu_up_idx.value(), du_ue_id));
  ASSERT_TRUE(setup_pdu_session_with_five_qi(env,
                                             du_idx.value(),
                                             cu_up_idx.value(),
                                             du_ue_id,
                                             to_rnti(0x4601),
                                             int_to_gnb_cu_up_ue_e1ap_id(0),
                                             pdu_session_id_t::min,
                                             drb_id_t::drb1,
                                             qos_flow_id_t::min,
                                             69,
                                             true));

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());
  expect_and_ack_ntn_slot_update(env, du_idx.value(), du_ue_id, ue_ctx->cu_ue_id.value(), to_rnti(0x4601));

  const cu_cp_ntn_beam_status beam_status =
      find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0001");
  ASSERT_EQ(beam_status.state, cu_cp_ntn_beam_assignment_state::active_loaded);
  ASSERT_TRUE(beam_status.qos.has_qos_demand);
  EXPECT_EQ(beam_status.qos.best_five_qi, uint_to_five_qi(69));
  EXPECT_EQ(beam_status.qos.best_qos_priority, 5U);
  EXPECT_EQ(beam_status.qos.best_arp_priority, 8U);
  EXPECT_TRUE(beam_status.qos.has_gbr);

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_qos_prioritized_loaded_beams, 1U);

  const std::vector<cu_cp_ntn_ue_status> ue_status = ntn_handler.get_current_ntn_ue_status();
  ASSERT_EQ(ue_status.size(), 1U);
  EXPECT_TRUE(ue_status.front().qos.has_qos_demand);
  EXPECT_EQ(ue_status.front().qos.best_five_qi, uint_to_five_qi(69));
}

TEST(cu_cp_ntn_mobility_test, lower_priority_pdu_demand_does_not_displace_higher_priority_loaded_service)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  params.ntn_location_mobility->served_beam_min_elevation_deg = -90.0;
  params.ntn_location_mobility->max_nof_served_beams          = 1;
  params.ntn_location_mobility->max_nof_loaded_digital_service_beams = 1;
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const gnb_du_ue_f1ap_id_t high_du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  ASSERT_TRUE(env.connect_new_ue(du_idx.value(), high_du_ue_id, to_rnti(0x4601)));
  ASSERT_TRUE(env.authenticate_ue(du_idx.value(), high_du_ue_id, uint_to_amf_ue_id(0)));
  ASSERT_TRUE(env.setup_ue_security(du_idx.value(), high_du_ue_id));
  ASSERT_TRUE(env.finish_ue_registration(du_idx.value(), cu_up_idx.value(), high_du_ue_id));
  ASSERT_TRUE(env.request_pdu_session_resource_setup(du_idx.value(), cu_up_idx.value(), high_du_ue_id));
  ASSERT_TRUE(setup_pdu_session_with_five_qi(env,
                                             du_idx.value(),
                                             cu_up_idx.value(),
                                             high_du_ue_id,
                                             to_rnti(0x4601),
                                             int_to_gnb_cu_up_ue_e1ap_id(0),
                                             pdu_session_id_t::min,
                                             drb_id_t::drb1,
                                             qos_flow_id_t::min,
                                             69,
                                             true));

  const cu_cp_test_environment::ue_context* high_ue_ctx = env.find_ue_context(du_idx.value(), high_du_ue_id);
  ASSERT_NE(high_ue_ctx, nullptr);
  ASSERT_TRUE(high_ue_ctx->cu_ue_id.has_value());
  expect_and_ack_ntn_slot_update(env, du_idx.value(), high_du_ue_id, high_ue_ctx->cu_ue_id.value(), to_rnti(0x4601));

  ntn_manual_override_command override;
  override.mode                         = ntn_manual_override_mode::replace_service_state;
  override.replacement_satellite_ecef   = make_ecef(0.0, 0.0, 500000.0);
  override.replacement_served_beam_ids  = {"CN-BEAM-0001", "CN-BEAM-0002"};
  ASSERT_TRUE(ntn_handler.handle_ntn_manual_override(override));

  ASSERT_EQ(find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0001").state,
            cu_cp_ntn_beam_assignment_state::active_loaded);
  ASSERT_EQ(find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0002").state,
            cu_cp_ntn_beam_assignment_state::candidate);

  const gnb_du_ue_f1ap_id_t low_du_ue_id = int_to_gnb_du_ue_f1ap_id(1);
  ASSERT_TRUE(env.connect_new_ue(du_idx.value(), low_du_ue_id, to_rnti(0x4602)));
  expect_and_ack_ntn_slot_update(env, du_idx.value(), high_du_ue_id, high_ue_ctx->cu_ue_id.value(), to_rnti(0x4601));
  ASSERT_TRUE(env.authenticate_ue(du_idx.value(), low_du_ue_id, uint_to_amf_ue_id(1)));
  ASSERT_TRUE(env.setup_ue_security(du_idx.value(), low_du_ue_id));
  ASSERT_TRUE(env.finish_ue_registration(du_idx.value(), cu_up_idx.value(), low_du_ue_id));

  const cu_cp_test_environment::ue_context* low_ue_ctx = env.find_ue_context(du_idx.value(), low_du_ue_id);
  ASSERT_NE(low_ue_ctx, nullptr);
  ASSERT_TRUE(low_ue_ctx->cu_ue_id.has_value());
  ASSERT_TRUE(low_ue_ctx->ran_ue_id.has_value());
  ASSERT_TRUE(low_ue_ctx->amf_ue_id.has_value());

  auto* cu_cp_impl = get_cu_cp_impl(env);
  ASSERT_NE(cu_cp_impl, nullptr);
  const ue_index_t low_ue_index = uint_to_ue_index(gnb_cu_ue_f1ap_id_to_uint(low_ue_ctx->cu_ue_id.value()));
  ntn_ue_location_report low_location = make_ntn_location_report(low_ue_index, make_default_env_nci(1));
  low_location.longitude_deg          = 1.0;
  cu_cp_impl->get_cu_cp_measurement_handler().handle_ue_location_report(low_location);
  expect_and_ack_ntn_slot_update(env, du_idx.value(), high_du_ue_id, high_ue_ctx->cu_ue_id.value(), to_rnti(0x4601));
  env.drain_f1ap_resource_coordination_requests(du_idx.value());

  ASSERT_TRUE(env.request_pdu_session_resource_setup(du_idx.value(), cu_up_idx.value(), low_du_ue_id));
  const pdu_session_id_t low_psi = uint_to_pdu_session_id(2);
  const qos_flow_id_t    low_qfi = uint_to_qos_flow_id(2);
  env.get_amf().push_tx_pdu(generate_valid_pdu_session_resource_setup_request_message(
      low_ue_ctx->amf_ue_id.value(),
      low_ue_ctx->ran_ue_id.value(),
      {{low_psi, {pdu_session_type_t::ipv4, {{low_qfi, 9}}}}}));

  ngap_message ngap_pdu;
  ASSERT_TRUE(env.wait_for_ngap_tx_pdu(ngap_pdu));
  ASSERT_TRUE(test_helpers::is_valid_pdu_session_resource_setup_response(ngap_pdu));
  ASSERT_TRUE(test_helpers::is_expected_pdu_session_resource_setup_response(ngap_pdu, {}, {low_psi}));

  e1ap_message e1ap_pdu;
  ASSERT_FALSE(env.wait_for_e1ap_tx_pdu(cu_up_idx.value(), e1ap_pdu, std::chrono::milliseconds{20}));

  EXPECT_EQ(find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0001").state,
            cu_cp_ntn_beam_assignment_state::active_loaded);
  EXPECT_EQ(find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0002").state,
            cu_cp_ntn_beam_assignment_state::candidate);
}

TEST(cu_cp_ntn_mobility_test, zero_loaded_digital_service_beam_cap_does_not_fallback_to_served_beam_limit)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  params.ntn_location_mobility->served_beam_min_elevation_deg = -90.0;
  params.ntn_location_mobility->max_nof_served_beams          = 1;
  params.ntn_location_mobility->max_nof_loaded_digital_service_beams = 0;
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const gnb_du_ue_f1ap_id_t first_du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  ASSERT_TRUE(env.attach_ue(du_idx.value(),
                            cu_up_idx.value(),
                            first_du_ue_id,
                            to_rnti(0x4601),
                            uint_to_amf_ue_id(0),
                            int_to_gnb_cu_up_ue_e1ap_id(0)));
  const cu_cp_test_environment::ue_context* first_ue_ctx = env.find_ue_context(du_idx.value(), first_du_ue_id);
  ASSERT_NE(first_ue_ctx, nullptr);
  ASSERT_TRUE(first_ue_ctx->cu_ue_id.has_value());
  expect_and_ack_ntn_slot_update(env, du_idx.value(), first_du_ue_id, first_ue_ctx->cu_ue_id.value(), to_rnti(0x4601));

  ntn_manual_override_command override;
  override.mode                        = ntn_manual_override_mode::replace_service_state;
  override.replacement_satellite_ecef  = make_ecef(0.0, 0.0, 500000.0);
  override.replacement_served_beam_ids = {"CN-BEAM-0001", "CN-BEAM-0002"};
  ASSERT_TRUE(ntn_handler.handle_ntn_manual_override(override));

  const gnb_du_ue_f1ap_id_t second_du_ue_id = int_to_gnb_du_ue_f1ap_id(1);
  ASSERT_TRUE(env.connect_new_ue(du_idx.value(), second_du_ue_id, to_rnti(0x4602)));
  expect_and_ack_ntn_slot_update(env, du_idx.value(), first_du_ue_id, first_ue_ctx->cu_ue_id.value(), to_rnti(0x4601));
  ASSERT_TRUE(env.authenticate_ue(du_idx.value(), second_du_ue_id, uint_to_amf_ue_id(1)));
  ASSERT_TRUE(env.setup_ue_security(du_idx.value(), second_du_ue_id));
  ASSERT_TRUE(env.finish_ue_registration(du_idx.value(), cu_up_idx.value(), second_du_ue_id));

  const cu_cp_test_environment::ue_context* second_ue_ctx = env.find_ue_context(du_idx.value(), second_du_ue_id);
  ASSERT_NE(second_ue_ctx, nullptr);
  ASSERT_TRUE(second_ue_ctx->cu_ue_id.has_value());

  auto* cu_cp_impl = get_cu_cp_impl(env);
  ASSERT_NE(cu_cp_impl, nullptr);
  ntn_ue_location_report second_location =
      make_ntn_location_report(uint_to_ue_index(gnb_cu_ue_f1ap_id_to_uint(second_ue_ctx->cu_ue_id.value())),
                               make_default_env_nci(1));
  second_location.longitude_deg = 1.0;
  cu_cp_impl->get_cu_cp_measurement_handler().handle_ue_location_report(second_location);
  expect_and_ack_ntn_slot_update(env, du_idx.value(), first_du_ue_id, first_ue_ctx->cu_ue_id.value(), to_rnti(0x4601));
  env.drain_f1ap_resource_coordination_requests(du_idx.value());

  ASSERT_TRUE(env.request_pdu_session_resource_setup(du_idx.value(), cu_up_idx.value(), second_du_ue_id));
  ASSERT_TRUE(setup_pdu_session_with_five_qi(env,
                                             du_idx.value(),
                                             cu_up_idx.value(),
                                             second_du_ue_id,
                                             to_rnti(0x4602),
                                             int_to_gnb_cu_up_ue_e1ap_id(1),
                                             uint_to_pdu_session_id(2),
                                             drb_id_t::drb1,
                                             uint_to_qos_flow_id(2),
                                             9,
                                             true));
  expect_and_ack_ntn_slot_update(env, du_idx.value(), second_du_ue_id, second_ue_ctx->cu_ue_id.value(), to_rnti(0x4602));
  env.drain_f1ap_resource_coordination_requests(du_idx.value());

  const std::vector<cu_cp_ntn_beam_status> beam_status = ntn_handler.get_current_ntn_beam_status();
  EXPECT_EQ(find_beam_status(beam_status, "CN-BEAM-0001").state, cu_cp_ntn_beam_assignment_state::active_loaded);
  EXPECT_EQ(find_beam_status(beam_status, "CN-BEAM-0002").state, cu_cp_ntn_beam_assignment_state::active_loaded);

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_active_loaded_beams, 2U);
  EXPECT_EQ(runtime.nof_loaded_digital_service_beams, 2U);
}

TEST(cu_cp_ntn_mobility_test, load_balancing_disabled_keeps_existing_binding_behavior)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_multi_beam_load_balancing_ntn_mobility_config(true);
  params.ntn_location_mobility->multi_beam_load_balancing_enabled = false;
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beam_cells(env, {0, 1});
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const auto first_ue =
      setup_service_bound_ntn_ue_on_existing_connections(env, ntn_handler, du_idx.value(), cu_up_idx.value(), 0, 0.0);
  ASSERT_TRUE(first_ue.has_value());
  const auto second_ue =
      setup_service_bound_ntn_ue_on_existing_connections(env, ntn_handler, du_idx.value(), cu_up_idx.value(), 1, 0.0);
  ASSERT_TRUE(second_ue.has_value());

  const cu_cp_ntn_ue_status first_status =
      find_ue_status(ntn_handler.get_current_ntn_ue_status(), first_ue->ue_index);
  const cu_cp_ntn_ue_status second_status =
      find_ue_status(ntn_handler.get_current_ntn_ue_status(), second_ue->ue_index);
  ASSERT_EQ(first_status.service_digital_beam_id.value(), "CN-BEAM-0001");
  ASSERT_EQ(second_status.service_digital_beam_id.value(), "CN-BEAM-0001");

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_load_balancing_admission_steered, 0U);
  EXPECT_EQ(runtime.nof_ntn_load_balancing_handover_requested, 0U);
}

TEST(cu_cp_ntn_mobility_test, new_service_binding_is_steered_to_less_loaded_ntn_service_beam)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_multi_beam_load_balancing_ntn_mobility_config(true);
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beam_cells(env, {0, 1});
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const auto first_ue =
      setup_service_bound_ntn_ue_on_existing_connections(env, ntn_handler, du_idx.value(), cu_up_idx.value(), 0, 0.0);
  ASSERT_TRUE(first_ue.has_value());
  const auto second_ue =
      setup_service_bound_ntn_ue_on_existing_connections(env, ntn_handler, du_idx.value(), cu_up_idx.value(), 1, 0.0);
  ASSERT_TRUE(second_ue.has_value());

  const cu_cp_ntn_ue_status first_status =
      find_ue_status(ntn_handler.get_current_ntn_ue_status(), first_ue->ue_index);
  const cu_cp_ntn_ue_status second_status =
      find_ue_status(ntn_handler.get_current_ntn_ue_status(), second_ue->ue_index);
  ASSERT_EQ(first_status.service_digital_beam_id.value(), "CN-BEAM-0001");
  ASSERT_EQ(second_status.service_digital_beam_id.value(), "CN-BEAM-0002");

  const std::vector<cu_cp_ntn_beam_status> beam_status = ntn_handler.get_current_ntn_beam_status();
  EXPECT_EQ(find_beam_status(beam_status, "CN-BEAM-0001").nof_ues, 1U);
  EXPECT_EQ(find_beam_status(beam_status, "CN-BEAM-0002").nof_ues, 1U);

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_load_balancing_admission_steered, 1U);
  EXPECT_EQ(runtime.nof_ntn_load_balancing_handover_requested, 0U);
}

TEST(cu_cp_ntn_mobility_test, multi_beam_load_balancing_prepares_connected_handover_from_hot_beam)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_multi_beam_load_balancing_ntn_mobility_config(false);
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beam_cells(env, {0, 1});
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const auto first_ue =
      setup_service_bound_ntn_ue_on_existing_connections(env, ntn_handler, du_idx.value(), cu_up_idx.value(), 0, 0.0);
  ASSERT_TRUE(first_ue.has_value());
  const auto second_ue = setup_service_bound_ntn_ue_on_existing_connections(
      env, ntn_handler, du_idx.value(), cu_up_idx.value(), 1, 0.0, false);
  ASSERT_TRUE(second_ue.has_value());

  f1ap_message target_setup;
  ASSERT_TRUE(wait_for_ue_context_setup_request(env, du_idx.value(), target_setup));
  expect_and_ack_ntn_slot_update(env, du_idx.value(), second_ue->du_ue_id, second_ue->cu_ue_id, second_ue->crnti);
  const auto& setup_req = target_setup.pdu.init_msg().value.ue_context_setup_request();
  ASSERT_EQ(setup_req->sp_cell_id.nr_cell_id.to_number(), make_default_env_nci(1).value());
  ASSERT_TRUE(setup_req->res_coordination_transfer_container_present);
  const std::optional<f1ap_ntn_ul_slot_resource_request> target_request =
      decode_f1ap_ntn_ul_slot_resource_request(setup_req->res_coordination_transfer_container);
  ASSERT_TRUE(target_request.has_value());
  ASSERT_TRUE(target_request->requested_c_rnti.has_value());

  const cu_cp_ntn_ue_status handover_status =
      find_ue_status(ntn_handler.get_current_ntn_ue_status(), first_ue->ue_index);
  ASSERT_EQ(handover_status.connected_handover_state, "target_resource_preparing");
  ASSERT_EQ(handover_status.connected_handover_reason, "load_balancing");
  ASSERT_EQ(handover_status.connected_handover_source_beam_id.value(), "CN-BEAM-0001");
  ASSERT_EQ(handover_status.connected_handover_target_beam_id.value(), "CN-BEAM-0002");
  ASSERT_EQ(handover_status.connected_handover_target_c_rnti, target_request->requested_c_rnti.value());

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_load_balancing_evaluations, 1U);
  EXPECT_EQ(runtime.nof_ntn_load_balancing_handover_requested, 1U);
  EXPECT_EQ(runtime.nof_ntn_load_balancing_handover_scheduled, 1U);
  EXPECT_EQ(runtime.nof_ntn_load_balancing_handover_skipped, 0U);
  EXPECT_EQ(runtime.nof_ntn_load_balancing_same_analog_scheduled, 1U);
  EXPECT_EQ(runtime.nof_ntn_load_balancing_cross_analog_scheduled, 0U);
  EXPECT_EQ(runtime.last_ntn_load_balancing_source_analog_id, "ANALOG-ACCESS-001");
  EXPECT_EQ(runtime.last_ntn_load_balancing_target_analog_id, "ANALOG-ACCESS-001");
  EXPECT_EQ(runtime.last_ntn_load_balancing_reason, "same_analog_scheduled");

  e1ap_message e1ap_pdu;
  ASSERT_FALSE(env.wait_for_e1ap_tx_pdu(cu_up_idx.value(), e1ap_pdu, std::chrono::milliseconds{20}));
  env.drain_f1ap_resource_coordination_requests(du_idx.value());
  env.get_du(du_idx.value())
      .push_ul_pdu(test_helpers::generate_ue_context_setup_failure(
          int_to_gnb_cu_ue_f1ap_id(setup_req->gnb_cu_ue_f1ap_id), first_ue->du_ue_id));
  expect_no_f1ap_ue_setup_or_release(env, du_idx.value(), std::chrono::milliseconds{50});
}

TEST(cu_cp_ntn_mobility_test, load_balancing_uses_service_pair_when_bidirectional_target_unavailable)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_service_pair_handover_ntn_mobility_config();
  params.ntn_location_mobility->multi_beam_load_balancing_enabled             = true;
  params.ntn_location_mobility->multi_beam_load_balancing_min_ue_delta        = 2;
  params.ntn_location_mobility->multi_beam_load_balancing_max_handovers_per_eval = 1;
  params.ntn_location_mobility->multi_beam_load_balancing_handover_cooldown   = std::chrono::milliseconds{0};
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beam_cells(env, {0, 1, 2});
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const auto first_ue =
      setup_service_bound_ntn_ue_on_existing_connections(env, ntn_handler, du_idx.value(), cu_up_idx.value(), 0, 0.0);
  ASSERT_TRUE(first_ue.has_value());
  const auto second_ue = setup_service_bound_ntn_ue_on_existing_connections(
      env, ntn_handler, du_idx.value(), cu_up_idx.value(), 1, 0.0, false);
  ASSERT_TRUE(second_ue.has_value());

  f1ap_message target_setup;
  ASSERT_TRUE(wait_for_ue_context_setup_request(env, du_idx.value(), target_setup));
  expect_and_ack_ntn_slot_update(env, du_idx.value(), second_ue->du_ue_id, second_ue->cu_ue_id, second_ue->crnti);
  const auto& setup_req = target_setup.pdu.init_msg().value.ue_context_setup_request();
  ASSERT_EQ(setup_req->sp_cell_id.nr_cell_id.to_number(), make_default_env_nci(1).value());
  ASSERT_TRUE(setup_req->res_coordination_transfer_container_present);
  const std::optional<f1ap_ntn_ul_slot_resource_request> target_request =
      decode_f1ap_ntn_ul_slot_resource_request(setup_req->res_coordination_transfer_container);
  ASSERT_TRUE(target_request.has_value());
  const cu_cp_ntn_beam_status uplink_resource_beam =
      find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-UL-0002");
  ASSERT_TRUE(target_request->sr_slot_offset.has_value());
  ASSERT_TRUE(target_request->srs_slot_offset.has_value());
  EXPECT_EQ(*target_request->sr_slot_offset, uplink_resource_beam.sr_slot_offset);
  EXPECT_EQ(*target_request->srs_slot_offset, uplink_resource_beam.srs_slot_offset);

  const cu_cp_ntn_ue_status handover_status =
      find_ue_status(ntn_handler.get_current_ntn_ue_status(), first_ue->ue_index);
  ASSERT_EQ(handover_status.connected_handover_state, "target_resource_preparing");
  ASSERT_EQ(handover_status.connected_handover_reason, "load_balancing");
  ASSERT_EQ(handover_status.connected_handover_source_beam_id.value(), "CN-BEAM-0001");
  ASSERT_EQ(handover_status.connected_handover_target_beam_id.value(), "CN-BEAM-0002");
  ASSERT_TRUE(handover_status.connected_handover_target_uplink_resource_beam_id.has_value());
  EXPECT_EQ(handover_status.connected_handover_target_uplink_resource_beam_id.value(), "CN-BEAM-UL-0002");

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_load_balancing_handover_requested, 1U);
  EXPECT_EQ(runtime.nof_ntn_load_balancing_handover_scheduled, 1U);
  EXPECT_EQ(runtime.nof_ntn_service_pair_handover_targets, 1U);
  EXPECT_EQ(runtime.nof_ntn_service_pair_handover_scheduled, 1U);
  EXPECT_EQ(runtime.last_ntn_service_pair_handover_reason, "same_analog_tac_uplink_resource");

  env.drain_f1ap_resource_coordination_requests(du_idx.value());
  env.get_du(du_idx.value())
      .push_ul_pdu(test_helpers::generate_ue_context_setup_failure(
          int_to_gnb_cu_ue_f1ap_id(setup_req->gnb_cu_ue_f1ap_id), first_ue->du_ue_id));
  expect_no_f1ap_ue_setup_or_release(env, du_idx.value(), std::chrono::milliseconds{50});

  const cu_cp_ntn_ue_status rollback_status =
      find_ue_status(ntn_handler.get_current_ntn_ue_status(), first_ue->ue_index);
  EXPECT_EQ(rollback_status.service_layer_state, "service_bound");
  ASSERT_TRUE(rollback_status.service_digital_beam_id.has_value());
  EXPECT_EQ(rollback_status.service_digital_beam_id.value(), "CN-BEAM-0001");
  EXPECT_EQ(rollback_status.connected_handover_state, "failed_retryable");
  EXPECT_EQ(rollback_status.connected_handover_target_resource_state, "rollback_restored");

  const cu_cp_ntn_runtime_status rollback_runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(rollback_runtime.nof_ntn_service_pair_handover_rolled_back, 1U);
  EXPECT_EQ(rollback_runtime.nof_ntn_service_pair_handover_committed, 0U);
  EXPECT_EQ(rollback_runtime.last_ntn_service_pair_handover_completion_reason, "source_preparation_failed");
}

TEST(cu_cp_ntn_mobility_test, service_pair_handover_success_commits_dl_anchor_and_ul_resource_to_target_ue)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_service_pair_handover_ntn_mobility_config();
  params.ntn_location_mobility->multi_beam_load_balancing_enabled             = true;
  params.ntn_location_mobility->multi_beam_load_balancing_min_ue_delta        = 2;
  params.ntn_location_mobility->multi_beam_load_balancing_max_handovers_per_eval = 1;
  params.ntn_location_mobility->multi_beam_load_balancing_handover_cooldown   = std::chrono::milliseconds{0};
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beam_cells(env, {0, 1, 2});
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const auto first_ue =
      setup_service_bound_ntn_ue_on_existing_connections(env, ntn_handler, du_idx.value(), cu_up_idx.value(), 0, 0.0);
  ASSERT_TRUE(first_ue.has_value());
  const auto second_ue = setup_service_bound_ntn_ue_on_existing_connections(
      env, ntn_handler, du_idx.value(), cu_up_idx.value(), 1, 0.0, false);
  ASSERT_TRUE(second_ue.has_value());

  f1ap_message target_setup;
  ASSERT_TRUE(wait_for_ue_context_setup_request(env, du_idx.value(), target_setup));
  expect_and_ack_ntn_slot_update(env, du_idx.value(), second_ue->du_ue_id, second_ue->cu_ue_id, second_ue->crnti);
  const auto& setup_req = target_setup.pdu.init_msg().value.ue_context_setup_request();
  ASSERT_TRUE(setup_req->res_coordination_transfer_container_present);
  const std::optional<f1ap_ntn_ul_slot_resource_request> target_request =
      decode_f1ap_ntn_ul_slot_resource_request(setup_req->res_coordination_transfer_container);
  ASSERT_TRUE(target_request.has_value());
  ASSERT_TRUE(target_request->requested_c_rnti.has_value());

  const ue_index_t target_ue_index = complete_ntn_connected_handover_success(
      env, *first_ue, target_setup, target_request->requested_c_rnti.value());

  const cu_cp_ntn_ue_status target_status =
      find_ue_status(ntn_handler.get_current_ntn_ue_status(), target_ue_index);
  EXPECT_EQ(target_status.connected_handover_state, "none");
  EXPECT_EQ(target_status.service_layer_state, "service_bound");
  ASSERT_TRUE(target_status.service_digital_beam_id.has_value());
  EXPECT_EQ(target_status.service_digital_beam_id.value(), "CN-BEAM-0002");
  ASSERT_TRUE(target_status.service_uplink_resource_beam_id.has_value());
  EXPECT_EQ(target_status.service_uplink_resource_beam_id.value(), "CN-BEAM-UL-0002");
  ASSERT_TRUE(target_status.service_uplink_resource_nci.has_value());
  EXPECT_EQ(target_status.service_uplink_resource_nci.value(), make_default_env_nci(2));
  EXPECT_EQ(target_status.service_uplink_resource_du_index, uint_to_du_index(du_idx.value()));
  EXPECT_EQ(target_status.service_pair_reason, "same_analog_tac_uplink_resource");
  EXPECT_EQ(target_status.service_layer_reason, "service_pair_bound");

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_service_pair_handover_committed, 1U);
  EXPECT_EQ(runtime.nof_ntn_service_pair_handover_rolled_back, 0U);
  EXPECT_EQ(runtime.last_ntn_service_pair_handover_completion_reason, "committed");
}

TEST(cu_cp_ntn_mobility_test, load_balancing_uses_cross_analog_when_source_analog_is_hot_and_no_sibling_capacity)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_analog_aware_load_balancing_ntn_mobility_config(false);
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beam_cells(env, {0, 2});
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const auto first_source_ue =
      setup_service_bound_ntn_ue_on_existing_connections(env, ntn_handler, du_idx.value(), cu_up_idx.value(), 0, 0.0);
  ASSERT_TRUE(first_source_ue.has_value());
  const auto second_source_ue = setup_service_bound_ntn_ue_on_existing_connections(
      env, ntn_handler, du_idx.value(), cu_up_idx.value(), 1, 0.0, false);
  ASSERT_TRUE(second_source_ue.has_value());

  f1ap_message target_setup;
  ASSERT_TRUE(wait_for_ue_context_setup_request(env, du_idx.value(), target_setup));
  expect_and_ack_ntn_slot_update(
      env, du_idx.value(), second_source_ue->du_ue_id, second_source_ue->cu_ue_id, second_source_ue->crnti);
  const auto& setup_req = target_setup.pdu.init_msg().value.ue_context_setup_request();
  ASSERT_EQ(setup_req->sp_cell_id.nr_cell_id.to_number(), make_default_env_nci(2).value());

  const cu_cp_ntn_ue_status handover_status =
      find_ue_status(ntn_handler.get_current_ntn_ue_status(), first_source_ue->ue_index);
  ASSERT_EQ(handover_status.connected_handover_state, "target_resource_preparing");
  ASSERT_EQ(handover_status.connected_handover_reason, "load_balancing");
  ASSERT_EQ(handover_status.connected_handover_source_beam_id.value(), "CN-BEAM-0001");
  ASSERT_EQ(handover_status.connected_handover_target_beam_id.value(), "CN-BEAM-0003");
  ASSERT_EQ(handover_status.connected_handover_source_analog_beam_id.value(), "ANALOG-ACCESS-001");
  ASSERT_EQ(handover_status.connected_handover_target_analog_beam_id.value(), "ANALOG-ACCESS-002");

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_load_balancing_handover_requested, 1U);
  EXPECT_EQ(runtime.nof_ntn_load_balancing_handover_scheduled, 1U);
  EXPECT_EQ(runtime.nof_ntn_load_balancing_same_analog_scheduled, 0U);
  EXPECT_EQ(runtime.nof_ntn_load_balancing_cross_analog_scheduled, 1U);
  EXPECT_EQ(runtime.last_ntn_load_balancing_source_analog_id, "ANALOG-ACCESS-001");
  EXPECT_EQ(runtime.last_ntn_load_balancing_target_analog_id, "ANALOG-ACCESS-002");
  EXPECT_EQ(runtime.last_ntn_load_balancing_reason, "cross_analog_scheduled");

  env.drain_f1ap_resource_coordination_requests(du_idx.value());
  env.get_du(du_idx.value())
      .push_ul_pdu(test_helpers::generate_ue_context_setup_failure(
          int_to_gnb_cu_ue_f1ap_id(setup_req->gnb_cu_ue_f1ap_id), first_source_ue->du_ue_id));
}

TEST(cu_cp_ntn_mobility_test, load_balancing_does_not_select_cold_analog_target)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_analog_aware_load_balancing_ntn_mobility_config(true);
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beam_cells(env, {0, 2});
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const auto first_source_ue =
      setup_service_bound_ntn_ue_on_existing_connections(env, ntn_handler, du_idx.value(), cu_up_idx.value(), 0, 0.0);
  ASSERT_TRUE(first_source_ue.has_value());
  const auto second_source_ue =
      setup_service_bound_ntn_ue_on_existing_connections(env, ntn_handler, du_idx.value(), cu_up_idx.value(), 1, 0.0);
  ASSERT_TRUE(second_source_ue.has_value());

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_GE(runtime.nof_ntn_load_balancing_handover_requested, 1U);
  EXPECT_EQ(runtime.nof_ntn_load_balancing_handover_scheduled, 0U);
  EXPECT_GE(runtime.nof_ntn_load_balancing_handover_skipped, 1U);
  EXPECT_GE(runtime.nof_ntn_load_balancing_skipped_cold_analog, 1U);
  EXPECT_EQ(runtime.last_ntn_load_balancing_source_analog_id, "ANALOG-ACCESS-001");
  EXPECT_EQ(runtime.last_ntn_load_balancing_target_analog_id, "none");
  EXPECT_EQ(runtime.last_ntn_load_balancing_reason, "cold_analog");
  EXPECT_EQ(runtime.nof_ntn_preheat_requested, 1U);
  EXPECT_EQ(runtime.nof_ntn_cold_analog_preheat_requested, 1U);
  EXPECT_EQ(runtime.nof_ntn_preheat_sent, 1U);
  EXPECT_EQ(runtime.nof_ntn_preheat_applied, 0U);
  EXPECT_GE(runtime.nof_ntn_preheat_skipped, 1U);
  EXPECT_GE(runtime.nof_ntn_preheat_skipped_by_capacity, 1U);
  EXPECT_EQ(runtime.last_ntn_preheat_reason, "capacity");
  EXPECT_EQ(runtime.last_ntn_preheat_source_analog_id, "ANALOG-ACCESS-001");
  EXPECT_EQ(runtime.last_ntn_preheat_target_analog_id, "ANALOG-ACCESS-002");
  expect_no_f1ap_ue_setup_or_release(env, du_idx.value(), std::chrono::milliseconds{50});
}

TEST(cu_cp_ntn_mobility_test, preheat_respects_analog_digital_du_and_resource_domain_caps)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_analog_aware_load_balancing_ntn_mobility_config(true);
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beam_cells(env, {0, 2});
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const auto first_source_ue =
      setup_service_bound_ntn_ue_on_existing_connections(env, ntn_handler, du_idx.value(), cu_up_idx.value(), 0, 0.0);
  ASSERT_TRUE(first_source_ue.has_value());
  const auto second_source_ue =
      setup_service_bound_ntn_ue_on_existing_connections(env, ntn_handler, du_idx.value(), cu_up_idx.value(), 1, 0.0);
  ASSERT_TRUE(second_source_ue.has_value());

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_preheat_requested, 1U);
  EXPECT_EQ(runtime.nof_ntn_preheat_sent, 1U);
  EXPECT_EQ(runtime.nof_ntn_preheat_applied, 0U);
  EXPECT_GE(runtime.nof_ntn_preheat_skipped, 1U);
  EXPECT_GE(runtime.nof_ntn_preheat_skipped_by_capacity, 1U);
  EXPECT_EQ(runtime.last_ntn_preheat_reason, "capacity");

  const cu_cp_ntn_beam_status target_beam =
      find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0003");
  EXPECT_NE(target_beam.state, cu_cp_ntn_beam_assignment_state::active_loaded);
  expect_no_f1ap_ue_setup_or_release(env, du_idx.value(), std::chrono::milliseconds{50});
}

TEST(cu_cp_ntn_mobility_test, load_balancing_schedules_cross_analog_after_preheat_applied)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_analog_aware_load_balancing_ntn_mobility_config(false);
  params.ntn_location_mobility->preheated_beam_min_ready_time = std::chrono::milliseconds{0};
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> source_du_idx = connect_du_for_ntn_beam_cells(env, {0});
  ASSERT_TRUE(source_du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const auto first_source_ue = setup_service_bound_ntn_ue_on_existing_connections(
      env, ntn_handler, source_du_idx.value(), cu_up_idx.value(), 0, 0.0);
  ASSERT_TRUE(first_source_ue.has_value());
  const auto second_source_ue = setup_service_bound_ntn_ue_on_existing_connections(
      env, ntn_handler, source_du_idx.value(), cu_up_idx.value(), 1, 0.0);
  ASSERT_TRUE(second_source_ue.has_value());

  cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  ASSERT_EQ(runtime.nof_ntn_preheat_requested, 1U);
  ASSERT_EQ(runtime.nof_ntn_preheat_applied, 0U);
  expect_no_f1ap_ue_setup_or_release(env, source_du_idx.value(), std::chrono::milliseconds{50});

  const std::optional<unsigned> target_du_idx = connect_du_for_ntn_beam_cells(env, {2}, int_to_gnb_du_id(0x12));
  ASSERT_TRUE(target_du_idx.has_value());
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  runtime = ntn_handler.get_current_ntn_runtime_status();
  ASSERT_EQ(runtime.nof_ntn_preheat_sent, 1U);
  ASSERT_EQ(runtime.nof_ntn_preheat_applied, 1U);
  ASSERT_EQ(runtime.nof_ntn_load_balancing_cross_analog_scheduled, 0U);
  env.drain_f1ap_resource_coordination_requests(target_du_idx.value());
  expect_no_f1ap_ue_setup_or_release(env, target_du_idx.value(), std::chrono::milliseconds{50});

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  f1ap_message target_setup;
  ASSERT_TRUE(wait_for_ue_context_setup_request(env, target_du_idx.value(), target_setup));
  const auto& setup_req = target_setup.pdu.init_msg().value.ue_context_setup_request();
  ASSERT_EQ(setup_req->sp_cell_id.nr_cell_id.to_number(), make_default_env_nci(2).value());

  const cu_cp_ntn_ue_status handover_status =
      find_ue_status(ntn_handler.get_current_ntn_ue_status(), first_source_ue->ue_index);
  ASSERT_EQ(handover_status.connected_handover_state, "target_resource_preparing");
  ASSERT_EQ(handover_status.connected_handover_reason, "load_balancing");
  ASSERT_EQ(handover_status.connected_handover_target_beam_id.value(), "CN-BEAM-0003");
  ASSERT_EQ(handover_status.connected_handover_target_analog_beam_id.value(), "ANALOG-ACCESS-002");

  runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_preheat_sent, 1U);
  EXPECT_EQ(runtime.nof_ntn_preheat_applied, 1U);
  EXPECT_GE(runtime.nof_ntn_preheat_skipped, 1U);
  EXPECT_EQ(runtime.nof_ntn_load_balancing_cross_analog_scheduled, 1U);
  EXPECT_EQ(runtime.last_ntn_load_balancing_reason, "cross_analog_scheduled");
  EXPECT_EQ(runtime.last_ntn_preheat_reason, "applied");

  env.get_du(target_du_idx.value())
      .push_ul_pdu(test_helpers::generate_ue_context_setup_failure(
          int_to_gnb_cu_ue_f1ap_id(setup_req->gnb_cu_ue_f1ap_id), first_source_ue->du_ue_id));
}

TEST(cu_cp_ntn_mobility_test, preheated_target_is_not_selected_until_ready_guard_expires)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_analog_aware_load_balancing_ntn_mobility_config(false);
  params.ntn_location_mobility->preheated_beam_min_ready_time = std::chrono::milliseconds{120};
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> source_du_idx = connect_du_for_ntn_beam_cells(env, {0});
  ASSERT_TRUE(source_du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const auto first_source_ue = setup_service_bound_ntn_ue_on_existing_connections(
      env, ntn_handler, source_du_idx.value(), cu_up_idx.value(), 0, 0.0);
  ASSERT_TRUE(first_source_ue.has_value());
  const auto second_source_ue = setup_service_bound_ntn_ue_on_existing_connections(
      env, ntn_handler, source_du_idx.value(), cu_up_idx.value(), 1, 0.0);
  ASSERT_TRUE(second_source_ue.has_value());

  const std::optional<unsigned> target_du_idx = connect_du_for_ntn_beam_cells(env, {2}, int_to_gnb_du_id(0x12));
  ASSERT_TRUE(target_du_idx.has_value());
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  ASSERT_EQ(runtime.nof_ntn_preheat_applied, 1U);
  EXPECT_EQ(runtime.nof_ntn_load_balancing_cross_analog_scheduled, 0U);
  EXPECT_EQ(runtime.nof_ntn_handover_skipped_by_preheat_ready_guard, 0U);
  EXPECT_EQ(runtime.last_ntn_scheduling_guard_reason, "preheat_applied");
  env.drain_f1ap_resource_coordination_requests(target_du_idx.value());
  expect_no_f1ap_ue_setup_or_release(env, target_du_idx.value(), std::chrono::milliseconds{50});

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));
  runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_load_balancing_cross_analog_scheduled, 0U);
  EXPECT_GE(runtime.nof_ntn_handover_skipped_by_preheat_ready_guard, 1U);
  EXPECT_EQ(runtime.last_ntn_scheduling_guard_reason, "preheat_ready_guard");
  expect_no_f1ap_ue_setup_or_release(env, target_du_idx.value(), std::chrono::milliseconds{20});

  std::this_thread::sleep_for(std::chrono::milliseconds{80});
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  f1ap_message target_setup;
  ASSERT_TRUE(wait_for_ue_context_setup_request(env, target_du_idx.value(), target_setup));
  const auto& setup_req = target_setup.pdu.init_msg().value.ue_context_setup_request();
  ASSERT_EQ(setup_req->sp_cell_id.nr_cell_id.to_number(), make_default_env_nci(2).value());

  runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_load_balancing_cross_analog_scheduled, 1U);
  EXPECT_EQ(runtime.nof_ntn_target_reservation_consumed, 1U);
  EXPECT_EQ(runtime.last_ntn_scheduling_guard_target_beam_id, "CN-BEAM-0003");
  EXPECT_EQ(runtime.last_ntn_scheduling_guard_target_analog_id, "ANALOG-ACCESS-002");

  env.get_du(target_du_idx.value())
      .push_ul_pdu(test_helpers::generate_ue_context_setup_failure(
          int_to_gnb_cu_ue_f1ap_id(setup_req->gnb_cu_ue_f1ap_id), first_source_ue->du_ue_id));
}

TEST(cu_cp_ntn_mobility_test, load_balancing_respects_target_analog_and_digital_caps)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_analog_aware_load_balancing_ntn_mobility_config(false);
  params.ntn_location_mobility->beams[0].resource_policy.emplace();
  params.ntn_location_mobility->beams[0].resource_policy->max_drbs = 2;
  params.ntn_location_mobility->beams[2].resource_policy.emplace();
  params.ntn_location_mobility->beams[2].resource_policy->max_drbs = 1;
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beam_cells(env, {0, 2});
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const auto first_source_ue =
      setup_service_bound_ntn_ue_on_existing_connections(env, ntn_handler, du_idx.value(), cu_up_idx.value(), 0, 0.0);
  ASSERT_TRUE(first_source_ue.has_value());
  ASSERT_TRUE(setup_pdu_session_with_five_qi(env,
                                             du_idx.value(),
                                             cu_up_idx.value(),
                                             first_source_ue->du_ue_id,
                                             first_source_ue->crnti,
                                             first_source_ue->cu_up_e1ap_id,
                                             uint_to_pdu_session_id(2),
                                             drb_id_t::drb2,
                                             uint_to_qos_flow_id(2),
                                             9,
                                             false,
                                             0,
                                             8));

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_GE(runtime.nof_ntn_load_balancing_handover_requested, 1U);
  EXPECT_EQ(runtime.nof_ntn_load_balancing_handover_scheduled, 0U);
  EXPECT_GE(runtime.nof_ntn_load_balancing_handover_skipped, 1U);
  EXPECT_GE(runtime.nof_ntn_load_balancing_skipped_projected_capacity, 1U);
  EXPECT_EQ(runtime.last_ntn_load_balancing_reason, "projected_capacity");
  expect_no_f1ap_ue_setup_or_release(env, du_idx.value(), std::chrono::milliseconds{50});
}

TEST(cu_cp_ntn_mobility_test, reserved_target_capacity_blocks_low_priority_new_demand)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_analog_aware_load_balancing_ntn_mobility_config(false);
  params.ntn_location_mobility->multi_beam_headroom_admission_enabled = true;
  params.ntn_location_mobility->preheated_beam_min_ready_time          = std::chrono::milliseconds{5000};
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> source_du_idx = connect_du_for_ntn_beam_cells(env, {0});
  ASSERT_TRUE(source_du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const auto first_ue =
      setup_service_bound_ntn_ue_on_existing_connections(env, ntn_handler, source_du_idx.value(), cu_up_idx.value(), 0, 0.0);
  ASSERT_TRUE(first_ue.has_value());
  const auto second_ue = setup_service_bound_ntn_ue_on_existing_connections(
      env, ntn_handler, source_du_idx.value(), cu_up_idx.value(), 1, 0.0, false);
  ASSERT_TRUE(second_ue.has_value());

  const std::optional<unsigned> target_du_idx = connect_du_for_ntn_beam_cells(env, {2}, int_to_gnb_du_id(0x12));
  ASSERT_TRUE(target_du_idx.has_value());
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));
  for (unsigned i = 0; i != 4; ++i) {
    f1ap_message f1ap_pdu;
    if (!env.wait_for_f1ap_tx_pdu(source_du_idx.value(), f1ap_pdu, std::chrono::milliseconds{20})) {
      break;
    }
    ASSERT_TRUE(try_ack_ntn_slot_update(env, source_du_idx.value(), f1ap_pdu));
  }

  const std::vector<cu_cp_ntn_beam_status> beam_statuses = ntn_handler.get_current_ntn_beam_status();
  const auto reserved_beam_it =
      std::find_if(beam_statuses.begin(), beam_statuses.end(), [](const cu_cp_ntn_beam_status& status) {
        return status.beam_id == "CN-BEAM-0003" && status.headroom_reserved;
  });
  ASSERT_NE(reserved_beam_it, beam_statuses.end());
  const std::string reserved_beam_id = reserved_beam_it->beam_id;

  const gnb_du_ue_f1ap_id_t low_du_ue_id = int_to_gnb_du_ue_f1ap_id(2);
  const rnti_t              low_crnti    = to_rnti(0x4609);
  const ue_index_t low_ue_index = finish_ntn_access_only_registration(env,
                                                                      target_du_idx.value(),
                                                                      cu_up_idx.value(),
                                                                      low_du_ue_id,
                                                                      low_crnti,
                                                                      uint_to_amf_ue_id(2),
                                                                      make_supported_ntn_ngso_ue_capability_info_pdu(),
                                                                      std::nullopt,
                                                                      make_default_env_nci(2));
  ASSERT_NE(low_ue_index, ue_index_t::invalid);

  ntn_ue_location_report low_location = make_ntn_location_report(low_ue_index, make_default_env_nci(2));
  low_location.longitude_deg          = 2.0;
  get_cu_cp_impl(env)->get_cu_cp_measurement_handler().handle_ue_location_report(low_location);
  env.drain_f1ap_resource_coordination_requests(source_du_idx.value());
  env.drain_f1ap_resource_coordination_requests(target_du_idx.value());

  ASSERT_TRUE(env.request_pdu_session_resource_setup(target_du_idx.value(), cu_up_idx.value(), low_du_ue_id));
  const cu_cp_test_environment::ue_context* low_ue_ctx = env.find_ue_context(target_du_idx.value(), low_du_ue_id);
  ASSERT_NE(low_ue_ctx, nullptr);
  ASSERT_TRUE(low_ue_ctx->amf_ue_id.has_value());
  ASSERT_TRUE(low_ue_ctx->ran_ue_id.has_value());

  const pdu_session_id_t low_psi = uint_to_pdu_session_id(3);
  env.get_amf().push_tx_pdu(make_pdu_session_resource_setup_request_with_five_qi(low_ue_ctx->amf_ue_id.value(),
                                                                                 low_ue_ctx->ran_ue_id.value(),
                                                                                 low_psi,
                                                                                 uint_to_qos_flow_id(3),
                                                                                 9,
                                                                                 false));

  ngap_message ngap_pdu;
  ASSERT_TRUE(env.wait_for_ngap_tx_pdu(ngap_pdu));
  ASSERT_TRUE(test_helpers::is_valid_pdu_session_resource_setup_response(ngap_pdu));
  ASSERT_TRUE(test_helpers::is_expected_pdu_session_resource_setup_response(ngap_pdu, {}, {low_psi}));

  e1ap_message e1ap_pdu;
  EXPECT_FALSE(env.wait_for_e1ap_tx_pdu(cu_up_idx.value(), e1ap_pdu, std::chrono::milliseconds{20}));

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_GE(runtime.nof_ntn_target_reservation_created, 1U);
  EXPECT_GE(runtime.nof_ntn_admission_blocked_by_target_reservation, 1U);
  EXPECT_EQ(runtime.last_ntn_scheduling_guard_reason, "target_reserved");
  EXPECT_EQ(runtime.last_ntn_scheduling_guard_target_beam_id, reserved_beam_id);

  env.drain_f1ap_resource_coordination_requests(source_du_idx.value());
  env.drain_f1ap_resource_coordination_requests(target_du_idx.value());
  expect_no_f1ap_ue_setup_or_release(env, source_du_idx.value(), std::chrono::milliseconds{50});
  expect_no_f1ap_ue_setup_or_release(env, target_du_idx.value(), std::chrono::milliseconds{50});
}

TEST(cu_cp_ntn_mobility_test, headroom_allows_priority_service_after_load_balancing_preload)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_multi_beam_load_balancing_ntn_mobility_config(false);
  params.ntn_location_mobility->multi_beam_headroom_admission_enabled = true;
  add_third_digital_service_beam(params.ntn_location_mobility.value());
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beam_cells(env, {0, 1, 2});
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const auto first_ue =
      setup_service_bound_ntn_ue_on_existing_connections(env, ntn_handler, du_idx.value(), cu_up_idx.value(), 0, 0.0);
  ASSERT_TRUE(first_ue.has_value());
  const auto second_ue = setup_service_bound_ntn_ue_on_existing_connections(
      env, ntn_handler, du_idx.value(), cu_up_idx.value(), 1, 0.0, false);
  ASSERT_TRUE(second_ue.has_value());

  f1ap_message target_setup;
  ASSERT_TRUE(wait_for_ue_context_setup_request(env, du_idx.value(), target_setup));
  const auto& target_setup_req = target_setup.pdu.init_msg().value.ue_context_setup_request();
  expect_and_ack_ntn_slot_update(env, du_idx.value(), second_ue->du_ue_id, second_ue->cu_ue_id, second_ue->crnti);

  const std::vector<cu_cp_ntn_beam_status> beam_statuses = ntn_handler.get_current_ntn_beam_status();
  const auto reserved_beam_it =
      std::find_if(beam_statuses.begin(), beam_statuses.end(), [](const cu_cp_ntn_beam_status& status) {
        return status.headroom_reserved;
      });
  ASSERT_NE(reserved_beam_it, beam_statuses.end());
  const std::string      reserved_beam_id = reserved_beam_it->beam_id;
  const nr_cell_identity reserved_nci     = reserved_beam_it->nci;

  const gnb_du_ue_f1ap_id_t low_du_ue_id = int_to_gnb_du_ue_f1ap_id(2);
  const rnti_t              low_crnti    = to_rnti(0x4603);
  const ue_index_t low_ue_index = finish_ntn_access_only_registration(env,
                                                                      du_idx.value(),
                                                                      cu_up_idx.value(),
                                                                      low_du_ue_id,
                                                                      low_crnti,
                                                                      uint_to_amf_ue_id(2),
                                                                      make_supported_ntn_ngso_ue_capability_info_pdu());
  ASSERT_NE(low_ue_index, ue_index_t::invalid);

  ntn_ue_location_report low_location = make_ntn_location_report(low_ue_index, reserved_nci);
  low_location.longitude_deg          = reserved_beam_id == "CN-BEAM-0003" ? 2.0 : 1.0;
  get_cu_cp_impl(env)->get_cu_cp_measurement_handler().handle_ue_location_report(low_location);
  env.drain_f1ap_resource_coordination_requests(du_idx.value());

  ASSERT_TRUE(env.request_pdu_session_resource_setup(du_idx.value(), cu_up_idx.value(), low_du_ue_id));
  const pdu_session_id_t low_psi = uint_to_pdu_session_id(3);
  const qos_flow_id_t    low_qfi = uint_to_qos_flow_id(3);
  ASSERT_TRUE(setup_pdu_session_with_five_qi(env,
                                             du_idx.value(),
                                             cu_up_idx.value(),
                                             low_du_ue_id,
                                             low_crnti,
                                             int_to_gnb_cu_up_ue_e1ap_id(2),
                                             low_psi,
                                             drb_id_t::drb1,
                                             low_qfi,
                                             9,
                                             true));
  const cu_cp_test_environment::ue_context* low_ue_ctx = env.find_ue_context(du_idx.value(), low_du_ue_id);
  ASSERT_NE(low_ue_ctx, nullptr);
  ASSERT_TRUE(low_ue_ctx->cu_ue_id.has_value());
  expect_and_ack_ntn_slot_update(env, du_idx.value(), low_du_ue_id, low_ue_ctx->cu_ue_id.value(), low_crnti);

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_headroom_admission_blocked, 0U);
  EXPECT_GT(runtime.nof_ntn_headroom_admission_allowed, 0U);
  EXPECT_NE(runtime.last_ntn_headroom_reason, "reserved_for_handover_target");
  EXPECT_TRUE(find_beam_status(ntn_handler.get_current_ntn_beam_status(), reserved_beam_id).headroom_reserved);

  env.drain_f1ap_resource_coordination_requests(du_idx.value());
  env.get_du(du_idx.value())
      .push_ul_pdu(test_helpers::generate_ue_context_setup_failure(
          int_to_gnb_cu_ue_f1ap_id(target_setup_req->gnb_cu_ue_f1ap_id), first_ue->du_ue_id));
  expect_no_f1ap_ue_setup_or_release(env, du_idx.value(), std::chrono::milliseconds{50});
}

TEST(cu_cp_ntn_mobility_test, headroom_allows_high_priority_or_preemptive_service_demand)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_multi_beam_load_balancing_ntn_mobility_config(false);
  params.ntn_location_mobility->multi_beam_headroom_admission_enabled = true;
  add_third_digital_service_beam(params.ntn_location_mobility.value());
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beam_cells(env, {0, 1, 2});
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const auto first_ue =
      setup_service_bound_ntn_ue_on_existing_connections(env, ntn_handler, du_idx.value(), cu_up_idx.value(), 0, 0.0);
  ASSERT_TRUE(first_ue.has_value());
  const auto second_ue = setup_service_bound_ntn_ue_on_existing_connections(
      env, ntn_handler, du_idx.value(), cu_up_idx.value(), 1, 0.0, false);
  ASSERT_TRUE(second_ue.has_value());

  f1ap_message target_setup;
  ASSERT_TRUE(wait_for_ue_context_setup_request(env, du_idx.value(), target_setup));
  const auto& target_setup_req = target_setup.pdu.init_msg().value.ue_context_setup_request();
  expect_and_ack_ntn_slot_update(env, du_idx.value(), second_ue->du_ue_id, second_ue->cu_ue_id, second_ue->crnti);

  const gnb_du_ue_f1ap_id_t high_du_ue_id = int_to_gnb_du_ue_f1ap_id(2);
  const rnti_t              high_crnti    = to_rnti(0x4603);
  const ue_index_t high_ue_index = finish_ntn_access_only_registration(env,
                                                                       du_idx.value(),
                                                                       cu_up_idx.value(),
                                                                       high_du_ue_id,
                                                                       high_crnti,
                                                                       uint_to_amf_ue_id(2),
                                                                       make_supported_ntn_ngso_ue_capability_info_pdu());
  ASSERT_NE(high_ue_index, ue_index_t::invalid);

  ntn_ue_location_report high_location = make_ntn_location_report(high_ue_index, make_default_env_nci(2));
  high_location.longitude_deg          = 2.0;
  get_cu_cp_impl(env)->get_cu_cp_measurement_handler().handle_ue_location_report(high_location);
  env.drain_f1ap_resource_coordination_requests(du_idx.value());

  ASSERT_TRUE(env.request_pdu_session_resource_setup(du_idx.value(), cu_up_idx.value(), high_du_ue_id));
  ASSERT_TRUE(setup_pdu_session_with_five_qi(env,
                                             du_idx.value(),
                                             cu_up_idx.value(),
                                             high_du_ue_id,
                                             high_crnti,
                                             int_to_gnb_cu_up_ue_e1ap_id(2),
                                             uint_to_pdu_session_id(3),
                                             uint_to_drb_id(3),
                                             uint_to_qos_flow_id(3),
                                             69,
                                             true));
  const cu_cp_test_environment::ue_context* high_ue_ctx = env.find_ue_context(du_idx.value(), high_du_ue_id);
  ASSERT_NE(high_ue_ctx, nullptr);
  ASSERT_TRUE(high_ue_ctx->cu_ue_id.has_value());
  expect_and_ack_ntn_slot_update(env, du_idx.value(), high_du_ue_id, high_ue_ctx->cu_ue_id.value(), high_crnti);

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_headroom_admission_blocked, 0U);
  EXPECT_GE(runtime.nof_ntn_headroom_admission_allowed, 1U);
  EXPECT_EQ(runtime.last_ntn_headroom_reason, "priority_allowed");
  EXPECT_EQ(find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0003").state,
            cu_cp_ntn_beam_assignment_state::active_loaded);

  env.drain_f1ap_resource_coordination_requests(du_idx.value());
  env.get_du(du_idx.value())
      .push_ul_pdu(test_helpers::generate_ue_context_setup_failure(
          int_to_gnb_cu_ue_f1ap_id(target_setup_req->gnb_cu_ue_f1ap_id), first_ue->du_ue_id));
  expect_no_f1ap_ue_setup_or_release(env, du_idx.value(), std::chrono::milliseconds{50});
}

TEST(cu_cp_ntn_mobility_test, load_balancing_skips_when_no_eligible_target_or_capacity)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_multi_beam_load_balancing_ntn_mobility_config(false);
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beam_cells(env, {0});
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const auto first_ue =
      setup_service_bound_ntn_ue_on_existing_connections(env, ntn_handler, du_idx.value(), cu_up_idx.value(), 0, 0.0);
  ASSERT_TRUE(first_ue.has_value());
  const auto second_ue =
      setup_service_bound_ntn_ue_on_existing_connections(env, ntn_handler, du_idx.value(), cu_up_idx.value(), 1, 0.0);
  ASSERT_TRUE(second_ue.has_value());

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_GE(runtime.nof_ntn_load_balancing_evaluations, 1U);
  EXPECT_GE(runtime.nof_ntn_load_balancing_handover_requested, 1U);
  EXPECT_EQ(runtime.nof_ntn_load_balancing_handover_scheduled, 0U);
  EXPECT_GE(runtime.nof_ntn_load_balancing_handover_skipped, 1U);
  EXPECT_EQ(runtime.last_ntn_load_balancing_reason, "no_eligible_target");

  const cu_cp_ntn_ue_status first_status =
      find_ue_status(ntn_handler.get_current_ntn_ue_status(), first_ue->ue_index);
  const cu_cp_ntn_ue_status second_status =
      find_ue_status(ntn_handler.get_current_ntn_ue_status(), second_ue->ue_index);
  EXPECT_EQ(first_status.connected_handover_state, "none");
  EXPECT_EQ(second_status.connected_handover_state, "none");
  expect_no_f1ap_ue_setup_or_release(env, du_idx.value(), std::chrono::milliseconds{50});
}

TEST(cu_cp_ntn_mobility_test, demand_aware_beam_scheduling_keeps_service_loaded_beam_in_window)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_access_service_layer_ntn_mobility_config();
  params.ntn_location_mobility->max_nof_served_beams = 1;
  params.ntn_location_mobility->demand_aware_beam_scheduling_enabled = true;
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beam_cells(env, {0, 1});
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));
  ASSERT_EQ(ntn_handler.get_current_ntn_served_beam_ids(), std::vector<std::string>({"CN-BEAM-0001"}));

  const auto service_ue =
      setup_service_bound_ntn_ue_on_existing_connections(env, ntn_handler, du_idx.value(), cu_up_idx.value(), 0, 0.0);
  ASSERT_TRUE(service_ue.has_value());
  ASSERT_EQ(find_ue_status(ntn_handler.get_current_ntn_ue_status(), service_ue->ue_index)
                .service_digital_beam_id.value(),
            "CN-BEAM-0001");

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 1.0, 500000.0)));
  ASSERT_EQ(ntn_handler.get_current_ntn_served_beam_ids(), std::vector<std::string>({"CN-BEAM-0001"}));

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_GT(runtime.nof_ntn_beam_scheduling_evaluations, 0U);
  EXPECT_GT(runtime.nof_ntn_beam_scheduling_demand_prioritized_windows, 0U);
  EXPECT_EQ(runtime.last_ntn_beam_scheduling_reason, "demand_prioritized");
  EXPECT_GT(runtime.nof_ntn_resource_weighting_evaluations, 0U);
  EXPECT_GT(runtime.nof_ntn_resource_weighting_weighted_beams, 0U);
  EXPECT_EQ(runtime.last_ntn_resource_weighting_reason, "weighted");

  const cu_cp_ntn_beam_status source_beam =
      find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0001");
  EXPECT_TRUE(source_beam.in_hopping_window);
  EXPECT_EQ(source_beam.window_rank, 0U);
  EXPECT_EQ(source_beam.scheduling_reason, "demand");
  EXPECT_GT(source_beam.scheduling_score, 0U);
  EXPECT_GT(source_beam.resource_weight, 0U);
  EXPECT_EQ(source_beam.resource_share, 1.0);
  EXPECT_NE(source_beam.resource_weight_reason, "legacy");
  expect_no_f1ap_ue_setup_or_release(env, du_idx.value(), std::chrono::milliseconds{50});
}

TEST(cu_cp_ntn_mobility_test, user_accessed_beam_receives_cucp_antenna_slot_after_pdu_session_setup)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  const ecef_coordinates_t   satellite   = make_ecef(0.0, 0.0, 500000.0);

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(satellite));
  cu_cp_ntn_beam_status beam_status = find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0001");
  ASSERT_EQ(beam_status.state, cu_cp_ntn_beam_assignment_state::candidate);
  ASSERT_EQ(beam_status.du_index, uint_to_du_index(du_idx.value()));
  ASSERT_EQ(beam_status.nof_antenna_slots, 0U);
  ASSERT_EQ(beam_status.antenna_slot_period, 0U);
  ASSERT_EQ(beam_status.sr_slot_period, 0U);
  ASSERT_EQ(beam_status.srs_slot_period, 0U);

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  ASSERT_TRUE(env.attach_ue(du_idx.value(),
                            cu_up_idx.value(),
                            du_ue_id,
                            to_rnti(0x4601),
                            uint_to_amf_ue_id(0),
                            int_to_gnb_cu_up_ue_e1ap_id(0)));

  beam_status = find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0001");
  ASSERT_EQ(beam_status.state, cu_cp_ntn_beam_assignment_state::active_loaded);
  ASSERT_EQ(beam_status.nof_ues, 1U);
  ASSERT_EQ(beam_status.nof_antenna_slots, 1U);
  ASSERT_EQ(beam_status.antenna_slot_index, 0U);
  ASSERT_EQ(beam_status.antenna_slot_period, 1U);
  ASSERT_EQ(beam_status.sr_slot_offset, 0U);
  ASSERT_EQ(beam_status.sr_slot_period, 1U);
  ASSERT_EQ(beam_status.srs_slot_offset, 0U);
  ASSERT_EQ(beam_status.srs_slot_period, 1U);

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());
  expect_and_ack_ntn_slot_update(env, du_idx.value(), du_ue_id, ue_ctx->cu_ue_id.value(), to_rnti(0x4601));
}

TEST(cu_cp_ntn_mobility_test, initial_context_setup_releases_analog_access_without_digital_service)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_access_service_layer_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beam_cells(env, {0, 1});
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  const ue_index_t ue_index = finish_ntn_access_only_registration(env,
                                                                  du_idx.value(),
                                                                  cu_up_idx.value(),
                                                                  du_ue_id,
                                                                  to_rnti(0x4601),
                                                                  amf_ue_id_t::min);
  ASSERT_NE(ue_index, ue_index_t::invalid);

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_access_active_ues, 0U);
  EXPECT_EQ(runtime.nof_ntn_control_only_ues, 1U);
  EXPECT_EQ(runtime.nof_ntn_analog_released_ues, 1U);
  EXPECT_EQ(runtime.nof_ntn_digital_service_bound_ues, 0U);
  EXPECT_EQ(runtime.nof_ntn_service_bound_ues, 0U);
  EXPECT_EQ(runtime.nof_loaded_service_beams, 0U);
  EXPECT_EQ(runtime.nof_loaded_digital_service_beams, 0U);

  const cu_cp_ntn_beam_status access_beam =
      find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0001");
  EXPECT_EQ(access_beam.state, cu_cp_ntn_beam_assignment_state::candidate);
  EXPECT_EQ(access_beam.nof_ues, 0U);
  EXPECT_EQ(access_beam.nof_drbs, 0U);
  EXPECT_EQ(access_beam.sr_slot_period, 0U);
  EXPECT_EQ(access_beam.srs_slot_period, 0U);

  const cu_cp_ntn_ue_status ue_status = find_ue_status(ntn_handler.get_current_ntn_ue_status(), ue_index);
  EXPECT_EQ(ue_status.ntn_runtime_state, "control_only");
  EXPECT_EQ(ue_status.access_layer_state, "released_after_ics");
  EXPECT_TRUE(ue_status.analog_access_released);
  ASSERT_TRUE(ue_status.access_analog_beam_id.has_value());
  EXPECT_EQ(ue_status.access_analog_beam_id.value(), "ANALOG-ACCESS-001");
  EXPECT_EQ(ue_status.access_du_index, uint_to_du_index(du_idx.value()));
  EXPECT_EQ(ue_status.service_layer_state, "none");
  EXPECT_FALSE(ue_status.service_digital_beam_id.has_value());
  EXPECT_FALSE(ue_status.has_ul_slot_request);
  EXPECT_EQ(ue_status.access_rnti_ownership_state, "released_after_ics");
  EXPECT_EQ(ue_status.access_rnti_ownership_reason, "released_after_ics");
  EXPECT_EQ(ue_status.digital_slot_intent_state, "none");

  const ntn_beam_service_resource_snapshot resource_snapshot =
      ntn_handler.get_current_ntn_beam_service_resource_snapshot();
  EXPECT_EQ(resource_snapshot.nof_access_rnti_owned, 0U);
  EXPECT_EQ(resource_snapshot.nof_access_rnti_conflicts, 0U);
  ASSERT_EQ(resource_snapshot.access_rnti_ownerships.size(), 1U);
  EXPECT_EQ(resource_snapshot.access_rnti_ownerships.front().ue_index, ue_index);
  EXPECT_EQ(resource_snapshot.access_rnti_ownerships.front().state, "released_after_ics");
  EXPECT_TRUE(resource_snapshot.digital_slot_intents.empty());

  const cu_cp_ntn_antenna_intent_snapshot intent =
      ntn_handler.get_current_ntn_antenna_intent_snapshot();
  auto analog_intent = std::find_if(intent.analog_access_intents.begin(),
                                    intent.analog_access_intents.end(),
                                    [](const cu_cp_ntn_analog_access_intent& entry) {
                                      return entry.analog_beam_id == "ANALOG-ACCESS-001";
                                    });
  ASSERT_NE(analog_intent, intent.analog_access_intents.end());
  EXPECT_EQ(analog_intent->nof_access_active_ues, 0U);
  EXPECT_TRUE(intent.digital_service_intents.empty());

  f1ap_message f1ap_pdu;
  EXPECT_FALSE(env.wait_for_f1ap_tx_pdu(du_idx.value(), f1ap_pdu, std::chrono::milliseconds{20}));
}

TEST(cu_cp_ntn_mobility_test, rrc_setup_creates_active_analog_access_context_before_ics)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_access_service_layer_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beam_cells(env, {0, 1});
  ASSERT_TRUE(du_idx.has_value());
  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  ASSERT_TRUE(env.connect_new_ue(du_idx.value(), du_ue_id, to_rnti(0x4601)));

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());
  const ue_index_t ue_index = uint_to_ue_index(gnb_cu_ue_f1ap_id_to_uint(ue_ctx->cu_ue_id.value()));

  const cu_cp_ntn_ue_status ue_status = find_ue_status(ntn_handler.get_current_ntn_ue_status(), ue_index);
  EXPECT_EQ(ue_status.ntn_runtime_state, "access_active");
  EXPECT_EQ(ue_status.access_layer_state, "access_active");
  EXPECT_FALSE(ue_status.analog_access_released);
  ASSERT_TRUE(ue_status.access_analog_beam_id.has_value());
  EXPECT_EQ(ue_status.access_analog_beam_id.value(), "ANALOG-ACCESS-001");
  EXPECT_EQ(ue_status.service_layer_state, "none");

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_access_active_ues, 1U);
  EXPECT_EQ(runtime.nof_ntn_control_only_ues, 0U);
  EXPECT_EQ(runtime.nof_ntn_analog_released_ues, 0U);
}

TEST(cu_cp_ntn_mobility_test, first_pdu_setup_binds_digital_service_by_latest_location)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_access_service_layer_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beam_cells(env, {0, 1});
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  const ue_index_t ue_index = finish_ntn_access_only_registration(env,
                                                                  du_idx.value(),
                                                                  cu_up_idx.value(),
                                                                  du_ue_id,
                                                                  to_rnti(0x4601),
                                                                  amf_ue_id_t::min,
                                                                  make_supported_ntn_ngso_ue_capability_info_pdu());
  ASSERT_NE(ue_index, ue_index_t::invalid);

  ntn_ue_location_report report = make_ntn_location_report(ue_index, make_default_env_nci(0));
  report.longitude_deg         = 1.0;
  cu_cp_measurement_handler& meas_handler = get_cu_cp_impl(env)->get_cu_cp_measurement_handler();
  meas_handler.handle_rrc_ue_location_report_outcome(ue_index, ntn_rrc_ue_location_report_outcome::received);
  meas_handler.handle_rrc_ue_location_report_outcome(ue_index, ntn_rrc_ue_location_report_outcome::decoded);
  meas_handler.handle_ue_location_report(report);
  env.drain_f1ap_resource_coordination_requests(du_idx.value());

  ASSERT_TRUE(env.request_pdu_session_resource_setup(du_idx.value(), cu_up_idx.value(), du_ue_id));
  ASSERT_TRUE(setup_pdu_session_with_five_qi(env,
                                             du_idx.value(),
                                             cu_up_idx.value(),
                                             du_ue_id,
                                             to_rnti(0x4601),
                                             int_to_gnb_cu_up_ue_e1ap_id(0),
                                             pdu_session_id_t::min,
                                             drb_id_t::drb1,
                                             uint_to_qos_flow_id(0),
                                             9,
                                             true));

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());
  expect_and_ack_ntn_slot_update(env, du_idx.value(), du_ue_id, ue_ctx->cu_ue_id.value(), to_rnti(0x4601));
  ASSERT_TRUE(env.tick_until(std::chrono::milliseconds{100}, [&]() {
    const cu_cp_ntn_ue_status status = find_ue_status(ntn_handler.get_current_ntn_ue_status(), ue_index);
    return status.digital_slot_intent_state == "applied_by_du";
  }));
  env.drain_f1ap_resource_coordination_requests(du_idx.value());

  const cu_cp_ntn_beam_status access_beam =
      find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0001");
  const cu_cp_ntn_beam_status service_beam =
      find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0002");
  EXPECT_EQ(access_beam.state, cu_cp_ntn_beam_assignment_state::candidate);
  EXPECT_EQ(access_beam.nof_drbs, 0U);
  EXPECT_EQ(service_beam.state, cu_cp_ntn_beam_assignment_state::active_loaded);
  EXPECT_EQ(service_beam.nof_ues, 1U);
  EXPECT_EQ(service_beam.nof_drbs, 1U);

  const cu_cp_ntn_ue_status ue_status = find_ue_status(ntn_handler.get_current_ntn_ue_status(), ue_index);
  EXPECT_EQ(ue_status.ntn_runtime_state, "service_bound");
  EXPECT_EQ(ue_status.access_layer_state, "released_after_ics");
  EXPECT_TRUE(ue_status.analog_access_released);
  ASSERT_TRUE(ue_status.access_analog_beam_id.has_value());
  EXPECT_EQ(ue_status.access_analog_beam_id.value(), "ANALOG-ACCESS-001");
  EXPECT_EQ(ue_status.service_layer_state, "service_bound");
  ASSERT_TRUE(ue_status.service_digital_beam_id.has_value());
  EXPECT_EQ(ue_status.service_digital_beam_id.value(), "CN-BEAM-0002");
  EXPECT_EQ(ue_status.service_binding_source, "location");
  EXPECT_TRUE(ue_status.has_ul_slot_request);
  EXPECT_EQ(ue_status.access_rnti_ownership_state, "released_after_ics");
  EXPECT_EQ(ue_status.digital_slot_intent_state, "applied_by_du");
  EXPECT_EQ(ue_status.digital_slot_intent_reason, "applied");

  const ntn_beam_service_resource_snapshot resource_snapshot =
      ntn_handler.get_current_ntn_beam_service_resource_snapshot();
  EXPECT_EQ(resource_snapshot.nof_access_rnti_owned, 0U);
  EXPECT_EQ(resource_snapshot.nof_digital_slot_active, 1U);
  EXPECT_EQ(resource_snapshot.nof_digital_slot_applied_by_du, 1U);
  ASSERT_EQ(resource_snapshot.digital_slot_intents.size(), 1U);
  EXPECT_EQ(resource_snapshot.digital_slot_intents.front().ue_index, ue_index);
  EXPECT_EQ(resource_snapshot.digital_slot_intents.front().state, "applied_by_du");

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_access_active_ues, 0U);
  EXPECT_EQ(runtime.nof_ntn_control_only_ues, 0U);
  EXPECT_EQ(runtime.nof_ntn_analog_released_ues, 1U);
  EXPECT_EQ(runtime.nof_ntn_digital_service_bound_ues, 1U);
  EXPECT_EQ(runtime.nof_ntn_service_bound_ues, 1U);
  EXPECT_EQ(runtime.nof_ntn_location_bound_service_ues, 1U);
  EXPECT_EQ(runtime.nof_ntn_access_cell_fallback_service_ues, 0U);
  EXPECT_EQ(runtime.nof_ntn_rrc_location_reports_received, 1U);
  EXPECT_EQ(runtime.nof_ntn_rrc_location_reports_decoded, 1U);
  EXPECT_EQ(runtime.nof_ntn_rrc_location_reports_unsupported, 0U);
  EXPECT_EQ(runtime.nof_ntn_rrc_location_reports_decode_failed, 0U);
  EXPECT_EQ(runtime.nof_ntn_location_reports_accepted, 1U);
  EXPECT_EQ(runtime.nof_ntn_location_reports_rejected, 0U);

  const cu_cp_ntn_antenna_intent_snapshot intent =
      ntn_handler.get_current_ntn_antenna_intent_snapshot();
  auto digital_intent = std::find_if(intent.digital_service_intents.begin(),
                                     intent.digital_service_intents.end(),
                                     [](const cu_cp_ntn_digital_service_intent& entry) {
      return entry.digital_beam_id == "CN-BEAM-0002";
    });
  ASSERT_NE(digital_intent, intent.digital_service_intents.end());
  EXPECT_EQ(digital_intent->nof_ues, 1U);
  EXPECT_EQ(digital_intent->nof_drbs, 1U);
  EXPECT_EQ(digital_intent->nof_antenna_slots, 1U);
}

TEST(cu_cp_ntn_mobility_test, service_bound_ntn_ue_enters_stale_when_location_ages_out)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_access_service_layer_ntn_mobility_config();
  params.ntn_location_mobility->measurement_report_period = std::chrono::milliseconds{10};
  params.ntn_location_mobility->location_max_age          = std::chrono::milliseconds{20};
  params.ntn_location_mobility->location_lost_release_grace_period = std::chrono::milliseconds{500};
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  const std::optional<service_bound_ntn_ue_context> ue =
      setup_service_bound_ntn_ue(env, ntn_handler, true);
  ASSERT_TRUE(ue.has_value());

  ntn_ue_location_report fresh_report = make_ntn_location_report(ue->ue_index, make_default_env_nci(0));
  get_cu_cp_impl(env)->get_cu_cp_measurement_handler().handle_ue_location_report(fresh_report);

  std::this_thread::sleep_for(std::chrono::milliseconds{100});
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));
  env.drain_f1ap_resource_coordination_requests(ue->du_idx);

  const cu_cp_ntn_ue_status ue_status = find_ue_status(ntn_handler.get_current_ntn_ue_status(), ue->ue_index);
  EXPECT_EQ(ue_status.ntn_location_freshness_state, "stale");
  EXPECT_EQ(ue_status.ntn_location_freshness_reason, "last_location_expired");
  EXPECT_FALSE(ue_status.ntn_location_release_pending);

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_location_fresh_ues, 0U);
  EXPECT_EQ(runtime.nof_ntn_location_stale_ues, 1U);
  EXPECT_EQ(runtime.nof_ntn_location_release_pending_ues, 0U);
  EXPECT_EQ(runtime.nof_ntn_location_watchdog_release_requested, 0U);
  EXPECT_EQ(runtime.nof_ntn_location_watchdog_release_scheduled, 0U);

  expect_no_f1ap_ue_setup_or_release(env, ue->du_idx);
}

TEST(cu_cp_ntn_mobility_test, stale_service_bound_ntn_ue_is_released_after_grace)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_access_service_layer_ntn_mobility_config();
  params.ntn_location_mobility->measurement_report_period = std::chrono::milliseconds{10};
  params.ntn_location_mobility->location_max_age          = std::chrono::milliseconds{20};
  params.ntn_location_mobility->location_lost_release_grace_period = std::chrono::milliseconds{30};
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  const std::optional<service_bound_ntn_ue_context> ue =
      setup_service_bound_ntn_ue(env, ntn_handler, true);
  ASSERT_TRUE(ue.has_value());

  ntn_ue_location_report fresh_report = make_ntn_location_report(ue->ue_index, make_default_env_nci(0));
  get_cu_cp_impl(env)->get_cu_cp_measurement_handler().handle_ue_location_report(fresh_report);

  std::this_thread::sleep_for(std::chrono::milliseconds{80});
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));
  env.drain_f1ap_resource_coordination_requests(ue->du_idx);

  const cu_cp_ntn_runtime_status release_requested_runtime = ntn_handler.get_current_ntn_runtime_status();
  ASSERT_EQ(release_requested_runtime.nof_ntn_location_release_pending_ues, 1U);
  ASSERT_EQ(release_requested_runtime.nof_ntn_location_watchdog_release_requested, 1U);
  ASSERT_EQ(release_requested_runtime.nof_ntn_location_watchdog_release_scheduled, 1U);

  e1ap_message e1ap_pdu;
  ASSERT_TRUE(env.wait_for_e1ap_tx_pdu(ue->cu_up_idx, e1ap_pdu, std::chrono::milliseconds{1000}));
  ASSERT_TRUE(test_helpers::is_valid_bearer_context_release_command(e1ap_pdu));
  env.get_cu_up(ue->cu_up_idx).push_tx_pdu(
      generate_bearer_context_release_complete(ue->cu_cp_e1ap_id, ue->cu_up_e1ap_id));

  f1ap_message f1ap_pdu;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu(ue->du_idx, f1ap_pdu, std::chrono::milliseconds{1000}));
  ASSERT_TRUE(test_helpers::is_valid_ue_context_release_command(f1ap_pdu));
  const auto& rel_cmd = f1ap_pdu.pdu.init_msg().value.ue_context_release_cmd();

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_location_release_pending_ues, 1U);
  EXPECT_EQ(runtime.nof_ntn_location_watchdog_release_requested, 1U);
  EXPECT_EQ(runtime.nof_ntn_location_watchdog_release_scheduled, 1U);
  EXPECT_EQ(runtime.nof_ntn_location_watchdog_release_skipped, 0U);
  EXPECT_EQ(runtime.last_ntn_location_watchdog_release_reason, "last_location_expired");

  env.get_du(ue->du_idx)
      .push_ul_pdu(test_helpers::generate_ue_context_release_complete(
          int_to_gnb_cu_ue_f1ap_id(rel_cmd->gnb_cu_ue_f1ap_id),
          int_to_gnb_du_ue_f1ap_id(rel_cmd->gnb_du_ue_f1ap_id)));
  ASSERT_TRUE(env.tick_until(std::chrono::milliseconds{1000}, [&]() {
    return env.get_cu_cp().get_metrics_handler().request_metrics_report().ues.empty();
  }));
  env.drain_f1ap_resource_coordination_requests(ue->du_idx);
}

TEST(cu_cp_ntn_mobility_test, missing_location_service_bound_ue_requests_refresh_then_releases_after_grace)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_access_service_layer_ntn_mobility_config();
  params.ntn_location_mobility->measurement_report_period = std::chrono::milliseconds{10};
  params.ntn_location_mobility->location_max_age          = std::chrono::milliseconds{20};
  params.ntn_location_mobility->location_lost_release_grace_period = std::chrono::milliseconds{30};
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  const std::optional<service_bound_ntn_ue_context> ue =
      setup_service_bound_ntn_ue(env, ntn_handler, false);
  ASSERT_TRUE(ue.has_value());

  const cu_cp_ntn_ue_status missing_status = find_ue_status(ntn_handler.get_current_ntn_ue_status(), ue->ue_index);
  ASSERT_EQ(missing_status.service_layer_state, "service_bound");
  EXPECT_EQ(missing_status.ntn_location_freshness_state, "missing");
  EXPECT_EQ(missing_status.ntn_location_freshness_reason, "missing_location");
  EXPECT_FALSE(missing_status.ntn_location_release_pending);

  const cu_cp_ntn_runtime_status refresh_runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(refresh_runtime.nof_ntn_location_missing_ues, 1U);
  EXPECT_EQ(refresh_runtime.nof_ntn_location_watchdog_release_requested, 0U);
  EXPECT_GE(refresh_runtime.nof_ntn_location_watchdog_refresh_requested, 1U);

  std::this_thread::sleep_for(std::chrono::milliseconds{50});
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));
  env.drain_f1ap_resource_coordination_requests(ue->du_idx);

  const cu_cp_ntn_runtime_status release_requested_runtime = ntn_handler.get_current_ntn_runtime_status();
  ASSERT_EQ(release_requested_runtime.nof_ntn_location_release_pending_ues, 1U);
  ASSERT_EQ(release_requested_runtime.nof_ntn_location_watchdog_release_requested, 1U);
  ASSERT_EQ(release_requested_runtime.nof_ntn_location_watchdog_release_scheduled, 1U);
  ASSERT_EQ(release_requested_runtime.last_ntn_location_watchdog_release_reason, "missing_location");

  e1ap_message e1ap_pdu;
  ASSERT_TRUE(env.wait_for_e1ap_tx_pdu(ue->cu_up_idx, e1ap_pdu, std::chrono::milliseconds{1000}));
  ASSERT_TRUE(test_helpers::is_valid_bearer_context_release_command(e1ap_pdu));
  env.get_cu_up(ue->cu_up_idx).push_tx_pdu(
      generate_bearer_context_release_complete(ue->cu_cp_e1ap_id, ue->cu_up_e1ap_id));

  f1ap_message f1ap_pdu;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu(ue->du_idx, f1ap_pdu, std::chrono::milliseconds{1000}));
  ASSERT_TRUE(test_helpers::is_valid_ue_context_release_command(f1ap_pdu));
  const auto& rel_cmd = f1ap_pdu.pdu.init_msg().value.ue_context_release_cmd();
  env.get_du(ue->du_idx)
      .push_ul_pdu(test_helpers::generate_ue_context_release_complete(
          int_to_gnb_cu_ue_f1ap_id(rel_cmd->gnb_cu_ue_f1ap_id),
          int_to_gnb_du_ue_f1ap_id(rel_cmd->gnb_du_ue_f1ap_id)));
  ASSERT_TRUE(env.tick_until(std::chrono::milliseconds{1000}, [&]() {
    return env.get_cu_cp().get_metrics_handler().request_metrics_report().ues.empty();
  }));
  env.drain_f1ap_resource_coordination_requests(ue->du_idx);
}

TEST(cu_cp_ntn_mobility_test, fresh_location_report_clears_stale_and_cancels_release_timer)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_access_service_layer_ntn_mobility_config();
  params.ntn_location_mobility->measurement_report_period = std::chrono::milliseconds{10};
  params.ntn_location_mobility->location_max_age          = std::chrono::milliseconds{20};
  params.ntn_location_mobility->location_lost_release_grace_period = std::chrono::milliseconds{200};
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  const std::optional<service_bound_ntn_ue_context> ue =
      setup_service_bound_ntn_ue(env, ntn_handler, true);
  ASSERT_TRUE(ue.has_value());

  ntn_ue_location_report fresh_report = make_ntn_location_report(ue->ue_index, make_default_env_nci(0));
  get_cu_cp_impl(env)->get_cu_cp_measurement_handler().handle_ue_location_report(fresh_report);
  std::this_thread::sleep_for(std::chrono::milliseconds{60});
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));
  env.drain_f1ap_resource_coordination_requests(ue->du_idx);
  EXPECT_EQ(find_ue_status(ntn_handler.get_current_ntn_ue_status(), ue->ue_index).ntn_location_freshness_state,
            "stale");

  ntn_ue_location_report refreshed_report = make_ntn_location_report(ue->ue_index, make_default_env_nci(0));
  get_cu_cp_impl(env)->get_cu_cp_measurement_handler().handle_ue_location_report(refreshed_report);
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));
  env.drain_f1ap_resource_coordination_requests(ue->du_idx);

  const cu_cp_ntn_ue_status ue_status = find_ue_status(ntn_handler.get_current_ntn_ue_status(), ue->ue_index);
  EXPECT_EQ(ue_status.ntn_location_freshness_state, "fresh");
  EXPECT_EQ(ue_status.ntn_location_freshness_reason, "fresh");
  EXPECT_FALSE(ue_status.ntn_location_release_pending);

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_location_fresh_ues, 1U);
  EXPECT_EQ(runtime.nof_ntn_location_stale_ues, 0U);
  EXPECT_EQ(runtime.nof_ntn_location_release_pending_ues, 0U);
  EXPECT_EQ(runtime.nof_ntn_location_watchdog_release_requested, 0U);
  expect_no_f1ap_ue_setup_or_release(env, ue->du_idx);
}

TEST(cu_cp_ntn_mobility_test, location_watchdog_skips_release_in_progress_ue)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_access_service_layer_ntn_mobility_config();
  params.ntn_location_mobility->measurement_report_period = std::chrono::milliseconds{10};
  params.ntn_location_mobility->location_max_age          = std::chrono::milliseconds{20};
  params.ntn_location_mobility->location_lost_release_grace_period = std::chrono::milliseconds{30};
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  const std::optional<service_bound_ntn_ue_context> ue =
      setup_service_bound_ntn_ue(env, ntn_handler, true);
  ASSERT_TRUE(ue.has_value());

  const cu_cp_ntn_ue_status before_release_status =
      find_ue_status(ntn_handler.get_current_ntn_ue_status(), ue->ue_index);
  ASSERT_EQ(before_release_status.service_layer_state, "service_bound");
  ASSERT_TRUE(before_release_status.service_digital_beam_id.has_value());

  ntn_ue_location_report fresh_report =
      make_ntn_location_report(ue->ue_index, make_default_env_nci(0), std::chrono::steady_clock::now());
  get_cu_cp_impl(env)->get_cu_cp_measurement_handler().handle_ue_location_report(fresh_report);
  env.drain_f1ap_resource_coordination_requests(ue->du_idx);

  ntn_service_switch_over_event event;
  event.event_id          = 109;
  event.type              = ntn_service_switch_over_type::hard;
  event.source            = ntn_service_switch_over_source::operator_command;
  event.policy            = ntn_service_switch_over_policy::release_allowed;
  event.affected_beam_ids = {before_release_status.service_digital_beam_id.value()};
  ASSERT_TRUE(ntn_handler.handle_ntn_service_switch_over_event(event));

  std::this_thread::sleep_for(std::chrono::milliseconds{80});
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));
  env.drain_f1ap_resource_coordination_requests(ue->du_idx);

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_release_allowed_ues_scheduled, 1U);
  EXPECT_EQ(runtime.nof_ntn_location_watchdog_release_requested, 1U);
  EXPECT_EQ(runtime.nof_ntn_location_watchdog_release_scheduled, 0U);
  EXPECT_EQ(runtime.nof_ntn_location_watchdog_release_skipped, 1U);

  e1ap_message e1ap_pdu;
  ASSERT_TRUE(env.wait_for_e1ap_tx_pdu(ue->cu_up_idx, e1ap_pdu, std::chrono::milliseconds{1000}));
  ASSERT_TRUE(test_helpers::is_valid_bearer_context_release_command(e1ap_pdu));
  env.get_cu_up(ue->cu_up_idx).push_tx_pdu(
      generate_bearer_context_release_complete(ue->cu_cp_e1ap_id, ue->cu_up_e1ap_id));

  f1ap_message f1ap_pdu;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu(ue->du_idx, f1ap_pdu, std::chrono::milliseconds{1000}));
  ASSERT_TRUE(test_helpers::is_valid_ue_context_release_command(f1ap_pdu));
  const auto& rel_cmd = f1ap_pdu.pdu.init_msg().value.ue_context_release_cmd();
  env.get_du(ue->du_idx)
      .push_ul_pdu(test_helpers::generate_ue_context_release_complete(
          int_to_gnb_cu_ue_f1ap_id(rel_cmd->gnb_cu_ue_f1ap_id),
          int_to_gnb_du_ue_f1ap_id(rel_cmd->gnb_du_ue_f1ap_id)));
  ASSERT_TRUE(env.tick_until(std::chrono::milliseconds{1000}, [&]() {
    return env.get_cu_cp().get_metrics_handler().request_metrics_report().ues.empty();
  }));
  env.drain_f1ap_resource_coordination_requests(ue->du_idx);
}

TEST(cu_cp_ntn_mobility_test, access_only_or_control_only_ue_is_not_released_by_location_watchdog)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_access_service_layer_ntn_mobility_config();
  params.ntn_location_mobility->measurement_report_period = std::chrono::milliseconds{10};
  params.ntn_location_mobility->location_max_age          = std::chrono::milliseconds{20};
  params.ntn_location_mobility->location_lost_release_grace_period = std::chrono::milliseconds{30};
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beam_cells(env, {0, 1});
  ASSERT_TRUE(du_idx.has_value());
  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  const ue_index_t ue_index = finish_ntn_access_only_registration(env,
                                                                  du_idx.value(),
                                                                  cu_up_idx.value(),
                                                                  du_ue_id,
                                                                  to_rnti(0x4601),
                                                                  amf_ue_id_t::min,
                                                                  make_supported_ntn_ngso_ue_capability_info_pdu());
  ASSERT_NE(ue_index, ue_index_t::invalid);

  std::this_thread::sleep_for(std::chrono::milliseconds{80});
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));
  env.drain_f1ap_resource_coordination_requests(du_idx.value());

  const cu_cp_ntn_ue_status ue_status = find_ue_status(ntn_handler.get_current_ntn_ue_status(), ue_index);
  EXPECT_EQ(ue_status.ntn_runtime_state, "control_only");
  EXPECT_EQ(ue_status.ntn_location_freshness_state, "not_required");

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_location_missing_ues, 0U);
  EXPECT_EQ(runtime.nof_ntn_location_watchdog_release_requested, 0U);
  EXPECT_EQ(runtime.nof_ntn_location_watchdog_release_scheduled, 0U);
  expect_no_f1ap_ue_setup_or_release(env, du_idx.value());
}

TEST(cu_cp_ntn_mobility_test, access_only_ntn_ue_does_not_request_common_location_info)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_access_service_layer_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beam_cells(env, {0, 1});
  ASSERT_TRUE(du_idx.has_value());
  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  const ue_index_t ue_index = finish_ntn_access_only_registration(env,
                                                                  du_idx.value(),
                                                                  cu_up_idx.value(),
                                                                  du_ue_id,
                                                                  to_rnti(0x4601),
                                                                  amf_ue_id_t::min,
                                                                  make_supported_ntn_ngso_ue_capability_info_pdu());
  ASSERT_NE(ue_index, ue_index_t::invalid);

  const cu_cp_ntn_runtime_status before_request = ntn_handler.get_current_ntn_runtime_status();
  const std::optional<rrc_meas_cfg> meas_cfg =
      get_cu_cp_impl(env)->get_cu_cp_measurement_handler().handle_measurement_config_request(ue_index,
                                                                                             make_default_env_nci(0));
  EXPECT_FALSE(meas_config_requests_common_location_info(meas_cfg));

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_rrc_location_request_configs_included,
            before_request.nof_ntn_rrc_location_request_configs_included);
  EXPECT_EQ(runtime.nof_ntn_rrc_location_request_configs_skipped_capability,
            before_request.nof_ntn_rrc_location_request_configs_skipped_capability);
  EXPECT_EQ(runtime.nof_ntn_rrc_location_request_configs_skipped_state,
            before_request.nof_ntn_rrc_location_request_configs_skipped_state + 1U);
  EXPECT_EQ(runtime.nof_ntn_rrc_location_request_desired_ues, 0U);
  EXPECT_EQ(runtime.nof_ntn_rrc_location_request_configured_ues, 0U);
}

TEST(cu_cp_ntn_mobility_test, rrc_requested_location_report_reaches_cu_cp_location_path)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_access_service_layer_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beam_cells(env, {0, 1});
  ASSERT_TRUE(du_idx.has_value());
  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  const ue_index_t ue_index = finish_ntn_access_only_registration(env,
                                                                  du_idx.value(),
                                                                  cu_up_idx.value(),
                                                                  du_ue_id,
                                                                  to_rnti(0x4601),
                                                                  amf_ue_id_t::min,
                                                                  make_supported_ntn_ngso_ue_capability_info_pdu());
  ASSERT_NE(ue_index, ue_index_t::invalid);
  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());

  ngap_location_reporting_control control;
  control.ue_index               = ue_index;
  control.request_type.event_type = ngap_location_reporting_event_type::change_of_serving_cell;
  ASSERT_TRUE(get_cu_cp_impl(env)->get_cu_cp_ngap_handler().handle_location_reporting_control(control).accepted);
  f1ap_message location_request_pdu;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu(du_idx.value(), location_request_pdu));
  ASSERT_TRUE(rrc_reconfiguration_requests_common_location_info(location_request_pdu));
  const std::optional<uint8_t> location_request_transaction_id =
      get_rrc_reconfiguration_transaction_id(location_request_pdu);
  ASSERT_TRUE(location_request_transaction_id.has_value());
  env.get_du(du_idx.value())
      .push_ul_pdu(test_helpers::generate_ul_rrc_message_transfer(
          du_ue_id,
          ue_ctx->cu_ue_id.value(),
          srb_id_t::srb1,
          generate_rrc_reconfiguration_complete_pdu(location_request_transaction_id.value(), 7)));

  const std::optional<rrc_meas_cfg> meas_cfg =
      get_cu_cp_impl(env)->get_cu_cp_measurement_handler().handle_measurement_config_request(ue_index,
                                                                                             make_default_env_nci(0));
  ASSERT_TRUE(meas_config_requests_common_location_info(meas_cfg));

  const cu_cp_ntn_runtime_status before_report = ntn_handler.get_current_ntn_runtime_status();
  byte_buffer                    meas_report_pdu = generate_measurement_report_with_location_coordinate_pdu("10400000C00000");
  env.get_du(du_idx.value())
      .push_ul_pdu(test_helpers::generate_ul_rrc_message_transfer(
          du_ue_id,
          ue_ctx->cu_ue_id.value(),
          srb_id_t::srb1,
          make_integrity_protected_srb1_ul_pdcp_pdu(6, std::move(meas_report_pdu))));
  env.drain_f1ap_resource_coordination_requests(du_idx.value());

  ASSERT_TRUE(env.tick_until(std::chrono::milliseconds{100}, [&]() {
    return ntn_handler.get_current_ntn_runtime_status().nof_ntn_location_reports_accepted ==
           before_report.nof_ntn_location_reports_accepted + 1U;
  }));

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_rrc_location_reports_received, before_report.nof_ntn_rrc_location_reports_received + 1U);
  EXPECT_EQ(runtime.nof_ntn_rrc_location_reports_decoded, before_report.nof_ntn_rrc_location_reports_decoded + 1U);
  EXPECT_EQ(runtime.nof_ntn_location_reports_rejected, before_report.nof_ntn_location_reports_rejected);

  const cu_cp_ntn_ue_status ue_status = find_ue_status(ntn_handler.get_current_ntn_ue_status(), ue_index);
  ASSERT_TRUE(ue_status.last_location.has_value());
  EXPECT_NEAR(ue_status.last_location->latitude_deg, 45.0, 1e-5);
  EXPECT_NEAR(ue_status.last_location->longitude_deg, -90.0, 1e-5);
  EXPECT_EQ(ue_status.last_location->source, ntn_ue_location_report_source::measurement_report);
}

TEST(cu_cp_ntn_mobility_test, unsupported_ntn_ue_meas_config_does_not_request_common_location_info)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_access_service_layer_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beam_cells(env, {0, 1});
  ASSERT_TRUE(du_idx.has_value());
  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  const ue_index_t ue_index =
      finish_ntn_access_only_registration(env, du_idx.value(), cu_up_idx.value(), du_ue_id, to_rnti(0x4601), amf_ue_id_t::min);
  ASSERT_NE(ue_index, ue_index_t::invalid);

  const cu_cp_ntn_runtime_status before_request = ntn_handler.get_current_ntn_runtime_status();
  const std::optional<rrc_meas_cfg> meas_cfg =
      get_cu_cp_impl(env)->get_cu_cp_measurement_handler().handle_measurement_config_request(ue_index,
                                                                                             make_default_env_nci(0));
  EXPECT_FALSE(meas_config_requests_common_location_info(meas_cfg));

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_rrc_location_request_configs_included,
            before_request.nof_ntn_rrc_location_request_configs_included);
  EXPECT_EQ(runtime.nof_ntn_rrc_location_request_configs_skipped_capability,
            before_request.nof_ntn_rrc_location_request_configs_skipped_capability + 1U);
}

TEST(cu_cp_ntn_mobility_test, profile_blocked_ntn_ue_meas_config_does_not_request_common_location_info)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_access_service_layer_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beam_cells(env, {0, 1});
  ASSERT_TRUE(du_idx.has_value());
  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  const ue_index_t ue_index = finish_ntn_access_only_registration(env,
                                                                  du_idx.value(),
                                                                  cu_up_idx.value(),
                                                                  du_ue_id,
                                                                  to_rnti(0x4601),
                                                                  amf_ue_id_t::min,
                                                                  make_supported_ntn_gso_ue_capability_info_pdu());
  ASSERT_NE(ue_index, ue_index_t::invalid);

  const cu_cp_ntn_runtime_status before_request = ntn_handler.get_current_ntn_runtime_status();
  const std::optional<rrc_meas_cfg> meas_cfg =
      get_cu_cp_impl(env)->get_cu_cp_measurement_handler().handle_measurement_config_request(ue_index,
                                                                                             make_default_env_nci(0));
  EXPECT_FALSE(meas_config_requests_common_location_info(meas_cfg));

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_rrc_location_request_configs_included,
            before_request.nof_ntn_rrc_location_request_configs_included);
  EXPECT_EQ(runtime.nof_ntn_rrc_location_request_configs_skipped_capability,
            before_request.nof_ntn_rrc_location_request_configs_skipped_capability + 1U);
}

TEST(cu_cp_ntn_mobility_test, ntn_location_disabled_does_not_request_common_location_info)
{
  cu_cp_test_environment env;
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = env.connect_new_du();
  ASSERT_TRUE(du_idx.has_value());
  ASSERT_TRUE(env.run_f1_setup(du_idx.value()));
  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));
  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  const ue_index_t ue_index = finish_ntn_access_only_registration(env,
                                                                  du_idx.value(),
                                                                  cu_up_idx.value(),
                                                                  du_ue_id,
                                                                  to_rnti(0x4601),
                                                                  amf_ue_id_t::min,
                                                                  make_supported_ntn_ngso_ue_capability_info_pdu());
  ASSERT_NE(ue_index, ue_index_t::invalid);

  const cu_cp_ntn_runtime_status before_request = ntn_handler.get_current_ntn_runtime_status();
  const std::optional<rrc_meas_cfg> meas_cfg =
      get_cu_cp_impl(env)->get_cu_cp_measurement_handler().handle_measurement_config_request(ue_index,
                                                                                             make_default_env_nci(0));
  EXPECT_FALSE(meas_config_requests_common_location_info(meas_cfg));

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_rrc_location_request_configs_included,
            before_request.nof_ntn_rrc_location_request_configs_included);
  EXPECT_EQ(runtime.nof_ntn_rrc_location_request_configs_skipped_capability,
            before_request.nof_ntn_rrc_location_request_configs_skipped_capability);
}

TEST(cu_cp_ntn_mobility_test, service_binding_requests_common_location_info_reconfiguration)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_access_service_layer_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beam_cells(env, {0, 1});
  ASSERT_TRUE(du_idx.has_value());
  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  const rnti_t              crnti    = to_rnti(0x4601);
  const ue_index_t ue_index = finish_ntn_access_only_registration(env,
                                                                  du_idx.value(),
                                                                  cu_up_idx.value(),
                                                                  du_ue_id,
                                                                  crnti,
                                                                  amf_ue_id_t::min,
                                                                  make_supported_ntn_ngso_ue_capability_info_pdu());
  ASSERT_NE(ue_index, ue_index_t::invalid);

  const cu_cp_ntn_runtime_status before_setup = ntn_handler.get_current_ntn_runtime_status();
  ASSERT_TRUE(env.request_pdu_session_resource_setup(du_idx.value(), cu_up_idx.value(), du_ue_id));
  ASSERT_TRUE(setup_pdu_session_with_five_qi(env,
                                             du_idx.value(),
                                             cu_up_idx.value(),
                                             du_ue_id,
                                             crnti,
                                             int_to_gnb_cu_up_ue_e1ap_id(0),
                                             pdu_session_id_t::min,
                                             drb_id_t::drb1,
                                             uint_to_qos_flow_id(0),
                                             9,
                                             true));

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());
  expect_and_ack_ntn_slot_update(env, du_idx.value(), du_ue_id, ue_ctx->cu_ue_id.value(), crnti);
  env.drain_f1ap_resource_coordination_requests(du_idx.value());

  const cu_cp_ntn_ue_status ue_status = find_ue_status(ntn_handler.get_current_ntn_ue_status(), ue_index);
  ASSERT_EQ(ue_status.service_layer_state, "service_bound");

  const std::optional<rrc_meas_cfg> meas_cfg =
      get_cu_cp_impl(env)->get_cu_cp_measurement_handler().handle_measurement_config_request(ue_index,
                                                                                             make_default_env_nci(0));
  EXPECT_TRUE(meas_config_requests_common_location_info(meas_cfg));

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_rrc_location_request_configs_included,
            before_setup.nof_ntn_rrc_location_request_configs_included + 1U);
  EXPECT_EQ(runtime.nof_ntn_rrc_location_request_desired_ues, 1U);
  EXPECT_EQ(runtime.nof_ntn_rrc_location_request_configured_ues, 1U);
  EXPECT_EQ(runtime.nof_ntn_rrc_location_request_pending_ues, 0U);
}

TEST(cu_cp_ntn_mobility_test, amf_location_reporting_control_requests_location_for_control_only_ue)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_access_service_layer_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beam_cells(env, {0, 1});
  ASSERT_TRUE(du_idx.has_value());
  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  const ue_index_t ue_index = finish_ntn_access_only_registration(env,
                                                                  du_idx.value(),
                                                                  cu_up_idx.value(),
                                                                  du_ue_id,
                                                                  to_rnti(0x4601),
                                                                  amf_ue_id_t::min,
                                                                  make_supported_ntn_ngso_ue_capability_info_pdu());
  ASSERT_NE(ue_index, ue_index_t::invalid);
  ASSERT_EQ(find_ue_status(ntn_handler.get_current_ntn_ue_status(), ue_index).service_layer_state, "none");
  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());

  const cu_cp_ntn_runtime_status before_control = ntn_handler.get_current_ntn_runtime_status();
  ngap_location_reporting_control control;
  control.ue_index               = ue_index;
  control.request_type.event_type = ngap_location_reporting_event_type::change_of_serving_cell;
  ASSERT_TRUE(get_cu_cp_impl(env)->get_cu_cp_ngap_handler().handle_location_reporting_control(control).accepted);
  f1ap_message location_request_pdu;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu(du_idx.value(), location_request_pdu));
  ASSERT_TRUE(rrc_reconfiguration_requests_common_location_info(location_request_pdu));
  const std::optional<uint8_t> location_request_transaction_id =
      get_rrc_reconfiguration_transaction_id(location_request_pdu);
  ASSERT_TRUE(location_request_transaction_id.has_value());
  env.get_du(du_idx.value())
      .push_ul_pdu(test_helpers::generate_ul_rrc_message_transfer(
          du_ue_id,
          ue_ctx->cu_ue_id.value(),
          srb_id_t::srb1,
          generate_rrc_reconfiguration_complete_pdu(location_request_transaction_id.value(), 7)));

  const std::optional<rrc_meas_cfg> meas_cfg =
      get_cu_cp_impl(env)->get_cu_cp_measurement_handler().handle_measurement_config_request(ue_index,
                                                                                             make_default_env_nci(0));
  EXPECT_TRUE(meas_config_requests_common_location_info(meas_cfg));

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_rrc_location_request_configs_included,
            before_control.nof_ntn_rrc_location_request_configs_included + 1U);
  EXPECT_EQ(runtime.nof_ntn_rrc_location_request_desired_ues, 1U);
  EXPECT_EQ(runtime.nof_ntn_rrc_location_request_configured_ues, 1U);
}

TEST(cu_cp_ntn_mobility_test, location_request_reconfiguration_is_not_duplicated)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_access_service_layer_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beam_cells(env, {0, 1});
  ASSERT_TRUE(du_idx.has_value());
  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  const rnti_t              crnti    = to_rnti(0x4601);
  const ue_index_t ue_index = finish_ntn_access_only_registration(env,
                                                                  du_idx.value(),
                                                                  cu_up_idx.value(),
                                                                  du_ue_id,
                                                                  crnti,
                                                                  amf_ue_id_t::min,
                                                                  make_supported_ntn_ngso_ue_capability_info_pdu());
  ASSERT_NE(ue_index, ue_index_t::invalid);

  ASSERT_TRUE(env.request_pdu_session_resource_setup(du_idx.value(), cu_up_idx.value(), du_ue_id));
  ASSERT_TRUE(setup_pdu_session_with_five_qi(env,
                                             du_idx.value(),
                                             cu_up_idx.value(),
                                             du_ue_id,
                                             crnti,
                                             int_to_gnb_cu_up_ue_e1ap_id(0),
                                             pdu_session_id_t::min,
                                             drb_id_t::drb1,
                                             uint_to_qos_flow_id(0),
                                             9,
                                             true));

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());
  expect_and_ack_ntn_slot_update(env, du_idx.value(), du_ue_id, ue_ctx->cu_ue_id.value(), crnti);
  env.drain_f1ap_resource_coordination_requests(du_idx.value());

  const cu_cp_ntn_runtime_status before_repeat = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_TRUE(meas_config_requests_common_location_info(
      get_cu_cp_impl(env)->get_cu_cp_measurement_handler().handle_measurement_config_request(ue_index,
                                                                                             make_default_env_nci(0))));
  EXPECT_TRUE(meas_config_requests_common_location_info(
      get_cu_cp_impl(env)->get_cu_cp_measurement_handler().handle_measurement_config_request(ue_index,
                                                                                             make_default_env_nci(0))));

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_rrc_location_request_configs_included,
            before_repeat.nof_ntn_rrc_location_request_configs_included);
  EXPECT_EQ(runtime.nof_ntn_rrc_location_request_reconfig_sent,
            before_repeat.nof_ntn_rrc_location_request_reconfig_sent);
  EXPECT_EQ(runtime.nof_ntn_rrc_location_request_desired_ues, 1U);
  EXPECT_EQ(runtime.nof_ntn_rrc_location_request_configured_ues, 1U);
}

TEST(cu_cp_ntn_mobility_test, service_release_removes_common_location_info_request)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_access_service_layer_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beam_cells(env, {0, 1});
  ASSERT_TRUE(du_idx.has_value());
  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  const rnti_t              crnti    = to_rnti(0x4601);
  const ue_index_t ue_index = finish_ntn_access_only_registration(env,
                                                                  du_idx.value(),
                                                                  cu_up_idx.value(),
                                                                  du_ue_id,
                                                                  crnti,
                                                                  uint_to_amf_ue_id(0),
                                                                  make_supported_ntn_ngso_ue_capability_info_pdu());
  ASSERT_NE(ue_index, ue_index_t::invalid);

  ASSERT_TRUE(env.request_pdu_session_resource_setup(du_idx.value(), cu_up_idx.value(), du_ue_id));
  ASSERT_TRUE(setup_pdu_session_with_five_qi(env,
                                             du_idx.value(),
                                             cu_up_idx.value(),
                                             du_ue_id,
                                             crnti,
                                             int_to_gnb_cu_up_ue_e1ap_id(0),
                                             pdu_session_id_t::min,
                                             drb_id_t::drb1,
                                             uint_to_qos_flow_id(0),
                                             9,
                                             true));

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());
  ASSERT_TRUE(ue_ctx->ran_ue_id.has_value());
  ASSERT_TRUE(ue_ctx->cu_cp_e1ap_id.has_value());
  ASSERT_TRUE(ue_ctx->cu_up_e1ap_id.has_value());
  expect_and_ack_ntn_slot_update(env, du_idx.value(), du_ue_id, ue_ctx->cu_ue_id.value(), crnti);
  env.drain_f1ap_resource_coordination_requests(du_idx.value());

  const cu_cp_ntn_runtime_status before_release = ntn_handler.get_current_ntn_runtime_status();
  ASSERT_EQ(before_release.nof_ntn_rrc_location_request_configured_ues, 1U);

  env.get_amf().push_tx_pdu(generate_valid_pdu_session_resource_release_command(
      uint_to_amf_ue_id(0), ue_ctx->ran_ue_id.value(), pdu_session_id_t::min));

  e1ap_message e1ap_pdu;
  ASSERT_TRUE(env.wait_for_e1ap_tx_pdu(cu_up_idx.value(), e1ap_pdu));
  ASSERT_TRUE(test_helpers::is_valid_bearer_context_release_command(e1ap_pdu));

  env.get_cu_up(cu_up_idx.value())
      .push_tx_pdu(generate_bearer_context_release_complete(ue_ctx->cu_cp_e1ap_id.value(),
                                                            ue_ctx->cu_up_e1ap_id.value()));
  f1ap_message f1ap_pdu;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu(du_idx.value(), f1ap_pdu));
  ASSERT_TRUE(test_helpers::is_valid_ue_context_modification_request(f1ap_pdu));

  env.get_du(du_idx.value())
      .push_ul_pdu(test_helpers::generate_ue_context_modification_response(du_ue_id,
                                                                           ue_ctx->cu_ue_id.value(),
                                                                           crnti,
                                                                           {drb_id_t::drb1},
                                                                           {},
                                                                           test_helpers::create_cell_group_config()));
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu(du_idx.value(), f1ap_pdu));
  ASSERT_TRUE(test_helpers::is_valid_dl_rrc_message_transfer(f1ap_pdu));

  env.get_du(du_idx.value())
      .push_ul_pdu(test_helpers::generate_ul_rrc_message_transfer(
          du_ue_id,
          ue_ctx->cu_ue_id.value(),
          srb_id_t::srb1,
          generate_rrc_reconfiguration_complete_pdu(0, 8)));

  ngap_message ngap_pdu;
  ASSERT_TRUE(env.wait_for_ngap_tx_pdu(ngap_pdu));
  ASSERT_TRUE(test_helpers::is_valid_pdu_session_resource_release_response(ngap_pdu));

  expect_and_ack_ntn_slot_update(env, du_idx.value(), du_ue_id, ue_ctx->cu_ue_id.value(), crnti);

  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu(du_idx.value(), f1ap_pdu));
  ASSERT_TRUE(test_helpers::is_valid_dl_rrc_message_transfer(f1ap_pdu));
  EXPECT_FALSE(rrc_reconfiguration_requests_common_location_info(f1ap_pdu));
  const std::optional<uint8_t> transaction_id = get_rrc_reconfiguration_transaction_id(f1ap_pdu);
  ASSERT_TRUE(transaction_id.has_value());
  env.get_du(du_idx.value())
      .push_ul_pdu(test_helpers::generate_ul_rrc_message_transfer(
          du_ue_id,
          ue_ctx->cu_ue_id.value(),
          srb_id_t::srb1,
          generate_rrc_reconfiguration_complete_pdu(transaction_id.value(), 9)));
  expect_and_ack_ntn_slot_update(env, du_idx.value(), du_ue_id, ue_ctx->cu_ue_id.value(), crnti);
  env.drain_f1ap_resource_coordination_requests(du_idx.value());

  ASSERT_TRUE(env.tick_until(std::chrono::milliseconds{100}, [&]() {
    return ntn_handler.get_current_ntn_runtime_status().nof_ntn_rrc_location_request_configured_ues == 0U;
  }));
  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_rrc_location_request_configs_removed,
            before_release.nof_ntn_rrc_location_request_configs_removed + 1U);
  EXPECT_EQ(runtime.nof_ntn_rrc_location_request_reconfig_sent,
            before_release.nof_ntn_rrc_location_request_reconfig_sent + 1U);
  EXPECT_EQ(runtime.nof_ntn_rrc_location_request_desired_ues, 0U);
  EXPECT_EQ(runtime.nof_ntn_rrc_location_request_configured_ues, 0U);
}

TEST(cu_cp_ntn_mobility_test, first_pdu_setup_binds_digital_service_by_access_cell_fallback_without_location)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_access_service_layer_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beam_cells(env, {0, 1});
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  const ue_index_t ue_index = finish_ntn_access_only_registration(env,
                                                                  du_idx.value(),
                                                                  cu_up_idx.value(),
                                                                  du_ue_id,
                                                                  to_rnti(0x4601),
                                                                  amf_ue_id_t::min,
                                                                  make_supported_ntn_ngso_ue_capability_info_pdu());
  ASSERT_NE(ue_index, ue_index_t::invalid);

  ASSERT_TRUE(env.request_pdu_session_resource_setup(du_idx.value(), cu_up_idx.value(), du_ue_id));
  ASSERT_TRUE(setup_pdu_session_with_five_qi(env,
                                             du_idx.value(),
                                             cu_up_idx.value(),
                                             du_ue_id,
                                             to_rnti(0x4601),
                                             int_to_gnb_cu_up_ue_e1ap_id(0),
                                             pdu_session_id_t::min,
                                             drb_id_t::drb1,
                                             uint_to_qos_flow_id(0),
                                             9,
                                             true));

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());
  expect_and_ack_ntn_slot_update(env, du_idx.value(), du_ue_id, ue_ctx->cu_ue_id.value(), to_rnti(0x4601));

  const cu_cp_ntn_beam_status service_beam =
      find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0001");
  EXPECT_EQ(service_beam.state, cu_cp_ntn_beam_assignment_state::active_loaded);
  EXPECT_EQ(service_beam.nof_drbs, 1U);

  const cu_cp_ntn_ue_status ue_status = find_ue_status(ntn_handler.get_current_ntn_ue_status(), ue_index);
  EXPECT_EQ(ue_status.ntn_runtime_state, "service_bound");
  EXPECT_EQ(ue_status.access_layer_state, "released_after_ics");
  EXPECT_TRUE(ue_status.analog_access_released);
  EXPECT_EQ(ue_status.service_layer_state, "service_bound");
  ASSERT_TRUE(ue_status.service_digital_beam_id.has_value());
  EXPECT_EQ(ue_status.service_digital_beam_id.value(), "CN-BEAM-0001");
  EXPECT_EQ(ue_status.service_binding_source, "access_cell_fallback");

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_location_bound_service_ues, 0U);
  EXPECT_EQ(runtime.nof_ntn_access_cell_fallback_service_ues, 1U);
}

TEST(cu_cp_ntn_mobility_test, failed_first_pdu_setup_keeps_ue_control_only_without_pending_digital_load)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_access_service_layer_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beam_cells(env, {0});
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  const ue_index_t ue_index =
      finish_ntn_access_only_registration(env, du_idx.value(), cu_up_idx.value(), du_ue_id, to_rnti(0x4601), amf_ue_id_t::min);
  ASSERT_NE(ue_index, ue_index_t::invalid);

  ntn_ue_location_report report = make_ntn_location_report(ue_index, make_default_env_nci(0));
  report.longitude_deg         = 1.0;
  get_cu_cp_impl(env)->get_cu_cp_measurement_handler().handle_ue_location_report(report);

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->amf_ue_id.has_value());
  ASSERT_TRUE(ue_ctx->ran_ue_id.has_value());

  env.get_amf().push_tx_pdu(generate_valid_pdu_session_resource_setup_request_message(
      ue_ctx->amf_ue_id.value(),
      ue_ctx->ran_ue_id.value(),
      {{pdu_session_id_t::min, {pdu_session_type_t::ipv4, {{uint_to_qos_flow_id(0), 9}}}}}));

  ngap_message ngap_pdu;
  ASSERT_TRUE(env.wait_for_ngap_tx_pdu(ngap_pdu));
  ASSERT_TRUE(test_helpers::is_valid_pdu_session_resource_setup_response(ngap_pdu));
  ASSERT_TRUE(test_helpers::is_expected_pdu_session_resource_setup_response(
      ngap_pdu, {}, {pdu_session_id_t::min}));

  const cu_cp_ntn_ue_status ue_status = find_ue_status(ntn_handler.get_current_ntn_ue_status(), ue_index);
  EXPECT_EQ(ue_status.ntn_runtime_state, "control_only");
  EXPECT_EQ(ue_status.access_layer_state, "released_after_ics");
  EXPECT_TRUE(ue_status.analog_access_released);
  EXPECT_EQ(ue_status.service_layer_state, "none");
  EXPECT_FALSE(ue_status.service_digital_beam_id.has_value());

  const cu_cp_ntn_beam_status rejected_service_beam =
      find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0002");
  EXPECT_NE(rejected_service_beam.state, cu_cp_ntn_beam_assignment_state::active_loaded);
  EXPECT_EQ(rejected_service_beam.nof_ues, 0U);
  EXPECT_EQ(rejected_service_beam.nof_drbs, 0U);
}

TEST(cu_cp_ntn_mobility_test, analog_access_resource_cap_is_released_after_initial_context_setup)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_access_service_layer_ntn_mobility_config();
  params.ntn_location_mobility->analog_beams.front().resource_policy.emplace();
  params.ntn_location_mobility->analog_beams.front().resource_policy->max_access_only_ues = 1;
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beam_cells(env, {0, 1});
  ASSERT_TRUE(du_idx.has_value());
  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const ue_index_t first_ue = finish_ntn_access_only_registration(env,
                                                                  du_idx.value(),
                                                                  cu_up_idx.value(),
                                                                  int_to_gnb_du_ue_f1ap_id(0),
                                                                  to_rnti(0x4601),
                                                                  amf_ue_id_t::min);
  ASSERT_NE(first_ue, ue_index_t::invalid);

  const gnb_du_ue_f1ap_id_t second_du_ue_id = int_to_gnb_du_ue_f1ap_id(1);
  ASSERT_TRUE(env.connect_new_ue(du_idx.value(), second_du_ue_id, to_rnti(0x4602)));

  const cu_cp_test_environment::ue_context* second_ue_ctx = env.find_ue_context(du_idx.value(), second_du_ue_id);
  ASSERT_NE(second_ue_ctx, nullptr);
  ASSERT_TRUE(second_ue_ctx->cu_ue_id.has_value());
  const ue_index_t second_ue =
      uint_to_ue_index(gnb_cu_ue_f1ap_id_to_uint(second_ue_ctx->cu_ue_id.value()));

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_access_active_ues, 1U);
  EXPECT_EQ(runtime.nof_ntn_control_only_ues, 1U);
  EXPECT_EQ(runtime.nof_ntn_analog_released_ues, 1U);
  EXPECT_EQ(find_ue_status(ntn_handler.get_current_ntn_ue_status(), first_ue).ntn_runtime_state, "control_only");
  EXPECT_EQ(find_ue_status(ntn_handler.get_current_ntn_ue_status(), second_ue).access_layer_state, "access_active");
}

TEST(cu_cp_ntn_mobility_test, unsupported_ntn_ue_capability_blocks_first_digital_service_binding)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_access_service_layer_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beam_cells(env, {0, 1});
  ASSERT_TRUE(du_idx.has_value());
  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  const ue_index_t ue_index =
      finish_ntn_access_only_registration(env, du_idx.value(), cu_up_idx.value(), du_ue_id, to_rnti(0x4601), amf_ue_id_t::min);
  ASSERT_NE(ue_index, ue_index_t::invalid);

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->amf_ue_id.has_value());
  ASSERT_TRUE(ue_ctx->ran_ue_id.has_value());

  env.get_amf().push_tx_pdu(generate_valid_pdu_session_resource_setup_request_message(
      ue_ctx->amf_ue_id.value(),
      ue_ctx->ran_ue_id.value(),
      {{pdu_session_id_t::min, {pdu_session_type_t::ipv4, {{uint_to_qos_flow_id(0), 9}}}}}));

  ngap_message ngap_pdu;
  ASSERT_TRUE(env.wait_for_ngap_tx_pdu(ngap_pdu));
  ASSERT_TRUE(test_helpers::is_valid_pdu_session_resource_setup_response(ngap_pdu));
  ASSERT_TRUE(test_helpers::is_expected_pdu_session_resource_setup_response(
      ngap_pdu, {}, {pdu_session_id_t::min}));

  const cu_cp_ntn_ue_status ue_status = find_ue_status(ntn_handler.get_current_ntn_ue_status(), ue_index);
  EXPECT_EQ(ue_status.ntn_runtime_state, "blocked");
  EXPECT_EQ(ue_status.service_layer_state, "blocked");
  EXPECT_EQ(ue_status.ntn_capability_state, ntn_ue_capability_state::unsupported);
  EXPECT_NE(ue_status.ntn_capability_reason, "supported");
  EXPECT_FALSE(ue_status.service_digital_beam_id.has_value());

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_capability_unsupported_ues, 1U);
  EXPECT_EQ(runtime.nof_ntn_capability_supported_ues, 0U);
}

TEST(cu_cp_ntn_mobility_test, digital_drb_resource_cap_rejects_new_pdu_demand_without_releasing_existing_service)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_access_service_layer_ntn_mobility_config();
  params.ntn_location_mobility->beams.front().resource_policy.emplace();
  params.ntn_location_mobility->beams.front().resource_policy->max_drbs = 1;
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beam_cells(env, {0, 1});
  ASSERT_TRUE(du_idx.has_value());
  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  const ue_index_t ue_index =
      finish_ntn_access_only_registration(env, du_idx.value(), cu_up_idx.value(), du_ue_id, to_rnti(0x4601), amf_ue_id_t::min);
  ASSERT_NE(ue_index, ue_index_t::invalid);

  ASSERT_TRUE(env.request_pdu_session_resource_setup(du_idx.value(), cu_up_idx.value(), du_ue_id));
  ASSERT_TRUE(setup_pdu_session_with_five_qi(env,
                                             du_idx.value(),
                                             cu_up_idx.value(),
                                             du_ue_id,
                                             to_rnti(0x4601),
                                             int_to_gnb_cu_up_ue_e1ap_id(0),
                                             pdu_session_id_t::min,
                                             drb_id_t::drb1,
                                             uint_to_qos_flow_id(0),
                                             9,
                                             true));
  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());
  expect_and_ack_ntn_slot_update(env, du_idx.value(), du_ue_id, ue_ctx->cu_ue_id.value(), to_rnti(0x4601));

  env.get_amf().push_tx_pdu(generate_valid_pdu_session_resource_setup_request_message(
      ue_ctx->amf_ue_id.value(),
      ue_ctx->ran_ue_id.value(),
      {{uint_to_pdu_session_id(2), {pdu_session_type_t::ipv4, {{uint_to_qos_flow_id(2), 9}}}}}));

  ngap_message ngap_pdu;
  ASSERT_TRUE(env.wait_for_ngap_tx_pdu(ngap_pdu));
  ASSERT_TRUE(test_helpers::is_valid_pdu_session_resource_setup_response(ngap_pdu));
  ASSERT_TRUE(test_helpers::is_expected_pdu_session_resource_setup_response(
      ngap_pdu, {}, {uint_to_pdu_session_id(2)}));

  const cu_cp_ntn_beam_status service_beam =
      find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0001");
  EXPECT_EQ(service_beam.state, cu_cp_ntn_beam_assignment_state::active_loaded);
  EXPECT_EQ(service_beam.nof_drbs, 1U);
  EXPECT_EQ(service_beam.digital_drb_cap, 1U);
  EXPECT_EQ(service_beam.digital_drb_load, 1U);
  const cu_cp_ntn_ue_status ue_status = find_ue_status(ntn_handler.get_current_ntn_ue_status(), ue_index);
  EXPECT_EQ(ue_status.ntn_runtime_state, "service_bound");
  EXPECT_EQ(ue_status.access_layer_state, "released_after_ics");
  EXPECT_TRUE(ue_status.analog_access_released);
  EXPECT_EQ(ue_status.service_layer_state, "service_bound");
}

TEST(cu_cp_ntn_mobility_test, ntn_ue_snapshot_exposes_location_candidate_and_slot_intent)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_access_service_layer_ntn_mobility_config();
  params.ntn_location_mobility->required_consecutive_location_reports = 2;
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  ASSERT_TRUE(env.attach_ue(du_idx.value(),
                            cu_up_idx.value(),
                            du_ue_id,
                            to_rnti(0x4601),
                            uint_to_amf_ue_id(0),
                            int_to_gnb_cu_up_ue_e1ap_id(0)));

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());
  expect_and_ack_ntn_slot_update(env, du_idx.value(), du_ue_id, ue_ctx->cu_ue_id.value(), to_rnti(0x4601));

  auto* cu_cp_impl = get_cu_cp_impl(env);
  ASSERT_NE(cu_cp_impl, nullptr);
  const ue_index_t ue_index = uint_to_ue_index(gnb_cu_ue_f1ap_id_to_uint(ue_ctx->cu_ue_id.value()));
  cu_cp_impl->get_cu_cp_measurement_handler().handle_ue_location_report(
      make_ntn_location_report(ue_index, make_default_env_nci(1)));

  const std::vector<cu_cp_ntn_ue_status> ue_status = ntn_handler.get_current_ntn_ue_status();
  ASSERT_EQ(ue_status.size(), 1U);
  EXPECT_EQ(ue_status.front().ue_index, ue_index);
  EXPECT_EQ(ue_status.front().du_index, uint_to_du_index(du_idx.value()));
  EXPECT_EQ(ue_status.front().rnti, to_rnti(0x4601));
  ASSERT_TRUE(ue_status.front().serving_nci.has_value());
  EXPECT_EQ(ue_status.front().serving_nci.value(), make_default_env_nci(1));
  ASSERT_TRUE(ue_status.front().serving_beam_id.has_value());
  EXPECT_EQ(ue_status.front().serving_beam_id.value(), "CN-BEAM-0002");
  ASSERT_TRUE(ue_status.front().last_location.has_value());
  EXPECT_TRUE(ue_status.front().has_ul_slot_request);
  EXPECT_EQ(ue_status.front().nof_drbs, 1U);
  ASSERT_TRUE(ue_status.front().candidate_beam_id.has_value());
  EXPECT_EQ(ue_status.front().candidate_beam_id.value(), "CN-BEAM-0001");
}

TEST(cu_cp_ntn_mobility_test, stale_ntn_assistance_rejects_new_access_and_drains_loaded_beam)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_stale_assistance_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  const ecef_coordinates_t   satellite   = make_ecef(0.0, 0.0, 500000.0);
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(satellite));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  ASSERT_TRUE(env.attach_ue(du_idx.value(),
                            cu_up_idx.value(),
                            du_ue_id,
                            to_rnti(0x4601),
                            uint_to_amf_ue_id(0),
                            int_to_gnb_cu_up_ue_e1ap_id(0)));

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());
  expect_and_ack_ntn_slot_update(env, du_idx.value(), du_ue_id, ue_ctx->cu_ue_id.value(), to_rnti(0x4601));

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(satellite));
  expect_no_f1ap_ue_setup_or_release(env, du_idx.value(), std::chrono::milliseconds{50});
  expire_ntn_assistance();
  const ntn_assistance_snapshot stale_snapshot = ntn_handler.get_current_ntn_assistance_snapshot();
  ASSERT_FALSE(stale_snapshot.valid);
  ASSERT_EQ(stale_snapshot.invalid_reason, ntn_assistance_invalid_reason::stale_satellite_state);

  const gnb_du_ue_f1ap_id_t rejected_du_ue_id = int_to_gnb_du_ue_f1ap_id(1);
  env.get_du(du_idx.value()).push_ul_pdu(
      test_helpers::generate_init_ul_rrc_message_transfer(rejected_du_ue_id, to_rnti(0x4602)));

  expect_and_ack_ntn_slot_update(env, du_idx.value(), du_ue_id, ue_ctx->cu_ue_id.value(), to_rnti(0x4601));

  f1ap_message f1ap_pdu;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu(du_idx.value(), f1ap_pdu, std::chrono::milliseconds{1000}));
  ASSERT_TRUE(test_helpers::is_valid_ue_context_release_command(f1ap_pdu));
  const auto& ue_rel = f1ap_pdu.pdu.init_msg().value.ue_context_release_cmd();
  ASSERT_EQ(int_to_gnb_du_ue_f1ap_id(ue_rel->gnb_du_ue_f1ap_id), rejected_du_ue_id);
  ASSERT_TRUE(ue_rel->srb_id_present);
  ASSERT_EQ(int_to_srb_id(ue_rel->srb_id), srb_id_t::srb0);

  asn1::rrc_nr::dl_ccch_msg_s ccch;
  asn1::cbit_ref              bref{ue_rel->rrc_container};
  ASSERT_EQ(ccch.unpack(bref), asn1::SRSASN_SUCCESS);
  ASSERT_EQ(ccch.msg.c1().type().value, asn1::rrc_nr::dl_ccch_msg_type_c::c1_c_::types_opts::rrc_reject);

  env.get_du(du_idx.value())
      .push_ul_pdu(test_helpers::generate_ue_context_release_complete(
          int_to_gnb_cu_ue_f1ap_id(ue_rel->gnb_cu_ue_f1ap_id), rejected_du_ue_id));

  ngap_message ngap_pdu;
  ASSERT_FALSE(env.get_amf().try_pop_rx_pdu(ngap_pdu));

  ASSERT_TRUE(wait_for_test_condition([&]() {
    const cu_cp_ntn_beam_status beam_status =
        find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0001");
    return beam_status.state == cu_cp_ntn_beam_assignment_state::draining && beam_status.nof_ues == 1U &&
           beam_status.nof_drbs == 1U;
  }));

  const cu_cp_ntn_beam_status beam_status =
      find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0001");
  ASSERT_EQ(beam_status.state, cu_cp_ntn_beam_assignment_state::draining);
  ASSERT_EQ(beam_status.nof_ues, 1U);
  ASSERT_EQ(beam_status.nof_drbs, 1U);
}

TEST(cu_cp_ntn_mobility_test, stale_ntn_assistance_rejects_new_pdu_session_demand_and_drains_loaded_beam)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_stale_assistance_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  const ecef_coordinates_t   satellite   = make_ecef(0.0, 0.0, 500000.0);
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(satellite));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  ASSERT_TRUE(env.attach_ue(du_idx.value(),
                            cu_up_idx.value(),
                            du_ue_id,
                            to_rnti(0x4601),
                            uint_to_amf_ue_id(0),
                            int_to_gnb_cu_up_ue_e1ap_id(0)));

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());
  ASSERT_TRUE(ue_ctx->ran_ue_id.has_value());
  ASSERT_TRUE(ue_ctx->amf_ue_id.has_value());
  expect_and_ack_ntn_slot_update(env, du_idx.value(), du_ue_id, ue_ctx->cu_ue_id.value(), to_rnti(0x4601));
  env.drain_f1ap_resource_coordination_requests(du_idx.value());

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(satellite));
  expect_no_f1ap_ue_setup_or_release(env, du_idx.value(), std::chrono::milliseconds{50});
  expire_ntn_assistance();
  const pdu_session_id_t second_psi = uint_to_pdu_session_id(2);
  const qos_flow_id_t    second_qfi = uint_to_qos_flow_id(2);
  env.get_amf().push_tx_pdu(generate_valid_pdu_session_resource_setup_request_message(
      ue_ctx->amf_ue_id.value(),
      ue_ctx->ran_ue_id.value(),
      {{second_psi, {pdu_session_type_t::ipv4, {{second_qfi, 9}}}}}));

  ngap_message ngap_pdu;
  ASSERT_TRUE(env.wait_for_ngap_tx_pdu(ngap_pdu));
  ASSERT_TRUE(test_helpers::is_valid_pdu_session_resource_setup_response(ngap_pdu));
  ASSERT_TRUE(test_helpers::is_expected_pdu_session_resource_setup_response(ngap_pdu, {}, {second_psi}));
  expect_no_f1ap_ue_setup_or_release(env, du_idx.value(), std::chrono::milliseconds{50});

  e1ap_message e1ap_pdu;
  ASSERT_FALSE(env.wait_for_e1ap_tx_pdu(cu_up_idx.value(), e1ap_pdu, std::chrono::milliseconds{20}));

  const cu_cp_ntn_beam_status beam_status =
      find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0001");
  ASSERT_EQ(beam_status.state, cu_cp_ntn_beam_assignment_state::draining);
  ASSERT_EQ(beam_status.nof_ues, 1U);
  ASSERT_EQ(beam_status.nof_drbs, 1U);

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_beam_hopping_ues_requested, 0U);
  EXPECT_EQ(runtime.nof_ntn_beam_hopping_ues_scheduled, 0U);
  EXPECT_EQ(runtime.nof_ntn_beam_hopping_ues_skipped, 0U);
}

TEST(cu_cp_ntn_mobility_test, hard_switch_over_drains_loaded_beam_and_blocks_new_pdu_demand)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  ASSERT_TRUE(env.attach_ue(du_idx.value(),
                            cu_up_idx.value(),
                            du_ue_id,
                            to_rnti(0x4601),
                            uint_to_amf_ue_id(0),
                            int_to_gnb_cu_up_ue_e1ap_id(0)));

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());
  ASSERT_TRUE(ue_ctx->ran_ue_id.has_value());
  ASSERT_TRUE(ue_ctx->amf_ue_id.has_value());
  expect_and_ack_ntn_slot_update(env, du_idx.value(), du_ue_id, ue_ctx->cu_ue_id.value(), to_rnti(0x4601));

  auto* cu_cp_impl = get_cu_cp_impl(env);
  ASSERT_NE(cu_cp_impl, nullptr);
  const ue_index_t ue_index = uint_to_ue_index(gnb_cu_ue_f1ap_id_to_uint(ue_ctx->cu_ue_id.value()));
  cu_cp_impl->get_cu_cp_measurement_handler().handle_ue_location_report(
      make_ntn_location_report(ue_index, make_default_env_nci(0)));

  ntn_service_switch_over_event event;
  event.event_id          = 101;
  event.type              = ntn_service_switch_over_type::hard;
  event.source            = ntn_service_switch_over_source::operator_command;
  event.policy            = ntn_service_switch_over_policy::drain;
  event.affected_beam_ids = {"CN-BEAM-0001"};
  ASSERT_TRUE(ntn_handler.handle_ntn_service_switch_over_event(event));
  f1ap_message drain_f1ap_pdu;
  ASSERT_FALSE(env.wait_for_f1ap_tx_pdu(du_idx.value(), drain_f1ap_pdu, std::chrono::milliseconds{20}));

  const cu_cp_ntn_beam_status switched_beam_status =
      find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0001");
  ASSERT_EQ(switched_beam_status.state, cu_cp_ntn_beam_assignment_state::draining);
  const cu_cp_ntn_runtime_status drain_runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(drain_runtime.nof_ntn_release_allowed_ues_requested, 0U);
  EXPECT_EQ(drain_runtime.nof_ntn_release_allowed_ues_scheduled, 0U);
  EXPECT_EQ(drain_runtime.nof_ntn_release_allowed_ues_skipped, 0U);
  EXPECT_EQ(drain_runtime.nof_ntn_handover_preferred_ues_requested, 0U);
  EXPECT_EQ(drain_runtime.nof_ntn_handover_preferred_ues_scheduled, 0U);
  EXPECT_EQ(drain_runtime.nof_ntn_handover_preferred_ues_skipped, 0U);

  const pdu_session_id_t second_psi = uint_to_pdu_session_id(2);
  const qos_flow_id_t    second_qfi = uint_to_qos_flow_id(2);
  env.get_amf().push_tx_pdu(generate_valid_pdu_session_resource_setup_request_message(
      ue_ctx->amf_ue_id.value(),
      ue_ctx->ran_ue_id.value(),
      {{second_psi, {pdu_session_type_t::ipv4, {{second_qfi, 9}}}}}));

  ngap_message ngap_pdu;
  ASSERT_TRUE(env.wait_for_ngap_tx_pdu(ngap_pdu));
  ASSERT_TRUE(test_helpers::is_valid_pdu_session_resource_setup_response(ngap_pdu));
  ASSERT_TRUE(test_helpers::is_expected_pdu_session_resource_setup_response(ngap_pdu, {}, {second_psi}));

  f1ap_message post_reject_f1ap_pdu;
  ASSERT_FALSE(env.wait_for_f1ap_tx_pdu(du_idx.value(), post_reject_f1ap_pdu, std::chrono::milliseconds{20}));
}

TEST(cu_cp_ntn_mobility_test, release_allowed_switch_over_releases_service_bound_ntn_ue)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_access_service_layer_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beam_cells(env, {0, 1});
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  const ue_index_t ue_index = finish_ntn_access_only_registration(env,
                                                                  du_idx.value(),
                                                                  cu_up_idx.value(),
                                                                  du_ue_id,
                                                                  to_rnti(0x4601),
                                                                  amf_ue_id_t::min,
                                                                  make_supported_ntn_ngso_ue_capability_info_pdu());
  ASSERT_NE(ue_index, ue_index_t::invalid);

  ASSERT_TRUE(env.request_pdu_session_resource_setup(du_idx.value(), cu_up_idx.value(), du_ue_id));
  ASSERT_TRUE(setup_pdu_session_with_five_qi(env,
                                             du_idx.value(),
                                             cu_up_idx.value(),
                                             du_ue_id,
                                             to_rnti(0x4601),
                                             int_to_gnb_cu_up_ue_e1ap_id(0),
                                             pdu_session_id_t::min,
                                             drb_id_t::drb1,
                                             uint_to_qos_flow_id(0),
                                             9,
                                             true));

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());
  expect_and_ack_ntn_slot_update(env, du_idx.value(), du_ue_id, ue_ctx->cu_ue_id.value(), to_rnti(0x4601));

  f1ap_message service_binding_f1ap_pdu;
  ASSERT_FALSE(
      env.wait_for_f1ap_tx_pdu(du_idx.value(), service_binding_f1ap_pdu, std::chrono::milliseconds{20}));
  ASSERT_TRUE(env.tick_until(std::chrono::milliseconds{100}, [&]() {
    const std::vector<cu_cp_ntn_ue_status> ue_status = ntn_handler.get_current_ntn_ue_status();
    const auto status_it = std::find_if(ue_status.begin(),
                                        ue_status.end(),
                                        [ue_index](const cu_cp_ntn_ue_status& status) {
                                          return status.ue_index == ue_index;
                                        });
    return status_it != ue_status.end() && status_it->service_layer_state == "service_bound";
  }));
  const cu_cp_ntn_ue_status before_release_status =
      find_ue_status(ntn_handler.get_current_ntn_ue_status(), ue_index);
  ASSERT_EQ(before_release_status.service_layer_state, "service_bound");
  ASSERT_TRUE(before_release_status.service_digital_beam_id.has_value());
  ASSERT_EQ(before_release_status.service_digital_beam_id.value(), "CN-BEAM-0001");

  ntn_service_switch_over_event event;
  event.event_id          = 104;
  event.type              = ntn_service_switch_over_type::hard;
  event.source            = ntn_service_switch_over_source::operator_command;
  event.policy            = ntn_service_switch_over_policy::release_allowed;
  event.affected_beam_ids = {"CN-BEAM-0001"};
  ASSERT_TRUE(ntn_handler.handle_ntn_service_switch_over_event(event));

  f1ap_message policy_coordination_pdu;
  ASSERT_FALSE(
      env.wait_for_f1ap_tx_pdu(du_idx.value(), policy_coordination_pdu, std::chrono::milliseconds{100}));

  e1ap_message e1ap_pdu;
  ASSERT_TRUE(env.wait_for_e1ap_tx_pdu(cu_up_idx.value(), e1ap_pdu, std::chrono::milliseconds{1000}));
  ASSERT_TRUE(test_helpers::is_valid_bearer_context_release_command(e1ap_pdu));
  ASSERT_TRUE(ue_ctx->cu_cp_e1ap_id.has_value());
  ASSERT_TRUE(ue_ctx->cu_up_e1ap_id.has_value());
  env.get_cu_up(cu_up_idx.value())
      .push_tx_pdu(generate_bearer_context_release_complete(ue_ctx->cu_cp_e1ap_id.value(),
                                                            ue_ctx->cu_up_e1ap_id.value()));

  f1ap_message f1ap_pdu;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu(du_idx.value(), f1ap_pdu, std::chrono::milliseconds{1000}));
  ASSERT_TRUE(test_helpers::is_valid_ue_context_release_command(f1ap_pdu));
  const auto& rel_cmd = f1ap_pdu.pdu.init_msg().value.ue_context_release_cmd();
  assert_f1ap_radio_network_cause(rel_cmd->cause, asn1::f1ap::cause_radio_network_opts::unspecified);

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_release_allowed_ues_requested, 1U);
  EXPECT_EQ(runtime.nof_ntn_release_allowed_ues_scheduled, 1U);
  EXPECT_EQ(runtime.nof_ntn_release_allowed_ues_skipped, 0U);

  env.get_du(du_idx.value())
      .push_ul_pdu(test_helpers::generate_ue_context_release_complete(
          int_to_gnb_cu_ue_f1ap_id(rel_cmd->gnb_cu_ue_f1ap_id),
          int_to_gnb_du_ue_f1ap_id(rel_cmd->gnb_du_ue_f1ap_id)));
  ASSERT_TRUE(env.tick_until(std::chrono::milliseconds{1000}, [&]() {
    return env.get_cu_cp().get_metrics_handler().request_metrics_report().ues.empty();
  }));

  f1ap_message post_release_coordination_pdu;
  ASSERT_FALSE(
      env.wait_for_f1ap_tx_pdu(du_idx.value(), post_release_coordination_pdu, std::chrono::milliseconds{100}));
}

TEST(cu_cp_ntn_mobility_test, handover_preferred_switch_over_prepares_connected_handover_for_service_bound_ue)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_access_service_layer_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beam_cells(env, {0, 1});
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  const ue_index_t ue_index = finish_ntn_access_only_registration(env,
                                                                  du_idx.value(),
                                                                  cu_up_idx.value(),
                                                                  du_ue_id,
                                                                  to_rnti(0x4601),
                                                                  amf_ue_id_t::min,
                                                                  make_supported_ntn_ngso_ue_capability_info_pdu());
  ASSERT_NE(ue_index, ue_index_t::invalid);

  ASSERT_TRUE(env.request_pdu_session_resource_setup(du_idx.value(), cu_up_idx.value(), du_ue_id));
  ASSERT_TRUE(setup_pdu_session_with_five_qi(env,
                                             du_idx.value(),
                                             cu_up_idx.value(),
                                             du_ue_id,
                                             to_rnti(0x4601),
                                             int_to_gnb_cu_up_ue_e1ap_id(0),
                                             pdu_session_id_t::min,
                                             drb_id_t::drb1,
                                             uint_to_qos_flow_id(0),
                                             9,
                                             true));

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());
  expect_and_ack_ntn_slot_update(env, du_idx.value(), du_ue_id, ue_ctx->cu_ue_id.value(), to_rnti(0x4601));

  const cu_cp_ntn_ue_status before_handover_status =
      find_ue_status(ntn_handler.get_current_ntn_ue_status(), ue_index);
  ASSERT_EQ(before_handover_status.service_layer_state, "service_bound");
  ASSERT_TRUE(before_handover_status.service_digital_beam_id.has_value());
  ASSERT_EQ(before_handover_status.service_digital_beam_id.value(), "CN-BEAM-0001");

  ntn_service_switch_over_event event;
  event.event_id          = 109;
  event.type              = ntn_service_switch_over_type::hard;
  event.source            = ntn_service_switch_over_source::operator_command;
  event.policy            = ntn_service_switch_over_policy::handover_preferred;
  event.affected_beam_ids = {"CN-BEAM-0001"};
  ASSERT_TRUE(ntn_handler.handle_ntn_service_switch_over_event(event));

  f1ap_message target_setup;
  ASSERT_TRUE(wait_for_ue_context_setup_request(env, du_idx.value(), target_setup));
  const auto& setup_req = target_setup.pdu.init_msg().value.ue_context_setup_request();
  ASSERT_EQ(setup_req->sp_cell_id.nr_cell_id.to_number(), make_default_env_nci(1).value());
  ASSERT_TRUE(setup_req->res_coordination_transfer_container_present);
  const std::optional<f1ap_ntn_ul_slot_resource_request> target_request =
      decode_f1ap_ntn_ul_slot_resource_request(setup_req->res_coordination_transfer_container);
  ASSERT_TRUE(target_request.has_value());
  ASSERT_TRUE(target_request->requested_c_rnti.has_value());
  env.drain_f1ap_resource_coordination_requests(du_idx.value());

  const cu_cp_ntn_ue_status handover_status = find_ue_status(ntn_handler.get_current_ntn_ue_status(), ue_index);
  ASSERT_EQ(handover_status.connected_handover_state, "target_resource_preparing");
  ASSERT_EQ(handover_status.connected_handover_reason, "service_switch_over");
  ASSERT_EQ(handover_status.connected_handover_source_beam_id.value(), "CN-BEAM-0001");
  ASSERT_EQ(handover_status.connected_handover_target_beam_id.value(), "CN-BEAM-0002");
  ASSERT_EQ(handover_status.connected_handover_target_du_index, uint_to_du_index(du_idx.value()));
  ASSERT_EQ(handover_status.connected_handover_target_c_rnti, target_request->requested_c_rnti.value());
  ASSERT_EQ(handover_status.connected_handover_target_resource_state, "target_resource_preparing");

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_handover_preferred_ues_requested, 1U);
  EXPECT_EQ(runtime.nof_ntn_handover_preferred_ues_scheduled, 1U);
  EXPECT_EQ(runtime.nof_ntn_handover_preferred_ues_skipped, 0U);
  EXPECT_EQ(runtime.nof_ntn_release_allowed_ues_requested, 0U);

  e1ap_message e1ap_pdu;
  ASSERT_FALSE(env.wait_for_e1ap_tx_pdu(cu_up_idx.value(), e1ap_pdu, std::chrono::milliseconds{20}));

  env.get_du(du_idx.value())
      .push_ul_pdu(test_helpers::generate_ue_context_setup_failure(
          int_to_gnb_cu_ue_f1ap_id(setup_req->gnb_cu_ue_f1ap_id), du_ue_id));
  expect_no_f1ap_ue_setup_or_release(env, du_idx.value(), std::chrono::milliseconds{50});
}

TEST(cu_cp_ntn_mobility_test, handover_preferred_uses_service_pair_target_when_no_bidirectional_target)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_service_pair_handover_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beam_cells(env, {0, 1, 2});
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  const ue_index_t ue_index = finish_ntn_access_only_registration(env,
                                                                  du_idx.value(),
                                                                  cu_up_idx.value(),
                                                                  du_ue_id,
                                                                  to_rnti(0x4601),
                                                                  amf_ue_id_t::min,
                                                                  make_supported_ntn_ngso_ue_capability_info_pdu());
  ASSERT_NE(ue_index, ue_index_t::invalid);

  ASSERT_TRUE(env.request_pdu_session_resource_setup(du_idx.value(), cu_up_idx.value(), du_ue_id));
  ASSERT_TRUE(setup_pdu_session_with_five_qi(env,
                                             du_idx.value(),
                                             cu_up_idx.value(),
                                             du_ue_id,
                                             to_rnti(0x4601),
                                             int_to_gnb_cu_up_ue_e1ap_id(0),
                                             pdu_session_id_t::min,
                                             drb_id_t::drb1,
                                             uint_to_qos_flow_id(0),
                                             9,
                                             true));

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());
  expect_and_ack_ntn_slot_update(env, du_idx.value(), du_ue_id, ue_ctx->cu_ue_id.value(), to_rnti(0x4601));

  ntn_service_switch_over_event event;
  event.event_id          = 172;
  event.type              = ntn_service_switch_over_type::hard;
  event.source            = ntn_service_switch_over_source::operator_command;
  event.policy            = ntn_service_switch_over_policy::handover_preferred;
  event.affected_beam_ids = {"CN-BEAM-0001"};
  ASSERT_TRUE(ntn_handler.handle_ntn_service_switch_over_event(event));

  f1ap_message target_setup;
  ASSERT_TRUE(wait_for_ue_context_setup_request(env, du_idx.value(), target_setup));
  const auto& setup_req = target_setup.pdu.init_msg().value.ue_context_setup_request();
  ASSERT_EQ(setup_req->sp_cell_id.nr_cell_id.to_number(), make_default_env_nci(1).value());
  ASSERT_TRUE(setup_req->res_coordination_transfer_container_present);
  const std::optional<f1ap_ntn_ul_slot_resource_request> target_request =
      decode_f1ap_ntn_ul_slot_resource_request(setup_req->res_coordination_transfer_container);
  ASSERT_TRUE(target_request.has_value());
  const cu_cp_ntn_beam_status uplink_resource_beam =
      find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-UL-0002");
  ASSERT_TRUE(target_request->sr_slot_offset.has_value());
  ASSERT_TRUE(target_request->srs_slot_offset.has_value());
  EXPECT_EQ(*target_request->sr_slot_offset, uplink_resource_beam.sr_slot_offset);
  EXPECT_EQ(*target_request->srs_slot_offset, uplink_resource_beam.srs_slot_offset);

  const cu_cp_ntn_ue_status handover_status = find_ue_status(ntn_handler.get_current_ntn_ue_status(), ue_index);
  ASSERT_EQ(handover_status.connected_handover_state, "target_resource_preparing");
  ASSERT_EQ(handover_status.connected_handover_reason, "service_switch_over");
  ASSERT_EQ(handover_status.connected_handover_target_beam_id.value(), "CN-BEAM-0002");
  ASSERT_TRUE(handover_status.connected_handover_target_uplink_resource_beam_id.has_value());
  EXPECT_EQ(handover_status.connected_handover_target_uplink_resource_beam_id.value(), "CN-BEAM-UL-0002");
  ASSERT_TRUE(handover_status.connected_handover_target_uplink_resource_nci.has_value());
  EXPECT_EQ(handover_status.connected_handover_target_uplink_resource_nci.value(), make_default_env_nci(2));
  EXPECT_EQ(handover_status.connected_handover_target_uplink_resource_du_index, uint_to_du_index(du_idx.value()));

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_handover_preferred_ues_requested, 1U);
  EXPECT_EQ(runtime.nof_ntn_handover_preferred_ues_scheduled, 1U);
  EXPECT_EQ(runtime.nof_ntn_service_pair_handover_targets, 1U);
  EXPECT_EQ(runtime.nof_ntn_service_pair_handover_scheduled, 1U);
  EXPECT_EQ(runtime.last_ntn_service_pair_handover_reason, "same_analog_tac_uplink_resource");

  env.drain_f1ap_resource_coordination_requests(du_idx.value());
  env.get_du(du_idx.value())
      .push_ul_pdu(test_helpers::generate_ue_context_setup_failure(
          int_to_gnb_cu_ue_f1ap_id(setup_req->gnb_cu_ue_f1ap_id), du_ue_id));
  expect_no_f1ap_ue_setup_or_release(env, du_idx.value(), std::chrono::milliseconds{50});
}

TEST(cu_cp_ntn_mobility_test, handover_preferred_skips_when_no_eligible_target)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_access_service_layer_ntn_mobility_config();
  params.ntn_location_mobility->max_nof_served_beams                = 1;
  params.ntn_location_mobility->max_nof_loaded_digital_service_beams = 1;
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beam_cells(env, {0, 1});
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  const ue_index_t ue_index = finish_ntn_access_only_registration(env,
                                                                  du_idx.value(),
                                                                  cu_up_idx.value(),
                                                                  du_ue_id,
                                                                  to_rnti(0x4601),
                                                                  amf_ue_id_t::min,
                                                                  make_supported_ntn_ngso_ue_capability_info_pdu());
  ASSERT_NE(ue_index, ue_index_t::invalid);

  ASSERT_TRUE(env.request_pdu_session_resource_setup(du_idx.value(), cu_up_idx.value(), du_ue_id));
  ASSERT_TRUE(setup_pdu_session_with_five_qi(env,
                                             du_idx.value(),
                                             cu_up_idx.value(),
                                             du_ue_id,
                                             to_rnti(0x4601),
                                             int_to_gnb_cu_up_ue_e1ap_id(0),
                                             pdu_session_id_t::min,
                                             drb_id_t::drb1,
                                             uint_to_qos_flow_id(0),
                                             9,
                                             true));

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());
  expect_and_ack_ntn_slot_update(env, du_idx.value(), du_ue_id, ue_ctx->cu_ue_id.value(), to_rnti(0x4601));

  ntn_service_switch_over_event event;
  event.event_id          = 110;
  event.type              = ntn_service_switch_over_type::hard;
  event.source            = ntn_service_switch_over_source::operator_command;
  event.policy            = ntn_service_switch_over_policy::handover_preferred;
  event.affected_beam_ids = {"CN-BEAM-0001"};
  ASSERT_TRUE(ntn_handler.handle_ntn_service_switch_over_event(event));

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_handover_preferred_ues_requested, 1U);
  EXPECT_EQ(runtime.nof_ntn_handover_preferred_ues_scheduled, 0U);
  EXPECT_EQ(runtime.nof_ntn_handover_preferred_ues_skipped, 1U);
  EXPECT_EQ(runtime.nof_ntn_release_allowed_ues_requested, 0U);

  const cu_cp_ntn_ue_status ue_status = find_ue_status(ntn_handler.get_current_ntn_ue_status(), ue_index);
  EXPECT_EQ(ue_status.service_layer_state, "service_bound");
  EXPECT_EQ(ue_status.connected_handover_state, "none");

  expect_no_f1ap_ue_setup_or_release(env, du_idx.value(), std::chrono::milliseconds{50});
  e1ap_message e1ap_pdu;
  ASSERT_FALSE(env.wait_for_e1ap_tx_pdu(cu_up_idx.value(), e1ap_pdu, std::chrono::milliseconds{20}));
}

TEST(cu_cp_ntn_mobility_test, handover_preferred_does_not_touch_access_only_or_control_only_ue)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_access_service_layer_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beam_cells(env, {0, 1});
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  const ue_index_t ue_index =
      finish_ntn_access_only_registration(env, du_idx.value(), cu_up_idx.value(), du_ue_id, to_rnti(0x4601), amf_ue_id_t::min);
  ASSERT_NE(ue_index, ue_index_t::invalid);
  const cu_cp_ntn_ue_status control_only_status = find_ue_status(ntn_handler.get_current_ntn_ue_status(), ue_index);
  ASSERT_EQ(control_only_status.service_layer_state, "none");
  ASSERT_EQ(control_only_status.ntn_runtime_state, "control_only");

  ntn_service_switch_over_event event;
  event.event_id          = 111;
  event.type              = ntn_service_switch_over_type::hard;
  event.source            = ntn_service_switch_over_source::operator_command;
  event.policy            = ntn_service_switch_over_policy::handover_preferred;
  event.affected_beam_ids = {"CN-BEAM-0001"};
  ASSERT_TRUE(ntn_handler.handle_ntn_service_switch_over_event(event));

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_handover_preferred_ues_requested, 0U);
  EXPECT_EQ(runtime.nof_ntn_handover_preferred_ues_scheduled, 0U);
  EXPECT_EQ(runtime.nof_ntn_handover_preferred_ues_skipped, 0U);

  expect_no_f1ap_ue_setup_or_release(env, du_idx.value(), std::chrono::milliseconds{50});
}

TEST(cu_cp_ntn_mobility_test, beam_hopping_draining_beam_prepares_connected_handover_for_service_bound_ue)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_access_service_layer_ntn_mobility_config();
  params.ntn_location_mobility->served_beam_min_elevation_deg = 80.0;
  params.ntn_location_mobility->max_nof_served_beams          = 1;
  enable_predictive_satellite_state_update(params.ntn_location_mobility.value());
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));
  ASSERT_EQ(ntn_handler.get_current_ntn_served_beam_ids(), std::vector<std::string>({"CN-BEAM-0001"}));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  const ue_index_t ue_index = finish_ntn_access_only_registration(env,
                                                                  du_idx.value(),
                                                                  cu_up_idx.value(),
                                                                  du_ue_id,
                                                                  to_rnti(0x4601),
                                                                  amf_ue_id_t::min,
                                                                  make_supported_ntn_ngso_ue_capability_info_pdu());
  ASSERT_NE(ue_index, ue_index_t::invalid);

  ASSERT_TRUE(env.request_pdu_session_resource_setup(du_idx.value(), cu_up_idx.value(), du_ue_id));
  ASSERT_TRUE(setup_pdu_session_with_five_qi(env,
                                             du_idx.value(),
                                             cu_up_idx.value(),
                                             du_ue_id,
                                             to_rnti(0x4601),
                                             int_to_gnb_cu_up_ue_e1ap_id(0),
                                             pdu_session_id_t::min,
                                             drb_id_t::drb1,
                                             uint_to_qos_flow_id(0),
                                             9,
                                             true));

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());
  expect_and_ack_ntn_slot_update(env, du_idx.value(), du_ue_id, ue_ctx->cu_ue_id.value(), to_rnti(0x4601));
  expect_service_binding_ready(env, du_idx.value(), ntn_handler, ue_index);

  const cu_cp_ntn_ue_status before_handover_status =
      find_ue_status(ntn_handler.get_current_ntn_ue_status(), ue_index);
  ASSERT_EQ(before_handover_status.service_layer_state, "service_bound");
  ASSERT_TRUE(before_handover_status.service_digital_beam_id.has_value());
  ASSERT_EQ(before_handover_status.service_digital_beam_id.value(), "CN-BEAM-0001");

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 1.0, 500000.0)));
  ASSERT_EQ(ntn_handler.get_current_ntn_served_beam_ids(), std::vector<std::string>({"CN-BEAM-0002"}));

  f1ap_message target_setup;
  ASSERT_TRUE(wait_for_ue_context_setup_request(env, du_idx.value(), target_setup));
  const auto& setup_req = target_setup.pdu.init_msg().value.ue_context_setup_request();
  ASSERT_EQ(setup_req->sp_cell_id.nr_cell_id.to_number(), make_default_env_nci(1).value());
  ASSERT_TRUE(setup_req->res_coordination_transfer_container_present);
  const std::optional<f1ap_ntn_ul_slot_resource_request> target_request =
      decode_f1ap_ntn_ul_slot_resource_request(setup_req->res_coordination_transfer_container);
  ASSERT_TRUE(target_request.has_value());
  ASSERT_TRUE(target_request->requested_c_rnti.has_value());

  const cu_cp_ntn_beam_status source_beam =
      find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0001");
  ASSERT_EQ(source_beam.state, cu_cp_ntn_beam_assignment_state::draining);

  const cu_cp_ntn_ue_status handover_status = find_ue_status(ntn_handler.get_current_ntn_ue_status(), ue_index);
  ASSERT_EQ(handover_status.connected_handover_state, "target_resource_preparing");
  ASSERT_EQ(handover_status.connected_handover_reason, "beam_hopping");
  ASSERT_EQ(handover_status.connected_handover_source_beam_id.value(), "CN-BEAM-0001");
  ASSERT_EQ(handover_status.connected_handover_target_beam_id.value(), "CN-BEAM-0002");
  ASSERT_EQ(handover_status.connected_handover_target_du_index, uint_to_du_index(du_idx.value()));
  ASSERT_EQ(handover_status.connected_handover_target_c_rnti, target_request->requested_c_rnti.value());
  ASSERT_EQ(handover_status.connected_handover_target_resource_state, "target_resource_preparing");

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_beam_hopping_ues_requested, 1U);
  EXPECT_EQ(runtime.nof_ntn_beam_hopping_ues_scheduled, 1U);
  EXPECT_EQ(runtime.nof_ntn_beam_hopping_ues_skipped, 0U);

  e1ap_message e1ap_pdu;
  ASSERT_FALSE(env.wait_for_e1ap_tx_pdu(cu_up_idx.value(), e1ap_pdu, std::chrono::milliseconds{20}));

  env.drain_f1ap_resource_coordination_requests(du_idx.value());
  env.get_du(du_idx.value())
      .push_ul_pdu(test_helpers::generate_ue_context_setup_failure(
          int_to_gnb_cu_ue_f1ap_id(setup_req->gnb_cu_ue_f1ap_id), du_ue_id));
}

TEST(cu_cp_ntn_mobility_test, beam_hopping_service_pair_target_prepares_connected_handover)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_service_pair_handover_ntn_mobility_config();
  params.ntn_location_mobility->served_beam_min_elevation_deg = 80.0;
  params.ntn_location_mobility->max_nof_served_beams          = 2;
  enable_predictive_satellite_state_update(params.ntn_location_mobility.value());
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beam_cells(env, {0, 1, 2});
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  const ue_index_t ue_index = finish_ntn_access_only_registration(env,
                                                                  du_idx.value(),
                                                                  cu_up_idx.value(),
                                                                  du_ue_id,
                                                                  to_rnti(0x4601),
                                                                  amf_ue_id_t::min,
                                                                  make_supported_ntn_ngso_ue_capability_info_pdu());
  ASSERT_NE(ue_index, ue_index_t::invalid);

  ASSERT_TRUE(env.request_pdu_session_resource_setup(du_idx.value(), cu_up_idx.value(), du_ue_id));
  ASSERT_TRUE(setup_pdu_session_with_five_qi(env,
                                             du_idx.value(),
                                             cu_up_idx.value(),
                                             du_ue_id,
                                             to_rnti(0x4601),
                                             int_to_gnb_cu_up_ue_e1ap_id(0),
                                             pdu_session_id_t::min,
                                             drb_id_t::drb1,
                                             uint_to_qos_flow_id(0),
                                             9,
                                             true));

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());
  expect_and_ack_ntn_slot_update(env, du_idx.value(), du_ue_id, ue_ctx->cu_ue_id.value(), to_rnti(0x4601));
  expect_service_binding_ready(env, du_idx.value(), ntn_handler, ue_index);

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 1.0, 500000.0)));

  f1ap_message target_setup;
  ASSERT_TRUE(wait_for_ue_context_setup_request(env, du_idx.value(), target_setup));
  const auto& setup_req = target_setup.pdu.init_msg().value.ue_context_setup_request();
  ASSERT_EQ(setup_req->sp_cell_id.nr_cell_id.to_number(), make_default_env_nci(1).value());
  ASSERT_TRUE(setup_req->res_coordination_transfer_container_present);
  const std::optional<f1ap_ntn_ul_slot_resource_request> target_request =
      decode_f1ap_ntn_ul_slot_resource_request(setup_req->res_coordination_transfer_container);
  ASSERT_TRUE(target_request.has_value());
  const cu_cp_ntn_beam_status uplink_resource_beam =
      find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-UL-0002");
  ASSERT_TRUE(target_request->sr_slot_offset.has_value());
  ASSERT_TRUE(target_request->srs_slot_offset.has_value());
  EXPECT_EQ(*target_request->sr_slot_offset, uplink_resource_beam.sr_slot_offset);
  EXPECT_EQ(*target_request->srs_slot_offset, uplink_resource_beam.srs_slot_offset);

  const cu_cp_ntn_ue_status handover_status = find_ue_status(ntn_handler.get_current_ntn_ue_status(), ue_index);
  ASSERT_EQ(handover_status.connected_handover_state, "target_resource_preparing");
  ASSERT_EQ(handover_status.connected_handover_reason, "beam_hopping");
  ASSERT_EQ(handover_status.connected_handover_source_beam_id.value(), "CN-BEAM-0001");
  ASSERT_EQ(handover_status.connected_handover_target_beam_id.value(), "CN-BEAM-0002");
  ASSERT_TRUE(handover_status.connected_handover_target_uplink_resource_beam_id.has_value());
  EXPECT_EQ(handover_status.connected_handover_target_uplink_resource_beam_id.value(), "CN-BEAM-UL-0002");

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_beam_hopping_ues_requested, 1U);
  EXPECT_EQ(runtime.nof_ntn_beam_hopping_ues_scheduled, 1U);
  EXPECT_EQ(runtime.nof_ntn_service_pair_handover_targets, 1U);
  EXPECT_EQ(runtime.nof_ntn_service_pair_handover_scheduled, 1U);
  EXPECT_EQ(runtime.last_ntn_service_pair_handover_reason, "same_analog_tac_uplink_resource");

  e1ap_message e1ap_pdu;
  ASSERT_FALSE(env.wait_for_e1ap_tx_pdu(cu_up_idx.value(), e1ap_pdu, std::chrono::milliseconds{20}));
  env.drain_f1ap_resource_coordination_requests(du_idx.value());
  env.get_du(du_idx.value())
      .push_ul_pdu(test_helpers::generate_ue_context_setup_failure(
          int_to_gnb_cu_ue_f1ap_id(setup_req->gnb_cu_ue_f1ap_id), du_ue_id));
  expect_no_f1ap_ue_setup_or_release(env, du_idx.value(), std::chrono::milliseconds{50});
}

TEST(cu_cp_ntn_mobility_test, beam_hopping_skips_when_no_eligible_target)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_access_service_layer_ntn_mobility_config();
  params.ntn_location_mobility->served_beam_min_elevation_deg = 80.0;
  params.ntn_location_mobility->max_nof_served_beams          = 1;
  enable_predictive_satellite_state_update(params.ntn_location_mobility.value());
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beam_cells(env, {0});
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  const ue_index_t ue_index = finish_ntn_access_only_registration(env,
                                                                  du_idx.value(),
                                                                  cu_up_idx.value(),
                                                                  du_ue_id,
                                                                  to_rnti(0x4601),
                                                                  amf_ue_id_t::min,
                                                                  make_supported_ntn_ngso_ue_capability_info_pdu());
  ASSERT_NE(ue_index, ue_index_t::invalid);

  ASSERT_TRUE(env.request_pdu_session_resource_setup(du_idx.value(), cu_up_idx.value(), du_ue_id));
  ASSERT_TRUE(setup_pdu_session_with_five_qi(env,
                                             du_idx.value(),
                                             cu_up_idx.value(),
                                             du_ue_id,
                                             to_rnti(0x4601),
                                             int_to_gnb_cu_up_ue_e1ap_id(0),
                                             pdu_session_id_t::min,
                                             drb_id_t::drb1,
                                             uint_to_qos_flow_id(0),
                                             9,
                                             true));

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());
  expect_and_ack_ntn_slot_update(env, du_idx.value(), du_ue_id, ue_ctx->cu_ue_id.value(), to_rnti(0x4601));
  expect_service_binding_ready(env, du_idx.value(), ntn_handler, ue_index);

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 1.0, 500000.0)));

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_beam_hopping_ues_requested, 1U);
  EXPECT_EQ(runtime.nof_ntn_beam_hopping_ues_scheduled, 0U);
  EXPECT_EQ(runtime.nof_ntn_beam_hopping_ues_skipped, 1U);

  const cu_cp_ntn_beam_status source_beam =
      find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0001");
  EXPECT_EQ(source_beam.state, cu_cp_ntn_beam_assignment_state::draining);

  const cu_cp_ntn_ue_status ue_status = find_ue_status(ntn_handler.get_current_ntn_ue_status(), ue_index);
  EXPECT_EQ(ue_status.service_layer_state, "service_bound");
  EXPECT_EQ(ue_status.connected_handover_state, "none");

  expect_no_f1ap_ue_setup_or_release(env, du_idx.value(), std::chrono::milliseconds{50});
  e1ap_message e1ap_pdu;
  ASSERT_FALSE(env.wait_for_e1ap_tx_pdu(cu_up_idx.value(), e1ap_pdu, std::chrono::milliseconds{20}));
}

TEST(cu_cp_ntn_mobility_test, beam_hopping_does_not_touch_control_only_ue)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_access_service_layer_ntn_mobility_config();
  params.ntn_location_mobility->served_beam_min_elevation_deg = 80.0;
  params.ntn_location_mobility->max_nof_served_beams          = 1;
  enable_predictive_satellite_state_update(params.ntn_location_mobility.value());
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  const ue_index_t ue_index = finish_ntn_access_only_registration(env,
                                                                  du_idx.value(),
                                                                  cu_up_idx.value(),
                                                                  du_ue_id,
                                                                  to_rnti(0x4601),
                                                                  amf_ue_id_t::min,
                                                                  make_supported_ntn_ngso_ue_capability_info_pdu());
  ASSERT_NE(ue_index, ue_index_t::invalid);
  const cu_cp_ntn_ue_status control_only_status = find_ue_status(ntn_handler.get_current_ntn_ue_status(), ue_index);
  ASSERT_EQ(control_only_status.service_layer_state, "none");
  ASSERT_FALSE(control_only_status.service_digital_beam_id.has_value());

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 1.0, 500000.0)));

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_beam_hopping_ues_requested, 0U);
  EXPECT_EQ(runtime.nof_ntn_beam_hopping_ues_scheduled, 0U);
  EXPECT_EQ(runtime.nof_ntn_beam_hopping_ues_skipped, 0U);

  expect_no_f1ap_ue_setup_or_release(env, du_idx.value(), std::chrono::milliseconds{50});
}

TEST(cu_cp_ntn_mobility_test, predictive_window_marks_upcoming_and_drain_soon_beams)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_access_service_layer_ntn_mobility_config();
  params.ntn_location_mobility->served_beam_min_elevation_deg = 80.0;
  params.ntn_location_mobility->max_nof_served_beams          = 1;
  enable_predictive_satellite_state_update(params.ntn_location_mobility.value());
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();
  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  ASSERT_TRUE(du_idx.has_value());

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0),
                                                            make_ecef(0.0, 1.0, 500000.0)));
  env.drain_f1ap_resource_coordination_requests(du_idx.value());

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_TRUE(runtime.predictive_window_valid);
  EXPECT_EQ(runtime.nof_ntn_predictive_upcoming_beams, 1U);
  EXPECT_EQ(runtime.nof_ntn_predictive_drain_soon_beams, 1U);

  const std::vector<cu_cp_ntn_beam_status> beam_status = ntn_handler.get_current_ntn_beam_status();
  const cu_cp_ntn_beam_status upcoming_beam = find_beam_status(beam_status, "CN-BEAM-0002");
  EXPECT_EQ(upcoming_beam.state, cu_cp_ntn_beam_assignment_state::candidate);
  EXPECT_TRUE(upcoming_beam.in_hopping_window);

  const ntn_assistance_snapshot assistance = ntn_handler.get_current_ntn_assistance_snapshot();
  ASSERT_TRUE(assistance.valid);
  const auto upcoming_assistance_it =
      std::find_if(assistance.beams.begin(), assistance.beams.end(), [](const ntn_assistance_beam_snapshot& beam) {
        return beam.beam_id == "CN-BEAM-0002";
      });
  ASSERT_NE(upcoming_assistance_it, assistance.beams.end());
  EXPECT_EQ(upcoming_assistance_it->state, ntn_assistance_beam_state::candidate);
}

TEST(cu_cp_ntn_mobility_test, multi_satellite_predictive_window_marks_cross_satellite_upcoming_and_drain_soon)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_access_service_layer_ntn_mobility_config();
  params.ntn_location_mobility->served_beam_min_elevation_deg = 80.0;
  params.ntn_location_mobility->max_nof_served_beams          = 1;
  enable_predictive_satellite_state_update(params.ntn_location_mobility.value());
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();
  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  ASSERT_TRUE(du_idx.has_value());

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(
      std::vector<ntn_satellite_state>{{"sat-A", make_ecef(0.0, 0.0, 500000.0)}},
      std::vector<ntn_satellite_state>{{"sat-B", make_ecef(0.0, 1.0, 500000.0)}}));
  env.drain_f1ap_resource_coordination_requests(du_idx.value());

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_TRUE(runtime.predictive_window_valid);
  EXPECT_TRUE(runtime.multi_satellite_window_valid);
  EXPECT_EQ(runtime.nof_ntn_current_window_satellites, 1U);
  EXPECT_EQ(runtime.nof_ntn_next_window_satellites, 1U);
  EXPECT_EQ(runtime.nof_ntn_predictive_upcoming_beams, 1U);
  EXPECT_EQ(runtime.nof_ntn_predictive_drain_soon_beams, 1U);

  const std::vector<cu_cp_ntn_beam_status> beam_status = ntn_handler.get_current_ntn_beam_status();
  const cu_cp_ntn_beam_status source_beam = find_beam_status(beam_status, "CN-BEAM-0001");
  const cu_cp_ntn_beam_status target_beam = find_beam_status(beam_status, "CN-BEAM-0002");
  EXPECT_EQ(source_beam.serving_satellite_id, "sat-A");
  EXPECT_EQ(source_beam.state_reason, "predictive_drain_soon");
  EXPECT_EQ(target_beam.serving_satellite_id, "sat-B");
  EXPECT_TRUE(target_beam.in_hopping_window);
  EXPECT_EQ(target_beam.state_reason, "predictive_upcoming");
}

TEST(cu_cp_ntn_mobility_test, predictive_timeline_marks_future_entry_and_exit_offsets)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_access_service_layer_ntn_mobility_config();
  params.ntn_location_mobility->served_beam_min_elevation_deg = 80.0;
  params.ntn_location_mobility->max_nof_served_beams          = 1;
  enable_predictive_timeline_satellite_state_update(params.ntn_location_mobility.value());
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();
  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  ASSERT_TRUE(du_idx.has_value());

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(
      std::vector<ntn_satellite_state>{{"sat-0", make_ecef(0.0, 0.0, 500000.0)}},
      std::vector<ntn_satellite_prediction_step>{
          {std::chrono::seconds{1}, {{"sat-0", make_ecef(0.0, 0.0, 500000.0)}}},
          {std::chrono::seconds{2}, {{"sat-0", make_ecef(0.0, 1.0, 500000.0)}}},
          {std::chrono::seconds{3}, {{"sat-0", make_ecef(0.0, 1.0, 500000.0)}}}}));
  env.drain_f1ap_resource_coordination_requests(du_idx.value());

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_TRUE(runtime.predictive_window_valid);
  EXPECT_EQ(runtime.ntn_predictive_service_window_horizon, std::chrono::seconds{3});
  EXPECT_EQ(runtime.ntn_predictive_handover_lead_time, std::chrono::seconds{1});
  EXPECT_EQ(runtime.nof_ntn_predictive_timeline_steps, 3U);
  EXPECT_EQ(runtime.nof_ntn_predictive_timeline_entry_beams, 1U);
  EXPECT_EQ(runtime.nof_ntn_predictive_timeline_exit_beams, 1U);
  EXPECT_EQ(runtime.nof_ntn_predictive_drain_soon_beams, 0U);
  ASSERT_TRUE(runtime.earliest_ntn_predictive_upcoming_offset.has_value());
  EXPECT_EQ(runtime.earliest_ntn_predictive_upcoming_offset.value(), std::chrono::seconds{2});
  ASSERT_TRUE(runtime.earliest_ntn_predictive_drain_offset.has_value());
  EXPECT_EQ(runtime.earliest_ntn_predictive_drain_offset.value(), std::chrono::seconds{2});

  const std::vector<cu_cp_ntn_beam_status> beam_status = ntn_handler.get_current_ntn_beam_status();
  const cu_cp_ntn_beam_status source_beam = find_beam_status(beam_status, "CN-BEAM-0001");
  const cu_cp_ntn_beam_status target_beam = find_beam_status(beam_status, "CN-BEAM-0002");
  ASSERT_TRUE(source_beam.predictive_exit_offset.has_value());
  EXPECT_EQ(source_beam.predictive_exit_offset.value(), std::chrono::seconds{2});
  EXPECT_FALSE(source_beam.new_demand_blocked);
  ASSERT_TRUE(target_beam.predictive_entry_offset.has_value());
  EXPECT_EQ(target_beam.predictive_entry_offset.value(), std::chrono::seconds{2});
  EXPECT_EQ(target_beam.state_reason, "predictive_upcoming");
}

TEST(cu_cp_ntn_mobility_test, beam_exiting_after_handover_lead_is_not_drained_yet)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_access_service_layer_ntn_mobility_config();
  params.ntn_location_mobility->served_beam_min_elevation_deg = 80.0;
  params.ntn_location_mobility->max_nof_served_beams          = 1;
  enable_predictive_timeline_satellite_state_update(params.ntn_location_mobility.value());
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  const std::optional<service_bound_ntn_ue_context> ue = setup_service_bound_ntn_ue(env, ntn_handler, false);
  ASSERT_TRUE(ue.has_value());

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(
      std::vector<ntn_satellite_state>{{"sat-0", make_ecef(0.0, 0.0, 500000.0)}},
      std::vector<ntn_satellite_prediction_step>{
          {std::chrono::seconds{1}, {{"sat-0", make_ecef(0.0, 0.0, 500000.0)}}},
          {std::chrono::seconds{2}, {{"sat-0", make_ecef(0.0, 1.0, 500000.0)}}}}));
  env.drain_f1ap_resource_coordination_requests(ue->du_idx);

  const cu_cp_ntn_beam_status source_beam =
      find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0001");
  ASSERT_TRUE(source_beam.predictive_exit_offset.has_value());
  EXPECT_EQ(source_beam.predictive_exit_offset.value(), std::chrono::seconds{2});
  EXPECT_FALSE(source_beam.new_demand_blocked);
  EXPECT_NE(source_beam.state_reason, "predictive_drain_soon");

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_predictive_drain_soon_beams, 0U);
  EXPECT_EQ(runtime.nof_ntn_predictive_beam_hopping_ues_requested, 0U);

  const cu_cp_ntn_ue_status ue_status = find_ue_status(ntn_handler.get_current_ntn_ue_status(), ue->ue_index);
  EXPECT_EQ(ue_status.service_layer_state, "service_bound");
  EXPECT_EQ(ue_status.connected_handover_state, "none");
  expect_no_f1ap_ue_setup_or_release(env, ue->du_idx, std::chrono::milliseconds{50});
}

TEST(cu_cp_ntn_mobility_test, same_beam_satellite_owner_change_updates_assistance_without_handover)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_access_service_layer_ntn_mobility_config();
  params.ntn_location_mobility->served_beam_min_elevation_deg = 80.0;
  params.ntn_location_mobility->max_nof_served_beams          = 1;
  enable_predictive_satellite_state_update(params.ntn_location_mobility.value());
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  const std::optional<service_bound_ntn_ue_context> ue = setup_service_bound_ntn_ue(env, ntn_handler, false);
  ASSERT_TRUE(ue.has_value());

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(
      std::vector<ntn_satellite_state>{{"sat-A", make_ecef(0.0, 0.0, 500000.0)}},
      std::vector<ntn_satellite_state>{}));
  env.drain_f1ap_resource_coordination_requests(ue->du_idx);
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(
      std::vector<ntn_satellite_state>{{"sat-B", make_ecef(0.0, 0.0, 500000.0)}},
      std::vector<ntn_satellite_state>{}));
  env.drain_f1ap_resource_coordination_requests(ue->du_idx);

  const cu_cp_ntn_beam_status source_beam =
      find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0001");
  EXPECT_EQ(source_beam.serving_satellite_id, "sat-B");

  const ntn_assistance_snapshot assistance = ntn_handler.get_current_ntn_assistance_snapshot();
  ASSERT_TRUE(assistance.valid);
  const auto beam_it =
      std::find_if(assistance.beams.begin(), assistance.beams.end(), [](const ntn_assistance_beam_snapshot& beam) {
        return beam.beam_id == "CN-BEAM-0001";
      });
  ASSERT_NE(beam_it, assistance.beams.end());
  EXPECT_EQ(beam_it->serving_satellite_id, "sat-B");

  const cu_cp_ntn_ue_status ue_status = find_ue_status(ntn_handler.get_current_ntn_ue_status(), ue->ue_index);
  EXPECT_EQ(ue_status.service_layer_state, "service_bound");
  EXPECT_EQ(ue_status.connected_handover_state, "none");
  expect_no_f1ap_ue_setup_or_release(env, ue->du_idx, std::chrono::milliseconds{50});
}

TEST(cu_cp_ntn_mobility_test, predictive_window_prepares_connected_handover_before_actual_drain)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_access_service_layer_ntn_mobility_config();
  params.ntn_location_mobility->served_beam_min_elevation_deg = 80.0;
  params.ntn_location_mobility->max_nof_served_beams          = 1;
  enable_predictive_satellite_state_update(params.ntn_location_mobility.value());
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));
  ASSERT_EQ(ntn_handler.get_current_ntn_served_beam_ids(), std::vector<std::string>({"CN-BEAM-0001"}));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  const ue_index_t ue_index = finish_ntn_access_only_registration(env,
                                                                  du_idx.value(),
                                                                  cu_up_idx.value(),
                                                                  du_ue_id,
                                                                  to_rnti(0x4601),
                                                                  amf_ue_id_t::min,
                                                                  make_supported_ntn_ngso_ue_capability_info_pdu());
  ASSERT_NE(ue_index, ue_index_t::invalid);

  ASSERT_TRUE(env.request_pdu_session_resource_setup(du_idx.value(), cu_up_idx.value(), du_ue_id));
  ASSERT_TRUE(setup_pdu_session_with_five_qi(env,
                                             du_idx.value(),
                                             cu_up_idx.value(),
                                             du_ue_id,
                                             to_rnti(0x4601),
                                             int_to_gnb_cu_up_ue_e1ap_id(0),
                                             pdu_session_id_t::min,
                                             drb_id_t::drb1,
                                             uint_to_qos_flow_id(0),
                                             9,
                                             true));

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());
  expect_and_ack_ntn_slot_update(env, du_idx.value(), du_ue_id, ue_ctx->cu_ue_id.value(), to_rnti(0x4601));
  expect_service_binding_ready(env, du_idx.value(), ntn_handler, ue_index);

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0),
                                                            make_ecef(0.0, 1.0, 500000.0)));

  f1ap_message target_setup;
  ASSERT_TRUE(wait_for_ue_context_setup_request(env, du_idx.value(), target_setup));
  const auto& setup_req = target_setup.pdu.init_msg().value.ue_context_setup_request();
  ASSERT_EQ(setup_req->sp_cell_id.nr_cell_id.to_number(), make_default_env_nci(1).value());
  ASSERT_TRUE(setup_req->res_coordination_transfer_container_present);
  const std::optional<f1ap_ntn_ul_slot_resource_request> target_request =
      decode_f1ap_ntn_ul_slot_resource_request(setup_req->res_coordination_transfer_container);
  ASSERT_TRUE(target_request.has_value());
  ASSERT_TRUE(target_request->requested_c_rnti.has_value());

  const cu_cp_ntn_ue_status handover_status = find_ue_status(ntn_handler.get_current_ntn_ue_status(), ue_index);
  ASSERT_EQ(handover_status.connected_handover_state, "target_resource_preparing");
  ASSERT_EQ(handover_status.connected_handover_reason, "predictive_beam_hopping");
  ASSERT_EQ(handover_status.connected_handover_source_beam_id.value(), "CN-BEAM-0001");
  ASSERT_EQ(handover_status.connected_handover_target_beam_id.value(), "CN-BEAM-0002");
  ASSERT_EQ(handover_status.connected_handover_target_c_rnti, target_request->requested_c_rnti.value());

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_predictive_beam_hopping_ues_requested, 1U);
  EXPECT_EQ(runtime.nof_ntn_predictive_beam_hopping_ues_scheduled, 1U);
  EXPECT_EQ(runtime.nof_ntn_predictive_beam_hopping_ues_skipped, 0U);
  EXPECT_EQ(runtime.nof_ntn_beam_hopping_ues_requested, 0U);

  e1ap_message e1ap_pdu;
  ASSERT_FALSE(env.wait_for_e1ap_tx_pdu(cu_up_idx.value(), e1ap_pdu, std::chrono::milliseconds{20}));

  env.drain_f1ap_resource_coordination_requests(du_idx.value());
  env.get_du(du_idx.value())
      .push_ul_pdu(test_helpers::generate_ue_context_setup_failure(
          int_to_gnb_cu_ue_f1ap_id(setup_req->gnb_cu_ue_f1ap_id), du_ue_id));
  expect_no_f1ap_ue_setup_or_release(env, du_idx.value(), std::chrono::milliseconds{50});
}

TEST(cu_cp_ntn_mobility_test, predictive_handover_service_pair_target_keeps_dl_anchor_and_ul_resource)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_service_pair_handover_ntn_mobility_config();
  params.ntn_location_mobility->served_beam_min_elevation_deg = 80.0;
  params.ntn_location_mobility->max_nof_served_beams          = 2;
  enable_predictive_satellite_state_update(params.ntn_location_mobility.value());
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beam_cells(env, {0, 1, 2});
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  const ue_index_t ue_index = finish_ntn_access_only_registration(env,
                                                                  du_idx.value(),
                                                                  cu_up_idx.value(),
                                                                  du_ue_id,
                                                                  to_rnti(0x4601),
                                                                  amf_ue_id_t::min,
                                                                  make_supported_ntn_ngso_ue_capability_info_pdu());
  ASSERT_NE(ue_index, ue_index_t::invalid);

  ASSERT_TRUE(env.request_pdu_session_resource_setup(du_idx.value(), cu_up_idx.value(), du_ue_id));
  ASSERT_TRUE(setup_pdu_session_with_five_qi(env,
                                             du_idx.value(),
                                             cu_up_idx.value(),
                                             du_ue_id,
                                             to_rnti(0x4601),
                                             int_to_gnb_cu_up_ue_e1ap_id(0),
                                             pdu_session_id_t::min,
                                             drb_id_t::drb1,
                                             uint_to_qos_flow_id(0),
                                             9,
                                             true));

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());
  expect_and_ack_ntn_slot_update(env, du_idx.value(), du_ue_id, ue_ctx->cu_ue_id.value(), to_rnti(0x4601));
  expect_service_binding_ready(env, du_idx.value(), ntn_handler, ue_index);

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0),
                                                            make_ecef(0.0, 1.0, 500000.0)));

  f1ap_message target_setup;
  ASSERT_TRUE(wait_for_ue_context_setup_request(env, du_idx.value(), target_setup));
  const auto& setup_req = target_setup.pdu.init_msg().value.ue_context_setup_request();
  ASSERT_EQ(setup_req->sp_cell_id.nr_cell_id.to_number(), make_default_env_nci(1).value());
  ASSERT_TRUE(setup_req->res_coordination_transfer_container_present);
  const std::optional<f1ap_ntn_ul_slot_resource_request> target_request =
      decode_f1ap_ntn_ul_slot_resource_request(setup_req->res_coordination_transfer_container);
  ASSERT_TRUE(target_request.has_value());
  const cu_cp_ntn_beam_status uplink_resource_beam =
      find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-UL-0002");
  ASSERT_TRUE(target_request->sr_slot_offset.has_value());
  ASSERT_TRUE(target_request->srs_slot_offset.has_value());
  EXPECT_EQ(*target_request->sr_slot_offset, uplink_resource_beam.sr_slot_offset);
  EXPECT_EQ(*target_request->srs_slot_offset, uplink_resource_beam.srs_slot_offset);

  const cu_cp_ntn_ue_status handover_status = find_ue_status(ntn_handler.get_current_ntn_ue_status(), ue_index);
  ASSERT_EQ(handover_status.connected_handover_state, "target_resource_preparing");
  ASSERT_EQ(handover_status.connected_handover_reason, "predictive_beam_hopping");
  ASSERT_EQ(handover_status.connected_handover_source_beam_id.value(), "CN-BEAM-0001");
  ASSERT_EQ(handover_status.connected_handover_target_beam_id.value(), "CN-BEAM-0002");
  ASSERT_TRUE(handover_status.connected_handover_target_uplink_resource_beam_id.has_value());
  EXPECT_EQ(handover_status.connected_handover_target_uplink_resource_beam_id.value(), "CN-BEAM-UL-0002");
  ASSERT_TRUE(handover_status.connected_handover_target_uplink_resource_nci.has_value());
  EXPECT_EQ(handover_status.connected_handover_target_uplink_resource_nci.value(), make_default_env_nci(2));

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_predictive_beam_hopping_ues_requested, 1U);
  EXPECT_EQ(runtime.nof_ntn_predictive_beam_hopping_ues_scheduled, 1U);
  EXPECT_EQ(runtime.nof_ntn_service_pair_handover_targets, 1U);
  EXPECT_EQ(runtime.nof_ntn_service_pair_handover_scheduled, 1U);
  EXPECT_EQ(runtime.last_ntn_service_pair_handover_reason, "same_analog_tac_uplink_resource");

  e1ap_message e1ap_pdu;
  ASSERT_FALSE(env.wait_for_e1ap_tx_pdu(cu_up_idx.value(), e1ap_pdu, std::chrono::milliseconds{20}));
  env.drain_f1ap_resource_coordination_requests(du_idx.value());
  env.get_du(du_idx.value())
      .push_ul_pdu(test_helpers::generate_ue_context_setup_failure(
          int_to_gnb_cu_ue_f1ap_id(setup_req->gnb_cu_ue_f1ap_id), du_ue_id));
  expect_no_f1ap_ue_setup_or_release(env, du_idx.value(), std::chrono::milliseconds{50});
}

TEST(cu_cp_ntn_mobility_test, beam_exiting_within_handover_lead_blocks_new_demand_and_prepares_handover)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_access_service_layer_ntn_mobility_config();
  params.ntn_location_mobility->served_beam_min_elevation_deg = 80.0;
  params.ntn_location_mobility->max_nof_served_beams          = 1;
  enable_predictive_timeline_satellite_state_update(params.ntn_location_mobility.value());
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  const std::optional<service_bound_ntn_ue_context> ue = setup_service_bound_ntn_ue(env, ntn_handler, false);
  ASSERT_TRUE(ue.has_value());

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(
      std::vector<ntn_satellite_state>{{"sat-0", make_ecef(0.0, 0.0, 500000.0)}},
      std::vector<ntn_satellite_prediction_step>{
          {std::chrono::seconds{1}, {{"sat-0", make_ecef(0.0, 1.0, 500000.0)}}},
          {std::chrono::seconds{2}, {{"sat-0", make_ecef(0.0, 1.0, 500000.0)}}}}));

  f1ap_message target_setup;
  ASSERT_TRUE(wait_for_ue_context_setup_request(env, ue->du_idx, target_setup));
  const auto& setup_req = target_setup.pdu.init_msg().value.ue_context_setup_request();
  ASSERT_EQ(setup_req->sp_cell_id.nr_cell_id.to_number(), make_default_env_nci(1).value());

  const cu_cp_ntn_beam_status source_beam =
      find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0001");
  EXPECT_EQ(source_beam.state_reason, "predictive_drain_soon");
  EXPECT_TRUE(source_beam.new_demand_blocked);
  ASSERT_TRUE(source_beam.predictive_exit_offset.has_value());
  EXPECT_EQ(source_beam.predictive_exit_offset.value(), std::chrono::seconds{1});

  const cu_cp_ntn_ue_status handover_status =
      find_ue_status(ntn_handler.get_current_ntn_ue_status(), ue->ue_index);
  ASSERT_EQ(handover_status.connected_handover_state, "target_resource_preparing");
  ASSERT_EQ(handover_status.connected_handover_reason, "predictive_beam_hopping");
  ASSERT_EQ(handover_status.connected_handover_source_beam_id.value(), "CN-BEAM-0001");
  ASSERT_EQ(handover_status.connected_handover_target_beam_id.value(), "CN-BEAM-0002");

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_predictive_drain_soon_beams, 1U);
  EXPECT_EQ(runtime.nof_ntn_predictive_beam_hopping_ues_requested, 1U);
  EXPECT_EQ(runtime.nof_ntn_predictive_beam_hopping_ues_scheduled, 1U);
  EXPECT_EQ(runtime.nof_ntn_predictive_beam_hopping_ues_skipped, 0U);

  e1ap_message e1ap_pdu;
  ASSERT_FALSE(env.wait_for_e1ap_tx_pdu(ue->cu_up_idx, e1ap_pdu, std::chrono::milliseconds{20}));
  env.drain_f1ap_resource_coordination_requests(ue->du_idx);
  env.get_du(ue->du_idx)
      .push_ul_pdu(test_helpers::generate_ue_context_setup_failure(
          int_to_gnb_cu_ue_f1ap_id(setup_req->gnb_cu_ue_f1ap_id), ue->du_ue_id));
  expect_no_f1ap_ue_setup_or_release(env, ue->du_idx, std::chrono::milliseconds{50});
}

TEST(cu_cp_ntn_mobility_test, multi_satellite_predictive_handover_prepares_cross_satellite_target)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_access_service_layer_ntn_mobility_config();
  params.ntn_location_mobility->served_beam_min_elevation_deg = 80.0;
  params.ntn_location_mobility->max_nof_served_beams          = 1;
  enable_predictive_satellite_state_update(params.ntn_location_mobility.value());
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  const std::optional<service_bound_ntn_ue_context> ue = setup_service_bound_ntn_ue(env, ntn_handler, false);
  ASSERT_TRUE(ue.has_value());

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(
      std::vector<ntn_satellite_state>{{"sat-A", make_ecef(0.0, 0.0, 500000.0)}},
      std::vector<ntn_satellite_state>{{"sat-B", make_ecef(0.0, 1.0, 500000.0)}}));

  f1ap_message target_setup;
  ASSERT_TRUE(wait_for_ue_context_setup_request(env, ue->du_idx, target_setup));
  const auto& setup_req = target_setup.pdu.init_msg().value.ue_context_setup_request();
  ASSERT_EQ(setup_req->sp_cell_id.nr_cell_id.to_number(), make_default_env_nci(1).value());

  const cu_cp_ntn_ue_status handover_status =
      find_ue_status(ntn_handler.get_current_ntn_ue_status(), ue->ue_index);
  ASSERT_EQ(handover_status.connected_handover_state, "target_resource_preparing");
  ASSERT_EQ(handover_status.connected_handover_reason, "multi_satellite_predictive_beam_hopping");
  ASSERT_EQ(handover_status.connected_handover_source_beam_id.value(), "CN-BEAM-0001");
  ASSERT_EQ(handover_status.connected_handover_target_beam_id.value(), "CN-BEAM-0002");

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_TRUE(runtime.multi_satellite_window_valid);
  EXPECT_EQ(runtime.nof_ntn_predictive_beam_hopping_ues_requested, 1U);
  EXPECT_EQ(runtime.nof_ntn_predictive_beam_hopping_ues_scheduled, 1U);
  EXPECT_EQ(runtime.nof_ntn_predictive_beam_hopping_ues_skipped, 0U);

  e1ap_message e1ap_pdu;
  ASSERT_FALSE(env.wait_for_e1ap_tx_pdu(ue->cu_up_idx, e1ap_pdu, std::chrono::milliseconds{20}));
  env.drain_f1ap_resource_coordination_requests(ue->du_idx);
  env.get_du(ue->du_idx)
      .push_ul_pdu(test_helpers::generate_ue_context_setup_failure(
          int_to_gnb_cu_ue_f1ap_id(setup_req->gnb_cu_ue_f1ap_id), ue->du_ue_id));
  expect_no_f1ap_ue_setup_or_release(env, ue->du_idx, std::chrono::milliseconds{50});
}

TEST(cu_cp_ntn_mobility_test, multi_satellite_timeline_handover_uses_cross_satellite_reason)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_access_service_layer_ntn_mobility_config();
  params.ntn_location_mobility->served_beam_min_elevation_deg = 80.0;
  params.ntn_location_mobility->max_nof_served_beams          = 1;
  enable_predictive_timeline_satellite_state_update(params.ntn_location_mobility.value());
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  const std::optional<service_bound_ntn_ue_context> ue = setup_service_bound_ntn_ue(env, ntn_handler, false);
  ASSERT_TRUE(ue.has_value());

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(
      std::vector<ntn_satellite_state>{{"sat-A", make_ecef(0.0, 0.0, 500000.0)}},
      std::vector<ntn_satellite_prediction_step>{
          {std::chrono::seconds{1}, {{"sat-B", make_ecef(0.0, 1.0, 500000.0)}}},
          {std::chrono::seconds{2}, {{"sat-B", make_ecef(0.0, 1.0, 500000.0)}}}}));

  f1ap_message target_setup;
  ASSERT_TRUE(wait_for_ue_context_setup_request(env, ue->du_idx, target_setup));
  const auto& setup_req = target_setup.pdu.init_msg().value.ue_context_setup_request();
  ASSERT_EQ(setup_req->sp_cell_id.nr_cell_id.to_number(), make_default_env_nci(1).value());

  const cu_cp_ntn_ue_status handover_status =
      find_ue_status(ntn_handler.get_current_ntn_ue_status(), ue->ue_index);
  ASSERT_EQ(handover_status.connected_handover_state, "target_resource_preparing");
  ASSERT_EQ(handover_status.connected_handover_reason, "multi_satellite_predictive_beam_hopping");
  ASSERT_EQ(handover_status.connected_handover_source_beam_id.value(), "CN-BEAM-0001");
  ASSERT_EQ(handover_status.connected_handover_target_beam_id.value(), "CN-BEAM-0002");

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_TRUE(runtime.multi_satellite_window_valid);
  EXPECT_EQ(runtime.nof_ntn_predictive_timeline_steps, 2U);
  EXPECT_EQ(runtime.nof_ntn_predictive_drain_soon_beams, 1U);
  EXPECT_EQ(runtime.nof_ntn_predictive_beam_hopping_ues_requested, 1U);
  EXPECT_EQ(runtime.nof_ntn_predictive_beam_hopping_ues_scheduled, 1U);
  EXPECT_EQ(runtime.nof_ntn_predictive_beam_hopping_ues_skipped, 0U);

  e1ap_message e1ap_pdu;
  ASSERT_FALSE(env.wait_for_e1ap_tx_pdu(ue->cu_up_idx, e1ap_pdu, std::chrono::milliseconds{20}));
  env.drain_f1ap_resource_coordination_requests(ue->du_idx);
  env.get_du(ue->du_idx)
      .push_ul_pdu(test_helpers::generate_ue_context_setup_failure(
          int_to_gnb_cu_ue_f1ap_id(setup_req->gnb_cu_ue_f1ap_id), ue->du_ue_id));
  expect_no_f1ap_ue_setup_or_release(env, ue->du_idx, std::chrono::milliseconds{50});
}

TEST(cu_cp_ntn_mobility_test, predictive_window_skips_when_no_eligible_target)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_access_service_layer_ntn_mobility_config();
  params.ntn_location_mobility->served_beam_min_elevation_deg = 80.0;
  params.ntn_location_mobility->max_nof_served_beams          = 1;
  enable_predictive_satellite_state_update(params.ntn_location_mobility.value());
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beam_cells(env, {0});
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  const ue_index_t ue_index = finish_ntn_access_only_registration(env,
                                                                  du_idx.value(),
                                                                  cu_up_idx.value(),
                                                                  du_ue_id,
                                                                  to_rnti(0x4601),
                                                                  amf_ue_id_t::min,
                                                                  make_supported_ntn_ngso_ue_capability_info_pdu());
  ASSERT_NE(ue_index, ue_index_t::invalid);

  ASSERT_TRUE(env.request_pdu_session_resource_setup(du_idx.value(), cu_up_idx.value(), du_ue_id));
  ASSERT_TRUE(setup_pdu_session_with_five_qi(env,
                                             du_idx.value(),
                                             cu_up_idx.value(),
                                             du_ue_id,
                                             to_rnti(0x4601),
                                             int_to_gnb_cu_up_ue_e1ap_id(0),
                                             pdu_session_id_t::min,
                                             drb_id_t::drb1,
                                             uint_to_qos_flow_id(0),
                                             9,
                                             true));

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());
  expect_and_ack_ntn_slot_update(env, du_idx.value(), du_ue_id, ue_ctx->cu_ue_id.value(), to_rnti(0x4601));
  expect_service_binding_ready(env, du_idx.value(), ntn_handler, ue_index);

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0),
                                                            make_ecef(0.0, 1.0, 500000.0)));
  env.drain_f1ap_resource_coordination_requests(du_idx.value());

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_predictive_beam_hopping_ues_requested, 1U);
  EXPECT_EQ(runtime.nof_ntn_predictive_beam_hopping_ues_scheduled, 0U);
  EXPECT_EQ(runtime.nof_ntn_predictive_beam_hopping_ues_skipped, 1U);
  EXPECT_EQ(runtime.nof_ntn_beam_hopping_ues_requested, 0U);

  const cu_cp_ntn_ue_status ue_status = find_ue_status(ntn_handler.get_current_ntn_ue_status(), ue_index);
  EXPECT_EQ(ue_status.service_layer_state, "service_bound");
  EXPECT_EQ(ue_status.connected_handover_state, "none");

  expect_no_f1ap_ue_setup_or_release(env, du_idx.value(), std::chrono::milliseconds{50});
  env.drain_f1ap_resource_coordination_requests(du_idx.value());
}

TEST(cu_cp_ntn_mobility_test, predictive_window_blocks_new_demand_on_drain_soon_beam)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_access_service_layer_ntn_mobility_config();
  params.ntn_location_mobility->served_beam_min_elevation_deg = 80.0;
  params.ntn_location_mobility->max_nof_served_beams          = 1;
  enable_predictive_satellite_state_update(params.ntn_location_mobility.value());
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  ASSERT_TRUE(du_idx.has_value());
  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  const ue_index_t ue_index = finish_ntn_access_only_registration(env,
                                                                  du_idx.value(),
                                                                  cu_up_idx.value(),
                                                                  du_ue_id,
                                                                  to_rnti(0x4601),
                                                                  amf_ue_id_t::min,
                                                                  make_supported_ntn_ngso_ue_capability_info_pdu());
  ASSERT_NE(ue_index, ue_index_t::invalid);

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0),
                                                            make_ecef(0.0, 1.0, 500000.0)));
  env.drain_f1ap_resource_coordination_requests(du_idx.value());

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->amf_ue_id.has_value());
  ASSERT_TRUE(ue_ctx->ran_ue_id.has_value());

  env.get_amf().push_tx_pdu(generate_valid_pdu_session_resource_setup_request_message(
      ue_ctx->amf_ue_id.value(),
      ue_ctx->ran_ue_id.value(),
      {{pdu_session_id_t::min, {pdu_session_type_t::ipv4, {{uint_to_qos_flow_id(0), 9}}}}}));

  ngap_message ngap_pdu;
  ASSERT_TRUE(env.wait_for_ngap_tx_pdu(ngap_pdu));
  ASSERT_TRUE(test_helpers::is_valid_pdu_session_resource_setup_response(ngap_pdu));
  ASSERT_TRUE(test_helpers::is_expected_pdu_session_resource_setup_response(ngap_pdu, {}, {pdu_session_id_t::min}));

  const cu_cp_ntn_ue_status ue_status = find_ue_status(ntn_handler.get_current_ntn_ue_status(), ue_index);
  EXPECT_NE(ue_status.service_layer_state, "service_bound");
  EXPECT_FALSE(ue_status.service_digital_beam_id.has_value());

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_TRUE(runtime.predictive_window_valid);
  EXPECT_EQ(runtime.nof_ntn_predictive_drain_soon_beams, 1U);
  EXPECT_EQ(runtime.nof_ntn_predictive_beam_hopping_ues_requested, 0U);

  env.drain_f1ap_resource_coordination_requests(du_idx.value());
}

TEST(cu_cp_ntn_mobility_test, operator_drain_policy_does_not_trigger_beam_hopping_executor)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_access_service_layer_ntn_mobility_config();
  params.ntn_location_mobility->served_beam_min_elevation_deg = 80.0;
  params.ntn_location_mobility->max_nof_served_beams          = 1;
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  const ue_index_t ue_index = finish_ntn_access_only_registration(env,
                                                                  du_idx.value(),
                                                                  cu_up_idx.value(),
                                                                  du_ue_id,
                                                                  to_rnti(0x4601),
                                                                  amf_ue_id_t::min,
                                                                  make_supported_ntn_ngso_ue_capability_info_pdu());
  ASSERT_NE(ue_index, ue_index_t::invalid);

  ASSERT_TRUE(env.request_pdu_session_resource_setup(du_idx.value(), cu_up_idx.value(), du_ue_id));
  ASSERT_TRUE(setup_pdu_session_with_five_qi(env,
                                             du_idx.value(),
                                             cu_up_idx.value(),
                                             du_ue_id,
                                             to_rnti(0x4601),
                                             int_to_gnb_cu_up_ue_e1ap_id(0),
                                             pdu_session_id_t::min,
                                             drb_id_t::drb1,
                                             uint_to_qos_flow_id(0),
                                             9,
                                             true));

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());
  expect_and_ack_ntn_slot_update(env, du_idx.value(), du_ue_id, ue_ctx->cu_ue_id.value(), to_rnti(0x4601));
  expect_service_binding_ready(env, du_idx.value(), ntn_handler, ue_index);

  ntn_service_switch_over_event event;
  event.event_id          = 112;
  event.type              = ntn_service_switch_over_type::hard;
  event.source            = ntn_service_switch_over_source::operator_command;
  event.policy            = ntn_service_switch_over_policy::drain;
  event.affected_beam_ids = {"CN-BEAM-0001"};
  ASSERT_TRUE(ntn_handler.handle_ntn_service_switch_over_event(event));

  const cu_cp_ntn_beam_status source_beam =
      find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0001");
  ASSERT_EQ(source_beam.state, cu_cp_ntn_beam_assignment_state::draining);

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_beam_hopping_ues_requested, 0U);
  EXPECT_EQ(runtime.nof_ntn_beam_hopping_ues_scheduled, 0U);
  EXPECT_EQ(runtime.nof_ntn_beam_hopping_ues_skipped, 0U);

  expect_no_f1ap_ue_setup_or_release(env, du_idx.value(), std::chrono::milliseconds{50});
  e1ap_message e1ap_pdu;
  ASSERT_FALSE(env.wait_for_e1ap_tx_pdu(cu_up_idx.value(), e1ap_pdu, std::chrono::milliseconds{20}));
}

TEST(cu_cp_ntn_mobility_test, release_allowed_does_not_release_access_only_or_control_only_ue)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_access_service_layer_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beam_cells(env, {0, 1});
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  const ue_index_t ue_index =
      finish_ntn_access_only_registration(env, du_idx.value(), cu_up_idx.value(), du_ue_id, to_rnti(0x4601), amf_ue_id_t::min);
  ASSERT_NE(ue_index, ue_index_t::invalid);
  const cu_cp_ntn_ue_status control_only_status = find_ue_status(ntn_handler.get_current_ntn_ue_status(), ue_index);
  ASSERT_EQ(control_only_status.service_layer_state, "none");
  ASSERT_EQ(control_only_status.ntn_runtime_state, "control_only");

  ntn_service_switch_over_event event;
  event.event_id          = 105;
  event.type              = ntn_service_switch_over_type::hard;
  event.source            = ntn_service_switch_over_source::operator_command;
  event.policy            = ntn_service_switch_over_policy::release_allowed;
  event.affected_beam_ids = {"CN-BEAM-0001"};
  ASSERT_TRUE(ntn_handler.handle_ntn_service_switch_over_event(event));

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_release_allowed_ues_requested, 0U);
  EXPECT_EQ(runtime.nof_ntn_release_allowed_ues_scheduled, 0U);
  EXPECT_EQ(runtime.nof_ntn_release_allowed_ues_skipped, 0U);

  f1ap_message f1ap_pdu;
  ASSERT_FALSE(env.wait_for_f1ap_tx_pdu(du_idx.value(), f1ap_pdu, std::chrono::milliseconds{20}));
}

TEST(cu_cp_ntn_mobility_test, hard_switch_over_blocks_rrc_reestablishment_on_affected_beam)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const rnti_t               old_crnti = to_rnti(0x4601);
  const gnb_du_ue_f1ap_id_t  du_ue_id  = int_to_gnb_du_ue_f1ap_id(0);
  ASSERT_TRUE(env.attach_ue(du_idx.value(),
                            cu_up_idx.value(),
                            du_ue_id,
                            old_crnti,
                            uint_to_amf_ue_id(0),
                            int_to_gnb_cu_up_ue_e1ap_id(0)));

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());
  expect_and_ack_ntn_slot_update(env, du_idx.value(), du_ue_id, ue_ctx->cu_ue_id.value(), old_crnti);

  ntn_service_switch_over_event event;
  event.event_id          = 102;
  event.type              = ntn_service_switch_over_type::hard;
  event.source            = ntn_service_switch_over_source::operator_command;
  event.policy            = ntn_service_switch_over_policy::drain;
  event.affected_beam_ids = {"CN-BEAM-0001"};
  ASSERT_TRUE(ntn_handler.handle_ntn_service_switch_over_event(event));

  auto* cu_cp_impl = get_cu_cp_impl(env);
  ASSERT_NE(cu_cp_impl, nullptr);
  const rrc_ue_reestablishment_context_response response =
      cu_cp_impl->get_cu_cp_rrc_ue_interface().handle_rrc_reestablishment_request(pci_t{1},
                                                                                  old_crnti,
                                                                                  uint_to_ue_index(42));

  EXPECT_EQ(response.ue_index, ue_index_t::invalid);
  EXPECT_FALSE(response.old_ue_fully_attached);
}

TEST(cu_cp_ntn_mobility_test, hard_switch_over_blocks_incoming_n2_handover_request_on_target_beam)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  ASSERT_TRUE(connect_du_for_ntn_beams(env).has_value());
  ASSERT_TRUE(connect_cu_up_for_ue_admission(env));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  auto* cu_cp_impl = get_cu_cp_impl(env);
  ASSERT_NE(cu_cp_impl, nullptr);

  nr_cell_global_id_t target_cgi{plmn_identity::test_value(), make_default_env_nci(0)};
  const ue_index_t    ue_index =
      cu_cp_impl->get_cu_cp_ngap_handler().handle_ue_index_allocation_request(target_cgi, plmn_identity::test_value());
  ASSERT_NE(ue_index, ue_index_t::invalid);

  ntn_service_switch_over_event event;
  event.event_id          = 103;
  event.type              = ntn_service_switch_over_type::hard;
  event.source            = ntn_service_switch_over_source::operator_command;
  event.policy            = ntn_service_switch_over_policy::drain;
  event.affected_beam_ids = {"CN-BEAM-0001"};
  ASSERT_TRUE(ntn_handler.handle_ntn_service_switch_over_event(event));

  ngap_handover_request request = make_minimal_ngap_handover_request(ue_index, target_cgi);
  async_task<ngap_handover_resource_allocation_response> task =
      cu_cp_impl->get_cu_cp_ngap_handler().handle_ngap_handover_request(request);
  lazy_task_launcher<ngap_handover_resource_allocation_response> launcher(task);
  ASSERT_TRUE(task.ready());
  const ngap_handover_resource_allocation_response response = task.get();

  EXPECT_EQ(response.ue_index, ue_index);
  EXPECT_FALSE(response.success);
  assert_ngap_radio_cause(response.cause, ngap_cause_radio_network_t::ho_target_not_allowed);
}

TEST(cu_cp_ntn_mobility_test, zero_served_beam_limit_allows_loaded_service_calendar_without_capping)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  params.ntn_location_mobility->max_nof_served_beams = 0;
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  const ecef_coordinates_t   satellite   = make_ecef(0.0, 0.0, 500000.0);
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(satellite));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  ASSERT_TRUE(env.attach_ue(du_idx.value(),
                            cu_up_idx.value(),
                            du_ue_id,
                            to_rnti(0x4601),
                            uint_to_amf_ue_id(0),
                            int_to_gnb_cu_up_ue_e1ap_id(0)));

  const cu_cp_ntn_beam_status beam_status =
      find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0001");
  ASSERT_EQ(beam_status.state, cu_cp_ntn_beam_assignment_state::active_loaded);
  ASSERT_EQ(beam_status.nof_ues, 1U);
  ASSERT_EQ(beam_status.nof_antenna_slots, 1U);
  ASSERT_EQ(beam_status.sr_slot_period, 1U);
  ASSERT_EQ(beam_status.srs_slot_period, 1U);

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());
  expect_and_ack_ntn_slot_update(env, du_idx.value(), du_ue_id, ue_ctx->cu_ue_id.value(), to_rnti(0x4601));
}

TEST(cu_cp_ntn_mobility_test, online_ntn_ue_loaded_beam_reconfiguration_carries_cucp_sr_srs_slot_request)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  const ecef_coordinates_t   satellite   = make_ecef(0.0, 0.0, 500000.0);

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(satellite));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  ASSERT_TRUE(env.attach_ue(du_idx.value(),
                            cu_up_idx.value(),
                            du_ue_id,
                            to_rnti(0x4601),
                            uint_to_amf_ue_id(0),
                            int_to_gnb_cu_up_ue_e1ap_id(0)));

  const cu_cp_ntn_beam_status beam_status =
      find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0001");
  ASSERT_EQ(beam_status.sr_slot_period, 1U);
  ASSERT_EQ(beam_status.srs_slot_period, 1U);

  f1ap_message f1ap_pdu;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu(du_idx.value(), f1ap_pdu));
  ASSERT_TRUE(test_helpers::is_valid_ue_context_modification_request(f1ap_pdu));

  const auto& mod_req = f1ap_pdu.pdu.init_msg().value.ue_context_mod_request();
  ASSERT_TRUE(mod_req->res_coordination_transfer_container_present);
  const std::optional<f1ap_ntn_ul_slot_resource_request> decoded_slot_request =
      decode_f1ap_ntn_ul_slot_resource_request(mod_req->res_coordination_transfer_container);
  ASSERT_TRUE(decoded_slot_request.has_value());
  ASSERT_TRUE(decoded_slot_request->sr_slot_offset.has_value());
  ASSERT_TRUE(decoded_slot_request->srs_slot_offset.has_value());
  ASSERT_TRUE(decoded_slot_request->sr_slot_period.has_value());
  ASSERT_TRUE(decoded_slot_request->srs_slot_period.has_value());
  ASSERT_EQ(*decoded_slot_request->sr_slot_offset, beam_status.sr_slot_offset);
  ASSERT_EQ(*decoded_slot_request->srs_slot_offset, beam_status.srs_slot_offset);
  ASSERT_EQ(*decoded_slot_request->sr_slot_period, beam_status.sr_slot_period);
  ASSERT_EQ(*decoded_slot_request->srs_slot_period, beam_status.srs_slot_period);
}

TEST(cu_cp_ntn_mobility_test, online_ntn_ue_reconfiguration_is_not_repeated_when_slot_request_is_unchanged)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  const ecef_coordinates_t   satellite   = make_ecef(0.0, 0.0, 500000.0);
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(satellite));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  ASSERT_TRUE(env.attach_ue(du_idx.value(),
                            cu_up_idx.value(),
                            du_ue_id,
                            to_rnti(0x4601),
                            uint_to_amf_ue_id(0),
                            int_to_gnb_cu_up_ue_e1ap_id(0)));

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());

  f1ap_message f1ap_pdu;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu(du_idx.value(), f1ap_pdu));
  ASSERT_TRUE(test_helpers::is_valid_ue_context_modification_request(f1ap_pdu));

  const auto& first_mod_req = f1ap_pdu.pdu.init_msg().value.ue_context_mod_request();
  ASSERT_TRUE(first_mod_req->res_coordination_transfer_container_present);
  const std::optional<f1ap_ntn_ul_slot_resource_request> first_slot_request =
      decode_f1ap_ntn_ul_slot_resource_request(first_mod_req->res_coordination_transfer_container);
  ASSERT_TRUE(first_slot_request.has_value());
  ASSERT_TRUE(first_slot_request->sr_slot_offset.has_value());
  ASSERT_TRUE(first_slot_request->srs_slot_offset.has_value());
  ASSERT_TRUE(first_slot_request->sr_slot_period.has_value());
  ASSERT_TRUE(first_slot_request->srs_slot_period.has_value());

  env.get_du(du_idx.value())
      .push_ul_pdu(test_helpers::generate_ue_context_modification_response(
          du_ue_id,
          ue_ctx->cu_ue_id.value(),
          to_rnti(0x4601),
          {},
          {},
          byte_buffer{},
          make_successful_ntn_ul_slot_result(first_slot_request)));
  ASSERT_FALSE(env.wait_for_f1ap_tx_pdu(du_idx.value(), f1ap_pdu, std::chrono::milliseconds{20}));

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.1, 500000.0)));
  const cu_cp_ntn_beam_status beam_status =
      find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0001");
  ASSERT_EQ(beam_status.state, cu_cp_ntn_beam_assignment_state::active_loaded);
  ASSERT_EQ(beam_status.sr_slot_period, 1U);
  ASSERT_EQ(beam_status.srs_slot_period, 1U);
  ASSERT_EQ(beam_status.sr_slot_offset, *first_slot_request->sr_slot_offset);
  ASSERT_EQ(beam_status.srs_slot_offset, *first_slot_request->srs_slot_offset);
  ASSERT_EQ(beam_status.sr_slot_period, *first_slot_request->sr_slot_period);
  ASSERT_EQ(beam_status.srs_slot_period, *first_slot_request->srs_slot_period);

  ASSERT_FALSE(env.wait_for_f1ap_tx_pdu(du_idx.value(), f1ap_pdu, std::chrono::milliseconds{20}));
}

TEST(cu_cp_ntn_mobility_test, online_ntn_ue_reconfiguration_keeps_slot_request_after_drb_release_when_ue_stays)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  const ecef_coordinates_t   satellite   = make_ecef(0.0, 0.0, 500000.0);
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(satellite));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  const rnti_t              crnti    = to_rnti(0x4601);
  ASSERT_TRUE(env.attach_ue(du_idx.value(),
                            cu_up_idx.value(),
                            du_ue_id,
                            crnti,
                            uint_to_amf_ue_id(0),
                            int_to_gnb_cu_up_ue_e1ap_id(0)));

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());
  ASSERT_TRUE(ue_ctx->ran_ue_id.has_value());
  ASSERT_TRUE(ue_ctx->cu_cp_e1ap_id.has_value());
  ASSERT_TRUE(ue_ctx->cu_up_e1ap_id.has_value());

  f1ap_message f1ap_pdu;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu(du_idx.value(), f1ap_pdu));
  ASSERT_TRUE(test_helpers::is_valid_ue_context_modification_request(f1ap_pdu));
  const auto& slot_mod_req = f1ap_pdu.pdu.init_msg().value.ue_context_mod_request();
  ASSERT_TRUE(slot_mod_req->res_coordination_transfer_container_present);
  const std::optional<f1ap_ntn_ul_slot_resource_request> first_slot_request =
      decode_f1ap_ntn_ul_slot_resource_request(slot_mod_req->res_coordination_transfer_container);
  ASSERT_TRUE(first_slot_request.has_value());
  ASSERT_FALSE(is_empty(*first_slot_request));

  env.get_du(du_idx.value())
      .push_ul_pdu(test_helpers::generate_ue_context_modification_response(
          du_ue_id,
          ue_ctx->cu_ue_id.value(),
          crnti,
          {},
          {},
          byte_buffer{},
          make_successful_ntn_ul_slot_result(first_slot_request)));
  ASSERT_FALSE(env.wait_for_f1ap_tx_pdu(du_idx.value(), f1ap_pdu, std::chrono::milliseconds{20}));

  env.get_amf().push_tx_pdu(generate_valid_pdu_session_resource_release_command(
      uint_to_amf_ue_id(0), ue_ctx->ran_ue_id.value(), pdu_session_id_t::min));

  e1ap_message e1ap_pdu;
  ASSERT_TRUE(env.wait_for_e1ap_tx_pdu(cu_up_idx.value(), e1ap_pdu));
  ASSERT_TRUE(test_helpers::is_valid_bearer_context_release_command(e1ap_pdu));

  env.get_cu_up(cu_up_idx.value())
      .push_tx_pdu(generate_bearer_context_release_complete(ue_ctx->cu_cp_e1ap_id.value(),
                                                            ue_ctx->cu_up_e1ap_id.value()));
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu(du_idx.value(), f1ap_pdu));
  ASSERT_TRUE(test_helpers::is_valid_ue_context_modification_request(f1ap_pdu));
  const auto& release_mod_req = f1ap_pdu.pdu.init_msg().value.ue_context_mod_request();
  std::optional<f1ap_ntn_ul_slot_resource_request> release_slot_request;
  if (release_mod_req->res_coordination_transfer_container_present) {
    release_slot_request = decode_f1ap_ntn_ul_slot_resource_request(release_mod_req->res_coordination_transfer_container);
  }

  env.get_du(du_idx.value())
      .push_ul_pdu(test_helpers::generate_ue_context_modification_response(du_ue_id,
                                                                           ue_ctx->cu_ue_id.value(),
                                                                           crnti,
                                                                           {drb_id_t::drb1},
                                                                           {},
                                                                           test_helpers::create_cell_group_config(),
                                                                           make_successful_ntn_ul_slot_result(
                                                                               release_slot_request)));
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu(du_idx.value(), f1ap_pdu));
  ASSERT_TRUE(test_helpers::is_valid_dl_rrc_message_transfer(f1ap_pdu));

  env.get_du(du_idx.value())
      .push_ul_pdu(test_helpers::generate_ul_rrc_message_transfer(
          du_ue_id,
          ue_ctx->cu_ue_id.value(),
          srb_id_t::srb1,
          generate_rrc_reconfiguration_complete_pdu(0, 8)));

  ngap_message ngap_pdu;
  ASSERT_TRUE(env.wait_for_ngap_tx_pdu(ngap_pdu));
  ASSERT_TRUE(test_helpers::is_valid_pdu_session_resource_release_response(ngap_pdu));

  const cu_cp_ntn_beam_status beam_status =
      find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0001");
  ASSERT_EQ(beam_status.state, cu_cp_ntn_beam_assignment_state::active_loaded);
  ASSERT_EQ(beam_status.nof_ues, 1U);
  ASSERT_EQ(beam_status.nof_drbs, 0U);
  ASSERT_EQ(beam_status.sr_slot_period, 1U);
  ASSERT_EQ(beam_status.srs_slot_period, 1U);

  ASSERT_FALSE(env.wait_for_f1ap_tx_pdu(du_idx.value(), f1ap_pdu, std::chrono::milliseconds{20}));
}

TEST(cu_cp_ntn_mobility_test, initial_context_setup_for_ntn_user_loaded_beam_carries_cucp_sr_srs_slot_request)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  ASSERT_TRUE(du_idx.has_value());
  ASSERT_TRUE(connect_cu_up_for_ue_admission(env));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  const ecef_coordinates_t   satellite   = make_ecef(0.0, 0.0, 500000.0);

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(satellite));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  ASSERT_TRUE(env.connect_new_ue(du_idx.value(), du_ue_id, to_rnti(0x4601)));
  ASSERT_TRUE(env.authenticate_ue(du_idx.value(), du_ue_id, amf_ue_id_t::min));

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->amf_ue_id.has_value());
  ASSERT_TRUE(ue_ctx->ran_ue_id.has_value());

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(satellite));
  const cu_cp_ntn_beam_status beam_status =
      find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0001");
  ASSERT_EQ(beam_status.sr_slot_period, 1U);
  ASSERT_EQ(beam_status.srs_slot_period, 1U);

  env.get_amf().push_tx_pdu(
      generate_valid_initial_context_setup_request_message(ue_ctx->amf_ue_id.value(), ue_ctx->ran_ue_id.value()));

  f1ap_message f1ap_pdu;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu(du_idx.value(), f1ap_pdu));
  ASSERT_TRUE(test_helpers::is_valid_ue_context_setup_request(f1ap_pdu));

  const auto& setup_req = f1ap_pdu.pdu.init_msg().value.ue_context_setup_request();
  ASSERT_TRUE(setup_req->res_coordination_transfer_container_present);
  const std::optional<f1ap_ntn_ul_slot_resource_request> decoded_slot_request =
      decode_f1ap_ntn_ul_slot_resource_request(setup_req->res_coordination_transfer_container);
  ASSERT_TRUE(decoded_slot_request.has_value());
  ASSERT_TRUE(decoded_slot_request->sr_slot_offset.has_value());
  ASSERT_TRUE(decoded_slot_request->srs_slot_offset.has_value());
  ASSERT_TRUE(decoded_slot_request->sr_slot_period.has_value());
  ASSERT_TRUE(decoded_slot_request->srs_slot_period.has_value());
  ASSERT_EQ(*decoded_slot_request->sr_slot_offset, beam_status.sr_slot_offset);
  ASSERT_EQ(*decoded_slot_request->srs_slot_offset, beam_status.srs_slot_offset);
  ASSERT_EQ(*decoded_slot_request->sr_slot_period, beam_status.sr_slot_period);
  ASSERT_EQ(*decoded_slot_request->srs_slot_period, beam_status.srs_slot_period);
}

TEST(cu_cp_ntn_mobility_test, satellite_state_update_keeps_candidate_until_du_capacity_later_becomes_available)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));
  ASSERT_TRUE(ntn_handler.get_current_ntn_served_beam_ids().empty());

  ASSERT_TRUE(connect_du_for_ntn_beams(env).has_value());

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));
  ASSERT_EQ(ntn_handler.get_current_ntn_served_beam_ids(), std::vector<std::string>({"CN-BEAM-0001"}));
}

TEST(cu_cp_ntn_mobility_test, current_served_beams_are_cleared_when_no_du_can_keep_visible_beams_active)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  ASSERT_TRUE(du_idx.has_value());

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));
  ASSERT_EQ(ntn_handler.get_current_ntn_served_beam_ids(), std::vector<std::string>({"CN-BEAM-0001"}));

  ASSERT_TRUE(env.drop_du_connection(du_idx.value()));

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 1.0, 500000.0)));
  ASSERT_TRUE(ntn_handler.get_current_ntn_served_beam_ids().empty());
}

TEST(cu_cp_ntn_mobility_test, unchanged_satellite_state_recomputes_placement_after_du_capacity_changes)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  ASSERT_TRUE(du_idx.has_value());

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();

  const ecef_coordinates_t satellite = make_ecef(0.0, 0.0, 500000.0);
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(satellite));
  ASSERT_EQ(ntn_handler.get_current_ntn_served_beam_ids(), std::vector<std::string>({"CN-BEAM-0001"}));

  ASSERT_TRUE(env.drop_du_connection(du_idx.value()));

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(satellite));
  ASSERT_TRUE(ntn_handler.get_current_ntn_served_beam_ids().empty());

  const std::vector<cu_cp_ntn_beam_status> beam_status = ntn_handler.get_current_ntn_beam_status();
  ASSERT_EQ(find_beam_status(beam_status, "CN-BEAM-0001").state, cu_cp_ntn_beam_assignment_state::candidate);
}

TEST(cu_cp_ntn_mobility_test, satellite_state_update_keeps_unsupported_du_beam_as_candidate)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();
  ASSERT_TRUE(connect_du_for_ntn_beam_cells(env, {0}).has_value());

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 1.0, 500000.0)));
  ASSERT_TRUE(ntn_handler.get_current_ntn_served_beam_ids().empty());

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));
  ASSERT_EQ(ntn_handler.get_current_ntn_served_beam_ids(), std::vector<std::string>({"CN-BEAM-0001"}));
}

TEST(cu_cp_ntn_mobility_test, satellite_state_update_selects_multi_beam_hopping_window)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_multi_beam_hopping_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();
  const std::optional<unsigned> du_idx = connect_du_for_ntn_beam_cells(env, {0, 1, 2});
  ASSERT_TRUE(du_idx.has_value());

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 1.0, 500000.0)));
  std::vector<std::string> served_beam_ids = ntn_handler.get_current_ntn_served_beam_ids();
  ASSERT_EQ(served_beam_ids.size(), 3);
  ASSERT_EQ(served_beam_ids.front(), "CN-BEAM-0002");
  ASSERT_NE(std::find(served_beam_ids.begin(), served_beam_ids.end(), "CN-BEAM-0001"), served_beam_ids.end());
  ASSERT_NE(std::find(served_beam_ids.begin(), served_beam_ids.end(), "CN-BEAM-0003"), served_beam_ids.end());

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 2.0, 500000.0)));
  ASSERT_EQ(ntn_handler.get_current_ntn_served_beam_ids(),
            std::vector<std::string>({"CN-BEAM-0003", "CN-BEAM-0002", "CN-BEAM-0001"}));

  env.drain_f1ap_resource_coordination_requests(du_idx.value());
}

TEST(cu_cp_ntn_mobility_test, satellite_state_update_rotates_multi_beam_hopping_window)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_rotating_multi_beam_hopping_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();
  const std::optional<unsigned> du_idx = connect_du_for_ntn_beam_cells(env, {0, 1, 2, 3});
  ASSERT_TRUE(du_idx.has_value());

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  const ecef_coordinates_t   satellite   = make_ecef(0.0, 0.0, 500000.0);

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(satellite));
  ASSERT_EQ(ntn_handler.get_current_ntn_served_beam_ids(),
            std::vector<std::string>({"CN-BEAM-0001", "CN-BEAM-0002"}));

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(satellite));
  ASSERT_EQ(ntn_handler.get_current_ntn_served_beam_ids(),
            std::vector<std::string>({"CN-BEAM-0003", "CN-BEAM-0004"}));

  const std::vector<cu_cp_ntn_beam_status> beam_status = ntn_handler.get_current_ntn_beam_status();
  ASSERT_EQ(find_beam_status(beam_status, "CN-BEAM-0001").state, cu_cp_ntn_beam_assignment_state::candidate);
  ASSERT_FALSE(find_beam_status(beam_status, "CN-BEAM-0001").in_hopping_window);
  ASSERT_EQ(find_beam_status(beam_status, "CN-BEAM-0002").state, cu_cp_ntn_beam_assignment_state::candidate);
  ASSERT_FALSE(find_beam_status(beam_status, "CN-BEAM-0002").in_hopping_window);
  ASSERT_EQ(find_beam_status(beam_status, "CN-BEAM-0003").state, cu_cp_ntn_beam_assignment_state::candidate);
  ASSERT_TRUE(find_beam_status(beam_status, "CN-BEAM-0003").in_hopping_window);
  ASSERT_EQ(find_beam_status(beam_status, "CN-BEAM-0004").state, cu_cp_ntn_beam_assignment_state::candidate);
  ASSERT_TRUE(find_beam_status(beam_status, "CN-BEAM-0004").in_hopping_window);

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(satellite));
  ASSERT_EQ(ntn_handler.get_current_ntn_served_beam_ids(),
            std::vector<std::string>({"CN-BEAM-0001", "CN-BEAM-0002"}));

  env.drain_f1ap_resource_coordination_requests(du_idx.value());
}

TEST(cu_cp_ntn_mobility_test, satellite_state_update_keeps_hopping_window_for_configured_dwell_updates)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_rotating_multi_beam_hopping_ntn_mobility_config();
  params.ntn_location_mobility->served_beam_hopping_dwell_updates = 3;
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();
  const std::optional<unsigned> du_idx = connect_du_for_ntn_beam_cells(env, {0, 1, 2, 3});
  ASSERT_TRUE(du_idx.has_value());

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  const ecef_coordinates_t   satellite   = make_ecef(0.0, 0.0, 500000.0);

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(satellite));
  ASSERT_EQ(ntn_handler.get_current_ntn_served_beam_ids(),
            std::vector<std::string>({"CN-BEAM-0001", "CN-BEAM-0002"}));

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(satellite));
  ASSERT_EQ(ntn_handler.get_current_ntn_served_beam_ids(),
            std::vector<std::string>({"CN-BEAM-0001", "CN-BEAM-0002"}));

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(satellite));
  ASSERT_EQ(ntn_handler.get_current_ntn_served_beam_ids(),
            std::vector<std::string>({"CN-BEAM-0001", "CN-BEAM-0002"}));

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(satellite));
  ASSERT_EQ(ntn_handler.get_current_ntn_served_beam_ids(),
            std::vector<std::string>({"CN-BEAM-0003", "CN-BEAM-0004"}));

  env.drain_f1ap_resource_coordination_requests(du_idx.value());
}

TEST(cu_cp_ntn_mobility_test, satellite_state_update_exposes_supported_candidates_as_mobility_eligible)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_multi_beam_hopping_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();
  ASSERT_TRUE(connect_du_for_ntn_beam_cells(env, {0, 1}).has_value());

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 1.0, 500000.0)));
  const std::vector<std::string> served_beam_ids = ntn_handler.get_current_ntn_served_beam_ids();
  ASSERT_EQ(served_beam_ids.size(), 2);
  ASSERT_EQ(served_beam_ids.front(), "CN-BEAM-0002");
  ASSERT_NE(std::find(served_beam_ids.begin(), served_beam_ids.end(), "CN-BEAM-0001"), served_beam_ids.end());
  ASSERT_EQ(std::find(served_beam_ids.begin(), served_beam_ids.end(), "CN-BEAM-0003"), served_beam_ids.end());

  const std::vector<cu_cp_ntn_beam_status> beam_status = ntn_handler.get_current_ntn_beam_status();
  ASSERT_EQ(find_beam_status(beam_status, "CN-BEAM-0002").state, cu_cp_ntn_beam_assignment_state::candidate);
  ASSERT_EQ(find_beam_status(beam_status, "CN-BEAM-0001").state, cu_cp_ntn_beam_assignment_state::candidate);
  ASSERT_EQ(find_beam_status(beam_status, "CN-BEAM-0003").state, cu_cp_ntn_beam_assignment_state::candidate);
  ASSERT_EQ(find_beam_status(beam_status, "CN-BEAM-0003").du_index, du_index_t::invalid);
}

TEST(cu_cp_ntn_mobility_test, satellite_state_update_retires_empty_candidate_beam_in_runtime_plan)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();
  ASSERT_TRUE(connect_du_for_ntn_beams(env).has_value());

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));
  ASSERT_EQ(find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0001").state,
            cu_cp_ntn_beam_assignment_state::candidate);

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 1.0, 500000.0)));
  const std::vector<cu_cp_ntn_beam_status> beam_status = ntn_handler.get_current_ntn_beam_status();
  ASSERT_EQ(find_beam_status(beam_status, "CN-BEAM-0002").state, cu_cp_ntn_beam_assignment_state::candidate);
  ASSERT_EQ(find_beam_status(beam_status, "CN-BEAM-0001").state, cu_cp_ntn_beam_assignment_state::inactive);
  ASSERT_EQ(ntn_handler.get_current_ntn_served_beam_ids(), std::vector<std::string>({"CN-BEAM-0002"}));
}

TEST(cu_cp_ntn_mobility_test, orbit_driven_satellite_state_update_waits_for_du_capacity)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_orbit_driven_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.get_current_ntn_served_beam_ids().empty());

  ASSERT_TRUE(connect_du_for_ntn_beams(env).has_value());
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));
  ASSERT_EQ(ntn_handler.get_current_ntn_served_beam_ids().size(), 1);
}

TEST(cu_cp_ntn_mobility_test, circular_orbit_config_drives_runtime_served_beam_updates)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_orbit_driven_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.get_current_ntn_served_beam_ids().empty());

  ASSERT_TRUE(connect_du_for_ntn_beams(env).has_value());
  ASSERT_TRUE(env.tick_until(std::chrono::milliseconds{100},
                             [&ntn_handler]() { return ntn_handler.get_current_ntn_served_beam_ids().size() == 1; },
                             false));

  const std::vector<std::string> served_beams = ntn_handler.get_current_ntn_served_beam_ids();
  ASSERT_EQ(served_beams.size(), 1);
  ASSERT_TRUE(served_beams.front() == "CN-BEAM-0001" || served_beams.front() == "CN-BEAM-0002");
  ASSERT_EQ(find_beam_status(ntn_handler.get_current_ntn_beam_status(), served_beams.front()).state,
            cu_cp_ntn_beam_assignment_state::candidate);
}

TEST(cu_cp_ntn_mobility_test, satellite_state_and_ue_location_trigger_intra_du_ntn_handover)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  params.ntn_location_mobility->served_beam_min_elevation_deg = -90.0;
  params.ntn_location_mobility->max_nof_served_beams          = 2;
  params.ntn_location_mobility->max_nof_loaded_digital_service_beams = 2;
  params.ntn_location_mobility->beams[0].analog_beam_id             = "ANALOG-ACCESS-001";
  params.ntn_location_mobility->beams[1].analog_beam_id             = "ANALOG-ACCESS-001";
  ntn_analog_beam_position analog;
  analog.analog_beam_id         = "ANALOG-ACCESS-001";
  analog.center_digital_beam_id = "CN-BEAM-0001";
  analog.child_digital_beam_ids = {"CN-BEAM-0001", "CN-BEAM-0002"};
  analog.is_edge_partial        = true;
  params.ntn_location_mobility->analog_beams = {analog};
  cu_cp_test_environment env(std::move(params));

  env.run_ng_setup();
  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  const ecef_coordinates_t   satellite   = make_ecef(0.0, 0.0, 500000.0);
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(satellite));

  const std::vector<std::string> served_beams = ntn_handler.get_current_ntn_served_beam_ids();
  ASSERT_EQ(served_beams.size(), 2);
  ASSERT_NE(std::find(served_beams.begin(), served_beams.end(), "CN-BEAM-0001"), served_beams.end());
  ASSERT_NE(std::find(served_beams.begin(), served_beams.end(), "CN-BEAM-0002"), served_beams.end());

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  ASSERT_TRUE(env.attach_ue(du_idx.value(),
                            cu_up_idx.value(),
                            du_ue_id,
                            to_rnti(0x4601),
                            uint_to_amf_ue_id(0),
                            int_to_gnb_cu_up_ue_e1ap_id(0)));

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());
  expect_and_ack_ntn_slot_update(env, du_idx.value(), du_ue_id, ue_ctx->cu_ue_id.value(), to_rnti(0x4601));

  auto* cu_cp_impl = get_cu_cp_impl(env);
  ASSERT_NE(cu_cp_impl, nullptr);

  ntn_ue_location_report report =
      make_ntn_location_report(uint_to_ue_index(gnb_cu_ue_f1ap_id_to_uint(ue_ctx->cu_ue_id.value())),
                               make_default_env_nci(0),
                               std::chrono::steady_clock::now());
  report.longitude_deg = 1.0;
  cu_cp_impl->get_cu_cp_measurement_handler().handle_ue_location_report(report);

  f1ap_message f1ap_pdu;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu(du_idx.value(), f1ap_pdu));
  ASSERT_TRUE(test_helpers::is_valid_ue_context_setup_request_with_ue_capabilities(f1ap_pdu));

  const auto& setup_req = f1ap_pdu.pdu.init_msg().value.ue_context_setup_request();
  ASSERT_EQ(setup_req->sp_cell_id.nr_cell_id.to_number(), make_default_env_nci(1).value());

  env.get_du(du_idx.value())
      .push_ul_pdu(test_helpers::generate_ue_context_setup_failure(
          int_to_gnb_cu_ue_f1ap_id(setup_req->gnb_cu_ue_f1ap_id), du_ue_id));
  const auto report_metrics = env.get_cu_cp().get_metrics_handler().request_metrics_report();
  ASSERT_EQ(report_metrics.ues.size(), 1);
}

TEST(cu_cp_ntn_mobility_test, connected_location_handover_preloads_target_digital_beam_before_handover)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  params.ntn_location_mobility->served_beam_min_elevation_deg       = -90.0;
  params.ntn_location_mobility->max_nof_served_beams                = 2;
  params.ntn_location_mobility->max_nof_loaded_digital_service_beams = 2;
  params.ntn_location_mobility->beams[0].analog_beam_id             = "ANALOG-ACCESS-001";
  params.ntn_location_mobility->beams[1].analog_beam_id             = "ANALOG-ACCESS-001";
  ntn_analog_beam_position analog;
  analog.analog_beam_id         = "ANALOG-ACCESS-001";
  analog.center_digital_beam_id = "CN-BEAM-0001";
  analog.child_digital_beam_ids = {"CN-BEAM-0001", "CN-BEAM-0002"};
  analog.is_edge_partial        = true;
  params.ntn_location_mobility->analog_beams = {analog};

  cu_cp_test_environment env(std::move(params));

  env.run_ng_setup();
  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  ASSERT_TRUE(env.attach_ue(du_idx.value(),
                            cu_up_idx.value(),
                            du_ue_id,
                            to_rnti(0x4601),
                            uint_to_amf_ue_id(0),
                            int_to_gnb_cu_up_ue_e1ap_id(0)));

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());
  const ue_index_t ue_index = uint_to_ue_index(gnb_cu_ue_f1ap_id_to_uint(ue_ctx->cu_ue_id.value()));
  expect_and_ack_ntn_slot_update(env, du_idx.value(), du_ue_id, ue_ctx->cu_ue_id.value(), to_rnti(0x4601));

  auto* cu_cp_impl = get_cu_cp_impl(env);
  ASSERT_NE(cu_cp_impl, nullptr);

  ntn_ue_location_report report = make_ntn_location_report(ue_index, make_default_env_nci(0), std::chrono::steady_clock::now());
  report.longitude_deg = 1.0;
  cu_cp_impl->get_cu_cp_measurement_handler().handle_ue_location_report(report);

  const cu_cp_ntn_beam_status target_beam =
      find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0002");
  ASSERT_EQ(target_beam.state, cu_cp_ntn_beam_assignment_state::active_loaded);
  ASSERT_EQ(target_beam.nof_ues, 1U);

  const cu_cp_ntn_ue_status ue_status = find_ue_status(ntn_handler.get_current_ntn_ue_status(), ue_index);
  ASSERT_EQ(ue_status.connected_handover_state, "target_resource_preparing");
  ASSERT_EQ(ue_status.connected_handover_reason, "location_boundary");
  ASSERT_EQ(ue_status.connected_handover_source_beam_id.value(), "CN-BEAM-0001");
  ASSERT_EQ(ue_status.connected_handover_target_beam_id.value(), "CN-BEAM-0002");
  ASSERT_EQ(ue_status.connected_handover_target_du_index, uint_to_du_index(du_idx.value()));
  ASSERT_NE(ue_status.connected_handover_target_c_rnti, rnti_t::INVALID_RNTI);
  ASSERT_EQ(ue_status.connected_handover_target_resource_state, "target_resource_preparing");
  ASSERT_FALSE(ue_status.connected_handover_target_sr_srs_applied);
  ASSERT_EQ(ntn_handler.get_current_ntn_runtime_status().nof_connected_handovers_resource_preparing, 1U);

  f1ap_message f1ap_pdu;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu(du_idx.value(), f1ap_pdu));
  ASSERT_TRUE(test_helpers::is_valid_ue_context_setup_request_with_ue_capabilities(f1ap_pdu));
  const auto& setup_req = f1ap_pdu.pdu.init_msg().value.ue_context_setup_request();
  ASSERT_EQ(setup_req->sp_cell_id.nr_cell_id.to_number(), make_default_env_nci(1).value());
  ASSERT_TRUE(setup_req->res_coordination_transfer_container_present);
  const std::optional<f1ap_ntn_ul_slot_resource_request> target_request =
      decode_f1ap_ntn_ul_slot_resource_request(setup_req->res_coordination_transfer_container);
  ASSERT_TRUE(target_request.has_value());
  ASSERT_TRUE(target_request->requested_c_rnti.has_value());
  ASSERT_EQ(target_request->requested_c_rnti.value(), ue_status.connected_handover_target_c_rnti);

  env.get_du(du_idx.value())
      .push_ul_pdu(test_helpers::generate_ue_context_setup_failure(
          int_to_gnb_cu_ue_f1ap_id(setup_req->gnb_cu_ue_f1ap_id), du_ue_id));
}

TEST(cu_cp_ntn_mobility_test, release_allowed_skips_handover_in_progress_ue)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  params.ntn_location_mobility->served_beam_min_elevation_deg       = -90.0;
  params.ntn_location_mobility->max_nof_served_beams                = 2;
  params.ntn_location_mobility->max_nof_loaded_digital_service_beams = 2;
  params.ntn_location_mobility->beams[0].analog_beam_id             = "ANALOG-ACCESS-001";
  params.ntn_location_mobility->beams[1].analog_beam_id             = "ANALOG-ACCESS-001";
  ntn_analog_beam_position analog;
  analog.analog_beam_id         = "ANALOG-ACCESS-001";
  analog.center_digital_beam_id = "CN-BEAM-0001";
  analog.child_digital_beam_ids = {"CN-BEAM-0001", "CN-BEAM-0002"};
  analog.is_edge_partial        = true;
  params.ntn_location_mobility->analog_beams = {analog};

  cu_cp_test_environment env(std::move(params));

  env.run_ng_setup();
  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  const ue_index_t ue_index = finish_ntn_access_only_registration(env,
                                                                  du_idx.value(),
                                                                  cu_up_idx.value(),
                                                                  du_ue_id,
                                                                  to_rnti(0x4601),
                                                                  uint_to_amf_ue_id(0),
                                                                  make_supported_ntn_ngso_ue_capability_info_pdu());
  ASSERT_NE(ue_index, ue_index_t::invalid);
  ASSERT_TRUE(env.request_pdu_session_resource_setup(du_idx.value(), cu_up_idx.value(), du_ue_id));
  ASSERT_TRUE(setup_pdu_session_with_five_qi(env,
                                             du_idx.value(),
                                             cu_up_idx.value(),
                                             du_ue_id,
                                             to_rnti(0x4601),
                                             int_to_gnb_cu_up_ue_e1ap_id(0),
                                             pdu_session_id_t::min,
                                             drb_id_t::drb1,
                                             uint_to_qos_flow_id(0),
                                             9,
                                             true));

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());
  expect_and_ack_ntn_slot_update(env, du_idx.value(), du_ue_id, ue_ctx->cu_ue_id.value(), to_rnti(0x4601));

  const cu_cp_ntn_ue_status service_status = find_ue_status(ntn_handler.get_current_ntn_ue_status(), ue_index);
  ASSERT_EQ(service_status.service_layer_state, "service_bound");
  ASSERT_TRUE(service_status.service_digital_beam_id.has_value());

  auto* cu_cp_impl = get_cu_cp_impl(env);
  ASSERT_NE(cu_cp_impl, nullptr);
  ntn_ue_location_report report = make_ntn_location_report(ue_index, make_default_env_nci(0), std::chrono::steady_clock::now());
  report.longitude_deg = 1.0;
  cu_cp_impl->get_cu_cp_measurement_handler().handle_ue_location_report(report);

  const cu_cp_ntn_ue_status handover_status = find_ue_status(ntn_handler.get_current_ntn_ue_status(), ue_index);
  ASSERT_EQ(handover_status.connected_handover_state, "target_resource_preparing");
  ASSERT_TRUE(handover_status.service_digital_beam_id.has_value());

  ntn_service_switch_over_event event;
  event.event_id          = 108;
  event.type              = ntn_service_switch_over_type::hard;
  event.source            = ntn_service_switch_over_source::operator_command;
  event.policy            = ntn_service_switch_over_policy::release_allowed;
  event.affected_beam_ids = {handover_status.service_digital_beam_id.value()};
  ASSERT_TRUE(ntn_handler.handle_ntn_service_switch_over_event(event));

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_release_allowed_ues_requested, 1U);
  EXPECT_EQ(runtime.nof_ntn_release_allowed_ues_scheduled, 0U);
  EXPECT_EQ(runtime.nof_ntn_release_allowed_ues_skipped, 1U);

  e1ap_message e1ap_pdu;
  ASSERT_FALSE(env.wait_for_e1ap_tx_pdu(cu_up_idx.value(), e1ap_pdu, std::chrono::milliseconds{20}));

  f1ap_message target_setup;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu(du_idx.value(), target_setup));
  ASSERT_TRUE(test_helpers::is_valid_ue_context_setup_request_with_ue_capabilities(target_setup));
  const auto& setup_req = target_setup.pdu.init_msg().value.ue_context_setup_request();
  env.get_du(du_idx.value())
      .push_ul_pdu(test_helpers::generate_ue_context_setup_failure(
          int_to_gnb_cu_ue_f1ap_id(setup_req->gnb_cu_ue_f1ap_id), du_ue_id));

  f1ap_message release_pdu;
  ASSERT_FALSE(env.wait_for_f1ap_tx_pdu(du_idx.value(), release_pdu, std::chrono::milliseconds{50}));
}

TEST(cu_cp_ntn_mobility_test, handover_preferred_skips_handover_in_progress_ue)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  params.ntn_location_mobility->served_beam_min_elevation_deg       = -90.0;
  params.ntn_location_mobility->max_nof_served_beams                = 2;
  params.ntn_location_mobility->max_nof_loaded_digital_service_beams = 2;
  params.ntn_location_mobility->beams[0].analog_beam_id             = "ANALOG-ACCESS-001";
  params.ntn_location_mobility->beams[1].analog_beam_id             = "ANALOG-ACCESS-001";
  ntn_analog_beam_position analog;
  analog.analog_beam_id         = "ANALOG-ACCESS-001";
  analog.center_digital_beam_id = "CN-BEAM-0001";
  analog.child_digital_beam_ids = {"CN-BEAM-0001", "CN-BEAM-0002"};
  analog.is_edge_partial        = true;
  params.ntn_location_mobility->analog_beams = {analog};

  cu_cp_test_environment env(std::move(params));

  env.run_ng_setup();
  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  const ue_index_t ue_index = finish_ntn_access_only_registration(env,
                                                                  du_idx.value(),
                                                                  cu_up_idx.value(),
                                                                  du_ue_id,
                                                                  to_rnti(0x4601),
                                                                  uint_to_amf_ue_id(0),
                                                                  make_supported_ntn_ngso_ue_capability_info_pdu());
  ASSERT_NE(ue_index, ue_index_t::invalid);
  ASSERT_TRUE(env.request_pdu_session_resource_setup(du_idx.value(), cu_up_idx.value(), du_ue_id));
  ASSERT_TRUE(setup_pdu_session_with_five_qi(env,
                                             du_idx.value(),
                                             cu_up_idx.value(),
                                             du_ue_id,
                                             to_rnti(0x4601),
                                             int_to_gnb_cu_up_ue_e1ap_id(0),
                                             pdu_session_id_t::min,
                                             drb_id_t::drb1,
                                             uint_to_qos_flow_id(0),
                                             9,
                                             true));

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());
  expect_and_ack_ntn_slot_update(env, du_idx.value(), du_ue_id, ue_ctx->cu_ue_id.value(), to_rnti(0x4601));

  const cu_cp_ntn_ue_status service_status = find_ue_status(ntn_handler.get_current_ntn_ue_status(), ue_index);
  ASSERT_EQ(service_status.service_layer_state, "service_bound");
  ASSERT_TRUE(service_status.service_digital_beam_id.has_value());

  auto* cu_cp_impl = get_cu_cp_impl(env);
  ASSERT_NE(cu_cp_impl, nullptr);
  ntn_ue_location_report report =
      make_ntn_location_report(ue_index, make_default_env_nci(0), std::chrono::steady_clock::now());
  report.longitude_deg = 1.0;
  cu_cp_impl->get_cu_cp_measurement_handler().handle_ue_location_report(report);

  const cu_cp_ntn_ue_status original_handover_status =
      find_ue_status(ntn_handler.get_current_ntn_ue_status(), ue_index);
  ASSERT_EQ(original_handover_status.connected_handover_state, "target_resource_preparing");
  ASSERT_EQ(original_handover_status.connected_handover_reason, "location_boundary");

  ntn_service_switch_over_event event;
  event.event_id          = 112;
  event.type              = ntn_service_switch_over_type::hard;
  event.source            = ntn_service_switch_over_source::operator_command;
  event.policy            = ntn_service_switch_over_policy::handover_preferred;
  event.affected_beam_ids = {service_status.service_digital_beam_id.value()};
  ASSERT_TRUE(ntn_handler.handle_ntn_service_switch_over_event(event));

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_handover_preferred_ues_requested, 1U);
  EXPECT_EQ(runtime.nof_ntn_handover_preferred_ues_scheduled, 0U);
  EXPECT_EQ(runtime.nof_ntn_handover_preferred_ues_skipped, 1U);
  EXPECT_EQ(runtime.nof_ntn_release_allowed_ues_requested, 0U);

  const cu_cp_ntn_ue_status skipped_status = find_ue_status(ntn_handler.get_current_ntn_ue_status(), ue_index);
  EXPECT_EQ(skipped_status.connected_handover_state, "target_resource_preparing");
  EXPECT_EQ(skipped_status.connected_handover_reason, "location_boundary");

  f1ap_message target_setup;
  ASSERT_TRUE(wait_for_ue_context_setup_request(env, du_idx.value(), target_setup));
  const auto& setup_req = target_setup.pdu.init_msg().value.ue_context_setup_request();
  env.get_du(du_idx.value())
      .push_ul_pdu(test_helpers::generate_ue_context_setup_failure(
          int_to_gnb_cu_ue_f1ap_id(setup_req->gnb_cu_ue_f1ap_id), du_ue_id));

  expect_no_f1ap_ue_setup_or_release(env, du_idx.value(), std::chrono::milliseconds{50});
}

TEST(cu_cp_ntn_mobility_test, stale_ntn_assistance_suppresses_new_location_handover_target)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_stale_assistance_ntn_mobility_config(2);
  cu_cp_test_environment env(std::move(params));

  env.run_ng_setup();
  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  const ecef_coordinates_t   satellite   = make_ecef(0.0, 0.0, 500000.0);
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(satellite));
  ASSERT_EQ(ntn_handler.get_current_ntn_served_beam_ids().size(), 2U);

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  ASSERT_TRUE(env.attach_ue(du_idx.value(),
                            cu_up_idx.value(),
                            du_ue_id,
                            to_rnti(0x4601),
                            uint_to_amf_ue_id(0),
                            int_to_gnb_cu_up_ue_e1ap_id(0)));

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());
  expect_and_ack_ntn_slot_update(env, du_idx.value(), du_ue_id, ue_ctx->cu_ue_id.value(), to_rnti(0x4601));

  auto* cu_cp_impl = get_cu_cp_impl(env);
  ASSERT_NE(cu_cp_impl, nullptr);

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(satellite));
  expire_ntn_assistance();
  const ntn_assistance_snapshot stale_snapshot = ntn_handler.get_current_ntn_assistance_snapshot();
  ASSERT_FALSE(stale_snapshot.valid);
  ASSERT_EQ(stale_snapshot.invalid_reason, ntn_assistance_invalid_reason::stale_satellite_state);

  ntn_ue_location_report report =
      make_ntn_location_report(uint_to_ue_index(gnb_cu_ue_f1ap_id_to_uint(ue_ctx->cu_ue_id.value())),
                               make_default_env_nci(0),
                               std::chrono::steady_clock::now());
  report.longitude_deg = 1.0;
  cu_cp_impl->get_cu_cp_measurement_handler().handle_ue_location_report(report);

  f1ap_message f1ap_pdu;
  ASSERT_FALSE(env.wait_for_f1ap_tx_pdu(du_idx.value(), f1ap_pdu, std::chrono::milliseconds{20}));
  ASSERT_EQ(find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0001").state,
            cu_cp_ntn_beam_assignment_state::draining);
}

TEST(cu_cp_ntn_mobility_test, satellite_state_and_ue_location_trigger_inter_du_ntn_handover_and_retry_after_failure)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  params.ntn_location_mobility->served_beam_min_elevation_deg = -90.0;
  params.ntn_location_mobility->max_nof_served_beams          = 2;
  cu_cp_test_environment env(std::move(params));

  env.run_ng_setup();
  const std::optional<unsigned> source_du_idx =
      connect_du_for_ntn_beam_cells(env, {0}, int_to_gnb_du_id(0x11));
  ASSERT_TRUE(source_du_idx.has_value());
  const std::optional<unsigned> target_du_idx =
      connect_du_for_ntn_beam_cells(env, {1}, int_to_gnb_du_id(0x12));
  ASSERT_TRUE(target_du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const std::vector<std::string> served_beams = ntn_handler.get_current_ntn_served_beam_ids();
  ASSERT_EQ(served_beams.size(), 2);
  ASSERT_NE(std::find(served_beams.begin(), served_beams.end(), "CN-BEAM-0001"), served_beams.end());
  ASSERT_NE(std::find(served_beams.begin(), served_beams.end(), "CN-BEAM-0002"), served_beams.end());

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  ASSERT_TRUE(env.attach_ue(source_du_idx.value(),
                            cu_up_idx.value(),
                            du_ue_id,
                            to_rnti(0x4601),
                            uint_to_amf_ue_id(0),
                            int_to_gnb_cu_up_ue_e1ap_id(0)));

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(source_du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());
  expect_and_ack_ntn_slot_update(env, source_du_idx.value(), du_ue_id, ue_ctx->cu_ue_id.value(), to_rnti(0x4601));

  auto* cu_cp_impl = get_cu_cp_impl(env);
  ASSERT_NE(cu_cp_impl, nullptr);

  const auto location_time = std::chrono::steady_clock::now();
  ntn_ue_location_report report =
      make_ntn_location_report(uint_to_ue_index(gnb_cu_ue_f1ap_id_to_uint(ue_ctx->cu_ue_id.value())),
                               make_default_env_nci(0),
                               location_time);
  report.longitude_deg = 1.0;
  cu_cp_impl->get_cu_cp_measurement_handler().handle_ue_location_report(report);

  f1ap_message f1ap_pdu;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu(target_du_idx.value(), f1ap_pdu));
  ASSERT_TRUE(test_helpers::is_valid_ue_context_setup_request_with_ue_capabilities(f1ap_pdu));

  const auto& setup_req = f1ap_pdu.pdu.init_msg().value.ue_context_setup_request();
  ASSERT_EQ(setup_req->sp_cell_id.nr_cell_id.to_number(), make_default_env_nci(1).value());

  env.get_du(target_du_idx.value())
      .push_ul_pdu(test_helpers::generate_ue_context_setup_failure(
          int_to_gnb_cu_ue_f1ap_id(setup_req->gnb_cu_ue_f1ap_id), du_ue_id));
  const auto report_metrics = env.get_cu_cp().get_metrics_handler().request_metrics_report();
  ASSERT_EQ(report_metrics.ues.size(), 1);

  report.received_time = location_time + std::chrono::milliseconds{1};
  cu_cp_impl->get_cu_cp_measurement_handler().handle_ue_location_report(report);

  f1ap_message retry_f1ap_pdu;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu(target_du_idx.value(), retry_f1ap_pdu));
  ASSERT_TRUE(test_helpers::is_valid_ue_context_setup_request_with_ue_capabilities(retry_f1ap_pdu));

  const auto& retry_setup_req = retry_f1ap_pdu.pdu.init_msg().value.ue_context_setup_request();
  ASSERT_EQ(retry_setup_req->sp_cell_id.nr_cell_id.to_number(), make_default_env_nci(1).value());

  env.get_du(target_du_idx.value())
      .push_ul_pdu(test_helpers::generate_ue_context_setup_failure(
          int_to_gnb_cu_ue_f1ap_id(retry_setup_req->gnb_cu_ue_f1ap_id), du_ue_id));
  const auto retry_report_metrics = env.get_cu_cp().get_metrics_handler().request_metrics_report();
  ASSERT_EQ(retry_report_metrics.ues.size(), 1);
}

TEST(cu_cp_ntn_mobility_test, release_allowed_skips_release_already_in_progress_ue)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_access_service_layer_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));

  env.run_ng_setup();
  const std::optional<unsigned> source_du_idx = connect_du_for_ntn_beams(env);
  ASSERT_TRUE(source_du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  const ue_index_t ue_index = finish_ntn_access_only_registration(env,
                                                                  source_du_idx.value(),
                                                                  cu_up_idx.value(),
                                                                  du_ue_id,
                                                                  to_rnti(0x4601),
                                                                  uint_to_amf_ue_id(0),
                                                                  make_supported_ntn_ngso_ue_capability_info_pdu());
  ASSERT_NE(ue_index, ue_index_t::invalid);
  ASSERT_TRUE(env.request_pdu_session_resource_setup(source_du_idx.value(), cu_up_idx.value(), du_ue_id));
  ASSERT_TRUE(setup_pdu_session_with_five_qi(env,
                                             source_du_idx.value(),
                                             cu_up_idx.value(),
                                             du_ue_id,
                                             to_rnti(0x4601),
                                             int_to_gnb_cu_up_ue_e1ap_id(0),
                                             pdu_session_id_t::min,
                                             drb_id_t::drb1,
                                             uint_to_qos_flow_id(0),
                                             9,
                                             true));

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(source_du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());
  expect_and_ack_ntn_slot_update(
      env, source_du_idx.value(), du_ue_id, ue_ctx->cu_ue_id.value(), to_rnti(0x4601));

  f1ap_message service_binding_f1ap_pdu;
  ASSERT_FALSE(
      env.wait_for_f1ap_tx_pdu(source_du_idx.value(), service_binding_f1ap_pdu, std::chrono::milliseconds{20}));
  ASSERT_TRUE(env.tick_until(std::chrono::milliseconds{100}, [&]() {
    const std::vector<cu_cp_ntn_ue_status> ue_status = ntn_handler.get_current_ntn_ue_status();
    const auto status_it = std::find_if(ue_status.begin(),
                                        ue_status.end(),
                                        [ue_index](const cu_cp_ntn_ue_status& status) {
                                          return status.ue_index == ue_index;
                                        });
    return status_it != ue_status.end() && status_it->service_layer_state == "service_bound";
  }));

  ntn_service_switch_over_event event;
  event.event_id          = 106;
  event.type              = ntn_service_switch_over_type::hard;
  event.source            = ntn_service_switch_over_source::operator_command;
  event.policy            = ntn_service_switch_over_policy::release_allowed;
  event.affected_beam_ids = {"CN-BEAM-0001"};
  ASSERT_TRUE(ntn_handler.handle_ntn_service_switch_over_event(event));

  f1ap_message policy_coordination_pdu;
  ASSERT_FALSE(
      env.wait_for_f1ap_tx_pdu(source_du_idx.value(), policy_coordination_pdu, std::chrono::milliseconds{100}));

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_release_allowed_ues_requested, 1U);
  EXPECT_EQ(runtime.nof_ntn_release_allowed_ues_scheduled, 1U);
  EXPECT_EQ(runtime.nof_ntn_release_allowed_ues_skipped, 0U);

  ntn_service_switch_over_event duplicate_event = event;
  duplicate_event.event_id                      = 107;
  ASSERT_TRUE(ntn_handler.handle_ntn_service_switch_over_event(duplicate_event));

  const cu_cp_ntn_runtime_status duplicate_runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(duplicate_runtime.nof_ntn_release_allowed_ues_requested, 2U);
  EXPECT_EQ(duplicate_runtime.nof_ntn_release_allowed_ues_scheduled, 1U);
  EXPECT_EQ(duplicate_runtime.nof_ntn_release_allowed_ues_skipped, 1U);

  e1ap_message e1ap_pdu;
  ASSERT_TRUE(env.wait_for_e1ap_tx_pdu(cu_up_idx.value(), e1ap_pdu, std::chrono::milliseconds{1000}));
  ASSERT_TRUE(test_helpers::is_valid_bearer_context_release_command(e1ap_pdu));
  ASSERT_TRUE(ue_ctx->cu_cp_e1ap_id.has_value());
  ASSERT_TRUE(ue_ctx->cu_up_e1ap_id.has_value());
  env.get_cu_up(cu_up_idx.value())
      .push_tx_pdu(generate_bearer_context_release_complete(ue_ctx->cu_cp_e1ap_id.value(),
                                                            ue_ctx->cu_up_e1ap_id.value()));

  f1ap_message release_pdu;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu(source_du_idx.value(), release_pdu, std::chrono::milliseconds{1000}));
  ASSERT_TRUE(test_helpers::is_valid_ue_context_release_command(release_pdu));
  const auto& rel_cmd = release_pdu.pdu.init_msg().value.ue_context_release_cmd();
  env.get_du(source_du_idx.value())
      .push_ul_pdu(test_helpers::generate_ue_context_release_complete(
          int_to_gnb_cu_ue_f1ap_id(rel_cmd->gnb_cu_ue_f1ap_id),
          int_to_gnb_du_ue_f1ap_id(rel_cmd->gnb_du_ue_f1ap_id)));
  ASSERT_TRUE(env.tick_until(std::chrono::milliseconds{1000}, [&]() {
    return env.get_cu_cp().get_metrics_handler().request_metrics_report().ues.empty();
  }));

  f1ap_message post_release_coordination_pdu;
  ASSERT_FALSE(
      env.wait_for_f1ap_tx_pdu(source_du_idx.value(), post_release_coordination_pdu, std::chrono::milliseconds{100}));
}

TEST(cu_cp_ntn_mobility_test, non_selected_access_du_setup_is_temporarily_admitted_and_relocated_before_service)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_analog_access_du_relocation_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));

  env.run_ng_setup();
  const std::optional<unsigned> source_du_idx =
      connect_du_for_ntn_beam_cells(env, {0}, int_to_gnb_du_id(0x11));
  ASSERT_TRUE(source_du_idx.has_value());
  const std::optional<unsigned> target_du_idx =
      connect_du_for_ntn_beam_cells(env, {1, 2}, int_to_gnb_du_id(0x12));
  ASSERT_TRUE(target_du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));
  const cu_cp_ntn_beam_status beam_status =
      find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0001");
  ASSERT_EQ(beam_status.access_du_index, uint_to_du_index(target_du_idx.value()));
  const cu_cp_ntn_beam_status source_beam_status =
      find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0002");
  ASSERT_EQ(source_beam_status.access_du_index, uint_to_du_index(target_du_idx.value()));

  const gnb_du_ue_f1ap_id_t source_du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  ASSERT_TRUE(env.connect_new_ue(source_du_idx.value(), source_du_ue_id, to_rnti(0x4601)));

  const cu_cp_test_environment::ue_context* ue_ctx =
      env.find_ue_context(source_du_idx.value(), source_du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());
  const ue_index_t ue_index = uint_to_ue_index(gnb_cu_ue_f1ap_id_to_uint(ue_ctx->cu_ue_id.value()));

  cu_cp_ntn_ue_status ue_status = find_ue_status(ntn_handler.get_current_ntn_ue_status(), ue_index);
  ASSERT_EQ(ue_status.pre_service_relocation_state, "pending_pre_service");
  ASSERT_EQ(ue_status.pre_service_relocation_reason, "access_du_mismatch");
  ASSERT_EQ(ue_status.pre_service_relocation_target_du_index, uint_to_du_index(target_du_idx.value()));
  ASSERT_EQ(ue_status.pre_service_relocation_target_beam_id.value(), "CN-BEAM-0002");
  ASSERT_EQ(ue_status.serving_nci.value(), make_default_env_nci(0));
  ASSERT_EQ(ue_status.pre_service_relocation_target_nci.value(), make_default_env_nci(1));

  ASSERT_TRUE(env.authenticate_ue(source_du_idx.value(), source_du_ue_id, uint_to_amf_ue_id(0)));
  ASSERT_TRUE(env.setup_ue_security(source_du_idx.value(), source_du_ue_id));

  f1ap_message f1ap_pdu;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu(target_du_idx.value(), f1ap_pdu));
  ASSERT_TRUE(test_helpers::is_valid_ue_context_setup_request_with_ue_capabilities(f1ap_pdu));
  const auto& setup_req = f1ap_pdu.pdu.init_msg().value.ue_context_setup_request();
  ASSERT_EQ(setup_req->sp_cell_id.nr_cell_id.to_number(), make_default_env_nci(1).value());

  ue_status = find_ue_status(ntn_handler.get_current_ntn_ue_status(), ue_index);
  ASSERT_EQ(ue_status.pre_service_relocation_state, "preparing");
  ASSERT_EQ(ntn_handler.get_current_ntn_runtime_status().nof_pre_service_relocations_active, 1U);

  env.get_du(target_du_idx.value())
      .push_ul_pdu(test_helpers::generate_ue_context_setup_failure(
          int_to_gnb_cu_ue_f1ap_id(setup_req->gnb_cu_ue_f1ap_id), source_du_ue_id));
}

TEST(cu_cp_ntn_mobility_test, pending_pre_service_relocation_rejects_first_pdu_session)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_analog_access_du_relocation_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));

  env.run_ng_setup();
  const std::optional<unsigned> source_du_idx =
      connect_du_for_ntn_beam_cells(env, {0}, int_to_gnb_du_id(0x11));
  ASSERT_TRUE(source_du_idx.has_value());
  const std::optional<unsigned> target_du_idx =
      connect_du_for_ntn_beam_cells(env, {1, 2}, int_to_gnb_du_id(0x12));
  ASSERT_TRUE(target_du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const gnb_du_ue_f1ap_id_t source_du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  ASSERT_TRUE(env.connect_new_ue(source_du_idx.value(), source_du_ue_id, to_rnti(0x4601)));
  ASSERT_TRUE(env.authenticate_ue(source_du_idx.value(), source_du_ue_id, uint_to_amf_ue_id(0)));
  ASSERT_TRUE(env.setup_ue_security(source_du_idx.value(), source_du_ue_id));

  f1ap_message target_setup;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu(target_du_idx.value(), target_setup));
  ASSERT_TRUE(test_helpers::is_valid_ue_context_setup_request_with_ue_capabilities(target_setup));
  const auto& target_setup_req = target_setup.pdu.init_msg().value.ue_context_setup_request();

  const cu_cp_test_environment::ue_context* ue_ctx =
      env.find_ue_context(source_du_idx.value(), source_du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->amf_ue_id.has_value());
  ASSERT_TRUE(ue_ctx->ran_ue_id.has_value());

  const pdu_session_id_t psi = uint_to_pdu_session_id(7);
  const ngap_message pdu_session_resource_setup_request = generate_valid_pdu_session_resource_setup_request_message(
      ue_ctx->amf_ue_id.value(), ue_ctx->ran_ue_id.value(), {{psi, {pdu_session_type_t::ipv4, {{uint_to_qos_flow_id(1), 9}}}}});
  env.get_amf().push_tx_pdu(pdu_session_resource_setup_request);

  ngap_message ngap_pdu;
  ASSERT_TRUE(env.wait_for_ngap_tx_pdu(ngap_pdu));
  ASSERT_TRUE(test_helpers::is_valid_pdu_session_resource_setup_response(ngap_pdu));
  ASSERT_TRUE(test_helpers::is_expected_pdu_session_resource_setup_response(ngap_pdu, {}, {psi}));

  e1ap_message e1ap_pdu;
  ASSERT_FALSE(env.get_cu_up(cu_up_idx.value()).try_pop_rx_pdu(e1ap_pdu));

  env.get_du(target_du_idx.value())
      .push_ul_pdu(test_helpers::generate_ue_context_setup_failure(
          int_to_gnb_cu_ue_f1ap_id(target_setup_req->gnb_cu_ue_f1ap_id), source_du_ue_id));
}

TEST(cu_cp_ntn_mobility_test, ntn_enabled_rsrp_measurement_report_does_not_trigger_handover)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));

  env.run_ng_setup();
  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  ASSERT_TRUE(env.attach_ue(du_idx.value(),
                            cu_up_idx.value(),
                            du_ue_id,
                            to_rnti(0x4601),
                            uint_to_amf_ue_id(0),
                            int_to_gnb_cu_up_ue_e1ap_id(0)));

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->du_ue_id.has_value());
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());

  env.get_du(du_idx.value())
      .push_ul_pdu(test_helpers::generate_ul_rrc_message_transfer(
          ue_ctx->du_ue_id.value(),
          ue_ctx->cu_ue_id.value(),
          srb_id_t::srb1,
          make_byte_buffer("000800410004015f741fe0804bf183fcaa6e9699").value()));

  f1ap_message f1ap_pdu;
  ASSERT_FALSE(env.wait_for_f1ap_tx_pdu(du_idx.value(), f1ap_pdu, std::chrono::milliseconds{20}));
}

TEST(cu_cp_ntn_mobility_test, ntn_disabled_rsrp_measurement_report_still_triggers_handover)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility          = make_ntn_mobility_config();
  params.ntn_location_mobility->enabled = false;
  cu_cp_test_environment env(std::move(params));

  env.run_ng_setup();
  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  ASSERT_TRUE(env.attach_ue(du_idx.value(),
                            cu_up_idx.value(),
                            du_ue_id,
                            to_rnti(0x4601),
                            uint_to_amf_ue_id(0),
                            int_to_gnb_cu_up_ue_e1ap_id(0)));

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->du_ue_id.has_value());
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());

  env.get_du(du_idx.value())
      .push_ul_pdu(test_helpers::generate_ul_rrc_message_transfer(
          ue_ctx->du_ue_id.value(),
          ue_ctx->cu_ue_id.value(),
          srb_id_t::srb1,
          make_byte_buffer("000800410004015f741fe0804bf183fcaa6e9699").value()));

  f1ap_message f1ap_pdu;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu(du_idx.value(), f1ap_pdu));
  ASSERT_TRUE(test_helpers::is_valid_ue_context_setup_request_with_ue_capabilities(f1ap_pdu));

  const auto& setup_req = f1ap_pdu.pdu.init_msg().value.ue_context_setup_request();
  ASSERT_EQ(setup_req->sp_cell_id.nr_cell_id.to_number(), make_default_env_nci(1).value());
}

TEST(cu_cp_ntn_mobility_test, location_reporting_control_rejects_unknown_ue)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));

  auto* cu_cp_impl = get_cu_cp_impl(env);
  ASSERT_NE(cu_cp_impl, nullptr);

  ngap_location_reporting_control control;
  control.ue_index = uint_to_ue_index(42);

  const ngap_location_reporting_control_response response =
      cu_cp_impl->get_cu_cp_ngap_handler().handle_location_reporting_control(control);

  ASSERT_FALSE(response.accepted);
  assert_ngap_radio_cause(response.cause, ngap_cause_radio_network_t::unknown_local_ue_ngap_id);
}

TEST(cu_cp_ntn_mobility_test, location_reporting_control_rejects_when_amf_control_is_disabled)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  params.ntn_location_mobility->core_network_reporting.amf_control_enabled = false;
  cu_cp_test_environment env(std::move(params));

  auto* cu_cp_impl = get_cu_cp_impl(env);
  ASSERT_NE(cu_cp_impl, nullptr);

  ngap_location_reporting_control control;
  control.ue_index = uint_to_ue_index(42);

  const ngap_location_reporting_control_response response =
      cu_cp_impl->get_cu_cp_ngap_handler().handle_location_reporting_control(control);

  ASSERT_FALSE(response.accepted);
  assert_ngap_radio_cause(response.cause, ngap_cause_radio_network_t::unspecified);
}

TEST(cu_cp_ntn_mobility_test, location_reporting_control_direct_rejects_when_no_accepted_location_is_available)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));

  env.run_ng_setup();
  const std::optional<connected_ngap_ntn_ue> ue = connect_ngap_ntn_ue(env);
  ASSERT_TRUE(ue.has_value());

  auto* cu_cp_impl = get_cu_cp_impl(env);
  ASSERT_NE(cu_cp_impl, nullptr);

  ngap_location_reporting_control control;
  control.ue_index               = ue->ue_index;
  control.request_type.event_type = ngap_location_reporting_event_type::direct;

  const ngap_location_reporting_control_response response =
      cu_cp_impl->get_cu_cp_ngap_handler().handle_location_reporting_control(control);
  ASSERT_FALSE(response.accepted);
  assert_ngap_radio_cause(response.cause, ngap_cause_radio_network_t::unspecified);
}

TEST(cu_cp_ntn_mobility_test, accepted_ntn_location_report_is_forwarded_to_ngap_location_report)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  params.ntn_location_mobility->core_network_reporting.local_forwarding_enabled = true;
  cu_cp_test_environment env(std::move(params));

  env.run_ng_setup();
  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  ASSERT_TRUE(du_idx.has_value());
  ASSERT_TRUE(env.get_cu_cp()
                  .get_command_handler()
                  .get_ntn_command_handler()
                  .handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));
  ASSERT_TRUE(connect_cu_up_for_ue_admission(env));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  ASSERT_TRUE(env.connect_new_ue(du_idx.value(), du_ue_id, to_rnti(0x4601)));
  ASSERT_TRUE(env.authenticate_ue(du_idx.value(), du_ue_id, uint_to_amf_ue_id(0)));

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());
  ASSERT_TRUE(ue_ctx->ran_ue_id.has_value());
  ASSERT_TRUE(ue_ctx->amf_ue_id.has_value());

  auto* cu_cp_impl = get_cu_cp_impl(env);
  ASSERT_NE(cu_cp_impl, nullptr);

  const ue_index_t ue_index = uint_to_ue_index(gnb_cu_ue_f1ap_id_to_uint(ue_ctx->cu_ue_id.value()));
  cu_cp_impl->get_cu_cp_measurement_handler().handle_ue_location_report(
      make_ntn_location_report(ue_index, make_default_env_nci(0)));

  ngap_message ngap_pdu;
  ASSERT_TRUE(env.wait_for_ngap_tx_pdu(ngap_pdu));
  ASSERT_TRUE(is_pdu_type(ngap_pdu,
                          asn1::ngap::ngap_elem_procs_o::init_msg_c::types::types_opts::location_report));

  const auto& asn1_report = ngap_pdu.pdu.init_msg().value.location_report();
  ASSERT_EQ(asn1_report->ran_ue_ngap_id, ran_ue_id_to_uint(ue_ctx->ran_ue_id.value()));
  ASSERT_EQ(asn1_report->amf_ue_ngap_id, amf_ue_id_to_uint(ue_ctx->amf_ue_id.value()));
  ASSERT_EQ(asn1_report->location_report_request_type.event_type.value, asn1::ngap::event_type_opts::direct);
  ASSERT_EQ(asn1_report->user_location_info.type().value,
            asn1::ngap::user_location_info_c::types_opts::user_location_info_nr);

  const auto& nr_info = asn1_report->user_location_info.user_location_info_nr();
  ASSERT_EQ(nr_info.nr_cgi.nr_cell_id.to_number(), make_default_env_nci(0).value());
  ASSERT_EQ(plmn_identity::from_bytes(nr_info.nr_cgi.plmn_id.to_bytes()).value(), plmn_identity::test_value());
  ASSERT_TRUE(nr_info.time_stamp_present);
  EXPECT_NE(nr_info.time_stamp.to_number(), 0U);

  ASSERT_TRUE(nr_info.ie_exts_present);
  ASSERT_TRUE(nr_info.ie_exts.nr_ntn_tai_info_present);
  const auto& ntn_tai_info = nr_info.ie_exts.nr_ntn_tai_info;
  ASSERT_EQ(ntn_tai_info.tac_list_in_nr_ntn.size(), 1);
  EXPECT_EQ(ntn_tai_info.tac_list_in_nr_ntn[0].to_number(), 1U);
  ASSERT_TRUE(ntn_tai_info.ue_location_derived_tac_in_nr_ntn_present);
  EXPECT_EQ(ntn_tai_info.ue_location_derived_tac_in_nr_ntn.to_number(), 1U);
}

TEST(cu_cp_ntn_mobility_test, ue_context_release_complete_carries_ntn_recommended_cell_with_beam_derived_tac)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));

  env.run_ng_setup();
  const std::optional<connected_ngap_ntn_ue> connected_ue = connect_ngap_ntn_ue(env);
  ASSERT_TRUE(connected_ue.has_value());

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  auto* cu_cp_impl = get_cu_cp_impl(env);
  ASSERT_NE(cu_cp_impl, nullptr);
  cu_cp_impl->get_cu_cp_measurement_handler().handle_ue_location_report(
      make_ntn_location_report(connected_ue->ue_index, make_default_env_nci(0)));

  env.get_amf().push_tx_pdu(generate_valid_ue_context_release_command_with_amf_ue_ngap_id(connected_ue->amf_ue_id));

  f1ap_message f1ap_pdu;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu(connected_ue->du_idx, f1ap_pdu));
  ASSERT_TRUE(test_helpers::is_valid_ue_context_release_command(f1ap_pdu));
  const auto& rel_cmd = f1ap_pdu.pdu.init_msg().value.ue_context_release_cmd();
  env.get_du(connected_ue->du_idx)
      .push_ul_pdu(test_helpers::generate_ue_context_release_complete(
          int_to_gnb_cu_ue_f1ap_id(rel_cmd->gnb_cu_ue_f1ap_id),
          int_to_gnb_du_ue_f1ap_id(rel_cmd->gnb_du_ue_f1ap_id)));

  ngap_message ngap_pdu;
  ASSERT_TRUE(env.wait_for_ngap_tx_pdu(ngap_pdu));
  ASSERT_TRUE(test_helpers::is_valid_ue_context_release_complete(ngap_pdu));

  const auto& release_complete = ngap_pdu.pdu.successful_outcome().value.ue_context_release_complete();
  ASSERT_TRUE(release_complete->info_on_recommended_cells_and_ran_nodes_for_paging_present);
  const auto& recommended_cells =
      release_complete->info_on_recommended_cells_and_ran_nodes_for_paging.recommended_cells_for_paging
          .recommended_cell_list;
  ASSERT_EQ(recommended_cells.size(), 1);
  EXPECT_EQ(recommended_cells[0].ngran_cgi.nr_cgi().nr_cell_id.to_number(), make_default_env_nci(0).value());
  ASSERT_TRUE(release_complete->user_location_info_present);
  const auto& nr_info = release_complete->user_location_info.user_location_info_nr();
  ASSERT_TRUE(nr_info.ie_exts.nr_ntn_tai_info_present);
  const auto& ntn_tai_info = nr_info.ie_exts.nr_ntn_tai_info;
  ASSERT_TRUE(ntn_tai_info.ue_location_derived_tac_in_nr_ntn_present);
  EXPECT_EQ(ntn_tai_info.ue_location_derived_tac_in_nr_ntn.to_number(), 1U);
}

TEST(cu_cp_ntn_mobility_test, idle_ntn_ue_release_persists_paging_context_with_5g_s_tmsi)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_idle_paging_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  const cu_cp_five_g_s_tmsi five_g_s_tmsi = make_test_paging_five_g_s_tmsi();
  const std::optional<service_bound_ntn_ue_context> ue =
      setup_service_bound_ntn_ue(env, ntn_handler, true, std::chrono::steady_clock::now(), five_g_s_tmsi);
  ASSERT_TRUE(ue.has_value());

  complete_service_bound_ue_release(env, *ue);

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_idle_paging_contexts, 1U);
  EXPECT_EQ(runtime.nof_ntn_idle_paging_contexts_with_5g_s_tmsi, 1U);
  EXPECT_EQ(runtime.nof_ntn_idle_paging_contexts_expired, 0U);
}

TEST(cu_cp_ntn_mobility_test, ntn_service_bound_ue_inactivity_enters_rrc_inactive)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_idle_paging_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  const cu_cp_five_g_s_tmsi five_g_s_tmsi = make_test_paging_five_g_s_tmsi();
  const std::optional<service_bound_ntn_ue_context> ue =
      setup_service_bound_ntn_ue(env, ntn_handler, true, std::chrono::steady_clock::now(), five_g_s_tmsi);
  ASSERT_TRUE(ue.has_value());

  env.get_cu_up(ue->cu_up_idx)
      .push_tx_pdu(generate_bearer_context_inactivity_notification_with_ue_level(ue->cu_cp_e1ap_id,
                                                                                 ue->cu_up_e1ap_id));

  f1ap_message f1ap_pdu;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu(ue->du_idx, f1ap_pdu, std::chrono::milliseconds{1000}));
  ASSERT_TRUE(test_helpers::is_valid_ue_context_release_command(f1ap_pdu));
  const auto& release_cmd = f1ap_pdu.pdu.init_msg().value.ue_context_release_cmd();
  ASSERT_TRUE(release_cmd->rrc_container_present);
  ASSERT_TRUE(release_cmd->srb_id_present);
  EXPECT_GT(release_cmd->rrc_container.size(), 0U);
  env.get_du(ue->du_idx)
      .push_ul_pdu(test_helpers::generate_ue_context_release_complete(
          int_to_gnb_cu_ue_f1ap_id(release_cmd->gnb_cu_ue_f1ap_id),
          int_to_gnb_du_ue_f1ap_id(release_cmd->gnb_du_ue_f1ap_id)));

  ngap_message ngap_pdu;
  ASSERT_TRUE(env.wait_for_ngap_tx_pdu(ngap_pdu, std::chrono::milliseconds{1000}));
  ASSERT_EQ(ngap_pdu.pdu.init_msg().value.type(),
            asn1::ngap::ngap_elem_procs_o::init_msg_c::types_opts::ue_context_suspend_request);
  const auto& suspend = ngap_pdu.pdu.init_msg().value.ue_context_suspend_request();
  EXPECT_EQ(suspend->amf_ue_ngap_id, amf_ue_id_to_uint(ue->amf_ue_id));

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_inactive_contexts, 1U);
  EXPECT_EQ(runtime.nof_ntn_inactive_suspend_requested, 1U);
  EXPECT_EQ(runtime.nof_ntn_inactive_suspend_succeeded, 1U);
  EXPECT_EQ(runtime.nof_ntn_idle_paging_contexts, 1U);

  auto* cu_cp_impl = get_cu_cp_impl(env);
  ASSERT_NE(cu_cp_impl, nullptr);
  cu_cp_impl->handle_ue_context_suspend_outcome(ue->ue_index, true);
  const cu_cp_ntn_runtime_status response_runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(response_runtime.nof_ntn_inactive_ngap_suspend_responses, 1U);
  EXPECT_EQ(response_runtime.nof_ntn_inactive_ngap_suspend_failures, 0U);
}

TEST(cu_cp_ntn_mobility_test, rrc_resume_request_for_known_inactive_ntn_ue_triggers_ngap_resume)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_idle_paging_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  const cu_cp_five_g_s_tmsi five_g_s_tmsi = make_test_paging_five_g_s_tmsi();
  const std::optional<service_bound_ntn_ue_context> ue =
      setup_service_bound_ntn_ue(env, ntn_handler, true, std::chrono::steady_clock::now(), five_g_s_tmsi);
  ASSERT_TRUE(ue.has_value());

  env.get_cu_up(ue->cu_up_idx)
      .push_tx_pdu(generate_bearer_context_inactivity_notification_with_ue_level(ue->cu_cp_e1ap_id,
                                                                                 ue->cu_up_e1ap_id));

  f1ap_message f1ap_pdu;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu(ue->du_idx, f1ap_pdu, std::chrono::milliseconds{1000}));
  ASSERT_TRUE(test_helpers::is_valid_ue_context_release_command(f1ap_pdu));
  const auto& release_cmd = f1ap_pdu.pdu.init_msg().value.ue_context_release_cmd();
  env.get_du(ue->du_idx)
      .push_ul_pdu(test_helpers::generate_ue_context_release_complete(
          int_to_gnb_cu_ue_f1ap_id(release_cmd->gnb_cu_ue_f1ap_id),
          int_to_gnb_du_ue_f1ap_id(release_cmd->gnb_du_ue_f1ap_id)));

  ngap_message ngap_pdu;
  ASSERT_TRUE(env.wait_for_ngap_tx_pdu(ngap_pdu, std::chrono::milliseconds{1000}));
  ASSERT_EQ(ngap_pdu.pdu.init_msg().value.type(),
            asn1::ngap::ngap_elem_procs_o::init_msg_c::types_opts::ue_context_suspend_request);

  auto* cu_cp_impl = get_cu_cp_impl(env);
  ASSERT_NE(cu_cp_impl, nullptr);
  cu_cp_impl->handle_rrc_resume_request(ue->ue_index, ue->ue_index, establishment_cause_t::mo_sig);

  ASSERT_TRUE(env.wait_for_ngap_tx_pdu(ngap_pdu, std::chrono::milliseconds{1000}));
  ASSERT_EQ(ngap_pdu.pdu.init_msg().value.type(),
            asn1::ngap::ngap_elem_procs_o::init_msg_c::types_opts::ue_context_resume_request);
  const auto& resume = ngap_pdu.pdu.init_msg().value.ue_context_resume_request();
  EXPECT_EQ(resume->amf_ue_ngap_id, amf_ue_id_to_uint(ue->amf_ue_id));
  EXPECT_EQ(resume->rrc_resume_cause.value, asn1::ngap::rrc_establishment_cause_opts::mo_sig);

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_inactive_resume_requested, 1U);
  EXPECT_EQ(runtime.nof_ntn_inactive_resume_succeeded, 1U);
  EXPECT_EQ(runtime.nof_ntn_inactive_resume_failed, 0U);

  cu_cp_impl->handle_ue_context_resume_outcome(ue->ue_index, true);
  const cu_cp_ntn_runtime_status response_runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(response_runtime.nof_ntn_inactive_contexts, 0U);
  EXPECT_EQ(response_runtime.nof_ntn_inactive_ngap_resume_responses, 1U);
  EXPECT_EQ(response_runtime.nof_ntn_inactive_ngap_resume_failures, 0U);
}

TEST(cu_cp_ntn_mobility_test, ngap_resume_failure_updates_ntn_inactive_state)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_idle_paging_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  const cu_cp_five_g_s_tmsi five_g_s_tmsi = make_test_paging_five_g_s_tmsi();
  const std::optional<service_bound_ntn_ue_context> ue =
      setup_service_bound_ntn_ue(env, ntn_handler, true, std::chrono::steady_clock::now(), five_g_s_tmsi);
  ASSERT_TRUE(ue.has_value());

  env.get_cu_up(ue->cu_up_idx)
      .push_tx_pdu(generate_bearer_context_inactivity_notification_with_ue_level(ue->cu_cp_e1ap_id,
                                                                                 ue->cu_up_e1ap_id));

  f1ap_message f1ap_pdu;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu(ue->du_idx, f1ap_pdu, std::chrono::milliseconds{1000}));
  ASSERT_TRUE(test_helpers::is_valid_ue_context_release_command(f1ap_pdu));
  const auto& release_cmd = f1ap_pdu.pdu.init_msg().value.ue_context_release_cmd();
  env.get_du(ue->du_idx)
      .push_ul_pdu(test_helpers::generate_ue_context_release_complete(
          int_to_gnb_cu_ue_f1ap_id(release_cmd->gnb_cu_ue_f1ap_id),
          int_to_gnb_du_ue_f1ap_id(release_cmd->gnb_du_ue_f1ap_id)));

  ngap_message ngap_pdu;
  ASSERT_TRUE(env.wait_for_ngap_tx_pdu(ngap_pdu, std::chrono::milliseconds{1000}));
  ASSERT_EQ(ngap_pdu.pdu.init_msg().value.type(),
            asn1::ngap::ngap_elem_procs_o::init_msg_c::types_opts::ue_context_suspend_request);

  auto* cu_cp_impl = get_cu_cp_impl(env);
  ASSERT_NE(cu_cp_impl, nullptr);
  cu_cp_impl->handle_rrc_resume_request(ue->ue_index, ue->ue_index, establishment_cause_t::mo_sig);
  ASSERT_TRUE(env.wait_for_ngap_tx_pdu(ngap_pdu, std::chrono::milliseconds{1000}));
  ASSERT_EQ(ngap_pdu.pdu.init_msg().value.type(),
            asn1::ngap::ngap_elem_procs_o::init_msg_c::types_opts::ue_context_resume_request);

  cu_cp_impl->handle_ue_context_resume_outcome(ue->ue_index, false);
  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_inactive_ngap_resume_responses, 0U);
  EXPECT_EQ(runtime.nof_ntn_inactive_ngap_resume_failures, 1U);
  EXPECT_EQ(runtime.nof_ntn_inactive_resume_failed, 1U);
  EXPECT_EQ(runtime.nof_ntn_inactive_fallback_releases, 1U);
  EXPECT_STREQ(runtime.last_ntn_inactive_reason.c_str(), "ngap_resume_failure");
}

TEST(cu_cp_ntn_mobility_test, ngap_paging_for_known_idle_ntn_ue_is_narrowed_to_last_pageable_beam)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_idle_paging_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  const cu_cp_five_g_s_tmsi five_g_s_tmsi = make_test_paging_five_g_s_tmsi();
  const std::optional<service_bound_ntn_ue_context> ue =
      setup_service_bound_ntn_ue(env, ntn_handler, true, std::chrono::steady_clock::now(), five_g_s_tmsi);
  ASSERT_TRUE(ue.has_value());
  complete_service_bound_ue_release(env, *ue);

  env.get_amf().push_tx_pdu(generate_valid_minimal_paging_message());

  f1ap_message paging;
  ASSERT_TRUE(wait_for_f1ap_paging(env, ue->du_idx, paging));
  const std::array<nr_cell_identity, 1> expected_ncis = {make_default_env_nci(0)};
  expect_paging_cells(paging, expected_ncis);

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_idle_paging_ue_hits, 1U);
  EXPECT_EQ(runtime.nof_ntn_idle_paging_tac_fallbacks, 0U);
  EXPECT_EQ(runtime.nof_ntn_idle_paging_recommendations, 1U);
}

TEST(cu_cp_ntn_mobility_test, paging_without_ue_context_uses_beam_derived_tac_fallback)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_idle_paging_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();
  const std::optional<unsigned> du_idx = connect_du_for_ntn_beam_cells(env, {0, 1});
  ASSERT_TRUE(du_idx.has_value());

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));
  env.drain_f1ap_resource_coordination_requests(du_idx.value());

  env.get_amf().push_tx_pdu(generate_valid_minimal_paging_message());

  f1ap_message paging;
  ASSERT_TRUE(wait_for_f1ap_paging(env, du_idx.value(), paging));
  const std::array<nr_cell_identity, 1> expected_ncis = {make_default_env_nci(0)};
  expect_paging_cells(paging, expected_ncis);

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_idle_paging_ue_hits, 0U);
  EXPECT_EQ(runtime.nof_ntn_idle_paging_tac_fallbacks, 1U);
  EXPECT_EQ(runtime.nof_ntn_idle_paging_recommendations, 1U);
}

TEST(cu_cp_ntn_mobility_test, paging_prefers_beam_with_downlink_and_uplink_access_ready)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_idle_paging_ntn_mobility_config();
  params.ntn_location_mobility->beams[0].uplink_enabled = false;
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();
  const std::optional<unsigned> du_idx = connect_du_for_ntn_beam_cells(env, {0, 1});
  ASSERT_TRUE(du_idx.has_value());

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));
  env.drain_f1ap_resource_coordination_requests(du_idx.value());

  const cu_cp_ntn_beam_status downlink_only =
      find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0007");
  EXPECT_TRUE(downlink_only.downlink_visible);
  EXPECT_FALSE(downlink_only.uplink_access_ready);
  EXPECT_FALSE(downlink_only.access_roundtrip_ready);
  EXPECT_TRUE(downlink_only.paired_uplink_access_ready);
  EXPECT_EQ(downlink_only.paired_uplink_beam_id, "CN-BEAM-ALT-0007");
  ASSERT_TRUE(downlink_only.paired_uplink_nci.has_value());
  EXPECT_EQ(downlink_only.paired_uplink_nci.value(), make_default_env_nci(1));
  EXPECT_EQ(downlink_only.paired_uplink_du_index, uint_to_du_index(du_idx.value()));
  EXPECT_EQ(downlink_only.access_pair_reason, "paired_same_analog_tac");
  EXPECT_TRUE(downlink_only.paging_recommendable);
  EXPECT_EQ(downlink_only.paging_recommendation_reason, "eligible");

  env.get_amf().push_tx_pdu(generate_valid_minimal_paging_message());

  f1ap_message paging;
  ASSERT_TRUE(wait_for_f1ap_paging(env, du_idx.value(), paging));
  const std::array<nr_cell_identity, 1> expected_ncis = {make_default_env_nci(1)};
  expect_paging_cells(paging, expected_ncis);

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_downlink_visible_beams, 2U);
  EXPECT_EQ(runtime.nof_uplink_access_ready_beams, 1U);
  EXPECT_EQ(runtime.nof_access_roundtrip_ready_beams, 1U);
  EXPECT_EQ(runtime.nof_paired_uplink_access_ready_beams, 1U);
  EXPECT_EQ(runtime.nof_downlink_only_without_ul_pair_beams, 0U);
  EXPECT_EQ(runtime.nof_paging_recommendable_beams, 2U);
  EXPECT_EQ(runtime.nof_ntn_idle_paging_tac_fallbacks, 1U);
  EXPECT_EQ(runtime.nof_ntn_idle_paging_recommendations, 1U);
}

TEST(cu_cp_ntn_mobility_test, downlink_only_beam_without_matching_uplink_sibling_is_not_paired_access_ready)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_idle_paging_ntn_mobility_config();
  params.ntn_location_mobility->beams[0].uplink_enabled = false;
  params.ntn_location_mobility->beams[1].beam_id = "CN-BEAM-ALT-0008";
  params.ntn_location_mobility->analog_beams[0].child_digital_beam_ids = {"CN-BEAM-0007", "CN-BEAM-ALT-0008"};
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();
  const std::optional<unsigned> du_idx = connect_du_for_ntn_beam_cells(env, {0, 1});
  ASSERT_TRUE(du_idx.has_value());

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));
  env.drain_f1ap_resource_coordination_requests(du_idx.value());

  const cu_cp_ntn_beam_status downlink_only =
      find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0007");
  EXPECT_TRUE(downlink_only.downlink_visible);
  EXPECT_FALSE(downlink_only.uplink_access_ready);
  EXPECT_FALSE(downlink_only.access_roundtrip_ready);
  EXPECT_FALSE(downlink_only.paired_uplink_access_ready);
  EXPECT_EQ(downlink_only.access_pair_reason, "no_same_analog_uplink_beam");
  EXPECT_FALSE(downlink_only.paging_recommendable);
  EXPECT_EQ(downlink_only.paging_recommendation_reason, "uplink_response_unavailable");

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_paired_uplink_access_ready_beams, 0U);
  EXPECT_EQ(runtime.nof_downlink_only_without_ul_pair_beams, 1U);
}

TEST(cu_cp_ntn_mobility_test,
     service_binding_after_paired_access_uses_bidirectional_sibling_not_downlink_only_wake_beam)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_paired_access_ntn_mobility_config_with_service_sibling();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();
  const std::optional<unsigned> du_idx = connect_du_for_ntn_beam_cells(env, {0, 1, 2});
  ASSERT_TRUE(du_idx.has_value());
  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));
  env.drain_f1ap_resource_coordination_requests(du_idx.value());

  const cu_cp_ntn_beam_status downlink_only =
      find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0007");
  ASSERT_TRUE(downlink_only.paired_uplink_access_ready);
  EXPECT_EQ(downlink_only.paired_uplink_beam_id, "CN-BEAM-ACC-0007");

  env.get_amf().push_tx_pdu(make_paging_message_with_recommended_cell(make_default_env_nci(0)));

  f1ap_message paging;
  ASSERT_TRUE(wait_for_f1ap_paging(env, du_idx.value(), paging));
  const std::array<nr_cell_identity, 1> paging_ncis = {make_default_env_nci(0)};
  expect_paging_cells(paging, paging_ncis);

  const cu_cp_five_g_s_tmsi five_g_s_tmsi = make_test_paging_five_g_s_tmsi();
  const gnb_du_ue_f1ap_id_t du_ue_id     = int_to_gnb_du_ue_f1ap_id(10);
  const rnti_t              crnti        = to_rnti(0x4601);
  const ue_index_t          ue_index     = finish_ntn_access_only_registration(env,
                                                                      du_idx.value(),
                                                                      cu_up_idx.value(),
                                                                      du_ue_id,
                                                                      crnti,
                                                                      uint_to_amf_ue_id(10),
                                                                      make_supported_ntn_ngso_ue_capability_info_pdu(),
                                                                      five_g_s_tmsi,
                                                                      make_default_env_nci(1));
  ASSERT_NE(ue_index, ue_index_t::invalid);

  ASSERT_TRUE(env.request_pdu_session_resource_setup(du_idx.value(), cu_up_idx.value(), du_ue_id));
  ASSERT_TRUE(setup_pdu_session_with_five_qi(env,
                                             du_idx.value(),
                                             cu_up_idx.value(),
                                             du_ue_id,
                                             crnti,
                                             int_to_gnb_cu_up_ue_e1ap_id(10),
                                             pdu_session_id_t::min,
                                             drb_id_t::drb1,
                                             uint_to_qos_flow_id(0),
                                             9,
                                             true));

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());
  expect_and_ack_ntn_slot_update(env, du_idx.value(), du_ue_id, ue_ctx->cu_ue_id.value(), crnti);
  env.drain_f1ap_resource_coordination_requests(du_idx.value());

  const cu_cp_ntn_ue_status ue_status = find_ue_status(ntn_handler.get_current_ntn_ue_status(), ue_index);
  EXPECT_EQ(ue_status.access_layer_reason, "paired_uplink_response");
  ASSERT_TRUE(ue_status.service_digital_beam_id.has_value());
  EXPECT_EQ(ue_status.service_digital_beam_id.value(), "CN-BEAM-SVC-0007");
  EXPECT_FALSE(ue_status.service_uplink_resource_beam_id.has_value());
  EXPECT_EQ(ue_status.service_pair_reason, "none");
  EXPECT_EQ(ue_status.service_binding_source, "paired_access_fallback");
  EXPECT_EQ(ue_status.service_layer_state, "service_bound");
}

TEST(cu_cp_ntn_mobility_test, service_binding_uses_downlink_service_and_paired_uplink_resource_beam)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_idle_paging_ntn_mobility_config();
  params.ntn_location_mobility->beams[0].uplink_enabled   = false;
  params.ntn_location_mobility->beams[1].downlink_enabled = false;
  params.ntn_location_mobility->beams[1].beam_id          = "CN-BEAM-ACC-0007";
  params.ntn_location_mobility->analog_beams[0].child_digital_beam_ids = {"CN-BEAM-0007", "CN-BEAM-ACC-0007"};
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();
  const std::optional<unsigned> du_idx = connect_du_for_ntn_beam_cells(env, {0, 1});
  ASSERT_TRUE(du_idx.has_value());
  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));
  env.drain_f1ap_resource_coordination_requests(du_idx.value());

  env.get_amf().push_tx_pdu(make_paging_message_with_recommended_cell(make_default_env_nci(0)));
  f1ap_message paging;
  ASSERT_TRUE(wait_for_f1ap_paging(env, du_idx.value(), paging));
  const std::array<nr_cell_identity, 1> paging_ncis = {make_default_env_nci(0)};
  expect_paging_cells(paging, paging_ncis);

  const cu_cp_five_g_s_tmsi five_g_s_tmsi = make_test_paging_five_g_s_tmsi();
  const gnb_du_ue_f1ap_id_t du_ue_id     = int_to_gnb_du_ue_f1ap_id(11);
  const rnti_t              crnti        = to_rnti(0x4601);
  const ue_index_t          ue_index     = finish_ntn_access_only_registration(env,
                                                                      du_idx.value(),
                                                                      cu_up_idx.value(),
                                                                      du_ue_id,
                                                                      crnti,
                                                                      uint_to_amf_ue_id(11),
                                                                      make_supported_ntn_ngso_ue_capability_info_pdu(),
                                                                      five_g_s_tmsi,
                                                                      make_default_env_nci(1));
  ASSERT_NE(ue_index, ue_index_t::invalid);

  ASSERT_TRUE(env.request_pdu_session_resource_setup(du_idx.value(), cu_up_idx.value(), du_ue_id));
  ASSERT_TRUE(setup_pdu_session_with_five_qi(env,
                                             du_idx.value(),
                                             cu_up_idx.value(),
                                             du_ue_id,
                                             crnti,
                                             int_to_gnb_cu_up_ue_e1ap_id(11),
                                             pdu_session_id_t::min,
                                             drb_id_t::drb1,
                                             uint_to_qos_flow_id(0),
                                             9,
                                             true));

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());

  f1ap_ntn_ul_slot_resource_request decoded_slot_request;
  expect_and_ack_ntn_slot_update(
      env, du_idx.value(), du_ue_id, ue_ctx->cu_ue_id.value(), crnti, &decoded_slot_request);
  env.drain_f1ap_resource_coordination_requests(du_idx.value());

  const cu_cp_ntn_beam_status uplink_resource_beam =
      find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-ACC-0007");
  ASSERT_TRUE(decoded_slot_request.sr_slot_offset.has_value());
  ASSERT_TRUE(decoded_slot_request.srs_slot_offset.has_value());
  EXPECT_EQ(*decoded_slot_request.sr_slot_offset, uplink_resource_beam.sr_slot_offset);
  EXPECT_EQ(*decoded_slot_request.srs_slot_offset, uplink_resource_beam.srs_slot_offset);

  const cu_cp_ntn_ue_status ue_status = find_ue_status(ntn_handler.get_current_ntn_ue_status(), ue_index);
  EXPECT_EQ(ue_status.access_layer_reason, "paired_uplink_response");
  EXPECT_EQ(ue_status.service_layer_state, "service_bound");
  ASSERT_TRUE(ue_status.service_digital_beam_id.has_value());
  EXPECT_EQ(ue_status.service_digital_beam_id.value(), "CN-BEAM-0007");
  ASSERT_TRUE(ue_status.service_uplink_resource_beam_id.has_value());
  EXPECT_EQ(ue_status.service_uplink_resource_beam_id.value(), "CN-BEAM-ACC-0007");
  ASSERT_TRUE(ue_status.service_uplink_resource_nci.has_value());
  EXPECT_EQ(ue_status.service_uplink_resource_nci.value(), make_default_env_nci(1));
  EXPECT_EQ(ue_status.service_binding_source, "paired_access_service_pair");
  EXPECT_EQ(ue_status.service_pair_reason, "same_analog_tac_uplink_resource");
  EXPECT_EQ(ue_status.service_layer_reason, "service_pair_bound");

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_service_pair_bound_ues, 1U);
  EXPECT_EQ(runtime.nof_ntn_service_pair_blocked_ues, 0U);

  const ntn_beam_service_resource_snapshot resource_snapshot =
      ntn_handler.get_current_ntn_beam_service_resource_snapshot();
  EXPECT_EQ(resource_snapshot.nof_service_pair_digital_slot_intents, 1U);
  ASSERT_EQ(resource_snapshot.digital_slot_intents.size(), 1U);
  EXPECT_EQ(resource_snapshot.digital_slot_intents.front().uplink_resource_beam_id, "CN-BEAM-ACC-0007");
  EXPECT_EQ(resource_snapshot.digital_slot_intents.front().uplink_resource_du_index, srs_cu_cp::uint_to_du_index(0));
  EXPECT_EQ(resource_snapshot.digital_slot_intents.front().uplink_resource_cell_index, srsran::to_du_cell_index(1));
  EXPECT_EQ(resource_snapshot.digital_slot_intents.front().uplink_resource_pci, 2);

  ntn_repair_command repair_command;
  repair_command.mode  = ntn_repair_mode::apply;
  repair_command.scope = ntn_repair_scope::resources;
  const cu_cp_ntn_runtime_status before_repair = ntn_handler.get_current_ntn_runtime_status();
  const ntn_repair_response      repair_response = ntn_handler.handle_ntn_repair_command(repair_command);
  const cu_cp_ntn_runtime_status after_repair = ntn_handler.get_current_ntn_runtime_status();
  env.drain_f1ap_resource_coordination_requests(du_idx.value());

  ASSERT_TRUE(repair_response.accepted);
  EXPECT_GT(repair_response.audit_targets, 0U);
  EXPECT_GT(after_repair.nof_ntn_service_pair_resource_audit_targets,
            before_repair.nof_ntn_service_pair_resource_audit_targets);
  EXPECT_EQ(after_repair.last_ntn_service_pair_resource_audit_reason, "audit_requested");
}

TEST(cu_cp_ntn_mobility_test, known_idle_ue_paging_skips_draining_last_beam_and_uses_same_analog_fallback)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_idle_paging_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  const cu_cp_five_g_s_tmsi five_g_s_tmsi = make_test_paging_five_g_s_tmsi();
  const std::optional<service_bound_ntn_ue_context> ue =
      setup_service_bound_ntn_ue(env, ntn_handler, true, std::chrono::steady_clock::now(), five_g_s_tmsi);
  ASSERT_TRUE(ue.has_value());
  complete_service_bound_ue_release(env, *ue);

  ntn_service_switch_over_event event;
  event.event_id          = 4801;
  event.type              = ntn_service_switch_over_type::hard;
  event.source            = ntn_service_switch_over_source::operator_command;
  event.policy            = ntn_service_switch_over_policy::drain;
  event.affected_beam_ids = {"CN-BEAM-0007"};
  ASSERT_TRUE(ntn_handler.handle_ntn_service_switch_over_event(event));
  env.drain_f1ap_resource_coordination_requests(ue->du_idx);

  env.get_amf().push_tx_pdu(generate_valid_minimal_paging_message());

  f1ap_message paging;
  ASSERT_TRUE(wait_for_f1ap_paging(env, ue->du_idx, paging));
  const std::array<nr_cell_identity, 1> expected_ncis = {make_default_env_nci(1)};
  expect_paging_cells(paging, expected_ncis);

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_idle_paging_ue_hits, 1U);
  EXPECT_EQ(runtime.nof_ntn_idle_paging_tac_fallbacks, 0U);
  EXPECT_EQ(runtime.nof_ntn_idle_paging_recommendations, 1U);
}

TEST(cu_cp_ntn_mobility_test, amf_recommended_cell_to_unpageable_ntn_beam_gets_ntn_fallback)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_idle_paging_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();
  const std::optional<unsigned> du_idx = connect_du_for_ntn_beam_cells(env, {0, 1});
  ASSERT_TRUE(du_idx.has_value());

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));
  env.drain_f1ap_resource_coordination_requests(du_idx.value());

  ntn_service_switch_over_event event;
  event.event_id          = 4802;
  event.type              = ntn_service_switch_over_type::hard;
  event.source            = ntn_service_switch_over_source::operator_command;
  event.policy            = ntn_service_switch_over_policy::drain;
  event.affected_beam_ids = {"CN-BEAM-ALT-0007"};
  ASSERT_TRUE(ntn_handler.handle_ntn_service_switch_over_event(event));
  env.drain_f1ap_resource_coordination_requests(du_idx.value());

  env.get_amf().push_tx_pdu(make_paging_message_with_recommended_cell(make_default_env_nci(1)));

  f1ap_message paging;
  ASSERT_TRUE(wait_for_f1ap_paging(env, du_idx.value(), paging));
  const std::array<nr_cell_identity, 1> expected_ncis = {make_default_env_nci(0)};
  expect_paging_cells(paging, expected_ncis);

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_idle_paging_ue_hits, 0U);
  EXPECT_EQ(runtime.nof_ntn_idle_paging_tac_fallbacks, 1U);
  EXPECT_EQ(runtime.nof_ntn_idle_paging_recommendations, 1U);
}

TEST(cu_cp_ntn_mobility_test, profile_blocked_ntn_ue_does_not_create_ue_specific_paging_context)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_idle_paging_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));
  env.run_ng_setup();
  const std::optional<unsigned> du_idx = connect_du_for_ntn_beam_cells(env, {0, 1});
  ASSERT_TRUE(du_idx.has_value());

  const std::optional<unsigned> cu_up_idx = env.connect_new_cu_up();
  ASSERT_TRUE(cu_up_idx.has_value());
  ASSERT_TRUE(env.run_e1_setup(cu_up_idx.value()));

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));
  env.drain_f1ap_resource_coordination_requests(du_idx.value());

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  const ue_index_t          ue_index = finish_ntn_access_only_registration(env,
                                                                  du_idx.value(),
                                                                  cu_up_idx.value(),
                                                                  du_ue_id,
                                                                  to_rnti(0x4601),
                                                                  amf_ue_id_t::min,
                                                                  make_supported_ntn_gso_ue_capability_info_pdu(),
                                                                  make_test_paging_five_g_s_tmsi());
  ASSERT_NE(ue_index, ue_index_t::invalid);
  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->amf_ue_id.has_value());

  env.get_amf().push_tx_pdu(generate_valid_ue_context_release_command_with_amf_ue_ngap_id(ue_ctx->amf_ue_id.value()));

  f1ap_message f1ap_pdu;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu(du_idx.value(), f1ap_pdu, std::chrono::milliseconds{1000}));
  ASSERT_TRUE(test_helpers::is_valid_ue_context_release_command(f1ap_pdu));
  const auto& rel_cmd = f1ap_pdu.pdu.init_msg().value.ue_context_release_cmd();
  env.get_du(du_idx.value())
      .push_ul_pdu(test_helpers::generate_ue_context_release_complete(
          int_to_gnb_cu_ue_f1ap_id(rel_cmd->gnb_cu_ue_f1ap_id),
          int_to_gnb_du_ue_f1ap_id(rel_cmd->gnb_du_ue_f1ap_id)));

  ngap_message ngap_pdu;
  ASSERT_TRUE(env.wait_for_ngap_tx_pdu(ngap_pdu, std::chrono::milliseconds{1000}));
  ASSERT_TRUE(test_helpers::is_valid_ue_context_release_complete(ngap_pdu));
  ASSERT_TRUE(env.tick_until(std::chrono::milliseconds{1000}, [&]() {
    return env.get_cu_cp().get_metrics_handler().request_metrics_report().ues.empty();
  }));
  env.drain_f1ap_resource_coordination_requests(du_idx.value());

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_ntn_idle_paging_contexts, 0U);
  EXPECT_EQ(runtime.nof_ntn_idle_paging_contexts_with_5g_s_tmsi, 0U);
}

TEST(cu_cp_ntn_mobility_test, stale_ntn_assistance_skips_release_paging_recommendation)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_stale_assistance_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));

  env.run_ng_setup();
  const std::optional<connected_ngap_ntn_ue> connected_ue = connect_ngap_ntn_ue(env);
  ASSERT_TRUE(connected_ue.has_value());

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  auto* cu_cp_impl = get_cu_cp_impl(env);
  ASSERT_NE(cu_cp_impl, nullptr);
  cu_cp_impl->get_cu_cp_measurement_handler().handle_ue_location_report(
      make_ntn_location_report(connected_ue->ue_index, make_default_env_nci(0)));

  expire_ntn_assistance();
  const cu_cp_ntn_beam_status stale_beam =
      find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0001");
  ASSERT_TRUE(stale_beam.derived_tac.has_value());
  EXPECT_EQ(stale_beam.derived_tac.value(), 1U);
  EXPECT_FALSE(stale_beam.paging_recommendable);
  EXPECT_EQ(stale_beam.paging_recommendation_reason, "stale_assistance");

  env.get_amf().push_tx_pdu(generate_valid_ue_context_release_command_with_amf_ue_ngap_id(connected_ue->amf_ue_id));

  f1ap_message f1ap_pdu;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu(connected_ue->du_idx, f1ap_pdu));
  ASSERT_TRUE(test_helpers::is_valid_ue_context_release_command(f1ap_pdu));
  const auto& rel_cmd = f1ap_pdu.pdu.init_msg().value.ue_context_release_cmd();
  env.get_du(connected_ue->du_idx)
      .push_ul_pdu(test_helpers::generate_ue_context_release_complete(
          int_to_gnb_cu_ue_f1ap_id(rel_cmd->gnb_cu_ue_f1ap_id),
          int_to_gnb_du_ue_f1ap_id(rel_cmd->gnb_du_ue_f1ap_id)));

  ngap_message ngap_pdu;
  ASSERT_TRUE(env.wait_for_ngap_tx_pdu(ngap_pdu));
  ASSERT_TRUE(test_helpers::is_valid_ue_context_release_complete(ngap_pdu));

  const auto& release_complete = ngap_pdu.pdu.successful_outcome().value.ue_context_release_complete();
  EXPECT_FALSE(release_complete->info_on_recommended_cells_and_ran_nodes_for_paging_present);
  env.drain_f1ap_resource_coordination_requests(connected_ue->du_idx);
}

TEST(cu_cp_ntn_mobility_test, invalid_beam_derived_tac_blocks_release_paging_recommendation)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  params.ntn_location_mobility->beams[0].beam_id = "CN-BEAM-A";
  cu_cp_test_environment env(std::move(params));

  env.run_ng_setup();
  const std::optional<connected_ngap_ntn_ue> connected_ue = connect_ngap_ntn_ue(env);
  ASSERT_TRUE(connected_ue.has_value());

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  const cu_cp_ntn_beam_status beam_status = find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-A");
  EXPECT_FALSE(beam_status.derived_tac.has_value());
  EXPECT_EQ(beam_status.derived_tac_reason, "missing_decimal_suffix");
  EXPECT_FALSE(beam_status.paging_recommendable);
  EXPECT_EQ(beam_status.paging_recommendation_reason, "invalid_tac");

  const cu_cp_ntn_runtime_status runtime = ntn_handler.get_current_ntn_runtime_status();
  EXPECT_EQ(runtime.nof_valid_service_area_beams, 1U);
  EXPECT_EQ(runtime.nof_invalid_service_area_beams, 1U);
  EXPECT_EQ(runtime.nof_paging_recommendable_beams, 0U);

  auto* cu_cp_impl = get_cu_cp_impl(env);
  ASSERT_NE(cu_cp_impl, nullptr);
  cu_cp_impl->get_cu_cp_measurement_handler().handle_ue_location_report(
      make_ntn_location_report(connected_ue->ue_index, make_default_env_nci(0)));

  env.get_amf().push_tx_pdu(generate_valid_ue_context_release_command_with_amf_ue_ngap_id(connected_ue->amf_ue_id));

  f1ap_message f1ap_pdu;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu(connected_ue->du_idx, f1ap_pdu));
  ASSERT_TRUE(test_helpers::is_valid_ue_context_release_command(f1ap_pdu));
  const auto& rel_cmd = f1ap_pdu.pdu.init_msg().value.ue_context_release_cmd();
  env.get_du(connected_ue->du_idx)
      .push_ul_pdu(test_helpers::generate_ue_context_release_complete(
          int_to_gnb_cu_ue_f1ap_id(rel_cmd->gnb_cu_ue_f1ap_id),
          int_to_gnb_du_ue_f1ap_id(rel_cmd->gnb_du_ue_f1ap_id)));

  ngap_message ngap_pdu;
  ASSERT_TRUE(env.wait_for_ngap_tx_pdu(ngap_pdu));
  ASSERT_TRUE(test_helpers::is_valid_ue_context_release_complete(ngap_pdu));

  const auto& release_complete = ngap_pdu.pdu.successful_outcome().value.ue_context_release_complete();
  EXPECT_FALSE(release_complete->info_on_recommended_cells_and_ran_nodes_for_paging_present);
  ASSERT_TRUE(release_complete->user_location_info_present);
  env.drain_f1ap_resource_coordination_requests(connected_ue->du_idx);
}

TEST(cu_cp_ntn_mobility_test, draining_ntn_beam_skips_release_paging_recommendation)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));

  env.run_ng_setup();
  const std::optional<connected_ngap_ntn_ue> connected_ue = connect_ngap_ntn_ue(env);
  ASSERT_TRUE(connected_ue.has_value());

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  auto* cu_cp_impl = get_cu_cp_impl(env);
  ASSERT_NE(cu_cp_impl, nullptr);
  cu_cp_impl->get_cu_cp_measurement_handler().handle_ue_location_report(
      make_ntn_location_report(connected_ue->ue_index, make_default_env_nci(0)));

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 1.0, 500000.0)));
  const cu_cp_ntn_beam_status draining_beam =
      find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0001");
  ASSERT_EQ(draining_beam.state, cu_cp_ntn_beam_assignment_state::draining);
  ASSERT_TRUE(draining_beam.derived_tac.has_value());
  EXPECT_EQ(draining_beam.derived_tac.value(), 1U);
  EXPECT_FALSE(draining_beam.paging_recommendable);
  EXPECT_EQ(draining_beam.paging_recommendation_reason, "draining");

  env.get_amf().push_tx_pdu(generate_valid_ue_context_release_command_with_amf_ue_ngap_id(connected_ue->amf_ue_id));

  f1ap_message f1ap_pdu;
  ASSERT_TRUE(env.wait_for_f1ap_tx_pdu(connected_ue->du_idx, f1ap_pdu));
  ASSERT_TRUE(test_helpers::is_valid_ue_context_release_command(f1ap_pdu));
  const auto& rel_cmd = f1ap_pdu.pdu.init_msg().value.ue_context_release_cmd();
  env.get_du(connected_ue->du_idx)
      .push_ul_pdu(test_helpers::generate_ue_context_release_complete(
          int_to_gnb_cu_ue_f1ap_id(rel_cmd->gnb_cu_ue_f1ap_id),
          int_to_gnb_du_ue_f1ap_id(rel_cmd->gnb_du_ue_f1ap_id)));

  ngap_message ngap_pdu;
  ASSERT_TRUE(env.wait_for_ngap_tx_pdu(ngap_pdu));
  ASSERT_TRUE(test_helpers::is_valid_ue_context_release_complete(ngap_pdu));

  const auto& release_complete = ngap_pdu.pdu.successful_outcome().value.ue_context_release_complete();
  EXPECT_FALSE(release_complete->info_on_recommended_cells_and_ran_nodes_for_paging_present);
  env.drain_f1ap_resource_coordination_requests(connected_ue->du_idx);
}

TEST(cu_cp_ntn_mobility_test, accepted_ntn_location_report_is_not_forwarded_when_serving_beam_is_not_active)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  params.ntn_location_mobility->core_network_reporting.local_forwarding_enabled = true;
  cu_cp_test_environment env(std::move(params));

  env.run_ng_setup();
  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  ASSERT_TRUE(du_idx.has_value());
  ASSERT_TRUE(connect_cu_up_for_ue_admission(env));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  ASSERT_TRUE(env.connect_new_ue(du_idx.value(), du_ue_id, to_rnti(0x4601)));
  ASSERT_TRUE(env.authenticate_ue(du_idx.value(), du_ue_id, uint_to_amf_ue_id(0)));

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());

  auto* cu_cp_impl = get_cu_cp_impl(env);
  ASSERT_NE(cu_cp_impl, nullptr);

  const ue_index_t ue_index = uint_to_ue_index(gnb_cu_ue_f1ap_id_to_uint(ue_ctx->cu_ue_id.value()));
  cu_cp_impl->get_cu_cp_measurement_handler().handle_ue_location_report(
      make_ntn_location_report(ue_index, make_default_env_nci(0)));

  ngap_message ngap_pdu;
  ASSERT_FALSE(env.wait_for_ngap_tx_pdu(ngap_pdu, std::chrono::milliseconds{20}));

  ngap_location_reporting_control control;
  control.ue_index               = ue_index;
  control.request_type.event_type = ngap_location_reporting_event_type::direct;
  const ngap_location_reporting_control_response response =
      cu_cp_impl->get_cu_cp_ngap_handler().handle_location_reporting_control(control);
  ASSERT_FALSE(response.accepted);
  assert_ngap_radio_cause(response.cause, ngap_cause_radio_network_t::unspecified);
}

TEST(cu_cp_ntn_mobility_test, location_reporting_control_direct_sends_last_accepted_ntn_location)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));

  env.run_ng_setup();
  const std::optional<unsigned> du_idx = connect_du_for_ntn_beams(env);
  ASSERT_TRUE(du_idx.has_value());
  ASSERT_TRUE(env.get_cu_cp()
                  .get_command_handler()
                  .get_ntn_command_handler()
                  .handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));
  ASSERT_TRUE(connect_cu_up_for_ue_admission(env));

  const gnb_du_ue_f1ap_id_t du_ue_id = int_to_gnb_du_ue_f1ap_id(0);
  ASSERT_TRUE(env.connect_new_ue(du_idx.value(), du_ue_id, to_rnti(0x4601)));
  ASSERT_TRUE(env.authenticate_ue(du_idx.value(), du_ue_id, uint_to_amf_ue_id(0)));

  const cu_cp_test_environment::ue_context* ue_ctx = env.find_ue_context(du_idx.value(), du_ue_id);
  ASSERT_NE(ue_ctx, nullptr);
  ASSERT_TRUE(ue_ctx->cu_ue_id.has_value());
  ASSERT_TRUE(ue_ctx->ran_ue_id.has_value());
  ASSERT_TRUE(ue_ctx->amf_ue_id.has_value());

  auto* cu_cp_impl = get_cu_cp_impl(env);
  ASSERT_NE(cu_cp_impl, nullptr);

  const ue_index_t ue_index = uint_to_ue_index(gnb_cu_ue_f1ap_id_to_uint(ue_ctx->cu_ue_id.value()));
  cu_cp_impl->get_cu_cp_measurement_handler().handle_ue_location_report(
      make_ntn_location_report(ue_index, make_default_env_nci(0)));

  ngap_location_reporting_control control;
  control.ue_index               = ue_index;
  control.request_type.event_type = ngap_location_reporting_event_type::direct;

  const ngap_location_reporting_control_response response =
      cu_cp_impl->get_cu_cp_ngap_handler().handle_location_reporting_control(control);
  ASSERT_TRUE(response.accepted);

  ngap_message ngap_pdu;
  ASSERT_TRUE(env.wait_for_ngap_tx_pdu(ngap_pdu));
  ASSERT_TRUE(is_pdu_type(ngap_pdu,
                          asn1::ngap::ngap_elem_procs_o::init_msg_c::types::types_opts::location_report));

  const auto& asn1_report = ngap_pdu.pdu.init_msg().value.location_report();
  ASSERT_EQ(asn1_report->ran_ue_ngap_id, ran_ue_id_to_uint(ue_ctx->ran_ue_id.value()));
  ASSERT_EQ(asn1_report->amf_ue_ngap_id, amf_ue_id_to_uint(ue_ctx->amf_ue_id.value()));
  ASSERT_EQ(asn1_report->location_report_request_type.event_type.value, asn1::ngap::event_type_opts::direct);
}

TEST(cu_cp_ntn_mobility_test, location_reporting_control_direct_sends_last_location_while_serving_beam_is_draining)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));

  env.run_ng_setup();
  const std::optional<connected_ngap_ntn_ue> ue = connect_ngap_ntn_ue(env);
  ASSERT_TRUE(ue.has_value());

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  auto* cu_cp_impl = get_cu_cp_impl(env);
  ASSERT_NE(cu_cp_impl, nullptr);

  cu_cp_impl->get_cu_cp_measurement_handler().handle_ue_location_report(
      make_ntn_location_report(ue->ue_index, make_default_env_nci(0)));

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 1.0, 500000.0)));
  ASSERT_EQ(find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0001").state,
            cu_cp_ntn_beam_assignment_state::draining);
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 1.0, 500000.0)));
  ASSERT_EQ(find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0001").state,
            cu_cp_ntn_beam_assignment_state::draining);

  ngap_location_reporting_control control;
  control.ue_index               = ue->ue_index;
  control.request_type.event_type = ngap_location_reporting_event_type::direct;

  const ngap_location_reporting_control_response response =
      cu_cp_impl->get_cu_cp_ngap_handler().handle_location_reporting_control(control);
  ASSERT_TRUE(response.accepted);

  ngap_message ngap_pdu;
  ASSERT_TRUE(env.wait_for_ngap_tx_pdu(ngap_pdu));
  ASSERT_TRUE(is_pdu_type(ngap_pdu,
                          asn1::ngap::ngap_elem_procs_o::init_msg_c::types::types_opts::location_report));
  ASSERT_EQ(ngap_pdu.pdu.init_msg().value.location_report()->location_report_request_type.event_type.value,
            asn1::ngap::event_type_opts::direct);
}

TEST(cu_cp_ntn_mobility_test, location_reporting_control_change_of_serving_cell_reports_only_on_cell_change)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  params.ntn_location_mobility->max_nof_served_beams = 2;
  cu_cp_test_environment env(std::move(params));

  env.run_ng_setup();
  const std::optional<connected_ngap_ntn_ue> ue = connect_ngap_ntn_ue(env);
  ASSERT_TRUE(ue.has_value());
  ASSERT_TRUE(env.get_cu_cp()
                  .get_command_handler()
                  .get_ntn_command_handler()
                  .handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  auto* cu_cp_impl = get_cu_cp_impl(env);
  ASSERT_NE(cu_cp_impl, nullptr);

  const auto base_time = std::chrono::steady_clock::now();
  cu_cp_impl->get_cu_cp_measurement_handler().handle_ue_location_report(
      make_ntn_location_report(ue->ue_index, make_default_env_nci(0), base_time));

  ngap_location_reporting_control control;
  control.ue_index               = ue->ue_index;
  control.request_type.event_type = ngap_location_reporting_event_type::change_of_serving_cell;

  ngap_location_reporting_control_response response =
      cu_cp_impl->get_cu_cp_ngap_handler().handle_location_reporting_control(control);
  ASSERT_TRUE(response.accepted);

  cu_cp_impl->get_cu_cp_measurement_handler().handle_ue_location_report(
      make_ntn_location_report(ue->ue_index, make_default_env_nci(0), base_time + std::chrono::milliseconds{1}));

  ngap_message ngap_pdu;
  ASSERT_FALSE(env.wait_for_ngap_tx_pdu(ngap_pdu, std::chrono::milliseconds{20}));

  ASSERT_TRUE(env.get_cu_cp()
                  .get_command_handler()
                  .get_ntn_command_handler()
                  .handle_ntn_satellite_state_update(make_ecef(0.0, 1.0, 500000.0)));
  cu_cp_impl->get_cu_cp_measurement_handler().handle_ue_location_report(
      make_ntn_location_report(ue->ue_index, make_default_env_nci(1), base_time + std::chrono::milliseconds{2}));

  ASSERT_TRUE(env.wait_for_ngap_tx_pdu(ngap_pdu));
  ASSERT_TRUE(is_pdu_type(ngap_pdu,
                          asn1::ngap::ngap_elem_procs_o::init_msg_c::types::types_opts::location_report));
  ASSERT_EQ(ngap_pdu.pdu.init_msg().value.location_report()->ran_ue_ngap_id, ran_ue_id_to_uint(ue->ran_ue_id));
  ASSERT_EQ(ngap_pdu.pdu.init_msg().value.location_report()->amf_ue_ngap_id, amf_ue_id_to_uint(ue->amf_ue_id));
  ASSERT_EQ(ngap_pdu.pdu.init_msg().value.location_report()->location_report_request_type.event_type.value,
            asn1::ngap::event_type_opts::change_of_serve_cell);

  cu_cp_impl->get_cu_cp_measurement_handler().handle_ue_location_report(
      make_ntn_location_report(ue->ue_index, make_default_env_nci(1), base_time + std::chrono::milliseconds{3}));

  ASSERT_FALSE(env.wait_for_ngap_tx_pdu(ngap_pdu, std::chrono::milliseconds{20}));
}

TEST(cu_cp_ntn_mobility_test, location_reporting_control_change_of_serving_cell_uses_draining_report_as_baseline)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  params.ntn_location_mobility->max_nof_served_beams = 2;
  cu_cp_test_environment env(std::move(params));

  env.run_ng_setup();
  const std::optional<connected_ngap_ntn_ue> ue = connect_ngap_ntn_ue(env);
  ASSERT_TRUE(ue.has_value());

  cu_cp_ntn_command_handler& ntn_handler = env.get_cu_cp().get_command_handler().get_ntn_command_handler();
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  auto* cu_cp_impl = get_cu_cp_impl(env);
  ASSERT_NE(cu_cp_impl, nullptr);

  const auto base_time = std::chrono::steady_clock::now();
  cu_cp_impl->get_cu_cp_measurement_handler().handle_ue_location_report(
      make_ntn_location_report(ue->ue_index, make_default_env_nci(0), base_time));

  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 1.0, 500000.0)));
  ASSERT_EQ(find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0001").state,
            cu_cp_ntn_beam_assignment_state::draining);
  ASSERT_TRUE(ntn_handler.handle_ntn_satellite_state_update(make_ecef(0.0, 1.0, 500000.0)));
  ASSERT_EQ(find_beam_status(ntn_handler.get_current_ntn_beam_status(), "CN-BEAM-0001").state,
            cu_cp_ntn_beam_assignment_state::draining);

  ngap_location_reporting_control control;
  control.ue_index               = ue->ue_index;
  control.request_type.event_type = ngap_location_reporting_event_type::change_of_serving_cell;

  ngap_location_reporting_control_response response =
      cu_cp_impl->get_cu_cp_ngap_handler().handle_location_reporting_control(control);
  ASSERT_TRUE(response.accepted);

  cu_cp_impl->get_cu_cp_measurement_handler().handle_ue_location_report(
      make_ntn_location_report(ue->ue_index, make_default_env_nci(1), base_time + std::chrono::milliseconds{1}));

  ngap_message ngap_pdu;
  ASSERT_TRUE(env.wait_for_ngap_tx_pdu(ngap_pdu));
  ASSERT_TRUE(is_pdu_type(ngap_pdu,
                          asn1::ngap::ngap_elem_procs_o::init_msg_c::types::types_opts::location_report));
  ASSERT_EQ(ngap_pdu.pdu.init_msg().value.location_report()->location_report_request_type.event_type.value,
            asn1::ngap::event_type_opts::change_of_serve_cell);

  cu_cp_impl->get_cu_cp_measurement_handler().handle_ue_location_report(
      make_ntn_location_report(ue->ue_index, make_default_env_nci(1), base_time + std::chrono::milliseconds{2}));
  ASSERT_FALSE(env.wait_for_ngap_tx_pdu(ngap_pdu, std::chrono::milliseconds{20}));
}

TEST(cu_cp_ntn_mobility_test, location_reporting_control_stop_change_of_serving_cell_disables_reports)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));

  env.run_ng_setup();
  const std::optional<connected_ngap_ntn_ue> ue = connect_ngap_ntn_ue(env);
  ASSERT_TRUE(ue.has_value());
  ASSERT_TRUE(env.get_cu_cp()
                  .get_command_handler()
                  .get_ntn_command_handler()
                  .handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  auto* cu_cp_impl = get_cu_cp_impl(env);
  ASSERT_NE(cu_cp_impl, nullptr);

  const auto base_time = std::chrono::steady_clock::now();
  cu_cp_impl->get_cu_cp_measurement_handler().handle_ue_location_report(
      make_ntn_location_report(ue->ue_index, make_default_env_nci(0), base_time));

  ngap_location_reporting_control control;
  control.ue_index               = ue->ue_index;
  control.request_type.event_type = ngap_location_reporting_event_type::change_of_serving_cell;
  ASSERT_TRUE(cu_cp_impl->get_cu_cp_ngap_handler().handle_location_reporting_control(control).accepted);

  control.request_type.event_type = ngap_location_reporting_event_type::stop_change_of_serving_cell;
  ASSERT_TRUE(cu_cp_impl->get_cu_cp_ngap_handler().handle_location_reporting_control(control).accepted);

  ASSERT_TRUE(env.get_cu_cp()
                  .get_command_handler()
                  .get_ntn_command_handler()
                  .handle_ntn_satellite_state_update(make_ecef(0.0, 1.0, 500000.0)));
  cu_cp_impl->get_cu_cp_measurement_handler().handle_ue_location_report(
      make_ntn_location_report(ue->ue_index, make_default_env_nci(1), base_time + std::chrono::milliseconds{1}));

  ngap_message ngap_pdu;
  ASSERT_FALSE(env.wait_for_ngap_tx_pdu(ngap_pdu, std::chrono::milliseconds{20}));
}

TEST(cu_cp_ntn_mobility_test, location_reporting_control_rejects_overlapping_area_of_interest_ref_id)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));

  env.run_ng_setup();
  const std::optional<connected_ngap_ntn_ue> ue = connect_ngap_ntn_ue(env);
  ASSERT_TRUE(ue.has_value());

  auto* cu_cp_impl = get_cu_cp_impl(env);
  ASSERT_NE(cu_cp_impl, nullptr);

  ngap_location_reporting_control control;
  control.ue_index                              = ue->ue_index;
  control.request_type.event_type               = ngap_location_reporting_event_type::ue_presence_in_area_of_interest;
  control.request_type.area_of_interest_ref_ids = {7};
  ASSERT_TRUE(cu_cp_impl->get_cu_cp_ngap_handler().handle_location_reporting_control(control).accepted);

  control.request_type.area_of_interest_ref_ids = {7, 8};
  const ngap_location_reporting_control_response response =
      cu_cp_impl->get_cu_cp_ngap_handler().handle_location_reporting_control(control);
  ASSERT_FALSE(response.accepted);
  assert_ngap_radio_cause(response.cause, ngap_cause_radio_network_t::multiple_location_report_ref_id_instances);
}

TEST(cu_cp_ntn_mobility_test, location_reporting_control_rejects_duplicate_area_ref_ids_in_same_request)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));

  env.run_ng_setup();
  const std::optional<connected_ngap_ntn_ue> ue = connect_ngap_ntn_ue(env);
  ASSERT_TRUE(ue.has_value());

  auto* cu_cp_impl = get_cu_cp_impl(env);
  ASSERT_NE(cu_cp_impl, nullptr);

  ngap_location_reporting_control control;
  control.ue_index                              = ue->ue_index;
  control.request_type.event_type               = ngap_location_reporting_event_type::ue_presence_in_area_of_interest;
  control.request_type.area_of_interest_ref_ids = {9, 9};

  const ngap_location_reporting_control_response response =
      cu_cp_impl->get_cu_cp_ngap_handler().handle_location_reporting_control(control);
  ASSERT_FALSE(response.accepted);
  assert_ngap_radio_cause(response.cause, ngap_cause_radio_network_t::multiple_location_report_ref_id_instances);
}

TEST(cu_cp_ntn_mobility_test, location_reporting_control_cancel_ref_id_preserves_other_area_refs)
{
  cu_cp_test_env_params params;
  params.ntn_location_mobility = make_ntn_mobility_config();
  cu_cp_test_environment env(std::move(params));

  env.run_ng_setup();
  const std::optional<connected_ngap_ntn_ue> ue = connect_ngap_ntn_ue(env);
  ASSERT_TRUE(ue.has_value());
  ASSERT_TRUE(env.get_cu_cp()
                  .get_command_handler()
                  .get_ntn_command_handler()
                  .handle_ntn_satellite_state_update(make_ecef(0.0, 0.0, 500000.0)));

  auto* cu_cp_impl = get_cu_cp_impl(env);
  ASSERT_NE(cu_cp_impl, nullptr);

  ngap_location_reporting_control control;
  control.ue_index                              = ue->ue_index;
  control.request_type.event_type               = ngap_location_reporting_event_type::ue_presence_in_area_of_interest;
  control.request_type.area_of_interest_ref_ids = {7, 8};
  ASSERT_TRUE(cu_cp_impl->get_cu_cp_ngap_handler().handle_location_reporting_control(control).accepted);

  const auto base_time = std::chrono::steady_clock::now();
  cu_cp_impl->get_cu_cp_measurement_handler().handle_ue_location_report(
      make_ntn_location_report(ue->ue_index, make_default_env_nci(0), base_time));

  ngap_message ngap_pdu;
  ASSERT_TRUE(env.wait_for_ngap_tx_pdu(ngap_pdu));
  ASSERT_TRUE(is_pdu_type(ngap_pdu,
                          asn1::ngap::ngap_elem_procs_o::init_msg_c::types::types_opts::location_report));

  const auto& first_report = ngap_pdu.pdu.init_msg().value.location_report();
  ASSERT_EQ(first_report->location_report_request_type.event_type.value,
            asn1::ngap::event_type_opts::ue_presence_in_area_of_interest);
  ASSERT_TRUE(first_report->ue_presence_in_area_of_interest_list_present);
  ASSERT_EQ(first_report->ue_presence_in_area_of_interest_list.size(), 2U);
  ASSERT_EQ(first_report->ue_presence_in_area_of_interest_list[0].location_report_ref_id, 7U);
  ASSERT_EQ(first_report->ue_presence_in_area_of_interest_list[1].location_report_ref_id, 8U);

  ngap_location_reporting_control cancel;
  cancel.ue_index = ue->ue_index;
  cancel.request_type.event_type = ngap_location_reporting_event_type::cancel_location_report_for_the_ue;
  cancel.request_type.location_report_ref_id_to_be_cancelled = 7;
  ASSERT_TRUE(cu_cp_impl->get_cu_cp_ngap_handler().handle_location_reporting_control(cancel).accepted);

  cu_cp_impl->get_cu_cp_measurement_handler().handle_ue_location_report(
      make_ntn_location_report(ue->ue_index, make_default_env_nci(0), base_time + std::chrono::milliseconds{1}));

  ASSERT_TRUE(env.wait_for_ngap_tx_pdu(ngap_pdu));
  ASSERT_TRUE(is_pdu_type(ngap_pdu,
                          asn1::ngap::ngap_elem_procs_o::init_msg_c::types::types_opts::location_report));

  const auto& second_report = ngap_pdu.pdu.init_msg().value.location_report();
  ASSERT_EQ(second_report->location_report_request_type.event_type.value,
            asn1::ngap::event_type_opts::ue_presence_in_area_of_interest);
  ASSERT_TRUE(second_report->ue_presence_in_area_of_interest_list_present);
  ASSERT_EQ(second_report->ue_presence_in_area_of_interest_list.size(), 1U);
  ASSERT_EQ(second_report->ue_presence_in_area_of_interest_list[0].location_report_ref_id, 8U);
}
