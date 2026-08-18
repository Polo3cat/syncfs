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

#include <files.h>
#include <gtest/gtest.h>
#include <libtorrent/create_torrent.hpp>
#include <libtorrent/info_hash.hpp>
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
constexpr auto default_time = "1700000000000000000";

// Writes content to file, stamps it with the time the announcement for it
// will claim, and returns the bencoded v2-only torrent, the same shape
// source::create() puts on the wire. A real sender reads the origin time off
// the file it just hashed, so the two agreeing is not a convenience: a
// message that claims to be older than the file it describes is describing a
// file that does not exist.
auto torrent_buffer(const std::filesystem::path &file,
                    const std::string &content,
                    const std::string &origin = default_time,
                    const std::vector<std::pair<std::string, int>> &nodes = {})
    -> std::vector<char> {
  {
    std::ofstream ofs(file);
    ofs << content;
  }
  utils::stamp(file, utils::to_file_time(utils::from_ticks(origin)));
  const std::filesystem::directory_entry entry(file);
  auto file_entry = lt::create_file_entry{
      file.lexically_normal(), static_cast<int64_t>(entry.file_size())};
  lt::create_torrent torrent(std::vector{std::move(file_entry)}, 0,
                             lt::create_torrent::v2_only);
  // A sender embeds its own libtorrent endpoint as a DHT node, which is how the
  // swarm seeds its own DHT with no router to ask (C, V61, R2).
  for (const auto &node : nodes) {
    torrent.add_node(node);
  }
  // The entry holds the whole path relative to the sync root, so the hashes
  // are read from the root and not from the file's own parent (§V.39).
  lt::set_piece_hashes(torrent, ".");
  return torrent.generate_buf();
}

// A create as source::create() sends it: the torrent, the origin time and the
// listing key of the file it carries.
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

// A message of n parts under the given first part, to say something about the
// count alone without saying anything about the payload.
auto message_of(const std::string &part0, size_t parts)
    -> std::vector<zmq::message_t> {
  std::vector<zmq::message_t> message;
  message.emplace_back(std::string_view{part0});
  while (message.size() < parts) {
    message.emplace_back(std::string_view{"payload"});
  }
  return message;
}

// The same, under a first part addressed at one endpoint: the verb, the
// endpoint, and a NUL closing each.
auto addressed_message_of(const std::string &verb, const std::string &endpoint,
                          size_t parts) -> std::vector<zmq::message_t> {
  return message_of(utils::address(verb, utils::Endpoint{endpoint}), parts);
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
  protocol::applied_map_t applied;

  Protocol(Protocol &) = delete;
  Protocol(Protocol &&) = delete;
  auto operator=(Protocol &) -> Protocol = delete;
  auto operator=(Protocol &&) -> Protocol = delete;
};
} // namespace

TEST_F(Protocol, V8CreateAddsEveryDhtNodeItCarries) {
  // The peer set is static and there is no DHT router (C, V61), so the nodes an
  // announcement carries are the whole of what the receiving session learns
  // about where its peers are. The outcome names each one it registered, which
  // is also the only line a triage of a starved swarm has to read (B1).
  const auto buffer =
      torrent_buffer("./important_file", "content", default_time,
                     {{"10.0.0.1", 6881}, {"10.0.0.2", 6882}});
  auto session = offline_session();

  const auto outcome =
      protocol::act(create_message(buffer), session, tombstones, applied);

  ASSERT_TRUE(outcome.has_value()) << outcome.error();
  ASSERT_NE(outcome->message.find("10.0.0.1:6881"), std::string::npos)
      << outcome->message;
  ASSERT_NE(outcome->message.find("10.0.0.2:6882"), std::string::npos)
      << outcome->message;
  ASSERT_EQ(session.get_torrents().size(), 1U);
}

