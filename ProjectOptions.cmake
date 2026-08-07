# Unix + clang only. MSVC/Windows/gcc/CUDA paths are intentionally absent.

include(CMakeDependentOption)
include(CheckCXXCompilerFlag)
include(CheckCXXSourceCompiles)

macro(syncfs_supports_sanitizers)
  set(TEST_PROGRAM "int main() { return 0; }")

  message(STATUS "Sanity checking UndefinedBehaviorSanitizer, it should be supported on this platform")
  set(CMAKE_REQUIRED_FLAGS "-fsanitize=undefined")
  set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=undefined")
  check_cxx_source_compiles("${TEST_PROGRAM}" HAS_UBSAN_LINK_SUPPORT)

  if(HAS_UBSAN_LINK_SUPPORT)
    message(STATUS "UndefinedBehaviorSanitizer is supported at both compile and link time.")
    set(SUPPORTS_UBSAN ON)
  else()
    message(WARNING "UndefinedBehaviorSanitizer is NOT supported at link time.")
    set(SUPPORTS_UBSAN OFF)
  endif()

  message(STATUS "Sanity checking AddressSanitizer, it should be supported on this platform")
  set(CMAKE_REQUIRED_FLAGS "-fsanitize=address")
  set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=address")
  check_cxx_source_compiles("${TEST_PROGRAM}" HAS_ASAN_LINK_SUPPORT)

  if(HAS_ASAN_LINK_SUPPORT)
    message(STATUS "AddressSanitizer is supported at both compile and link time.")
    set(SUPPORTS_ASAN ON)
  else()
    message(WARNING "AddressSanitizer is NOT supported at link time.")
    set(SUPPORTS_ASAN OFF)
  endif()
endmacro()

macro(syncfs_setup_options)
  option(syncfs_ENABLE_HARDENING "Enable hardening" ON)
  cmake_dependent_option(
    syncfs_ENABLE_GLOBAL_HARDENING
    "Attempt to push hardening options to built dependencies"
    ON
    syncfs_ENABLE_HARDENING
    OFF)

  syncfs_supports_sanitizers()

  option(syncfs_ENABLE_IPO "Enable IPO/LTO" ON)
  option(syncfs_WARNINGS_AS_ERRORS "Treat Warnings As Errors" ON)
  option(syncfs_ENABLE_SANITIZER_ADDRESS "Enable address sanitizer" ${SUPPORTS_ASAN})
  option(syncfs_ENABLE_SANITIZER_LEAK "Enable leak sanitizer" OFF)
  option(syncfs_ENABLE_SANITIZER_UNDEFINED "Enable undefined sanitizer" ${SUPPORTS_UBSAN})
  option(syncfs_ENABLE_SANITIZER_THREAD "Enable thread sanitizer" OFF)
  option(syncfs_ENABLE_SANITIZER_MEMORY "Enable memory sanitizer" OFF)
  option(syncfs_ENABLE_UNITY_BUILD "Enable unity builds" OFF)
  option(syncfs_ENABLE_CLANG_TIDY "Enable clang-tidy" ON)
  option(syncfs_ENABLE_CPPCHECK "Enable cpp-check analysis" ON)
  option(syncfs_ENABLE_PCH "Enable precompiled headers" OFF)
  option(syncfs_ENABLE_CACHE "Enable ccache" ON)
endmacro()

macro(syncfs_global_options)
  if(syncfs_ENABLE_IPO)
    include(cmake/InterproceduralOptimization.cmake)
    syncfs_enable_ipo()
  endif()

  syncfs_supports_sanitizers()

  if(syncfs_ENABLE_HARDENING AND syncfs_ENABLE_GLOBAL_HARDENING)
    include(cmake/Hardening.cmake)
    if(NOT SUPPORTS_UBSAN
       OR syncfs_ENABLE_SANITIZER_UNDEFINED
       OR syncfs_ENABLE_SANITIZER_ADDRESS
       OR syncfs_ENABLE_SANITIZER_THREAD
       OR syncfs_ENABLE_SANITIZER_LEAK)
      set(ENABLE_UBSAN_MINIMAL_RUNTIME FALSE)
    else()
      set(ENABLE_UBSAN_MINIMAL_RUNTIME TRUE)
    endif()
    syncfs_enable_hardening(syncfs_options ON ${ENABLE_UBSAN_MINIMAL_RUNTIME})
  endif()
endmacro()

macro(syncfs_local_options)
  include(cmake/StandardProjectSettings.cmake)

  add_library(syncfs_warnings INTERFACE)
  add_library(syncfs_options INTERFACE)

  include(cmake/CompilerWarnings.cmake)
  syncfs_set_project_warnings(syncfs_warnings ${syncfs_WARNINGS_AS_ERRORS} "")

  include(cmake/Sanitizers.cmake)
  syncfs_enable_sanitizers(
    syncfs_options
    ${syncfs_ENABLE_SANITIZER_ADDRESS}
    ${syncfs_ENABLE_SANITIZER_LEAK}
    ${syncfs_ENABLE_SANITIZER_UNDEFINED}
    ${syncfs_ENABLE_SANITIZER_THREAD}
    ${syncfs_ENABLE_SANITIZER_MEMORY})

  set_target_properties(syncfs_options PROPERTIES UNITY_BUILD ${syncfs_ENABLE_UNITY_BUILD})

  if(syncfs_ENABLE_PCH)
    target_precompile_headers(
      syncfs_options
      INTERFACE
      <vector>
      <string>
      <utility>)
  endif()

  if(syncfs_ENABLE_CACHE)
    include(cmake/Cache.cmake)
    syncfs_enable_cache()
  endif()

  include(cmake/StaticAnalyzers.cmake)
  if(syncfs_ENABLE_CLANG_TIDY)
    syncfs_enable_clang_tidy(syncfs_options ${syncfs_WARNINGS_AS_ERRORS})
  endif()

  if(syncfs_ENABLE_CPPCHECK)
    syncfs_enable_cppcheck(${syncfs_WARNINGS_AS_ERRORS} "" # override cppcheck options
    )
  endif()

  if(syncfs_ENABLE_HARDENING AND NOT syncfs_ENABLE_GLOBAL_HARDENING)
    include(cmake/Hardening.cmake)
    if(NOT SUPPORTS_UBSAN
       OR syncfs_ENABLE_SANITIZER_UNDEFINED
       OR syncfs_ENABLE_SANITIZER_ADDRESS
       OR syncfs_ENABLE_SANITIZER_THREAD
       OR syncfs_ENABLE_SANITIZER_LEAK)
      set(ENABLE_UBSAN_MINIMAL_RUNTIME FALSE)
    else()
      set(ENABLE_UBSAN_MINIMAL_RUNTIME TRUE)
    endif()
    syncfs_enable_hardening(syncfs_options OFF ${ENABLE_UBSAN_MINIMAL_RUNTIME})
  endif()
endmacro()
