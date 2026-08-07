add_subdirectory(common)
add_subdirectory(core)
add_subdirectory(theme)

if(WIN32 AND ZRINPUT_BUILD_WINDOWS)
  add_subdirectory(windows)
endif()
