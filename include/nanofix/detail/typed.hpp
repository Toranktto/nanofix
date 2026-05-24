#pragma once

#include <cstdint>

namespace nanofix {

/// FIX value-type category a tag belongs to, used to gate typed_value accessors.
enum class fix_type : std::uint8_t { decimal, integer, string, character, timestamp, unknown };

/// Compile-time tag handle carrying its number and value-type category. The
/// generated `tag::` namespace (fields.hpp) is made of these. The implicit
/// `operator int()` keeps every int-context use (`push_back_int(tag::Price, …)`,
/// `it->tag() == tag::Price`, `group(tag::NoMDEntries, …)`) working unchanged,
/// while `find(tag::Price)` resolves to the typed overload and returns a
/// `typed_value`.
template <int Tag, fix_type Type>
struct field_tag {
    static constexpr int tag = Tag;
    static constexpr fix_type type = Type;

    constexpr operator int() const noexcept { return Tag; }
};

}  // namespace nanofix
