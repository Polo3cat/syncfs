#include "utils.h"

#include <boost/algorithm/string/split.hpp>
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
