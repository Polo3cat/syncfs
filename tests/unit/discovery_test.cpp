#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <discovery.h>
#include <gtest/gtest.h>

namespace {
// Writes a peers file and hands back its path, inside a directory the test owns
// so nothing outside it is read or left behind.
class Peers : public ::testing::Test {
protected:
  Peers() : root{std::filesystem::temp_directory_path() / "discovery_test"} {
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
  }

  ~Peers() override {
    std::error_code err;
    std::filesystem::remove_all(root, err);
  }

  [[nodiscard]] auto written(std::string_view content) const
      -> std::filesystem::path {
    const auto path = root / "peers";
    std::ofstream out{path};
    out << content;
    return path;
  }

  std::filesystem::path root;

  Peers(Peers &) = delete;
  Peers(Peers &&) = delete;
  auto operator=(Peers &) -> Peers = delete;
  auto operator=(Peers &&) -> Peers = delete;
};
} // namespace

TEST_F(Peers, OneAddressPerLine) {
  const auto file = written("tcp://a:5555\ntcp://b:5556\n");

  const auto peers = discovery::parse(file);

  ASSERT_TRUE(peers.has_value()) << peers.error();
  ASSERT_EQ(*peers, (std::vector<std::string>{"tcp://a:5555", "tcp://b:5556"}));
}

TEST_F(Peers, ALastLineWithoutANewlineIsStillAPeer) {
  const auto file = written("tcp://a:5555");

  const auto peers = discovery::parse(file);

  ASSERT_TRUE(peers.has_value()) << peers.error();
  ASSERT_EQ(*peers, (std::vector<std::string>{"tcp://a:5555"}));
}

TEST_F(Peers, BlankLinesAreSkipped) {
  // An empty line read as an address reaches zmq::socket_t::connect as an empty
  // endpoint, which throws out of main: one stray newline in a mounted file
  // would stop the daemon from starting at all.
  const auto file = written("\ntcp://a:5555\n\n\ntcp://b:5556\n\n");

  const auto peers = discovery::parse(file);

  ASSERT_TRUE(peers.has_value()) << peers.error();
  ASSERT_EQ(*peers, (std::vector<std::string>{"tcp://a:5555", "tcp://b:5556"}));
}

TEST_F(Peers, ALineLongerThanTheLimitIsAnError) {
  // I fixes the line at 512 characters. Reading a longer one leaves a truncated
  // address behind and, worse, stops the parse where it stands: the daemon
  // would come up with the peers listed above the long line and none of the
  // ones below it, and nothing would say so.
  const auto file =
      written(std::string{"tcp://"} +
              std::string(discovery::max_line_len, 'a') + "\ntcp://b:5556\n");

  const auto peers = discovery::parse(file);

  ASSERT_FALSE(peers.has_value());
  ASSERT_NE(peers.error().find("Line longer than"), std::string::npos)
      << peers.error();
}

TEST_F(Peers, AMissingFileIsAnError) {
  // Not an empty peer list: a daemon started against a path that is not there
  // used to bind its socket, subscribe to nobody and report success (B5).
  const auto peers = discovery::parse(root / "not_here");

  ASSERT_FALSE(peers.has_value());
  ASSERT_NE(peers.error().find("Cannot read peers file"), std::string::npos)
      << peers.error();
}

TEST_F(Peers, ADirectoryIsAnError) {
  const auto peers = discovery::parse(root);

  ASSERT_FALSE(peers.has_value());
}

TEST_F(Peers, AnEmptyFileParsesToNoPeers) {
  // Readable and holding nothing is not a parse failure. Whether a daemon may
  // run with no peers at all is V2's question, answered in main.
  const auto file = written("");

  const auto peers = discovery::parse(file);

  ASSERT_TRUE(peers.has_value()) << peers.error();
  ASSERT_TRUE(peers->empty());
}
