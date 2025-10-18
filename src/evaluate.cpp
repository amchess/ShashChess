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

#include "evaluate.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <tuple>

#include "nnue/network.h"
#include "nnue/nnue_misc.h"
#include "position.h"
#include "types.h"
#include "uci.h"
#include "nnue/nnue_accumulator.h"

namespace ShashChess {

// Returns a static, purely materialistic evaluation of the position from
// the point of view of the side to move. It can be divided by PawnValue to get
// an approximation of the material advantage on the board in terms of pawns.
int Eval::simple_eval(const Position& pos) {
    Color c = pos.side_to_move();
    return PawnValue * (pos.count<PAWN>(c) - pos.count<PAWN>(~c))
         + (pos.non_pawn_material(c) - pos.non_pawn_material(~c));
}

bool Eval::use_smallnet(const Position& pos) { return std::abs(simple_eval(pos)) > 962; }

// Evaluate is the evaluator for the outer world. It returns a static evaluation
// of the position from the point of view of the side to move.
Value Eval::evaluate(const Eval::NNUE::Networks&    networks,
                     const Position&                pos,
                     Eval::NNUE::AccumulatorStack&  accumulators,
                     Eval::NNUE::AccumulatorCaches& caches,
                     int                            optimism) {

    assert(!pos.checkers());

    bool smallNet           = use_smallnet(pos);
    auto [psqt, positional] = smallNet ? networks.small.evaluate(pos, accumulators, &caches.small)
                                       : networks.big.evaluate(pos, accumulators, &caches.big);

    Value nnue = (125 * psqt + 131 * positional) / 128;

    // Re-evaluate the position when higher eval accuracy is worth the time spent
    if (smallNet && (std::abs(nnue) < 236))
    {
        std::tie(psqt, positional) = networks.big.evaluate(pos, accumulators, &caches.big);
        nnue                       = (125 * psqt + 131 * positional) / 128;
        smallNet                   = false;
    }

    // Blend optimism and eval with nnue complexity
    int nnueComplexity = std::abs(psqt - positional);
    optimism += optimism * nnueComplexity / 468;
    nnue -= nnue * nnueComplexity / 18000;

    int material = 535 * pos.count<PAWN>() + pos.non_pawn_material();
    int v        = (nnue * (77777 + material) + optimism * (7777 + material)) / 77777;

    // Damp down the evaluation linearly when shuffling
    v -= v * pos.rule50_count() / 212;

    // Guarantee evaluation does not hit the tablebase range
    v = std::clamp(v, VALUE_TB_LOSS_IN_MAX_PLY + 1, VALUE_TB_WIN_IN_MAX_PLY - 1);

    return v;
}

namespace {

// Funzione per determinare la zona di Shashin basata sulla win probability
std::string get_shashin_zone(uint8_t winProb, Color sideToMove) {
    if (sideToMove == WHITE)
    {
        if (winProb <= 5)
            return "High Petrosian (Losing position)";
        else if (winProb <= 10)
            return "High-Middle Petrosian (Decisive disadvantage)";
        else if (winProb <= 15)
            return "Middle Petrosian (Clear disadvantage)";
        else if (winProb <= 20)
            return "Middle-Low Petrosian (Significant disadvantage)";
        else if (winProb <= 24)
            return "Low Petrosian (Slight disadvantage)";
        else if (winProb <= 49)
            return "Chaos: Capablanca-Petrosian (Balanced with opponent pressure)";
        else if (winProb == 50)
            return "Capablanca (Equal position)";
        else if (winProb <= 75)
            return "Chaos: Capablanca-Tal (Initiative)";
        else if (winProb <= 79)
            return "Low Tal (Slight advantage)";
        else if (winProb <= 84)
            return "Middle-Low Tal (Growing advantage)";
        else if (winProb <= 89)
            return "Middle Tal (Clear advantage)";
        else if (winProb <= 94)
            return "High-Middle Tal (Dominant position)";
        else
            return "High Tal (Winning position)";
    }
    else
    {
        // Per il nero, invertiamo le probabilità
        uint8_t blackWinProb = 100 - winProb;
        if (blackWinProb <= 5)
            return "High Petrosian (Losing position)";
        else if (blackWinProb <= 10)
            return "High-Middle Petrosian (Decisive disadvantage)";
        else if (blackWinProb <= 15)
            return "Middle Petrosian (Clear disadvantage)";
        else if (blackWinProb <= 20)
            return "Middle-Low Petrosian (Significant disadvantage)";
        else if (blackWinProb <= 24)
            return "Low Petrosian (Slight disadvantage)";
        else if (blackWinProb <= 49)
            return "Chaos: Capablanca-Petrosian (Balanced with opponent pressure)";
        else if (blackWinProb == 50)
            return "Capablanca (Equal position)";
        else if (blackWinProb <= 75)
            return "Chaos: Capablanca-Tal (Initiative)";
        else if (blackWinProb <= 79)
            return "Low Tal (Slight advantage)";
        else if (blackWinProb <= 84)
            return "Middle-Low Tal (Growing advantage)";
        else if (blackWinProb <= 89)
            return "Middle Tal (Clear advantage)";
        else if (blackWinProb <= 94)
            return "High-Middle Tal (Dominant position)";
        else
            return "High Tal (Winning position)";
    }
}

// Versione orizzontale dell'analisi dell'attività dei pezzi
std::string analyze_piece_activity_horizontal(const Position& pos) {
    std::vector<std::pair<std::string, int>> whitePieces, blackPieces;

    // Analizza i pezzi bianchi
    Bitboard whiteBB = pos.pieces(WHITE);
    while (whiteBB)
    {
        Square    sq = pop_lsb(whiteBB);
        Piece     pc = pos.piece_on(sq);
        PieceType pt = type_of(pc);
        if (pt == KING)
            continue;

        std::string pieceName;
        switch (pt)
        {
        case PAWN :
            pieceName = "P";
            break;
        case KNIGHT :
            pieceName = "N";
            break;
        case BISHOP :
            pieceName = "B";
            break;
        case ROOK :
            pieceName = "R";
            break;
        case QUEEN :
            pieceName = "Q";
            break;
        default :
            continue;
        }

        // Calcola attività
        Bitboard attacks  = attacks_bb(pt, sq, pos.pieces());
        int      activity = popcount(attacks) * 2;

        constexpr Bitboard Center = (FileDBB | FileEBB) & (Rank4BB | Rank5BB);
        if (attacks & Center)
            activity += 5;

        Square   enemyKingSq = pos.square<KING>(BLACK);
        Bitboard kingZone    = attacks_bb<KING>(enemyKingSq) | enemyKingSq;
        if (attacks & kingZone)
            activity += 8;

        if (pt != PAWN)
        {
            if (color_of(pc) == WHITE && rank_of(sq) > RANK_2)
                activity += 3;
        }

        whitePieces.emplace_back(pieceName + UCIEngine::square(sq), activity);
    }

    // Analizza i pezzi neri
    Bitboard blackBB = pos.pieces(BLACK);
    while (blackBB)
    {
        Square    sq = pop_lsb(blackBB);
        Piece     pc = pos.piece_on(sq);
        PieceType pt = type_of(pc);
        if (pt == KING)
            continue;

        std::string pieceName;
        switch (pt)
        {
        case PAWN :
            pieceName = "P";
            break;
        case KNIGHT :
            pieceName = "N";
            break;
        case BISHOP :
            pieceName = "B";
            break;
        case ROOK :
            pieceName = "R";
            break;
        case QUEEN :
            pieceName = "Q";
            break;
        default :
            continue;
        }

        Bitboard attacks  = attacks_bb(pt, sq, pos.pieces());
        int      activity = popcount(attacks) * 2;

        constexpr Bitboard Center = (FileDBB | FileEBB) & (Rank4BB | Rank5BB);
        if (attacks & Center)
            activity += 5;

        Square   enemyKingSq = pos.square<KING>(WHITE);
        Bitboard kingZone    = attacks_bb<KING>(enemyKingSq) | enemyKingSq;
        if (attacks & kingZone)
            activity += 8;

        if (pt != PAWN)
        {
            if (color_of(pc) == BLACK && rank_of(sq) < RANK_7)
                activity += 3;
        }

        blackPieces.emplace_back(pieceName + UCIEngine::square(sq), activity);
    }

    // Ordina per attività (dalla meno attiva alla più attiva)
    std::sort(whitePieces.begin(), whitePieces.end(),
              [](const auto& a, const auto& b) { return a.second < b.second; });
    std::sort(blackPieces.begin(), blackPieces.end(),
              [](const auto& a, const auto& b) { return a.second < b.second; });

    std::stringstream ss;
    ss << "White pieces (worst to best static activity): ";
    for (size_t i = 0; i < whitePieces.size(); ++i)
    {
        if (i > 0)
            ss << ", ";
        ss << whitePieces[i].first << "(" << whitePieces[i].second << ")";
        if (i == 0)
            ss << " <-- Worst unit (Makogonov)";
    }
    ss << "\n";

    ss << "Black pieces (worst to best static activity): ";
    for (size_t i = 0; i < blackPieces.size(); ++i)
    {
        if (i > 0)
            ss << ", ";
        ss << blackPieces[i].first << "(" << blackPieces[i].second << ")";
        if (i == 0)
            ss << " <-- Worst unit (Makogonov)";
    }
    ss << "\n";

    return ss.str();
}

// Funzione per analizzare le mosse legali ordinate per attività
std::string analyze_legal_moves(const Position& pos, const Eval::NNUE::Networks& networks) {
    std::vector<std::tuple<std::string, Value, uint8_t>> moves;  // (move, eval, win_prob)

    Color side_to_move = pos.side_to_move();

    // Genera tutte le mosse legali
    for (const auto& move : MoveList<LEGAL>(pos))
    {
        // Usa una copia della posizione invece di copiare
        StateListPtr states(new std::deque<StateInfo>(1));
        Position     posCopy;
        posCopy.set(pos.fen(), pos.is_chess960(), &states->back());

        // Esegui la mossa
        StateInfo st;
        posCopy.do_move(move, st);

        // Crea un nuovo contesto di valutazione per questa mossa
        Eval::NNUE::AccumulatorStack acc_stack;
        auto caches_new = std::make_unique<Eval::NNUE::AccumulatorCaches>(networks);

        // Valuta la posizione dopo la mossa
        Value v = Eval::evaluate(networks, posCopy, acc_stack, *caches_new, VALUE_ZERO);

        // La valutazione v è dal punto di vista del giocatore che deve muovere nella nuova posizione
        // Dopo la mossa, il turno cambia, quindi v è dal punto di vista dell'avversario
        // Per ottenere la valutazione dal punto di vista del giocatore che ha fatto la mossa, invertiamo
        Value eval_for_mover = -v;

        // Converti al punto di vista del bianco per calcolare la win probability
        Value v_white = posCopy.side_to_move() == WHITE ? v : -v;

        // Calcola la win probability dal punto di vista del bianco
        uint8_t win_prob_white = WDLModel::get_win_probability(v_white, posCopy);

        // Per il giocatore che muove, convertiamo la win probability
        uint8_t win_prob_for_mover;
        if (side_to_move == WHITE)
        {
            win_prob_for_mover = win_prob_white;
        }
        else
        {
            // Se il nero muove, la win probability per il nero è 100 - win_prob_white
            win_prob_for_mover = 100 - win_prob_white;
        }

        moves.emplace_back(UCIEngine::move(move, pos.is_chess960()), eval_for_mover,
                           win_prob_for_mover);
    }

    // Ordina le mosse per win probability decrescente (migliori prime)
    // A parità di win probability, ordina per valutazione in centipawn decrescente
    std::sort(moves.begin(), moves.end(), [](const auto& a, const auto& b) {
        if (std::get<2>(a) != std::get<2>(b))
            return std::get<2>(a) > std::get<2>(b);  // win probability più alta prima
        return std::get<1>(a) > std::get<1>(b);      // a parità, eval più alta prima
    });

    std::stringstream ss;
    ss << "Legal moves ordered by static activity (best first): ";
    for (size_t i = 0; i < moves.size(); ++i)
    {
        if (i > 0)
            ss << ", ";

        const auto& move_str = std::get<0>(moves[i]);
        Value       eval     = std::get<1>(moves[i]);
        uint8_t     win_prob = std::get<2>(moves[i]);

        ss << move_str << "(" << std::showpos << std::fixed << std::setprecision(1)
           << 0.01 * UCIEngine::to_cp(eval, pos) << " cp, " << static_cast<int>(win_prob) << "%)";
    }

    return ss.str();
}

}  // namespace

// Like evaluate(), but instead of returning a value, it returns
// a string (suitable for outputting to stdout) that contains the detailed
// descriptions and values of each evaluation term. Useful for debugging.
// Trace scores are from white's point of view
std::string Eval::trace(Position& pos, const Eval::NNUE::Networks& networks, bool showWDL) {

    if (pos.checkers())
        return "Final evaluation: none (in check)";

    Eval::NNUE::AccumulatorStack accumulators;
    auto                         caches = std::make_unique<Eval::NNUE::AccumulatorCaches>(networks);

    std::stringstream ss;

    // Inizializza il modello WDL se necessario
    if (!WDLModel::is_initialized())
    {
        WDLModel::init();
    }

    // Calcola la valutazione finale
    Value v = evaluate(networks, pos, accumulators, *caches, VALUE_ZERO);

    // Converti SEMPRE al punto di vista del bianco
    Value v_white = pos.side_to_move() == WHITE ? v : -v;

    if (showWDL)
    {
        // Calcola win probability e WDL DAL PUNTO DI VISTA DEL BIANCO
        uint8_t       winProbWhite = WDLModel::get_win_probability(v_white, pos);
        WDLModel::WDL wdl          = WDLModel::get_wdl(v_white, pos);

        // Determina la zona di Shashin per il bianco
        std::string shashinZone = get_shashin_zone(winProbWhite, WHITE);

        // Mostra la trace NNUE in centipawn
        ss << std::showpoint << std::noshowpos << std::fixed << std::setprecision(2);
        ss << '\n' << NNUE::trace(pos, networks, *caches) << '\n';

        ss << "=== SHASHIN STATIC ANALYSIS ===\n";
        ss << "*** Static activity evaluation based on NNUE network ***\n";
        ss << "*** All NNUE values shown in centipawn for technical accuracy ***\n";
        ss << "*** Win probability and Shashin zone always from White's perspective ***\n\n";

        // Analisi dell'attività dei pezzi in orizzontale
        ss << "--- PIECE ACTIVITY ANALYSIS (Makogonov Principle) ---\n";
        ss << analyze_piece_activity_horizontal(pos) << "\n";

        // Analisi delle mosse legali
        ss << "--- LEGAL MOVES ANALYSIS ---\n";
        ss << analyze_legal_moves(pos, networks) << "\n";

        ss << "================================\n";
        ss << "Final Static Activity Evaluation: " << 0.01 * UCIEngine::to_cp(v_white, pos)
           << " cp | " << static_cast<int>(winProbWhite) << "% win probability (White)\n";
        ss << "WDL (Win/Draw/Loss): " << static_cast<int>(wdl.win) << "%/"
           << static_cast<int>(wdl.draw) << "%/" << static_cast<int>(wdl.loss) << "%\n";
        ss << "Shashin Zone: " << shashinZone << "\n";
        ss << "*** Note: Static activity analysis based on current piece placement ***\n";
    }
    else
    {
        // Comportamento tradizionale
        ss << std::showpoint << std::noshowpos << std::fixed << std::setprecision(2);
        ss << '\n' << NNUE::trace(pos, networks, *caches) << '\n';
        ss << std::showpoint << std::showpos << std::fixed << std::setprecision(2) << std::setw(15);

        auto [psqt, positional] = networks.big.evaluate(pos, accumulators, &caches->big);
        Value v_nnue            = psqt + positional;
        v_nnue                  = pos.side_to_move() == WHITE ? v_nnue : -v_nnue;
        ss << "NNUE evaluation        " << 0.01 * UCIEngine::to_cp(v_nnue, pos)
           << " (white side)\n";

        ss << "Final evaluation       " << 0.01 * UCIEngine::to_cp(v_white, pos) << " (white side)";
        ss << " [with scaled NNUE, ...]";
        ss << "\n";
    }

    return ss.str();
}

}  // namespace ShashChess