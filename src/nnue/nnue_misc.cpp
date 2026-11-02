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

#include "nnue_misc.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iosfwd>
#include <iostream>
#include <memory>
#include <sstream>
#include <string_view>
#include <tuple>
#include <vector>

#include "../misc.h"
#include "../position.h"
#include "../types.h"
#include "../uci.h"
#include "../wdl/win_probability.h"
#include "network.h"
#include "nnue_accumulator.h"

namespace ShashChess::Eval::NNUE {

constexpr std::string_view PieceToChar(" PNBRQK  pnbrqk");

namespace {

[[maybe_unused]] void format_cp_compact(Value v, char* buffer, const Position& pos) {
    buffer[0] = (v < 0 ? '-' : v > 0 ? '+' : ' ');
    int cp    = std::abs(UCIEngine::to_cp(v, pos));
    if (cp >= 10000)
    {
        buffer[1] = '0' + cp / 10000;
        cp %= 10000;
        buffer[2] = '0' + cp / 1000;
        cp %= 1000;
        buffer[3] = '0' + cp / 100;
        buffer[4] = ' ';
    }
    else if (cp >= 1000)
    {
        buffer[1] = '0' + cp / 1000;
        cp %= 1000;
        buffer[2] = '0' + cp / 100;
        cp %= 100;
        buffer[3] = '.';
        buffer[4] = '0' + cp / 10;
    }
    else
    {
        buffer[1] = '0' + cp / 100;
        cp %= 100;
        buffer[2] = '.';
        buffer[3] = '0' + cp / 10;
        cp %= 10;
        buffer[4] = '0' + cp / 1;
    }
}

[[maybe_unused]] void
format_cp_aligned_dot(Value v, std::stringstream& stream, const Position& pos) {
    const double pawns = std::abs(0.01 * UCIEngine::to_cp(v, pos));
    stream << (v < 0   ? '-'
               : v > 0 ? '+'
                       : ' ')
           << std::setiosflags(std::ios::fixed) << std::setw(6) << std::setprecision(2) << pawns;
}

}  // namespace

std::string
trace(Position& pos, const Eval::NNUE::Network& network, Eval::NNUE::AccumulatorCaches& caches) {

    std::stringstream ss;
    char              board[3 * 8 + 1][8 * 8 + 2];
    std::memset(board, ' ', sizeof(board));

    for (int row = 0; row < 3 * 8 + 1; ++row)
        board[row][8 * 8 + 1] = '\0';

    if (!WDLModel::is_initialized())
    {
        WDLModel::init();
    }

    auto writeSquareWP = [&board](File file, Rank rank, Piece pc, int wp_delta) {
        const int x = int(file) * 8;
        const int y = (7 - int(rank)) * 3;
        for (int i = 1; i < 8; ++i)
            board[y][x + i] = board[y + 3][x + i] = '-';
        for (int i = 1; i < 3; ++i)
            board[y + i][x] = board[y + i][x + 8] = '|';
        board[y][x] = board[y][x + 8] = board[y + 3][x + 8] = board[y + 3][x] = '+';
        if (pc != NO_PIECE)
            board[y + 1][x + 4] = PieceToChar[pc];
        if (wp_delta != -999)
        {
            char buf[6] = "     ";
            buf[0]      = (wp_delta < 0 ? '-' : wp_delta > 0 ? '+' : ' ');
            int val     = std::abs(wp_delta);
            if (val >= 100)
            {
                buf[1] = '1';
                buf[2] = '0';
                buf[3] = '0';
                buf[4] = '%';
            }
            else if (val >= 10)
            {
                buf[1] = ' ';
                buf[2] = '0' + (val / 10);
                buf[3] = '0' + (val % 10);
                buf[4] = '%';
            }
            else
            {
                buf[1] = ' ';
                buf[2] = ' ';
                buf[3] = '0' + val;
                buf[4] = '%';
            }
            std::memcpy(&board[y + 2][x + 2], buf, 5);
        }
    };

    auto                                     accumulators = std::make_unique<AccumulatorStack>();
    std::vector<std::pair<std::string, int>> white_impacts;
    std::vector<std::pair<std::string, int>> black_impacts;

    int single_ablation[SQUARE_NB];
    for (int i = 0; i < SQUARE_NB; ++i)
        single_ablation[i] = -999;

    auto [psqt, positional] = network.evaluate(pos, *accumulators, caches);
    Value base              = psqt + positional;
    base                    = pos.side_to_move() == WHITE ? base : -base;
    int wp_base             = WDLModel::get_win_probability(base, pos);

    // Sezione 1 & 2: Ablazione Spaziale e Classifica SHAP
    for (File f = FILE_A; f <= FILE_H; ++f)
    {
        for (Rank r = RANK_1; r <= RANK_8; ++r)
        {
            Square sq       = make_square(f, r);
            Piece  pc       = pos.piece_on(sq);
            int    wp_delta = -999;

            if (pc != NO_PIECE && type_of(pc) != KING)
            {
                pos.remove_piece(sq);
                accumulators->reset();
                auto [psqt_abl, pos_abl] = network.evaluate(pos, *accumulators, caches);
                Value eval               = psqt_abl + pos_abl;
                eval                     = pos.side_to_move() == WHITE ? eval : -eval;

                int wp_abl = WDLModel::get_win_probability(eval, pos);
                if (color_of(pc) == WHITE)
                    wp_delta = wp_base - wp_abl;
                else
                    wp_delta = wp_abl - wp_base;
                single_ablation[sq] = wp_delta;
                pos.put_piece(pc, sq);

                std::string pieceName = std::string(1, PieceToChar[pc]) + UCIEngine::square(sq);
                if (color_of(pc) == WHITE)
                    white_impacts.emplace_back(pieceName, wp_delta);
                else
                    black_impacts.emplace_back(pieceName, wp_delta);
            }
            writeSquareWP(f, r, pc, wp_delta);
        }
    }

    // Sezione 5: Inserimento Virtuale (Avamposti Latenti)
    // Il bucket NNUE vale (pezzi - 1) / 4 e deve restare < LayerStacks: con 32 pezzi
    // un pezzo fantasma porterebbe il bucket a 8, fuori dagli array della rete
    // (psqtAccumulation e layer stack) -> lettura fuori dai limiti / crash di 'eval'.
    // La sezione 7 prova solo le case trovate qui, quindi eredita la protezione.
    std::vector<std::pair<std::string, int>> outpost_impacts;
    Piece                                    ghost_knight = make_piece(pos.side_to_move(), KNIGHT);
    Color                                    stm          = pos.side_to_move();
    const bool ghostAllowed = pos.count<ALL_PIECES>() < int(LayerStacks) * 4;

    for (Square sq = SQ_A1; ghostAllowed && sq <= SQ_H8; ++sq)
    {
        if (pos.piece_on(sq) == NO_PIECE)
        {
            pos.put_piece(ghost_knight, sq);
            accumulators->reset();
            auto [psqt_ghost, pos_ghost] = network.evaluate(pos, *accumulators, caches);
            Value eval_ghost             = psqt_ghost + pos_ghost;
            eval_ghost                   = stm == WHITE ? eval_ghost : -eval_ghost;

            int wp_ghost    = WDLModel::get_win_probability(eval_ghost, pos);
            int improvement = (stm == WHITE) ? (wp_ghost - wp_base) : (wp_base - wp_ghost);
            if (improvement > 0)
            {
                outpost_impacts.emplace_back(UCIEngine::square(sq), improvement);
            }
            pos.remove_piece(sq);
        }
    }

    // Sezione 6: Ablazione a Coppie (Sinergia Filtrata)
    std::vector<std::tuple<std::string, std::string, int>> synergy_impacts;
    Bitboard                                               my_pieces = pos.pieces(stm);
    std::vector<Square>                                    my_squares;
    while (my_pieces)
        my_squares.push_back(pop_lsb(my_pieces));

    for (size_t i = 0; i < my_squares.size(); ++i)
    {
        for (size_t j = i + 1; j < my_squares.size(); ++j)
        {
            Square sqA = my_squares[i];
            Square sqB = my_squares[j];
            Piece  pcA = pos.piece_on(sqA);
            Piece  pcB = pos.piece_on(sqB);

            if (type_of(pcA) == KING || type_of(pcB) == KING)
                continue;

            int deltaA = single_ablation[sqA];
            int deltaB = single_ablation[sqB];

            if (deltaA != -999 && deltaB != -999)
            {
                int wp_abl_A = (stm == WHITE) ? (wp_base - deltaA) : (wp_base + deltaA);
                int wp_abl_B = (stm == WHITE) ? (wp_base - deltaB) : (wp_base + deltaB);

                if (wp_abl_A > 5 && wp_abl_A < 95 && wp_abl_B > 5 && wp_abl_B < 95)
                {
                    pos.remove_piece(sqA);
                    pos.remove_piece(sqB);
                    accumulators->reset();
                    auto [psqt_both, pos_both] = network.evaluate(pos, *accumulators, caches);
                    Value eval_both            = psqt_both + pos_both;
                    eval_both                  = stm == WHITE ? eval_both : -eval_both;

                    int wp_both   = WDLModel::get_win_probability(eval_both, pos);
                    int deltaBoth = (stm == WHITE) ? (wp_base - wp_both) : (wp_both - wp_base);
                    pos.put_piece(pcA, sqA);
                    pos.put_piece(pcB, sqB);

                    int synergy = deltaBoth - (deltaA + deltaB);
                    if (std::abs(synergy) > 1)
                    {
                        std::string nameA =
                          std::string(1, PieceToChar[pcA]) + UCIEngine::square(sqA);
                        std::string nameB =
                          std::string(1, PieceToChar[pcB]) + UCIEngine::square(sqB);
                        synergy_impacts.emplace_back(nameA, nameB, synergy);
                    }
                }
            }
        }
    }

    // Stampe base Sezioni 1-6
    ss << "=== NNUE MECHANISTIC TRACE (WIN PROBABILITY DOMAIN) ===\n";
    ss << "*** Base Win Probability (White): " << wp_base << "% ***\n\n";
    ss << "1. SPATIAL ABLATION GRID (Delta WP % if piece is removed):\n";
    for (int row = 0; row < 3 * 8 + 1; ++row)
        ss << board[row] << '\n';
    ss << '\n';

    auto sort_impacts = [](const auto& a, const auto& b) {
        return std::abs(a.second) > std::abs(b.second);
    };
    std::sort(white_impacts.begin(), white_impacts.end(), sort_impacts);
    std::sort(black_impacts.begin(), black_impacts.end(), sort_impacts);

    ss << "2. CRITICAL FEATURES RANKING (SHAP-Proxy in WP%)\n";
    ss << "   White's Most Influential Pieces:\n";
    for (size_t i = 0; i < std::min(white_impacts.size(), size_t(10)); ++i)
    {
        ss << "    - " << std::setw(3) << white_impacts[i].first << " : " << std::showpos
           << white_impacts[i].second << std::noshowpos << "% WP impact\n";
    }
    ss << "\n   Black's Most Influential Pieces:\n";
    for (size_t i = 0; i < std::min(black_impacts.size(), size_t(10)); ++i)
    {
        ss << "    - " << std::setw(3) << black_impacts[i].first << " : " << std::showpos
           << black_impacts[i].second << std::noshowpos << "% WP impact\n";
    }
    ss << '\n';

    auto sort_outposts = [](const auto& a, const auto& b) { return a.second > b.second; };
    std::sort(outpost_impacts.begin(), outpost_impacts.end(), sort_outposts);

    ss << "5. VIRTUAL PIECE INSERTION (Latent Outposts / Weaknesses)\n";
    ss << "   Highest WP impact on empty squares if a friendly Knight were placed there:\n";
    if (!ghostAllowed)
        ss << "    (n/a: " << pos.count<ALL_PIECES>()
           << " pieces on the board, the network cannot evaluate an extra piece)\n";
    for (size_t i = 0; i < std::min(outpost_impacts.size(), size_t(5)); ++i)
    {
        ss << "    - Square " << outpost_impacts[i].first << " : " << std::showpos
           << outpost_impacts[i].second << std::noshowpos << "% WP improvement\n";
    }
    ss << '\n';

    auto sort_synergy = [](const auto& a, const auto& b) {
        return std::abs(std::get<2>(a)) > std::abs(std::get<2>(b));
    };
    std::sort(synergy_impacts.begin(), synergy_impacts.end(), sort_synergy);

    ss << "6. PAIRWISE ABLATION (Non-linear Synergy & Coordination)\n";
    ss << "   Coordination in WP % (Positive = Battery/Defense, Negative = Redundancy):\n";
    for (size_t i = 0; i < std::min(synergy_impacts.size(), size_t(5)); ++i)
    {
        ss << "    - " << std::get<0>(synergy_impacts[i]) << " + "
           << std::get<1>(synergy_impacts[i]) << " : " << std::showpos
           << std::get<2>(synergy_impacts[i]) << std::noshowpos << "% WP synergy\n";
    }
    ss << '\n';

    accumulators->reset();
    auto        t            = network.trace_evaluate(pos, *accumulators, caches);
    int         total_pieces = pos.count<ALL_PIECES>();
    std::string phase_desc;
    if (t.correctBucket >= 6)
        phase_desc = "Opening / Early Middlegame (High Complexity)";
    else if (t.correctBucket >= 3)
        phase_desc = "Late Middlegame (Strategic Phase)";
    else
        phase_desc = "Endgame (Simplified Topology)";

    ss << "3. NEURAL NETWORK INTERNAL ROUTING\n";
    ss << "   Active Sub-Network (Bucket) : " << t.correctBucket << " out of 7\n";
    ss << "   Piece Count Detected        : " << total_pieces << "\n";
    ss << "   Deduced Game Phase          : " << phase_desc << "\n\n";

    ss << "4. EVALUATION COMPOSITION\n";
    ss << "+--------+-------------------+-----------------------+----------+\n"
       << "| Bucket | Material/PSQT(WP) | Deep Positional Delta | Total WP |\n"
       << "+--------+-------------------+-----------------------+----------+\n";

    for (std::size_t bucket = 0; bucket < LayerStacks; ++bucket)
    {
        int wp_mat    = WDLModel::get_win_probability(t.psqt[bucket], pos);
        int wp_tot    = WDLModel::get_win_probability(t.psqt[bucket] + t.positional[bucket], pos);
        int pos_delta = wp_tot - wp_mat;
        ss << "| " << (bucket == t.correctBucket ? "->" : "  ") << " " << bucket << "  | "
           << std::setw(15) << wp_mat << "% | " << std::setw(19) << std::showpos << pos_delta
           << std::noshowpos << "% | " << std::setw(7) << wp_tot << "% |\n";
    }
    ss << "+--------+-------------------+-----------------------+----------+\n";

    int wp_mat_act = WDLModel::get_win_probability(t.psqt[t.correctBucket], pos);
    int wp_tot_act =
      WDLModel::get_win_probability(t.psqt[t.correctBucket] + t.positional[t.correctBucket], pos);
    int pos_delta_act = wp_tot_act - wp_mat_act;

    ss << "\n   Abstract Understanding vs Material (Active Bucket):\n";
    ss << "   - Material perception (PSQT)   : " << wp_mat_act << "% WP\n";
    ss << "   - Structural abstraction       : " << std::showpos << pos_delta_act << std::noshowpos
       << "% WP added\n";
    ss
      << "   (Note: High structural abstraction WP shift indicates complex patterns overriding raw material)\n\n";

    // 7. MULTI-PIECE FILTER PROBE
    struct FilterProbe {
        std::string sq_name;
        int         knight_imp = 0;
        int         bishop_imp = 0;
        int         rook_imp   = 0;
        int         best_imp   = 0;
        char        best_char  = 'N';
    };
    std::vector<FilterProbe> filter_probes;

    {
        auto probe_piece_type = [&](Square probe_sq, PieceType pt) -> int {
            if (!ghostAllowed)
                return 0;
            Piece ghost = make_piece(stm, pt);
            pos.put_piece(ghost, probe_sq);
            accumulators->reset();
            auto [p_g, q_g] = network.evaluate(pos, *accumulators, caches);
            Value e_g       = p_g + q_g;
            e_g             = stm == WHITE ? e_g : -e_g;
            int wp_g        = WDLModel::get_win_probability(e_g, pos);
            pos.remove_piece(probe_sq);
            int raw = wp_g - wp_base;
            return (stm == WHITE) ? raw : -raw;
        };

        size_t probe_limit = std::min(outpost_impacts.size(), size_t(6));
        for (size_t pi = 0; pi < probe_limit; ++pi)
        {
            const auto& [sq_name, kn_imp] = outpost_impacts[pi];
            if (sq_name.size() < 2)
                continue;
            Square probe_sq = make_square(File(sq_name[0] - 'a'), Rank(sq_name[1] - '1'));
            if (probe_sq < SQ_A1 || probe_sq > SQ_H8)
                continue;
            if (pos.piece_on(probe_sq) != NO_PIECE)
                continue;

            FilterProbe fp;
            fp.sq_name    = sq_name;
            fp.knight_imp = kn_imp;
            fp.best_imp   = kn_imp;
            fp.best_char  = 'N';

            fp.bishop_imp = probe_piece_type(probe_sq, BISHOP);
            fp.rook_imp   = probe_piece_type(probe_sq, ROOK);

            if (fp.bishop_imp > fp.best_imp)
            {
                fp.best_imp  = fp.bishop_imp;
                fp.best_char = 'B';
            }
            if (fp.rook_imp > fp.best_imp)
            {
                fp.best_imp  = fp.rook_imp;
                fp.best_char = 'R';
            }

            if (fp.best_imp > 0)
                filter_probes.push_back(fp);
        }
        std::sort(filter_probes.begin(), filter_probes.end(),
                  [](const auto& a, const auto& b) { return a.best_imp > b.best_imp; });
    }

    ss << "7. MULTI-PIECE FILTER PROBE (Transcoder Channel Activation)\n";
    ss << "   Delta WP% se un pezzo viene inserito sulla casa vuota:\n";
    ss << "   Sq   | Knight | Bishop | Rook  | Winner (filtro NNUE attivato)\n";
    ss << "   " << std::string(58, '-') << "\n";
    for (const auto& fp : filter_probes)
    {
        const char* winner_desc = "N (mobility/outpost filter)";
        if (fp.best_char == 'B')
            winner_desc = "B (diagonal filter)";
        if (fp.best_char == 'R')
            winner_desc = "R (column/file filter)";

        ss << "   " << std::left << std::setw(5) << fp.sq_name << std::right << std::showpos
           << std::setw(6) << fp.knight_imp << "%" << std::setw(7) << fp.bishop_imp << "%"
           << std::setw(6) << fp.rook_imp << "%" << std::noshowpos << "  => " << winner_desc
           << "\n";
    }
    if (filter_probes.empty())
        ss << "   (Nessun miglioramento significativo trovato)\n";
    ss << '\n';

    // 8. PIECE CONTRIBUTION SPECTRUM
    std::pair<std::string, int>              white_best_piece  = {"", -999};
    std::pair<std::string, int>              black_best_piece  = {"", -999};
    std::pair<std::string, int>              white_worst_piece = {"", 999};
    std::pair<std::string, int>              black_worst_piece = {"", 999};
    std::vector<std::pair<std::string, int>> white_blindspots, black_blindspots;

    auto classify_impact = [](int d) -> const char* {
        if (d < -1)
            return "BLINDSPOT";
        if (d < 2)
            return "Passive  ";
        if (d < 5)
            return "Standard ";
        if (d < 10)
            return "Active   ";
        return "Keystone ";
    };

    ss << "8. PIECE CONTRIBUTION SPECTRUM\n";
    ss << "   Keystone(>=10%) | Active(5-9%) | Standard(2-4%) | Passive(<2%) | BLINDSPOT(neg)\n";
    ss << "   White:\n   ";
    for (const auto& [nm, d] : white_impacts)
    {
        ss << nm << "[" << classify_impact(d) << " " << std::showpos << d << std::noshowpos
           << "%] ";
        if (d > white_best_piece.second)
            white_best_piece = {nm, d};
        if (d >= 0 && d < white_worst_piece.second)
            white_worst_piece = {nm, d};
        if (d < -1)
            white_blindspots.emplace_back(nm, d);
    }
    ss << "\n   Black:\n   ";
    for (const auto& [nm, d] : black_impacts)
    {
        ss << nm << "[" << classify_impact(d) << " " << std::showpos << d << std::noshowpos
           << "%] ";
        if (d > black_best_piece.second)
            black_best_piece = {nm, d};
        if (d >= 0 && d < black_worst_piece.second)
            black_worst_piece = {nm, d};
        if (d < -1)
            black_blindspots.emplace_back(nm, d);
    }
    ss << "\n\n";

    // 9. NATURAL LANGUAGE NNUE INTERPRETATION (SCAP Framework)
    ss << "9. NATURAL LANGUAGE NNUE INTERPRETATION (SCAP Framework)\n";
    ss << "   " << std::string(66, '~') << "\n";

    // 9A: Assessment
    {
        const int   wp_active = (stm == WHITE) ? wp_base : (100 - wp_base);
        const char* side      = (stm == WHITE) ? "White" : "Black";
        ss << "   [ASSESSMENT] ";
        if (wp_active >= 90)
            ss << side << " is winning decisively (" << wp_active
               << "% WP). Technical conversion required.\n";
        else if (wp_active >= 75)
            ss << side << " has a clear, substantial advantage (" << wp_active << "% WP).\n";
        else if (wp_active >= 62)
            ss << side << " is clearly better (" << wp_active << "% WP). Press the advantage.\n";
        else if (wp_active >= 52)
            ss << side << " holds a slight edge (" << wp_active << "% WP).\n";
        else if (wp_active >= 48)
            ss << "The position is dynamically balanced (White WP=" << wp_base << "%).\n";
        else if (wp_active >= 38)
            ss << side << " is slightly worse. Seek counterplay.\n";
        else if (wp_active >= 25)
            ss << side << " is clearly worse. Find practical resources.\n";
        else
            ss << side << " faces a decisive deficit. Extreme defensive measures required.\n";
    }

    // 9B: Phase & Routing
    {
        const char* precise_phase = "Unknown";
        switch (t.correctBucket)
        {
        case 7 :
            precise_phase = "Opening (32 pcs) - massima complessita' strutturale";
            break;
        case 6 :
            precise_phase = "Early Middlegame (25-28 pcs) - piano ancora fluido";
            break;
        case 5 :
            precise_phase = "Middlegame (21-24 pcs) - fase strategica centrale";
            break;
        case 4 :
            precise_phase = "Late Middlegame (17-20 pcs) - semplificazioni iniziate";
            break;
        case 3 :
            precise_phase = "Endgame Transition (13-16 pcs)";
            break;
        case 2 :
            precise_phase = "Endgame (9-12 pcs) - re attivo, pedonale decisivo";
            break;
        case 1 :
            precise_phase = "Late Endgame (5-8 pcs)";
            break;
        default :
            precise_phase = "Pawn/King Endgame (1-4 pcs)";
            break;
        }
        ss << "   [PHASE] Sub-network #" << t.correctBucket << " -> " << precise_phase << ".\n";
    }

    // 9C: Neural Signal
    {
        if (std::abs(pos_delta_act) >= 3)
        {
            ss << "   [NEURAL SIGNAL] PSQT=" << wp_mat_act << "% -> Total=" << wp_tot_act << "% WP";
            if (pos_delta_act > 0)
                ss << " (+" << pos_delta_act << "% da pattern recognition). ";
            else
                ss << " (" << pos_delta_act << "% penalita' strutturale). ";
            if (pos_delta_act >= 10)
                ss
                  << "FORTE segnale neurale: la rete vede pattern complessi non riducibili al materiale.\n";
            else if (pos_delta_act <= -10)
                ss << "FORTE segnale negativo: debolezze erodono il vantaggio materiale.\n";
            else
                ss << "Correzione strutturale moderata.\n";
        }
        else
        {
            ss << "   [NEURAL SIGNAL] La valutazione e' prevalentemente materiale.\n";
        }
    }

    // 9D: Keystones
    if (!white_best_piece.first.empty() && white_best_piece.second > 0)
    {
        ss << "   [WHITE KEYSTONE] " << white_best_piece.first
           << " (impatto: " << white_best_piece.second << "% WP).\n";
    }
    if (!black_best_piece.first.empty() && black_best_piece.second > 0)
    {
        ss << "   [BLACK KEYSTONE] " << black_best_piece.first
           << " (impatto: " << black_best_piece.second << "% WP).\n";
    }

    // 9E: Worst Pieces (Makogonov)
    if (!white_worst_piece.first.empty() && white_worst_piece.second < 5)
    {
        ss << "   [WHITE MAKOGONOV] " << white_worst_piece.first << " contribuisce solo "
           << white_worst_piece.second << "% WP - pezzo peggiore.\n";
    }
    if (!black_worst_piece.first.empty() && black_worst_piece.second < 5)
    {
        ss << "   [BLACK MAKOGONOV] " << black_worst_piece.first << " contribuisce solo "
           << black_worst_piece.second << "% WP - pezzo peggiore.\n";
    }

    // 9F: Blindspots
    for (const auto& [nm, d] : white_blindspots)
    {
        ss << "   [*** BLINDSPOT/WHITE ***] " << nm
           << " riduce paradossalmente la WP del Bianco di " << (-d)
           << "%! (Defensive Fixation).\n";
    }
    for (const auto& [nm, d] : black_blindspots)
    {
        ss << "   [*** BLINDSPOT/BLACK ***] " << nm << " riduce paradossalmente la WP del Nero di "
           << (-d) << "%! (Defensive Fixation).\n";
    }

    // 9G: Outpost
    if (!outpost_impacts.empty())
    {
        const auto& [best_sq, best_kn_imp] = outpost_impacts[0];
        ss << "   [OUTPOST] Casa " << best_sq << " e' il miglior avamposto latente";
        bool found_better = false;
        for (const auto& fp : filter_probes)
        {
            if (fp.sq_name == best_sq)
            {
                if (fp.best_char == 'B')
                    ss << " (Alfiere domina: +" << fp.bishop_imp
                       << "% -> filtro diagonale attivo).\n";
                else if (fp.best_char == 'R')
                    ss << " (Torre domina: +" << fp.rook_imp << "% -> colonna/filtro attivo).\n";
                else
                    ss << " (Cavallo ottimale: +" << best_kn_imp << "% WP -> avamposto attivo).\n";
                found_better = true;
                break;
            }
        }
        if (!found_better)
            ss << " (Cavallo: +" << best_kn_imp << "% WP).\n";
    }

    // 9H: Battery
    if (!synergy_impacts.empty())
    {
        const auto& [na, nb, syn] = synergy_impacts[0];
        if (syn > 1)
            ss << "   [BATTERY] " << na << "+" << nb << " formano una batteria coordinata (+" << syn
               << "% WP).\n";
    }

    // 9I: Strategic Plan
    ss << "   [PIANO STRATEGICO] ";
    if (wp_base >= 70)
        ss << "Conversione tecnica: semplificare e prendere di mira i punti deboli.";
    else if (wp_base >= 55)
        ss
          << "Spingere l'attivita': migliorare il pezzo peggiore, contestare i filtri neurali attivi.";
    else if (wp_base >= 45)
        ss
          << "Mantenere l'equilibrio dinamico: preservare la coordinazione, difendere le case outpost.";
    else
        ss << "Ricercare risorse tattiche e contrattacco per rompere la stabilita' avversaria.";
    ss << "\n";

    // 9J: SHASHHERMES COMPACT TRACE FORMAT
    ss << "\n   [SHASHHERMES COMPACT TRACE FORMAT]\n";
    ss << "   WP_White=" << wp_base << "% | ";
    if (wp_base >= 95)
        ss << "Zone=HighTal | ";
    else if (wp_base >= 90)
        ss << "Zone=HighMidTal | ";
    else if (wp_base >= 85)
        ss << "Zone=MidTal | ";
    else if (wp_base >= 80)
        ss << "Zone=MidLowTal | ";
    else if (wp_base >= 76)
        ss << "Zone=LowTal | ";
    else if (wp_base > 50)
        ss << "Zone=Capablanca-Tal | ";
    else if (wp_base == 50)
        ss << "Zone=Capablanca | ";
    else if (wp_base >= 25)
        ss << "Zone=Capablanca-Petrosian | ";
    else if (wp_base >= 21)
        ss << "Zone=LowPetrosian | ";
    else if (wp_base >= 16)
        ss << "Zone=MidLowPetrosian | ";
    else if (wp_base >= 11)
        ss << "Zone=MidPetrosian | ";
    else if (wp_base >= 6)
        ss << "Zone=HighMidPetrosian | ";
    else
        ss << "Zone=HighPetrosian | ";

    if (!white_worst_piece.first.empty())
        ss << "WorstWhite=" << white_worst_piece.first << "(" << white_worst_piece.second
           << "%) | ";
    if (!black_worst_piece.first.empty())
        ss << "WorstBlack=" << black_worst_piece.first << "(" << black_worst_piece.second
           << "%) | ";
    if (!outpost_impacts.empty())
        ss << "BestOutpost=" << outpost_impacts[0].first << "(+" << outpost_impacts[0].second
           << "%) | ";
    if (!white_blindspots.empty())
        ss << "BLINDSPOT_W=" << white_blindspots[0].first << " | ";
    if (!black_blindspots.empty())
        ss << "BLINDSPOT_B=" << black_blindspots[0].first << " | ";
    ss << "Bucket=" << t.correctBucket << " | NeuralDelta=" << std::showpos << pos_delta_act
       << std::noshowpos << "%\n";
    ss << "   " << std::string(66, '~') << "\n\n";

    return ss.str();
}

}  // namespace ShashChess::Eval::NNUE
