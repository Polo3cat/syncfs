#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace discovery {
[[nodiscard]] auto discover(const std::filesystem::path &peers_file) -> std::vector<std::string>;
}// namespace discovery
