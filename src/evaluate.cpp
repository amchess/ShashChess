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

#include "evaluate.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <tuple>
#include <vector>

#include "misc.h"
#include "nnue/network.h"
#include "nnue/nnue_misc.h"
#include "position.h"
#include "types.h"
#include "uci.h"
#include "nnue/nnue_accumulator.h"
#include "wdl/win_probability.h"

namespace ShashChess {

// Evaluate is the evaluator for the outer world. It returns a static evaluation
// of the position from the point of view of the side to move.
Value Eval::evaluate(const Eval::NNUE::Network&     network,
                     const Position&                pos,
                     Eval::NNUE::AccumulatorStack&  accumulators,
                     Eval::NNUE::AccumulatorCaches& caches,
                     int                            optimism) {

    assert(!pos.checkers());

    auto [psqt, positional] = network.evaluate(pos, accumulators, caches);

    Value nnue = psqt + positional;

    // Blend optimism and eval with nnue complexity
    int nnueComplexity = std::abs(psqt - positional);
    optimism += optimism * i64(nnueComplexity) / 476;
    nnue -= nnue * i64(nnueComplexity) / 18236;

    int material = 534 * pos.count<PAWN>() + pos.non_pawn_material();
    int v        = nnue + (nnue * i64(material) + optimism * i64(7675)) / 91000;

    // Damp down the evaluation linearly when shuffling
    v -= v * pos.rule50_count() / 199;

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

        // Overload corretto con Piece per evitare il crash su PAWN
        Bitboard attacks  = Attacks::attacks_bb(pc, sq, pos.pieces());
        int      activity = popcount(attacks) * 2;

        constexpr Bitboard Center = (FileDBB | FileEBB) & (Rank4BB | Rank5BB);
        if (attacks & Center)
            activity += 5;

        Square   enemyKingSq = pos.square<KING>(BLACK);
        Bitboard kingZone    = Attacks::attacks_bb<KING>(enemyKingSq) | enemyKingSq;
        if (attacks & kingZone)
            activity += 8;

        if (pt != PAWN)
        {
            if (color_of(pc) == WHITE && rank_of(sq) > RANK_2)
                activity += 3;
        }

        whitePieces.emplace_back(pieceName + UCIEngine::square(sq), activity);
    }

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

        // Overload corretto con Piece per evitare il crash su PAWN
        Bitboard attacks  = Attacks::attacks_bb(pc, sq, pos.pieces());
        int      activity = popcount(attacks) * 2;

        constexpr Bitboard Center = (FileDBB | FileEBB) & (Rank4BB | Rank5BB);
        if (attacks & Center)
            activity += 5;

        Square   enemyKingSq = pos.square<KING>(WHITE);
        Bitboard kingZone    = Attacks::attacks_bb<KING>(enemyKingSq) | enemyKingSq;
        if (attacks & kingZone)
            activity += 8;

        if (pt != PAWN)
        {
            if (color_of(pc) == BLACK && rank_of(sq) < RANK_7)
                activity += 3;
        }

        blackPieces.emplace_back(pieceName + UCIEngine::square(sq), activity);
    }

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

