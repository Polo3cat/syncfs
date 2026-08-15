#include <chrono>
#include <filesystem>

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
