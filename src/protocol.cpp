#include <expected>
#include <filesystem>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include <libtorrent/load_torrent.hpp>
#include <libtorrent/session.hpp>
#include <zmq.hpp>

#include <protocol.h>

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

    const auto &layout = torrent.ti->layout();
    auto file_index = *(layout.file_range().begin());
    auto filename = layout.file_path(file_index);

    s.add_torrent(std::move(torrent));

    return std::format("Create \"{}\"", filename);
  }
  return std::unexpected{"Wrong protocol verb."};
}

} // namespace protocol