std::string analyze_legal_moves(const Position& pos, const Eval::NNUE::Network& network) {
    std::vector<std::tuple<std::string, Value, uint8_t>> moves;

    Color side_to_move = pos.side_to_move();

    // Allocati una sola volta: AccumulatorCaches non va riusata tra posizioni
    // diverse, ma va resettata. AccumulatorStack invece supporta reset().
    auto acc_stack = std::make_unique<Eval::NNUE::AccumulatorStack>();
    auto caches    = std::make_unique<Eval::NNUE::AccumulatorCaches>(network);

    for (const auto& move : MoveList<LEGAL>(pos))
    {
        StateListPtr states(new std::deque<StateInfo>(1));
        Position     posCopy;
        posCopy.set(pos.fen(), pos.is_chess960(), &states->back());

        StateInfo st;
        posCopy.do_move(move, st);

        // Reset dello stack e delle cache per ciascuna mossa
        acc_stack->reset();
        caches->clear(network);
        Value v;
        if (posCopy.checkers())  // Eval::evaluate() non ammette posizioni sotto scacco
        {
            auto [psqt, positional] = network.evaluate(posCopy, *acc_stack, *caches);
            v                       = psqt + positional;
        }
        else
            v = Eval::evaluate(network, posCopy, *acc_stack, *caches, VALUE_ZERO);
        Value eval_for_mover = -v;
        Value v_white        = posCopy.side_to_move() == WHITE ? v : -v;

        uint8_t win_prob_white = WDLModel::get_win_probability(v_white, posCopy);
        uint8_t win_prob_for_mover =
          (side_to_move == WHITE) ? win_prob_white : (100 - win_prob_white);

        moves.emplace_back(UCIEngine::move(move, pos.is_chess960()), eval_for_mover,
                           win_prob_for_mover);
    }

    std::sort(moves.begin(), moves.end(), [](const auto& a, const auto& b) {
        if (std::get<2>(a) != std::get<2>(b))
            return std::get<2>(a) > std::get<2>(b);
        return std::get<1>(a) > std::get<1>(b);
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

std::string generate_strategic_synthesis(uint8_t              winProbWhite,
                                         const WDLModel::WDL& wdl,
                                         const std::string&   shashinZone,
                                         Value                v_white,
                                         const Position&      pos,
                                         int                  pos_delta_act) {
    std::stringstream ss;
    const int         wp = static_cast<int>(winProbWhite);
    const int         cp = UCIEngine::to_cp(v_white, pos);
    const int         dw = static_cast<int>(wdl.win);
    const int         dd = static_cast<int>(wdl.draw);
    const int         dl = static_cast<int>(wdl.loss);

    ss << "=======================================================================\n";
    ss << "STRATEGIC SYNTHESIS — ShashHermes Ground Truth Format\n";
    ss << "=======================================================================\n";
    ss << "Evaluation  : " << (cp >= 0 ? "+" : "") << std::fixed << std::setprecision(2)
       << 0.01 * cp << " cp | WP White=" << wp << "% | WDL=" << dw << "/" << dd << "/" << dl
       << "%\n";
    ss << "Shashin Zone: " << shashinZone << "\n\n";

    ss << "Position Character:\n";
    ss << "  Drawability   : "
       << (dd > 60   ? "VERY HIGH. Fortress risk."
           : dd > 40 ? "MODERATE. Balanced."
                     : "LOW. Decisive.")
       << "\n";
    ss << "  Volatility    : "
       << (std::abs(dw - dl) > 50 ? "LOW stability."
                                  : "HIGH variance. Critical tactical calculations.")
       << "\n";

    ss << "\nStrategic Directives:\n";
    if (wp >= 65)
        ss << "  [PRESS] Improve the worst piece. Target vulnerabilities. Open active channels.\n";
    else if (wp >= 45)
        ss << "  [BALANCE] Keep piece coordination tight. Prevent tactical outposts.\n";
    else
        ss << "  [COUNTERPLAY] Imbalance the game. Complicate variations. Avoid massive trades.\n";

    ss << "\n--- SHASHHERMES TRAINING SNIPPET ---\n";
    ss << "TRACCIA SHASHCHESS: [" << wp << "% | Zone=" << shashinZone << " | WDL=" << dw << "/"
       << dd << "/" << dl << "]\n";

    ss << "\nFor Elo 1400-1999 (Beginner):\n";
    if (wp >= 60)
        ss
          << "  \"White holds the initiative. Focus on pushing piece activity and avoiding unnecessary pawn weaknesses.\"\n";
    else if (wp >= 40)
        ss
          << "  \"The board is statically balanced. Maintain proper piece protection and secure the king.\"\n";
    else
        ss
          << "  \"Black is exerting pressure. Search for dynamic counterplay and active defensive resources.\"\n";

    ss << "\nFor Elo 2000-2199 (Intermediate):\n";
    if (wp >= 60)
        ss
          << "  \"" << shashinZone << " (" << wp
          << "% WP). Active bucket detects structural advantage beyond material. Prioritize fixing the worst unit.\"\n";
    else if (wp >= 40)
        ss << "  \"" << shashinZone << " (" << wp << "% WP). High draw probability (" << dd
           << "%). Strategic play revolves around center control and outpost denial.\"\n";
    else
        ss
          << "  \"" << shashinZone << " (" << wp
          << "% WP). Structural compression detected. Complicate the topology before spatial paralysis sets in.\"\n";

    ss << "\nFor Elo 2200+ (Advanced/Expert):\n";
    ss
      << "  \"NNUE WP=" << wp << "% | " << shashinZone << " | WDL=" << dw << "/" << dd << "/" << dl
      << "%. "
      << "Neural structural correction: " << std::showpos << pos_delta_act << std::noshowpos
      << "% WP deviation from PSQT material. "
      << "Consult the NNUE Mechanistic Trace layer output for precise Shapley attributions, filter channel activations, and defensive fixation blindspots.\"\n";

    ss << "--- END SHASHHERMES TRAINING SNIPPET ---\n";
    ss << "=======================================================================\n";

    return ss.str();
}

}  // namespace

std::string Eval::trace(Position& pos, const Eval::NNUE::Network& network, bool showWDL) {

    if (pos.checkers())
        return "Final evaluation: none (in check)";

    auto accumulators = std::make_unique<Eval::NNUE::AccumulatorStack>();
    auto caches       = std::make_unique<Eval::NNUE::AccumulatorCaches>(network);

    std::stringstream ss;

    if (!WDLModel::is_initialized())
        WDLModel::init();

    Value v       = evaluate(network, pos, *accumulators, *caches, VALUE_ZERO);
    Value v_white = pos.side_to_move() == WHITE ? v : -v;

    if (showWDL)
    {
        uint8_t       winProbWhite = WDLModel::get_win_probability(v_white, pos);
        WDLModel::WDL wdl          = WDLModel::get_wdl(v_white, pos);
        std::string   shashinZone  = get_shashin_zone(winProbWhite, WHITE);

        ss << std::showpoint << std::noshowpos << std::fixed << std::setprecision(2);
        ss << '\n' << NNUE::trace(pos, network, *caches) << '\n';

        ss << "=== SHASHIN STATIC ANALYSIS ===\n";
        ss << "*** Static activity evaluation based on NNUE network ***\n";
        ss << "*** All NNUE values shown in centipawn for technical accuracy ***\n";
        ss << "*** Win probability and Shashin zone always from White's perspective ***\n\n";

        ss << "--- PIECE ACTIVITY ANALYSIS (Makogonov Principle) ---\n";
        ss << analyze_piece_activity_horizontal(pos) << "\n";

        ss << "--- LEGAL MOVES ANALYSIS ---\n";
        ss << analyze_legal_moves(pos, network) << "\n";

        // FIX DEFINITIVO SF 19: Evitiamo trace_evaluate (che causa crash a causa dell'assenza
        // del bucket structure). Estraiamo i punteggi nudi e crudi in totale sicurezza.
        auto [psqt_syn, positional_syn] = network.evaluate(pos, *accumulators, *caches);

        const int persp_syn  = pos.side_to_move() == WHITE ? 1 : -1;
        int       wp_mat_syn = WDLModel::get_win_probability(Value(persp_syn * psqt_syn), pos);
        int       wp_tot_syn =
          WDLModel::get_win_probability(Value(persp_syn * (psqt_syn + positional_syn)), pos);
        int structural_delta = wp_tot_syn - wp_mat_syn;

        ss << "--- STRATEGIC SYNTHESIS ---\n";
        ss << generate_strategic_synthesis(winProbWhite, wdl, shashinZone, v_white, pos,
                                           structural_delta)
           << "\n";

        ss << "================================\n";
        ss << "Final Static Activity Evaluation: " << 0.01 * UCIEngine::to_cp(v_white, pos)
           << " pawns (White perspective)\n";
        ss << "WDL (Win/Draw/Loss): " << static_cast<int>(wdl.win) << "%/"
           << static_cast<int>(wdl.draw) << "%/" << static_cast<int>(wdl.loss) << "%\n";
        ss << "Shashin Zone: " << shashinZone << "\n";
        ss << "*** Note: Static activity analysis based on current piece placement ***\n";
    }
    else
    {
        ss << std::showpoint << std::noshowpos << std::fixed << std::setprecision(2);
        ss << '\n' << NNUE::trace(pos, network, *caches) << '\n';
        ss << std::showpoint << std::showpos << std::fixed << std::setprecision(2) << std::setw(15);

        auto [psqt, positional] = network.evaluate(pos, *accumulators, *caches);
        Value v_nnue            = psqt + positional;
        ss << "NNUE evaluation          " << v_nnue << " (side to move, internal units)\n";
        v_nnue = pos.side_to_move() == WHITE ? v_nnue : -v_nnue;
        ss << "NNUE evaluation        " << 0.01 * UCIEngine::to_cp(v_nnue, pos)
           << " (white side)\n";

        ss << "Final evaluation      ";
        ss << 0.01 * UCIEngine::to_cp(v_white, pos) << " (white side)";
        ss << " [with scaled NNUE, ...]\n";
    }

    return ss.str();
}

}  // namespace ShashChess