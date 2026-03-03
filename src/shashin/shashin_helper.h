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
// Helper per range con Enum Sequenziali
// =============================================

// Verifica se 'value' è compreso tra 'lower' e 'upper' (inclusi)
inline constexpr bool isInRange(ShashinPosition value, 
                                ShashinPosition lower, 
                                ShashinPosition upper) {
    auto v = to_underlying(value);
    auto l = to_underlying(lower);
    auto u = to_underlying(upper);
    return v >= l && v <= u;
}

// =============================================
// Template Metaprogramming Utilities
// =============================================

template<typename... Ts>
using are_all_shashin = std::conjunction<std::is_same<Ts, ShashinPosition>...>;

// =============================================
// Core Operations (Type-Safe)
// =============================================

// Uso confronto diretto (fold expression)
template<typename... Args>
inline constexpr bool anyOf(ShashinPosition value, Args... ranges) {
    static_assert((std::is_same_v<ShashinPosition, Args> && ...),
                  "All ranges must be ShashinPosition");

    return ((value == ranges) || ...);
}

}  // namespace ShashChess

#endif  // SHASHIN_HELPER_H