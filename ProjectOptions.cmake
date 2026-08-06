# Unix + clang only. MSVC/Windows/gcc/CUDA paths are intentionally absent.

include(CMakeDependentOption)
include(CheckCXXCompilerFlag)
include(CheckCXXSourceCompiles)

macro(myproject_supports_sanitizers)
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

macro(myproject_setup_options)
  option(myproject_ENABLE_HARDENING "Enable hardening" ON)
  cmake_dependent_option(
    myproject_ENABLE_GLOBAL_HARDENING
    "Attempt to push hardening options to built dependencies"
    ON
    myproject_ENABLE_HARDENING
    OFF)

  myproject_supports_sanitizers()

  option(myproject_ENABLE_IPO "Enable IPO/LTO" ON)
  option(myproject_WARNINGS_AS_ERRORS "Treat Warnings As Errors" ON)
  option(myproject_ENABLE_SANITIZER_ADDRESS "Enable address sanitizer" ${SUPPORTS_ASAN})
  option(myproject_ENABLE_SANITIZER_LEAK "Enable leak sanitizer" OFF)
  option(myproject_ENABLE_SANITIZER_UNDEFINED "Enable undefined sanitizer" ${SUPPORTS_UBSAN})
  option(myproject_ENABLE_SANITIZER_THREAD "Enable thread sanitizer" OFF)
  option(myproject_ENABLE_SANITIZER_MEMORY "Enable memory sanitizer" OFF)
  option(myproject_ENABLE_UNITY_BUILD "Enable unity builds" OFF)
  option(myproject_ENABLE_CLANG_TIDY "Enable clang-tidy" ON)
  option(myproject_ENABLE_CPPCHECK "Enable cpp-check analysis" ON)
  option(myproject_ENABLE_PCH "Enable precompiled headers" OFF)
  option(myproject_ENABLE_CACHE "Enable ccache" ON)
endmacro()

macro(myproject_global_options)
  if(myproject_ENABLE_IPO)
    include(cmake/InterproceduralOptimization.cmake)
    myproject_enable_ipo()
  endif()

  myproject_supports_sanitizers()

  if(myproject_ENABLE_HARDENING AND myproject_ENABLE_GLOBAL_HARDENING)
    include(cmake/Hardening.cmake)
    if(NOT SUPPORTS_UBSAN
       OR myproject_ENABLE_SANITIZER_UNDEFINED
       OR myproject_ENABLE_SANITIZER_ADDRESS
       OR myproject_ENABLE_SANITIZER_THREAD
       OR myproject_ENABLE_SANITIZER_LEAK)
      set(ENABLE_UBSAN_MINIMAL_RUNTIME FALSE)
    else()
      set(ENABLE_UBSAN_MINIMAL_RUNTIME TRUE)
    endif()
    myproject_enable_hardening(myproject_options ON ${ENABLE_UBSAN_MINIMAL_RUNTIME})
  endif()
endmacro()

macro(myproject_local_options)
  include(cmake/StandardProjectSettings.cmake)

  add_library(myproject_warnings INTERFACE)
  add_library(myproject_options INTERFACE)

  include(cmake/CompilerWarnings.cmake)
  myproject_set_project_warnings(myproject_warnings ${myproject_WARNINGS_AS_ERRORS} "")

  include(cmake/Sanitizers.cmake)
  myproject_enable_sanitizers(
    myproject_options
    ${myproject_ENABLE_SANITIZER_ADDRESS}
    ${myproject_ENABLE_SANITIZER_LEAK}
    ${myproject_ENABLE_SANITIZER_UNDEFINED}
    ${myproject_ENABLE_SANITIZER_THREAD}
    ${myproject_ENABLE_SANITIZER_MEMORY})

  set_target_properties(myproject_options PROPERTIES UNITY_BUILD ${myproject_ENABLE_UNITY_BUILD})

  if(myproject_ENABLE_PCH)
    target_precompile_headers(
      myproject_options
      INTERFACE
      <vector>
      <string>
      <utility>)
  endif()

  if(myproject_ENABLE_CACHE)
    include(cmake/Cache.cmake)
    myproject_enable_cache()
  endif()

  include(cmake/StaticAnalyzers.cmake)
  if(myproject_ENABLE_CLANG_TIDY)
    myproject_enable_clang_tidy(myproject_options ${myproject_WARNINGS_AS_ERRORS})
  endif()

  if(myproject_ENABLE_CPPCHECK)
    myproject_enable_cppcheck(${myproject_WARNINGS_AS_ERRORS} "" # override cppcheck options
    )
  endif()

  if(myproject_ENABLE_HARDENING AND NOT myproject_ENABLE_GLOBAL_HARDENING)
    include(cmake/Hardening.cmake)
    if(NOT SUPPORTS_UBSAN
       OR myproject_ENABLE_SANITIZER_UNDEFINED
       OR myproject_ENABLE_SANITIZER_ADDRESS
       OR myproject_ENABLE_SANITIZER_THREAD
       OR myproject_ENABLE_SANITIZER_LEAK)
      set(ENABLE_UBSAN_MINIMAL_RUNTIME FALSE)
    else()
      set(ENABLE_UBSAN_MINIMAL_RUNTIME TRUE)
    endif()
    myproject_enable_hardening(myproject_options OFF ${ENABLE_UBSAN_MINIMAL_RUNTIME})
  endif()
endmacro()
