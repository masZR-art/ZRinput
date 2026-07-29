function(zrinput_set_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE
      /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc)
    if(ZRINPUT_ENABLE_STRICT_WARNINGS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    if(ZRINPUT_ENABLE_STRICT_WARNINGS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

