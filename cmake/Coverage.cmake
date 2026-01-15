function(mskdsp_enable_coverage_flags)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR
      "MSKDSP_BUILD_TESTS requires GCC/Clang coverage flags; current compiler is ${CMAKE_CXX_COMPILER_ID}"
    )
  endif()

  add_compile_options(-O0 -g --coverage)
  add_link_options(--coverage)
endfunction()
