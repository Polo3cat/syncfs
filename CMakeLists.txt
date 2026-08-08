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

# Used so the linker can garbage collect later.
string(APPEND CMAKE_C_FLAGS " -ffunction-sections -fdata-sections")
string(APPEND CMAKE_CXX_FLAGS " -ffunction-sections -fdata-sections")

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

add_subdirectory(src)

include(CTest)

if(BUILD_TESTING)
  include(TestDependencies.cmake)
  setup_test_dependencies()
  add_subdirectory(tests)
endif()


include(GNUInstallDirs)

# Create symlink to compile_commands.json for IDE to pick it up
execute_process(
    COMMAND ${CMAKE_COMMAND} -E create_symlink
        ${CMAKE_BINARY_DIR}/compile_commands.json
        ${CMAKE_CURRENT_SOURCE_DIR}/compile_commands.json
)
