#include "utils.h"

#include <algorithm>
#include <boost/algorithm/string/split.hpp>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

auto utils::parse_host_port(const std::string &s)
    -> std::pair<std::string, int> {
  std::vector<std::string> tokens;
  boost::algorithm::split(tokens, s, [](char c) -> bool { return c == ':'; });
  if (tokens.at(0).empty()) {
    throw std::invalid_argument(s);
  }
  return {tokens.at(0), std::stoi(tokens.at(1))};
}

namespace {
// Everything before the last colon with any brackets taken off, which is the
// host even when it is an IPv6 literal carrying colons of its own.
auto host_of(std::string_view addr) -> std::string_view {
  const auto colon = addr.rfind(':');
  auto host = colon == std::string_view::npos ? addr : addr.substr(0, colon);
  if (host.starts_with('[')) {
    host.remove_prefix(1);
  }
  if (host.ends_with(']')) {
    host.remove_suffix(1);
  }
  return host;
}
} // namespace

auto utils::check_listen_address(std::string_view addr)
    -> std::expected<void, std::string> {
  const auto host = host_of(addr);
  // Zeros, dots and colons and nothing else is every spelling of the
  // unspecified address there is - 0.0.0.0, ::, 0:0:0:0:0:0:0:0, and the empty
  // host - and no host that names one machine can be written without a letter
  // or a digit that is not zero. The ZMQ wildcard is its own spelling of the
  // same thing.
  const auto anywhere =
      host == "*" || std::ranges::all_of(host, [](char c) -> bool {
        return c == '0' || c == '.' || c == ':';
      });
  if (anywhere) {
    return std::unexpected{std::format(
        "\"{}\" names no one host, and this address is what peers address "
        "their digests to as well as what the publisher binds",
        host)};
  }
  return {};
}

auto utils::to_ticks(std::chrono::system_clock::time_point t) -> std::int64_t {
  return t.time_since_epoch().count();
}

auto utils::to_ticks(std::filesystem::file_time_type t) -> std::int64_t {
  return to_ticks(std::chrono::file_clock::to_sys(t));
}

auto utils::from_ticks(std::string_view s)
    -> std::chrono::system_clock::time_point {
  std::int64_t ticks = 0;
  // from_chars is the only conversion that refuses trailing garbage without
  // throwing, and it speaks in pointers.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const auto *const end = s.data() + s.size();
  // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
  const auto parsed = std::from_chars(s.data(), end, ticks);
  if (parsed.ec != std::errc{} || parsed.ptr != end) {
    return std::chrono::system_clock::time_point{};
  }
  return std::chrono::system_clock::time_point{
      std::chrono::system_clock::duration{ticks}};
}

auto utils::to_file_time(std::chrono::system_clock::time_point t)
    -> std::filesystem::file_time_type {
  return std::chrono::file_clock::from_sys(t);
}

auto utils::address(std::string_view verb, Endpoint at) -> std::string {
  auto framed = std::string{verb};
  framed.push_back('\0');
  framed.append(at.value);
  framed.push_back('\0');
  return framed;
}

auto utils::verb_of(std::string_view part0) -> std::string_view {
  return part0.substr(0, part0.find('\0'));
}

auto utils::stamp(const std::filesystem::path &p,
                  std::filesystem::file_time_type t) -> bool {
  std::error_code err;
  std::filesystem::last_write_time(p, t, err);
  return !err;
}
