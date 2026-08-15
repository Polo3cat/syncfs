#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <libtorrent/session.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <zmq.hpp>

namespace protocol {

// Every verb fixes its own part count and this is the largest of them, which
// is all a receiver needs to know before it has read the verb.
inline constexpr size_t max_parts = 4;

inline void subscribe(zmq::socket_t &s) {
  s.set(zmq::sockopt::subscribe, "create");
  s.set(zmq::sockopt::subscribe, "remove");
  s.set(zmq::sockopt::subscribe, "state");
  s.set(zmq::sockopt::subscribe, "digest");
}

// What act() did, for a caller that has to follow up on it. The verb comes
// back because the reconciliation reads state and digest for itself, and the
// created path because a received file still needs the time it was written at
// its origin put back on it once libtorrent is done writing it (V44).
struct outcome {
  std::string verb;
  std::string message;
  std::optional<std::filesystem::path> created;
  std::filesystem::file_time_type origin{};
};

auto act(const std::vector<zmq::message_t> &v, lt::session &s)
    -> std::expected<outcome, std::string>;

// Where a torrent's single file sits inside the sync root. A v2-only torrent
// holding one file under one directory drops that directory on load, so the
// torrent alone no longer names the file and the save path has to be read
// back with it.
auto held_path(const lt::torrent_handle &h)
    -> std::optional<std::filesystem::path>;

// The path a message asks to delete, or nothing if it asks for something
// else. Lets a caller know which file act() is about to remove without
// having to read the wire format itself.
auto removed_path(const std::vector<zmq::message_t> &v)
    -> std::optional<std::filesystem::path>;
} // namespace protocol
