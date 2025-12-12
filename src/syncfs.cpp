#include <cstdlib>
#include <map>
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

auto main() -> int
try {
    const sink::Sink remote;
    sync_loop(remote);
} catch (...) {
    return EXIT_FAILURE;
}
