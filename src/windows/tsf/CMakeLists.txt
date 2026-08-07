add_library(zrinput_tsf_impl STATIC
  class_factory.cpp
  module_state.cpp
  registration.cpp
  registration_transaction.cpp
  text_service.cpp
)

target_include_directories(zrinput_tsf_impl PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(zrinput_tsf_impl PUBLIC
  zrinput_core
  advapi32
  ole32
  oleaut32
  user32
  uuid
)
target_compile_features(zrinput_tsf_impl PUBLIC cxx_std_20)
target_compile_definitions(zrinput_tsf_impl PRIVATE
  NOMINMAX
  UNICODE
  WIN32_LEAN_AND_MEAN
  WINVER=0x0A00
  _UNICODE
  _WIN32_WINNT=0x0A00
)
zrinput_set_warnings(zrinput_tsf_impl)
if(MSVC)
  target_compile_options(zrinput_tsf_impl PRIVATE /guard:cf)
endif()

add_library(zrinput_tsf SHARED
  dll_exports.cpp
  zrinput_tsf.def
  zrinput_tsf.rc
)

target_link_libraries(zrinput_tsf PRIVATE zrinput_tsf_impl)
target_compile_features(zrinput_tsf PRIVATE cxx_std_20)
target_compile_definitions(zrinput_tsf PRIVATE
  NOMINMAX
  UNICODE
  WIN32_LEAN_AND_MEAN
  WINVER=0x0A00
  _UNICODE
  _WIN32_WINNT=0x0A00
)
set_target_properties(zrinput_tsf PROPERTIES
  OUTPUT_NAME "ZRinputTsf"
)
zrinput_set_warnings(zrinput_tsf)
if(MSVC)
  target_compile_options(zrinput_tsf PRIVATE /guard:cf)
  target_link_options(zrinput_tsf PRIVATE
    /DYNAMICBASE
    /guard:cf
    /HIGHENTROPYVA
    /NXCOMPAT
  )
endif()

install(TARGETS zrinput_tsf
  RUNTIME DESTINATION bin
  COMPONENT Runtime
)
