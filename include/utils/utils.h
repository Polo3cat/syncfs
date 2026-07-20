#pragma once

#include <string>
#include <utility>

namespace utils {
auto parse_host_port(const std::string &s) -> std::pair<std::string, int>;
}
