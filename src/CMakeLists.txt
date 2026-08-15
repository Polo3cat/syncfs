add_library(utils STATIC utils.cpp)
target_include_directories(utils PUBLIC ${CMAKE_SOURCE_DIR}/include/utils)
target_link_libraries(utils PRIVATE Boost::algorithm)

add_library(source STATIC source.cpp)
target_include_directories(source PUBLIC ${CMAKE_SOURCE_DIR}/include/source)
target_link_libraries(source PUBLIC cppzmq-static)
target_link_libraries(source PRIVATE spdlog::spdlog)
target_link_libraries(source PRIVATE torrent-rasterbar)
target_link_libraries(source PRIVATE utils)

add_library(protocol STATIC protocol.cpp)
target_include_directories(protocol PUBLIC ${CMAKE_SOURCE_DIR}/include/protocol)
target_link_libraries(protocol PUBLIC cppzmq-static)
target_link_libraries(protocol PUBLIC torrent-rasterbar)
target_link_libraries(protocol PRIVATE spdlog::spdlog)
target_link_libraries(protocol PRIVATE utils)

add_library(sink STATIC sink.cpp)
target_include_directories(sink PUBLIC ${CMAKE_SOURCE_DIR}/include/sink)
target_link_libraries(sink PRIVATE cppzmq-static)

add_library(monitor STATIC monitor.cpp)
target_include_directories(monitor PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(monitor PRIVATE spdlog::spdlog)

add_executable(syncfs syncfs.cpp files.cpp discovery.cpp)
target_include_directories(syncfs PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(syncfs PRIVATE monitor)
target_link_libraries(syncfs PRIVATE protocol)
target_link_libraries(syncfs PRIVATE source)
target_link_libraries(syncfs PRIVATE sink)
target_link_libraries(syncfs PRIVATE utils)
target_link_libraries(syncfs PRIVATE cppzmq-static)
target_link_libraries(syncfs PRIVATE spdlog::spdlog)

target_link_options(syncfs PRIVATE -static-libgcc)
# Garbage collect sections and fold identical code.
target_link_options(syncfs PRIVATE -fuse-ld=lld -Wl,--gc-sections -Wl,--icf=safe)

install(TARGETS syncfs RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
