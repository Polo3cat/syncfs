#include "libtorrent/settings_pack.hpp"
#include <algorithm>
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
#include <span>
#include <spdlog/common.h>
#include <spdlog/spdlog.h>
#include <string>
#include <string_view>
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
#include <reconcile.h>
#include <sink.h>
#include <source.h>

namespace {
// How many announcements may sit queued on either end before ZMQ starts
// dropping them. One announcement per file, so this is the largest burst of
// file changes that survives a receiver busy adding torrents.
const int announcement_backlog = 100000;

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

void send(const diff_t &diff, const files::file_map_t &current,
          const source::Source &server) {
  // The entries of modified come from removed, so they carry the timestamp the
  // file had before the change. Announcing that one would tell every peer the
  // edit is older than the copy they already hold, and the edit would never
  // travel.
  auto announce = [&current, &server](files::file_map_t m) -> void {
    for (auto &entry : m) {
      const auto now = current.find(entry.first);
      if (now != current.end()) {
        entry.second = now->second;
      }
    }
    server.create(m);
  };
  server.remove(diff.removed);
  announce(diff.created);
  announce(diff.modified);
}

auto file_path(const std::shared_ptr<const lt::torrent_info> &ti)
    -> std::string {
  const auto &layout = ti->layout();
  auto file_index = *(layout.file_range().begin());
  return layout.file_path(file_index);
}

// What this node remembers about the files its peers sent it.
struct inbound_t {
  // The time each file being received was written at its origin, waiting for
  // libtorrent to be done with the file so it can be put back on it. An entry
  // is spent by the stamp; a transfer that never finishes leaves its own
  // behind, which is a path and a timestamp and nothing more.
  files::file_map_t origins;
  // What each file looked like when libtorrent finished writing it.
  files::file_map_t written;
};

// Puts the time the file was written at its origin back on it, and then
// remembers what it looked like, so the inotify event for libtorrent's own
// write can be told apart from a real local edit. Without the snapshot the
// receiver re-hashes the file it was just sent and announces an info hash
// every peer already has; without the stamp first, every entry of the
// snapshot is stale the moment it is taken and the snapshot suppresses
// nothing at all.
void stamp_and_remember(inbound_t &inbound, const lt::torrent_handle &handle) {
  const auto path = protocol::held_path(handle);
  if (!path) {
    return;
  }
  if (const auto origin = inbound.origins.find(*path);
      origin != inbound.origins.end()) {
    static_cast<void>(utils::stamp(*path, origin->second));
    inbound.origins.erase(origin);
  }
  std::error_code err;
  const auto time = std::filesystem::last_write_time(*path, err);
  if (err) {
    return;
  }
  inbound.written.insert_or_assign(*path, time);
}

void drain_alerts(lt::session &session, inbound_t &inbound) {
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
      stamp_and_remember(inbound, flushed->handle);
    }
  }
}

// The changes worth announcing: what the diff reports minus everything a peer
// caused us to do, which the peers already know about.
auto local_changes(const files::file_map_t &former,
                   const files::file_map_t &current,
                   const files::file_map_t &written,
                   reconcile::tombstone_map_t &tombstones) -> diff_t {
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
  // A file that is back but no newer than the deletion that killed it stays
  // where it is: every peer still holds that deletion and would repair the
  // file away again, which is last write wins applied honestly rather than an
  // exception carved out for it. A file that is newer cancels the deletion.
  auto loses_to_tombstone = [&tombstones, &current](const auto &entry) -> bool {
    const auto grave = tombstones.find(entry.first);
    if (grave == tombstones.end()) {
      return false;
    }
    const auto now = current.find(entry.first);
    if (now != current.end() && !reconcile::beats(grave->second, now->second)) {
      tombstones.erase(grave);
      return false;
    }
    return true;
  };
  std::erase_if(diff.created, loses_to_tombstone);
  std::erase_if(diff.modified, loses_to_tombstone);
  // A deletion this node already remembers is the echo of one a peer asked
  // for. Republishing it is not only waste: an echo arriving after the path
  // was recreated deletes the new file.
  std::erase_if(diff.removed, [&tombstones](const auto &entry) -> bool {
    return tombstones.contains(entry.first);
  });
  // What is left is this node's own deletion, and nothing else records it.
  // The file is gone, so there is no timestamp left to read and the moment it
  // was noticed is what orders it; an old modification time would lose to any
  // create at all.
  const auto noticed = std::chrono::system_clock::now();
  for (const auto &entry : diff.removed) {
    reconcile::mark(tombstones, entry.first, noticed);
  }
  return diff;
}

