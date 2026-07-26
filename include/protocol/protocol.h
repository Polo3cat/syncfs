#pragma once

#include <expected>
#include <string>
#include <vector>
#include <zmq.hpp>

namespace protocol {

inline const size_t length = 2;

inline auto act(const std::vector<zmq::message_t> &v)
    -> std::expected<std::string, std::string> {
  if (v.size() != 2) {
    return std::unexpected{"Wrong protocol message."};
  }
  auto action = v.at(0).to_string_view();
  if (action == "delete") {
    ;
  }
  if (action == "create") {
    ;
  }
  return "Filename";
}

inline void subscribe(zmq::socket_t &s) {
  s.set(zmq::sockopt::subscribe, "create");
  s.set(zmq::sockopt::subscribe, "remove");
}

} // namespace protocol