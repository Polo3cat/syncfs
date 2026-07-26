#pragma once

#include <cstddef>
#include <expected>
#include <string>

#include <zmq.hpp>

namespace sink {
struct Sink {
  mutable zmq::socket_t client;
  zmq::poller_t<> in_poller;

  explicit Sink(zmq::socket_t &&s) : client{std::move(s)} {
    in_poller.add(client, zmq::event_flags::pollin);
  }

  [[nodiscard]] auto receive(size_t max_messages) const
      -> std::expected<std::vector<zmq::message_t>, std::string>;
  auto receive_ready() -> bool;
};
} // namespace sink