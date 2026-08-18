add_test(NAME syncfs-performance
	COMMAND "${Poetry_EXECUTABLE}" run python3 -m pytest "${CMAKE_CURRENT_SOURCE_DIR}"
	WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# The suite measures around seventy seconds, and the one gigabyte case carries
# its own hundred and twenty second deadline, so a run past this one is not a
# benchmark that came out badly: it is a run that is not coming back. Without a
# deadline ctest waits 1500 seconds and then names nobody (B11).
set_tests_properties(syncfs-performance PROPERTIES TIMEOUT 400)

# The build directory first, then the PATH this configure ran with.
# CMAKE_SYSTEM_PREFIX_PATH used to stand in for the second half, and it is a
# semicolon separated list of install prefixes (/usr/local;/usr) rather than a
# PATH: /usr/bin was unreachable, so syncfs was the only tool a test could
# resolve by name and everything else needed an absolute path.
set_tests_properties(syncfs-performance
	PROPERTIES ENVIRONMENT "PATH=$<TARGET_FILE_DIR:syncfs>:$ENV{PATH};POETRY_VIRTUALENVS_IN_PROJECT=true")