TEST_F(Protocol, V8CreateWithoutDhtNodesIsStillApplied) {
  // A repair rebuilt off disk carries whatever nodes its rebuilder embedded,
  // and an announcement with none is not malformed: the file still has to land.
  const auto buffer = torrent_buffer("./important_file", "content");
  auto session = offline_session();

  const auto outcome =
      protocol::act(create_message(buffer), session, tombstones, applied);

  ASSERT_TRUE(outcome.has_value()) << outcome.error();
  ASSERT_EQ(session.get_torrents().size(), 1U);
}

TEST_F(Protocol, V35NewTorrentReplacesSamePathTorrent) {
  auto session = offline_session();
  const auto file = std::filesystem::path{"important_file"};

  const auto first = torrent_buffer(file, "Important file content\n");
  ASSERT_TRUE(protocol::act(create_message(first), session, tombstones, applied)
                  .has_value());
  ASSERT_EQ(session.get_torrents().size(), 1);

  // A file update: same path, later, different content, therefore a different
  // info hash that add_torrent() would happily add alongside the first one.
  const auto later = "1700000000000000001";
  const auto second =
      torrent_buffer(file, "Different file content entirely\n", later);
  const auto expected = lt::load_torrent_buffer(second);
  ASSERT_TRUE(protocol::act(create_message(second, "./important_file", later),
                            session, tombstones, applied)
                  .has_value());

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
      protocol::act(create_message(buffer), session, tombstones, applied)
          .has_value());
  ASSERT_TRUE(
      protocol::act(create_message(buffer), session, tombstones, applied)
          .has_value());

  const auto torrents = session.get_torrents();
  ASSERT_EQ(torrents.size(), 1);
  ASSERT_EQ(torrents.front().info_hashes(), expected.ti->info_hashes());
}

