#include <cstddef>
#include <expected>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <sink.h>
#include <zmq.hpp>

namespace {
// Long enough that a poll answers, short enough that a test which is going
// nowhere still ends.
constexpr int poll_ms = 50;
constexpr int attempts = 100;
// What the daemon asks for: the largest part count any verb has (V3).
constexpr size_t cap = 4;

auto subscriber(zmq::context_t &ctx, const std::string &address)
    -> zmq::socket_t {
  zmq::socket_t s{ctx, zmq::socket_type::sub};
  s.set(zmq::sockopt::subscribe, "");
  // Without this a receive that has nothing to read blocks for ever, which in
  // ctest is a wedged suite rather than a failed test (B11).
  s.set(zmq::sockopt::rcvtimeo, poll_ms);
  s.connect(address);
  return s;
}

// A publisher and the sink that listens to it, each on its own context, the way
// the daemon has them: one process publishes and subscribes to itself.
struct Wire {
  zmq::context_t publisher_ctx;
  zmq::context_t subscriber_ctx;
  zmq::socket_t publisher;
  sink::Sink listener;

  explicit Wire(const std::string &address)
      : publisher{publisher_ctx, zmq::socket_type::pub},
        listener{subscriber(subscriber_ctx, address)} {
    publisher.bind(address);
  }
};

void send_parts(zmq::socket_t &publisher, size_t parts) {
  for (size_t part = 0; part + 1 < parts; ++part) {
    static_cast<void>(
        publisher.send(zmq::str_buffer("part"), zmq::send_flags::sndmore));
  }
  static_cast<void>(publisher.send(zmq::str_buffer("last")));
}

// A publisher drops whatever it sends before the subscriber has finished
// connecting and nothing tells either end when that has happened, so the
// message is repeated until one lands (V53). An empty result is that "not yet":
// the receive timed out with nothing to read.
auto receive_until_something_lands(Wire &wire, size_t parts)
    -> std::expected<std::vector<zmq::message_t>, std::string> {
  for (int attempt = 0; attempt < attempts; ++attempt) {
    send_parts(wire.publisher, parts);
    auto received = wire.listener.receive(cap);
    if (!received.has_value() || !received->empty()) {
      return received;
    }
  }
  return std::unexpected{"nothing arrived inside the attempt budget"};
}
} // namespace

TEST(Sink, AMessageShorterThanTheCapIsHandedOverWhole) {
  // The sink counts nothing: a two part message is passed up as two parts and
  // it is the protocol that rejects it against the count its verb fixes (V3).
  // Reading fewer parts than the cap as a failure here would turn every
  // malformed announcement into a lost message with no verb to name it.
  Wire wire{"tcp://127.0.0.1:3100"};

  const auto received = receive_until_something_lands(wire, 2);

  ASSERT_TRUE(received.has_value()) << received.error();
  ASSERT_EQ(received->size(), 2U);
}

TEST(Sink, AMessageOfExactlyTheCapIsHandedOverWhole) {
  Wire wire{"tcp://127.0.0.1:3101"};

  const auto received = receive_until_something_lands(wire, cap);

  ASSERT_TRUE(received.has_value()) << received.error();
  ASSERT_EQ(received->size(), cap);
}

TEST(Sink, AMessageWithMorePartsThanTheCapIsRejected) {
  // Five parts is no verb this wire has, so the sink refuses the message rather
  // than handing up the first four and leaving the fifth to be read as the
  // start of the next one.
  Wire wire{"tcp://127.0.0.1:3102"};

  const auto received = receive_until_something_lands(wire, cap + 1);

  ASSERT_FALSE(received.has_value());
  ASSERT_EQ(received.error(),
            "Did not receive the expected number of messages");
}

TEST(Sink, NothingToReceiveIsNotAFailure) {
  // The daemon calls receive_ready() first and only reads when it says so, but
  // a receive that finds nothing still has to come back empty handed rather
  // than as an error: the loop has a stop flag to check (V33).
  Wire wire{"tcp://127.0.0.1:3103"};

  ASSERT_FALSE(wire.listener.receive_ready());

  const auto received = wire.listener.receive(cap);

  ASSERT_TRUE(received.has_value()) << received.error();
  ASSERT_TRUE(received->empty());
}
