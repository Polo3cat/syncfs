#pragma once

#include <chrono>
#include <filesystem>
#include <map>

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
} // namespace reconcile
