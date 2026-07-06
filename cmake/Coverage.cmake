function(mskdsp_enable_coverage_flags)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR
      "MSKDSP_ENABLE_COVERAGE 需要 GCC/Clang 覆盖率编译选项；当前编译器为 ${CMAKE_CXX_COMPILER_ID}"
    )
  endif()

  add_compile_options(-O0 -g --coverage)
  add_link_options(--coverage)
endfunction()
