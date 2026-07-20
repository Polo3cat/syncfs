#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <libtorrent/create_torrent.hpp>
#include <spdlog/spdlog.h>
#include <system_error>
#include <utility>
#include <vector>
#include <zmq.hpp>

#include "sink.h"

namespace sink {
void Sink::create(const std::filesystem::path &file) const {
  spdlog::debug("-> Create {}", file.native());

  std::error_code err{};
  const std::filesystem::directory_entry entry(file, err);
  if (err) {
    spdlog::error("Failed to send create for file {}. {} {}", file.c_str(),
                  err.value(), err.message());
    return;
  }

  lt::create_torrent torrent(std::vector{
      lt::create_file_entry{file, static_cast<int64_t>(entry.file_size())}});
  torrent.add_node(addr);

  static constexpr size_t buf_size = 4ULL * 1024;
  std::array<char, buf_size> file_buf{};

  auto file_stream =
      std::fstream{file, std::ios_base::in | std::ios_base::binary};
  file_stream.read(file_buf.data(), file_buf.size());

  client.send(zmq::str_buffer("create"), zmq::send_flags::sndmore);
  client.send(zmq::const_buffer(file.native().c_str(), file.native().size()),
              zmq::send_flags::sndmore);
  client.send(zmq::const_buffer(file_buf.data(), file_stream.gcount()));
}

void Sink::remove(const std::filesystem::path &file) const {
  spdlog::debug("-> Remove {}", file.native());

  client.send(zmq::str_buffer("remove"), zmq::send_flags::sndmore);
  client.send(zmq::const_buffer(file.native().c_str(), file.native().size()));
}

void Sink::update(const std::filesystem::path &file) const {
  spdlog::debug("-> Update {}", file.native());

  static constexpr size_t buf_size = 4ULL * 1024;
  std::array<char, buf_size> file_buf{};

  auto file_stream =
      std::fstream{file, std::ios_base::in | std::ios_base::binary};
  file_stream.read(file_buf.data(), file_buf.size());

  client.send(zmq::str_buffer("update"), zmq::send_flags::sndmore);
  client.send(zmq::const_buffer(file.native().c_str(), file.native().size()),
              zmq::send_flags::sndmore);
  client.send(zmq::const_buffer(file_buf.data(), file_stream.gcount()));
}
} // namespace sink