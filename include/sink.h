#pragma once

#include <filesystem>
#include <fstream>
#include <ios>
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
        zmq::socket_t s{ ctx, zmq::socket_type::pub };
        s.connect(remote);
        return s;
    }
}// namespace detail

struct Sink
{
    mutable zmq::socket_t client;

    explicit Sink(zmq::context_t &ctx, const std::string &remote) : client{ detail::init_client(ctx, remote) } {}

    template<Iterable T> void create(const T &container) const
    {
        for (const auto &el : container) { create(el); }
    }
    template<Pair T> void create(const T &pair) const { create(pair.first); }

    void create(const std::filesystem::path &file) const
    {
        std::println("Add {}", file.native());

        static constexpr size_t buf_size = 4ULL * 1024;
        std::array<char, buf_size> file_buf;

        auto file_stream = std::fstream{ file, std::ios_base::in | std::ios_base::binary };
        file_stream.read(file_buf.data(), file_buf.size());

        client.send(zmq::str_buffer("add"), zmq::send_flags::sndmore);
        client.send(zmq::const_buffer(file_buf.data(), file_stream.gcount()));
    }

    template<Iterable T> void remove(const T &container) const
    {
        for (const auto &el : container) { remove(el); }
    }
    template<Pair T> void remove(const T &pair) const { remove(pair.first); }
    void remove(const std::filesystem::path &file) const
    {
        std::println("Remove {}", file.native());

        client.send(zmq::str_buffer("remove"), zmq::send_flags::sndmore);
        client.send(zmq::const_buffer(file.native().c_str(), file.native().size()));
    }

    template<Iterable T> void update(const T &container) const
    {
        for (const auto &el : container) { update(el); }
    }
    template<Pair T> void update(const T &pair) const { update(pair.first); }
    void update(const std::filesystem::path &file) const
    {
        std::println("Update {}", file.native());

        static constexpr size_t buf_size = 4ULL * 1024;
        std::array<char, buf_size> file_buf;

        auto file_stream = std::fstream{ file, std::ios_base::in | std::ios_base::binary };
        file_stream.read(file_buf.data(), file_buf.size());

        client.send(zmq::str_buffer("update"), zmq::send_flags::sndmore);
        client.send(zmq::const_buffer(file_buf.data(), file_stream.gcount()));
    }
};
}// namespace sink