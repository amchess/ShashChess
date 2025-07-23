#ifndef SHASHIN_HELPER_H
#define SHASHIN_HELPER_H

#include <type_traits>
#include <utility>
#include "shashin_types.h"

namespace ShashChess {

template<typename E>
constexpr auto to_underlying(E e) noexcept
  -> std::enable_if_t<std::is_enum_v<E>, std::underlying_type_t<E>> {
    return static_cast<std::underlying_type_t<E>>(e);
}
// =============================================
// Template Metaprogramming Utilities
// =============================================

template<typename... Ts>
using are_all_shashin = std::conjunction<std::is_same<Ts, ShashinPosition>...>;

// =============================================
// Core Bitmask Operations (Type-Safe)
// =============================================

template<typename... Args>
__attribute__((always_inline)) constexpr bool anyOf(ShashinPosition value, Args... ranges) {
    static_assert((std::is_same_v<ShashinPosition, Args> && ...),
                  "All ranges must be ShashinPosition");

    return (((to_underlying(value) & to_underlying(ranges)) != 0) || ...);
}

// =============================================
// Bitwise Operator Overloads
// =============================================

inline constexpr ShashinPosition operator|(ShashinPosition lhs, ShashinPosition rhs) noexcept {
    return static_cast<ShashinPosition>(to_underlying(lhs) | to_underlying(rhs));
}

inline constexpr ShashinPosition operator&(ShashinPosition lhs, ShashinPosition rhs) noexcept {
    return static_cast<ShashinPosition>(to_underlying(lhs) & to_underlying(rhs));
}

inline constexpr ShashinPosition operator~(ShashinPosition v) noexcept {
    return static_cast<ShashinPosition>(~to_underlying(v));
}

inline constexpr ShashinPosition& operator|=(ShashinPosition& lhs, ShashinPosition rhs) noexcept {
    lhs = lhs | rhs;
    return lhs;
}

inline constexpr ShashinPosition& operator&=(ShashinPosition& lhs, ShashinPosition rhs) noexcept {
    lhs = lhs & rhs;
    return lhs;
}

}  // namespace ShashChess

#endif  // SHASHIN_HELPER_H