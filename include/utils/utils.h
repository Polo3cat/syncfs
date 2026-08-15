#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

namespace utils {
auto parse_host_port(const std::string &s) -> std::pair<std::string, int>;

// The wire carries times as system_clock ticks. The epoch of file_time_type is
// implementation defined, so a file timestamp only means the same thing on two
// nodes once it has been converted.
auto to_ticks(std::chrono::system_clock::time_point t) -> std::int64_t;
auto to_ticks(std::filesystem::file_time_type t) -> std::int64_t;

// The other direction, for a time that arrived over the wire. A count that is
// not a decimal integer reads as the epoch, which loses every ordering
// comparison there is: a peer that cannot spell the protocol does not get to
// overwrite anything.
auto from_ticks(std::string_view s) -> std::chrono::system_clock::time_point;
auto to_file_time(std::chrono::system_clock::time_point t)
    -> std::filesystem::file_time_type;

// Puts a modification time back on a file. libtorrent writes a received file
// with the receiving node's clock, so without this the origin time the wire
// carried is lost the moment the file reaches disk (V44). Answers whether the
// time could be set.
auto stamp(const std::filesystem::path &p, std::filesystem::file_time_type t)
    -> bool;
} // namespace utils
