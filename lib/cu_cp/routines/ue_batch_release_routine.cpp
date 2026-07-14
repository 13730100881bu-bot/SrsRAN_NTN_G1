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

#include "ue_batch_release_routine.h"
#include "srsran/support/async/coroutine.h"
#include "srsran/support/async/manual_event.h"
#include <set>

using namespace srsran;
using namespace srs_cu_cp;

struct ue_batch_release_routine::release_state {
  cu_cp_ue_context_release_batch_response response;
  manual_event_flag                       all_ues_released;
  unsigned                                nof_pending_releases = 0;
  std::set<ue_index_t>                    scheduled_ues;
};

ue_batch_release_routine::ue_batch_release_routine(std::vector<cu_cp_ue_context_release_command> commands_,
                                                   cu_cp_ue_context_release_handler& ue_release_handler_,
                                                   ue_manager&                       ue_mng_,
                                                   srslog::basic_logger&             logger_) :
  commands(std::move(commands_)),
  ue_release_handler(ue_release_handler_),
  ue_mng(ue_mng_),
  logger(logger_),
  state(std::make_shared<release_state>())
{
}

void ue_batch_release_routine::operator()(coro_context<async_task<cu_cp_ue_context_release_batch_response>>& ctx)
{
  CORO_BEGIN(ctx);

  state->response.nof_requested_ues = commands.size();
  logger.info("\"{}\" started for {} UEs", name(), state->response.nof_requested_ues);

  for (command_it = commands.begin(); command_it != commands.end(); ++command_it) {
    schedule_ue_release(*command_it);
  }

  if (state->nof_pending_releases != 0) {
    CORO_AWAIT(state->all_ues_released);
  }

  logger.info("\"{}\" finished: released={} not_found={} duplicate={} schedule_failed={}",
              name(),
              state->response.released_ues.size(),
              state->response.ues_not_found.size(),
              state->response.duplicate_ues.size(),
              state->response.failed_to_schedule_ues.size());

  CORO_RETURN(state->response);
}

void ue_batch_release_routine::schedule_ue_release(const cu_cp_ue_context_release_command& command)
{
  if (command.ue_index == ue_index_t::invalid) {
    logger.warning("Ignoring UE batch release item with invalid UE index");
    state->response.ues_not_found.push_back(command.ue_index);
    return;
  }

  if (!state->scheduled_ues.insert(command.ue_index).second) {
    logger.warning("ue={}: Batch release skipped. Cause: duplicate UE index", command.ue_index);
    state->response.duplicate_ues.push_back(command.ue_index);
    return;
  }

  cu_cp_ue* ue = ue_mng.find_du_ue(command.ue_index);
  if (ue == nullptr) {
    logger.warning("ue={}: Batch release skipped. Cause: DU UE not found", command.ue_index);
    state->response.ues_not_found.push_back(command.ue_index);
    return;
  }

  ++state->nof_pending_releases;
  auto state_copy = state;
  cu_cp_ue_context_release_handler& release_handler = ue_release_handler;
  async_task<void> release_task =
      launch_async([state_copy, &release_handler, command](coro_context<async_task<void>>& ctx) mutable {
        CORO_BEGIN(ctx);

        CORO_AWAIT(release_handler.handle_ue_context_release_command(command));
        ue_batch_release_routine::mark_ue_release_finished(state_copy, command.ue_index, true);

        CORO_RETURN();
      });

  if (!ue->get_task_sched().schedule_async_task(std::move(release_task))) {
    logger.warning("ue={}: Batch release could not be scheduled", command.ue_index);
    state->response.failed_to_schedule_ues.push_back(command.ue_index);
    mark_ue_release_finished(state, command.ue_index, false);
  }
}

void ue_batch_release_routine::mark_ue_release_finished(const std::shared_ptr<release_state>& state,
                                                        ue_index_t                            ue_index,
                                                        bool                                  released)
{
  if (released) {
    state->response.released_ues.push_back(ue_index);
  }
  if (--state->nof_pending_releases == 0) {
    state->all_ues_released.set();
  }
}
