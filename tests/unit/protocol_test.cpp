#include <chrono>
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
#include <reconcile.h>
#include <utils.h>
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
  // The entry holds the whole path relative to the sync root, so the hashes
  // are read from the root and not from the file's own parent (§V.39).
  lt::set_piece_hashes(torrent, ".");
  return torrent.generate_buf();
}

// A create as source::create() sends it: the torrent, the origin time and the
// listing key of the file it carries.
constexpr auto default_time = "1700000000000000000";

auto create_message(const std::vector<char> &buffer,
                    const std::string &path = "./important_file",
                    const std::string &origin = default_time)
    -> std::vector<zmq::message_t> {
  std::vector<zmq::message_t> parts;
  parts.emplace_back(std::string_view{"create"});
  parts.emplace_back(buffer.data(), buffer.size());
  parts.emplace_back(std::string_view{origin});
  parts.emplace_back(std::string_view{path});
  return parts;
}

auto remove_message(const std::string &path,
                    const std::string &deleted = default_time)
    -> std::vector<zmq::message_t> {
  std::vector<zmq::message_t> parts;
  parts.emplace_back(std::string_view{"remove"});
  parts.emplace_back(std::string_view{path});
  parts.emplace_back(std::string_view{deleted});
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
  // Every act() writes and reads these, so they belong to the run and not to
  // any one call.
  reconcile::tombstone_map_t tombstones;

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
  ASSERT_TRUE(
      protocol::act(create_message(first), session, tombstones).has_value());
  ASSERT_EQ(session.get_torrents().size(), 1);

  // A file update: same path, different content, therefore a different info
  // hash that add_torrent() would happily add alongside the first one.
  const auto second = torrent_buffer(file, "Different file content entirely\n");
  const auto expected = lt::load_torrent_buffer(second);
  ASSERT_TRUE(
      protocol::act(create_message(second), session, tombstones).has_value());

  const auto torrents = session.get_torrents();
  ASSERT_EQ(torrents.size(), 1);
  ASSERT_EQ(torrents.front().info_hashes(), expected.ti->info_hashes());
}

TEST_F(Protocol, V35IdenticalTorrentIsNotReadded) {
  auto session = offline_session();
  const auto file = std::filesystem::path{"important_file"};

  const auto buffer = torrent_buffer(file, "Important file content\n");
  const auto expected = lt::load_torrent_buffer(buffer);

  ASSERT_TRUE(
      protocol::act(create_message(buffer), session, tombstones).has_value());
  ASSERT_TRUE(
      protocol::act(create_message(buffer), session, tombstones).has_value());

  const auto torrents = session.get_torrents();
  ASSERT_EQ(torrents.size(), 1);
  ASSERT_EQ(torrents.front().info_hashes(), expected.ti->info_hashes());
}

TEST_F(Protocol, V38RemoveDropsTorrentForPath) {
  auto session = offline_session();
  const auto file = std::filesystem::path{"important_file"};

  const auto buffer = torrent_buffer(file, "Important file content\n");
  ASSERT_TRUE(
      protocol::act(create_message(buffer), session, tombstones).has_value());
  ASSERT_EQ(session.get_torrents().size(), 1);

  // The wire carries the key files::list() produced, which is the same path
  // the torrent holds normalized.
  ASSERT_TRUE(
      protocol::act(remove_message("./important_file"), session, tombstones)
          .has_value());

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
  ASSERT_EQ(protocol::act(message_of("create", 2), session, tombstones).error(),
            "Wrong protocol length.");
  ASSERT_EQ(protocol::act(message_of("remove", 2), session, tombstones).error(),
            "Wrong protocol length.");
  ASSERT_EQ(protocol::act(message_of("state", 4), session, tombstones).error(),
            "Wrong protocol length.");
  ASSERT_EQ(protocol::act(message_of("digest", 2), session, tombstones).error(),
            "Wrong protocol length.");
  ASSERT_EQ(protocol::act({}, session, tombstones).error(),
            "Wrong protocol length.");

  // And the counts the interface fixes are accepted.
  ASSERT_TRUE(
      protocol::act(message_of("state", 2), session, tombstones).has_value());
  ASSERT_TRUE(
      protocol::act(message_of("digest", 4), session, tombstones).has_value());

  const auto file = std::filesystem::path{"important_file"};
  const auto buffer = torrent_buffer(file, "Important file content\n");
  ASSERT_TRUE(
      protocol::act(create_message(buffer), session, tombstones).has_value());
  ASSERT_TRUE(
      protocol::act(remove_message("./important_file"), session, tombstones)
          .has_value());
}

TEST_F(Protocol, V4UnknownVerbIsRejected) {
  auto session = offline_session();

  ASSERT_EQ(protocol::act(message_of("bogus", 2), session, tombstones).error(),
            "Wrong protocol verb.");
  // The count is right for a create, the verb still is not.
  ASSERT_EQ(protocol::act(message_of("update", 4), session, tombstones).error(),
            "Wrong protocol verb.");
  // The verb is judged before the count, so a known verb never reports this.
  ASSERT_EQ(protocol::act(message_of("state", 3), session, tombstones).error(),
            "Wrong protocol length.");
}

