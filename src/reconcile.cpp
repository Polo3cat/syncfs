#include <algorithm>
#include <chrono>
#include <filesystem>

#include <reconcile.h>
#include <utils.h>

namespace reconcile {

auto beats(time_point deleted, std::filesystem::file_time_type written)
    -> bool {
  // Both sides go through the wire form: the epoch of a file timestamp is the
  // implementation's business, and comparing the two clocks directly would
  // compare two different origins.
  return utils::to_ticks(deleted) > utils::to_ticks(written);
}

void mark(tombstone_map_t &tombstones, const std::filesystem::path &path,
          time_point deleted) {
  const auto [entry, inserted] = tombstones.try_emplace(path, deleted);
  if (!inserted) {
    entry->second = std::max(entry->second, deleted);
  }
}

void expire(tombstone_map_t &tombstones, time_point now) {
  std::erase_if(tombstones, [now](const auto &entry) -> bool {
    return now - entry.second > tombstone_ttl;
  });
}
} // namespace reconcile
