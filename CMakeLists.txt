cmake_minimum_required(VERSION 3.24)
project(ZRinput VERSION 0.1.0 LANGUAGES CXX)

if(MSVC)
  # Release ZIPs must work on a clean Windows installation without a separate
  # Visual C++ Redistributable bootstrapper.
  set(CMAKE_MSVC_RUNTIME_LIBRARY
      "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_library(zrinput_core STATIC
  src/core/composition_state.cpp
  src/core/personal_language_model.cpp
  src/core/pinyin_parser.cpp
  src/core/pinyin_engine.cpp
)
target_include_directories(zrinput_core PUBLIC src)
target_compile_features(zrinput_core PUBLIC cxx_std_20)
if(MSVC)
  target_compile_options(zrinput_core PUBLIC /utf-8 /W4 /permissive-)
endif()

add_executable(zrinput_core_tests tests/core_tests.cpp)
target_link_libraries(zrinput_core_tests PRIVATE zrinput_core)

add_executable(zrinput_query tools/query_cli.cpp)
target_link_libraries(zrinput_query PRIVATE zrinput_core)

add_executable(zrinput_lexicon_tests tests/lexicon_integration_tests.cpp)
target_link_libraries(zrinput_lexicon_tests PRIVATE zrinput_core)

add_executable(zrinput_lexicon_engine_tests tests/lexicon_engine_tests.cpp)
target_link_libraries(zrinput_lexicon_engine_tests PRIVATE zrinput_core)
target_compile_features(zrinput_lexicon_engine_tests PRIVATE cxx_std_20)
if(MSVC)
  target_compile_options(zrinput_lexicon_engine_tests PRIVATE /utf-8 /W4 /permissive-)
endif()

enable_testing()
add_test(NAME zrinput_core_tests COMMAND zrinput_core_tests)
add_test(NAME zrinput_powershell_script_tests
         COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
                 -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/powershell_script_tests.ps1"
                 -BuildDirectory "${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME zrinput_lexicon_tests COMMAND zrinput_lexicon_tests
  "${CMAKE_CURRENT_SOURCE_DIR}/data/default_lexicon.tsv")
add_test(NAME zrinput_lexicon_engine_tests
         COMMAND zrinput_lexicon_engine_tests
                 "${CMAKE_CURRENT_SOURCE_DIR}/data/default_lexicon.tsv"
                 "${CMAKE_CURRENT_SOURCE_DIR}/data/standard_pinyin_syllables.txt")
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME zrinput_lexicon_audit
           COMMAND "${Python3_EXECUTABLE}"
                   "${CMAKE_CURRENT_SOURCE_DIR}/tests/lexicon_audit.py")
endif()

