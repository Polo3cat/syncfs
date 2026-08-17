#include <algorithm>
#include <chrono>
#include <filesystem>
#include <set>
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

TEST(Reconcile, V59PartnerIsOneMismatchingPeer) {
  const auto mine = std::string{"my own root hash"};
  const auto peers = reconcile::state_map_t{{"tcp://a:5555", "another hash"},
                                            {"tcp://b:5555", "another hash"},
                                            {"tcp://c:5555", mine}};
  auto partner = reconcile::Partner{};

  // One peer is asked, never the whole set, and never a peer that already
  // agrees: a digest costs O(M) bytes and there is nothing in it for a node
  // holding the same tree.
  auto seen = std::set<std::string>{};
  for (int round = 0; round < 100; ++round) {
    const auto asked = partner.pick(peers, mine);
    ASSERT_TRUE(asked.has_value());
    ASSERT_NE(*asked, "tcp://c:5555");
    seen.insert(*asked);
  }
  // And a different one over the rounds, which is what makes a wedged peer cost
  // one round rather than the gap for ever. Assigning the choice instead would
  // need every node to agree on a set carried over a plane that drops.
  ASSERT_EQ(seen, (std::set<std::string>{"tcp://a:5555", "tcp://b:5555"}));
}

TEST(Reconcile, V59NoPartnerWhenEveryPeerAgrees) {
  const auto mine = std::string{"my own root hash"};
  const auto peers =
      reconcile::state_map_t{{"tcp://a:5555", mine}, {"tcp://b:5555", mine}};
  auto partner = reconcile::Partner{};

  // A converged swarm sends nothing but its root hash, which is the whole of
  // what makes the idle cost thirty two bytes a node a period.
  ASSERT_FALSE(partner.pick(peers, mine).has_value());
  ASSERT_FALSE(partner.pick({}, mine).has_value());
}

TEST(Reconcile, V59RepairsArePacedWithoutARandomDraw) {
  const auto now = std::chrono::steady_clock::now();
  auto pending = reconcile::pending_map_t{};

  reconcile::arm(pending, {"./a", "./b", "./c"}, now);
  ASSERT_EQ(pending.size(), 3);

  // One goes at once and the rest queue behind it. A node answering thousands
  // of gaps would otherwise put them all on the wire in one loop iteration,
  // where the receiver adds one torrent an iteration and drops the rest.
  const auto first = reconcile::due(pending, now);
  ASSERT_EQ(first.size(), 1);
  // Taken out as they are handed over, so a repair goes out once. An unrepaired
  // gap needs no retry engine: it turns up in the next digest.
  ASSERT_EQ(pending.size(), 2);

  const auto rest = reconcile::due(pending, now + std::chrono::seconds{1});
  ASSERT_EQ(rest.size(), 2);
  ASSERT_TRUE(pending.empty());

  // And the same gaps give the same moments: there is no generator left in this
  // path, because one reader per digest means there are no duplicates to
  // spread.
  auto again = reconcile::pending_map_t{};
  reconcile::arm(again, {"./a", "./b", "./c"}, now);
  auto once_more = reconcile::pending_map_t{};
  reconcile::arm(once_more, {"./a", "./b", "./c"}, now);
  ASSERT_EQ(again, once_more);
}

TEST(Reconcile, V43ArmingAgainDoesNotPushTheMomentBack) {
  const auto now = std::chrono::steady_clock::now();
  auto pending = reconcile::pending_map_t{};

  reconcile::arm(pending, {"./a"}, now);
  const auto due_at = pending.at("./a");
  // The same gap turns up in every digest until it is closed. Re-arming it
  // would move its moment along every round and it would never arrive.
  reconcile::arm(pending, {"./a"}, now + std::chrono::seconds{1});
  ASSERT_EQ(pending.at("./a"), due_at);
}
