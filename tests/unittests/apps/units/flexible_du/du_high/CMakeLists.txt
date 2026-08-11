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

add_subdirectory(metrics)

add_executable(du_high_unit_config_test du_high_unit_config_test.cpp)
target_include_directories(du_high_unit_config_test PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(du_high_unit_config_test srsran_du_high_unit_helpers gtest gtest_main)
gtest_discover_tests(du_high_unit_config_test PROPERTIES "LABELS;du_high_app_unit")
