#pragma once

#include <filesystem>
#include <iterator>
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
    inline auto init_client(zmq::context_t &ctx, const std::string &addr) -> zmq::socket_t
    {
        zmq::socket_t s{ ctx, zmq::socket_type::pub };
        s.bind(addr);
        return s;
    }
}// namespace detail

struct Sink
{
    mutable zmq::socket_t client;

    explicit Sink(zmq::context_t &ctx, const std::string &addr) : client{ detail::init_client(ctx, addr) } {}

    template<Iterable T> void create(const T &container) const;
    template<Pair T> void create(const T &pair) const;
    void create(const std::filesystem::path &file) const;

    template<Iterable T> void remove(const T &container) const;
    template<Pair T> void remove(const T &pair) const;
    void remove(const std::filesystem::path &file) const;

    template<Iterable T> void update(const T &container) const;
    template<Pair T> void update(const T &pair) const;
    void update(const std::filesystem::path &file) const;
};

template<Iterable T> void Sink::create(const T &container) const
{
    for (const auto &el : container) { create(el); }
}
template<Pair T> void Sink::create(const T &pair) const { create(pair.first); }


template<Iterable T> void Sink::remove(const T &container) const
{
    for (const auto &el : container) { remove(el); }
}
template<Pair T> void Sink::remove(const T &pair) const { remove(pair.first); }

template<Iterable T> void Sink::update(const T &container) const
{
    for (const auto &el : container) { update(el); }
}
template<Pair T> void Sink::update(const T &pair) const { update(pair.first); }

}// namespace sink