#include <iterator>
#include <print>

namespace sink {
template<typename T>
concept Iterable = requires(T t) { std::begin(t); };
template<typename T>
concept Pair = requires(T t) {
    t.first;
    t.second;
};

struct Sink
{
    template<Iterable T> void create(T container) const
    {
        for (const auto &el : container) { create(el); }
    }
    template<Pair T> void create(T pair) const { create(pair.first); }
    static void create(std::string file)  { std::println("Add {}", file); }

    template<Iterable T> void remove(T container) const
    {
        for (const auto &el : container) { remove(el); }
    }
    template<Pair T> void remove(T pair) const { remove(pair.first); }
    static void remove(std::string file)  { std::println("Remove {}", file); }

    template<Iterable T> void update(T container) const
    {
        for (const auto &el : container) { update(el); }
    }
    template<Pair T> void update(T pair) const { update(pair.first); }
    static void update(std::string file)  { std::println("Update {}", file); }

    
};
}// namespace sink