TEST_F(Protocol, V38RemoveDropsTorrentForPath) {
  auto session = offline_session();
  const auto file = std::filesystem::path{"important_file"};

  const auto buffer = torrent_buffer(file, "Important file content\n");
  ASSERT_TRUE(
      protocol::act(create_message(buffer), session, tombstones, applied)
          .has_value());
  ASSERT_EQ(session.get_torrents().size(), 1);

  // The wire carries the key files::list() produced, which is the same path
  // the torrent holds normalized.
  ASSERT_TRUE(protocol::act(remove_message("./important_file"), session,
                            tombstones, applied)
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
  ASSERT_EQ(protocol::act(message_of("create", 2), session, tombstones, applied)
                .error(),
            "Wrong protocol length.");
  ASSERT_EQ(protocol::act(message_of("remove", 2), session, tombstones, applied)
                .error(),
            "Wrong protocol length.");
  // A state without its sender address is a hash nobody can answer.
  ASSERT_EQ(protocol::act(message_of("state", 2), session, tombstones, applied)
                .error(),
            "Wrong protocol length.");
  ASSERT_EQ(protocol::act(message_of("state", 4), session, tombstones, applied)
                .error(),
            "Wrong protocol length.");
  ASSERT_EQ(protocol::act(message_of("digest", 2), session, tombstones, applied)
                .error(),
            "Wrong protocol length.");
  ASSERT_EQ(protocol::act({}, session, tombstones, applied).error(),
            "Wrong protocol length.");

  // And the counts the interface fixes are accepted.
  ASSERT_TRUE(
      protocol::act(message_of("state", 3), session, tombstones, applied)
          .has_value());
  ASSERT_TRUE(
      protocol::act(message_of("digest", 4), session, tombstones, applied)
          .has_value());

  const auto file = std::filesystem::path{"important_file"};
  const auto buffer = torrent_buffer(file, "Important file content\n");
  ASSERT_TRUE(
      protocol::act(create_message(buffer), session, tombstones, applied)
          .has_value());
  ASSERT_TRUE(protocol::act(remove_message("./important_file"), session,
                            tombstones, applied)
                  .has_value());
}

TEST_F(Protocol, V4UnknownVerbIsRejected) {
  auto session = offline_session();

  ASSERT_EQ(protocol::act(message_of("bogus", 2), session, tombstones, applied)
                .error(),
            "Wrong protocol verb.");
  // The count is right for a create, the verb still is not.
  ASSERT_EQ(protocol::act(message_of("update", 4), session, tombstones, applied)
                .error(),
            "Wrong protocol verb.");
  // The verb is judged before the count, so a known verb never reports this.
  ASSERT_EQ(protocol::act(message_of("state", 2), session, tombstones, applied)
                .error(),
            "Wrong protocol length.");
}

TEST_F(Protocol, V58AddressedDigestIsReadAsItsVerb) {
  auto session = offline_session();

  // The first part of an addressed digest is the verb and the endpoint it is
  // meant for. Comparing the whole of it against the verb table finds nothing,
  // so every digest a peer addressed here would be thrown away as a verb nobody
  // knows, and the reconciliation would never exchange anything.
  const auto addressed =
      addressed_message_of("digest", "tcp://127.0.0.1:5555", 4);
  const auto r = protocol::act(addressed, session, tombstones, applied);
  ASSERT_TRUE(r.has_value());
  ASSERT_EQ(r->verb, "digest");

  // The count is still the verb's own, and still judged.
  ASSERT_EQ(
      protocol::act(addressed_message_of("digest", "tcp://127.0.0.1:5555", 3),
                    session, tombstones, applied)
          .error(),
      "Wrong protocol length.");
}

TEST_F(Protocol, V58UnknownVerbBeforeTheNulIsStillRejected) {
  auto session = offline_session();

  // Splitting the first part is not a licence to accept whatever stands before
  // the NUL: what the address buys is that the verb can be found at all.
  ASSERT_EQ(
      protocol::act(addressed_message_of("bogus", "tcp://127.0.0.1:5555", 4),
                    session, tombstones, applied)
          .error(),
      "Wrong protocol verb.");
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

  ASSERT_TRUE(protocol::act(create_message(buffer, "./a/f.txt"), session,
                            tombstones, applied)
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
  ASSERT_TRUE(protocol::act(create_message(buffer, "./a/f.txt"), session,
                            tombstones, applied)
                  .has_value());
  ASSERT_EQ(session.get_torrents().size(), 1);

  // The directory lives in the save path now, so finding the torrent again
  // means putting the save path and what the torrent still knows back
  // together.
  ASSERT_TRUE(
      protocol::act(remove_message("./a/f.txt"), session, tombstones, applied)
          .has_value());
  ASSERT_TRUE(session.get_torrents().empty());
  ASSERT_FALSE(std::filesystem::exists(file));
}

TEST_F(Protocol, V35OlderCreateIsNotApplied) {
  auto session = offline_session();
  const auto file = std::filesystem::path{"important_file"};

  const auto here = torrent_buffer(file, "The copy this node holds\n");
  ASSERT_TRUE(protocol::act(create_message(here), session, tombstones, applied)
                  .has_value());
  const auto expected = lt::load_torrent_buffer(here);

  // Under repair the same path is announced over and over. Applying whichever
  // arrived last would leave two nodes flipping each other's copy every round
  // for ever, so only a newer one is applied.
  const auto older = "1699999999999999999";
  const auto stale = torrent_buffer(std::filesystem::path{"stale_source"},
                                    "An older copy of it\n", older);
  const auto ignored =
      protocol::act(create_message(stale, "./important_file", older), session,
                    tombstones, applied);

  ASSERT_TRUE(ignored.has_value());
  ASSERT_FALSE(ignored->created.has_value());
  const auto torrents = session.get_torrents();
  ASSERT_EQ(torrents.size(), 1);
  ASSERT_EQ(torrents.front().info_hashes(), expected.ti->info_hashes());
}

TEST_F(Protocol, V35TieBreaksOnLowerInfoHash) {
  const auto file = std::filesystem::path{"important_file"};
  // The same length, so that adding one of them never resizes the file and
  // moves the very timestamp the case turns on.
  const auto one = torrent_buffer(file, "One copy of the content\n");
  const auto other = torrent_buffer(file, "Two copy of the content\n");

  // The two claim the same instant, so nothing about when they happened tells
  // them apart. Content does, and content is all either end can read out of
  // the message: neither create carries a sender.
  const auto lower = lt::load_torrent_buffer(one).ti->info_hashes() <
                             lt::load_torrent_buffer(other).ti->info_hashes()
                         ? one
                         : other;
  const auto higher = lower == one ? other : one;

  // What the session is left holding, and whether the second announcement is
  // what put it there. Read out before the session goes, since a handle
  // outliving one answers for nothing.
  struct settled_t {
    bool applied;
    size_t torrents;
    lt::info_hash_t held;
  };
  auto second_after_first = [&file](const std::vector<char> &first,
                                    const std::vector<char> &second) {
    auto session = offline_session();
    auto graves = reconcile::tombstone_map_t{};
    auto seen = protocol::applied_map_t{};
    const auto held =
        protocol::act(create_message(first), session, graves, seen);
    seen.insert_or_assign(
        *held->created,
        protocol::applied_t{.origin = held->origin, .content = held->content});
    utils::stamp(file, utils::to_file_time(utils::from_ticks(default_time)));
    const auto answer =
        protocol::act(create_message(second), session, graves, seen);
    const auto torrents = session.get_torrents();
    return settled_t{.applied =
                         answer.has_value() && answer->created.has_value(),
                     .torrents = torrents.size(),
                     .held = torrents.empty() ? lt::info_hash_t{}
                                              : torrents.front().info_hashes()};
  };

  const auto expected = lt::load_torrent_buffer(lower).ti->info_hashes();

  const auto lower_wins = second_after_first(higher, lower);
  ASSERT_TRUE(lower_wins.applied);
  ASSERT_EQ(lower_wins.torrents, 1);
  ASSERT_EQ(lower_wins.held, expected);

  const auto higher_loses = second_after_first(lower, higher);
  ASSERT_FALSE(higher_loses.applied);
  ASSERT_EQ(higher_loses.torrents, 1);
  ASSERT_EQ(higher_loses.held, expected);
}

TEST_F(Protocol, V35IdenticalRepairIsStillApplied) {
  auto session = offline_session();
  const auto file = std::filesystem::path{"important_file"};
  const auto buffer = torrent_buffer(file, "Important file content\n");

  // A node subscribes to itself, so its own announcement comes back to it and
  // is what makes it seed the file at all. It ties on time and on content,
  // and a tie it cannot lose to itself.
  const auto first =
      protocol::act(create_message(buffer), session, tombstones, applied);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(first->created.has_value());
  ASSERT_EQ(session.get_torrents().size(), 1);
  applied.insert_or_assign(
      *first->created,
      protocol::applied_t{.origin = first->origin, .content = first->content});

  const auto again =
      protocol::act(create_message(buffer), session, tombstones, applied);
  ASSERT_TRUE(again.has_value());
  ASSERT_TRUE(again->created.has_value());
  ASSERT_EQ(session.get_torrents().size(), 1);
}

TEST_F(Protocol, V37RemoveRecordsTombstoneForAPathNeverHeld) {
  auto session = offline_session();

  // No file, no torrent: nothing to delete. The deletion is still recorded,
  // because a peer that still holds the path can only be repaired by a node
  // that remembers it was deleted.
  ASSERT_TRUE(protocol::act(remove_message("./never_held"), session, tombstones,
                            applied)
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
                    session, tombstones, applied)
          .has_value());

  const auto ignored =
      protocol::act(create_message(buffer), session, tombstones, applied);
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
                    session, tombstones, applied)
          .has_value());
  ASSERT_TRUE(
      protocol::act(create_message(buffer), session, tombstones, applied)
          .has_value());

  ASSERT_EQ(session.get_torrents().size(), 1);
  ASSERT_FALSE(tombstones.contains("./important_file"));
}

