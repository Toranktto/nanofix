#pragma once

// Fixture file loader shared by the data-backed tests.

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <vector>

namespace nanofix_test {

inline std::vector<char> read_file(std::filesystem::path const& p) {
    std::ifstream in(p, std::ios::binary | std::ios::ate);
    if (!in)
        return {};
    auto size = in.tellg();
    if (size <= 0)
        return {};
    std::vector<char> buf(static_cast<std::size_t>(size));
    in.seekg(0);
    in.read(buf.data(), size);
    return buf;
}

}  // namespace nanofix_test
