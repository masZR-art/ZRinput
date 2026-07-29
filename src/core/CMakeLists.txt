add_library(zrinput_core STATIC
  composition_buffer.cpp
)

target_include_directories(zrinput_core PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(zrinput_core PUBLIC zrinput_common)
target_compile_features(zrinput_core PUBLIC cxx_std_20)
zrinput_set_warnings(zrinput_core)

