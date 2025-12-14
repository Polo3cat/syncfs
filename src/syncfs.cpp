#include <cstdlib>
#include <exception>
#include <filesystem>
#include <map>
#include <print>
#include <ranges>
#include <span>
#include <utility>

#include <discovery.h>
#include <files.h>
#include <sink.h>
#include <vector>

namespace {
[[noreturn]] void sync_loop(const std::vector<sink::Sink> &remotes)
{
    auto former = files::list();
    // This obviously has to use inotify
    while (true) {
        auto current = files::list();

        const auto removed = files::diff(former, current);
        const auto created = files::diff(current, former);
        const auto modified = files::intersection_name(removed, created);

        for (const auto &remote : remotes) {
            remote.remove(files::diff_name(removed, modified));
            remote.create(files::diff_name(created, modified));
            remote.update(modified);
        }

        former = std::move(current);
    }
}
}// namespace

auto main(int argc, char *argv[]) -> int
try {
    if (argc < 2) {
        std::println("Usage: syncf <peers file>");
        std::println("Synchronizes the working directory with <peers file>");
        return EXIT_FAILURE;
    }
    auto args = std::span(argv, size_t(argc));

    auto peers = discovery::discover(std::filesystem::path{ args[1] });
    auto dirs = peers | std::views::transform([](const auto &s) { return std::filesystem::directory_entry{ s }; });
    auto remotes = dirs | std::views::transform([](const auto &d) { return sink::Sink{ d }; });

    sync_loop(std::ranges::to<std::vector<sink::Sink>>(remotes));

} catch (std::exception &e) {
    try {
        std::println("{}", e.what());
    } catch (...) {
        return EXIT_FAILURE;
    }
    return EXIT_FAILURE;
}
