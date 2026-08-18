add_test(NAME syncfs-integration
	COMMAND "${Poetry_EXECUTABLE}" run python3 -m pytest "${CMAKE_CURRENT_SOURCE_DIR}"
	WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# Four times what the suite measures at, so that a loaded host is still slow
# rather than stuck. Without a deadline of its own ctest falls back to 1500
# seconds and a wedged test takes the whole run with it while naming nobody
# (B11). These tests wait on a control plane that drops messages, so the ones
# that fail do so on their own budgets; this only catches the ones that never
# come back at all.
set_tests_properties(syncfs-integration PROPERTIES TIMEOUT 900)

# The build directory first, then the PATH this configure ran with.
# CMAKE_SYSTEM_PREFIX_PATH used to stand in for the second half, and it is a
# semicolon separated list of install prefixes (/usr/local;/usr) rather than a
# PATH: /usr/bin was unreachable, so syncfs was the only tool a test could
# resolve by name and everything else needed an absolute path.
set_tests_properties(syncfs-integration
	PROPERTIES ENVIRONMENT "PATH=$<TARGET_FILE_DIR:syncfs>:$ENV{PATH};POETRY_VIRTUALENVS_IN_PROJECT=true;SYNCFS_BINARY_DIR=${CMAKE_BINARY_DIR}")
