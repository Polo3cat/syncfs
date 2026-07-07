add_test(NAME syncfs-performance
	COMMAND "${Poetry_EXECUTABLE}" run python3 -m pytest "${CMAKE_CURRENT_SOURCE_DIR}"
	WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

set_tests_properties(syncfs-performance
	PROPERTIES ENVIRONMENT "PATH=$<TARGET_FILE_DIR:syncfs>:${CMAKE_SYSTEM_PREFIX_PATH};POETRY_VIRTUALENVS_IN_PROJECT=true")
