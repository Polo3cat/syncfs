#include <filesystem>
#include <fstream>
#include <utility>

#include <gtest/gtest.h>
#include <libtorrent/entry.hpp>
#include <libtorrent/load_torrent.hpp>
#include <sink.h>
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
TEST_F(TempFile, CreateSendsTorrent) {
  zmq::context_t ctx;
  zmq::socket_t s{ctx, zmq::socket_type::pub};
  s.bind("tcp://127.0.0.1:3000");
  constexpr auto sink_address = 3000;
  auto sender = sink::Sink(std::move(s), std::pair{"127.0.0.1", sink_address});

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

  // Assertions on the torrent file
  auto torrent = lt::load_torrent_buffer(recv_msgs.at(1).to_string_view());
  ASSERT_EQ(1, torrent.ti->num_files());
  auto file_index = *(torrent.ti->layout().file_range().begin());
  auto file_name = torrent.ti->layout().file_name(file_index);
  ASSERT_EQ(file_name, "important_file");
}
