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

#include "ntn_initial_ul_position_authorizer.h"
#include <algorithm>

using namespace srsran;
using namespace srsran::srs_cu_cp;

namespace {

constexpr size_t max_observation_identifier_size = 256U;

bool valid_key(const ntn_initial_ul_position_observation_key& key)
{
  return key.du_index != du_index_t::invalid && key.du_cell_index != du_cell_index_t::invalid &&
         key.c_rnti != rnti_t::INVALID_RNTI;
}

bool valid_observation(const ntn_initial_ul_position_observation& observation)
{
  if (observation.observation_id == 0 || observation.du_index == du_index_t::invalid ||
      observation.c_rnti == rnti_t::INVALID_RNTI) {
    return false;
  }
  return observation.du_cell_index != du_cell_index_t::invalid && !observation.satellite_id.empty() &&
         observation.satellite_id.size() <= max_observation_identifier_size &&
         observation.catalog_version != 0 && observation.schedule_version != 0 &&
         !observation.source_content_hash.empty() &&
         observation.source_content_hash.size() <= max_observation_identifier_size &&
         !observation.calendar_hash.empty() &&
         observation.calendar_hash.size() <= max_observation_identifier_size && is_valid(observation.pci) &&
         !observation.position_id.empty() && observation.position_id.size() <= max_observation_identifier_size &&
         observation.occasion_time != std::chrono::system_clock::time_point{} &&
         observation.ul_beam_port_id != ntn_access_calendar_intent::no_resource_port;
}

} // namespace

const char* srsran::srs_cu_cp::to_string(ntn_initial_ul_position_record_status status)
{
  switch (status) {
    case ntn_initial_ul_position_record_status::stored:
      return "stored";
    case ntn_initial_ul_position_record_status::invalid:
      return "invalid";
    case ntn_initial_ul_position_record_status::full:
      return "full";
    case ntn_initial_ul_position_record_status::replayed:
      return "replayed";
    case ntn_initial_ul_position_record_status::source_unavailable:
      return "source_unavailable";
  }
  return "invalid";
}

const char* srsran::srs_cu_cp::to_string(ntn_initial_ul_position_authorization_status status)
{
  switch (status) {
    case ntn_initial_ul_position_authorization_status::authorized:
      return "authorized";
    case ntn_initial_ul_position_authorization_status::observation_missing:
      return "observation_missing";
    case ntn_initial_ul_position_authorization_status::observation_expired:
      return "observation_expired";
    case ntn_initial_ul_position_authorization_status::ambiguous_observation:
      return "ambiguous_observation";
    case ntn_initial_ul_position_authorization_status::replayed_observation:
      return "replayed_observation";
    case ntn_initial_ul_position_authorization_status::source_unavailable:
      return "source_unavailable";
    case ntn_initial_ul_position_authorization_status::invalid_request:
      return "invalid_request";
    case ntn_initial_ul_position_authorization_status::ue_identity_mismatch:
      return "ue_identity_mismatch";
    case ntn_initial_ul_position_authorization_status::du_generation_mismatch:
      return "du_generation_mismatch";
    case ntn_initial_ul_position_authorization_status::active_plan_unavailable:
      return "active_plan_unavailable";
    case ntn_initial_ul_position_authorization_status::active_plan_mismatch:
      return "active_plan_mismatch";
    case ntn_initial_ul_position_authorization_status::plan_audit_only:
      return "plan_audit_only";
  }
  return "invalid_request";
}

ntn_initial_ul_position_record_status
ntn_initial_ul_position_observation_store::record(ntn_initial_ul_position_observation observation,
                                                  std::chrono::steady_clock::time_point now)
{
  if (!valid_observation(observation)) {
    return ntn_initial_ul_position_record_status::invalid;
  }

  std::lock_guard<std::mutex> lock(mutex);
  if (!source_ready) {
    return ntn_initial_ul_position_record_status::source_unavailable;
  }
  prune_expired(now);
  const auto duplicate = std::find_if(observations.begin(), observations.end(), [&](const stored_observation& stored) {
    return stored.value.observation_id == observation.observation_id;
  });
  if (duplicate != observations.end()) {
    return ntn_initial_ul_position_record_status::replayed;
  }
  if (observations.size() >= max_ntn_initial_ul_position_observations) {
    return ntn_initial_ul_position_record_status::full;
  }
  observations.push_back({std::move(observation), now, false});
  return ntn_initial_ul_position_record_status::stored;
}

