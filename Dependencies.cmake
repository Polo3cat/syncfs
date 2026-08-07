include(cmake/CPM.cmake)

set(CPM_SOURCE_CACHE "${CMAKE_CURRENT_SOURCE_DIR}/.cpm-cache")

# Done as a function so that updates to variables like
# CMAKE_CXX_FLAGS don't propagate out to other
# targets
function(syncfs_setup_dependencies)

  # Every dependency must be a static archive so the syncfs binary carries them
  # and runs outside the build tree. libtorrent declares BUILD_SHARED_LIBS as an
  # option(), which writes a cache entry; once that entry exists every later
  # configure builds all dependencies shared. Seed the cache first so the
  # option() call is a no-op.
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build dependencies as static archives" FORCE)

  # EXCLUDE_FROM_ALL on every dependency keeps its install() rules out of ours:
  # since CMake 3.28 an excluded subdirectory is skipped by `cmake --install`.
  # Without it, `make install` copies the headers, archives and CMake package
  # files of Boost, libtorrent and libzmq into the prefix. The shorthand syntax
  # used for fmt already implies EXCLUDE_FROM_ALL and SYSTEM.
  cpmaddpackage("gh:fmtlib/fmt#11.1.4")

  cpmaddpackage(
    NAME spdlog
    VERSION 1.17.0
    GITHUB_REPOSITORY "gabime/spdlog"
    EXCLUDE_FROM_ALL YES
    OPTIONS "SPDLOG_FMT_EXTERNAL ON"
  )

  cpmaddpackage(
    NAME libzmq
    VERSION 4.3.5
    GITHUB_REPOSITORY "zeromq/libzmq"
    PATCHES patches/libzmq-cmake.patch
    EXCLUDE_FROM_ALL YES
    # libzmq ignores BUILD_SHARED_LIBS and uses its own BUILD_SHARED/BUILD_STATIC.
    # With BUILD_SHARED OFF only the libzmq-static target exists, so consumers
    # must link cppzmq-static rather than cppzmq.
    OPTIONS "BUILD_TESTS OFF" "BUILD_SHARED OFF"
  )

  cpmaddpackage(
    NAME cppzmq
    VERSION 4.11.0
    GITHUB_REPOSITORY "zeromq/cppzmq"
    PATCHES patches/cppzmq-zmq_addon.patch
    EXCLUDE_FROM_ALL YES
    OPTIONS "CPPZMQ_BUILD_TESTS OFF"
  )

  cpmaddpackage(
    NAME Boost 
    VERSION 1.91.0,
    URL https://github.com/boostorg/boost/releases/download/boost-1.91.0-1/boost-1.91.0-1-cmake.tar.xz
    URL_HASH SHA256=cc5dc5006ecbdf0051f90979be31b4eee5987d9ae14ae9fb9c03cfa43fa3cdad
    EXCLUDE_FROM_ALL YES
    # BOOST_SKIP_INSTALL_RULES stays OFF: it also drops the Boost targets from
    # their export set, and libtorrent exports torrent-rasterbar, which requires
    # boost_headers to be in one. The install rules are generated but never run,
    # EXCLUDE_FROM_ALL above keeps them out of `cmake --install`.
    OPTIONS "BOOST_ENABLE_CMAKE ON" "BOOST_LOCALE_ENABLE_ICU OFF" "BOOST_SKIP_INSTALL_RULES OFF" "BOOST_ENABLE_COMPATIBILITY_TARGETS ON"
  )

  cpmaddpackage(
    NAME libtorrent
    GITHUB_REPOSITORY arvidn/libtorrent
    VERSION 2.1.0
    EXCLUDE_FROM_ALL YES
    OPTIONS "webtorrent OFF" "deprecated-functions OFF"
  )
endfunction()
