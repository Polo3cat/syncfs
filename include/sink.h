#pragma once

#include <filesystem>
#include <iterator>
#include <print>

#include <zmq.hpp>

namespace sink {
template<typename T>
concept Iterable = requires(T t) { std::begin(t); };
template<typename T>
concept Pair = requires(T t) {
    t.first;
    t.second;
};

namespace detail {
    inline auto init_client(zmq::context_t &ctx, const std::string &remote) -> zmq::socket_t
    {
        zmq::socket_t s{ ctx, zmq::socket_type::client };
        s.connect(remote);
        return s;
    }
}// namespace detail

struct Sink
{
    mutable zmq::socket_t client;

    explicit Sink(zmq::context_t &ctx, const std::string &remote) : client{ detail::init_client(ctx, remote) } {}

    template<Iterable T> void create(T container) const
    {
        for (const auto &el : container) { create(el); }
    }
    template<Pair T> void create(T pair) const { create(pair.first); }

    void create(const std::filesystem::path &file) const
    {
        std::println("Add {}", file.native());
        auto msg = std::format("Add {}", file.native());
        auto buffer = zmq::const_buffer(msg.c_str(), msg.length());
        [[maybe_unused]] auto res = client.send(buffer);
    }

    template<Iterable T> void remove(T container) const
    {
        for (const auto &el : container) { remove(el); }
    }
    template<Pair T> void remove(T pair) const { remove(pair.first); }
    void remove(const std::filesystem::path &file) const
    {
        std::println("Remove {}", file.native());
        auto msg = std::format("Remove {}", file.native());
        auto buffer = zmq::const_buffer(msg.c_str(), msg.length());
        [[maybe_unused]] auto res = client.send(buffer);
    }

    template<Iterable T> void update(T container) const
    {
        for (const auto &el : container) { update(el); }
    }
    template<Pair T> void update(T pair) const { update(pair.first); }
    void update(const std::filesystem::path &file) const
    {
        std::println("Update {}", file.native());
        auto msg = std::format("Update {}", file.native());
        auto buffer = zmq::const_buffer(msg.c_str(), msg.length());
        [[maybe_unused]] auto res = client.send(buffer);
    }
};
}// namespace sink