TEST_F(Protocol, V25SavePathRestoresLostDirectory) {
  auto session = offline_session();
  const auto file = std::filesystem::path{"a"} / "f.txt";
  std::filesystem::create_directories(file.parent_path());

  const auto buffer = torrent_buffer(file, "Important file content\n");
  // A v2-only torrent holding one file under one directory loses that
  // directory on load, so the torrent alone would write the file to "./f.txt".
  const auto loaded = lt::load_torrent_buffer(buffer);
  ASSERT_EQ(loaded.ti->layout().file_path(
                *(loaded.ti->layout().file_range().begin())),
            "f.txt");

  ASSERT_TRUE(
      protocol::act(create_message(buffer, "./a/f.txt"), session, tombstones)
          .has_value());

  const auto torrents = session.get_torrents();
  ASSERT_EQ(torrents.size(), 1);
  ASSERT_EQ(std::filesystem::path{torrents.front().status().save_path},
            std::filesystem::current_path() / "a");
}

TEST_F(Protocol, V25TorrentIsFoundUnderTheAnnouncedPath) {
  auto session = offline_session();
  const auto file = std::filesystem::path{"a"} / "f.txt";
  std::filesystem::create_directories(file.parent_path());

  const auto buffer = torrent_buffer(file, "Important file content\n");
  ASSERT_TRUE(
      protocol::act(create_message(buffer, "./a/f.txt"), session, tombstones)
          .has_value());
  ASSERT_EQ(session.get_torrents().size(), 1);

  // The directory lives in the save path now, so finding the torrent again
  // means putting the save path and what the torrent still knows back
  // together.
  ASSERT_TRUE(protocol::act(remove_message("./a/f.txt"), session, tombstones)
                  .has_value());
  ASSERT_TRUE(session.get_torrents().empty());
  ASSERT_FALSE(std::filesystem::exists(file));
}

TEST_F(Protocol, V37RemoveRecordsTombstoneForAPathNeverHeld) {
  auto session = offline_session();

  // No file, no torrent: nothing to delete. The deletion is still recorded,
  // because a peer that still holds the path can only be repaired by a node
  // that remembers it was deleted.
  ASSERT_TRUE(protocol::act(remove_message("./never_held"), session, tombstones)
                  .has_value());
  ASSERT_EQ(tombstones.at("./never_held"), utils::from_ticks(default_time));
}

TEST_F(Protocol, V49CreateBeatenByTombstoneIsNotApplied) {
  auto session = offline_session();
  const auto file = std::filesystem::path{"important_file"};
  const auto buffer = torrent_buffer(file, "Important file content\n");
  std::filesystem::remove(file);

  // Deleted one tick after the file was written, so the deletion wins.
  ASSERT_TRUE(
      protocol::act(remove_message("./important_file", "1700000000000000001"),
                    session, tombstones)
          .has_value());

  const auto ignored =
      protocol::act(create_message(buffer), session, tombstones);
  ASSERT_TRUE(ignored.has_value());
  ASSERT_TRUE(session.get_torrents().empty());
  ASSERT_FALSE(std::filesystem::exists(file));
  ASSERT_FALSE(ignored->created.has_value());
  // And the deletion still stands, so the next announcement of that file
  // loses to it as well.
  ASSERT_TRUE(tombstones.contains("./important_file"));
}

TEST_F(Protocol, V37NewerCreateDefeatsTheTombstone) {
  auto session = offline_session();
  const auto file = std::filesystem::path{"important_file"};
  const auto buffer = torrent_buffer(file, "Important file content\n");

  // Deleted one tick before the file was written: the file is the newer of
  // the two and the deletion is spent.
  ASSERT_TRUE(
      protocol::act(remove_message("./important_file", "1699999999999999999"),
                    session, tombstones)
          .has_value());
  ASSERT_TRUE(
      protocol::act(create_message(buffer), session, tombstones).has_value());

  ASSERT_EQ(session.get_torrents().size(), 1);
  ASSERT_FALSE(tombstones.contains("./important_file"));
}

TEST_F(Protocol, V37TieKeepsTheFile) {
  auto session = offline_session();
  const auto file = std::filesystem::path{"important_file"};
  const auto buffer = torrent_buffer(file, "Important file content\n");

  // Deleted at the very moment it was written. The unresolvable case biases
  // towards keeping data.
  ASSERT_TRUE(
      protocol::act(remove_message("./important_file"), session, tombstones)
          .has_value());
  ASSERT_TRUE(
      protocol::act(create_message(buffer), session, tombstones).has_value());

  ASSERT_EQ(session.get_torrents().size(), 1);
  ASSERT_FALSE(tombstones.contains("./important_file"));
}

TEST_F(Protocol, V7AddedTorrentSavePathAndFlags) {
  auto session = offline_session();
  const auto file = std::filesystem::path{"important_file"};

  const auto buffer = torrent_buffer(file, "Important file content\n");
  ASSERT_TRUE(
      protocol::act(create_message(buffer), session, tombstones).has_value());

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
