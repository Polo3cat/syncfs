add_executable(syncfs syncfs.cpp files.cpp discovery.cpp)
target_include_directories(syncfs PRIVATE ${CMAKE_SOURCE_DIR}/include)
