#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

#include <gtest/gtest.h>
#include <libtorrent/bdecode.hpp>
#include <source.h>
#include <utils.h>
#include <zmq.hpp>
#include <zmq_addon.hpp>

using namespace std::string_view_literals;

namespace {
class TempFile : public ::testing::Test {
protected:
  TempFile() : file{std::filesystem::temp_directory_path() / "important_file"} {
    std::ofstream ofs(file);
    ofs << "Important file content\n";
    ofs << "Important file content\n";
    ofs << "Important file content\n";
    ofs << "Important file content\n";
    ofs.close();
  }
  ~TempFile() override { std::filesystem::remove(file); }

  std::filesystem::path file;

  TempFile(TempFile &) = delete;
  TempFile(TempFile &&) = delete;
  auto operator=(TempFile &) -> TempFile = delete;
  auto operator=(TempFile &&) -> TempFile = delete;
};

constexpr int poll_ms = 50;
constexpr int attempts = 100;

// The publisher and subscriber a test talks over.
struct Wire {
  zmq::context_t publisher_ctx;
  zmq::context_t subscriber_ctx;
  zmq::socket_t publisher;
  zmq::socket_t subscriber;

  Wire(const std::string &address, const std::string &verb)
      : publisher{publisher_ctx, zmq::socket_type::pub},
        subscriber{subscriber_ctx, zmq::socket_type::sub} {
    publisher.bind(address);
    subscriber.connect(address);
    subscriber.set(zmq::sockopt::subscribe, verb);
    subscriber.set(zmq::sockopt::rcvtimeo, poll_ms);
  }
};

// A publisher drops whatever it sends before a subscriber has finished
// connecting, and nothing tells either end when that has happened, so the
// announcement is repeated until one of them lands.
template <size_t N, typename Announce>
auto announce_until_received(zmq::socket_t &subscriber,
                             std::array<zmq::message_t, N> &parts,
                             Announce announce) -> bool {
  for (int attempt = 0; attempt < attempts; ++attempt) {
    announce();
    if (zmq::recv_multipart_n(subscriber, parts.begin(), parts.size())) {
      return true;
    }
  }
  return false;
}
} // namespace

// For debugging remember to call
// settings set target.disable-aslr false
// on lldb

/** Explores a generated torrent file to assert
 * all it's values are correct. A torrent file
 * is nothing more than a few fields with metainformation
 * and a tree (dictionary data structure) describing
 * the contained files. Here's an example of what
 * a tree looks like:
 * {
  info: {
    file tree: {
      dir1: {
        dir2: {
          fileA.txt: {
            "": {
              length: <length of file in bytes (integer)>,
              pieces root: <optional, merkle tree root (string)>,
              ...
            }
          },
          fileB.txt: {
            "": {
              ...
            }
          }
        },
        dir3: {
          ...
        }
      }
    }
  }
}
 */
TEST_F(TempFile, CreateSendsTorrent) {
  constexpr auto source_address = 3000;
  Wire wire{"tcp://127.0.0.1:3000", "create"};
  auto sender = source::Source(std::move(wire.publisher),
                               std::pair{"127.0.0.1", source_address});

  std::array<zmq::message_t, 4> recv_msgs{};
  ASSERT_TRUE(announce_until_received(wire.subscriber, recv_msgs, [&] -> void {
    sender.create(this->file, std::filesystem::last_write_time(this->file));
  }));

  // Assertions on the protocol.
  ASSERT_EQ(recv_msgs.at(0).to_string_view(), "create"sv);

  lt::error_code err{};
  const auto decoded = lt::bdecode(recv_msgs.at(1).to_string_view(), err);
  ASSERT_FALSE(err);

  const auto info = decoded.dict_find_dict("info");
  ASSERT_TRUE(info);
  ASSERT_EQ(info.dict_find_int_value("meta version"), 2);

  const auto tree = info.dict_find_dict("file tree");
  ASSERT_TRUE(tree);
  ASSERT_EQ(tree.dict_size(), 1);

  const auto name = info.dict_find_string_value("name");
  const auto entry = tree.dict_at(0);
  const auto file_name = entry.first;

  ASSERT_EQ(entry.second.dict_size(), 1);
  ASSERT_TRUE(entry.second.dict_at(0).first.empty());

  ASSERT_FALSE(name.empty());
  ASSERT_NE(name.front(), '/');
  ASSERT_EQ(file_name, "important_file");
  ASSERT_EQ(std::filesystem::path(name) / file_name, "tmp/important_file");
}

TEST_F(TempFile, V48CreateCarriesOriginMtimeAndPath) {
  constexpr auto source_address = 3001;
  Wire wire{"tcp://127.0.0.1:3001", "create"};
  auto sender = source::Source(std::move(wire.publisher),
                               std::pair{"127.0.0.1", source_address});

  const auto mtime = std::filesystem::last_write_time(this->file);

  std::array<zmq::message_t, 4> recv_msgs{};
  ASSERT_TRUE(announce_until_received(wire.subscriber, recv_msgs, [&] -> void {
    sender.create(this->file, mtime);
  }));

  // The origin time is the one this node observed, not the one the receiver
  // will see, and it travels as system_clock ticks because the epoch of a
  // file timestamp is nobody's business but this machine's.
  ASSERT_EQ(recv_msgs.at(2).to_string(),
            std::to_string(utils::to_ticks(mtime)));
  // The path the sender holds it under, which the receiver writes it to.
  ASSERT_EQ(recv_msgs.at(3).to_string(), this->file.native());
}

TEST_F(TempFile, RemoveCarriesDeleteTime) {
  constexpr auto source_address = 3002;
  Wire wire{"tcp://127.0.0.1:3002", "remove"};
  auto sender = source::Source(std::move(wire.publisher),
                               std::pair{"127.0.0.1", source_address});

  const auto before = utils::to_ticks(std::chrono::system_clock::now());

  std::array<zmq::message_t, 3> recv_msgs{};
  ASSERT_TRUE(announce_until_received(
      wire.subscriber, recv_msgs, [&] -> void { sender.remove(this->file); }));

  ASSERT_EQ(recv_msgs.at(0).to_string_view(), "remove"sv);
  ASSERT_EQ(recv_msgs.at(1).to_string(), this->file.native());

  // The file is gone by then, so the time is the moment of the deletion, taken
  // when it was noticed.
  const auto deleted = std::stoll(recv_msgs.at(2).to_string());
  const auto after = utils::to_ticks(std::chrono::system_clock::now());
  ASSERT_GE(deleted, before);
  ASSERT_LE(deleted, after);
}
