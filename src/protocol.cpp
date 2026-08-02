#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <boost/asio/ip/tcp.hpp>
#include <boost/system/system_error.hpp>
#include <libtorrent/fwd.hpp>
#include <libtorrent/load_torrent.hpp>
#include <libtorrent/pex_flags.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/socket.hpp>
#include <libtorrent/torrent_flags.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/torrent_status.hpp>
#include <spdlog/spdlog.h>
#include <zmq.hpp>

#include <protocol.h>

namespace {
auto file_path(const std::shared_ptr<const lt::torrent_info> &ti)
    -> std::string {
  const auto &layout = ti->layout();
  auto file_index = *(layout.file_range().begin());
  return layout.file_path(file_index);
}

auto can_add_peers(const lt::torrent_handle &h) -> bool {
  const auto status = h.status({});
  return status.state == lt::torrent_status::downloading ||
         status.state == lt::torrent_status::seeding ||
         status.state == lt::torrent_status::finished;
}

auto add_peers(lt::torrent_handle &h, const std::vector<lt::tcp::endpoint> &v)
    -> std::string {
  std::string added;
  for (const auto &peer : v) {
    try {
      h.connect_peer(peer, {}, lt::pex_seed);
    } catch (const boost::system::system_error &e) {
      spdlog::debug("Couldn't connect to peer. {}", e.what());
      continue;
    }
    added =
        std::format("{} {}:{}", added, peer.address().to_string(), peer.port());
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

    auto handle = s.find_torrent(torrent.info_hashes.v2);
    const auto *state = handle.is_valid() ? "Added peers for" : "Create";

    std::string peers;
    if (handle.is_valid()) {
      if (can_add_peers(handle)) {
        peers = add_peers(handle, torrent.peers);
      }
    } else {
      handle = s.add_torrent(torrent);
    }

    return std::format("{} \"{}\"{}", state, file_path(handle.torrent_file()),
                       peers.empty() ? "" : peers);
  }
  return std::unexpected{"Wrong protocol verb."};
}

} // namespace protocol