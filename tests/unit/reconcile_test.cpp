#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

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

TEST(Reconcile, V42DigestSetsRoundTrip) {
  auto held = files::file_map_t{};
  held.emplace("./a/f", written_at(utils::from_ticks("1")));
  held.emplace("./two\nlines", written_at(utils::from_ticks("2")));
  auto deleted = reconcile::tombstone_map_t{};
  deleted.emplace("./gone", utils::from_ticks("3"));

  ASSERT_EQ(reconcile::decode_held(reconcile::encode(held)), held);
  ASSERT_EQ(reconcile::decode_tombstones(reconcile::encode(deleted)), deleted);
}

TEST(Reconcile, V42TruncatedDigestDropsTheEntryItWasCutIn) {
  auto held = files::file_map_t{};
  held.emplace("./a", written_at(utils::from_ticks("1")));
  held.emplace("./b", written_at(utils::from_ticks("2")));
  const auto whole = reconcile::encode(held);

  // Half an entry is not an entry. Reading a path out of one would invent a
  // file nobody holds, and a file nobody holds is a gap no repair can close.
  const auto cut = reconcile::decode_held(whole.substr(0, whole.size() - 1));
  ASSERT_EQ(cut.size(), 1);
  ASSERT_TRUE(cut.contains("./a"));
}

TEST(Reconcile, V50AbsentUndefeatedTombstonesAreAdopted) {
  const auto now = std::chrono::system_clock::now();
  const auto recently = now - std::chrono::seconds{1};

  auto theirs = reconcile::tombstone_map_t{};
  theirs.emplace("./news", recently);
  theirs.emplace("./known", recently);
  theirs.emplace("./defeated", recently);
  theirs.emplace("./ancient",
                 now - reconcile::tombstone_ttl - std::chrono::seconds{1});

  auto mine = reconcile::tombstone_map_t{};
  mine.emplace("./known", recently);

  auto held = files::file_map_t{};
  // Written after the peer deleted it, so this node's copy wins and the
  // deletion is not worth taking on.
  held.emplace("./defeated", written_at(now));

  const auto adopt = reconcile::adoptable(theirs, held, mine, now);

  ASSERT_TRUE(adopt.contains("./news"));
  ASSERT_FALSE(adopt.contains("./known"));
  ASSERT_FALSE(adopt.contains("./defeated"));
  // Taking on a deletion this node would expire at once only hands it back to
  // the peer next round, for as long as the two disagree about its age.
  ASSERT_FALSE(adopt.contains("./ancient"));
}

TEST(Reconcile, V43GapsAreTheHoldersToRepair) {
  auto mine = files::file_map_t{};
  mine.emplace("./only_here", written_at(utils::from_ticks("5")));
  mine.emplace("./newer_here", written_at(utils::from_ticks("9")));
  mine.emplace("./same", written_at(utils::from_ticks("5")));
  mine.emplace("./older_here", written_at(utils::from_ticks("1")));
  mine.emplace("./deleted_here", written_at(utils::from_ticks("5")));

  auto theirs = files::file_map_t{};
  theirs.emplace("./newer_here", written_at(utils::from_ticks("5")));
  theirs.emplace("./same", written_at(utils::from_ticks("5")));
  theirs.emplace("./older_here", written_at(utils::from_ticks("9")));

  auto tombstones = reconcile::tombstone_map_t{};
  tombstones.emplace("./deleted_here", utils::from_ticks("6"));

  const auto found = reconcile::gaps(mine, tombstones, theirs);

  ASSERT_EQ(found, (std::vector<std::filesystem::path>{"./newer_here",
                                                       "./only_here"}));
}

TEST(Reconcile, V51WindowScalesWithGapCount) {
  // A repair is only cancelled by one that is observed, and a receiver drains
  // one torrent per loop iteration, so a fixed second is already too short at
  // a few dozen gaps: nothing would be seen inside it, nobody would stand
  // down, and every holder would answer every gap.
  ASSERT_LT(reconcile::repair_window(1), reconcile::repair_window(1000));
  ASSERT_LT(reconcile::repair_window(100), reconcile::repair_window(1000));
  // With a floor, so that a single gap is still spread over something.
  ASSERT_GE(reconcile::repair_window(1), std::chrono::seconds{1});
  // And a ceiling, so that it always ends.
  ASSERT_LE(reconcile::repair_window(1000000), std::chrono::seconds{60});
}

TEST(Reconcile, V43ArmedRepairsFireOnceAndOnlyWhenDue) {
  const auto now = std::chrono::steady_clock::now();
  auto pending = reconcile::pending_map_t{};
  auto backoff = reconcile::Backoff{};

  reconcile::arm(pending, {"./a", "./b"}, now, backoff);
  ASSERT_EQ(pending.size(), 2);
  // Drawn from the second half of the window, so nothing is due at once.
  ASSERT_TRUE(reconcile::due(pending, now).empty());

  const auto fired = reconcile::due(pending, now + reconcile::repair_window(2));
  ASSERT_EQ(fired.size(), 2);
  // And taken out as they are handed over, so a repair goes out once. An
  // unrepaired gap needs no retry engine: it turns up in the next digest.
  ASSERT_TRUE(pending.empty());
}

TEST(Reconcile, V43ArmingAgainDoesNotPushTheMomentBack) {
  const auto now = std::chrono::steady_clock::now();
  auto pending = reconcile::pending_map_t{};
  auto backoff = reconcile::Backoff{};

  reconcile::arm(pending, {"./a"}, now, backoff);
  const auto due_at = pending.at("./a");
  // The same gap turns up in every digest until it is closed. Re-arming it
  // would move its moment along every round and it would never arrive.
  reconcile::arm(pending, {"./a"}, now + std::chrono::seconds{1}, backoff);
  ASSERT_EQ(pending.at("./a"), due_at);
}