void forget_spent_marks(const files::file_map_t &current,
                        files::file_map_t &written,
                        reconcile::tombstone_map_t &tombstones) {
  std::erase_if(written, [&current](const auto &entry) -> bool {
    return !current.contains(entry.first);
  });
  // A tombstone is not spent by the event it suppressed. It stands until a
  // newer file cancels it or it outlives the time to live, because until then
  // it is the only record a peer that missed the deletion can be repaired
  // from.
  reconcile::expire(tombstones, std::chrono::system_clock::now());
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

// The repairs this node has taken on, and the source of the randomized wait
// that keeps every holder of the same gap from answering at once.
struct repairs_t {
  reconcile::pending_map_t pending;
  reconcile::Backoff backoff;
  // What this node last applied for each path, which is what an announcement
  // for that path is judged against.
  protocol::applied_map_t applied;
  // The announcement each path was last seen with, which is what a repair
  // says again. The session cannot rebuild it and reading the file to hash it
  // afresh is what a repair exists to avoid.
  std::map<std::filesystem::path, std::string> announcements;
};

// Hands over the repairs whose wait is up. A path this node has no
// announcement for cannot be repaired from here, and saying so is better than
// dropping it in silence: after a restart a node holds its files and none of
// their announcements, and it is another holder that answers for them.
void publish_repairs(repairs_t &repairs, const source::Source &server,
                     const files::file_map_t &former,
                     std::chrono::steady_clock::time_point now) {
  for (const auto &path : reconcile::due(repairs.pending, now)) {
    const auto announcement = repairs.announcements.find(path);
    const auto mtime = former.find(path);
    if (announcement == repairs.announcements.end() || mtime == former.end()) {
      spdlog::debug("Nothing to repair \"{}\" with", path.native());
      continue;
    }
    server.repair(path, mtime->second, announcement->second);
  }
}

// What a peer's digest asks of this node. The deletions it carries that this
// node never heard are taken on, and any file here that loses to one of them
// goes: a node that never saw the remove would otherwise mismatch the root
// hash for ever and ship a full digest every round, and a node coming back
// with the file would hand it to everyone again. What is left over is the
// files this node holds and the peer does not, which are its to repair.
void read_digest(const std::vector<zmq::message_t> &v, lt::session &session,
                 files::file_map_t &former,
                 reconcile::tombstone_map_t &tombstones, repairs_t &repairs) {
  const auto held = reconcile::decode_held(v.at(2).to_string_view());
  const auto deleted = reconcile::decode_tombstones(v.at(3).to_string_view());

  // Adoption comes first. The other order would have this node repair a file
  // back to the very peer that has just said the file is dead.
  for (const auto &[path, at] : reconcile::adoptable(
           deleted, former, tombstones, std::chrono::system_clock::now())) {
    reconcile::mark(tombstones, path, at);
    if (former.erase(path) > 0) {
      protocol::erase(session, path);
      spdlog::info("Delete \"{}\", a peer had it deleted", path.native());
    }
  }

  // Whoever holds a file the peer lacks is the one that repairs it: PUB/SUB
  // has no request path, so the node with the gap cannot ask anyone. Every
  // holder waits a randomized while and drops its own repair on seeing
  // another's, so nobody is elected and the whole thing keeps working with
  // any one of them wedged.
  const auto missing = reconcile::gaps(former, tombstones, held);
  if (!missing.empty()) {
    spdlog::info("{} paths to repair for a peer", missing.size());
    reconcile::arm(repairs.pending, missing, std::chrono::steady_clock::now(),
                   repairs.backoff);
  }
}

// When the root hash is due to go out: once a reconcile period while the
// directory is quiet and every torrent has settled, and once a minute
// regardless. Without that ceiling a workload writing every nine seconds keeps
// the node busy for ever and the repair never runs in exactly the regime that
// loses announcements; without the gate a node still receiving publishes the
// hash of a tree it is halfway through filling in.
struct quiescence_t {
  std::chrono::steady_clock::time_point now;
  std::chrono::steady_clock::time_point last_state;
  std::chrono::steady_clock::time_point last_change;
  bool settled = true;
};

auto state_due(const quiescence_t &q) -> bool {
  const auto since_state = q.now - q.last_state;
  const bool quiescent =
      q.settled && (q.now - q.last_change >= reconcile::quiescence_window);
  return since_state >= reconcile::state_ceiling ||
         (quiescent && since_state >= reconcile::period);
}

// The two verbs the reconciliation reads for itself, once act() has judged
// them well formed.
void reconcile_message(std::string_view verb,
                       const std::vector<zmq::message_t> &v,
                       lt::session &session, const source::Source &server,
                       files::file_map_t &former,
                       reconcile::tombstone_map_t &tombstones,
                       repairs_t &repairs) {
  // A root hash that differs means one of the two is missing something and
  // neither knows what. Both answer with a full digest: on a mismatch each
  // side's is information the other needs, so there is nothing to suppress.
  if (verb == "state") {
    if (v.at(1).to_string_view() != reconcile::hash(former, tombstones)) {
      server.digest(reconcile::encode(former), reconcile::encode(tombstones));
    }
    return;
  }
  if (verb == "digest" && v.at(1).to_string_view() != server.endpoint) {
    read_digest(v, session, former, tombstones, repairs);
  }
}

// One inbound message, from the wire to whatever it asks of this node.
void receive_one(const sink::Sink &listener, lt::session &session,
                 const source::Source &server, files::file_map_t &former,
                 reconcile::tombstone_map_t &tombstones, inbound_t &inbound,
                 repairs_t &repairs) {
  auto const received = listener.receive(protocol::max_parts);
  if (!received.has_value()) {
    spdlog::warn(received.error());
    return;
  }
  auto r =
      protocol::act(received.value(), session, tombstones, repairs.applied);
  if (!r.has_value()) {
    spdlog::warn(r.error());
    return;
  }
  spdlog::info(r->message);
  if (const auto gone = protocol::removed_path(received.value())) {
    repairs.announcements.erase(*gone);
    repairs.applied.erase(*gone);
    repairs.pending.erase(*gone);
  }
  if (r->created) {
    inbound.origins.insert_or_assign(*r->created, r->origin);
    // Every create this node applies passes here, its own included: it
    // subscribes to itself, so this is the one place the announcement for a
    // path is known whoever made it.
    repairs.announcements.insert_or_assign(*r->created,
                                           received->at(1).to_string());
    repairs.applied.insert_or_assign(
        *r->created,
        protocol::applied_t{.origin = r->origin, .content = r->content});
    // And another holder getting there first is what stands this node's own
    // repair down, which is what keeps the duplicates far below the number of
    // holders.
    repairs.pending.erase(*r->created);
  }
  reconcile_message(r->verb, received.value(), session, server, former,
                    tombstones, repairs);
}

auto torrent_states(const lt::session &s) -> std::vector<lt::torrent_status> {
  return s.get_torrent_status(
      [](const auto &) -> bool { return true; },
      lt::torrent_handle::query_accurate_download_counters |
          lt::torrent_handle::query_torrent_file);
}

// Whether every torrent has stopped moving bytes. Half of the quiescence gate,
// and it costs nothing: the statistics already ask for this list every two
// seconds. A node still receiving would otherwise publish the hash of a tree
// it is in the middle of filling in.
auto all_settled(const std::vector<lt::torrent_status> &torrents) -> bool {
  return std::ranges::all_of(torrents, [](const auto &t) -> bool {
    return t.state == lt::torrent_status::state_t::seeding ||
           t.state == lt::torrent_status::state_t::finished;
  });
}

void print_session_statistics(const std::vector<lt::torrent_status> &torrents) {
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

  // Every torrent is added auto managed, and the queueing defaults are meant
  // for a user downloading a handful of things at a time: three downloads and
  // five seeds active, rotated every thirty seconds. One file is one torrent
  // here, so those defaults cap the whole daemon at three files per rotation,
  // and with thousands of files the small active sets of sender and receiver
  // stop overlapping altogether and no swarm ever forms. Unlimited queues let
  // every announced file move at once.
  const int unlimited = -1;
  settings.set_int(lt::settings_pack::active_downloads, unlimited);
  settings.set_int(lt::settings_pack::active_seeds, unlimited);
  settings.set_int(lt::settings_pack::active_limit, unlimited);

  // Lifting the queue limits is pointless while the connection limit stays at
  // its default of 200: one file is one torrent and every torrent holds its
  // own connection to every peer, so the limit is reached long before the
  // files are. Non-positive means as many as the file descriptor limit allows.
  settings.set_int(lt::settings_pack::connections_limit, unlimited);

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

  const auto endpoint = std::format("tcp://{}:{}", local_addr, local_port);
  auto server = source::Source(
      std::move(sender),
      std::make_pair(std::move(local_addr), session.listen_port()), endpoint);
  auto listener = sink::Sink(std::move(receiver));

  auto file_monitor = monitor::Monitor();
  auto inbound = inbound_t{};
  // Every path this node knows to have been deleted, and when. Peer driven
  // deletions and its own alike: a deletion it does not remember is one it
  // cannot repair a peer with.
  auto tombstones = reconcile::tombstone_map_t{};
  auto repairs = repairs_t{};
  auto last_stats = std::chrono::steady_clock::now();
  auto const interval = std::chrono::seconds{2};
  // The last time anything happened to the sync directory, whether this node
  // did it or libtorrent did.
  auto last_change = std::chrono::steady_clock::now();
  auto last_state = std::chrono::steady_clock::now();
  bool settled = true;
  while (stop_requested == 0) {
    const auto now = std::chrono::steady_clock::now();
    if (now - last_stats >= interval) {
      last_stats += interval;
      const auto torrents = torrent_states(session);
      print_session_statistics(torrents);
      settled = all_settled(torrents);
    }
    if (std::atomic_flag_test_explicit(&alert_ready,
                                       std::memory_order::relaxed)) {
      drain_alerts(session, inbound);
      std::atomic_flag_clear_explicit(&alert_ready, std::memory_order::relaxed);
    }
    if (file_monitor.wait()) {
      file_monitor.discard();
      last_change = now;
      auto current = files::list();
      send(local_changes(former, current, inbound.written, tombstones), current,
           server);
      former = std::move(current);
      forget_spent_marks(former, inbound.written, tombstones);
    }
    if (state_due({.now = now,
                   .last_state = last_state,
                   .last_change = last_change,
                   .settled = settled})) {
      last_state = now;
      server.state(reconcile::hash(former, tombstones));
    }
    publish_repairs(repairs, server, former, now);
    if (listener.receive_ready()) {
      receive_one(listener, session, server, former, tombstones, inbound,
                  repairs);
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
  // A publisher silently discards messages for a subscriber whose queue is
  // full, and the default queue is a thousand messages. One announcement per
  // file means a directory of a few thousand files overruns it long before the
  // receiver, which adds one torrent per loop iteration, has caught up, and
  // the files whose announcement was dropped are never synchronized. The high
  // water marks have to be set before the socket connects or binds.
  listener.set(zmq::sockopt::rcvhwm, announcement_backlog);

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
  client.set(zmq::sockopt::sndhwm, announcement_backlog);
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
