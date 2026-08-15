#pragma once

#include <filesystem>
#include <iterator>
#include <string_view>
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
  // Where this node publishes, in the form the peers file uses. It rides on a
  // digest so a receiver can tell one of its own from a peer's, which is the
  // only message that needs telling apart: everything else is idempotent.
  std::string endpoint;

  explicit Source(zmq::socket_t &&client,
                  const std::pair<std::string, int> &addr, std::string endpoint)
      : client{std::move(client)}, addr{addr}, endpoint{std::move(endpoint)} {}

  template <Iterable T> void create(const T &container) const;
  template <Pair T> void create(const T &pair) const;
  // The time is the origin one: the modification time this node observed,
  // which is what orders the file against every other copy of it.
  void create(const std::filesystem::path &file,
              std::filesystem::file_time_type mtime) const;

  template <Iterable T> void remove(const T &container) const;
  template <Pair T> void remove(const T &pair) const;
  void remove(const std::filesystem::path &file) const;

  // The root hash of everything this node holds and everything it knows to
  // have been deleted, so a peer can tell in 32 bytes whether the two of them
  // agree. A list of hashes rather than one, because splitting it into buckets
  // later is then a change of length and not a change of format.
  void state(std::string_view hashes) const;

  // Everything this node holds and everything it knows to have been deleted,
  // in full. Only ever published on a hash mismatch, so what is an expensive
  // message in principle is one nobody sends while the peers agree.
  void digest(std::string_view held, std::string_view deleted) const;
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