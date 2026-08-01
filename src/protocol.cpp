#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

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
    torrent.flags =
        lt::torrent_flags::auto_managed | lt::torrent_flags::auto_managed;

    auto dup_handle = s.find_torrent(torrent.info_hashes.v2);
    const auto *state =
        dup_handle.is_valid() ? "Existed torrent for" : "Create torrent for";

    auto handle = s.add_torrent(torrent);

    return std::format("{} \"{}\"", state, file_path(handle.torrent_file()));
  }
  return std::unexpected{"Wrong protocol verb."};
}

} // namespace protocol