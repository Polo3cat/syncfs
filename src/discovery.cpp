#include <discovery.h>

#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace discovery {
auto parse(const std::filesystem::path &peers_file) -> std::vector<std::string>
{
    std::fstream file{ peers_file };
    static constexpr size_t max_line_len{ 512 };
    std::vector<std::string> r;
    for (std::array<char, max_line_len> line{}; file.getline(line.data(), line.size());) {
        r.emplace_back(line.data());
    }
    return r;
}
}// namespace discovery
