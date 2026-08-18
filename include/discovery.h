#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace discovery {
// The peer set, one address per line, in the form tcp://host:port (I). Blank
// lines are skipped; a line longer than the 512 characters I allows, and a file
// that cannot be read at all, are errors rather than a shorter peer list. A
// daemon that starts with the peers it could parse and none of the ones it
// could not publishes to nobody and says so in no log line (B5).
inline constexpr std::size_t max_line_len{512};

[[nodiscard]] auto parse(const std::filesystem::path &peers_file)
    -> std::expected<std::vector<std::string>, std::string>;
} // namespace discovery
