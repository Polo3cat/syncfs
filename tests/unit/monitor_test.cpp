#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <linux/limits.h>
#include <string>
#include <sys/inotify.h>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <monitor.h>

namespace {
// Monitor::wait() does not block (V26), so the ceiling has to come from the
// clock: a count of attempts would be spent in microseconds, which turns every
// positive assertion below into a flake and leaves V34's negative one asserting
// nothing at all. The interval is what keeps the wait from being a spin.
constexpr auto event_deadline = std::chrono::seconds{2};
constexpr auto poll_interval = std::chrono::milliseconds{5};

auto wait_for_event(monitor::Monitor &m) -> bool {
  const auto end = std::chrono::steady_clock::now() + event_deadline;
  while (true) {
    if (m.wait()) {
      return true;
    }
    if (std::chrono::steady_clock::now() >= end) {
      return false;
    }
    std::this_thread::sleep_for(poll_interval);
  }
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

TEST_F(Monitor, V15MonitorIsMoveOnlyAndLeavesNothingBehind) {
  // The daemon holds one monitor and moves it into the sync loop, so a copy
  // would duplicate the inotify descriptor and close it twice.
  static_assert(!std::is_copy_constructible_v<monitor::Monitor>);
  static_assert(!std::is_copy_assignable_v<monitor::Monitor>);
  static_assert(std::is_move_constructible_v<monitor::Monitor>);
  static_assert(std::is_move_assignable_v<monitor::Monitor>);

  auto original = monitor::Monitor();
  ASSERT_FALSE(original.watched().empty());

  auto moved = std::move(original);
  // The moved-to monitor owns the descriptor and the watches with it.
  ASSERT_TRUE(is_watched(moved, "."));
  ASSERT_TRUE(is_watched(moved, "./a"));
  // Reading a moved-from monitor is the invariant under test, so both
  // analysers are told this access is deliberate.
  // cppcheck-suppress accessMoved
  // NOLINTNEXTLINE(bugprone-use-after-move,hicpp-invalid-access-moved)
  ASSERT_TRUE(original.watched().empty());
}

TEST_F(Monitor, V22DiscardReportsAReadFailure) {
  // A monitor that was moved from holds no descriptor (V15), which is the one
  // way to make the refill inside discard() fail without breaking the kernel's
  // idea of the tree. What is asserted is that the failure is reported at all:
  // it used to be dropped into a [[maybe_unused]] and the daemon carried on
  // believing it was still watching.
  auto original = monitor::Monitor();
  auto moved = std::move(original);

  // cppcheck-suppress accessMoved
  // NOLINTNEXTLINE(bugprone-use-after-move,hicpp-invalid-access-moved)
  const auto discarded = original.discard();

  ASSERT_FALSE(discarded.has_value());
  ASSERT_NE(discarded.error().find("Failed to read on inotify fd"),
            std::string::npos)
      << discarded.error();
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

TEST_F(Monitor, V26WaitDoesNotBlock) {
  // The ZMQ poller is the sync loop's one blocking point, so this wait only
  // reports what inotify already has and returns (V26). Repeated, because a
  // single call is too short to tell a poll that returned at once from one that
  // slept: at the 50 ms this used to wait, twenty of them cost a second.
  constexpr int calls = 20;
  constexpr auto budget = std::chrono::milliseconds{25};
  monitor::Monitor m;

  const auto started = std::chrono::steady_clock::now();
  for (int call = 0; call < calls; ++call) {
    static_cast<void>(m.wait());
  }
  const auto spent = std::chrono::steady_clock::now() - started;

  ASSERT_LT(spent, budget)
      << std::chrono::duration_cast<std::chrono::milliseconds>(spent).count()
      << " ms for " << calls << " calls";
}

TEST_F(Monitor, V23NewSubdirectoryBecomesWatched) {
  monitor::Monitor m;

  std::filesystem::create_directory("./a/b/c");

  ASSERT_TRUE(wait_for_event(m));
  ASSERT_TRUE(m.discard().has_value());
  ASSERT_TRUE(is_watched(m, "./a/b/c"));
  ASSERT_EQ(m.watched(), directories_below_root());
}

TEST_F(Monitor, V23RemovedDirectoryIsUnwatched) {
  monitor::Monitor m;

  ASSERT_TRUE(is_watched(m, "./a/b"));
  std::filesystem::remove("./a/b");

  ASSERT_TRUE(wait_for_event(m));
  ASSERT_TRUE(m.discard().has_value());
  ASSERT_FALSE(is_watched(m, "./a/b"));
  ASSERT_EQ(m.watched(), directories_below_root());
}
