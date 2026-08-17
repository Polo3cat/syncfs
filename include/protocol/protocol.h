#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <libtorrent/info_hash.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <zmq.hpp>

#include <files.h>
#include <reconcile.h>

namespace protocol {

// Every verb fixes its own part count and this is the largest of them, which
// is all a receiver needs to know before it has read the verb.
inline constexpr size_t max_parts = 4;

// What this node asks the wire for. The data verbs and the root hash are
// broadcast and subscribed plainly; a digest is addressed at one node, so what
// is subscribed is the whole framed prefix and no other node's digest ever
// reaches this socket at all (V58).
void subscribe(zmq::socket_t &s, std::string_view endpoint);

// What act() did, for a caller that has to follow up on it. The verb comes
// back because the reconciliation reads state and digest for itself, and the
// created path because a received file still needs the time it was written at
// its origin put back on it once libtorrent is done writing it (V44).
struct outcome {
  std::string verb;
  std::string message;
  std::optional<std::filesystem::path> created;
  std::filesystem::file_time_type origin{};
  lt::info_hash_t content;
};

// What this node last applied for a path: the moment the copy it holds was
// written at its origin, and what that copy's content hashes to. The session
// cannot answer this. Between a local edit and the moment that edit's own
// announcement comes back round, the torrent for the path is still the one
// for the content before it, and comparing against that would have a node
// refuse its own edit.
struct applied_t {
  std::filesystem::file_time_type origin;
  lt::info_hash_t content;
};

using applied_map_t = std::map<std::filesystem::path, applied_t>;

// Everything this node remembers a path by for the sake of repairing a peer
// with it: the repairs it has taken on, what it last applied for each path, and
// the announcement each path was last seen with. They are one struct because
// they have one lifetime: a path that leaves the listing has to leave all three
// at once, and three maps cleaned up in three places is three chances to leak.
struct repairs_t {
  reconcile::pending_map_t pending;
  // What this node last applied for each path, which is what an announcement
  // for that path is judged against.
  applied_map_t applied;
  // The announcement each path was last seen with, which is what a repair says
  // again. The session cannot rebuild it, and reading the file to hash it
  // afresh is the cost a repair exists to avoid.
  std::map<std::filesystem::path, std::string> announcements;

  // Drops every trace of a path. Called wherever the path leaves the listing,
  // whether a peer asked for the deletion or its digest only implied it: an
  // announcement left behind by a path that is gone is one nothing will ever
  // erase (V57).
  void forget(const std::filesystem::path &path);
};

// The tombstones travel in because a create and a remove are the two things
// that write them: a deletion this node is told about has to be recorded, and
// a file that arrives newer than a deletion cancels it.
auto act(const std::vector<zmq::message_t> &v, lt::session &s,
         reconcile::tombstone_map_t &tombstones, const applied_map_t &applied)
    -> std::expected<outcome, std::string>;

// Where a torrent's single file sits inside the sync root. A v2-only torrent
// holding one file under one directory drops that directory on load, so the
// torrent alone no longer names the file and the save path has to be read
// back with it.
auto held_path(const lt::torrent_handle &h)
    -> std::optional<std::filesystem::path>;

// Takes a path out of the session and off the disk, in that order. Unlinking
// underneath a live torrent leaves it seeding, and erroring on, a file that is
// no longer there. Used for a deletion this node was told about and for one it
// worked out from a peer's digest alike.
void erase(lt::session &s, const std::filesystem::path &path);

// The path a message asks to delete, or nothing if it asks for something
// else. Lets a caller know which file act() is about to remove without
// having to read the wire format itself.
auto removed_path(const std::vector<zmq::message_t> &v)
    -> std::optional<std::filesystem::path>;

// Takes on the deletions a peer's digest carries that this node has not got,
// and drops whatever loses to them. A node that never saw the remove would
// otherwise mismatch the root hash for ever and ship a full digest every round,
// and a node coming back with the file would hand it to everyone again.
//
// A path that goes leaves the listing, the session, the disk and the repair
// cache together (V57). Answers with the paths whose file was actually deleted,
// so the caller can say which they were.
auto adopt(lt::session &s, const reconcile::tombstone_map_t &theirs,
           files::file_map_t &former, reconcile::tombstone_map_t &mine,
           repairs_t &repairs, reconcile::time_point now)
    -> std::vector<std::filesystem::path>;
} // namespace protocol
