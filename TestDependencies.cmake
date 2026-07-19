function(setup_test_dependencies)
    find_package(Poetry)
    if (NOT DEFINED Poetry_EXECUTABLE)
        message(FATAL_ERROR "Poetry command could not be installed locally")
    endif()

    set(ENV{POETRY_VIRTUALENVS_IN_PROJECT} true)

    add_custom_target(poetry-install ALL
        COMMAND ${Poetry_EXECUTABLE} install --no-root
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Installing Python dependencies with Poetry"
    )

    cpmaddpackage(
        NAME googletest
        URL https://github.com/google/googletest/releases/download/v1.17.0/googletest-1.17.0.tar.gz
        URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
        VERSION 1.17.0
        OPTIONS "INSTALL_GTEST OFF"
    )

endfunction()
