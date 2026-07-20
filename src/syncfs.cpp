#include <array>
#include <bits/chrono.h>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <discovery.h>
#include <exception>
#include <expected>
#include <files.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <map>
#include <monitor.h>
#include <print>
#include <sink.h>
#include <span>
#include <spdlog/common.h>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <string>
#include <utility>
#include <utils.h>
#include <vector>
#include <zmq.hpp>
#include <zmq_addon.hpp>

namespace {
struct diff_t {
  files::file_map_t removed;
  files::file_map_t created;
  files::file_map_t modified;
};

auto create_diff(const files::file_map_t &former,
                 const files::file_map_t &current) -> diff_t {
  const auto removed = files::diff(former, current);
  const auto created = files::diff(current, former);
  const auto modified = files::intersection_name(removed, created);
  return diff_t{.removed = files::diff_name(removed, modified),
                .created = files::diff_name(created, modified),
                .modified = modified};
}

void send(const diff_t &diff, const sink::Sink &server) {
  server.remove(diff.removed);
  server.create(diff.created);
  server.update(diff.modified);
}

auto receive_ready(zmq::poller_t<> &p) -> bool {
  using namespace std::chrono_literals;
  std::vector<zmq::poller_event<>> evs(1);
  return p.wait_all(evs, 100ms) != 0;
}

enum class Action : uint8_t { CREATE, UPDATE, REMOVE };

[[nodiscard]] auto receive(zmq::socket_t &s)
    -> std::expected<std::pair<std::string, Action>, std::string> {
  std::array<zmq::message_t, 3> recv_msgs{};
  zmq::recv_result_t res{};
  try {
    res = zmq::recv_multipart_n(s, recv_msgs.begin(), recv_msgs.size());
  } catch (std::runtime_error &e) {
    return std::unexpected{"Did not receive the expected number of messages"};
  }

  if (!res.has_value()) {
    return std::unexpected{"Expected to receive messages"};
  }
  switch (res.value()) {
  case 1:
    return std::unexpected{"Expected more than 1 message"};
  case 2:
    if (recv_msgs.at(0).to_string_view() == "remove") {
      std::filesystem::remove(recv_msgs.at(1).to_string_view());
      return std::pair{recv_msgs.at(1).to_string(), Action::REMOVE};
    }
    break;
  case 3:
    if (recv_msgs.at(0).to_string_view() == "create" ||
        recv_msgs.at(0).to_string_view() == "update") {
      std::ofstream file_stream{recv_msgs.at(1).to_string(),
                                std::ios_base::out | std::ios_base::trunc |
                                    std::ios_base::binary};
      file_stream << recv_msgs.at(2).to_string_view();
      return std::pair{recv_msgs.at(1).to_string(), Action::CREATE};
    }
    break;
  default:
    break;
  }
  return std::unexpected{"Did not receive the expected number of messages"};
}

void sync_loop(sink::Sink server, zmq::socket_t listener) {
  zmq::poller_t<> in_poller;
  in_poller.add(listener, zmq::event_flags::pollin);

  auto const file_monitor = monitor::Monitor();
  auto former = files::list();
  while (true) {
    if (file_monitor.wait()) {
      file_monitor.discard();
      auto current = files::list();
      send(create_diff(former, current), server);
      former = std::move(current);
    }
    if (!receive_ready(in_poller)) {
      continue;
    }
    auto const received = receive(listener);
    if (received.has_value()) {
      if (received.value().second == Action::REMOVE) {
        spdlog::debug("<- Remove {}", received.value().first);
        former = files::remove(std::move(former), received.value().first);
      } else {
        spdlog::debug("<- {} {}",
                      (received.value().second == Action::CREATE) ? "Create"
                                                                  : "Update",
                      received.value().first);
        former = files::append(std::move(former), received.value().first);
      }
    } else {
      spdlog::warn(received.error());
    }
  }
}

auto pub_socket(zmq::context_t &ctx, const std::string &addr) -> zmq::socket_t {
  zmq::socket_t s{ctx, zmq::socket_type::pub};
  s.bind(addr);
  return s;
}
} // namespace

auto main(int argc, char *argv[]) -> int try {
  auto args = std::span(argv, static_cast<size_t>(argc));
  if (args.size() < 3) {
    std::println("Usage: syncf <peers file> <listen address>");
    std::println();
    std::println("Synchronizes the working directory with <peers file>");
    std::println("<peers file> contains for each line a network address.");
    std::println("Network addresses are IPv4 addresses with a TCP port.");
    std::println("<listen address> is an IPv4 address and port.");
    return EXIT_FAILURE;
  }
#ifdef NDEBUG
  spdlog::set_pattern("[%Y-%m-%d %T] [%P] [%^%l%$] %v");
  spdlog::set_level(spdlog::level::info);
#else
  spdlog::set_pattern("[%Y-%m-%d %T.%F] [%P] [%^%l%$] %v");
  spdlog::set_level(spdlog::level::debug);
#endif

  auto peers = discovery::parse(std::filesystem::path{args[1]});
  assert(!peers.empty());

  zmq::context_t ctx;

  zmq::socket_t listener{ctx, zmq::socket_type::sub};
  listener.set(zmq::sockopt::subscribe, "create");
  listener.set(zmq::sockopt::subscribe, "remove");
  listener.set(zmq::sockopt::subscribe, "update");

  for (const auto &peer : peers) {
    listener.connect(peer);
    spdlog::info("Subscribed to {}", peer);
  }

  spdlog::info("Publishing on {}", args[2]);

  // The "sink" server does not need to be available early.
  // ZMQ makes the actual underlying connection as needed.
  auto client = pub_socket(ctx, std::format("tcp://{}", args[2]));
  sync_loop(sink::Sink(std::move(client), utils::parse_host_port(args[2])),
            std::move(listener));

} catch (zmq::error_t &e) {
  try {
    spdlog::critical("{} {}", e.num(), e.what());
  } catch (...) {
    return EXIT_FAILURE;
  }
} catch (std::exception &e) {
  try {
    spdlog::critical("{}", e.what());
  } catch (...) {
    return EXIT_FAILURE;
  }
  return EXIT_FAILURE;
}