TEST_F(Protocol, V37TieKeepsTheFile) {
  auto session = offline_session();
  const auto file = std::filesystem::path{"important_file"};
  const auto buffer = torrent_buffer(file, "Important file content\n");

  // Deleted at the very moment it was written. The unresolvable case biases
  // towards keeping data.
  ASSERT_TRUE(protocol::act(remove_message("./important_file"), session,
                            tombstones, applied)
                  .has_value());
  ASSERT_TRUE(
      protocol::act(create_message(buffer), session, tombstones, applied)
          .has_value());

  ASSERT_EQ(session.get_torrents().size(), 1);
  ASSERT_FALSE(tombstones.contains("./important_file"));
}

TEST_F(Protocol, V57AdoptionForgetsEveryTraceOfAPath) {
  auto session = offline_session();
  const auto file = std::filesystem::path{"important_file"};
  const auto key = std::filesystem::path{"./important_file"};
  const auto buffer = torrent_buffer(file, "Important file content\n");

  const auto held =
      protocol::act(create_message(buffer), session, tombstones, applied);
  ASSERT_TRUE(held.has_value());
  ASSERT_TRUE(held->created.has_value());

  // Everything receive_one() records for an applied create, so that what
  // adoption is asked to clean up is what the daemon would actually be holding.
  auto former = files::file_map_t{
      {key, utils::to_file_time(utils::from_ticks(default_time))}};
  auto repairs = protocol::repairs_t{};
  repairs.announcements.insert_or_assign(
      key, std::string{buffer.data(), buffer.size()});
  repairs.applied.insert_or_assign(
      key,
      protocol::applied_t{.origin = held->origin, .content = held->content});
  repairs.pending.insert_or_assign(key, std::chrono::steady_clock::now());

  // A peer's digest carries a deletion this node never heard, one tick newer
  // than the copy it holds, so the file loses and goes. Read a tick after that,
  // since a deletion already past the time to live here is not one to take on.
  const auto theirs = reconcile::tombstone_map_t{
      {key, utils::from_ticks("1700000000000000001")}};
  const auto now = utils::from_ticks("1700000000000000002");

  const auto deleted =
      protocol::adopt(session, theirs, former, tombstones, repairs, now);

  ASSERT_EQ(deleted, std::vector<std::filesystem::path>{key});
  ASSERT_TRUE(former.empty());
  ASSERT_TRUE(session.get_torrents().empty());
  ASSERT_FALSE(std::filesystem::exists(file));
  ASSERT_TRUE(tombstones.contains(key));
  // The path is gone from the listing, so nothing will ever read what the
  // repair cache still remembers it by and nothing would ever drop it either:
  // a bencoded torrent leaked for the life of the process.
  ASSERT_FALSE(repairs.announcements.contains(key));
  ASSERT_FALSE(repairs.applied.contains(key));
  ASSERT_FALSE(repairs.pending.contains(key));
}

