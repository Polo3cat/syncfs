#include <filesystem>
#include <fstream>
#include <utility>

#include <gtest/gtest.h>
#include <libtorrent/bdecode.hpp>
#include <source.h>
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
  zmq::context_t ctx;
  zmq::socket_t s{ctx, zmq::socket_type::pub};
  s.bind("tcp://127.0.0.1:3000");
  constexpr auto source_address = 3000;
  auto sender =
      source::Source(std::move(s), std::pair{"127.0.0.1", source_address});

  zmq::context_t ctx2;
  zmq::socket_t receiver{ctx2, zmq::socket_type::sub};
  receiver.connect("tcp://127.0.0.1:3000");
  receiver.set(zmq::sockopt::subscribe, "create");

  sender.create(this->file);

  std::array<zmq::message_t, 2> recv_msgs{};
  zmq::recv_result_t res{};
  res = zmq::recv_multipart_n(receiver, recv_msgs.begin(), recv_msgs.size());

  // Assertions on the protocol.
  ASSERT_TRUE(res.has_value());
  ASSERT_EQ(res.value(), 2);
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
