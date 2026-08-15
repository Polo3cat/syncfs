#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <libtorrent/session.hpp>
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

auto act(const std::vector<zmq::message_t> &v, lt::session &s)
    -> std::expected<std::string, std::string>;

// The path a message asks to delete, or nothing if it asks for something
// else. Lets a caller know which file act() is about to remove without
// having to read the wire format itself.
auto removed_path(const std::vector<zmq::message_t> &v)
    -> std::optional<std::filesystem::path>;
} // namespace protocol
