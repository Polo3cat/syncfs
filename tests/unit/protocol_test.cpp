#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <libtorrent/create_torrent.hpp>
#include <libtorrent/load_torrent.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/torrent_flags.hpp>
#include <libtorrent/torrent_info.hpp>
#include <protocol.h>
#include <zmq.hpp>

namespace {
// Writes content to file and returns the bencoded v2-only torrent for it, the
// same shape source::create() puts on the wire.
auto torrent_buffer(const std::filesystem::path &file,
                    const std::string &content) -> std::vector<char> {
  {
    std::ofstream ofs(file);
    ofs << content;
  }
  const std::filesystem::directory_entry entry(file);
  auto file_entry = lt::create_file_entry{
      file.lexically_normal(), static_cast<int64_t>(entry.file_size())};
  lt::create_torrent torrent(std::vector{std::move(file_entry)}, 0,
                             lt::create_torrent::v2_only);
  lt::set_piece_hashes(torrent, file.parent_path());
  return torrent.generate_buf();
}

// A create as source::create() sends it: the torrent, the origin time and the
// listing key of the file it carries.
auto create_message(const std::vector<char> &buffer,
                    const std::string &path = "./important_file")
    -> std::vector<zmq::message_t> {
  std::vector<zmq::message_t> parts;
  parts.emplace_back(std::string_view{"create"});
  parts.emplace_back(buffer.data(), buffer.size());
  parts.emplace_back(std::string_view{"1700000000000000000"});
  parts.emplace_back(std::string_view{path});
  return parts;
}

auto remove_message(const std::string &path) -> std::vector<zmq::message_t> {
  std::vector<zmq::message_t> parts;
  parts.emplace_back(std::string_view{"remove"});
  parts.emplace_back(std::string_view{path});
  parts.emplace_back(std::string_view{"1700000000000000000"});
  return parts;
}

// A message of n parts under the given verb, to say something about the count
// alone without saying anything about the payload.
auto message_of(const std::string &verb, size_t parts)
    -> std::vector<zmq::message_t> {
  std::vector<zmq::message_t> message;
  message.emplace_back(std::string_view{verb});
  while (message.size() < parts) {
    message.emplace_back(std::string_view{"payload"});
  }
  return message;
}

// A session that touches no network: the unit tests only care about what the
// torrent list looks like after protocol::act().
auto offline_session() -> lt::session {
  lt::settings_pack pack;
  pack.set_bool(lt::settings_pack::enable_dht, false);
  pack.set_bool(lt::settings_pack::enable_lsd, false);
  pack.set_str(lt::settings_pack::listen_interfaces, "127.0.0.1:0");
  pack.set_int(lt::settings_pack::alert_mask, 0);
  return lt::session{pack};
}

// Runs each test with the working directory moved into a throwaway root, so
// the save path protocol::act() sets (".") stays inside it.
class Protocol : public ::testing::Test {
protected:
  Protocol()
      : previous{std::filesystem::current_path()},
        root{std::filesystem::temp_directory_path() / "protocol_test_root"} {
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    std::filesystem::current_path(root);
  }

  ~Protocol() override {
    std::error_code err;
    std::filesystem::current_path(previous, err);
    std::filesystem::remove_all(root, err);
  }

  std::filesystem::path previous;
  std::filesystem::path root;

  Protocol(Protocol &) = delete;
  Protocol(Protocol &&) = delete;
  auto operator=(Protocol &) -> Protocol = delete;
  auto operator=(Protocol &&) -> Protocol = delete;
};
} // namespace

TEST_F(Protocol, V35NewTorrentReplacesSamePathTorrent) {
  auto session = offline_session();
  const auto file = std::filesystem::path{"important_file"};

  const auto first = torrent_buffer(file, "Important file content\n");
  ASSERT_TRUE(protocol::act(create_message(first), session).has_value());
  ASSERT_EQ(session.get_torrents().size(), 1);

  // A file update: same path, different content, therefore a different info
  // hash that add_torrent() would happily add alongside the first one.
  const auto second = torrent_buffer(file, "Different file content entirely\n");
  const auto expected = lt::load_torrent_buffer(second);
  ASSERT_TRUE(protocol::act(create_message(second), session).has_value());

  const auto torrents = session.get_torrents();
  ASSERT_EQ(torrents.size(), 1);
  ASSERT_EQ(torrents.front().info_hashes(), expected.ti->info_hashes());
}

