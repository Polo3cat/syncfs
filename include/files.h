#pragma once

#include <filesystem>
#include <map>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace files {

using file_map_t = std::map<std::string, std::filesystem::file_time_type>;
using file_vec_t = std::vector<std::pair<std::string, std::filesystem::file_time_type>>;

auto list_files() -> file_map_t;
auto file_diff(const file_map_t &, const file_map_t &) -> file_vec_t;

struct Files
{
    file_map_t snapshot = list_files();
    file_vec_t added_files = std::ranges::to<file_vec_t>(snapshot);
    file_vec_t removed_files;

    Files(const Files &past)
        : snapshot{ list_files() }, added_files{ file_diff(snapshot, past.snapshot) },
          removed_files{ file_diff(past.snapshot, snapshot) }
    {}
    Files() = default;
    ~Files() = default;
    auto operator=(Files &&) -> Files & = default;

    Files(Files &&) = delete;
    auto operator=(const Files &) -> Files & = delete;
};
}// namespace files