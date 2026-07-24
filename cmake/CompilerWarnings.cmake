function(matching_engine_enable_warnings target_name)
  if(MSVC)
    target_compile_options(${target_name} PRIVATE /W4 /permissive-)
    return()
  endif()

  target_compile_options(
    ${target_name}
    PRIVATE -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wsign-conversion
            -Wshadow)
endfunction()
