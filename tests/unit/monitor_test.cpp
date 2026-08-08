#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <linux/limits.h>
#include <string>
#include <sys/inotify.h>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>
#include <monitor.h>

namespace {
// Monitor::wait() polls with a 50 ms timeout, so this is a 2 s ceiling.
constexpr int max_polls = 40;

auto wait_for_event(monitor::Monitor &m) -> bool {
  for (int i = 0; i < max_polls; ++i) {
    if (m.wait()) {
      return true;
    }
  }
  return false;
}

// The directories files::list() would descend into, sorted the same way
// Monitor::watched() sorts. Symlinked directories are excluded because
// recursive_directory_iterator does not follow them by default.
auto directories_below_root() -> std::vector<std::filesystem::path> {
  auto dirs = std::vector<std::filesystem::path>{"."};
  for (const auto &entry : std::filesystem::recursive_directory_iterator(".")) {
    if (entry.is_directory() && !entry.is_symlink()) {
      dirs.push_back(entry.path());
    }
  }
  std::ranges::sort(dirs);
  return dirs;
}

auto is_watched(const monitor::Monitor &m, const std::filesystem::path &dir)
    -> bool {
  const auto dirs = m.watched();
  return std::ranges::find(dirs, dir) != dirs.end();
}

// Runs each test inside a throwaway tree "./a/b" with the working directory
// moved into its root, which is what Monitor watches.
class Monitor : public ::testing::Test {
protected:
  Monitor()
      : previous{std::filesystem::current_path()},
        root{std::filesystem::temp_directory_path() / "monitor_test_root"} {
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "a" / "b");
    std::filesystem::current_path(root);
  }

  ~Monitor() override {
    std::error_code err;
    std::filesystem::current_path(previous, err);
    std::filesystem::remove_all(root, err);
  }

  std::filesystem::path previous;
  std::filesystem::path root;

  Monitor(Monitor &) = delete;
  Monitor(Monitor &&) = delete;
  auto operator=(Monitor &) -> Monitor = delete;
  auto operator=(Monitor &&) -> Monitor = delete;
};
} // namespace

TEST_F(Monitor, V14MaskAllowsSubdirectoryDiscovery) {
  // IN_CREATE and IN_MOVED_TO are the only signals that a directory appeared;
  // IN_MOVED_FROM the only one that a watched directory left its parent.
  ASSERT_EQ(monitor::watch_mask,
            static_cast<uint32_t>(IN_CLOSE_WRITE | IN_DELETE | IN_CREATE |
                                  IN_MOVED_FROM | IN_MOVED_TO | IN_DONT_FOLLOW |
                                  IN_EXCL_UNLINK));
}

TEST_F(Monitor, V23WatchesEveryDirectoryAtConstruction) {
  const monitor::Monitor m;
  ASSERT_EQ(m.watched(), directories_below_root());
}

TEST_F(Monitor, V23EventInNestedDirectoryIsSeen) {
  monitor::Monitor m;

  std::ofstream ofs("./a/b/f.txt");
  ofs << "content\n";
  ofs.close();

  ASSERT_TRUE(wait_for_event(m));
  const auto event = m.next_event();
  ASSERT_TRUE(event.has_value());
  ASSERT_EQ(event->name.value_or(""), std::string{"./a/b/f.txt"});
}

TEST_F(Monitor, V22SurvivesNameMaxFilename) {
  monitor::Monitor m;

  const std::string long_name(NAME_MAX, 'x');
  std::ofstream ofs("./a/b/" + long_name);
  ofs << "content\n";
  ofs.close();

  ASSERT_TRUE(wait_for_event(m));
  const auto event = m.next_event();
  ASSERT_TRUE(event.has_value());
  ASSERT_EQ(event->name.value_or(""), "./a/b/" + long_name);
}

TEST_F(Monitor, V22DrainsEveryQueuedEvent) {
  monitor::Monitor m;

  constexpr size_t file_count = 3;
  for (size_t i = 0; i < file_count; ++i) {
    std::ofstream ofs(std::format("./a/b/f{}.txt", i));
    ofs << "content\n";
  }

  auto seen = std::vector<std::string>{};
  while (seen.size() < file_count && wait_for_event(m)) {
    const auto event = m.next_event();
    ASSERT_TRUE(event.has_value());
    seen.push_back(event->name.value_or(""));
  }
  std::ranges::sort(seen);
  ASSERT_EQ(seen, (std::vector<std::string>{"./a/b/f0.txt", "./a/b/f1.txt",
                                            "./a/b/f2.txt"}));
}

TEST_F(Monitor, V34FileCreationAloneDoesNotDriveSync) {
  monitor::Monitor m;

  // Held open, so IN_CREATE fires but IN_CLOSE_WRITE does not yet.
  std::ofstream ofs("./a/b/f.txt");
  ASSERT_FALSE(wait_for_event(m));

  ofs << "content\n";
  ofs.close();
  ASSERT_TRUE(wait_for_event(m));
}

TEST_F(Monitor, V23NewSubdirectoryBecomesWatched) {
  monitor::Monitor m;

  std::filesystem::create_directory("./a/b/c");

  ASSERT_TRUE(wait_for_event(m));
  m.discard();
  ASSERT_TRUE(is_watched(m, "./a/b/c"));
  ASSERT_EQ(m.watched(), directories_below_root());
}

TEST_F(Monitor, V23RemovedDirectoryIsUnwatched) {
  monitor::Monitor m;

  ASSERT_TRUE(is_watched(m, "./a/b"));
  std::filesystem::remove("./a/b");

  ASSERT_TRUE(wait_for_event(m));
  m.discard();
  ASSERT_FALSE(is_watched(m, "./a/b"));
  ASSERT_EQ(m.watched(), directories_below_root());
}
