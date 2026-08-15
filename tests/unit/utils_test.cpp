#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <stdexcept>
#include <utility>
#include <utils.h>

// For debugging remember to call
// set target.disable-aslr false
// on lldb
TEST(Utils, ParseHostPort) {
  ASSERT_EQ(utils::parse_host_port("1.1.1.1:1234"),
            (std::pair{"1.1.1.1", 1234}));
}

TEST(Utils, EmptyHostThrows) {
  ASSERT_THROW(utils::parse_host_port(":1234"), std::invalid_argument);
}

TEST(Utils, EmptyPortThrows) {
  ASSERT_THROW(utils::parse_host_port("1.1.1.1:"), std::invalid_argument);
}

TEST(Utils, MissingColonThrows) {
  ASSERT_THROW(utils::parse_host_port("1.1.1.1 1234"), std::out_of_range);
}

TEST(Utils, TicksRoundTripThroughTheWireForm) {
  const auto now = std::chrono::system_clock::now();
  ASSERT_EQ(utils::from_ticks(std::to_string(utils::to_ticks(now))), now);
}

TEST(Utils, UnreadableTicksAreTheEpoch) {
  // A count that is not a decimal integer loses every ordering comparison
  // there is, which is the only safe reading of a peer that cannot spell the
  // protocol.
  const auto epoch = std::chrono::system_clock::time_point{};
  ASSERT_EQ(utils::from_ticks(""), epoch);
  ASSERT_EQ(utils::from_ticks("not a time"), epoch);
  ASSERT_EQ(utils::from_ticks("17000000 "), epoch);
  ASSERT_EQ(utils::from_ticks("17000000x"), epoch);
}

TEST(Utils, V44StampSetsOriginMtime) {
  const auto file = std::filesystem::temp_directory_path() / "stamp_test_file";
  {
    std::ofstream ofs(file);
    ofs << "Important file content\n";
  }
  // A file written now, given the time some other node wrote it: what the
  // receiver does to every file libtorrent hands it.
  const auto origin = utils::to_file_time(std::chrono::system_clock::now() -
                                          std::chrono::hours{3});
  ASSERT_NE(std::filesystem::last_write_time(file), origin);

  ASSERT_TRUE(utils::stamp(file, origin));
  ASSERT_EQ(utils::to_ticks(std::filesystem::last_write_time(file)),
            utils::to_ticks(origin));

  std::filesystem::remove(file);
}

TEST(Utils, StampReportsAMissingFile) {
  ASSERT_FALSE(
      utils::stamp(std::filesystem::temp_directory_path() /
                       "stamp_test_file_that_is_not_there",
                   utils::to_file_time(std::chrono::system_clock::now())));
}
