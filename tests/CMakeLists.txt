add_executable(zrinput_core_tests
  candidate_pipeline_tests.cpp
  composition_buffer_tests.cpp
  dictionary_decoder_tests.cpp
  pinyin_parser_tests.cpp
  prediction_service_tests.cpp
  user_memory_tests.cpp
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

if(WIN32 AND ZRINPUT_BUILD_WINDOWS)
  add_executable(zrinput_tsf_smoke_tests
    tsf_smoke_tests.cpp
  )
  target_include_directories(zrinput_tsf_smoke_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src")
  target_link_libraries(zrinput_tsf_smoke_tests PRIVATE zrinput_tsf_impl)
  target_compile_features(zrinput_tsf_smoke_tests PRIVATE cxx_std_20)
  zrinput_set_warnings(zrinput_tsf_smoke_tests)
  add_dependencies(zrinput_tsf_smoke_tests zrinput_tsf)

  add_test(NAME zrinput_tsf_smoke_tests
           COMMAND zrinput_tsf_smoke_tests $<TARGET_FILE:zrinput_tsf>)
  set_tests_properties(zrinput_tsf_smoke_tests PROPERTIES TIMEOUT 30)

  add_executable(zrinput_tsf_registration_tests
    tsf_registration_tests.cpp
  )
  target_link_libraries(zrinput_tsf_registration_tests PRIVATE
    zrinput_tsf_impl)
  target_compile_features(zrinput_tsf_registration_tests PRIVATE cxx_std_20)
  zrinput_set_warnings(zrinput_tsf_registration_tests)
  add_test(NAME zrinput_tsf_registration_tests
           COMMAND zrinput_tsf_registration_tests)
  set_tests_properties(zrinput_tsf_registration_tests PROPERTIES TIMEOUT 30)

  get_filename_component(ZRINPUT_MSVC_TOOL_DIR "${CMAKE_LINKER}" DIRECTORY)
  set(ZRINPUT_DUMPBIN "${ZRINPUT_MSVC_TOOL_DIR}/dumpbin.exe")
  if(EXISTS "${ZRINPUT_DUMPBIN}")
    add_test(NAME zrinput_tsf_binary_tests
      COMMAND powershell.exe -NoProfile -NonInteractive
              -ExecutionPolicy Bypass
              -File "${CMAKE_CURRENT_SOURCE_DIR}/verify_tsf_binary.ps1"
              -DllPath $<TARGET_FILE:zrinput_tsf>
              -DumpbinPath "${ZRINPUT_DUMPBIN}"
              -ExpectedMachine "${CMAKE_GENERATOR_PLATFORM}")
    set_tests_properties(zrinput_tsf_binary_tests PROPERTIES TIMEOUT 30)
  endif()
endif()
