#include <algorithm>
#include <expected>
#include <filesystem>
#include <iterator>
#include <map>
#include <ranges>
#include <system_error>
#include <utility>
#include <vector>

#include <files.h>

namespace files {
namespace {
auto last_write_time(const std::filesystem::directory_entry &entry)
    -> std::expected<std::filesystem::file_time_type, std::error_code> {
  std::error_code err;
  const auto time = entry.last_write_time(err);
  if (err) {
    return std::unexpected(err);
  }
  return time;
}
} // namespace

auto list() -> files::file_map_t {
  // It's necessary to materialize due to std::view::filter caching
  // the begin iterator. This causes a toctou failure on the value
  // returned by last_write_time. It doesn't check that the value
  // pointed to by the begin iterator is true with the predicate
  // on the actual iteration. The value changes because the entry may
  // change on disk.
  //
  // The walk itself reports its errors rather than throwing them, the same way
  // Monitor::resync_watches walks the same tree: deleting a subtree inside the
  // sync root wakes the loop through inotify, and the listing that follows
  // races whatever is still being unlinked. On the throwing iterator that race
  // leaves the sync loop, main catches it and the daemon exits - V28 working as
  // designed and the process gone anyway (V66, B8, B16). A listing that comes
  // back short costs one diff round, which the next one repairs.
  auto entries_with_times = std::vector<std::pair<
      std::filesystem::directory_entry,
      std::expected<std::filesystem::file_time_type, std::error_code>>>{};
  std::error_code err;
  auto entry = std::filesystem::recursive_directory_iterator(
      ".", std::filesystem::directory_options::none, err);
  const auto last = std::filesystem::recursive_directory_iterator{};
  while (!err && entry != last) {
    entries_with_times.emplace_back(*entry, last_write_time(*entry));
    entry.increment(err);
  }

  return entries_with_times |
         std::views::filter([](const auto &entry_time) -> auto {
           // Every question about the entry is asked the same way: a status
           // that cannot be read is an entry left out, never an exception out
           // of the loop.
           std::error_code status_err;
           return entry_time.first.is_regular_file(status_err) && !status_err &&
                  !entry_time.first.is_symlink(status_err) && !status_err &&
                  entry_time.second.has_value();
         }) |
         std::views::transform([](const auto &entry_time) -> auto {
           return std::pair(entry_time.first.path(), *entry_time.second);
         }) |
         std::ranges::to<std::map>();
}

auto diff(const file_map_t &left, const file_map_t &right) -> file_map_t {
  file_map_t diff;
  std::ranges::set_difference(left, right, std::inserter(diff, diff.end()));
  return diff;
}

auto diff_name(const file_map_t &left, const file_map_t &right) -> file_map_t {
  file_map_t diff_name;
  std::ranges::set_difference(
      left, right, std::inserter(diff_name, diff_name.end()),
      [](const auto &l, const auto &r) -> auto { return l.first < r.first; });
  return diff_name;
}

auto intersection_name(const file_map_t &left, const file_map_t &right)
    -> file_map_t {
  file_map_t intersection;
  std::ranges::set_intersection(
      left, right, std::inserter(intersection, intersection.end()),
      [](const auto &l, const auto &r) -> auto { return l.first < r.first; });
  return intersection;
}

} // namespace files