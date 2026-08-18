#include <discovery.h>

#include <array>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace discovery {
auto parse(const std::filesystem::path &peers_file)
    -> std::expected<std::vector<std::string>, std::string> {
  // Read-only: an fstream defaults to in|out and fails to open a peers file
  // that is not writable, leaving the peer list silently empty (V32, B5).
  std::ifstream file{peers_file};
  if (!file.is_open()) {
    return std::unexpected{
        std::format("Cannot read peers file \"{}\"", peers_file.native())};
  }
  std::vector<std::string> r;
  for (std::array<char, max_line_len> line{}; !file.eof();) {
    file.getline(line.data(), line.size());
    if (file.bad()) {
      return std::unexpected{
          std::format("Cannot read peers file \"{}\"", peers_file.native())};
    }
    // getline raises failbit when it fills the buffer without reaching the
    // delimiter. Reading that as end of input would keep the truncated address
    // and drop every peer listed below it, silently: the daemon would run with
    // part of its peer set and nothing would say which part.
    if (file.fail() && !file.eof()) {
      return std::unexpected{
          std::format("Line longer than {} characters in peers file \"{}\"",
                      max_line_len, peers_file.native())};
    }
    const auto address = std::string_view{line.data()};
    if (address.empty()) {
      continue;
    }
    r.emplace_back(address);
  }
  return r;
}
} // namespace discovery