ntn_initial_ul_position_take_result
ntn_initial_ul_position_observation_store::take(const ntn_initial_ul_position_observation_key& key,
                                                std::chrono::steady_clock::time_point now)
{
  if (!valid_key(key)) {
    return {ntn_initial_ul_position_take_status::missing, std::nullopt};
  }

  std::lock_guard<std::mutex> lock(mutex);
  if (!source_ready) {
    return {ntn_initial_ul_position_take_status::source_unavailable, std::nullopt};
  }

  const auto replay = std::find_if(observations.begin(), observations.end(), [&](const stored_observation& stored) {
    return same_key(stored.value, key) && stored.consumed &&
           now - stored.received_at < ntn_initial_ul_position_observation_ttl;
  });
  if (replay != observations.end()) {
    return {ntn_initial_ul_position_take_status::replayed, std::nullopt};
  }

  size_t live_matches    = 0;
  bool   expired_matches = false;
  auto   match           = observations.end();
  for (auto it = observations.begin(); it != observations.end(); ++it) {
    if (!same_key(it->value, key) || it->consumed) {
      continue;
    }
    if (now - it->received_at >= ntn_initial_ul_position_observation_ttl) {
      expired_matches = true;
      continue;
    }
    ++live_matches;
    match = it;
  }

  if (live_matches == 0) {
    if (expired_matches) {
      observations.erase(std::remove_if(observations.begin(),
                                        observations.end(),
                                        [&](const stored_observation& stored) {
                                          return same_key(stored.value, key) && !stored.consumed;
                                        }),
                         observations.end());
      return {ntn_initial_ul_position_take_status::expired, std::nullopt};
    }
    const auto stale_generation = std::find_if(observations.begin(), observations.end(), [&](const auto& stored) {
      return stored.value.du_index == key.du_index && stored.value.du_cell_index == key.du_cell_index &&
             stored.value.c_rnti == key.c_rnti && !stored.consumed &&
             now - stored.received_at < ntn_initial_ul_position_observation_ttl &&
             stored.value.du_connection_generation != key.du_connection_generation;
    });
    if (stale_generation != observations.end()) {
      return {ntn_initial_ul_position_take_status::du_generation_mismatch, std::nullopt};
    }
    prune_expired(now);
    return {ntn_initial_ul_position_take_status::missing, std::nullopt};
  }
  if (live_matches != 1) {
    return {ntn_initial_ul_position_take_status::ambiguous, std::nullopt};
  }

  match->consumed = true;
  return {ntn_initial_ul_position_take_status::found, match->value};
}

ntn_initial_ul_position_source_snapshot ntn_initial_ul_position_observation_store::source_snapshot() const
{
  std::lock_guard<std::mutex> lock(mutex);
  const auto                  now = std::chrono::steady_clock::now();
  return {source_ready,
          source_authority,
          static_cast<size_t>(std::count_if(observations.begin(), observations.end(), [&](const auto& stored) {
            return !stored.consumed && now - stored.received_at < ntn_initial_ul_position_observation_ttl;
          }))};
}

bool ntn_initial_ul_position_observation_store::is_ready() const
{
  return source_snapshot().ready;
}

std::string ntn_initial_ul_position_observation_store::authority() const
{
  return source_snapshot().authority;
}

size_t ntn_initial_ul_position_observation_store::pending_count() const
{
  std::lock_guard<std::mutex> lock(mutex);
  return std::count_if(observations.begin(), observations.end(), [](const stored_observation& stored) {
    return !stored.consumed;
  });
}

void ntn_initial_ul_position_observation_store::invalidate_du(du_index_t du_index)
{
  std::lock_guard<std::mutex> lock(mutex);
  observations.erase(std::remove_if(observations.begin(),
                                    observations.end(),
                                    [du_index](const stored_observation& stored) {
                                      return stored.value.du_index == du_index;
                                    }),
                     observations.end());
}

void ntn_initial_ul_position_observation_store::invalidate_all()
{
  std::lock_guard<std::mutex> lock(mutex);
  observations.clear();
}

void ntn_initial_ul_position_observation_store::set_source_ready(bool ready)
{
  std::lock_guard<std::mutex> lock(mutex);
  source_ready = ready;
  if (!source_ready) {
    observations.clear();
  }
}

bool ntn_initial_ul_position_observation_store::same_key(
    const ntn_initial_ul_position_observation& observation,
    const ntn_initial_ul_position_observation_key& key)
{
  return observation.du_index == key.du_index && observation.du_cell_index == key.du_cell_index &&
         observation.c_rnti == key.c_rnti &&
         observation.du_connection_generation == key.du_connection_generation;
}

void ntn_initial_ul_position_observation_store::prune_expired(std::chrono::steady_clock::time_point now)
{
  observations.erase(std::remove_if(observations.begin(),
                                    observations.end(),
                                    [&](const stored_observation& stored) {
                                      return now - stored.received_at >= ntn_initial_ul_position_observation_ttl;
                                    }),
                     observations.end());
}

