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

add_executable(ntn_mobility_test
  ntn_beam_placement_planner_test.cpp
  ntn_satellite_state_updater_test.cpp
  ntn_served_beam_scheduler_test.cpp
)
target_include_directories(ntn_mobility_test PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(ntn_mobility_test srsran_cu_cp_ntn_mobility srsran_support srslog gtest gtest_main)
gtest_discover_tests(ntn_mobility_test PROPERTIES "LABELS;ntn_mobility")
