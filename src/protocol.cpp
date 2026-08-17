#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/fwd.hpp>
#include <libtorrent/load_torrent.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/torrent_flags.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/torrent_status.hpp>

#include <zmq.hpp>

#include <files.h>
#include <protocol.h>
#include <reconcile.h>
#include <utils.h>

namespace {
auto file_path(const std::shared_ptr<const lt::torrent_info> &ti)
    -> std::string {
  const auto &layout = ti->layout();
  auto file_index = *(layout.file_range().begin());
  return layout.file_path(file_index);
}

// Where a torrent's single file sits inside the sync root. A v2-only torrent
// holding one file under one directory drops that directory on load, so what
// the torrent still knows is only the tail of the path and the rest of it has
// to be read back out of the save path.
auto join(const std::filesystem::path &root, const std::string &save_path,
          const std::shared_ptr<const lt::torrent_info> &ti)
    -> std::filesystem::path {
  return (std::filesystem::path{save_path}.lexically_relative(root) /
          file_path(ti))
      .lexically_normal();
}

// The directory a torrent has to be written into for its file to land where
// the sender holds it. Whatever the load dropped is exactly what the save path
// puts back, so this is the tail of the announced path removed from the whole
// of it.
auto save_path_for(const std::filesystem::path &announced,
                   const std::filesystem::path &in_torrent) -> std::string {
  const auto whole = announced.lexically_normal().native();
  const auto tail = in_torrent.lexically_normal().native();
  if (whole.size() < tail.size() || !whole.ends_with(tail)) {
    return ".";
  }
  auto directory =
      std::string_view{whole}.substr(0, whole.size() - tail.size());
  while (directory.ends_with('/')) {
    directory.remove_suffix(1);
  }
  return directory.empty() ? "." : std::string{directory};
}

// The torrents serving one file, named by the listing key the rest of the
// daemon speaks in ("./a/f"). One session call answers for every torrent at
// once, which a per-handle lookup of the save path would not.
auto torrents_at(lt::session &s, const std::filesystem::path &path)
    -> std::vector<lt::torrent_handle> {
  const auto wanted = path.lexically_normal();
  const auto root = std::filesystem::current_path();
  std::vector<lt::torrent_handle> found;
  const auto all =
      s.get_torrent_status([](const auto &) -> bool { return true; },
                           lt::torrent_handle::query_save_path |
                               lt::torrent_handle::query_torrent_file);
  for (const auto &status : all) {
    const auto info = status.torrent_file.lock();
    if (info && join(root, status.save_path, info) == wanted) {
      found.push_back(status.handle);
    }
  }
  return found;
}

// A file update arrives as a fresh torrent for a path we may already serve.
// The content changed, so the info hash changed, and add_torrent() only
// deduplicates by info hash: without this the old torrent stays in the session
// and keeps seeding stale bytes for the very path the new one writes to.
// Identical announcements are left alone, since removing and re-adding them
// would only force a recheck and drop the swarm.
void remove_stale_torrents(lt::session &s,
                           const std::filesystem::path &announced,
                           const lt::add_torrent_params &added) {
  for (const auto &handle : torrents_at(s, announced)) {
    const auto info = handle.torrent_file();
    if (info->info_hashes() == added.ti->info_hashes()) {
      continue;
    }
    // No delete_files: the new torrent overwrites the file in place.
    s.remove_torrent(handle);
  }
}

// Whether an announced file beats the copy already here. Newer wins, whatever
// the copy here came from. On a tie the lower info hash wins, which is a
// property of the content and so the only thing both ends can agree on from
// the message alone: neither a create nor a remove carries a sender.
//
// A tie is only settled that way when this node knows which announcement its
// copy came from and that announcement describes the file it has now. A copy
// it cannot account for is one it is not seeding, so taking the announcement
// is how it starts.
auto wins(const protocol::applied_map_t &applied,
          const std::filesystem::path &announced,
          std::filesystem::file_time_type origin,
          const lt::torrent_info &incoming) -> bool {
  std::error_code err;
  const auto here = std::filesystem::last_write_time(announced, err);
  if (err) {
    return true;
  }
  const auto theirs = utils::to_ticks(origin);
  const auto ours = utils::to_ticks(here);
  if (theirs != ours) {
    return theirs > ours;
  }
  const auto known = applied.find(announced);
  if (known == applied.end() || utils::to_ticks(known->second.origin) != ours) {
    return true;
  }
  return !(known->second.content < incoming.info_hashes());
}

// The part count is a property of the verb, not of the wire: a blanket check
// would let a create truncated to its torrent through, and reject the very
// message that carries an origin time.
struct verb_t {
  std::string_view name;
  size_t parts;
};

constexpr size_t remove_parts = 3;

constexpr std::array<verb_t, 4> verbs{
    {{.name = "create", .parts = 4},
     {.name = "remove", .parts = remove_parts},
     {.name = "state", .parts = 3},
     {.name = "digest", .parts = 4}}};

// The first part carries the verb and, for an addressed one, the endpoint it is
// meant for. Comparing the whole of it would have every addressed digest report
// a verb nobody knows, so the verb is what stands before the first NUL (V3,
// V58).
auto parts_for(std::string_view part0) -> std::optional<size_t> {
  const auto *found =
      std::ranges::find(verbs, utils::verb_of(part0), &verb_t::name);
  if (found == verbs.end()) {
    return std::nullopt;
  }
  return found->parts;
}

auto add_nodes(lt::session &s,
               const std::vector<std::pair<std::string, int>> &v)
    -> std::string {
  std::string added;
  for (const auto &node : v) {
    s.add_dht_node(node);
    added = std::format("{} {}:{}", added, node.first, node.second);
  }
  return added;
}
} // namespace

