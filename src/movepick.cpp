/*
  ShashChess, a UCI chess playing engine derived from Stockfish
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

#include "movepick.h"

#include <cassert>
#include <limits>
#include <utility>
#include <cmath>  //for ShashChess
#include "misc.h"
//for ShashChess

namespace ShashChess {
using namespace ShashChess::Shashin;  //shashin
namespace {

enum Stages {
    // generate main search moves
    MAIN_TT,
    CAPTURE_INIT,
    GOOD_CAPTURE,
    QUIET_INIT,
    GOOD_QUIET,
    BAD_CAPTURE,
    BAD_QUIET,

    // generate evasion moves
    EVASION_TT,
    EVASION_INIT,
    EVASION,

    // generate probcut moves
    PROBCUT_TT,
    PROBCUT_INIT,
    PROBCUT,

    // generate qsearch moves
    QSEARCH_TT,
    QCAPTURE_INIT,
    QCAPTURE
};


// Sort moves in descending order up to and including a given limit.
// The order of moves smaller than the limit is left unspecified.
void partial_insertion_sort(ExtMove* begin, ExtMove* end, int limit) {

    for (ExtMove *sortedEnd = begin, *p = begin + 1; p < end; ++p)
        if (p->value >= limit)
        {
            ExtMove tmp = *p, *q;
            *p          = *++sortedEnd;
            for (q = sortedEnd; q != begin && *(q - 1) < tmp; --q)
                *q = *(q - 1);
            *q = tmp;
        }
}

}  // namespace


// Constructors of the MovePicker class. As arguments, we pass information
// to decide which class of moves to emit, to help sorting the (presumably)
// good moves first, and how important move ordering is at the current node.

// MovePicker constructor for the main search and for the quiescence search
MovePicker::MovePicker(const Position&              p,
                       Move                         ttm,
                       Depth                        d,
                       const ButterflyHistory*      mh,
                       const LowPlyHistory*         lph,
                       const CapturePieceToHistory* cph,
                       const PieceToHistory**       ch,
                       const PawnHistory*           ph,
                       int                          pl) :
    pos(p),
    mainHistory(mh),
    lowPlyHistory(lph),
    captureHistory(cph),
    continuationHistory(ch),
    pawnHistory(ph),
    ttMove(ttm),
    depth(d),
    ply(pl) {

    if (pos.checkers())
        stage = EVASION_TT + !(ttm && pos.pseudo_legal(ttm));
    else
        stage = (depth > 0 ? MAIN_TT : QSEARCH_TT) + !(ttm && pos.pseudo_legal(ttm));
}

// MovePicker constructor for ProbCut: we generate captures with Static Exchange
// Evaluation (SEE) greater than or equal to the given threshold.
MovePicker::MovePicker(const Position& p, Move ttm, int th, const CapturePieceToHistory* cph) :
    pos(p),
    captureHistory(cph),
    ttMove(ttm),
    threshold(th) {
    assert(!pos.checkers());

    stage = PROBCUT_TT
          + !(ttm && pos.capture_stage(ttm) && pos.pseudo_legal(ttm) && pos.see_ge(ttm, threshold));
}

// Assigns a numerical value to each move in a list, used for sorting.
// Captures are ordered by Most Valuable Victim (MVV), preferring captures
// with a good history. Quiets moves are ordered using the history tables.
template<GenType Type>
void MovePicker::score() {

    static_assert(Type == CAPTURES || Type == QUIETS || Type == EVASIONS, "Wrong type");

    Color us = pos.side_to_move();
    //for fortress begin
    const bool fortress = MoveConfig::isFortress;

    int phase           = std::clamp(pos.game_ply() / 25, 0, 3);
    int breakingPenalty = 1800 + 600 * phase;
    int preservingBonus = 900 + 300 * (3 - phase);

    //for fotress end
    [[maybe_unused]] Bitboard threatByLesser[QUEEN + 1];
    if constexpr (Type == QUIETS)
    {
        threatByLesser[KNIGHT] = threatByLesser[BISHOP] = pos.attacks_by<PAWN>(~us);
        threatByLesser[ROOK] =
          pos.attacks_by<KNIGHT>(~us) | pos.attacks_by<BISHOP>(~us) | threatByLesser[KNIGHT];
        threatByLesser[QUEEN] = pos.attacks_by<ROOK>(~us) | threatByLesser[ROOK];
    }

    for (auto& m : *this)
    {
        const Square    from          = m.from_sq();
        const Square    to            = m.to_sq();
        const Piece     pc            = pos.moved_piece(m);
        const PieceType pt            = type_of(pc);
        const Piece     capturedPiece = pos.piece_on(to);

        if constexpr (Type == CAPTURES)
        {
            m.value = (*captureHistory)[pc][to][type_of(capturedPiece)]
                    + 7 * int(PieceValue[capturedPiece]) + 1024 * bool(pos.check_squares(pt) & to);

            //for fortress begin
            if (fortress && MoveConfig::isAggressive)
                m.value += 500;
            //for fortress end
        }
        else if constexpr (Type == QUIETS)
        {
            // histories
            m.value = 2 * (*mainHistory)[us][m.from_to()];
            m.value += 2 * (*pawnHistory)[pawn_structure_index(pos)][pc][to];
            m.value += (*continuationHistory[0])[pc][to];
            m.value += (*continuationHistory[1])[pc][to];
            m.value += (*continuationHistory[2])[pc][to];
            m.value += (*continuationHistory[3])[pc][to];
            m.value += (*continuationHistory[5])[pc][to];

            // bonus for checks
            m.value += (bool(pos.check_squares(pt) & to) && pos.see_ge(m, -75)) * 16384;

            // penalty for moving to a square threatened by a lesser piece
            // or bonus for escaping an attack by a lesser piece.
            if (KNIGHT <= pt && pt <= QUEEN)
            {
                static constexpr int bonus[QUEEN + 1] = {0, 0, 144, 144, 256, 517};
                int v = threatByLesser[pt] & to ? -95 : 100 * bool(threatByLesser[pt] & from);
                m.value += bonus[pt] * v;
            }

            if (ply < LOW_PLY_HISTORY_SIZE)
                m.value += 8 * (*lowPlyHistory)[ply][m.from_to()] / (1 + ply);
            //for fortress begin
            if (fortress && MoveConfig::isStrategical)
                m.value += 200;
            //for fortress end
        }
        else
        {  // EVASIONS
            if (pos.capture_stage(m))
            {
                m.value = PieceValue[pos.piece_on(to)] + (1 << 28);
            }
            else
            {
                m.value = (*mainHistory)[us][m.from_to()] + (*continuationHistory[0])[pc][to];
                if (ply < LOW_PLY_HISTORY_SIZE)
                    m.value += 2 * (*lowPlyHistory)[ply][m.from_to()] / (1 + ply);
            }
        }
        //for fortress begin
        if (fortress)
        {
            bool preserve = is_fortress_preserving_move(pos, m);
            if (is_fortress_breaking_move(pos, m))
            {
                // Penalità maggiore per mosse che rompono la fortezza
                m.value -= breakingPenalty * 1.5;

                // Penalità extra per pezzi chiave
                if (is_fortress_key_piece(pc))
                    m.value -= 600;
            }
            else if (preserve)
            {
                // Bonus maggiore per mosse che mantengono la fortezza
                m.value += preservingBonus * 1.8;

                // Bonus speciale per mosse di re in fase finale
                if (type_of(pc) == KING && phase == 3)
                    m.value += 300;

                // Bonus aggiuntivo per mosse di pedone che rafforzano la struttura
                if (type_of(pc) == PAWN)
                {
                    // Bonus per pedoni connessi (corretto con attacks_bb)
                    Bitboard adjacentPawns = attacks_bb<PAWN>(to, us) & pos.pieces(us, PAWN);
                    if (adjacentPawns)
                        m.value += 200;

                    // Bonus per pedoni avanzati
                    Rank r          = rank_of(to);
                    bool isAdvanced = (us == WHITE) ? (r >= RANK_5) : (r <= RANK_4);
                    if (isAdvanced)
                        m.value += 150;
                }
            }

            // Gestione regola delle 50 mosse
            if (no_progress_for(pos, 15) && preserve)
            {
                m.value += 300;
            }

            // Logica Shashin dinamica
            if (MoveConfig::useMoveShashinLogic)
            {
                double plyFactor = std::min(pos.game_ply() / 40.0, 1.0);

                if (MoveConfig::isStrategical)
                    m.value += static_cast<int>(100 * plyFactor);
                else if (MoveConfig::isAggressive)
                    m.value += static_cast<int>(180 * plyFactor);
            }
        }
        //fortress end
    }
}


// Returns the next move satisfying a predicate function.
// This never returns the TT move, as it was emitted before.
template<typename Pred>
Move MovePicker::select(Pred filter) {

    for (; cur < endCur; ++cur)
        if (*cur != ttMove && filter())
            return *cur++;

    return Move::none();
}

// This is the most important method of the MovePicker class. We emit one
// new pseudo-legal move on every call until there are no more moves left,
// picking the move with the highest score from a list of generated moves.
Move MovePicker::next_move() {

    constexpr int goodQuietThreshold = -14000;
top:
    switch (stage)
    {

    case MAIN_TT :
    case EVASION_TT :
    case QSEARCH_TT :
    case PROBCUT_TT :
        ++stage;
        return ttMove;

    case CAPTURE_INIT :
    case PROBCUT_INIT :
    case QCAPTURE_INIT :
        cur = endBadCaptures = moves;
        endCur = endCaptures = generate<CAPTURES>(pos, cur);

        score<CAPTURES>();
        partial_insertion_sort(cur, endCur, std::numeric_limits<int>::min());
        ++stage;
        goto top;

    case GOOD_CAPTURE :
        if (select([&]() {
                if (pos.see_ge(*cur, -cur->value / 18))
                    return true;
                std::swap(*endBadCaptures++, *cur);
                return false;
            }))
            return *(cur - 1);

        ++stage;
        [[fallthrough]];

    case QUIET_INIT :
        if (!skipQuiets)
        {
            endCur = endGenerated = generate<QUIETS>(pos, cur);

            score<QUIETS>();
            partial_insertion_sort(cur, endCur, -3560 * depth);
        }

        ++stage;
        [[fallthrough]];

    case GOOD_QUIET :
        if (!skipQuiets && select([&]() { return cur->value > goodQuietThreshold; }))
            return *(cur - 1);

        // Prepare the pointers to loop over the bad captures
        cur    = moves;
        endCur = endBadCaptures;

        ++stage;
        [[fallthrough]];

    case BAD_CAPTURE :
        if (select([]() { return true; }))
            return *(cur - 1);

        // Prepare the pointers to loop over quiets again
        cur    = endCaptures;
        endCur = endGenerated;

        ++stage;
        [[fallthrough]];

    case BAD_QUIET :
        if (!skipQuiets)
            return select([&]() { return cur->value <= goodQuietThreshold; });

        return Move::none();

    case EVASION_INIT :
        cur    = moves;
        endCur = endGenerated = generate<EVASIONS>(pos, cur);

        score<EVASIONS>();
        partial_insertion_sort(cur, endCur, std::numeric_limits<int>::min());
        ++stage;
        [[fallthrough]];

    case EVASION :
    case QCAPTURE :
        return select([]() { return true; });

    case PROBCUT :
        return select([&]() { return pos.see_ge(*cur, threshold); });
    }

    assert(false);
    return Move::none();  // Silence warning
}

void MovePicker::skip_quiet_moves() { skipQuiets = true; }

// this function must be called after all quiet moves and captures have been generated
bool MovePicker::can_move_king_or_pawn() const {
    // SEE negative captures shouldn't be returned in GOOD_CAPTURE stage
    assert(stage > GOOD_CAPTURE && stage != EVASION_INIT);

    for (const ExtMove* m = moves; m < endGenerated; ++m)
    {
        PieceType movedPieceType = type_of(pos.moved_piece(*m));
        if ((movedPieceType == PAWN || movedPieceType == KING) && pos.legal(*m))
            return true;
    }
    return false;
}

}  // namespace ShashChess