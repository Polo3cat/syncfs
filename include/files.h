#pragma once

#include <filesystem>
#include <map>

namespace files {

using file_map_t =
    std::map<std::filesystem::path, std::filesystem::file_time_type>;

auto list() -> file_map_t;
auto diff(const file_map_t &, const file_map_t &) -> file_map_t;
auto diff_name(const file_map_t &left, const file_map_t &right) -> file_map_t;
auto intersection_name(const file_map_t &, const file_map_t &) -> file_map_t;
auto append(file_map_t &&, std::string_view) -> file_map_t;
auto remove(file_map_t &&, std::string_view) -> file_map_t;
} // namespace files