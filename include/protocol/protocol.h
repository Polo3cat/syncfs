#pragma once

#include <expected>
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
} // namespace protocol