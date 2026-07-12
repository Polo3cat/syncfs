include(cmake/CPM.cmake)

set(CPM_SOURCE_CACHE "${CMAKE_CURRENT_SOURCE_DIR}/.cpm-cache")

# Done as a function so that updates to variables like
# CMAKE_CXX_FLAGS don't propagate out to other
# targets
function(myproject_setup_dependencies)

  # For each dependency, see if it's
  # already been provided to us by a parent project

  if(NOT TARGET fmtlib::fmtlib)
    cpmaddpackage("gh:fmtlib/fmt#11.1.4")
  endif()

  if(NOT TARGET spdlog::spdlog)
    cpmaddpackage(
      NAME
      spdlog
      VERSION
      1.17.0
      GITHUB_REPOSITORY
      "gabime/spdlog"
      OPTIONS
      "SPDLOG_FMT_EXTERNAL ON")
  endif()

  if(NOT TARGET Catch2::Catch2WithMain)
    cpmaddpackage("gh:catchorg/Catch2@3.12.0")
  endif()

  if(NOT TARGET CLI11::CLI11)
    cpmaddpackage("gh:CLIUtils/CLI11@2.5.0")
  endif()

  if(NOT TARGET ftxui::screen)
    cpmaddpackage("gh:ArthurSonzogni/FTXUI@6.0.2")
  endif()

  if(NOT TARGET tools::tools)
    cpmaddpackage("gh:lefticus/tools#update_build_system")
  endif()

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
    EXCLUDE_FROM_ALL
  )

  find_package(Boost REQUIRED COMPONENTS json)

  cpmaddpackage("gh:arvidn/libtorrent@2.1.0")
  # I'm too lazy to patch libtorrent
  target_link_libraries(torrent-rasterbar PUBLIC Boost::json)

endfunction()
