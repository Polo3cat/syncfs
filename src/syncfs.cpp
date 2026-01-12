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
#include <format>
#include <sink.h>
#include <vector>
#include <zmq.hpp>

namespace {
struct diff_t
{
    files::file_map_t removed;
    files::file_map_t created;
    files::file_map_t modified;
};

auto create_diff(const files::file_map_t &former, const files::file_map_t &current) -> diff_t
{
    const auto removed = files::diff(former, current);
    const auto created = files::diff(current, former);
    const auto modified = files::intersection_name(removed, created);
    return diff_t{ .removed = files::diff_name(removed, modified),
        .created = files::diff_name(created, modified),
        .modified = modified };
}

void send(const diff_t &diff, const std::vector<sink::Sink> &remotes)
{
    for (const auto &remote : remotes) {
        remote.remove(diff.removed);
        remote.create(diff.created);
        remote.update(diff.modified);
    }
}

void receive(zmq::socket_t &s)
{
    zmq::message_t msg;
    auto res = s.recv(msg, zmq::recv_flags::dontwait);
    if (res.has_value()) {
        std::println("Server received {} bytes", res.value());
        std::println("From client {}", msg.routing_id());
        std::println("Message contents follow");
        std::println("{}", msg.to_string_view());
    }
}

void sync_loop(const std::vector<sink::Sink> &remotes, zmq::socket_t server)
{
    auto former = files::list();
    // This obviously has to use inotify
    while (true) {
        auto current = files::list();
        send(create_diff(former, current), remotes);
        former = std::move(current);
        receive(server);
    }
}
}// namespace

auto main(int argc, char *argv[]) -> int
try {
    if (argc < 3) {
        std::println("Usage: syncf <peers file> <listen address>");
        std::println();
        std::println("Synchronizes the working directory with <peers file>");
        std::println("<peers file> contains for each line a network address.");
        std::println("Network addresses are IPv4 addresses with a TCP port.");
        std::println("<listen address> is an IPv4 address and port.");
        return EXIT_FAILURE;
    }
    auto args = std::span(argv, size_t(argc));

    zmq::context_t ctx;

    auto peers = discovery::parse(std::filesystem::path{ args[1] });
    auto remotes = peers | std::views::transform([&ctx](const auto &p) { return sink::Sink{ ctx, p }; });

    zmq::socket_t server{ ctx, zmq::socket_type::server };
    server.bind(std::format("tcp://{}", args[2]));

    sync_loop(std::ranges::to<std::vector<sink::Sink>>(remotes), std::move(server));

} catch (zmq::error_t &e) {
    try {
        std::println("{} {}", e.num(), e.what());
    } catch (...) {
        return EXIT_FAILURE;
    }
} catch (std::exception &e) {
    try {
        std::println("{}", e.what());
    } catch (...) {
        return EXIT_FAILURE;
    }
    return EXIT_FAILURE;
}