if(WIN32)
  add_library(zrinput_tsf SHARED
    src/windows/dllmain.cpp
    src/windows/exports.def
    src/windows/candidate_layout.cpp
    src/windows/candidate_window.cpp
    src/windows/theme.cpp
    src/windows/registration.cpp
    src/windows/text_service.cpp
  )
  target_link_libraries(zrinput_tsf PRIVATE
    zrinput_core
    ole32
    advapi32
    shell32
  )
  target_compile_features(zrinput_tsf PRIVATE cxx_std_20)
  target_compile_definitions(zrinput_tsf PRIVATE UNICODE _UNICODE NOMINMAX)
  target_include_directories(zrinput_tsf PRIVATE src)
  set_target_properties(zrinput_tsf PROPERTIES OUTPUT_NAME "ZRinputTSF")
  if(MSVC)
    target_compile_options(zrinput_tsf PRIVATE /utf-8 /W4 /permissive-)
  endif()

  add_executable(zrinput_candidate_preview WIN32
    tools/candidate_preview.cpp
    src/windows/candidate_layout.cpp
    src/windows/candidate_window.cpp
    src/windows/theme.cpp
  )
  target_link_libraries(zrinput_candidate_preview PRIVATE zrinput_core user32 gdi32 shell32)
  target_compile_features(zrinput_candidate_preview PRIVATE cxx_std_20)
  target_compile_definitions(zrinput_candidate_preview PRIVATE UNICODE _UNICODE NOMINMAX)
  target_include_directories(zrinput_candidate_preview PRIVATE src)
  if(MSVC)
    target_compile_options(zrinput_candidate_preview PRIVATE /utf-8 /W4 /permissive-)
  endif()
  add_test(NAME zrinput_candidate_preview_render
           COMMAND zrinput_candidate_preview --render
                   "${CMAKE_CURRENT_BINARY_DIR}/candidate-preview-test.bmp")
  if(Python3_Interpreter_FOUND)
    add_test(NAME zrinput_candidate_bitmap_audit
             COMMAND "${Python3_EXECUTABLE}"
                     "${CMAKE_CURRENT_SOURCE_DIR}/tests/candidate_bitmap_audit.py"
                     "${CMAKE_CURRENT_BINARY_DIR}/candidate-preview-test.bmp")
    set_tests_properties(zrinput_candidate_bitmap_audit PROPERTIES
                         DEPENDS zrinput_candidate_preview_render)
  endif()

  add_executable(zrinput_theme_editor WIN32
    tools/theme_editor.cpp
    src/windows/theme.cpp
  )
  target_link_libraries(zrinput_theme_editor PRIVATE user32 gdi32 comdlg32 shell32 ole32)
  target_compile_features(zrinput_theme_editor PRIVATE cxx_std_20)
  target_compile_definitions(zrinput_theme_editor PRIVATE UNICODE _UNICODE NOMINMAX)
  target_include_directories(zrinput_theme_editor PRIVATE src)
  if(MSVC)
    target_compile_options(zrinput_theme_editor PRIVATE /utf-8 /W4 /permissive-)
  endif()

  add_executable(zrinput_theme_tests
    tests/theme_tests.cpp
    src/windows/theme.cpp
  )
  target_compile_features(zrinput_theme_tests PRIVATE cxx_std_20)
  target_compile_definitions(zrinput_theme_tests PRIVATE UNICODE _UNICODE NOMINMAX)
  target_include_directories(zrinput_theme_tests PRIVATE src)
  if(MSVC)
    target_compile_options(zrinput_theme_tests PRIVATE /utf-8 /W4 /permissive-)
  endif()
  add_test(NAME zrinput_theme_tests COMMAND zrinput_theme_tests)

  add_executable(zrinput_candidate_layout_tests
    tests/candidate_layout_tests.cpp
    src/windows/candidate_layout.cpp
  )
  target_compile_features(zrinput_candidate_layout_tests PRIVATE cxx_std_20)
  target_include_directories(zrinput_candidate_layout_tests PRIVATE src)
  if(MSVC)
    target_compile_options(zrinput_candidate_layout_tests PRIVATE /utf-8 /W4 /permissive-)
  endif()
  add_test(NAME zrinput_candidate_layout_tests
           COMMAND zrinput_candidate_layout_tests)

  add_executable(zrinput_profile_tool tools/profile_tool.cpp)
  target_link_libraries(zrinput_profile_tool PRIVATE ole32)
  target_compile_features(zrinput_profile_tool PRIVATE cxx_std_20)
  target_compile_definitions(zrinput_profile_tool PRIVATE UNICODE _UNICODE NOMINMAX)
  target_include_directories(zrinput_profile_tool PRIVATE src)
  if(MSVC)
    target_compile_options(zrinput_profile_tool PRIVATE /utf-8 /W4 /permissive-)
  endif()

  add_executable(zrinput_tsf_dll_smoke_tests tests/tsf_dll_smoke_tests.cpp)
  target_compile_features(zrinput_tsf_dll_smoke_tests PRIVATE cxx_std_20)
  target_compile_definitions(zrinput_tsf_dll_smoke_tests PRIVATE
                             UNICODE _UNICODE NOMINMAX)
  target_include_directories(zrinput_tsf_dll_smoke_tests PRIVATE src)
  if(MSVC)
    target_compile_options(zrinput_tsf_dll_smoke_tests PRIVATE
                           /utf-8 /W4 /permissive-)
  endif()
  add_test(NAME zrinput_tsf_dll_smoke_tests
           COMMAND zrinput_tsf_dll_smoke_tests
                   "$<TARGET_FILE:zrinput_tsf>")

  install(TARGETS zrinput_tsf zrinput_theme_editor zrinput_profile_tool
          RUNTIME DESTINATION .)
  install(FILES data/default_lexicon.tsv DESTINATION data)
  install(FILES themes/microsoft-dark.ini DESTINATION themes)
  install(FILES README.md LICENSE THIRD_PARTY_NOTICES.md DESTINATION .)
  install(PROGRAMS scripts/install.ps1 scripts/update.ps1 scripts/uninstall.ps1
                   scripts/package_common.ps1
           DESTINATION scripts)
endif()

set(CPACK_GENERATOR ZIP)
set(CPACK_PACKAGE_NAME ZRinput)
set(CPACK_PACKAGE_VERSION "0.1.0-alpha.2")
set(CPACK_PACKAGE_FILE_NAME "ZRinput-0.1.0-alpha.2-windows-x64")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
include(CPack)