TEST_F(Protocol, V57AdoptionLeavesAPathItDidNotDeleteAlone) {
  auto session = offline_session();
  const auto key = std::filesystem::path{"./important_file"};

  // A deletion this node has already got is not adopted, so nothing about the
  // path changes and its repair state is not something to throw away.
  auto former = files::file_map_t{
      {key, utils::to_file_time(utils::from_ticks(default_time))}};
  auto repairs = protocol::repairs_t{};
  repairs.announcements.insert_or_assign(key, "bencoded");
  reconcile::mark(tombstones, key, utils::from_ticks(default_time));

  const auto theirs =
      reconcile::tombstone_map_t{{key, utils::from_ticks(default_time)}};
  const auto deleted =
      protocol::adopt(session, theirs, former, tombstones, repairs,
                      utils::from_ticks(default_time));

  ASSERT_TRUE(deleted.empty());
  ASSERT_TRUE(former.contains(key));
  ASSERT_TRUE(repairs.announcements.contains(key));
}

TEST_F(Protocol, V7AddedTorrentSavePathAndFlags) {
  auto session = offline_session();
  const auto file = std::filesystem::path{"important_file"};

  const auto buffer = torrent_buffer(file, "Important file content\n");
  ASSERT_TRUE(
      protocol::act(create_message(buffer), session, tombstones, applied)
          .has_value());

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
