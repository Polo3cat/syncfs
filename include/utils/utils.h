#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>

namespace utils {
auto parse_host_port(const std::string &s) -> std::pair<std::string, int>;

// The wire carries times as system_clock ticks. The epoch of file_time_type is
// implementation defined, so a file timestamp only means the same thing on two
// nodes once it has been converted.
auto to_ticks(std::chrono::system_clock::time_point t) -> std::int64_t;
auto to_ticks(std::filesystem::file_time_type t) -> std::int64_t;
} // namespace utils
