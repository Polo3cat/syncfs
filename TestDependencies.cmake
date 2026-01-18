function(setup_test_dependencies)
    find_package(Poetry)
    if (NOT DEFINED Poetry_EXECUTABLE)
        message(FATAL_ERROR "Poetry command could not be installed locally")
    endif()
    
    add_custom_target(poetry-install ALL
        COMMAND ${Poetry_EXECUTABLE} install --no-root
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Installing Python dependencies with Poetry"
    )

endfunction()
