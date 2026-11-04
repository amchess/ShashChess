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

// [SC:explain] See nnue_explain.h

#include "nnue_explain.h"

#include <memory>

#include "../attacks.h"
#include "../bitboard.h"
#include "features/full_threats.h"
#include "features/half_ka_v2_hm.h"
#include "features/pp_3wide.h"
#include "network.h"
#include "nnue_feature_transformer.h"

namespace ShashChess::Eval::NNUE::Explain {

namespace {

struct BoardBB {
    Bitboard byColorType[COLOR_NB][PIECE_TYPE_NB] = {};
    Bitboard occupied                              = 0;
    Square   ksq[COLOR_NB]                         = {SQ_NONE, SQ_NONE};

    explicit BoardBB(const Board& b) {
        for (Square s = SQ_A1; s <= SQ_H8; ++s)
            if (b[s] != NO_PIECE)
            {
                byColorType[color_of(b[s])][type_of(b[s])] |= s;
                byColorType[color_of(b[s])][ALL_PIECES] |= s;
                occupied |= s;
                if (type_of(b[s]) == KING)
                    ksq[color_of(b[s])] = s;
            }
    }
    Bitboard pieces(PieceType pt) const {
        return byColorType[WHITE][pt] | byColorType[BLACK][pt];
    }
};

IndexType threat_index(Color persp, Piece a, Square from, Square to, Piece b, Square ksq) {
    const IndexType i = Features::FullThreats::make_index_ext(persp, a, from, to, b, ksq);
    return i < Features::FullThreats::Dimensions ? i : NotActive;
}

}  // namespace

int natural_bucket(const Board& board) {
    int n = 0;
    for (Piece pc : board)
        n += pc != NO_PIECE;
    return std::clamp((n - 1) / 4, 0, int(LayerStacks) - 1);
}

// Mirrors HalfKAv2_hm, FullThreats::append_active_indices() and PP_3Wide::append_active_indices()
FeatureSet Explainer::collect(const Board& board) {
    FeatureSet f;
    BoardBB    bb(board);

    for (Color p : {WHITE, BLACK})
        for (Square s = SQ_A1; s <= SQ_H8; ++s)
            if (board[s] != NO_PIECE)
                f.psq[p].push_back(Features::HalfKAv2_hm::make_index(p, s, board[s], bb.ksq[p]));

    auto add_threat = [&](Piece a, Square from, Square to) {
        Relation r;
        r.kind = REL_THREAT;
        r.pcA  = a;
        r.sqA  = from;
        r.pcB  = board[to];
        r.sqB  = to;
        for (Color p : {WHITE, BLACK})
            r.idx[p] = threat_index(p, a, from, to, r.pcB, bb.ksq[p]);
        if (r.idx[WHITE] != NotActive || r.idx[BLACK] != NotActive)
            f.relations.push_back(r);
    };

    const Bitboard pawnTargets        = bb.pieces(KNIGHT) | bb.pieces(ROOK);
    const Bitboard minorSliderTargets = bb.pieces(PAWN) | bb.pieces(KNIGHT) | bb.pieces(BISHOP)
                                      | bb.pieces(ROOK);
    const Bitboard queenTargets = minorSliderTargets | bb.pieces(QUEEN);

    for (Color c : {WHITE, BLACK})
    {
        const Bitboard cPawns = bb.byColorType[c][PAWN];
        for (Direction d : {c == WHITE ? NORTH_EAST : SOUTH_WEST, c == WHITE ? NORTH_WEST : SOUTH_EAST})
        {
            Bitboard attacks = shift(cPawns, d) & pawnTargets;
            while (attacks)
            {
                Square to = pop_lsb(attacks);
                add_threat(make_piece(c, PAWN), to - d, to);
            }
        }
    }
    for (Color c : {WHITE, BLACK})
        for (PieceType pt = KNIGHT; pt < KING; ++pt)
        {
            Bitboard       b       = bb.byColorType[c][pt];
            const Bitboard targets = pt == KNIGHT || pt == QUEEN ? queenTargets : minorSliderTargets;
            while (b)
            {
                Square   from    = pop_lsb(b);
                Bitboard attacks = Attacks::attacks_bb(pt, from, bb.occupied) & targets;
                while (attacks)
                    add_threat(make_piece(c, pt), from, pop_lsb(attacks));
            }
        }

    auto add_pair = [&](Color ca, Square a, Color cb, Square b) {
        Relation r;
        r.kind = REL_PAWN_PAIR;
        r.pcA  = make_piece(ca, PAWN);
        r.sqA  = a;
        r.pcB  = make_piece(cb, PAWN);
        r.sqB  = b;
        for (Color p : {WHITE, BLACK})
            r.idx[p] = Features::PP_3Wide::make_index_ext(p, ca, a, b, cb, bb.ksq[p]);
        f.relations.push_back(r);
    };
    const Bitboard white = bb.byColorType[WHITE][PAWN];
    const Bitboard black = bb.byColorType[BLACK][PAWN];
    for (Bitboard w = white; w;)
    {
        Square         from = pop_lsb(w);
        const Bitboard band = pawn_pair_bb(from);
        for (Bitboard ww = band & w; ww;)
            add_pair(WHITE, from, WHITE, pop_lsb(ww));
        for (Bitboard wb = band & black; wb;)
            add_pair(WHITE, from, BLACK, pop_lsb(wb));
    }
    for (Bitboard bl = black; bl;)
    {
        Square         from = pop_lsb(bl);
        const Bitboard band = pawn_pair_bb(from);
        for (Bitboard bbk = band & bl; bbk;)
            add_pair(BLACK, from, BLACK, pop_lsb(bbk));
    }
    return f;
}

WideAccumulator Explainer::accumulate(const FeatureSet& f, bool threats, bool pairs) const {
    const FeatureTransformer& ft = net.feature_transformer();
    auto                      a  = std::make_unique<WideAccumulator>();
    for (Color p : {WHITE, BLACK})
    {
        for (IndexType j = 0; j < L1; ++j)
            a->acc[p][j] = ft.biases[j];
        a->psqt[p].fill(0);
        for (IndexType idx : f.psq[p])
        {
            const auto* w = &ft.weights[usize(idx) * L1];
            for (IndexType j = 0; j < L1; ++j)
                a->acc[p][j] += w[j];
            for (IndexType k = 0; k < PSQTBuckets; ++k)
                a->psqt[p][k] += ft.psqtWeights[usize(idx) * PSQTBuckets + k];
        }
    }
    for (const auto& r : f.relations)
        if ((r.kind == REL_THREAT && threats) || (r.kind == REL_PAWN_PAIR && pairs))
            add_relation(*a, r, +1);
    return *a;
}

void Explainer::add_relation(WideAccumulator& a, const Relation& r, int sign) const {
    const FeatureTransformer& ft = net.feature_transformer();
    for (Color p : {WHITE, BLACK})
    {
        if (r.idx[p] == NotActive)
            continue;
        const auto* w = &ft.threatAndPpWeights[usize(r.idx[p]) * L1];
        for (IndexType j = 0; j < L1; ++j)
            a.acc[p][j] += sign * i32(w[j]);
        for (IndexType k = 0; k < PSQTBuckets; ++k)
            a.psqt[p][k] += sign * ft.threatAndPpPsqtWeights[usize(r.idx[p]) * PSQTBuckets + k];
    }
}

Output Explainer::evaluate(const WideAccumulator& a, Color stm, int bucket) const {
    auto ea = std::make_unique<ExplainAccumulator>();
    for (Color p : {WHITE, BLACK})
    {
        for (IndexType j = 0; j < L1; ++j)
            ea->accumulation[p][j] = static_cast<i16>(a.acc[p][j]);
        ea->psqtAccumulation[p] = a.psqt[p];
    }
    auto [psqt, positional] = net.evaluate_explain(*ea, stm, bucket);
    return {psqt, positional};
}

Output Explainer::evaluate_white(const WideAccumulator& a, Color stm, int bucket) const {
    Output o = evaluate(a, stm, bucket);
    if (stm == BLACK)
    {
        o.psqt       = -o.psqt;
        o.positional = -o.positional;
    }
    return o;
}

Output Explainer::evaluate_board_white(const Board& board, Color stm, int bucket) const {
    return evaluate_white(accumulate(collect(board)), stm, bucket);
}

}  // namespace ShashChess::Eval::NNUE::Explain
