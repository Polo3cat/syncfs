#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <files.h>
#include <sink.h>

using file_map_t = std::map<std::string, std::filesystem::file_time_type>;
using file_vec_t = std::vector<std::pair<std::string, std::filesystem::file_time_type>>;

namespace {
void print(const auto &containter)
{
    std::stringstream ss;
    for (const auto &it : containter) {
        ss << it.first << " " << std::format("{}", std::chrono::floor<std::chrono::seconds>(it.second)) << '\n';
    }
    std::cout << ss.str() << std::flush;
}
}// namespace

auto main() -> int
try {
    auto former = files::Files();
    const sink::Sink remote;
    while (true) {
        auto current = files::Files(former);
        if (not current.removed_files.empty()) {
            remote >> current.removed_files;
        }
        if (not current.added_files.empty()) {
            remote << current.added_files;
        }
        former = std::move(current);
    }
} catch (...) {
    return EXIT_FAILURE;
}