TEST_F(Protocol, V35IdenticalTorrentIsNotReadded) {
  auto session = offline_session();
  const auto file = std::filesystem::path{"important_file"};

  const auto buffer = torrent_buffer(file, "Important file content\n");
  const auto expected = lt::load_torrent_buffer(buffer);

  ASSERT_TRUE(protocol::act(create_message(buffer), session).has_value());
  ASSERT_TRUE(protocol::act(create_message(buffer), session).has_value());

  const auto torrents = session.get_torrents();
  ASSERT_EQ(torrents.size(), 1);
  ASSERT_EQ(torrents.front().info_hashes(), expected.ti->info_hashes());
}

TEST_F(Protocol, V38RemoveDropsTorrentForPath) {
  auto session = offline_session();
  const auto file = std::filesystem::path{"important_file"};

  const auto buffer = torrent_buffer(file, "Important file content\n");
  ASSERT_TRUE(protocol::act(create_message(buffer), session).has_value());
  ASSERT_EQ(session.get_torrents().size(), 1);

  // The wire carries the key files::list() produced, which is the same path
  // the torrent holds normalized.
  ASSERT_TRUE(
      protocol::act(remove_message("./important_file"), session).has_value());

  ASSERT_TRUE(session.get_torrents().empty());
  ASSERT_FALSE(std::filesystem::exists(file));
}

TEST_F(Protocol, RemovedPathReadsRemoveMessages) {
  const auto path =
      protocol::removed_path(remove_message("./a/important_file"));
  ASSERT_TRUE(path.has_value());
  ASSERT_EQ(*path, std::filesystem::path{"./a/important_file"});

  ASSERT_FALSE(protocol::removed_path(message_of("create", 4)).has_value());
  ASSERT_FALSE(protocol::removed_path(message_of("remove", 2)).has_value());
}

TEST_F(Protocol, V3PartCountIsPerVerb) {
  auto session = offline_session();

  // A create truncated to its torrent used to be a well formed message.
  ASSERT_EQ(protocol::act(message_of("create", 2), session).error(),
            "Wrong protocol length.");
  ASSERT_EQ(protocol::act(message_of("remove", 2), session).error(),
            "Wrong protocol length.");
  ASSERT_EQ(protocol::act(message_of("state", 4), session).error(),
            "Wrong protocol length.");
  ASSERT_EQ(protocol::act(message_of("digest", 2), session).error(),
            "Wrong protocol length.");
  ASSERT_EQ(protocol::act({}, session).error(), "Wrong protocol length.");

  // And the counts the interface fixes are accepted.
  ASSERT_TRUE(protocol::act(message_of("state", 2), session).has_value());
  ASSERT_TRUE(protocol::act(message_of("digest", 4), session).has_value());

  const auto file = std::filesystem::path{"important_file"};
  const auto buffer = torrent_buffer(file, "Important file content\n");
  ASSERT_TRUE(protocol::act(create_message(buffer), session).has_value());
  ASSERT_TRUE(
      protocol::act(remove_message("./important_file"), session).has_value());
}

TEST_F(Protocol, V4UnknownVerbIsRejected) {
  auto session = offline_session();

  ASSERT_EQ(protocol::act(message_of("bogus", 2), session).error(),
            "Wrong protocol verb.");
  // The count is right for a create, the verb still is not.
  ASSERT_EQ(protocol::act(message_of("update", 4), session).error(),
            "Wrong protocol verb.");
  // The verb is judged before the count, so a known verb never reports this.
  ASSERT_EQ(protocol::act(message_of("state", 3), session).error(),
            "Wrong protocol length.");
}

TEST_F(Protocol, V7AddedTorrentSavePathAndFlags) {
  auto session = offline_session();
  const auto file = std::filesystem::path{"important_file"};

  const auto buffer = torrent_buffer(file, "Important file content\n");
  ASSERT_TRUE(protocol::act(create_message(buffer), session).has_value());

  const auto torrents = session.get_torrents();
  ASSERT_EQ(torrents.size(), 1);
  // protocol::act() sets ".", which libtorrent resolves against the working
  // directory when the torrent is added.
  const auto status = torrents.front().status();
  ASSERT_EQ(std::filesystem::path{status.save_path},
            std::filesystem::current_path());
  ASSERT_TRUE((status.flags & lt::torrent_flags::auto_managed) ==
              lt::torrent_flags::auto_managed);
}
