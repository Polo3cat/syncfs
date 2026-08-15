#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <libtorrent/create_torrent.hpp>
#include <spdlog/spdlog.h>
#include <zmq.hpp>

#include <source.h>
#include <utils.h>

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

// Puts a create on the wire. The origin time travels beside the torrent
// rather than inside it: libtorrent would fold it into the info dictionary,
// and then identical content written at two different moments would hash
// differently.
void announce(zmq::socket_t &client, std::string_view bencoded_torrent,
              std::filesystem::file_time_type mtime,
              const std::filesystem::path &file) {
  const auto origin = std::format("{}", utils::to_ticks(mtime));
  const auto &path = file.native();

  client.send(zmq::str_buffer("create"), zmq::send_flags::sndmore);
  client.send(
      zmq::const_buffer(bencoded_torrent.data(), bencoded_torrent.size()),
      zmq::send_flags::sndmore);
  client.send(zmq::const_buffer(origin.data(), origin.size()),
              zmq::send_flags::sndmore);
  client.send(zmq::const_buffer(path.data(), path.size()));
}
} // namespace

namespace source {
void Source::create(const std::filesystem::path &file,
                    std::filesystem::file_time_type mtime) const {
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
  const auto bencoded = torrent.generate_buf();
  announce(client, {bencoded.data(), bencoded.size()}, mtime, file);
}

void Source::repair(const std::filesystem::path &file,
                    std::filesystem::file_time_type mtime,
                    std::string_view bencoded) const {
  spdlog::debug("-> Repair {}", file.native());

  // The very bytes the path was last announced with, so the info hash is the
  // one every peer already knows and a duplicate repair costs nothing.
  announce(client, bencoded, mtime, file);
}

void Source::remove(const std::filesystem::path &file) const {
  spdlog::debug("-> Remove {}", file.native());

  // The file is gone, so there is no timestamp left to read: what orders this
  // deletion is the moment it was noticed.
  const auto deleted =
      std::format("{}", utils::to_ticks(std::chrono::system_clock::now()));
  const auto &path = file.native();

  client.send(zmq::str_buffer("remove"), zmq::send_flags::sndmore);
  client.send(zmq::const_buffer(path.data(), path.size()),
              zmq::send_flags::sndmore);
  client.send(zmq::const_buffer(deleted.data(), deleted.size()));
}

void Source::state(std::string_view hashes) const {
  spdlog::debug("-> State");

  client.send(zmq::str_buffer("state"), zmq::send_flags::sndmore);
  client.send(zmq::const_buffer(hashes.data(), hashes.size()));
}

void Source::digest(std::string_view held, std::string_view deleted) const {
  spdlog::debug("-> Digest");

  client.send(zmq::str_buffer("digest"), zmq::send_flags::sndmore);
  client.send(zmq::const_buffer(endpoint.data(), endpoint.size()),
              zmq::send_flags::sndmore);
  client.send(zmq::const_buffer(held.data(), held.size()),
              zmq::send_flags::sndmore);
  client.send(zmq::const_buffer(deleted.data(), deleted.size()));
}
} // namespace source