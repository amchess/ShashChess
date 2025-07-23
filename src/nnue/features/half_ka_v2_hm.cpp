/*
  ShashChess, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2025 The ShashChess developers (see AUTHORS file)

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

//Definition of input features HalfKAv2_hm of NNUE evaluation function

#include "half_ka_v2_hm.h"

#include "../../bitboard.h"
#include "../../position.h"
#include "../../types.h"
#include "../nnue_common.h"

namespace ShashChess::Eval::NNUE::Features {

// Index of a feature for a given king position and another piece on some square
template<Color Perspective>
inline IndexType HalfKAv2_hm::make_index(Square s, Piece pc, Square ksq) {
    return IndexType((int(s) ^ OrientTBL[Perspective][ksq]) + PieceSquareIndex[Perspective][pc]
                     + KingBuckets[Perspective][ksq]);
}

// Get a list of indices for active features
template<Color Perspective>
void HalfKAv2_hm::append_active_indices(const Position& pos, IndexList& active) {
    Square   ksq = pos.square<KING>(Perspective);
    Bitboard bb  = pos.pieces();
    while (bb)
    {
        Square s = pop_lsb(bb);
        active.push_back(make_index<Perspective>(s, pos.piece_on(s), ksq));
    }
}

// Explicit template instantiations
template void HalfKAv2_hm::append_active_indices<WHITE>(const Position& pos, IndexList& active);
template void HalfKAv2_hm::append_active_indices<BLACK>(const Position& pos, IndexList& active);
template IndexType HalfKAv2_hm::make_index<WHITE>(Square s, Piece pc, Square ksq);
template IndexType HalfKAv2_hm::make_index<BLACK>(Square s, Piece pc, Square ksq);

// Get a list of indices for recently changed features
template<Color Perspective>
void HalfKAv2_hm::append_changed_indices(Square            ksq,
                                         const DirtyPiece& dp,
                                         IndexList&        removed,
                                         IndexList&        added) {
    removed.push_back(make_index<Perspective>(dp.from, dp.pc, ksq));
    if (dp.to != SQ_NONE)
        added.push_back(make_index<Perspective>(dp.to, dp.pc, ksq));

    if (dp.remove_sq != SQ_NONE)
        removed.push_back(make_index<Perspective>(dp.remove_sq, dp.remove_pc, ksq));

    if (dp.add_sq != SQ_NONE)
        added.push_back(make_index<Perspective>(dp.add_sq, dp.add_pc, ksq));
}

// Explicit template instantiations
template void HalfKAv2_hm::append_changed_indices<WHITE>(Square            ksq,
                                                         const DirtyPiece& dp,
                                                         IndexList&        removed,
                                                         IndexList&        added);
template void HalfKAv2_hm::append_changed_indices<BLACK>(Square            ksq,
                                                         const DirtyPiece& dp,
                                                         IndexList&        removed,
                                                         IndexList&        added);

bool HalfKAv2_hm::requires_refresh(const DirtyPiece& dirtyPiece, Color perspective) {
    return dirtyPiece.pc == make_piece(perspective, KING);
}

}  // namespace ShashChess::Eval::NNUE::Features
