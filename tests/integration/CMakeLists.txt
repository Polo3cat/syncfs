add_test(NAME syncfs-integration
	COMMAND "${Poetry_EXECUTABLE}" run python3 -m pytest "${CMAKE_CURRENT_SOURCE_DIR}"
	WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

set_tests_properties(syncfs-integration
	PROPERTIES ENVIRONMENT "PATH=$<TARGET_FILE_DIR:syncfs>:${CMAKE_SYSTEM_PREFIX_PATH};POETRY_VIRTUALENVS_IN_PROJECT=true")

add_test(NAME syncfs-update
	COMMAND "${Poetry_EXECUTABLE}" run python3 -m pytest "${CMAKE_CURRENT_SOURCE_DIR}/many_syncfs_test.py::test_one_syncf_updates_many_other" 
	WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

set_tests_properties(syncfs-update
	PROPERTIES ENVIRONMENT "PATH=$<TARGET_FILE_DIR:syncfs>:${CMAKE_SYSTEM_PREFIX_PATH};POETRY_VIRTUALENVS_IN_PROJECT=true")
