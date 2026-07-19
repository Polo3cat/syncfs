add_executable(sink-test sink_test.cpp)
target_link_libraries(sink-test sink)
target_link_libraries(sink-test GTest::gtest_main)
