#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <libtorrent/session.hpp>
#include <zmq.hpp>

namespace protocol {

inline const size_t length = 2;

inline void subscribe(zmq::socket_t &s) {
  s.set(zmq::sockopt::subscribe, "create");
  s.set(zmq::sockopt::subscribe, "remove");
}

auto act(const std::vector<zmq::message_t> &v, lt::session &s)
    -> std::expected<std::string, std::string>;

// The path a message asks to delete, or nothing if it asks for something
// else. Lets a caller know which file act() is about to remove without
// having to read the wire format itself.
auto removed_path(const std::vector<zmq::message_t> &v)
    -> std::optional<std::filesystem::path>;
} // namespace protocol