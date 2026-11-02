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

#ifndef SHASHIN_POSITION_H
#define SHASHIN_POSITION_H

#include "../position.h"
#include "shashin_params.h"
namespace ShashChess {
namespace Shashin {
[[nodiscard]]
inline uint64_t shashin_position_hash(const Position& pos) noexcept {
    return pos.key() ^ pos.material_key();
}
// [SC:shashin] begin
[[nodiscard]]
// shashin_position.h
[[nodiscard]]
inline int count_safe_waiting_moves(const Position& pos) {
    int             count = 0;
    MoveList<LEGAL> moves(pos);

    for (const auto& m : moves)
    {
        if (!pos.capture_stage(m) && !pos.gives_check(m))
        {
            const Square to         = m.to_sq();
            Piece        moved      = pos.moved_piece(m);
            Value        movedValue = PieceValue[type_of(moved)];

            // Gestione promozioni
            if (m.type_of() == PROMOTION)
                movedValue = PieceValue[m.promotion_type()];  // Sovrascrive il valore del pedone

            Bitboard attackers           = pos.attackers_to(to, pos.pieces() ^ m.from_sq())
                                         & pos.pieces(~pos.side_to_move());  // [SC fix]
            Value    minOpponentAttacker = VALUE_INFINITE;

            // Trova il pezzo attaccante avversario di valore minimo
            while (attackers)
            {
                Square s            = pop_lsb(attackers);
                Piece  attacker     = pos.piece_on(s);
                minOpponentAttacker = std::min(minOpponentAttacker, PieceValue[type_of(attacker)]);
            }

            // Condizioni di sicurezza:
            const bool isSafe =
              (minOpponentAttacker > movedValue) ||        // Nessun attacco redditizio
              (!attackers && m.type_of() != PROMOTION) ||  // Mossa normale non attaccata
              (m.type_of() == PROMOTION && !attackers);    // Promozione non attaccata

            if (isSafe)
                count++;
        }
    }
    return count;
}

[[nodiscard]]
inline bool king_danger(const Position& pos, Color c) noexcept {
    // [SC] Enemy pieces attacking the king zone (king square and adjacent squares).
    // Counting only the attackers of the king square meant counting the checkers: the
    // weighted sum could exceed the threshold only with a double check by two heavy
    // pieces, so kingDanger was never true (0% of suite and game positions). With the
    // king zone it is true in about 13% of the suite positions and 6% of game positions.
    const Square ksq  = pos.square<KING>(c);
    Bitboard     zone = Attacks::attacks_bb<KING>(ksq) | ksq;
    Bitboard     attackers = 0;
    while (zone)
        attackers |= pos.attackers_to(pop_lsb(zone));
    attackers &= pos.pieces(~c) & ~pos.pieces(KING);
    if (!attackers)
        return false;
    constexpr int STRONG_WEIGHT    = 3;
    constexpr int WEAK_WEIGHT      = 1;
    constexpr int DANGER_THRESHOLD = 5;
    int           sum              = 0;
    Bitboard      tmp              = attackers;
    while (tmp)
    {
        Square    s  = pop_lsb(tmp);
        Piece     p  = pos.piece_on(s);
        PieceType pt = type_of(p);
        sum += (pt == QUEEN || pt == ROOK) ? STRONG_WEIGHT : WEAK_WEIGHT;
        if (sum > DANGER_THRESHOLD)
            return true;
    }
    return false;
}


[[nodiscard]]
inline int king_safety_score(const Position& pos, Color c) noexcept {
    const Square   ksq           = pos.square<KING>(c);
    const Bitboard attackers     = pos.attackers_to(ksq) & pos.pieces(~c);
    const int      attackerCount = popcount(attackers);

    // Caso senza attaccanti: rapido controllo shield pedonale
    if (attackerCount == 0)
    {
        const Bitboard ourPawns = pos.pieces(c, PAWN);
        const Bitboard pawnShield0 =
          ((c == WHITE ? (shift(ourPawns, NORTH_WEST) | shift(ourPawns, NORTH_EAST)) : (shift(ourPawns, SOUTH_WEST) | shift(ourPawns, SOUTH_EAST)))
           & ksq);
        if (popcount(pawnShield0) >= 3)
            return 100;
    }

    // Calcolo del pawn shield sui file adiacenti
    const File     kf            = file_of(ksq);
    const Bitboard adjacentFiles = shift(file_bb(kf), EAST) | shift(file_bb(kf), WEST);
    const Bitboard pawnShield    = pos.pieces(c, PAWN) & (adjacentFiles | file_bb(kf));

    // Early-exit su shield pedonale
    int      shieldCount = 0;
    Bitboard tmp         = pawnShield;
    while (tmp && shieldCount < 3)
    {
        pop_lsb(tmp);
        ++shieldCount;
    }

    // Centralizzazione calcolata in modo puro e universale
    const Square centerSq       = make_square(FILE_E, c == WHITE ? RANK_4 : RANK_5);
    const int    centralization = 14 - std::max(std::abs(file_of(ksq) - file_of(centerSq)), std::abs(rank_of(ksq) - rank_of(centerSq))) * 2;

    // Costanti per il punteggio
    constexpr int ATTACKER_PENALTY = 12;
    constexpr int SHIELD_BONUS     = 15;
    constexpr int CENTER_BONUS     = 8;

    int score =
      shieldCount * SHIELD_BONUS - attackerCount * ATTACKER_PENALTY + centralization * CENTER_BONUS;

    return std::clamp(score, -150, 100);
}

[[nodiscard]]
inline bool is_open_file(const Position& pos, Square s) noexcept {
    const Bitboard fileBB = file_bb(file_of(s));
    return (pos.pieces(PAWN) & fileBB) == 0;
}

[[nodiscard]]
inline Bitboard attacking_pieces(const Position& pos, Color c, Square s) {
    Bitboard atks = pos.attackers_to(s);
    return atks ? (atks & pos.pieces(c)) : 0ULL;
}
[[nodiscard]]
inline bool is_ok_piece(Piece p) {
    return p >= W_PAWN && p <= B_KING;
}
[[nodiscard]]
inline bool is_sacrifice(const Position& pos, Move m) noexcept {
    const Square from = m.from_sq();
    const Square to   = m.to_sq();

    const Piece moved    = pos.piece_on(from);
    const Piece captured = pos.piece_on(to);

    if (type_of(moved) == KING || captured == NO_PIECE)
        return false;
    Value moveValue = PieceValue[type_of(moved)];
    if (m.type_of() == PROMOTION)
    {
        moveValue += PieceValue[m.promotion_type()] - PieceValue[PAWN];
    }

    if (moveValue <= PieceValue[type_of(captured)])
        return false;

    // [SC] A real sacrifice against the enemy king: the exchange loses material (SEE < 0)
    // and the capture gives check or lands in the enemy king zone. Any capture by a more
    // valuable piece, even of an undefended pawn, made 79% of the game positions
    // "sacrificial" (85% of the suite); now 27% of the game positions and 45% of the suite.
    if (pos.see_ge(m, 0))
        return false;

    const Square theirKing = pos.square<KING>(~pos.side_to_move());
    return pos.gives_check(m) || ((Attacks::attacks_bb<KING>(theirKing) | theirKing) & to);
}

// [SC:shashin] begin
[[nodiscard]]
inline Bitboard pawn_attacks(Color c, Square s) {
    return Attacks::attacks_bb<PAWN>(s, c);
}
[[nodiscard]]
inline Bitboard king_ring(Square ksq) {
    return Attacks::attacks_bb<KING>(ksq) | ksq;
}
[[nodiscard]]
inline bool has_pawn_breaks(const Position& pos) noexcept {
    const Color    us            = pos.side_to_move();
    const Bitboard pawns         = pos.pieces(us, PAWN);
    const Bitboard eligiblePawns = pawns & ~(us == WHITE ? Rank7BB : Rank2BB);
    const Bitboard pushSquares =
      us == WHITE ? shift(eligiblePawns, NORTH) : shift(eligiblePawns, SOUTH);
    return (pushSquares & ~pos.pieces()) != 0;
}
[[nodiscard]]
inline bool is_king_cutoff(const Position& pos, Color strongSide) {
    Square kingSq = pos.square<KING>(strongSide);
    return (pos.pieces(strongSide, PAWN) & king_ring(kingSq))
        || (pos.blockers_for_king(~strongSide)
            & (rank_bb(rank_of(kingSq)) | file_bb(file_of(kingSq))));
}
[[nodiscard]]
inline bool has_safe_waiting_moves(const Position& pos) {
    MoveList<LEGAL> moves(pos);
    for (const auto& m : moves)
    {
        if (!pos.capture_stage(m) && !pos.gives_check(m))
        {
            const Square to         = m.to_sq();
            Piece        moved      = pos.moved_piece(m);
            Value        movedValue = PieceValue[type_of(moved)];

            // Gestione promozioni
            if (m.type_of() == PROMOTION)
                movedValue = PieceValue[m.promotion_type()]; 

            Bitboard attackers           = pos.attackers_to(to, pos.pieces() ^ m.from_sq())
                                         & pos.pieces(~pos.side_to_move());  // [SC fix]
            Value    minOpponentAttacker = VALUE_INFINITE;

            // Trova il pezzo attaccante avversario di valore minimo
            while (attackers)
            {
                Square s            = pop_lsb(attackers);
                Piece  attacker     = pos.piece_on(s);
                minOpponentAttacker = std::min(minOpponentAttacker, PieceValue[type_of(attacker)]);
            }

            // Condizioni di sicurezza aggiornate:
            const bool isSafe = (minOpponentAttacker > movedValue) ||  
                                (!attackers && m.type_of() != PROMOTION)
                             || 
                                (m.type_of() == PROMOTION
                                 && !attackers); 

            if (isSafe)
                return true;
        }
    }
    return false;
}

[[nodiscard]]
inline bool is_king_nearby(const Position& pos, Square sq) {
    Square kingSq = pos.square<KING>(pos.side_to_move());
    return std::max(std::abs(file_of(kingSq) - file_of(sq)), std::abs(rank_of(kingSq) - rank_of(sq))) <= 2;
}
[[nodiscard]]
inline bool is_pawn_weakening_move(const Position& pos, Square from, Square to) {
    Bitboard pawnShield =
      pawn_attacks(pos.side_to_move(), from) & pos.pieces(pos.side_to_move(), PAWN);
    return pawnShield
        && !(pawn_attacks(pos.side_to_move(), to) & pos.pieces(pos.side_to_move(), PAWN));
}
[[nodiscard]]
inline bool is_fortress_key_piece(Piece p) noexcept {
    if (p == NO_PIECE)
        return false;
    const PieceType pt = type_of(p);
    return pt == ROOK || pt == KNIGHT || pt == BISHOP;
}
[[nodiscard]]
inline bool is_fortress_breaking_move(const Position& pos, const Move& m) {
    Square from = m.from_sq(), to = m.to_sq();
    Piece  moved = pos.piece_on(from), captured = pos.piece_on(to);
    return (type_of(moved) == KING && !is_king_nearby(pos, to))
        || (type_of(moved) == PAWN && is_pawn_weakening_move(pos, from, to))
        || (captured != NO_PIECE && is_fortress_key_piece(captured));
}
[[nodiscard]]
inline bool is_inside_fortress(const Position& pos, Square sq) noexcept {
    const Color  us           = pos.side_to_move();
    const Square ksq          = pos.square<KING>(us);
    Bitboard     kingArea     = Attacks::attacks_bb<KING>(ksq) | ksq;
    Bitboard     fortressZone = kingArea | shift(kingArea, NORTH) | shift(kingArea, SOUTH);
    Bitboard     ourPawns     = pos.pieces(us, PAWN);
    Bitboard     pawnAttacks =
      (us == WHITE) ? (shift(ourPawns, NORTH_WEST) | shift(ourPawns, NORTH_EAST)) : (shift(ourPawns, SOUTH_WEST) | shift(ourPawns, SOUTH_EAST));
    Bitboard pawnShield      = ourPawns & (pawnAttacks | kingArea);
    Bitboard pawnStorm       = pos.pieces(~us, PAWN) & fortressZone;
    bool     hasPawnShield   = popcount(pawnShield & fortressZone) >= 3;
    bool     noEnemyPawns    = !pawnStorm;
    bool     kingProximity   = std::max(std::abs(file_of(sq) - file_of(ksq)), std::abs(rank_of(sq) - rank_of(ksq))) <= 2;
    Bitboard keyDefenders    = pos.pieces(us, ROOK, BISHOP) & fortressZone;
    bool     hasKeyDefenders = popcount(keyDefenders) >= 1;

    return (hasPawnShield && noEnemyPawns && kingProximity) && (sq & (fortressZone | keyDefenders))
        && hasKeyDefenders;
}
[[nodiscard]]
inline bool is_fortress_preserving_move(const Position& pos, const Move& m) {
    return (type_of(pos.piece_on(m.from_sq())) == KING && is_king_nearby(pos, m.to_sq()))
        || is_inside_fortress(pos, m.to_sq());
}
[[nodiscard]]
inline bool no_progress_for(const Position& pos, int moves) {
    return pos.rule50_count() >= moves;
}
// [SC:shashin] end

}  // namespace Shashin
}  // namespace ShashChess

#endif  // SHASHIN_POSITION_H
