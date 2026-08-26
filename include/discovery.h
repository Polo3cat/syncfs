#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace discovery {
// Defined as the longest example,
// plus 1 for the null character
// appended by getline.
inline constexpr std::size_t max_line_len =
    std::string_view{"tcp://255.255.255.255:65535"}.length() + 1;

// The peer set, one address per line, in the form tcp://host:port . Blank
// lines are skipped; a line longer than the 512 characters, and a file
// that cannot be read at all, are errors rather than a shorter peer list.
[[nodiscard]] auto parse(const std::filesystem::path &peers_file)
    -> std::expected<std::vector<std::string>, std::string>;
} // namespace discovery
