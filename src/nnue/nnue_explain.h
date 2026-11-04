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

// [SC:explain] NNUE explanation toolkit, used only by the evaluation trace.
//
// The network of ShashChess 42 has three families of input features, summed into the same
// 1024-wide accumulator of each perspective:
//   - HalfKAv2_hm : every piece on its square, relative to the own king (piece placement)
//   - FullThreats : every "piece X attacks/defends piece Y" relation (tactics, coordination)
//   - PP_3Wide    : every pair of pawns on the same or adjacent files (pawn structure)
// followed by 8 layer stacks (one per piece-count bucket).
//
// The Explainer rebuilds the accumulators from scratch for an arbitrary board (or an arbitrary
// subset of the active features) and evaluates them with any layer stack. This allows:
//   - bucket-consistent ablations (remove a piece, keep the same sub-network),
//   - occlusion of single relations (how much does "Bg5 pins Nf6" weigh for the network?),
//   - attribution to the three feature families,
//   - virtual insertion of pieces on any square (no 32-pieces limit, the bucket is fixed).

#ifndef NNUE_EXPLAIN_H_INCLUDED
#define NNUE_EXPLAIN_H_INCLUDED

#include <array>
#include <vector>

#include "../types.h"
#include "nnue_architecture.h"
#include "nnue_common.h"

namespace ShashChess {
class Position;
}

namespace ShashChess::Eval::NNUE {

class Network;

namespace Explain {

using Board = std::array<Piece, SQUARE_NB>;

enum RelationKind : u8 {
    REL_THREAT,     // pcA on sqA attacks (or defends) pcB on sqB
    REL_PAWN_PAIR,  // pawns pcA on sqA and pcB on sqB, same or adjacent files
};

struct Relation {
    RelationKind kind;
    Piece        pcA, pcB;
    Square       sqA, sqB;
    IndexType    idx[COLOR_NB];  // feature index for each perspective (NotActive if excluded)
};

constexpr IndexType NotActive = IndexType(-1);

struct FeatureSet {
    std::vector<IndexType> psq[COLOR_NB];
    std::vector<Relation>  relations;
};

// Accumulator with 32 bit lanes: rows can be added and removed freely, the conversion to the
// 16 bit lanes of the network happens only when evaluating (it wraps exactly as the engine does)
struct WideAccumulator {
    std::array<std::array<i32, L1>, COLOR_NB>          acc;
    std::array<std::array<i32, PSQTBuckets>, COLOR_NB> psqt;
};

struct Output {
    Value psqt       = VALUE_ZERO;  // linear "material/placement" part
    Value positional = VALUE_ZERO;  // non-linear layer-stack part
    Value total() const { return psqt + positional; }
};

class Explainer {
   public:
    explicit Explainer(const Network& network) :
        net(network) {}

    // Active features of a board (both kings must be on it)
    static FeatureSet collect(const Board& board);

    WideAccumulator accumulate(const FeatureSet& f, bool threats = true, bool pairs = true) const;
    void            add_relation(WideAccumulator& a, const Relation& r, int sign) const;

    // Evaluation with the given layer stack, point of view of 'stm' (as Network::evaluate)
    Output evaluate(const WideAccumulator& a, Color stm, int bucket) const;
    // Same, White point of view
    Output evaluate_white(const WideAccumulator& a, Color stm, int bucket) const;
    // Shortcut: collect + accumulate + evaluate (White point of view)
    Output evaluate_board_white(const Board& board, Color stm, int bucket) const;

   private:
    const Network& net;
};

int natural_bucket(const Board& board);

}  // namespace Explain
}  // namespace ShashChess::Eval::NNUE

#endif  // #ifndef NNUE_EXPLAIN_H_INCLUDED