ntn_initial_ul_position_authorization_result
ntn_initial_ul_position_authorizer::authorize(const ntn_initial_ul_position_authorization_request& request)
{
  ntn_initial_ul_position_authorization_result result;
  if (!valid_key(request.key) || request.ue_index == ue_index_t::invalid ||
      request.du_cell_index == du_cell_index_t::invalid || request.key.du_cell_index != request.du_cell_index ||
      request.key.du_connection_generation != request.du_connection_generation ||
      request.now == std::chrono::system_clock::time_point{}) {
    result.status = ntn_initial_ul_position_authorization_status::invalid_request;
    return result;
  }

  const ntn_initial_ul_position_take_result taken = provider.take(request.key, request.observation_now);
  switch (taken.status) {
    case ntn_initial_ul_position_take_status::found:
      if (!taken.observation.has_value()) {
        result.status = ntn_initial_ul_position_authorization_status::source_unavailable;
        return result;
      }
      break;
    case ntn_initial_ul_position_take_status::missing:
      result.status = ntn_initial_ul_position_authorization_status::observation_missing;
      return result;
    case ntn_initial_ul_position_take_status::expired:
      result.status = ntn_initial_ul_position_authorization_status::observation_expired;
      return result;
    case ntn_initial_ul_position_take_status::ambiguous:
      result.status = ntn_initial_ul_position_authorization_status::ambiguous_observation;
      return result;
    case ntn_initial_ul_position_take_status::replayed:
      result.status = ntn_initial_ul_position_authorization_status::replayed_observation;
      return result;
    case ntn_initial_ul_position_take_status::source_unavailable:
      result.status = ntn_initial_ul_position_authorization_status::source_unavailable;
      return result;
    case ntn_initial_ul_position_take_status::du_generation_mismatch:
      result.status = ntn_initial_ul_position_authorization_status::du_generation_mismatch;
      return result;
  }

  const ntn_initial_ul_position_observation& observation = *taken.observation;
  if (!valid_observation(observation)) {
    result.status = ntn_initial_ul_position_authorization_status::source_unavailable;
    return result;
  }
  result.observation_id                                  = observation.observation_id;
  result.position_id                                     = observation.position_id;

  if (observation.ue_index != ue_index_t::invalid && observation.ue_index != request.ue_index) {
    result.status = ntn_initial_ul_position_authorization_status::ue_identity_mismatch;
    return result;
  }
  if (observation.du_index != request.key.du_index || observation.du_cell_index != request.du_cell_index ||
      observation.du_connection_generation != request.du_connection_generation) {
    result.status = ntn_initial_ul_position_authorization_status::du_generation_mismatch;
    return result;
  }
  // The observation must describe the current Initial UL attempt, not merely any matching PRACH slot in the active
  // calendar. The same one-second bound used by the store prevents an old event from being re-injected with a fresh
  // receive timestamp, while a future event is rejected because it cannot have produced this Initial UL message.
  if (observation.occasion_time > request.now ||
      request.now - observation.occasion_time >= ntn_initial_ul_position_observation_ttl) {
    result.status = ntn_initial_ul_position_authorization_status::active_plan_mismatch;
    return result;
  }
  if (request.runtime_mapping == nullptr) {
    result.status = ntn_initial_ul_position_authorization_status::active_plan_unavailable;
    return result;
  }

  const ntn_onboard_runtime_mapping_snapshot& mapping = *request.runtime_mapping;
  if (request.now < mapping.activation_epoch() || request.now < mapping.valid_from() ||
      request.now >= mapping.valid_until()) {
    result.status = ntn_initial_ul_position_authorization_status::active_plan_mismatch;
    return result;
  }

  ntn_initial_access_plan_event event;
  event.satellite_id        = observation.satellite_id;
  event.catalog_version     = observation.catalog_version;
  event.schedule_version    = observation.schedule_version;
  event.source_content_hash = observation.source_content_hash;
  event.calendar_hash       = observation.calendar_hash;
  event.cell                = {observation.nci, observation.pci};
  event.position_id         = observation.position_id;
  event.occasion_time       = observation.occasion_time;
  event.ul_beam_port_id     = observation.ul_beam_port_id;
  result.plan_audit         = controller.audit_initial_access_event(event);

  if (result.plan_audit.decision == ntn_initial_access_plan_decision::audit_only) {
    result.status = ntn_initial_ul_position_authorization_status::plan_audit_only;
    return result;
  }
  if (result.plan_audit.decision != ntn_initial_access_plan_decision::accept) {
    result.status = ntn_initial_ul_position_authorization_status::active_plan_mismatch;
    return result;
  }

  const ntn_onboard_runtime_position*   position = mapping.find_position(observation.position_id);
  const ntn_onboard_runtime_cell_route* route    = mapping.resolve_cell_route(observation.nci);
  if (observation.satellite_id != mapping.satellite_id() || observation.catalog_version != mapping.catalog_version() ||
      observation.schedule_version != mapping.schedule_version() ||
      observation.source_content_hash != mapping.source_hash() ||
      observation.calendar_hash != mapping.calendar_hash() ||
      position == nullptr || route == nullptr || position->owner.nci != observation.nci ||
      position->owner.pci != observation.pci || route->identity.nci != observation.nci ||
      route->identity.pci != observation.pci || route->du_index != observation.du_index ||
      route->du_cell_index != observation.du_cell_index ||
      route->du_connection_generation != observation.du_connection_generation) {
    result.status = ntn_initial_ul_position_authorization_status::active_plan_mismatch;
    return result;
  }

  result.status = ntn_initial_ul_position_authorization_status::authorized;
  return result;
}
