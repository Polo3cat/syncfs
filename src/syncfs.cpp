#include "libtorrent/session_params.hpp"
#include "libtorrent/torrent_handle.hpp"
#include "libtorrent/torrent_status.hpp"
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <map>
#include <memory>
#include <print>
#include <span>
#include <spdlog/common.h>
#include <spdlog/spdlog.h>
#include <string>
#include <utility>
#include <utils.h>
#include <vector>
#include <zmq.hpp>

#include <libtorrent/session.hpp>
#include <libtorrent/torrent_info.hpp>

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

auto file_path(const std::shared_ptr<const lt::torrent_info> &ti)
    -> std::string {
  const auto &layout = ti->layout();
  auto file_index = *(layout.file_range().begin());
  return layout.file_path(file_index);
}

auto to_string(lt::torrent_status::state_t s) -> std::string {
  switch (s) {
  case lt::torrent_status::state_t::checking_files:
    return "checking_files";
  case lt::torrent_status::state_t::downloading_metadata:
    return "downloading_metadata";
  case lt::torrent_status::state_t::downloading:
    return "downloading";
  case lt::torrent_status::state_t::finished:
    return "finished";
  case lt::torrent_status::state_t::seeding:
    return "seeding";
  case lt::torrent_status::state_t::checking_resume_data:
    return "checking_resume_data";
  default:
    return std::format("unknown({})",
                       std::to_underlying<lt::torrent_status::state_t>(s));
  }
}

void print_session_statistics(const lt::session &s) {
  auto torrents = s.get_torrent_status(
      [](const auto &) -> bool { return true; },
      lt::torrent_handle::query_accurate_download_counters |
          lt::torrent_handle::query_torrent_file);
  auto msg = std::format("\n{}\t\t{}\t\t{}\t\t{}\t{}\t{}", "Name", "Progr",
                         "Total", "Seeds", "Peers", "State");
  for (const auto &t : torrents) {
    msg =
        std::format("{}\n{}\t\t{:.2f}\t\t{}\t\t{}\t{}\t{}", msg,
                    file_path(t.torrent_file.lock()), t.progress, t.total_done,
                    t.num_seeds, t.num_peers, to_string(t.state));
  }
  spdlog::debug(msg);
}

void sync_loop(const source::Source &server, sink::Sink listener) {
  auto const file_monitor = monitor::Monitor();
  auto former = files::list();
  auto params = lt::session_params{};
  auto session = lt::session(params);
  auto last_stats = std::chrono::steady_clock::now();
  auto const interval = std::chrono::seconds{2};
  while (true) {
    if (std::chrono::steady_clock::now() - last_stats >= interval) {
      last_stats += interval;
      print_session_statistics(session);
    }
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
