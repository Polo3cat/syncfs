#pragma once

#include <filesystem>
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
    std::filesystem::directory_entry e{ std::filesystem::temp_directory_path() };

    template<Iterable T> void create(T container) const
    {
        for (const auto &el : container) { create(el); }
    }
    template<Pair T> void create(T pair) const { create(pair.first); }

    void create(const std::filesystem::path &file) const
    {
        std::println("Add {}", file.native());
        std::filesystem::copy_file(file, e.path() / file.filename(), std::filesystem::copy_options::overwrite_existing);
    }

    template<Iterable T> void remove(T container) const
    {
        for (const auto &el : container) { remove(el); }
    }
    template<Pair T> void remove(T pair) const { remove(pair.first); }
    void remove(const std::filesystem::path &file) const
    {
        std::println("Remove {}", file.native());
        std::filesystem::remove(e.path() / file.filename());
    }

    template<Iterable T> void update(T container) const
    {
        for (const auto &el : container) { update(el); }
    }
    template<Pair T> void update(T pair) const { update(pair.first); }
    void update(const std::filesystem::path &file) const
    {
        std::println("Update {}", file.native());
        std::filesystem::copy_file(file, e.path() / file.filename(), std::filesystem::copy_options::overwrite_existing);
    }
};
}// namespace sink