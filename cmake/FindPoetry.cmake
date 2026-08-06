#[=======================================================================[.rst:
FindPoetry.cmake
---------------

This module is intended for use with ``find_package`` and should not be imported on
its own.

It will download and install the poetry package manager.

Usage
+++++

To use this module, make sure you're setting the cmake module path to this
directory and call
```
find_package(Poetry VERSION version)
```
The version is optional; without it the latest poetry release is installed.

Output Variables
++++++++++++++++
- ``Poetry_EXECUTABLE`` path to the poetry executable


#]=======================================================================]
include(FetchContent)

set(LOCALINSTALL_POETRY_DIR "${CMAKE_BINARY_DIR}/tools/poetry")

# Nothing to do if a previous configure already installed it.
find_program(
	Poetry_EXECUTABLE
	poetry
	PATHS "${LOCALINSTALL_POETRY_DIR}/bin"
	NO_DEFAULT_PATH
)
if(Poetry_EXECUTABLE AND EXISTS "${Poetry_EXECUTABLE}")
	message(STATUS "Poetry executable is located at: ${Poetry_EXECUTABLE}")
	return()
endif()
unset(Poetry_EXECUTABLE CACHE)

message(STATUS "Checking for installed Python package")
set(Python_FIND_UNVERSIONED_NAMES "FIRST")  # Helps find pyenv if installed
find_package(Python COMPONENTS Interpreter Development)
if(NOT ${Python_FOUND})
	message(FATAL_ERROR "Could not find installed python version. Cannot install poetry. Exiting...")
else()
	message(STATUS "Found Python executable at: ${Python_EXECUTABLE}")
endif()

message(STATUS "Downloading poetry install script")
FetchContent_Declare(
	POETRY_LOCALINSTALL
	URL "https://install.python-poetry.org/"
	DOWNLOAD_NAME "install_poetry.py"
	DOWNLOAD_NO_EXTRACT True
)
FetchContent_MakeAvailable(POETRY_LOCALINSTALL)

# FetchContent decides where the script lands; DOWNLOAD_DIR is not honoured here.
if(NOT EXISTS "${poetry_localinstall_SOURCE_DIR}/install_poetry.py")
	message(FATAL_ERROR "Poetry install script not found in ${poetry_localinstall_SOURCE_DIR}")
endif()

message(STATUS "Installing Poetry into ${LOCALINSTALL_POETRY_DIR}")
set(ENV{POETRY_HOME} ${LOCALINSTALL_POETRY_DIR})
if(Poetry_FIND_VERSION)
	set(ENV{POETRY_VERSION} ${Poetry_FIND_VERSION})
endif()
execute_process(
	COMMAND ${Python_EXECUTABLE} install_poetry.py
	WORKING_DIRECTORY ${poetry_localinstall_SOURCE_DIR}
	RESULT_VARIABLE _poetry_install_result
)
if(NOT _poetry_install_result EQUAL 0)
	message(FATAL_ERROR "Poetry install script failed with exit code ${_poetry_install_result}")
endif()

find_program(
	Poetry_EXECUTABLE
	poetry
	PATHS "${LOCALINSTALL_POETRY_DIR}/bin"
	NO_DEFAULT_PATH
	REQUIRED
)
message(STATUS "Poetry executable is located at: ${Poetry_EXECUTABLE}")
