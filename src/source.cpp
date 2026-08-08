#include <cstdint>
#include <filesystem>
#include <system_error>
#include <utility>
#include <vector>

#include <libtorrent/create_torrent.hpp>
#include <spdlog/spdlog.h>
#include <zmq.hpp>

#include <source.h>

namespace {
auto torrent_from_file(const std::filesystem::directory_entry &file)
    -> lt::create_torrent {
  auto file_entry = lt::create_file_entry{
      file.path().lexically_normal(), static_cast<int64_t>(file.file_size())};
  lt::create_torrent torrent(std::vector{std::move(file_entry)}, 0,
                             lt::create_torrent::v2_only);
  // Expensive operation. Reads disk and hashes the file contents.
  // The entry holds the whole path relative to the sync root, and libtorrent
  // resolves it against this directory, so anything but the root itself would
  // apply the parent twice and look for "./a/a/f.txt".
  lt::set_piece_hashes(torrent, ".");
  return torrent;
}
} // namespace

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

  auto torrent = torrent_from_file(entry);
  torrent.add_node(addr);
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