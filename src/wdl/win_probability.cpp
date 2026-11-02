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

#include "win_probability.h"
#include <cmath>
#include <sstream>

namespace ShashChess {
namespace WDLModel {

namespace {
// Fattorizzazione dei coefficienti del modello WDL di Stockfish 19
constexpr double as[] = {-142.72052667, 372.35176398, -340.71073572, 415.23490212};
constexpr double bs[] = {5.93832785, 15.61267078, -30.57816876, 69.63866711};
}

// 8001 * 62 = 496062: WDL array dimension
#define WIN_PROBABILITY_SIZE 496062

// WDL array definition
WDL wdl_data[WIN_PROBABILITY_SIZE];

//Function to calculate the index in the array
inline size_t index(const ShashChess::Value v, const int m) {
    assert(m >= 17 && m <= 78);

    return (v + 4000) * 62 + m - 17;
}

// [SC:shashin] begin
// Calculate the "a" and "b" parameters of the WDL model for a given material
WinRateParams win_rate_params(int materialClamp) {
    double m = materialClamp / 58.0;

    double a = (((as[0] * m + as[1]) * m + as[2]) * m) + as[3];
    double b = (((bs[0] * m + bs[1]) * m + bs[2]) * m) + bs[3];

    return {a, b};
}
// [SC:shashin] end

WinRateParams win_rate_params(const Position& pos) {

    int material = pos.count<PAWN>() + 3 * pos.count<KNIGHT>() + 3 * pos.count<BISHOP>()
                 + 5 * pos.count<ROOK>() + 9 * pos.count<QUEEN>();

    // The fitted model only uses data for material counts in [17, 78], and is anchored at count 58.
    double m = std::clamp(material, 17, 78) / 58.0;

    double a = (((as[0] * m + as[1]) * m + as[2]) * m) + as[3];
    double b = (((bs[0] * m + bs[1]) * m + bs[2]) * m) + bs[3];

    return {a, b};
}

// Calcola the win rate by using the WDL model
// The win rate model is 1 / (1 + exp((a - eval) / b)), where a = p_a(material) and b = p_b(material).
// It fits the LTC fishtest statistics rather accurately.
int win_rate_model(Value v, const Position& pos) {  //resta invariao

    auto [a, b] = win_rate_params(pos);

    // Return the win rate in per mille units, rounded to the nearest integer.
    return int(0.5 + 1000 / (1 + std::exp((a - double(v)) / b)));
}
static bool initialized = false;  // Inizialization flag
// Function to inizialize the WDL array
void init() {
    if (initialized)
        return;  // Evita di ricaricare se già inizializzato
    initialized = true;
    for (int valueClamp = -4000; valueClamp <= 4000; ++valueClamp)
    {
        for (int materialClamp = 17; materialClamp <= 78; ++materialClamp)
        {
            auto [a, b] = win_rate_params(materialClamp);

            double w = 0.5 + 1000 / (1 + std::exp((a - double(valueClamp)) / b));
            double l = 0.5 + 1000 / (1 + std::exp((a - double(-valueClamp)) / b));
            double d = 1000 - w - l;

            wdl_data[index(valueClamp, materialClamp)] = {static_cast<uint8_t>(round(w / 10.0)),
                                                          static_cast<uint8_t>(round(d / 10.0)),
                                                          static_cast<uint8_t>(round(l / 10.0))};
        }
    }
}
bool is_initialized() { return initialized; }
WDL  get_precomputed_wdl(int valueClamp, int materialClamp) {
    return wdl_data[index(valueClamp, materialClamp)];
}
// Functions to get WDL or win probability
WDL get_wdl_by_material(const Value value, const int materialClamp) {
    const auto valueClamp = std::clamp(static_cast<int>(value), -4000, 4000);
    return get_precomputed_wdl(valueClamp, materialClamp);
}
WDL get_wdl(const Value value, const Position& pos) {
    const int material = pos.count<PAWN>() + 3 * pos.count<KNIGHT>() + 3 * pos.count<BISHOP>()
                       + 5 * pos.count<ROOK>() + 9 * pos.count<QUEEN>();

    const int materialClamp = std::clamp(material, 17, 78);
    return get_wdl_by_material(value, materialClamp);
}
uint8_t get_win_probability_by_material(const Value value, const int materialClamp) {
    WDL wdl = get_wdl_by_material(value, materialClamp);
    return static_cast<uint8_t>(wdl.win + wdl.draw / 2);
}
uint8_t get_win_probability(const Value value, const Position& pos) {
    WDL wdl = get_wdl(value, pos);
    return static_cast<uint8_t>(wdl.win + wdl.draw / 2);
}
uint8_t get_win_probability(const Value value, const int plies) {
    const int full_moves = plies / 2 + 1;
    auto [a, b]          = win_rate_params(full_moves);

    double w = 0.5 + 1000 / (1 + std::exp((a - double(value)) / b));
    double l = 0.5 + 1000 / (1 + std::exp((a - double(-value)) / b));
    double d = 1000 - w - l;

    return static_cast<uint8_t>(round((w + d / 2.0) / 10.0));
}
std::string wdl(Value v, const Position& pos) {
    WDL               wdl = get_wdl(v, pos);
    std::stringstream ss;
    ss << int(wdl.win * 10) << " " << int(wdl.draw * 10) << " " << int(wdl.loss * 10);
    return ss.str();
}


}
}
#undef WIN_PROBABILITY_SIZE