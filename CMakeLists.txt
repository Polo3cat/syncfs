cmake_minimum_required(VERSION 4.3)

set(CMAKE_CXX_STANDARD 23)

# strongly encouraged to enable this globally to avoid conflicts between
# -Wpedantic being enabled and -std=c++20 and -std=gnu++20 for example
# when compiling with PCH enabled
set(CMAKE_CXX_EXTENSIONS OFF)

# Set the project name and language
project(
  syncfs
  VERSION 0.0.1
  DESCRIPTION "Syncronizing file system"
  LANGUAGES CXX)


list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(cmake/PreventInSourceBuilds.cmake)
include(ProjectOptions.cmake)

syncfs_setup_options()

syncfs_global_options()
include(Dependencies.cmake)
syncfs_setup_dependencies()

syncfs_local_options()

# don't know if this should be set globally from here or not...
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

set(GIT_SHA
    "Unknown"
    CACHE STRING "SHA this build was generated from")
string(
  SUBSTRING "${GIT_SHA}"
            0
            8
            GIT_SHORT_SHA)

target_compile_features(syncfs_options INTERFACE cxx_std_${CMAKE_CXX_STANDARD})

add_library(syncfs::syncfs_options ALIAS syncfs_options)
add_library(syncfs::syncfs_warnings ALIAS syncfs_warnings)

#add_library(syncfs::syncfs_options INTERFACE IMPORTED)
#add_library(syncfs::syncfs_warnings INTERFACE IMPORTED)

# Adding the src:
add_subdirectory(src)

# Don't even look at tests if we're not top level
if(NOT PROJECT_IS_TOP_LEVEL)
  return()
endif()

# Adding the tests:
include(CTest)

if(BUILD_TESTING)
  include(TestDependencies.cmake)
  setup_test_dependencies()
  add_subdirectory(tests)
endif()


if(CMAKE_SKIP_INSTALL_RULES)
  return()
endif()

include(cmake/PackageProject.cmake)

# Add other targets that you want installed here, by default we just package the one executable
# we know we want to ship
syncfs_package_project(
  TARGETS
  syncfs
  syncfs_options
  syncfs_warnings
  # FIXME: this does not work! CK
  # PRIVATE_DEPENDENCIES_CONFIGURED project_options project_warnings
)

# Experience shows that explicit package naming can help make it easier to sort
# out potential ABI related issues before they start, while helping you
# track a build to a specific GIT SHA
set(CPACK_PACKAGE_FILE_NAME
    "${CMAKE_PROJECT_NAME}-${CMAKE_PROJECT_VERSION}-${GIT_SHORT_SHA}-${CMAKE_SYSTEM_NAME}-${CMAKE_BUILD_TYPE}-${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}"
)

# Create symlink to compile_commands.json for IDE to pick it up
execute_process(
    COMMAND ${CMAKE_COMMAND} -E create_symlink
        ${CMAKE_BINARY_DIR}/compile_commands.json
        ${CMAKE_CURRENT_SOURCE_DIR}/compile_commands.json
)

include(CPack)
