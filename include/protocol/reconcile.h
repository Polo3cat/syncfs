#pragma once

#include <chrono>
#include <filesystem>
#include <map>
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
// of. The holder is the one that repairs them: PUB/SUB has no request path,
// so a node with a gap has no way of asking anyone for anything.
auto gaps(const files::file_map_t &mine, const tombstone_map_t &tombstones,
          const files::file_map_t &theirs)
    -> std::vector<std::filesystem::path>;
} // namespace reconcile
