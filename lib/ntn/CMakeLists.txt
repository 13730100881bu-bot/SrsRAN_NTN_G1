add_library(srsran_ntn
  beam_position.cpp
  beam_hopping_table.cpp
  ta_calculator.cpp
  orbit_propagator.cpp
  beam_hopping_controller.cpp
)

target_link_libraries(srsran_ntn
  srsran_ran
  srslog
)

target_include_directories(srsran_ntn PUBLIC
  ${CMAKE_SOURCE_DIR}/include
)

add_to_exported_libs(srsran_ntn)
