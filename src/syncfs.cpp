#include "libtorrent/settings_pack.hpp"
#include <atomic>
#include <cassert>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <map>
#include <memory>
#include <print>
#include <set>
#include <span>
#include <spdlog/common.h>
#include <spdlog/spdlog.h>
#include <string>
#include <system_error>
#include <utility>
#include <utils.h>
#include <vector>
#include <zmq.hpp>

#include <libtorrent/alert.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/extensions/ut_pex.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/session_params.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/torrent_status.hpp>

#include <discovery.h>
#include <files.h>
#include <monitor.h>
#include <protocol.h>
#include <sink.h>
#include <source.h>

namespace {
// Set from a signal handler, so nothing but a volatile sig_atomic_t will do.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
volatile std::sig_atomic_t stop_requested = 0;

extern "C" void request_stop(int /*signal*/) { stop_requested = 1; }

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

// The path files::list() would report for the single file of a torrent. The
// torrent carries it normalized ("a/f"), the listing walks "." ("./a/f").
auto listed_path(const std::shared_ptr<const lt::torrent_info> &ti)
    -> std::filesystem::path {
  return std::filesystem::path{"."} / file_path(ti);
}

// Remembers what a file looked like the moment libtorrent was done writing it,
// so the inotify event for that write can be told apart from a real local
// edit. Without this the receiver re-hashes the file it was just sent and
// announces an info hash every peer already has.
void remember_written_file(files::file_map_t &written,
                           const lt::torrent_handle &handle) {
  const auto info = handle.torrent_file();
  if (!info) {
    return;
  }
  auto path = listed_path(info);
  std::error_code err;
  const auto time = std::filesystem::last_write_time(path, err);
  if (err) {
    return;
  }
  written.insert_or_assign(std::move(path), time);
}

void drain_alerts(lt::session &session, files::file_map_t &written) {
  auto alerts = std::vector<lt::alert *>{};
  session.pop_alerts(&alerts);
  for (const auto *alert : alerts) {
    spdlog::debug("{}", alert->message());
    // A finished torrent may still have pieces in flight to disk, so the file
    // is only stable once the flush it triggers comes back.
    if (const auto *finished =
            lt::alert_cast<lt::torrent_finished_alert>(alert)) {
      finished->handle.flush_cache();
    } else if (const auto *flushed =
                   lt::alert_cast<lt::cache_flushed_alert>(alert)) {
      remember_written_file(written, flushed->handle);
    }
  }
}

// The changes worth announcing: what the diff reports minus everything a peer
// caused us to do, which the peers already know about.
auto local_changes(const files::file_map_t &former,
                   const files::file_map_t &current,
                   const files::file_map_t &written,
                   std::set<std::filesystem::path> &deleted) -> diff_t {
  auto diff = create_diff(former, current);
  // A file still carrying the write libtorrent gave it is an echo. The entries
  // of modified hold the timestamp the file had before the change, so the
  // comparison has to read the current one.
  auto is_echo = [&written, &current](const auto &entry) -> bool {
    const auto seen = written.find(entry.first);
    const auto now = current.find(entry.first);
    return seen != written.end() && now != current.end() &&
           seen->second == now->second;
  };
  std::erase_if(diff.created, is_echo);
  std::erase_if(diff.modified, is_echo);
  // Likewise a file gone because a peer said so. The mark is spent on the
  // event it was meant for.
  std::erase_if(diff.removed, [&deleted](const auto &entry) -> bool {
    return deleted.erase(entry.first) > 0;
  });
  return diff;
}

void forget_spent_marks(const files::file_map_t &current,
                        files::file_map_t &written,
                        std::set<std::filesystem::path> &deleted) {
  std::erase_if(written, [&current](const auto &entry) -> bool {
    return !current.contains(entry.first);
  });
  // A path that is present again was never deleted, or has been recreated
  // since: either way its mark no longer stands for anything.
  std::erase_if(deleted, [&current](const auto &path) -> bool {
    return current.contains(path);
  });
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

void sync_loop(zmq::socket_t sender, zmq::socket_t receiver,
               std::string local_addr, unsigned short local_port) {

  auto former = files::list();
  auto settings = lt::settings_pack{};

  const unsigned short libtorrent_port_offset = 2000;
  const unsigned short libtorrent_listen_port =
      local_port + libtorrent_port_offset;
  auto libtorrent_listen_address =
      std::format("{}:{}", "0.0.0.0", libtorrent_listen_port);

  settings.set_str(lt::settings_pack::listen_interfaces,
                   std::move(libtorrent_listen_address));

  // By default libtorrent keeps a single connection per peer IP, which is
  // precisely the setup we use for testing. This also considers situations
  // where one receives from multiple syncfs on the same host wanting to
  // synchornize multiple directories into the same on the remote.
  settings.set_bool(lt::settings_pack::allow_multiple_connections_per_ip, true);

  // status carries torrent_finished_alert and storage carries
  // cache_flushed_alert, the pair that tells us a file has reached disk.
  settings.set_int(lt::settings_pack::alert_mask,
                   static_cast<int>(static_cast<std::uint32_t>(
                       lt::alert_category::error | lt::alert_category::status |
                       lt::alert_category::storage)));

  auto params = lt::session_params(settings);
  auto session = lt::session(params);

  session.add_extension(&lt::create_ut_pex_plugin);

  spdlog::info("Started libtorrent session on {}:{}", "0.0.0.0",
               session.listen_port());

  auto alert_ready = std::atomic_flag{};
  session.set_alert_notify([&] -> void {
    std::atomic_flag_test_and_set_explicit(&alert_ready,
                                           std::memory_order::relaxed);
  });

  auto server =
      source::Source(std::move(sender), std::make_pair(std::move(local_addr),
                                                       session.listen_port()));
  auto listener = sink::Sink(std::move(receiver));

  auto file_monitor = monitor::Monitor();
  // What each file looked like when libtorrent finished writing it.
  auto written = files::file_map_t{};
  // Files deleted because a peer asked for it, still waiting for their own
  // inotify event to arrive.
  auto deleted = std::set<std::filesystem::path>{};
  auto last_stats = std::chrono::steady_clock::now();
  auto const interval = std::chrono::seconds{2};
  while (stop_requested == 0) {
    if (std::chrono::steady_clock::now() - last_stats >= interval) {
      last_stats += interval;
      print_session_statistics(session);
    }
    if (std::atomic_flag_test_explicit(&alert_ready,
                                       std::memory_order::relaxed)) {
      drain_alerts(session, written);
      std::atomic_flag_clear_explicit(&alert_ready, std::memory_order::relaxed);
    }
    if (file_monitor.wait()) {
      file_monitor.discard();
      auto current = files::list();
      send(local_changes(former, current, written, deleted), server);
      former = std::move(current);
      forget_spent_marks(former, written, deleted);
    }
    if (!listener.receive_ready()) {
      continue;
    }
    auto const received = listener.receive(protocol::length);
    if (!received.has_value()) {
      spdlog::warn(received.error());
      continue;
    }
    // Read before acting: afterwards there is no telling whether the file was
    // there to begin with, and a mark for a file we never had would sit around
    // waiting to swallow a real deletion.
    const auto removed = protocol::removed_path(received.value());
    const bool existed = removed && std::filesystem::exists(*removed);

    auto r = protocol::act(received.value(), session);
    if (!r.has_value()) {
      spdlog::warn(r.error());
    } else {
      spdlog::info(r.value());
      if (existed) {
        deleted.insert(*removed);
      }
    }
  }
  spdlog::info("Stopping.");
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
    std::println("Usage: syncfs <peers file> <listen address>");
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

  // As PID 1 of a container there is no default disposition for these, so
  // without a handler the process can only be killed.
  // The previously installed handlers are of no interest.
  static_cast<void>(std::signal(SIGTERM, request_stop));
  static_cast<void>(std::signal(SIGINT, request_stop));

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

  auto [host, port] = utils::parse_host_port(args[2]);

  sync_loop(std::move(client), std::move(listener), std::move(host), port);

  return EXIT_SUCCESS;
} catch (zmq::error_t &e) {
  try {
    spdlog::critical("{} {}", e.num(), e.what());
  } catch (...) {
    return EXIT_FAILURE;
  }
  return EXIT_FAILURE;
} catch (std::exception &e) {
  try {
    spdlog::critical("{}", e.what());
  } catch (...) {
    return EXIT_FAILURE;
  }
  return EXIT_FAILURE;
}
