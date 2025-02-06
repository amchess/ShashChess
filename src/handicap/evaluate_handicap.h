/*
  Alexander, a UCI chess playing engine derived from Stockfish
  Copyright (C) 2004-2025 The Alexander developers (see AUTHORS file)

  Alexander is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Alexander is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITYB or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef EVALUATE_HANDICAP_H_INCLUDED
#define EVALUATE_HANDICAP_H_INCLUDED

#include <string>
#include <unordered_map>  //for classical

#include "../types.h"
#include <random>  //from Handicap mode
#include <cstdint>
#include "../wdl/win_probability.h"
namespace Alexander {

class Position;
class OptionsMap;  //for classical

namespace {
//handicap mode begin
// Evaluation weights, initialized from UCI options
enum {
    MATERIAL_INDEX = 0,
    IMBALANCE_INDEX,
    PAWN_STRUCTURE_INDEX,
    KNIGHT_INDEX,
    BISHOP_INDEX,
    ROOK_INDEX,
    QUEEN_INDEX,
    MOBILITY_INDEX,
    KING_SAFETY_INDEX,
    THREATS_INDEX,
    PASSED_PAWN_INDEX,
    SPACE_INDEX,
    WINNABLE_INDEX,
    AVATAR_NB
};
struct Weight {
    std::string mgName;
    std::string egName;
    int         mg, eg;
} Weights[AVATAR_NB] = {{"Material(mg)", "Material(eg)", 100, 100},
                        {"Imbalance(mg)", "Imbalance(eg)", 100, 100},
                        {"PawnStructure(mg)", "PawnStructure(eg)", 100, 100},
                        {"Knight(mg)", "Knight(eg)", 100, 100},
                        {"Bishop(mg)", "Bishop(eg)", 100, 100},
                        {"Rook(mg)", "Rook(eg)", 100, 100},
                        {"Queen(mg)", "Queen(eg)", 100, 100},
                        {"Mobility(mg)", "Mobility(eg)", 100, 100},
                        {"KingSafety(mg)", "KingSafety(eg)", 100, 100},
                        {"Threats(mg)", "Threats(eg)", 100, 100},
                        {"PassedPawns(mg)", "PassedPawns(eg)", 100, 100},
                        {"Space(mg)", "Space(eg)", 100, 100},
                        {"Winnable(mg)", "Winnable(eg)", 100, 100}};
    template<Phase P>
    inline Value apply_weight(Value v, int wi) {
        if constexpr (P == MG)
            return v * Weights[wi].mg / 100;
        else if constexpr (P == EG)
            return v * Weights[wi].eg / 100;
    }

    inline ScoreForClassical apply_weights(ScoreForClassical s, int wi) {
        return make_score(apply_weight<MG>(mg_value(s), wi), apply_weight<EG>(eg_value(s), wi));
    }

}

namespace Eval {
const int   MIN_ELO             = 1320; 
// Definizione delle costanti
constexpr int BEGINNER_MAX_ELO = 1999;
constexpr int INTERMEDIATE_MAX_ELO = 2199;
constexpr int ADVANCED_MAX_ELO = 2399;
constexpr int EXPERT_MIN_ELO = 2400;

constexpr double COMPLEX_POSITION_MULTIPLIER = 1.5;

constexpr int MIN_ERROR_MAGNITUDE_BEGINNER = 50;
constexpr int MAX_ERROR_MAGNITUDE_BEGINNER = 200;
constexpr int MIN_ERROR_MAGNITUDE_INTERMEDIATE = 30;
constexpr int MAX_ERROR_MAGNITUDE_INTERMEDIATE = 100;
constexpr int MIN_ERROR_MAGNITUDE_ADVANCED = 10;
constexpr int MAX_ERROR_MAGNITUDE_ADVANCED = 50;
constexpr int MIN_ERROR_MAGNITUDE_EXPERT = 5;
constexpr int MAX_ERROR_MAGNITUDE_EXPERT = 20;

constexpr int OPENING_PHASE_MAX_PLY = 20;
constexpr int MIDDLEGAME_PHASE_MAX_PLY = 60;
const int   MAX_ELO             = 3190;

extern bool limitStrength, pawnsToEvaluate, winnableToEvaluate, imbalancesToEvaluate,
  simulateHumanBlunders, handicappedDepth;
extern int uciElo, RandomEvalPerturb;
void       loadAvatar(const std::string& fname);  //avatar
void       initHandicapMode(const OptionsMap&);   //handicap mode
double     compute_position_complexity(const Position& pos); 
bool       should_apply_perturbation(int uciElo, const Position& pos) ;
Value      get_perturbated_value(const Position& pos, Value baseEvaluation);
Value      get_handicap_value(Value baseEvaluation, int uciElo, const Position& pos);
bool       isComplex(const Position& pos);
struct MinMax {
    Value min_value;
    Value max_value;
};
extern MinMax min_max_threshold[WDLModel::MAX_WIN_PROBABILITY + 1];
uint8_t get_handicap_max_win_probability(uint8_t wp);
uint8_t get_handicap_min_win_probability(uint8_t wp);
double computeAdjustedComplexityFactor(int uciElo, const Position& pos);
void initHandicapMinMaxValueThresholds();
//true handicap mode end

}  // namespace Eval

}  // namespace Alexander

#endif  // #ifndef EVALUATE_HANDICAP_H_INCLUDED
