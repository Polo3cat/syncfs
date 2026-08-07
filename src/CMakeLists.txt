add_library(source STATIC source.cpp)
target_include_directories(source PUBLIC ${CMAKE_SOURCE_DIR}/include/source)
target_link_libraries(source PUBLIC cppzmq-static)
target_link_libraries(source PRIVATE spdlog::spdlog)
target_link_libraries(source PRIVATE torrent-rasterbar)

add_library(protocol STATIC protocol.cpp)
target_include_directories(protocol PUBLIC ${CMAKE_SOURCE_DIR}/include/protocol)
target_link_libraries(protocol PUBLIC cppzmq-static)
target_link_libraries(protocol PUBLIC torrent-rasterbar)
target_link_libraries(protocol PRIVATE spdlog::spdlog)

add_library(sink STATIC sink.cpp)
target_include_directories(sink PUBLIC ${CMAKE_SOURCE_DIR}/include/sink)
target_link_libraries(sink PRIVATE cppzmq-static)

add_library(utils STATIC utils.cpp)
target_include_directories(utils PUBLIC ${CMAKE_SOURCE_DIR}/include/utils)
target_link_libraries(utils PRIVATE Boost::algorithm)

add_executable(syncfs syncfs.cpp files.cpp discovery.cpp monitor.cpp)
target_include_directories(syncfs PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(syncfs PRIVATE protocol)
target_link_libraries(syncfs PRIVATE source)
target_link_libraries(syncfs PRIVATE sink)
target_link_libraries(syncfs PRIVATE utils)
target_link_libraries(syncfs PRIVATE cppzmq-static)
target_link_libraries(syncfs PRIVATE spdlog::spdlog)

# Fold the C++ runtime into the binary as well. libc, OpenSSL and libatomic stay
# dynamic so name resolution keeps working and OpenSSL updates apply without a
# rebuild.
target_link_options(syncfs PRIVATE -static-libstdc++ -static-libgcc)
