add_executable(utils-test utils_test.cpp)
target_link_libraries(utils-test GTest::gtest_main utils)
add_test(NAME utils-unit COMMAND utils-test)

add_executable(sink-test sink_test.cpp)
target_link_libraries(sink-test GTest::gtest_main sink torrent-rasterbar)
add_test(NAME sink-unit COMMAND sink-test)
target_compile_options(sink-test PRIVATE $<$<CONFIG:Debug>:-glldb>)
