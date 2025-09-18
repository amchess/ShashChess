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
//from shashin begin
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

            Bitboard attackers           = pos.attackers_to(to, ~pos.side_to_move());
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
    const Square   ksq       = pos.square<KING>(c);
    const Bitboard attackers = pos.attackers_to(ksq) & pos.pieces(~c);
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
          ((c == WHITE ? pawn_attacks_bb<WHITE>(ourPawns) : pawn_attacks_bb<BLACK>(ourPawns))
           & ksq);
        if (popcount(pawnShield0) >= 3)
            return 100;
    }

    // Calcolo del pawn shield sui file adiacenti
    const File     kf            = file_of(ksq);
    const Bitboard adjacentFiles = shift<EAST>(file_bb(kf)) | shift<WEST>(file_bb(kf));
    const Bitboard pawnShield    = pos.pieces(c, PAWN) & (adjacentFiles | file_bb(kf));

    // Early-exit su shield pedonale
    int      shieldCount = 0;
    Bitboard tmp         = pawnShield;
    while (tmp && shieldCount < 3)
    {
        pop_lsb(tmp);
        ++shieldCount;
    }

    // Centralizzazione
    const Square centerSq       = make_square(FILE_E, c == WHITE ? RANK_4 : RANK_5);
    const int    centralization = 14 - distance(ksq, centerSq) * 2;

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
    {  // Usa type_of() invece di type()
        moveValue += PieceValue[m.promotion_type()] - PieceValue[PAWN];
    }

    return moveValue > PieceValue[type_of(captured)];
}

//For fortresses by Shashin begin
[[nodiscard]]
inline Bitboard pawn_attacks(Color c, Square s) {
    return attacks_bb<PAWN>(s, c);
}
[[nodiscard]]
inline Bitboard king_ring(Square ksq) {
    return attacks_bb<KING>(ksq) | ksq;
}
[[nodiscard]]
inline bool has_pawn_breaks(const Position& pos) noexcept {
    const Color    us            = pos.side_to_move();
    const Bitboard pawns         = pos.pieces(us, PAWN);
    const Bitboard eligiblePawns = pawns & ~(us == WHITE ? Rank7BB : Rank2BB);
    const Bitboard pushSquares =
      us == WHITE ? shift<NORTH>(eligiblePawns) : shift<SOUTH>(eligiblePawns);
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
                movedValue = PieceValue[m.promotion_type()];  // Sovrascrive il valore del pedone

            Bitboard attackers           = pos.attackers_to(to, ~pos.side_to_move());
            Value    minOpponentAttacker = VALUE_INFINITE;

            // Trova il pezzo attaccante avversario di valore minimo
            while (attackers)
            {
                Square s            = pop_lsb(attackers);
                Piece  attacker     = pos.piece_on(s);
                minOpponentAttacker = std::min(minOpponentAttacker, PieceValue[type_of(attacker)]);
            }

            // Condizioni di sicurezza aggiornate:
            const bool isSafe = (minOpponentAttacker > movedValue) ||  // Nessun attacco redditizio
                                (!attackers && m.type_of() != PROMOTION)
                             ||  // Mossa normale non attaccata
                                (m.type_of() == PROMOTION
                                 && !attackers);  // Promozione non attaccata (qualsiasi tipo)

            if (isSafe)
                return true;
        }
    }
    return false;
}

[[nodiscard]]
inline bool is_king_nearby(const Position& pos, Square sq) {
    Square kingSq = pos.square<KING>(pos.side_to_move());
    return distance(kingSq, sq) <= 2;
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
    Bitboard     kingArea     = attacks_bb<KING>(ksq) | ksq;
    Bitboard     fortressZone = kingArea | shift<NORTH>(kingArea) | shift<SOUTH>(kingArea);
    Bitboard     ourPawns     = pos.pieces(us, PAWN);
    Bitboard     pawnAttacks =
      (us == WHITE) ? pawn_attacks_bb<WHITE>(ourPawns) : pawn_attacks_bb<BLACK>(ourPawns);
    Bitboard pawnShield      = ourPawns & (pawnAttacks | kingArea);
    Bitboard pawnStorm       = pos.pieces(~us, PAWN) & fortressZone;
    bool     hasPawnShield   = popcount(pawnShield & fortressZone) >= 3;
    bool     noEnemyPawns    = !pawnStorm;
    bool     kingProximity   = distance(sq, ksq) <= 2;
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
//For fortresses by Shashin end
//perhaps to do begin
//is_isolated(), is_doubled() â†’ Struttura pedonale (Capablanca/Petrosian).
//pawn_structure_score() â†’ Se vogliamo migliorare la valutazione posizionale.
//leads_to_attack() â†’ Se la selezione delle mosse tattiche non Ã¨ ancora ottimale.
//perhaps to do end
//from Shashin end

}  // namespace Shashin
}  // namespace ShashChess

#endif  // SHASHIN_POSITION_H
