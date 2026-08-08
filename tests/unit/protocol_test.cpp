#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
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

auto create_message(const std::vector<char> &buffer)
    -> std::vector<zmq::message_t> {
  std::vector<zmq::message_t> parts;
  parts.emplace_back(std::string_view{"create"});
  parts.emplace_back(buffer.data(), buffer.size());
  return parts;
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
  std::vector<zmq::message_t> remove_message;
  remove_message.emplace_back(std::string_view{"remove"});
  remove_message.emplace_back(std::string_view{"./important_file"});
  ASSERT_TRUE(protocol::act(remove_message, session).has_value());

  ASSERT_TRUE(session.get_torrents().empty());
  ASSERT_FALSE(std::filesystem::exists(file));
}

TEST_F(Protocol, RemovedPathReadsRemoveMessages) {
  std::vector<zmq::message_t> remove_message;
  remove_message.emplace_back(std::string_view{"remove"});
  remove_message.emplace_back(std::string_view{"./a/important_file"});

  const auto path = protocol::removed_path(remove_message);
  ASSERT_TRUE(path.has_value());
  ASSERT_EQ(*path, std::filesystem::path{"./a/important_file"});

  std::vector<zmq::message_t> create_message;
  create_message.emplace_back(std::string_view{"create"});
  create_message.emplace_back(std::string_view{"bencoded torrent"});
  ASSERT_FALSE(protocol::removed_path(create_message).has_value());

  std::vector<zmq::message_t> truncated;
  truncated.emplace_back(std::string_view{"remove"});
  ASSERT_FALSE(protocol::removed_path(truncated).has_value());
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
