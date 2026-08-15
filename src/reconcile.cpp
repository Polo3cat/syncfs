#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <string_view>
#include <vector>

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

// Walks the pinned form, handing back one path and its ticks at a time. A
// tail that does not close both of its fields is dropped whole, along with
// everything after it: half an entry is not an entry, and a path invented out
// of one would be a gap no repair could ever close.
template <typename F> void for_each_entry(std::string_view form, F on_entry) {
  size_t at = 0;
  while (at < form.size()) {
    const auto path_end = form.find('\0', at);
    if (path_end == std::string_view::npos) {
      return;
    }
    const auto ticks_end = form.find('\0', path_end + 1);
    if (ticks_end == std::string_view::npos) {
      return;
    }
    on_entry(form.substr(at, path_end - at),
             form.substr(path_end + 1, ticks_end - path_end - 1));
    at = ticks_end + 1;
  }
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

auto decode_held(std::string_view form) -> files::file_map_t {
  files::file_map_t held;
  for_each_entry(form, [&held](auto path, auto ticks) -> void {
    held.insert_or_assign(std::filesystem::path{path},
                          utils::to_file_time(utils::from_ticks(ticks)));
  });
  return held;
}

auto decode_tombstones(std::string_view form) -> tombstone_map_t {
  tombstone_map_t deleted;
  for_each_entry(form, [&deleted](auto path, auto ticks) -> void {
    deleted.insert_or_assign(std::filesystem::path{path},
                             utils::from_ticks(ticks));
  });
  return deleted;
}

auto adoptable(const tombstone_map_t &theirs, const files::file_map_t &held,
               const tombstone_map_t &mine, time_point now) -> tombstone_map_t {
  tombstone_map_t adopt;
  for (const auto &[path, at] : theirs) {
    if (mine.contains(path)) {
      continue;
    }
    // Already past its time here. Taking it on would only hand it straight
    // back to the peer next round, for as long as the two disagree about
    // whether it has expired.
    if (now - at > tombstone_ttl) {
      continue;
    }
    const auto file = held.find(path);
    if (file != held.end() && !beats(at, file->second)) {
      continue;
    }
    adopt.emplace(path, at);
  }
  return adopt;
}

auto gaps(const files::file_map_t &mine, const tombstone_map_t &tombstones,
          const files::file_map_t &theirs)
    -> std::vector<std::filesystem::path> {
  std::vector<std::filesystem::path> found;
  for (const auto &[path, written] : mine) {
    // A copy this node's own tombstones have already killed is not something
    // to hand anyone: repairing it would bring back the file the deletion was
    // about.
    const auto grave = tombstones.find(path);
    if (grave != tombstones.end() && beats(grave->second, written)) {
      continue;
    }
    const auto peer = theirs.find(path);
    if (peer == theirs.end() ||
        utils::to_ticks(peer->second) < utils::to_ticks(written)) {
      found.push_back(path);
    }
  }
  return found;
}

auto repair_window(size_t gaps) -> std::chrono::milliseconds {
  // Twenty milliseconds a gap, which puts a thousand of them just under a
  // minute: the same order as the time the files themselves take to move, and
  // far enough past the second and a half a swarm needs merely to finish
  // subscribing for a peer's repair to be seen before this node's own is due.
  constexpr auto per_gap = std::chrono::milliseconds{20};
  constexpr auto shortest = std::chrono::milliseconds{1000};
  constexpr auto longest = std::chrono::milliseconds{60000};
  return std::clamp(per_gap * static_cast<std::int64_t>(gaps), shortest,
                    longest);
}

Backoff::Backoff() : generator{std::random_device{}()} {}

auto Backoff::draw(std::chrono::milliseconds window)
    -> std::chrono::milliseconds {
  std::uniform_int_distribution<std::int64_t> spread{window.count() / 2,
                                                     window.count()};
  return std::chrono::milliseconds{spread(generator)};
}

void arm(pending_map_t &pending,
         const std::vector<std::filesystem::path> &missing,
         std::chrono::steady_clock::time_point now, Backoff &backoff) {
  const auto window = repair_window(missing.size());
  for (const auto &path : missing) {
    pending.try_emplace(path, now + backoff.draw(window));
  }
}

auto due(pending_map_t &pending, std::chrono::steady_clock::time_point now)
    -> std::vector<std::filesystem::path> {
  std::vector<std::filesystem::path> ready;
  for (const auto &[path, at] : pending) {
    if (at <= now) {
      ready.push_back(path);
    }
  }
  for (const auto &path : ready) {
    pending.erase(path);
  }
  return ready;
}
} // namespace reconcile
