add_library(zrinput_theme STATIC
  theme.cpp
  theme_manager.cpp
)

target_include_directories(zrinput_theme PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(zrinput_theme PUBLIC zrinput_common)
target_compile_features(zrinput_theme PUBLIC cxx_std_20)
zrinput_set_warnings(zrinput_theme)
