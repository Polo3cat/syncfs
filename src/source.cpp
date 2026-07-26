#include <cstdint>
#include <filesystem>
#include <libtorrent/create_torrent.hpp>
#include <spdlog/spdlog.h>
#include <system_error>
#include <utility>
#include <vector>
#include <zmq.hpp>

#include "source.h"

namespace source {
void Source::create(const std::filesystem::path &file) const {
  spdlog::debug("-> Create {}", file.native());

  std::error_code err{};
  const std::filesystem::directory_entry entry(file, err);
  if (err) {
    spdlog::error("Failed to send create for file {}. {} {}", file.c_str(),
                  err.value(), err.message());
    return;
  }

  auto file_entry =
      lt::create_file_entry{file, static_cast<int64_t>(entry.file_size())};
  lt::create_torrent torrent(std::vector{std::move(file_entry)});
  torrent.add_node(addr);
  // Expensive operation. Reads disk and hashes the file contents.
  lt::set_piece_hashes(torrent, file.parent_path());
  auto bencoded_torrent = torrent.generate_buf();

  client.send(zmq::str_buffer("create"), zmq::send_flags::sndmore);
  client.send(
      zmq::const_buffer(bencoded_torrent.data(), bencoded_torrent.size()));
}

void Source::remove(const std::filesystem::path &file) const {
  spdlog::debug("-> Remove {}", file.native());

  client.send(zmq::str_buffer("remove"), zmq::send_flags::sndmore);
  client.send(zmq::const_buffer(file.native().c_str(), file.native().size()));
}
} // namespace source