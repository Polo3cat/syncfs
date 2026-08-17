# Every test carries a deadline of its own. Without one ctest falls back to its
# 1500 second default, and a test that wedges rather than fails takes the whole
# suite down with it and names nobody: B11 cost forty eight minutes of that
# before anyone knew which test was blocked. The unit tests are milliseconds of
# work, so the number here is not a budget for them to fit in. It is the point
# past which one of them is not slow but stuck.
set(unit_timeout 120)

add_executable(utils-test utils_test.cpp)
target_link_libraries(utils-test GTest::gtest_main utils)
add_test(NAME utils-unit COMMAND utils-test)
set_tests_properties(utils-unit PROPERTIES TIMEOUT ${unit_timeout})

add_executable(monitor-test monitor_test.cpp)
target_link_libraries(monitor-test GTest::gtest_main monitor)
add_test(NAME monitor-unit COMMAND monitor-test)
set_tests_properties(monitor-unit PROPERTIES TIMEOUT ${unit_timeout})

# The reconciliation ships inside the protocol library, so it is the same
# ctest name: one wire, one suite.
add_executable(protocol-test protocol_test.cpp reconcile_test.cpp)
target_link_libraries(protocol-test GTest::gtest_main protocol utils torrent-rasterbar)
add_test(NAME protocol-unit COMMAND protocol-test)
set_tests_properties(protocol-unit PROPERTIES TIMEOUT ${unit_timeout})

add_executable(source-test source_test.cpp)
target_link_libraries(source-test GTest::gtest_main source utils torrent-rasterbar)
add_test(NAME source-unit COMMAND source-test)
set_tests_properties(source-unit PROPERTIES TIMEOUT ${unit_timeout})
target_compile_options(source-test PRIVATE $<$<CONFIG:Debug>:-glldb>)
