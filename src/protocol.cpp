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
  if (v.size() != 2) {
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
  auto action = v.at(0).to_string_view();
  if (action == "create") {
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
  return std::unexpected{"Wrong protocol verb."};
}

auto removed_path(const std::vector<zmq::message_t> &v)
    -> std::optional<std::filesystem::path> {
  if (v.size() != length || v.at(0).to_string_view() != "remove") {
    return std::nullopt;
  }
  return std::filesystem::path{v.at(1).to_string_view()};
}

} // namespace protocol