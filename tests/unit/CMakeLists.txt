add_executable(utils-test utils_test.cpp)
target_link_libraries(utils-test GTest::gtest_main)
target_link_libraries(utils-test utils)
add_test(NAME utils-unit COMMAND utils-test)
