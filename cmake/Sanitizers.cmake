function(matching_engine_enable_sanitizers target_name)
  if(NOT MATCHING_ENGINE_ENABLE_SANITIZERS)
    return()
  endif()

  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    message(WARNING "Sanitizers are enabled, but ${CMAKE_CXX_COMPILER_ID} is not configured here.")
    return()
  endif()

  target_compile_options(${target_name} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(${target_name} PRIVATE -fsanitize=address,undefined)
endfunction()
