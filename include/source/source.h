#pragma once

#include <filesystem>
#include <iterator>
#include <string_view>
#include <utils.h>
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
  // Where this node publishes, in the form the peers file uses. It rides on
  // the root hash alone: a hash nobody can answer is worth nothing, while the
  // digest that answers one is addressed at its reader already and needs to
  // name nobody.
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
  // later is then a change of length and not a change of format. Broadcast, and
  // it names this node: a hash whose sender is anonymous cannot be answered.
  void state(std::string_view hashes) const;

  // Everything this node holds and everything it knows to have been deleted,
  // in full, addressed at the one peer whose root hash differed. Broadcasting
  // it costs every other peer a message of the same size for nothing, every
  // round, for as long as the two of them disagree. A digest is bookkeeping, so
  // unlike a repair there is nothing in it for a second reader. It carries no
  // sender: three parts, the address in the first of them and the two sets
  // after it.
  void digest(utils::Endpoint target, std::string_view held,
              std::string_view deleted) const;

  // The announcement this node last saw for a path, said again. Nothing is
  // read off disk and nothing is hashed: hashing a holder's whole tree inside
  // the sync loop is the cost that made a one gigabyte file miss its deadline
  // once already, and the session cannot rebuild the announcement it was
  // added from.
  void repair(const std::filesystem::path &file,
              std::filesystem::file_time_type mtime,
              std::string_view bencoded) const;
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