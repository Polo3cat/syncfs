#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#include <libtorrent/hasher.hpp>
#include <libtorrent/sha1_hash.hpp>

#include <files.h>
#include <reconcile.h>
#include <utils.h>

namespace {
// One entry of the pinned form. The separator is the one byte a POSIX
// filename cannot hold, so nothing a user can name changes where an entry
// ends.
void append(std::string &out, const std::filesystem::path &path,
            std::int64_t ticks) {
  out.append(path.native());
  out.push_back('\0');
  out.append(std::to_string(ticks));
  out.push_back('\0');
}

// libtorrent asserts on an empty update, so an empty set is fed nothing at
// all, which leaves the digest of the empty string.
auto digest_of(const std::string &form) -> std::string {
  lt::hasher256 hasher;
  if (!form.empty()) {
    hasher.update(form.data(), static_cast<int>(form.size()));
  }
  const auto digest = hasher.final();
  return std::string{digest.data(),
                     static_cast<size_t>(lt::sha256_hash::size())};
}
} // namespace

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

auto encode(const files::file_map_t &held) -> std::string {
  std::string out;
  for (const auto &[path, written] : held) {
    append(out, path, utils::to_ticks(written));
  }
  return out;
}

auto encode(const tombstone_map_t &deleted) -> std::string {
  std::string out;
  for (const auto &[path, at] : deleted) {
    append(out, path, utils::to_ticks(at));
  }
  return out;
}

auto hash(const files::file_map_t &held, const tombstone_map_t &deleted)
    -> std::string {
  // Each set is hashed on its own and the two digests are hashed together.
  // Feeding the two encodings into one hasher would leave no boundary between
  // them, and then a node holding a file and a node remembering that same path
  // deleted at that same tick agree on the root hash: the two most divergent
  // nodes there are would never exchange a digest. Two fixed width digests
  // give the boundary whatever a path happens to contain.
  const auto files = digest_of(encode(held));
  const auto graves = digest_of(encode(deleted));
  lt::hasher256 hasher;
  hasher.update(files.data(), static_cast<int>(files.size()));
  hasher.update(graves.data(), static_cast<int>(graves.size()));
  const auto digest = hasher.final();
  return std::string{digest.data(),
                     static_cast<size_t>(lt::sha256_hash::size())};
}
} // namespace reconcile
