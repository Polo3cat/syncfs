#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <stdexcept>
#include <string>
#include <string_view>
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

TEST(Utils, V58AddressFramesVerbAndEndpoint) {
  using namespace std::string_view_literals;

  // Byte exact, both NULs included. A subscriber asks for the whole of this and
  // the publisher matches it against the first part of every message, so the
  // two ends have to spell it identically or an addressed message reaches
  // nobody.
  ASSERT_EQ(utils::address("digest", utils::Endpoint{"tcp://127.0.0.1:5555"}),
            "digest\0tcp://127.0.0.1:5555\0"sv);

  // And the trailing one is what the framing is for: without it the shorter
  // port is a prefix of the longer and the node on 555 receives everything
  // meant for the node on 5555.
  const auto shorter =
      utils::address("digest", utils::Endpoint{"tcp://127.0.0.1:555"});
  const auto longer =
      utils::address("digest", utils::Endpoint{"tcp://127.0.0.1:5555"});
  ASSERT_FALSE(std::string_view{longer}.starts_with(shorter));
}

TEST(Utils, V58VerbIsWhatStandsBeforeTheFirstNul) {
  using namespace std::string_view_literals;

  ASSERT_EQ(utils::verb_of("digest\0tcp://127.0.0.1:5555\0"sv), "digest"sv);
  // A broadcast verb is spelled plainly, and the same reading has to answer for
  // it: the part count is looked up once, whichever form arrived.
  ASSERT_EQ(utils::verb_of("create"sv), "create"sv);
  ASSERT_EQ(utils::verb_of(""sv), ""sv);
  // An address whose endpoint is empty still names its verb, and the verb is
  // all the count depends on.
  ASSERT_EQ(utils::verb_of("state\0\0"sv), "state"sv);
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
