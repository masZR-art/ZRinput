add_executable(zrinput_core_tests
  composition_buffer_tests.cpp
)

target_link_libraries(zrinput_core_tests PRIVATE zrinput_core)
target_include_directories(zrinput_core_tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}")
zrinput_set_warnings(zrinput_core_tests)

add_test(NAME zrinput_core_tests COMMAND zrinput_core_tests)

