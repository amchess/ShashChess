/*
  ShashChess, a UCI chess playing engine derived from Stockfish
  Copyright (C) 2004-2026 The Stockfish developers (see AUTHORS file)
  Copyright (C) 2018-2026 The ShashChess developers (see AUTHORS file)

  ShashChess is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  ShashChess is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

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
inline constexpr bool
isInRange(ShashinPosition value, ShashinPosition lower, ShashinPosition upper) {
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