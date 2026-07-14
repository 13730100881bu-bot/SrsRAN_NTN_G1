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

#include "srsran/cu_cp/common_task_scheduler.h"
#include "srsran/nrppa/nrppa.h"
#include <memory>

namespace srsran {
namespace srs_cu_cp {

class nrppa_dummy_impl : public nrppa_interface, public nrppa_message_handler, public nrppa_ue_context_removal_handler
{
public:
  nrppa_dummy_impl(nrppa_cu_cp_notifier& cu_cp_notifier_, common_task_scheduler& common_task_sched_);
  ~nrppa_dummy_impl();

  // See nrppa_message_handler for documentation.
  void handle_new_nrppa_pdu(const byte_buffer&                    nrppa_pdu,
                            std::variant<ue_index_t, amf_index_t> ue_or_amf_index) override;

  // See nrppa_ue_context_removal_handle for documentation.
  void remove_ue_context(ue_index_t ue_index) override;

  nrppa_message_handler&            get_nrppa_message_handler() override { return *this; }
  nrppa_ue_context_removal_handler& get_nrppa_ue_context_removal_handler() override { return *this; }

private:
  srslog::basic_logger& logger;
  nrppa_cu_cp_notifier& cu_cp_notifier;
  common_task_scheduler& common_task_sched;

  async_task<void> handle_trp_information_request(trp_information_request_t request,
                                                  amf_index_t               amf_index,
                                                  bool                      use_standard_codec);
  async_task<void> handle_positioning_information_request(positioning_information_request_t request, ue_index_t ue_index);
  async_task<void> handle_positioning_activation_request(positioning_activation_request_t request, ue_index_t ue_index);
  async_task<void> handle_positioning_deactivation_request(positioning_deactivation_request_t request, ue_index_t ue_index);
  async_task<void> handle_positioning_assistance_information_control(
      positioning_assistance_information_control_request_t request, amf_index_t amf_index);
  async_task<void> handle_measurement_request(measurement_request_t request, ue_index_t ue_index);
};

} // namespace srs_cu_cp
} // namespace srsran
