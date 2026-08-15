#pragma once

#include <filesystem>
#include <iterator>
#include <zmq.hpp>

namespace source {
template <typename T>
concept Iterable = requires(T t) { std::begin(t); };
template <typename T>
concept Pair = requires(T t) {
  t.first;
  t.second;
};

struct Source {
  mutable zmq::socket_t client;
  std::pair<std::string, int> addr;

  explicit Source(zmq::socket_t &&client,
                  const std::pair<std::string, int> &addr)
      : client{std::move(client)}, addr{addr} {}

  template <Iterable T> void create(const T &container) const;
  template <Pair T> void create(const T &pair) const;
  // The time is the origin one: the modification time this node observed,
  // which is what orders the file against every other copy of it.
  void create(const std::filesystem::path &file,
              std::filesystem::file_time_type mtime) const;

  template <Iterable T> void remove(const T &container) const;
  template <Pair T> void remove(const T &pair) const;
  void remove(const std::filesystem::path &file) const;
};

template <Iterable T> void Source::create(const T &container) const {
  for (const auto &el : container) {
    create(el);
  }
}
template <Pair T> void Source::create(const T &pair) const {
  create(pair.first, pair.second);
}

template <Iterable T> void Source::remove(const T &container) const {
  for (const auto &el : container) {
    remove(el);
  }
}
template <Pair T> void Source::remove(const T &pair) const {
  remove(pair.first);
}
} // namespace source