add_library(zrinput_common STATIC
  utf.cpp
)

target_include_directories(zrinput_common PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_compile_features(zrinput_common PUBLIC cxx_std_20)
zrinput_set_warnings(zrinput_common)

