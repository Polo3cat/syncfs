#include <cassert>
#include <cstdlib>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <libtorrent/session.hpp>
#include <map>
#include <print>
#include <span>
#include <spdlog/common.h>
#include <spdlog/spdlog.h>
#include <string>
#include <utility>
#include <utils.h>
#include <vector>
#include <zmq.hpp>

#include <discovery.h>
#include <files.h>
#include <monitor.h>
#include <protocol.h>
#include <sink.h>
#include <source.h>

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

void send(const diff_t &diff, const source::Source &server) {
  server.remove(diff.removed);
  server.create(diff.created);
  server.create(diff.modified);
}

void sync_loop(const source::Source &server, sink::Sink listener) {
  auto const file_monitor = monitor::Monitor();
  auto former = files::list();
  lt::session session;
  while (true) {
    if (file_monitor.wait()) {
      file_monitor.discard();
      auto current = files::list();
      send(create_diff(former, current), server);
      former = std::move(current);
    }
    if (!listener.receive_ready()) {
      continue;
    }
    auto const received = listener.receive(protocol::length);
    if (!received.has_value()) {
      spdlog::warn(received.error());
      continue;
    }
    auto r = protocol::act(received.value(), session);
    if (!r.has_value()) {
      spdlog::warn(r.error());
    } else {
      spdlog::info(r.value());
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
  protocol::subscribe(listener);

  for (const auto &peer : peers) {
    listener.connect(peer);
    spdlog::info("Subscribed to {}", peer);
  }

  const auto my_address = std::format("tcp://{}", args[2]);
  listener.connect(my_address);
  spdlog::info("Subscribed to {} (myself)", my_address);

  // The "source" server does not need to be available early.
  // ZMQ makes the actual underlying connection as needed.
  zmq::socket_t client{ctx, zmq::socket_type::pub};
  client.bind(my_address);
  spdlog::info("Publishing on {}", args[2]);

  sync_loop(source::Source(std::move(client), utils::parse_host_port(args[2])),
            sink::Sink(std::move(listener)));

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
