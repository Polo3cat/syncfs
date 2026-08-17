#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <files.h>

// The periodic repair of everything the control plane lost. ZMQ drops an
// announcement whose subscriber has not finished connecting, or whose queue is
// full, and says nothing about it, so a node cannot learn what it missed from
// the announcements themselves. It compares itself against its peers instead.
namespace reconcile {

// Constants, not configuration. The period bounds how long a lost
// announcement stays lost; the time to live bounds how long a peer may be away
// before a file it deleted comes back. They are decoupled on purpose: tying
// the second to the first would put the downtime budget below a container
// restart.
inline constexpr auto period = std::chrono::seconds{5};
inline constexpr auto quiescence_window = std::chrono::seconds{10};
inline constexpr auto state_ceiling = std::chrono::seconds{60};
inline constexpr auto tombstone_ttl = std::chrono::hours{1};

using time_point = std::chrono::system_clock::time_point;

// One per-path delete state: the path, and the moment the file was deleted.
// The same map suppresses the echo of a deletion, orders a deletion against a
// file that arrives afterwards, and tells a peer about a deletion it never
// heard. Three maps would only be three chances to disagree.
using tombstone_map_t = std::map<std::filesystem::path, time_point>;

// A deletion beats a file only if it is strictly newer. A tie keeps the file:
// a wrongly kept file is visible and can be deleted again, a wrongly deleted
// one is gone.
auto beats(time_point deleted, std::filesystem::file_time_type written) -> bool;

// Records a deletion, keeping the later of the two if the path already has
// one. An older record would weaken a deletion this node has already seen.
void mark(tombstone_map_t &tombstones, const std::filesystem::path &path,
          time_point deleted);

// Forgets the deletions that have outlived the time to live. A node that has
// been away for longer than that brings the file back, which is the price of
// keeping the tombstones in memory.
void expire(tombstone_map_t &tombstones, time_point now);

// The pinned form of a set: sorted by path, every entry "path<NUL>ticks<NUL>",
// ticks decimal. NUL framed because a POSIX filename excludes only "/" and NUL
// itself, so a name holding a tab or a newline would misparse into paths that
// do not exist, and a path that does not exist is a gap no repair can ever
// close.
auto encode(const files::file_map_t &held) -> std::string;
auto encode(const tombstone_map_t &deleted) -> std::string;

// The root hash the peers compare, over the held set followed by the tombstone
// set in exactly the encoding above. Both ends have to build it the same way
// or they diverge permanently and without a word, so the form is pinned by
// test. Raw SHA-256, so the caller holds 32 bytes and not a spelling of them.
auto hash(const files::file_map_t &held, const tombstone_map_t &deleted)
    -> std::string;

// The inverse of encode(). An entry that does not close both of its fields is
// dropped along with everything after it: a digest cut short says nothing
// about what it never carried, and inventing an entry from half of one would
// invent a gap that no repair can ever close.
auto decode_held(std::string_view form) -> files::file_map_t;
auto decode_tombstones(std::string_view form) -> tombstone_map_t;

// The deletions in a peer's digest this node has not got. A node that never
// saw a remove would otherwise mismatch the root hash for ever and ship a
// full digest every round, and a node coming back with the file would bring
// it back to everyone. Already defeated by this node's own copy, or already
// past the time to live here, means not worth adopting.
auto adoptable(const tombstone_map_t &theirs, const files::file_map_t &held,
               const tombstone_map_t &mine, time_point now) -> tombstone_map_t;

// The paths this node holds that a peer either lacks or holds an older copy
// of, read off the digest that peer addressed here. Whoever was asked is the
// one that answers, and being asked is the whole of the appointment: nobody is
// elected, and a peer that cannot answer costs the round it was asked in.
auto gaps(const files::file_map_t &mine, const tombstone_map_t &tombstones,
          const files::file_map_t &theirs)
    -> std::vector<std::filesystem::path>;

// The repairs this node has agreed to make and the moment each is due. One
// digest has one reader, so these are this node's alone to answer and the
// moments only pace them: they are not there to keep two holders from answering
// at once, which is what the randomized window used to be for.
using pending_map_t =
    std::map<std::filesystem::path, std::chrono::steady_clock::time_point>;

// What this node last heard each of its peers' trees hash to, by the endpoint
// the peer named in its state. One entry a peer, so a root hash that differs
// can be answered without waiting to hear it again.
using state_map_t = std::map<std::string, std::string>;

// Which peer to ask this round: uniformly at random among those whose last root
// hash differed from this node's own, and nothing at all when they all agree,
// so a converged swarm ships no digest.
//
// Randomized rather than assigned. A rule every node could compute alone —
// modulo over the holder set, say — needs every node to agree on that set, and
// the digests carrying it ride a plane that drops: the disagreement case is
// nobody sending, silently and for ever, where randomization's is a duplicate
// that costs nothing.
struct Partner {
  std::mt19937 generator;

  Partner();
  auto pick(const state_map_t &peers, std::string_view mine)
      -> std::optional<std::string>;
};

// Takes on every gap not already waiting, one pace apart so that a node holding
// thousands of them does not put them all on the wire in a single loop
// iteration. Re-arming a gap already waiting would push its moment back every
// round and it would never arrive.
void arm(pending_map_t &pending,
         const std::vector<std::filesystem::path> &missing,
         std::chrono::steady_clock::time_point now);

// The repairs whose wait is over, taken out of the map as they are handed
// back. An unrepaired gap needs no backoff engine to try again: it turns up
// in the next digest.
auto due(pending_map_t &pending, std::chrono::steady_clock::time_point now)
    -> std::vector<std::filesystem::path>;
} // namespace reconcile
