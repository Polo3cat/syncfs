#include <cstdlib>
#include <exception>
#include <filesystem>
#include <map>
#include <print>
#include <span>
#include <utility>

#include <files.h>
#include <sink.h>

namespace {
[[noreturn]] void sync_loop(const sink::Sink &remote)
{
    auto former = files::list();
    // This obviously has to use inotify
    while (true) {
        auto current = files::list();

        const auto removed = files::diff(former, current);
        const auto created = files::diff(current, former);
        const auto modified = files::intersection_name(removed, created);

        remote.remove(files::diff_name(removed, modified));
        remote.create(files::diff_name(created, modified));
        remote.update(modified);

        former = std::move(current);
    }
}
}// namespace

auto main(int argc, char *argv[]) -> int
try {
    if (argc < 2) {
        std::println("Usage: syncf <destination>");
        std::println("Synchronizes the working directory with <destination>");
        return EXIT_FAILURE;
    }
    auto args = std::span(argv, size_t(argc));

    const sink::Sink remote{ std::filesystem::directory_entry{ args[1] } };

    sync_loop(remote);
} catch (std::exception &e) {
    try {
        std::println("{}", e.what());
    } catch (...) {
        return EXIT_FAILURE;
    }
    return EXIT_FAILURE;
}
