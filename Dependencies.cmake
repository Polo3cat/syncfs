include(cmake/CPM.cmake)

set(CPM_SOURCE_CACHE "${CMAKE_CURRENT_SOURCE_DIR}/.cpm-cache")

# Done as a function so that updates to variables like
# CMAKE_CXX_FLAGS don't propagate out to other
# targets
function(syncfs_setup_dependencies)

  cpmaddpackage("gh:fmtlib/fmt#11.1.4")

  cpmaddpackage(
    NAME spdlog
    VERSION 1.17.0
    GITHUB_REPOSITORY "gabime/spdlog"
    OPTIONS "SPDLOG_FMT_EXTERNAL ON"
  )

  cpmaddpackage(
    NAME libzmq
    VERSION 4.3.5
    GITHUB_REPOSITORY "zeromq/libzmq"
    PATCHES patches/libzmq-cmake.patch
    OPTIONS "BUILD_TESTS OFF"
  )

  cpmaddpackage(
    NAME cppzmq
    VERSION 4.11.0
    GITHUB_REPOSITORY "zeromq/cppzmq"
    PATCHES patches/cppzmq-zmq_addon.patch
    OPTIONS "CPPZMQ_BUILD_TESTS OFF"
  )

  cpmaddpackage(
    NAME Boost 
    VERSION 1.91.0,
    URL https://github.com/boostorg/boost/releases/download/boost-1.91.0-1/boost-1.91.0-1-cmake.tar.xz
    URL_HASH SHA256=cc5dc5006ecbdf0051f90979be31b4eee5987d9ae14ae9fb9c03cfa43fa3cdad
    OPTIONS "BOOST_ENABLE_CMAKE ON" "BOOST_LOCALE_ENABLE_ICU OFF" "BOOST_SKIP_INSTALL_RULES OFF" "BOOST_ENABLE_COMPATIBILITY_TARGETS ON"
  )

  cpmaddpackage(
    NAME libtorrent
    GITHUB_REPOSITORY arvidn/libtorrent
    VERSION 2.1.0
    OPTIONS "webtorrent OFF" "deprecated-functions OFF"
  )
endfunction()
