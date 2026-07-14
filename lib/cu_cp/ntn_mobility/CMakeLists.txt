#
# Copyright 2021-2026 Software Radio Systems Limited
#
# This file is part of srsRAN
#
# srsRAN is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as
# published by the Free Software Foundation, either version 3 of
# the License, or (at your option) any later version.
#
# srsRAN is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU Affero General Public License for more details.
#
# A copy of the GNU Affero General Public License can be found in
# the LICENSE file in the top-level directory of this distribution
# and at http://www.gnu.org/licenses/.
#

add_library(srsran_cu_cp_ntn_mobility STATIC
  ntn_assistance_snapshot_generator.cpp
  ntn_beam_assignment_repository.cpp
  ntn_beam_rebalance_policy.cpp
  ntn_beam_service_resource_manager.cpp
  ntn_beam_tac.cpp
  ntn_beam_placement_planner.cpp
  ntn_qos_policy.cpp
  ntn_onboard_position_plan.cpp
  ntn_satellite_state_updater.cpp
  ntn_service_switch_over_controller.cpp
  ntn_served_beam_scheduler.cpp
  ntn_served_beam_selector.cpp
  ntn_sib19_assistance_builder.cpp
  ntn_sib19_broadcast_controller.cpp
  ntn_ue_capability_gate.cpp
)

target_link_libraries(srsran_cu_cp_ntn_mobility rrc_nr_asn1 srsran_ntn srsran_ran srsran_security srslog srsran_support)

target_include_directories(srsran_cu_cp_ntn_mobility PUBLIC
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}
)
