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

// Where a message is addressed, and nothing else that happens to be text. An
// address and a payload are both a string_view, and a digest addressed at its
// own contents reaches nobody and says nothing about it.
struct Endpoint {
  std::string_view value;
};

// The first part of a message addressed at one node: the verb, the endpoint it
// is meant for, and a NUL closing each of them. A subscriber asks for the whole
// of that prefix, and the publisher's own subscription trie then writes the
// message to that one pipe alone.
//
// The trailing NUL is the whole point of the framing. Without it "tcp://h:555"
// is a prefix of "tcp://h:5555" and the node on the shorter port receives every
// message meant for the longer one.
auto address(std::string_view verb, Endpoint at) -> std::string;

// The verb a first part names, whether it is addressed or not: everything up to
// the first NUL, or the whole of it when there is none. A broadcast verb is
// spelled plainly, so the two forms have to be read by the same code or an
// addressed message reads as a verb nobody knows.
auto verb_of(std::string_view part0) -> std::string_view;

// Puts a modification time back on a file. libtorrent writes a received file
// with the receiving node's clock, so without this the origin time the wire
// carried is lost the moment the file reaches disk (V44). Answers whether the
// time could be set.
auto stamp(const std::filesystem::path &p, std::filesystem::file_time_type t)
    -> bool;
} // namespace utils
