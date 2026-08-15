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
#include <utility>
#include <vector>

#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/fwd.hpp>
#include <libtorrent/load_torrent.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/torrent_flags.hpp>
#include <libtorrent/torrent_info.hpp>

#include <zmq.hpp>

#include <protocol.h>

namespace {
auto file_path(const std::shared_ptr<const lt::torrent_info> &ti)
    -> std::string {
  const auto &layout = ti->layout();
  auto file_index = *(layout.file_range().begin());
  return layout.file_path(file_index);
}

// The torrents serving one file. A torrent carries its path normalized
// ("a/f"), while the rest of the daemon speaks in listing keys ("./a/f").
auto torrents_at(lt::session &s, const std::filesystem::path &path)
    -> std::vector<lt::torrent_handle> {
  const auto wanted = path.lexically_normal();
  std::vector<lt::torrent_handle> found;
  for (const auto &handle : s.get_torrents()) {
    const auto info = handle.torrent_file();
    if (info && std::filesystem::path{file_path(info)} == wanted) {
      found.push_back(handle);
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
                           const lt::add_torrent_params &added) {
  for (const auto &handle : torrents_at(s, file_path(added.ti))) {
    const auto info = handle.torrent_file();
    if (info->info_hashes() == added.ti->info_hashes()) {
      continue;
    }
    // No delete_files: the new torrent overwrites the file in place.
    s.remove_torrent(handle);
  }
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
     {.name = "state", .parts = 2},
     {.name = "digest", .parts = 4}}};

auto parts_for(std::string_view verb) -> std::optional<size_t> {
  const auto *found = std::ranges::find(verbs, verb, &verb_t::name);
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

auto act(const std::vector<zmq::message_t> &v, lt::session &s)
    -> std::expected<std::string, std::string> {
  if (v.empty()) {
    return std::unexpected{"Wrong protocol length."};
  }
  const auto verb = v.at(0).to_string_view();
  // The verb has to be read before the length can be judged at all.
  const auto parts = parts_for(verb);
  if (!parts.has_value()) {
    return std::unexpected{"Wrong protocol verb."};
  }
  if (v.size() != *parts) {
    return std::unexpected{"Wrong protocol length."};
  }
  if (const auto removed = removed_path(v)) {
    // The torrent goes first: unlinking the file underneath a live torrent
    // leaves it seeding, and erroring on, a path that is no longer there.
    for (const auto &handle : torrents_at(s, *removed)) {
      s.remove_torrent(handle);
    }
    std::filesystem::remove(*removed);
    return std::format("Delete \"{}\"", removed->native());
  }
  if (verb == "create") {
    auto torrent = lt::load_torrent_buffer(v.at(1).to_string_view());
    torrent.save_path = ".";
    torrent.flags = lt::torrent_flags::auto_managed;

    remove_stale_torrents(s, torrent);

    auto handle = s.add_torrent(torrent);

    const std::string nodes = add_nodes(s, torrent.dht_nodes);

    handle.force_dht_announce();

    const auto *state = "Added nodes for";
    return std::format("{} \"{}\"{}", state, file_path(handle.torrent_file()),
                       nodes.empty() ? "" : nodes);
  }
  // state and digest belong to the reconciliation, which reads them once it
  // exists. Until then they are well formed and carry nothing to do.
  return std::format("Nothing to do for \"{}\"", verb);
}

auto removed_path(const std::vector<zmq::message_t> &v)
    -> std::optional<std::filesystem::path> {
  if (v.size() != remove_parts || v.at(0).to_string_view() != "remove") {
    return std::nullopt;
  }
  return std::filesystem::path{v.at(1).to_string_view()};
}

} // namespace protocol