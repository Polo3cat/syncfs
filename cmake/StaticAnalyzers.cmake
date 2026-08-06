macro(myproject_enable_cppcheck WARNINGS_AS_ERRORS CPPCHECK_OPTIONS)
  find_program(CPPCHECK cppcheck)
  if(CPPCHECK)

    if("${CPPCHECK_OPTIONS}" STREQUAL "")
      # Enable all warnings that are actionable by the user of this toolset
      # style should enable the other 3, but we'll be explicit just in case
      set(SUPPRESS_DIR "*:.cpm-cache/**")
      message(STATUS "CPPCHECK_OPTIONS suppress: ${SUPPRESS_DIR}")
      set(CMAKE_CXX_CPPCHECK
          ${CPPCHECK}
          --template=gcc
          --enable=style,performance,warning,portability
          --inline-suppr
          # We cannot act on a bug/missing feature of cppcheck
          --suppress=cppcheckError
          --suppress=internalAstError
          # if a file does not have an internalAstError, we get an unmatchedSuppression error
          --suppress=unmatchedSuppression
          # noisy and incorrect sometimes
          --suppress=passedByValue
          # ignores code that cppcheck thinks is invalid C++
          --suppress=syntaxError
          --suppress=preprocessorErrorDirective
          # ignores static_assert type failures
          --suppress=knownConditionTrueFalse
          --inconclusive
          --suppress=${SUPPRESS_DIR}
          # makes headers cleaner
          --suppress=funcArgNamesDifferentUnnamed)
    else()
      set(CMAKE_CXX_CPPCHECK ${CPPCHECK} --template=gcc ${CPPCHECK_OPTIONS})
    endif()

    if(NOT
       "${CMAKE_CXX_STANDARD}"
       STREQUAL
       "")
      set(CMAKE_CXX_CPPCHECK ${CMAKE_CXX_CPPCHECK} --std=c++${CMAKE_CXX_STANDARD})
    endif()
    if(${WARNINGS_AS_ERRORS})
      list(APPEND CMAKE_CXX_CPPCHECK --error-exitcode=2)
    endif()
  else()
    message(${WARNING_MESSAGE} "cppcheck requested but executable not found")
  endif()
endmacro()

macro(myproject_enable_clang_tidy target WARNINGS_AS_ERRORS)

  find_program(CLANGTIDY clang-tidy)
  if(CLANGTIDY)
    # construct the clang-tidy command line
    set(CLANG_TIDY_OPTIONS
        ${CLANGTIDY}
        -extra-arg=-Wno-unknown-warning-option
        -extra-arg=-Wno-ignored-optimization-argument
        -extra-arg=-Wno-unused-command-line-argument
        --allow-no-checks
        -p)
    # set standard
    if(NOT
       "${CMAKE_CXX_STANDARD}"
       STREQUAL
       "")
      set(CLANG_TIDY_OPTIONS ${CLANG_TIDY_OPTIONS} -extra-arg=-std=c++${CMAKE_CXX_STANDARD})
    endif()

    # set warnings as errors
    if(${WARNINGS_AS_ERRORS})
      list(APPEND CLANG_TIDY_OPTIONS -warnings-as-errors=*)
    endif()

    message("Also setting clang-tidy globally")
    set(CMAKE_CXX_CLANG_TIDY ${CLANG_TIDY_OPTIONS})
  else()
    message(${WARNING_MESSAGE} "clang-tidy requested but executable not found")
  endif()
endmacro()
