#include <algorithm>
#include <expected>
#include <filesystem>
#include <iterator>
#include <map>
#include <ranges>
#include <string>
#include <system_error>
#include <utility>

#include <files.h>

namespace files {
namespace {
    auto last_write_time(const std::filesystem::directory_entry &entry)
        -> std::expected<std::filesystem::file_time_type, std::error_code>
    {
        std::error_code err;
        const auto time = entry.last_write_time(err);
        if (err) { return std::unexpected(err); }
        return time;
    }
}// namespace

auto list() -> files::file_map_t
{
    return std::views::all(std::filesystem::recursive_directory_iterator(std::filesystem::current_path()))
           | std::views::transform(
               [](const auto &entry_it) { return std::pair(entry_it.path(), last_write_time(entry_it)); })
           | std::views::filter([](const auto &path_write_time) { return path_write_time.second.has_value(); })
           | std::views::transform([](const auto &path_write_time) {
                 return std::pair(path_write_time.first, path_write_time.second.value());
             })
           | std::ranges::to<std::map<std::string, std::filesystem::file_time_type>>();
}

auto diff(const file_map_t &left, const file_map_t &right) -> file_map_t
{
    file_map_t diff;
    std::ranges::set_difference(left, right, std::inserter(diff, diff.end()));
    return diff;
}

auto diff_name(const file_map_t &left, const file_map_t &right) -> file_map_t
{
    file_map_t diff_name;
    std::ranges::set_difference(
        left, right, std::inserter(diff_name, diff_name.end()), [](const auto &l, const auto &r) {
            return l.first < r.first;
        });
    return diff_name;
}

auto intersection_name(const file_map_t &left, const file_map_t &right) -> file_map_t
{
    file_map_t intersection;
    std::ranges::set_intersection(
        left, right, std::inserter(intersection, intersection.end()), [](const auto &l, const auto &r) {
            return l.first < r.first;
        });
    return intersection;
}

}// namespace files