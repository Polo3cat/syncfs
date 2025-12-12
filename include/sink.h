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
    template<Iterable T> void operator<<(T container) const
    {
        for (const auto &el : container) { *this << el; }
    }
    template<Pair T> void operator<<(T pair) const { *this << pair.first; }
    void operator<<(std::string file) const { std::println("Add {}", file); }

    template<Iterable T> void operator>>(T container) const
    {
        for (const auto &el : container) { *this >> el; }
    }
    template<Pair T> void operator>>(T pair) const { *this >> pair.first; }
    void operator>>(std::string file) const { std::println("Remove {}", file); }

    
};
}// namespace sink