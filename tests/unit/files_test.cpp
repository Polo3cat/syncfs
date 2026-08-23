#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <system_error>
#include <thread>
#include <vector>

#include <files.h>
#include <gtest/gtest.h>

namespace {
void write_file(const std::filesystem::path &path, std::string_view content) {
  std::ofstream out{path};
  out << content;
}

auto keys_of(const files::file_map_t &listed)
    -> std::vector<std::filesystem::path> {
  auto keys = std::vector<std::filesystem::path>{};
  for (const auto &[path, time] : listed) {
    keys.push_back(path);
  }
  return keys;
}

auto at(int seconds) -> std::filesystem::file_time_type {
  return std::filesystem::file_time_type{} + std::chrono::seconds{seconds};
}

// Runs each test with the working directory inside a throwaway tree, which is
// the only root files::list() knows: one process, one sync root, the working
// directory (V16).
class Files : public ::testing::Test {
protected:
  Files()
      : previous{std::filesystem::current_path()},
        root{std::filesystem::temp_directory_path() / "files_test_root"} {
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "a");
    std::filesystem::current_path(root);
  }

  ~Files() override {
    std::error_code err;
    std::filesystem::current_path(previous, err);
    std::filesystem::remove_all(root, err);
  }

  std::filesystem::path previous;
  std::filesystem::path root;

  Files(Files &) = delete;
  Files(Files &&) = delete;
  auto operator=(Files &) -> Files = delete;
  auto operator=(Files &&) -> Files = delete;
};
} // namespace

TEST_F(Files, V10ListsRegularFilesOnly) {
  write_file("./f", "content");
  write_file("./a/g", "content");
  // A symlink pointing at a regular file reads as regular, so the listing has
  // to reject it on being a link rather than on what it points at: following it
  // would announce the same bytes under two paths and hand libtorrent a path it
  // must not write through.
  std::filesystem::create_symlink("./f", "./link");
  std::filesystem::create_directory_symlink("./a", "./dir_link");
  ASSERT_EQ(::mkfifo("./pipe", S_IRUSR | S_IWUSR), 0);

  const auto listed = files::list();

  ASSERT_EQ(keys_of(listed),
            (std::vector<std::filesystem::path>{"./a/g", "./f"}));
}

TEST_F(Files, V10KeysAreRelativeToTheSyncRoot) {
  write_file("./f", "content");
  write_file("./a/g", "content");

  for (const auto &[path, time] : files::list()) {
    // The same shape Monitor reports its events in and the wire carries as the
    // announced path, so the diff, the inotify batch and the digest all key on
    // one form.
    ASSERT_TRUE(path.native().starts_with("./")) << path.native();
  }
}

TEST_F(Files, V10TimeIsTheOneOnDisk) {
  write_file("./f", "content");

  const auto listed = files::list();

  ASSERT_TRUE(listed.contains("./f"));
  ASSERT_EQ(listed.at("./f"), std::filesystem::last_write_time("./f"));
}

TEST_F(Files, V11EntriesCarryTheTimeThatWasRead) {
  // The listing materializes every entry with its timestamp before filtering,
  // because views::filter caches its begin iterator and would otherwise apply
  // the predicate to one read of the entry and the transform to another. With
  // the tree changing underneath, the transform then dereferences a timestamp
  // that was never read. This exercises that race rather than proving the
  // caching: what it can catch is the listing throwing, crashing, or reporting
  // an entry whose time is the default one no file on disk has.
  constexpr size_t churned = 64;
  for (size_t i = 0; i < churned; ++i) {
    write_file(std::filesystem::path{"./a"} / std::to_string(i), "content");
  }

  auto stop = std::atomic_flag{};
  auto churn = std::thread{[&stop] -> void {
    while (!stop.test()) {
      for (size_t i = 0; i < churned; ++i) {
        const auto path = std::filesystem::path{"./a"} / std::to_string(i);
        std::error_code err;
        std::filesystem::remove(path, err);
        write_file(path, "content changed");
      }
    }
  }};

  for (int round = 0; round < 20; ++round) {
    for (const auto &[path, time] : files::list()) {
      ASSERT_FALSE(path.empty());
      ASSERT_NE(time, std::filesystem::file_time_type{}) << path.native();
    }
  }

  stop.test_and_set();
  churn.join();
}

