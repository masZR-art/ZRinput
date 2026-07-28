cmake_minimum_required(VERSION 3.24)
project(ZRinput VERSION 0.1.0 LANGUAGES CXX)

add_library(zrinput_core STATIC
  src/core/personal_language_model.cpp
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
