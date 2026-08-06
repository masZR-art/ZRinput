cmake_minimum_required(VERSION 3.26)

project(ZRinput VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ZRINPUT_BUILD_TESTS "Build unit and integration tests" ON)
option(ZRINPUT_BUILD_WINDOWS "Build Windows TSF and desktop applications" ON)
option(ZRINPUT_ENABLE_STRICT_WARNINGS "Treat project warnings as errors" ON)

if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY
      "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ZRinputWarnings)

add_subdirectory(src)
add_subdirectory(tools)

if(ZRINPUT_BUILD_TESTS)
  include(CTest)
  enable_testing()
  add_subdirectory(tests)
endif()

include(GNUInstallDirs)
include(CPack)
