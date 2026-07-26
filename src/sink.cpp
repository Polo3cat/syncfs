#include <bits/chrono.h>
#include <cstddef>
#include <expected>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>
#include <zmq.hpp>
#include <zmq_addon.hpp>

#include <sink.h>

namespace sink {

auto Sink::receive(size_t max_messages) const
    -> std::expected<std::vector<zmq::message_t>, std::string> {
  std::vector<zmq::message_t> recv_msgs{};
  try {
    [[maybe_unused]] auto res = zmq::recv_multipart_n(
        client, std::back_inserter(recv_msgs), max_messages);
  } catch (const std::runtime_error &e) {
    return std::unexpected{"Did not receive the expected number of messages"};
  }
  return recv_msgs;
}

auto Sink::receive_ready() -> bool {
  using namespace std::chrono_literals;
  std::vector<zmq::poller_event<>> evs(1);
  return in_poller.wait_all(evs, 100ms) != 0;
}
} // namespace sink
