add_executable(utils-test utils_test.cpp)
target_link_libraries(utils-test GTest::gtest_main utils)
add_test(NAME utils-unit COMMAND utils-test)

add_executable(monitor-test monitor_test.cpp)
target_link_libraries(monitor-test GTest::gtest_main monitor)
add_test(NAME monitor-unit COMMAND monitor-test)

add_executable(protocol-test protocol_test.cpp)
target_link_libraries(protocol-test GTest::gtest_main protocol torrent-rasterbar)
add_test(NAME protocol-unit COMMAND protocol-test)

add_executable(source-test source_test.cpp)
target_link_libraries(source-test GTest::gtest_main source torrent-rasterbar)
add_test(NAME source-unit COMMAND source-test)
target_compile_options(source-test PRIVATE $<$<CONFIG:Debug>:-glldb>)
