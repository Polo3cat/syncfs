#include <bits/chrono.h>
#include <cassert>
#include <cstdlib>
#include <discovery.h>
#include <exception>
#include <expected>
#include <files.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <map>
#include <monitor.h>
#include <print>
#include <ranges>
#include <sink.h>
#include <span>
#include <spdlog/common.h>
#include <spdlog/spdlog.h>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <zmq.hpp>
#include <zmq_addon.hpp>

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

auto receive_ready(zmq::poller_t<> &p) -> bool
{
    using namespace std::chrono_literals;
    std::vector<zmq::poller_event<>> evs(1);
    return p.wait_all(evs, 100ms) != 0;
}

[[nodiscard]] auto receive(zmq::socket_t &s) -> std::expected<std::string, std::string>
{
    std::vector<zmq::message_t> recv_msgs;
    zmq::recv_result_t const res = zmq::recv_multipart(s, std::back_inserter(recv_msgs));

    if (!res.has_value() or recv_msgs.empty()) { return std::unexpected{ "Expected to receive messages" }; }
    if (recv_msgs.size() == 1) { return std::unexpected{ "Expected more than 1 message" }; }
    if (recv_msgs.size() == 2 && recv_msgs[0].to_string_view() == "remove") {
        std::filesystem::remove(recv_msgs[1].to_string_view());
        return recv_msgs[1].to_string();
    }
    if (recv_msgs.size() == 3
        && (recv_msgs[0].to_string_view() == "create" || recv_msgs[0].to_string_view() == "update")) {
        std::ofstream file_stream{ recv_msgs[1].to_string(),
            std::ios_base::out | std::ios_base::trunc | std::ios_base::binary };
        file_stream << recv_msgs[2].to_string_view();
        return recv_msgs[1].to_string();
    }
    return std::unexpected{ "Did not receive the expected number of messages" };
}

void sync_loop(const std::vector<sink::Sink> &remotes, zmq::socket_t server)
{
    zmq::poller_t<> in_poller;
    in_poller.add(server, zmq::event_flags::pollin);

    auto const mon = monitor::Monitor();
    auto former = files::list();
    while (true) {
        if (mon.wait()) {
            mon.discard();
            auto current = files::list();
            send(create_diff(former, current), remotes);
            former = std::move(current);
        }
        if (receive_ready(in_poller)) {
            auto const received = receive(server);
            if (received.has_value()) {
                //TODO: Also consider removed files
                spdlog::debug("Received file {}", received.value());
                former = files::append(std::move(former), received.value());
            } else {
                spdlog::warn(received.error());
            }
        }
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

    spdlog::set_pattern("[%Y-%m-%d %T] [%P] [%^%l%$] %v");
    spdlog::set_level(spdlog::level::debug);

    auto args = std::span(argv, size_t(argc));

    auto peers = discovery::parse(std::filesystem::path{ args[1] });
    assert(!peers.empty());

    zmq::context_t ctx;
    auto remotes = peers | std::views::transform([&ctx](const auto &p) { return sink::Sink{ ctx, p }; });

    zmq::socket_t server{ ctx, zmq::socket_type::sub };
    server.bind(std::format("tcp://{}", args[2]));
    server.set(zmq::sockopt::subscribe, "create");
    server.set(zmq::sockopt::subscribe, "remove");
    server.set(zmq::sockopt::subscribe, "update");

    spdlog::info("Started synchronization loop listening on {}", args[2]);
    {
        std::stringstream ss;
        for (const auto &peer : peers) { ss << ' ' << peer; }
        spdlog::info("Sinking to{}", ss.str());
    }

    sync_loop(std::ranges::to<std::vector<sink::Sink>>(remotes), std::move(server));

} catch (zmq::error_t &e) {
    try {
        spdlog::critical("{} {}", e.num(), e.what());
    } catch (...) {
        return EXIT_FAILURE;
    }
} catch (std::exception &e) {
    try {
        spdlog::critical("{}", e.what());
    } catch (...) {
        return EXIT_FAILURE;
    }
    return EXIT_FAILURE;
}
