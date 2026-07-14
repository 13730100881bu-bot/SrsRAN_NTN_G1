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
  ntn_beam_assignment_repository.cpp
  ntn_beam_placement_planner.cpp
  ntn_satellite_state_updater.cpp
  ntn_served_beam_scheduler.cpp
  ntn_served_beam_selector.cpp
)

target_link_libraries(srsran_cu_cp_ntn_mobility srsran_ntn srsran_ran srslog srsran_support)

target_include_directories(srsran_cu_cp_ntn_mobility PUBLIC
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}
)
