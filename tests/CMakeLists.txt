add_executable(zrinput_core_tests
  candidate_pipeline_tests.cpp
  composition_buffer_tests.cpp
  dictionary_decoder_tests.cpp
  pinyin_parser_tests.cpp
  prediction_service_tests.cpp
)

target_link_libraries(zrinput_core_tests PRIVATE zrinput_core)
target_include_directories(zrinput_core_tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}")
zrinput_set_warnings(zrinput_core_tests)

add_test(NAME zrinput_core_tests COMMAND zrinput_core_tests)

add_executable(zrinput_lexicon_integration_tests
  lexicon_integration_tests.cpp
)
target_link_libraries(zrinput_lexicon_integration_tests PRIVATE zrinput_core)
if(WIN32)
  target_link_libraries(zrinput_lexicon_integration_tests PRIVATE psapi)
endif()
zrinput_set_warnings(zrinput_lexicon_integration_tests)

add_test(NAME zrinput_lexicon_integration_tests
  COMMAND zrinput_lexicon_integration_tests
          "${PROJECT_SOURCE_DIR}/data/system.zrdict"
          "${PROJECT_SOURCE_DIR}/data/pinyin_syllables.txt")

add_executable(zrinput_composition_fuzz_tests
  composition_fuzz_tests.cpp
)
target_link_libraries(zrinput_composition_fuzz_tests PRIVATE zrinput_core)
zrinput_set_warnings(zrinput_composition_fuzz_tests)
add_test(NAME zrinput_composition_fuzz_tests
         COMMAND zrinput_composition_fuzz_tests)
set_tests_properties(zrinput_composition_fuzz_tests PROPERTIES TIMEOUT 60)

add_executable(zrinput_theme_tests
  theme_tests.cpp
)
target_link_libraries(zrinput_theme_tests PRIVATE zrinput_theme)
target_include_directories(zrinput_theme_tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}")
zrinput_set_warnings(zrinput_theme_tests)
add_test(NAME zrinput_theme_tests COMMAND zrinput_theme_tests)

add_executable(zrinput_theme_manager_tests
  theme_manager_tests.cpp
)
target_link_libraries(zrinput_theme_manager_tests PRIVATE zrinput_theme)
target_include_directories(zrinput_theme_manager_tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}")
zrinput_set_warnings(zrinput_theme_manager_tests)
add_test(NAME zrinput_theme_manager_tests COMMAND zrinput_theme_manager_tests)
set_tests_properties(zrinput_theme_manager_tests PROPERTIES TIMEOUT 30)
