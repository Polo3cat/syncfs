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
  // It's necessary to materliaze die to std::view::filter caching
  // the begin iterator. This causes a toctou failure on the value
  // returned by last_write_time. It doesn't check that the value
  // pointed to by the begin iterator is true with the predicate
  // on the actual iteration. The value changes because the entry may
  // change on disk.
  auto entries_with_times =
      std::views::all(std::filesystem::recursive_directory_iterator(".")) |
      std::views::transform([](const auto &entry) -> auto {
        return std::pair(entry, last_write_time(entry));
      }) |
      std::ranges::to<std::vector>();

  return entries_with_times |
         std::views::filter([](const auto &entry_time) -> auto {
           return entry_time.first.is_regular_file() &&
                  !entry_time.first.is_symlink() &&
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