namespace protocol {

void subscribe(zmq::socket_t &s, std::string_view endpoint) {
  s.set(zmq::sockopt::subscribe, "create");
  s.set(zmq::sockopt::subscribe, "remove");
  s.set(zmq::sockopt::subscribe, "state");
  // Addressed, and framed at both ends: the publisher matches this against the
  // first part of every digest and writes it to this pipe alone. Subscribing to
  // the bare verb here would take every peer's digest as well, which is the
  // whole of what V56 is about.
  s.set(zmq::sockopt::subscribe,
        utils::address("digest", utils::Endpoint{endpoint}));
}

auto act(const std::vector<zmq::message_t> &v, lt::session &s,
         reconcile::tombstone_map_t &tombstones, const applied_map_t &applied)
    -> std::expected<outcome, std::string> {
  if (v.empty()) {
    return std::unexpected{"Wrong protocol length."};
  }
  const auto verb = utils::verb_of(v.at(0).to_string_view());
  // The verb has to be read before the length can be judged at all.
  const auto parts = parts_for(v.at(0).to_string_view());
  if (!parts.has_value()) {
    return std::unexpected{"Wrong protocol verb."};
  }
  if (v.size() != *parts) {
    return std::unexpected{"Wrong protocol length."};
  }
  if (const auto removed = removed_path(v)) {
    erase(s, *removed);
    // Recorded even for a path this node never held: the peer that still
    // holds it has to be told, and it can only be told by a node that
    // remembers the deletion.
    reconcile::mark(tombstones, *removed,
                    utils::from_ticks(v.at(2).to_string_view()));
    return outcome{.verb = std::string{verb},
                   .message = std::format("Delete \"{}\"", removed->native())};
  }
  if (verb == "create") {
    // The announced path is the whole of it; the torrent may only still know
    // its tail, so the two together decide where the file is written.
    const auto announced = std::filesystem::path{v.at(3).to_string_view()};
    const auto origin =
        utils::to_file_time(utils::from_ticks(v.at(2).to_string_view()));

    // Delete against create: the deletion wins only if it is strictly newer,
    // and then the file is neither written nor added to the session. A file
    // that is newer cancels the deletion instead, so the next deletion of that
    // path is a fresh one and travels normally.
    if (const auto grave = tombstones.find(announced);
        grave != tombstones.end()) {
      if (reconcile::beats(grave->second, origin)) {
        return outcome{.verb = std::string{verb},
                       .message = std::format("Ignore \"{}\", deleted since",
                                              announced.native())};
      }
      tombstones.erase(grave);
    }

    auto torrent = lt::load_torrent_buffer(v.at(1).to_string_view());
    const auto content = torrent.ti->info_hashes();

    // Create against create. Under repair the same path is announced over and
    // over, and last received wins is an oscillator: two nodes holding
    // different copies would each apply the other's every round, for ever.
    // Which of the two is newer settles it, and the tie is settled by content
    // rather than by identity, because neither message carries a sender.
    if (!wins(applied, announced, origin, *torrent.ti)) {
      return outcome{.verb = std::string{verb},
                     .message = std::format("Ignore \"{}\", not newer",
                                            announced.native())};
    }

    torrent.save_path = save_path_for(announced, file_path(torrent.ti));
    torrent.flags = lt::torrent_flags::auto_managed;

    remove_stale_torrents(s, announced, torrent);

    auto handle = s.add_torrent(torrent);

    const std::string nodes = add_nodes(s, torrent.dht_nodes);

    handle.force_dht_announce();

    const auto *state = "Added nodes for";
    return outcome{.verb = std::string{verb},
                   .message =
                       std::format("{} \"{}\"{}", state, announced.native(),
                                   nodes.empty() ? "" : nodes),
                   .created = announced,
                   .origin = origin,
                   .content = content};
  }
  // state and digest belong to the reconciliation, which reads them for
  // itself once the verb has been judged well formed here.
  return outcome{.verb = std::string{verb},
                 .message = std::format("Nothing to do for \"{}\"", verb)};
}

void erase(lt::session &s, const std::filesystem::path &path) {
  // The torrent goes first: unlinking the file underneath a live torrent
  // leaves it seeding, and erroring on, a path that is no longer there.
  for (const auto &handle : torrents_at(s, path)) {
    s.remove_torrent(handle);
  }
  std::filesystem::remove(path);
}

auto held_path(const lt::torrent_handle &h)
    -> std::optional<std::filesystem::path> {
  const auto status = h.status(lt::torrent_handle::query_save_path |
                               lt::torrent_handle::query_torrent_file);
  const auto info = status.torrent_file.lock();
  if (!info) {
    return std::nullopt;
  }
  // In the key form files::list() reports, which walks "." and so prefixes
  // every path with it. A caller matching a torrent against that listing needs
  // the same spelling, not the normalized one the comparisons here use.
  return std::filesystem::path{"."} /
         join(std::filesystem::current_path(), status.save_path, info);
}

auto removed_path(const std::vector<zmq::message_t> &v)
    -> std::optional<std::filesystem::path> {
  if (v.size() != remove_parts ||
      utils::verb_of(v.at(0).to_string_view()) != "remove") {
    return std::nullopt;
  }
  return std::filesystem::path{v.at(1).to_string_view()};
}

void repairs_t::forget(const std::filesystem::path &path) {
  announcements.erase(path);
  applied.erase(path);
  pending.erase(path);
}

auto adopt(lt::session &s, const reconcile::tombstone_map_t &theirs,
           files::file_map_t &former, reconcile::tombstone_map_t &mine,
           repairs_t &repairs, reconcile::time_point now)
    -> std::vector<std::filesystem::path> {
  std::vector<std::filesystem::path> deleted;
  for (const auto &[path, at] :
       reconcile::adoptable(theirs, former, mine, now)) {
    reconcile::mark(mine, path, at);
    if (former.erase(path) == 0) {
      continue;
    }
    erase(s, path);
    // The listing no longer has the path, so nothing will ever look at what the
    // repair cache still remembers it by, and nothing would ever drop it
    // either.
    repairs.forget(path);
    deleted.push_back(path);
  }
  return deleted;
}

} // namespace protocol