TEST_F(Files, V66ListingSurvivesADirectoryRemovedUnderIt) {
  // The tree moving under the walk is this daemon's own workload: deleting a
  // subtree wakes the loop through inotify and the listing that follows races
  // whatever is still being unlinked. On the throwing iterator that race is a
  // dead daemon - the exception leaves the sync loop, main catches it and exits
  // failure, which is V28 working as designed and the process gone anyway (B8,
  // B16). A listing that comes back short costs one diff round instead.
  constexpr size_t dirs = 32;
  constexpr size_t per_dir = 4;
  auto churned = [](size_t i) -> std::filesystem::path {
    return std::filesystem::path{"./b"} / std::to_string(i);
  };
  auto fill = [&churned](size_t i) -> void {
    std::error_code err;
    std::filesystem::create_directories(churned(i) / "deeper", err);
    for (size_t f = 0; f < per_dir; ++f) {
      write_file(churned(i) / "deeper" / std::to_string(f), "content");
    }
  };
  for (size_t i = 0; i < dirs; ++i) {
    fill(i);
  }

  auto stop = std::atomic_flag{};
  auto churn = std::thread{[&stop, &churned, &fill] -> void {
    while (!stop.test()) {
      for (size_t i = 0; i < dirs; ++i) {
        std::error_code err;
        std::filesystem::remove_all(churned(i), err);
        fill(i);
      }
    }
  }};

  // The thread has to be joined before anything can fail the test: an early
  // return here would leave it joinable and take the whole binary down with it.
  std::string thrown;
  for (int round = 0; round < 40 && thrown.empty(); ++round) {
    try {
      for (const auto &[path, time] : files::list()) {
        EXPECT_FALSE(path.empty());
        EXPECT_NE(time, std::filesystem::file_time_type{}) << path.native();
      }
    } catch (const std::exception &e) {
      thrown = e.what();
    }
  }

  stop.test_and_set();
  churn.join();

  ASSERT_TRUE(thrown.empty()) << "the listing threw: " << thrown;
}

TEST_F(Files, DiffReportsWhatTheOtherSideDoesNotHold) {
  const auto left = files::file_map_t{{"./a", at(1)}, {"./b", at(2)}};
  const auto right = files::file_map_t{{"./a", at(1)}};

  ASSERT_EQ(files::diff(left, right), (files::file_map_t{{"./b", at(2)}}));
}

TEST_F(Files, DiffReportsAPathWhoseTimeChanged) {
  const auto left = files::file_map_t{{"./a", at(2)}};
  const auto right = files::file_map_t{{"./a", at(1)}};

  // Same path, later time: this is what makes an edit a change rather than a
  // path both sides already have.
  ASSERT_EQ(files::diff(left, right), (files::file_map_t{{"./a", at(2)}}));
}

TEST_F(Files, DiffNameIgnoresTheTime) {
  const auto left = files::file_map_t{{"./a", at(2)}};
  const auto right = files::file_map_t{{"./a", at(1)}};

  // Which paths appeared or vanished, whatever their timestamps do: this is the
  // created and removed halves of the diff.
  ASSERT_TRUE(files::diff_name(left, right).empty());
  ASSERT_EQ(files::diff_name(files::file_map_t{{"./b", at(1)}}, left),
            (files::file_map_t{{"./b", at(1)}}));
}

TEST_F(Files, IntersectionNameKeepsTheLeftHandTime) {
  const auto left = files::file_map_t{{"./a", at(1)}};
  const auto right = files::file_map_t{{"./a", at(2)}};

  // V13 rests on this: the modified set is built from the removed side, so its
  // entries carry the timestamp the file had before the change. Anything
  // reading a modified entry's time and expecting the current one - the echo
  // check of V36, the origin stamp of V48 - has to read the fresh listing
  // instead.
  ASSERT_EQ(files::intersection_name(left, right),
            (files::file_map_t{{"./a", at(1)}}));
}
