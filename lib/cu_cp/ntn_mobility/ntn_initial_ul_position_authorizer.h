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
 * the LICENSE file in the top-level directory of this distribution.
 *
 */

#pragma once

#include "ntn_onboard_position_plan.h"
#include "ntn_onboard_runtime_mapping.h"
#include "srsran/cu_cp/ntn_initial_ul_position_observation.h"
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace srsran {
namespace srs_cu_cp {

enum class ntn_initial_ul_position_record_status { stored, invalid, full, replayed, source_unavailable };

enum class ntn_initial_ul_position_authorization_status {
  authorized,
  observation_missing,
  observation_expired,
  ambiguous_observation,
  replayed_observation,
  source_unavailable,
  invalid_request,
  ue_identity_mismatch,
  du_generation_mismatch,
  rnti_generation_mismatch,
  receive_port_unavailable,
  active_plan_unavailable,
  active_plan_mismatch,
  plan_audit_only
};

const char* to_string(ntn_initial_ul_position_record_status status);
const char* to_string(ntn_initial_ul_position_authorization_status status);

/// Thread-safe, bounded and intentionally non-persistent observation store.
///
/// A successful take consumes the observation exactly once and retains a replay marker only for the remainder of the
/// same one-second lifetime. Two live observations for one (DU, C-RNTI) are ambiguous and neither is consumed.
class ntn_initial_ul_position_observation_store final : public ntn_initial_ul_position_observation_provider
{
public:
  explicit ntn_initial_ul_position_observation_store(std::string authority_ = "private_injected",
                                                     bool rnti_generation_authoritative_ = false,
                                                     bool device_verification_capable_ = false) :
    source_authority(std::move(authority_)),
    rnti_generation_authoritative(rnti_generation_authoritative_),
    device_verification_capable(device_verification_capable_)
  {
  }

  ntn_initial_ul_position_record_status
  record(ntn_initial_ul_position_observation observation,
         std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

  ntn_initial_ul_position_source_snapshot source_snapshot() const override;

  bool        is_ready() const;
  std::string authority() const;

  ntn_initial_ul_position_take_result
  take(const ntn_initial_ul_position_observation_key& key,
       std::chrono::steady_clock::time_point          now = std::chrono::steady_clock::now()) override;

  size_t pending_count() const;
  void   invalidate_du(du_index_t du_index) override;
  void   invalidate_all() override;

  /// Controls availability of the private observation source. Making it unavailable also discards pending records.
  void set_source_ready(bool ready);

private:
  struct stored_observation {
    ntn_initial_ul_position_observation value;
    /// CU-CP receive time. It is assigned by the store and is never accepted from the source.
    std::chrono::steady_clock::time_point received_at{};
    bool                                  consumed = false;
  };

  static bool same_key(const ntn_initial_ul_position_observation& observation,
                       const ntn_initial_ul_position_observation_key& key);
  void        prune_expired(std::chrono::steady_clock::time_point now);

  mutable std::mutex             mutex;
  std::deque<stored_observation> observations;
  std::string                    source_authority;
  bool                           source_ready = true;
  bool                           rnti_generation_authoritative = false;
  bool                           device_verification_capable = false;
};

struct ntn_initial_ul_position_authorization_request {
  ntn_initial_ul_position_observation_key key;
  ue_index_t                              ue_index      = ue_index_t::invalid;
  du_cell_index_t                         du_cell_index = du_cell_index_t::invalid;
  uint64_t                                du_connection_generation = 0;
  /// Strict production admission accepts only an SDR or OFH device-verified observation.
  bool                                    require_device_verified = false;
  std::shared_ptr<const ntn_onboard_runtime_mapping_snapshot> runtime_mapping;
  std::chrono::system_clock::time_point now{};
  std::chrono::steady_clock::time_point observation_now{};
};

struct ntn_initial_ul_position_authorization_result {
  ntn_initial_ul_position_authorization_status status =
      ntn_initial_ul_position_authorization_status::observation_missing;
  uint64_t                                     observation_id = 0;
  std::string                                  position_id;
  ntn_initial_ul_position_observation_authority authority =
      ntn_initial_ul_position_observation_authority::invalid;
  ntn_initial_access_plan_audit                plan_audit;
};

/// Consumes one observation and checks it against the current UE/DU route, runtime mapping and active plan.
class ntn_initial_ul_position_authorizer
{
public:
  ntn_initial_ul_position_authorizer(ntn_initial_ul_position_observation_provider& provider_,
                                     const ntn_onboard_position_plan_controller& controller_) :
    provider(provider_), controller(controller_)
  {
  }

  ntn_initial_ul_position_authorization_result authorize(const ntn_initial_ul_position_authorization_request& request);

private:
  ntn_initial_ul_position_observation_provider& provider;
  const ntn_onboard_position_plan_controller&   controller;
};

} // namespace srs_cu_cp
} // namespace srsran
