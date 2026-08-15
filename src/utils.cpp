#include "utils.h"

#include <boost/algorithm/string/split.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
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

auto utils::to_ticks(std::chrono::system_clock::time_point t) -> std::int64_t {
  return t.time_since_epoch().count();
}

auto utils::to_ticks(std::filesystem::file_time_type t) -> std::int64_t {
  return to_ticks(std::chrono::file_clock::to_sys(t));
}
