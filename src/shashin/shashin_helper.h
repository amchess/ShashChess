#ifndef SHASHIN_HELPER_H
#define SHASHIN_HELPER_H

#include <type_traits>
#include "shashin_types.h"

namespace Alexander {

// Helper per verificare se un valore è in uno o più range
template<typename... Args>
inline bool isInRanges(ShashinPosition value, Args... ranges) {
    static_assert((std::is_same_v<ShashinPosition, Args> && ...),
                  "All ranges must be of type ShashinPosition");
    return ((... || ((value & ranges) != ShashinPosition(0))));
}

// Helper per convertire un enum class al tipo sottostante
template<typename Enum>
constexpr auto to_underlying(Enum e) -> std::underlying_type_t<Enum> {
    return static_cast<std::underlying_type_t<Enum>>(e);
}

// Operatore bitwise OR (|)
inline constexpr ShashinPosition operator|(ShashinPosition lhs, ShashinPosition rhs) {
    using T = std::underlying_type_t<ShashinPosition>;
    return static_cast<ShashinPosition>(static_cast<T>(lhs) | static_cast<T>(rhs));
}

// Operatore bitwise AND (&)
inline constexpr ShashinPosition operator&(ShashinPosition lhs, ShashinPosition rhs) {
    using T = std::underlying_type_t<ShashinPosition>;
    return static_cast<ShashinPosition>(static_cast<T>(lhs) & static_cast<T>(rhs));
}

// Operatore NOT (~)
inline constexpr ShashinPosition operator~(ShashinPosition lhs) {
    using T = std::underlying_type_t<ShashinPosition>;
    return static_cast<ShashinPosition>(~static_cast<T>(lhs));
}

// Operatore OR assegnazione (|=)
inline constexpr ShashinPosition& operator|=(ShashinPosition& lhs, ShashinPosition rhs) {
    lhs = lhs | rhs;
    return lhs;
}

// Operatore AND assegnazione (&=)
inline constexpr ShashinPosition& operator&=(ShashinPosition& lhs, ShashinPosition rhs) {
    lhs = lhs & rhs;
    return lhs;
}

}  // namespace Alexander

#endif  // SHASHIN_HELPER_H