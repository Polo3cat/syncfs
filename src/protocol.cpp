#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
  auto action = v.at(0).to_string_view();
  if (action == "remove") {
    auto filename = v.at(1).to_string_view();
    std::filesystem::remove(filename);
    return std::format("Delete \"{}\"", std::string{filename});
  }
  if (action == "create") {
    auto torrent = lt::load_torrent_buffer(v.at(1).to_string_view());
    torrent.save_path = ".";
    torrent.flags = lt::torrent_flags::auto_managed;

    auto handle = s.add_torrent(torrent);

    const std::string nodes = add_nodes(s, torrent.dht_nodes);

    handle.force_dht_announce();

    const auto *state = "Added nodes for";
    return std::format("{} \"{}\"{}", state, file_path(handle.torrent_file()),
                       nodes.empty() ? "" : nodes);
  }
  return std::unexpected{"Wrong protocol verb."};
}

} // namespace protocol