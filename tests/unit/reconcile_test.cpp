#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>

#include <files.h>
#include <gtest/gtest.h>
#include <reconcile.h>
#include <utils.h>

namespace {
// A wall clock instant and the file timestamp for the same moment, since a
// deletion is ordered against a modification time and the two clocks have
// different epochs.
auto at(std::chrono::system_clock::time_point base, std::chrono::seconds offset)
    -> std::chrono::system_clock::time_point {
  return base + offset;
}

auto written_at(std::chrono::system_clock::time_point t)
    -> std::filesystem::file_time_type {
  return utils::to_file_time(t);
}
} // namespace

TEST(Reconcile, V37TombstoneBeatsOlderCreateAndTieKeepsFile) {
  const auto base = std::chrono::system_clock::now();
  const auto deleted = at(base, std::chrono::seconds{10});

  // Strictly newer wins.
  ASSERT_TRUE(
      reconcile::beats(deleted, written_at(at(base, std::chrono::seconds{9}))));
  // A file written afterwards survives the deletion.
  ASSERT_FALSE(reconcile::beats(
      deleted, written_at(at(base, std::chrono::seconds{11}))));
  // A tie keeps the file: a wrongly kept file is visible and can be deleted
  // again, a wrongly deleted one is gone.
  ASSERT_FALSE(reconcile::beats(deleted, written_at(deleted)));
}

TEST(Reconcile, V37MarkKeepsTheLaterDeletion) {
  const auto base = std::chrono::system_clock::now();
  auto tombstones = reconcile::tombstone_map_t{};
  const std::filesystem::path path{"./f"};

  reconcile::mark(tombstones, path, at(base, std::chrono::seconds{10}));
  // An older record of the same deletion must not weaken the one already
  // held, whichever order the two arrive in.
  reconcile::mark(tombstones, path, at(base, std::chrono::seconds{5}));
  ASSERT_EQ(tombstones.at(path), at(base, std::chrono::seconds{10}));

  reconcile::mark(tombstones, path, at(base, std::chrono::seconds{20}));
  ASSERT_EQ(tombstones.at(path), at(base, std::chrono::seconds{20}));
}

TEST(Reconcile, V37TombstonesExpireOnTheTimeToLive) {
  const auto now = std::chrono::system_clock::now();
  auto tombstones = reconcile::tombstone_map_t{};
  reconcile::mark(tombstones, "./old",
                  now - reconcile::tombstone_ttl - std::chrono::seconds{1});
  reconcile::mark(tombstones, "./exactly", now - reconcile::tombstone_ttl);
  reconcile::mark(tombstones, "./fresh", now - std::chrono::seconds{1});

  reconcile::expire(tombstones, now);

  ASSERT_FALSE(tombstones.contains("./old"));
  // Still inside the window it was given, so it still stands.
  ASSERT_TRUE(tombstones.contains("./exactly"));
  ASSERT_TRUE(tombstones.contains("./fresh"));
}

TEST(Reconcile, V46CanonicalFormIsPinned) {
  auto held = files::file_map_t{};
  held.emplace("./b", written_at(utils::from_ticks("2")));
  held.emplace("./a/f", written_at(utils::from_ticks("1")));

  // Sorted by path, every entry the path, a NUL, the decimal ticks, a NUL.
  // Two nodes spelling this differently diverge for good and say nothing
  // about it, so the form is pinned here rather than left to the writer.
  ASSERT_EQ(reconcile::encode(held), std::string("./a/f\0"
                                                 "1\0"
                                                 "./b\0"
                                                 "2\0",
                                                 14));

  auto deleted = reconcile::tombstone_map_t{};
  deleted.emplace("./gone", utils::from_ticks("3"));
  ASSERT_EQ(reconcile::encode(deleted), std::string("./gone\0"
                                                    "3\0",
                                                    9));
}

TEST(Reconcile, V46NamesWithTabsAndNewlinesSurvive) {
  // A POSIX filename excludes only "/" and NUL, so a name may hold a tab or a
  // newline. Framing on either of those turns one file into two paths that do
  // not exist, and a path that does not exist is a gap no repair can close.
  auto held = files::file_map_t{};
  held.emplace("./two\nlines", written_at(utils::from_ticks("1")));

  const auto encoded = reconcile::encode(held);
  ASSERT_NE(encoded.find('\n'), std::string::npos);
  ASSERT_EQ(std::ranges::count(encoded, '\0'), 2);
}

TEST(Reconcile, V46HashSeparatesHeldFromDeleted) {
  auto held = files::file_map_t{};
  held.emplace("./f", written_at(utils::from_ticks("1")));
  auto deleted = reconcile::tombstone_map_t{};
  deleted.emplace("./f", utils::from_ticks("1"));

  const auto empty_held = files::file_map_t{};
  const auto empty_deleted = reconcile::tombstone_map_t{};

  ASSERT_EQ(reconcile::hash(held, empty_deleted).size(), 32);
  // A node holding a file and a node remembering it deleted are not the same
  // node, however alike their two sets look concatenated.
  ASSERT_NE(reconcile::hash(held, empty_deleted),
            reconcile::hash(empty_held, deleted));
  ASSERT_NE(reconcile::hash(held, deleted),
            reconcile::hash(held, empty_deleted));
  // And the same two sets always hash the same.
  ASSERT_EQ(reconcile::hash(held, deleted), reconcile::hash(held, deleted));
}
