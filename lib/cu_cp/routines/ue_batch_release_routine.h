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

#include "../cu_cp_impl_interface.h"
#include "../ue_manager/ue_manager_impl.h"
#include "srsran/cu_cp/cu_cp_types.h"
#include <memory>

namespace srsran {
namespace srs_cu_cp {

/// Releases a batch of UEs by reusing the regular UE Context Release Command procedure per UE.
class ue_batch_release_routine
{
public:
  ue_batch_release_routine(std::vector<cu_cp_ue_context_release_command> commands_,
                           cu_cp_ue_context_release_handler&             ue_release_handler_,
                           ue_manager&                                   ue_mng_,
                           srslog::basic_logger&                         logger_);

  static const char* name() { return "UE Batch Release Routine"; }

  void operator()(coro_context<async_task<cu_cp_ue_context_release_batch_response>>& ctx);

private:
  struct release_state;

  void schedule_ue_release(const cu_cp_ue_context_release_command& command);
  static void
  mark_ue_release_finished(const std::shared_ptr<release_state>& state, ue_index_t ue_index, bool released);

  std::vector<cu_cp_ue_context_release_command>           commands;
  std::vector<cu_cp_ue_context_release_command>::iterator command_it;
  cu_cp_ue_context_release_handler&                       ue_release_handler;
  ue_manager&                                             ue_mng;
  srslog::basic_logger&                                   logger;

  std::shared_ptr<release_state> state;
};

} // namespace srs_cu_cp
} // namespace srsran
