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

#ifndef NNUE_MISC_H_INCLUDED
#define NNUE_MISC_H_INCLUDED

#include <string>
#include <string_view>
#include <optional>
#include <filesystem>

#include "../misc.h"
#include "../types.h"
#include "nnue_architecture.h"
#include "../evaluate.h"
#include "nnue_explain.h"

#include <array>
#include <vector>

namespace ShashChess {

class Position;

namespace Eval::NNUE {

// NNUE file metadata uses fixed string types so it stays trivially copyable and cheap to move
// around between the engine and the network loader.
struct EvalFile {
    // Default net name, will use one of the EvalFileDefaultName* macros defined
    // in evaluate.h
    constexpr static std::string_view defaultName = EvalFileDefaultName;
    // Selected net name, either via uci option or default
    std::optional<std::filesystem::path> current;
    // Net description extracted from the net file
    std::string netDescription;
};

struct NnueEvalTrace {
    static_assert(LayerStacks == PSQTBuckets);

    Value psqt[LayerStacks];
    Value positional[LayerStacks];
    usize correctBucket;
};

class Network;
struct AccumulatorCaches;

// [SC:explain] begin
// Structured result of the NNUE explanation (see nnue_explain.h). Values are internal units,
// White point of view, computed with the layer stack of the position ('bucket').
struct PieceInsight {
    Square sq;
    Piece  pc;
    Value  value;       // paired ablation (owner's view): evaluation lost by the owner when the
                        // piece AND an equivalent enemy piece are removed together (pawn-pawn,
                        // minor-minor with knight and bishop interchangeable, rook-rook,
                        // queen-queen), averaged over all the equivalent enemy pieces: > 0 the
                        // piece is worth more than its counterparts. Material stays balanced, so
                        // the value measures the quality of the piece, not its material.
                        // Without counterparts: the single ablation ('counterparts' == 0)
    Value  psqt;        // part of 'value' due to the linear PSQT output
    Value  positional;  // part of 'value' due to the layer stack
    Value  single;      // single ablation: evaluation lost by the owner if only this piece is
                        // removed (< 0: BLINDSPOT, the owner is better off without the piece)
    int    counterparts;  // number of equivalent enemy pieces averaged in 'value' (0: single)
    Value  activity;    // value of the piece's relations only (its attacks/defences and the
                        // attacks/defences it receives), owner's view: the piece stays on the
                        // board, so the network is not pushed into its saturated regime
    int    efficiency;  // paired: 100% + value / reference value of the piece type in this
                        // position (100% = as good as its counterparts); single: value / reference
};

struct RelationInsight {
    Explain::Relation rel;
    Value             value;  // what the relation is worth for White (occlusion)
};

struct ProbeInsight {
    Square    sq;
    PieceType pt;         // best piece type for the square (largest value above its reference)
    Value     gain;       // value of the virtual piece for its owner
    Value     relative;   // gain minus the reference (median) value of that piece type
    Value     byType[3];  // KNIGHT, BISHOP, ROOK (index pt - KNIGHT)
};

struct SynergyInsight {
    Square a, b;
    Value  synergy;  // activity of the pair minus the two single activities (owner's view)
};

struct NnueInsight {
    bool                         consistent = false;  // explanation reproduces the engine
    int                          bucket     = 0;
    Explain::Output              base;  // raw network output, White point of view
    Value                        familyPsq = 0, familyThreat = 0, familyPair = 0;
    std::vector<PieceInsight>    pieces;
    std::vector<RelationInsight> threats;  // sorted by |value|
    std::vector<RelationInsight> pairs;    // sorted by |value|
    std::array<Value, SQUARE_NB> pressure{};  // threat relations summed on the target square
    std::array<Value, SQUARE_NB> pawnStructure{};  // pawn-pair relations summed on each pawn
    std::vector<ProbeInsight>    outposts[COLOR_NB];  // sorted by relative value
    Value                        reference[COLOR_NB][PIECE_TYPE_NB]{};  // typical piece values
    std::vector<SynergyInsight>  synergies[COLOR_NB];

    const PieceInsight* piece_on(Square s) const {
        for (const auto& p : pieces)
            if (p.sq == s)
                return &p;
        return nullptr;
    }
};

NnueInsight analyze(const Position& pos, const Network& network, AccumulatorCaches& caches);

// Short human-readable label of a relation, e.g. "Bg5>nf6 pin/attack" or "c4/d5 lever"
std::string relation_label(const Explain::Relation& r);
// [SC:explain] end

std::string trace(Position& pos, const Network& network, AccumulatorCaches& caches);

}  // namespace ShashChess::Eval::NNUE
}  // namespace ShashChess

#endif  // #ifndef NNUE_MISC_H_INCLUDED
