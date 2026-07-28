cmake_minimum_required(VERSION 3.24)
project(ZRinput VERSION 0.1.0 LANGUAGES CXX)

add_library(zrinput_core STATIC
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

enable_testing()
add_test(NAME zrinput_core_tests COMMAND zrinput_core_tests)

if(WIN32)
  add_library(zrinput_tsf SHARED
    src/windows/dllmain.cpp
    src/windows/exports.def
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
    src/windows/candidate_window.cpp
    src/windows/theme.cpp
  )
  target_link_libraries(zrinput_candidate_preview PRIVATE zrinput_core user32 gdi32)
  target_compile_features(zrinput_candidate_preview PRIVATE cxx_std_20)
  target_compile_definitions(zrinput_candidate_preview PRIVATE UNICODE _UNICODE NOMINMAX)
  target_include_directories(zrinput_candidate_preview PRIVATE src)
  if(MSVC)
    target_compile_options(zrinput_candidate_preview PRIVATE /utf-8 /W4 /permissive-)
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

  install(TARGETS zrinput_tsf zrinput_theme_editor RUNTIME DESTINATION .)
  install(FILES data/default_lexicon.tsv DESTINATION data)
  install(FILES themes/microsoft-dark.ini DESTINATION themes)
endif()
