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
#include <array>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <tuple>
#include <vector>

#include "misc.h"
#include "movegen.h"
#include "nnue/network.h"
#include "nnue/nnue_explain.h"
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
    return evaluate_unchecked(network, pos, accumulators, caches, optimism);
}

// [SC:trace] Same as evaluate(), without the "not in check" assertion: the static probes of
// the trace (quiescence searches) also evaluate positions in check at their depth limit.
Value Eval::evaluate_unchecked(const Eval::NNUE::Network&     network,
                               const Position&                pos,
                               Eval::NNUE::AccumulatorStack&  accumulators,
                               Eval::NNUE::AccumulatorCaches& caches,
                               int                            optimism) {

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

// ============================================================================================
// [SC:trace] Static probes: NNUE static evaluation resolved by a small quiescence search
// (captures with SEE >= 0, all evasions when in check). A bare static evaluation after a
// capture ignores the recapture (Bxf6 looked like "+3" with ...Nxf6 on the board): every move
// and every "what if" of the trace is evaluated through this probe instead.
// ============================================================================================
constexpr int   QS_DEPTH   = 8;
constexpr int   NODE_LIMIT = 20000;
constexpr Value MATE_SCORE = 20000;

bool is_mate(Value v) { return std::abs(v) >= MATE_SCORE - 2 * MAX_PLY; }

class Probe {
   public:
    explicit Probe(const Eval::NNUE::Network& n) :
        net(n),
        acc(std::make_unique<Eval::NNUE::AccumulatorStack>()),
        caches(std::make_unique<Eval::NNUE::AccumulatorCaches>(n)) {}

    // Static evaluation, side to move point of view
    Value static_eval(const Position& p) {
        acc->reset();
        return Eval::evaluate_unchecked(net, p, *acc, *caches, VALUE_ZERO);
    }

    // Quiet evaluation, side to move point of view; 'best' receives the first move of the
    // capture sequence found (if any)
    Value quiet(Position& p, Move* best = nullptr) {
        nodes    = 0;
        rootBest = Move::none();
        Value v  = search(p, -VALUE_INFINITE, VALUE_INFINITE, QS_DEPTH, 0);
        if (best)
            *best = rootBest;
        return v;
    }

    Value quiet_white(Position& p) {
        Value v = quiet(p);
        return p.side_to_move() == WHITE ? v : -v;
    }

   private:
    Value search(Position& p, Value alpha, Value beta, int depth, int ply) {
        ++nodes;
        const bool inCheck = bool(p.checkers());
        if (inCheck)
        {
            MoveList<LEGAL> evasions(p);
            if (evasions.size() == 0)
                return -MATE_SCORE + ply;
            if (depth > -4 && nodes < NODE_LIMIT)
            {
                Value best = -MATE_SCORE + ply;
                for (const auto& m : evasions)
                {
                    StateInfo st;
                    p.do_move(m, st);
                    Value v = -search(p, -beta, -alpha, depth - 1, ply + 1);
                    p.undo_move(m);
                    if (v > best)
                    {
                        best = v;
                        if (ply == 0)
                            rootBest = m;
                        if (v > alpha && (alpha = v) >= beta)
                            break;
                    }
                }
                return best;
            }
        }
        Value stand = static_eval(p);
        if (inCheck || stand >= beta || depth <= 0 || nodes >= NODE_LIMIT)
            return stand;
        alpha = std::max(alpha, stand);

        std::vector<std::pair<int, Move>> caps;
        for (const auto& m : MoveList<CAPTURES>(p))
        {
            if (!p.legal(m) || !p.see_ge(m, 0))
                continue;
            const Piece captured    = p.piece_on(m.to_sq());
            const Value victimValue = m.type_of() == EN_PASSANT ? PawnValue
                                    : captured == NO_PIECE      ? VALUE_ZERO
                                                                : PieceValue[captured];
            caps.emplace_back(victimValue * 8 - PieceValue[p.piece_on(m.from_sq())] / 64, m);
        }
        std::sort(caps.begin(), caps.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        Value best = stand;
        for (const auto& [order, m] : caps)
        {
            StateInfo st;
            p.do_move(m, st);
            Value v = -search(p, -beta, -alpha, depth - 1, ply + 1);
            p.undo_move(m);
            if (v > best)
            {
                best = v;
                if (ply == 0)
                    rootBest = m;
                if (v > alpha && (alpha = v) >= beta)
                    break;
            }
        }
        return best;
    }

    const Eval::NNUE::Network&                     net;
    std::unique_ptr<Eval::NNUE::AccumulatorStack>  acc;
    std::unique_ptr<Eval::NNUE::AccumulatorCaches> caches;
    int                                            nodes = 0;
    Move                                           rootBest;
};

// Builds a position from an edited board (castling rights kept only where still valid).
// Returns false if the edited position is illegal (the side not to move is in check).
bool make_position(const std::array<Piece, SQUARE_NB>& b,
                   Color                               stm,
                   const Position&                     ref,
                   Position&                           out,
                   StateInfo&                          st) {
    constexpr std::string_view PieceChars(" PNBRQK  pnbrqk");
    std::stringstream          ss;
    for (int r = RANK_8; r >= RANK_1; --r)
    {
        int empty = 0;
        for (int f = FILE_A; f <= FILE_H; ++f)
        {
            Piece pc = b[make_square(File(f), Rank(r))];
            if (pc == NO_PIECE)
                ++empty;
            else
            {
                if (empty)
                    ss << empty;
                empty = 0;
                ss << PieceChars[pc];
            }
        }
        if (empty)
            ss << empty;
        if (r > RANK_1)
            ss << '/';
    }
    ss << (stm == WHITE ? " w " : " b ");
    std::string castling;
    if (!ref.is_chess960())
    {
        if (ref.can_castle(WHITE_OO) && b[SQ_E1] == W_KING && b[SQ_H1] == W_ROOK)
            castling += 'K';
        if (ref.can_castle(WHITE_OOO) && b[SQ_E1] == W_KING && b[SQ_A1] == W_ROOK)
            castling += 'Q';
        if (ref.can_castle(BLACK_OO) && b[SQ_E8] == B_KING && b[SQ_H8] == B_ROOK)
            castling += 'k';
        if (ref.can_castle(BLACK_OOO) && b[SQ_E8] == B_KING && b[SQ_A8] == B_ROOK)
            castling += 'q';
    }
    ss << (castling.empty() ? "-" : castling) << " - 0 1";
    if (out.set(ss.str(), ref.is_chess960(), &st))
        return false;
    return !(out.attackers_to(out.square<KING>(~stm)) & out.pieces(stm));
}

std::string color_name(Color c) { return c == WHITE ? "White" : "Black"; }

std::string piece_label(Piece pc, Square s) {
    return std::string(1, "PNBRQK"[type_of(pc) - 1]) + UCIEngine::square(s);
}

Bitboard adjacent_files(File f) {
    return (f > FILE_A ? file_bb(File(f - 1)) : 0) | (f < FILE_H ? file_bb(File(f + 1)) : 0);
}

// Squares in front of 's' (from c's point of view) on the same file
Bitboard forward_file(Color c, Square s) {
    Bitboard b = 0;
    for (int r = int(rank_of(s)) + (c == WHITE ? 1 : -1); r >= RANK_1 && r <= RANK_8;
         r += (c == WHITE ? 1 : -1))
        b |= make_square(file_of(s), Rank(r));
    return b;
}

Bitboard pawn_attacks(Color c, Bitboard b) {
    return c == WHITE ? shift(b, NORTH_WEST) | shift(b, NORTH_EAST)
                      : shift(b, SOUTH_WEST) | shift(b, SOUTH_EAST);
}

// Shashin zone from the win probability
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

// Horizontal (one line per side) static piece activity analysis
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

        // Piece overload (with the color) so that pawn attacks are correct
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

        // Piece overload (with the color) so that pawn attacks are correct
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

// ============================================================================================
// [SC:trace] Shared context of the advanced modules
// ============================================================================================
struct Candidate {
    Move  m;
    Value q;     // quiet value after the move, mover's point of view
    Value gain;  // q minus the quiet value after a pass (tempo neutral improvement)
    int   wp;    // expected score of the mover, %
    bool  capture, check, kingMove, pawnMove, sacrifice;
};

struct Context {
    Position&                  pos;
    const Eval::NNUE::Network& net;
    Probe                      probe;
    Eval::NNUE::NnueInsight    insight;
    Color                      us, them;
    bool                       inCheck;
    Value                      staticUs, quietUs, nullQuietUs;
    Move                       rootTactic, threatMove;
    bool                       tactical;  // an immediate tactic dominates the position
    std::vector<Candidate>     cands;     // all legal moves, best first

    Context(Position& p, const Eval::NNUE::Network& n, Eval::NNUE::AccumulatorCaches& caches) :
        pos(p),
        net(n),
        probe(n),
        insight(Eval::NNUE::analyze(p, n, caches)),
        us(p.side_to_move()),
        them(~p.side_to_move()),
        inCheck(bool(p.checkers())) {

        staticUs    = probe.static_eval(pos);
        quietUs     = probe.quiet(pos, &rootTactic);
        tactical    = is_mate(quietUs) || std::abs(quietUs - staticUs) > 2 * PawnValue;
        nullQuietUs = quietUs;
        threatMove  = Move::none();
        if (!inCheck)
        {
            StateInfo st;
            pos.do_null_move(st);
            nullQuietUs = -probe.quiet(pos, &threatMove);
            pos.undo_null_move();
        }
        for (const auto& m : MoveList<LEGAL>(pos))
        {
            Candidate c;
            c.m         = m;
            c.capture   = pos.capture_stage(m);
            c.check     = pos.gives_check(m);
            c.kingMove  = type_of(pos.piece_on(m.from_sq())) == KING;
            c.pawnMove  = type_of(pos.piece_on(m.from_sq())) == PAWN;
            c.sacrifice = !pos.see_ge(m, 0);
            StateInfo st;
            pos.do_move(m, st);
            c.q = -probe.quiet(pos);
            pos.undo_move(m);
            c.gain = c.q - nullQuietUs;
            c.wp   = WDLModel::get_win_probability(c.q, pos);
            cands.push_back(c);
        }
        // Near-ties (the static evaluation saturates in won endgames) are broken in favour of
        // queen promotions: a small sorting bonus, the displayed values are unchanged
        // (in clearly won positions the bonus is larger: there the network output is flat and
        // the move that wins the most material is the practical choice)
        auto key = [](const Candidate& c) {
            const bool queenPromo =
              c.m.type_of() == PROMOTION && c.m.promotion_type() == QUEEN;
            return c.q + (queenPromo ? (c.q > 2 * PawnValue ? 2 * PawnValue : 15) : 0);
        };
        std::stable_sort(cands.begin(), cands.end(),
                         [&](const Candidate& a, const Candidate& b) { return key(a) > key(b); });
    }

    int         cp(Value v) const { return UCIEngine::to_cp(v, pos); }
    std::string cps(Value v) const {
        if (is_mate(v))
            return v > 0 ? "MATE" : "MATED";
        std::stringstream ss;
        ss << std::showpos << cp(v) << std::noshowpos << " cp";
        return ss.str();
    }
    std::string mv(Move m) const { return UCIEngine::move(m, pos.is_chess960()); }
    // Baseline of the "what if" probes: the quiet value, or the static one in tactical positions
    Value base_us() const { return tactical ? staticUs : quietUs; }
    Value probe_us(Position& p) {
        Value v = tactical ? probe.static_eval(p) : probe.quiet(p);
        return p.side_to_move() == us ? v : -v;
    }
};

// Structured facts collected by the advanced modules: the Master's Verdict is composed from
// these, so that every combination of cases produces coherent natural language
struct Facts {
    // threats and prophylaxis
    bool        threat = false, threatMate = false;
    Move        threatMove = Move::none();
    int         threatCp   = 0;
    Move        oppIdea    = Move::none();
    int         oppIdeaCp  = 0;
    // space
    int         spaceQs = 0, spaceC = 0, spaceKs = 0;
    std::string spaceZone;
    Square      spaceKeyPawn = SQ_NONE;
    // outposts
    std::vector<Square> outposts;
    Square              favSquare = SQ_NONE;
    PieceType           favType   = NO_PIECE_TYPE;
    // color complexes
    bool lightDom = false, darkDom = false;
    // trades
    bool hasQueenTrade = false, hasRookTrade = false;
    int  queenTrade = 0, rookTrade = 0;
    std::vector<std::pair<std::string, int>> goodTrades, badTrades;  // "your X for the Y", cp
    // files
    std::string open, semiUs, semiThem;
    // pawn structure: index 0 = side to move, 1 = opponent
    std::vector<std::string> passed[2], isolated[2], backward[2];
    bool                     tension = false, releaseTension = false;
    Move                     tensionMove = Move::none();
    int                      tensionCp   = 0;
    std::vector<Move>        goodBreaks, badBreaks, shelterBad;
    // own king
    int  missingShield = 0, ringAttacks = 0, zonePressureCp = 0;
    Move kingMove      = Move::none();
    int  kingGainCp    = 0;
    bool kingDanger    = false;
};

struct MasterReportData {
    Facts       f;
    std::string phase_advice;
    std::string volatility;
    std::string tension;
    std::string pieces;
    std::string rooks;
    std::string queens;
    std::string files;
    std::string color_complex;
    std::string pawns;
    std::string king;
    std::string space;
    std::string outposts;
    std::string threats;
    std::string plan;
};

// ============================================================================================
// LEGAL MOVES ANALYSIS (original block): now evaluated with the quiet probe
// ============================================================================================
std::string analyze_legal_moves(Context& ctx) {
    std::stringstream ss;
    ss << "Legal moves ordered by static activity (best first): ";
    for (size_t i = 0; i < ctx.cands.size(); ++i)
    {
        const auto& c = ctx.cands[i];
        if (i > 0)
            ss << ", ";
        ss << ctx.mv(c.m) << "(" << std::showpos << std::fixed << std::setprecision(1)
           << double(ctx.cp(c.q)) << " cp, " << c.wp << "%)" << std::noshowpos;
    }
    ss << "\n(values: NNUE static evaluation resolved by a capture-only quiescence search, "
          "mover's point of view)\n";
    return ss.str();
}

std::string generate_strategic_synthesis(uint8_t              winProbWhite,
                                         const WDLModel::WDL& wdl,
                                         const std::string&   shashinZone,
                                         Value                v_white,
                                         const Position&      pos,
                                         int                  pos_delta_act,
                                         int                  complexity) {
    std::stringstream ss;
    const int         wp = static_cast<int>(winProbWhite);
    const int         cp = UCIEngine::to_cp(v_white, pos);
    const int         dw = static_cast<int>(wdl.win);
    const int         dd = static_cast<int>(wdl.draw);
    const int         dl = static_cast<int>(wdl.loss);

    ss << "=======================================================================\n";
    ss << "STRATEGIC SYNTHESIS - Ground Truth Format\n";
    ss << "=======================================================================\n";
    ss << "Evaluation  : " << (cp >= 0 ? "+" : "") << std::fixed << std::setprecision(2)
       << 0.01 * cp << " pawns | WP White=" << wp << "% | WDL=" << dw << "/" << dd << "/" << dl
       << "%\n";
    ss << "Shashin Zone: " << shashinZone << "\n\n";

    ss << "Position Character:\n";
    ss << "  Drawability   : "
       << (dd > 60   ? "VERY HIGH. Fortress risk."
           : dd > 40 ? "MODERATE. Balanced."
                     : "LOW. Decisive.")
       << "\n";
    // [SC:trace] The old test (|win - loss| > 50 -> "LOW stability") was inverted and did not
    // measure volatility at all: use the NNUE complexity |PSQT - positional| instead
    ss << "  Volatility    : "
       << (complexity > 250  ? "HIGH variance. Critical tactical calculations."
           : complexity < 75 ? "LOW. Stable position, maneuvering play."
                             : "MODERATE. Mix of calculation and maneuvering.")
       << " (NNUE complexity " << UCIEngine::to_cp(complexity, pos) << " cp)\n";

    ss << "\nStrategic Directives:\n";
    if (wp >= 65)
        ss << "  [PRESS] Improve the worst piece. Target vulnerabilities. Open active channels.\n";
    else if (wp >= 45)
        ss << "  [BALANCE] Keep piece coordination tight. Prevent tactical outposts.\n";
    else
        ss << "  [COUNTERPLAY] Imbalance the game. Complicate variations. Avoid massive trades.\n";

    ss << "\n--- TRAINING SNIPPET ---\n";
    ss << "SHASHCHESS TRACE: [" << wp << "% | Zone=" << shashinZone << " | WDL=" << dw << "/"
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

    ss << "--- END TRAINING SNIPPET ---\n";
    ss << "=======================================================================\n";

    return ss.str();
}

// ============================================================================================
// ADVANCED SHASHIN MODULES (ported from Branch-Max to the single network of ShashChess 42)
// ============================================================================================

std::string get_shashin_advice(uint8_t winProbWhite, Color sideToMove, MasterReportData& mr) {
    uint8_t           myWinProb = (sideToMove == WHITE) ? winProbWhite : (100 - winProbWhite);
    std::stringstream ss;
    ss << "--- SHASHIN STRATEGIC DIRECTIVE (" << color_name(sideToMove) << " to move) ---\n";
    if (myWinProb <= 10)
    {
        ss << "[!] TIGRAN PETROSIAN MODE (CRITICAL DEFENSE)\n    * Observation: You are facing a "
              "decisive disadvantage (Win Probability: "
           << static_cast<int>(myWinProb) << "%).\n";
        mr.phase_advice = "Critical position: you are at a severe disadvantage. Your only priority "
                          "is to build defensive barricades and seek complications or stalemates.";
    }
    else if (myWinProb <= 25)
    {
        ss << "[!] TIGRAN PETROSIAN MODE (DEFENSE & PROPHYLAXIS)\n    * Observation: You are "
              "facing a significant disadvantage (Win Probability: "
           << static_cast<int>(myWinProb) << "%).\n";
        mr.phase_advice = "Clear disadvantage: the opponent has strong pressure. Focus on solid "
                          "defense and apply strict prophylaxis to avoid yielding vital space.";
    }
    else if (myWinProb <= 45)
    {
        ss << "[~] CAPABLANCA-PETROSIAN MODE (ACTIVE DEFENSE)\n    * Observation: The opponent "
              "has a slight edge (Win Probability: "
           << static_cast<int>(myWinProb) << "%).\n";
        mr.phase_advice = "Slight disadvantage: you are under enemy initiative but the position is "
                          "elastic. Absorb the pressure while maintaining a solid block.";
    }
    else if (myWinProb <= 54)
    {
        ss << "[i] JOSE RAUL CAPABLANCA MODE (POSITIONAL HARMONY)\n    * Observation: The "
              "position is balanced or transitional (Win Probability: "
           << static_cast<int>(myWinProb) << "%).\n";
        mr.phase_advice = "Absolute balance: apply classical positional principles. Improve piece "
                          "coordination and accumulate imperceptible long-term advantages.";
    }
    else if (myWinProb <= 74)
    {
        ss << "[~] CAPABLANCA-TAL MODE (INITIATIVE)\n    * Observation: You have a comfortable "
              "edge (Win Probability: "
           << static_cast<int>(myWinProb) << "%).\n";
        mr.phase_advice = "Slight advantage: you dictate the pace and enjoy the initiative. "
                          "Maintain constant pressure to force the opponent to create permanent "
                          "weaknesses.";
    }
    else if (myWinProb <= 89)
    {
        ss << "[!] MIKHAIL TAL MODE (ATTACK)\n    * Observation: You possess a clear advantage "
              "(Win Probability: "
           << static_cast<int>(myWinProb) << "%).\n";
        mr.phase_advice = "Clear advantage: you are in control. Abandon slow maneuvering and "
                          "calculate forcing lines and tactical combinations to convert.";
    }
    else
    {
        ss << "[!] MIKHAIL TAL MODE (DOMINATION)\n    * Observation: You possess a "
              "dominant/winning advantage (Win Probability: "
           << static_cast<int>(myWinProb) << "%).\n";
        mr.phase_advice = "Absolute dominance: victory is within grasp. Play the safest and most "
                          "dominant moves to convert the advantage, without allowing the opponent "
                          "any desperate counterplay.";
    }
    return ss.str();
}

std::string analyze_engine_flags(Context& ctx, MasterReportData& mr) {
    std::stringstream ss;
    ss << "--- STRUCTURAL & DYNAMIC FLAGS ---\n";
    auto accumulators       = std::make_unique<Eval::NNUE::AccumulatorStack>();
    auto caches             = std::make_unique<Eval::NNUE::AccumulatorCaches>(ctx.net);
    auto [psqt, positional] = ctx.net.evaluate(ctx.pos, *accumulators, *caches);
    const int complexity    = std::abs(psqt - positional);

    ss << "[i] VOLATILITY INDEX: " << ctx.cp(complexity) << " cp (|PSQT - positional|)\n";
    if (complexity > 250)
        mr.volatility = "In this configuration, tactical combinations outweigh long-term "
                        "strategic evaluations.";
    else if (complexity < 75)
        mr.volatility =
          "Piece positioning is stable; you can rely on classical positional maneuvering.";
    else
        mr.volatility = "The position requires a balance between precise tactical calculation "
                        "and strategic development.";

    const auto& in  = ctx.insight;
    const int   thr = ctx.cp(in.familyThreat), pp = ctx.cp(in.familyPair);
    ss << "[i] DYNAMIC vs STATIC: piece relations " << std::showpos << thr
       << " cp, pawn structure " << pp << " cp" << std::noshowpos
       << (std::abs(thr) >= std::abs(pp) ? " -> dynamic position\n" : " -> structural position\n");
    if (ctx.tactical)
        ss << "[!] TACTICAL ALERT: " << color_name(ctx.us) << " has an immediate tactic ("
           << ctx.mv(ctx.rootTactic) << ", " << ctx.cps(ctx.quietUs) << ").\n";
    ss << "\n";
    return ss.str();
}

std::string analyze_threats(Context& ctx, MasterReportData& mr) {
    std::stringstream ss;
    ss << "--- THREATS AND PROPHYLAXIS (what if " << color_name(ctx.us) << " passed?) ---\n";
    if (ctx.inCheck)
    {
        ss << "  " << color_name(ctx.us) << " is in check: the first task is to parry it.\n\n";
        mr.threats = "You are in check: parry it first.";
        return ss.str();
    }
    const Value loss = ctx.base_us() - ctx.nullQuietUs;
    if (ctx.threatMove && (is_mate(ctx.nullQuietUs) || ctx.cp(loss) >= 10))
    {
        ss << "  [!] THREAT: " << color_name(ctx.them) << " threatens " << ctx.mv(ctx.threatMove)
           << " (" << (is_mate(ctx.nullQuietUs) ? "MATE" : std::to_string(ctx.cp(loss)) + " cp")
           << "): it must be prevented or met.\n";
        mr.threats = "Beware of the threat " + ctx.mv(ctx.threatMove) + ": address it first.";
        mr.f.threat     = true;
        mr.f.threatMate = is_mate(ctx.nullQuietUs);
        mr.f.threatMove = ctx.threatMove;
        mr.f.threatCp   = ctx.cp(loss);
    }
    else
    {
        ss << "  No immediate tactical threat against " << color_name(ctx.us) << ".\n";
        mr.threats = "There is no immediate tactical threat against you.";
    }

    // Opponent's ideas: its best quiet moves if it were its turn (prophylaxis targets)
    std::vector<std::pair<Value, Move>> ideas;
    {
        StateInfo st;
        ctx.pos.do_null_move(st);
        if (!ctx.pos.checkers())
            for (const auto& m : MoveList<LEGAL>(ctx.pos))
            {
                StateInfo st2;
                ctx.pos.do_move(m, st2);
                Value q = -ctx.probe.quiet(ctx.pos);
                ctx.pos.undo_move(m);
                // Improvement for the opponent with respect to the current position (same
                // side to move in both: us), so that the comparison is tempo neutral
                ideas.emplace_back(q + ctx.base_us(), m);
            }
        ctx.pos.undo_null_move();
    }
    std::sort(ideas.begin(), ideas.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    ss << "  Opponent's ideas (" << color_name(ctx.them) << "'s best moves if it were its turn):\n  > ";
    for (size_t i = 0; i < ideas.size() && i < 5; ++i)
        ss << (i ? " | " : "") << ctx.mv(ideas[i].second) << " (" << ctx.cps(ideas[i].first)
           << ")";
    ss << (ideas.empty() ? "n/a\n" : "\n");
    if (!ideas.empty())
    {
        mr.f.oppIdea   = ideas.front().second;
        mr.f.oppIdeaCp = ctx.cp(ideas.front().first);
    }
    if (!ideas.empty() && ctx.cp(ideas.front().first) >= 15)
        mr.threats += " Prophylaxis: the opponent wants to play " + ctx.mv(ideas.front().second)
                    + ".";
    ss << "\n";
    return ss.str();
}

std::string analyze_space_advantage(const Position& pos, MasterReportData& mr) {
    std::stringstream ss;
    Color             us         = pos.side_to_move();
    Color             them       = ~us;
    Bitboard          qs         = FileABB | FileBBB | FileCBB;
    Bitboard          center     = FileDBB | FileEBB;
    Bitboard          ks         = FileFBB | FileGBB | FileHBB;

    // Squares controlled by the pieces of 'c' in the half of the board of the opponent, plus the
    // advancement of its pawns, in a given sector
    auto count_space = [&](Color c, Bitboard zone) {
        const Bitboard enemyCamp = c == WHITE ? (Rank5BB | Rank6BB | Rank7BB | Rank8BB)
                                              : (Rank4BB | Rank3BB | Rank2BB | Rank1BB);
        int      score  = 0;
        Bitboard pce_bb = pos.pieces(c) & ~pos.pieces(PAWN, KING);
        while (pce_bb)
        {
            Square    sq = pop_lsb(pce_bb);
            PieceType pt = type_of(pos.piece_on(sq));
            score += popcount(Attacks::attacks_bb(pt, sq, pos.pieces()) & zone & enemyCamp);
        }
        Bitboard pawns = pos.pieces(c, PAWN) & zone;
        while (pawns)
        {
            int r = relative_rank(c, pop_lsb(pawns));
            if (r >= RANK_4)
                score += (r - RANK_3) * 2;
        }
        return score;
    };

    int delta_qs    = count_space(us, qs) - count_space(them, qs);
    int delta_c     = count_space(us, center) - count_space(them, center);
    int delta_ks    = count_space(us, ks) - count_space(them, ks);
    int total_delta = delta_qs + delta_c + delta_ks;

    mr.f.spaceQs = delta_qs;
    mr.f.spaceC  = delta_c;
    mr.f.spaceKs = delta_ks;
    ss << "--- SPACE ADVANTAGE AND ZONE OF ACTION (" << color_name(us) << ") ---\n";
    ss << "    Sector balance: queen side " << std::showpos << delta_qs << ", center " << delta_c
       << ", king side " << delta_ks << std::noshowpos << "\n";
    std::string best_zone;
    Bitboard    best_bb   = 0;
    int         max_delta = 2;
    if (delta_qs > max_delta)
    {
        best_zone = "Queenside";
        best_bb   = qs;
        max_delta = delta_qs;
    }
    if (delta_ks > max_delta)
    {
        best_zone = "Kingside";
        best_bb   = ks;
        max_delta = delta_ks;
    }
    if (delta_c > max_delta)
    {
        best_zone = "Center";
        best_bb   = center;
    }

    if (!best_zone.empty())
    {
        Square   key   = SQ_NONE;
        int      max_r = -1;
        Bitboard pz    = pos.pieces(us, PAWN) & best_bb;
        while (pz)
        {
            Square psq = pop_lsb(pz);
            if (int(relative_rank(us, psq)) > max_r)
            {
                max_r = relative_rank(us, psq);
                key   = psq;
            }
        }
        std::string key_pawn_str =
          key != SQ_NONE ? " (advanced pawn on " + UCIEngine::square(key) + ")" : "";
        ss << "[+] SPACE ADVANTAGE: You dominate the " << best_zone << key_pawn_str << ".\n";
        mr.f.spaceZone    = best_zone;
        mr.f.spaceKeyPawn = key != SQ_NONE && relative_rank(us, key) >= RANK_4 ? key : SQ_NONE;
        mr.space = "You have a space advantage on the " + best_zone + key_pawn_str
                 + ": operate in that sector and avoid trading pieces.";
    }
    else if (total_delta < -6)
    {
        ss << "[-] SPACE DISADVANTAGE: The opponent has more territorial control.\n";
        mr.space = "You have a space disadvantage: seek minor piece trades to relieve pressure.";
        mr.f.spaceZone = "-";
    }
    else
    {
        ss << "[~] SPATIAL BALANCE: Neither side markedly dominates space.\n";
        mr.space = "Space control is balanced across the board.";
    }
    ss << "\n";
    return ss.str();
}

// Outposts: classical holes (no enemy pawn can ever attack them) supported by an own pawn, in
// the opponent's half of the board, ranked by the NNUE value of a virtual knight on them
std::string analyze_knight_outposts(Context& ctx, MasterReportData& mr) {
    std::stringstream ss;
    const Position&   pos  = ctx.pos;
    const Color       us   = ctx.us;
    const Bitboard    ours = pos.pieces(us, PAWN), theirs = pos.pieces(ctx.them, PAWN);
    const Bitboard    zone = us == WHITE ? (Rank4BB | Rank5BB | Rank6BB) : (Rank5BB | Rank4BB | Rank3BB);

    auto hole = [&](Square s) {
        // An enemy pawn could attack s only from the adjacent files, ahead of s
        Bitboard span = 0;
        for (Bitboard f = adjacent_files(file_of(s)); f;)
            span |= forward_file(us, make_square(file_of(pop_lsb(f)), rank_of(s)));
        return !(theirs & span);
    };

    std::vector<std::pair<Value, Square>> outposts;
    for (const auto& o : ctx.insight.outposts[us])
        if ((zone & o.sq) && hole(o.sq) && (pawn_attacks(us, ours) & o.sq)
            && o.byType[0] != VALUE_NONE)
            outposts.emplace_back(o.byType[0] - ctx.insight.reference[us][KNIGHT], o.sq);
    std::sort(outposts.begin(), outposts.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });

    for (const auto& o : outposts)
        mr.f.outposts.push_back(o.second);
    ss << "--- KNIGHT OUTPOSTS (" << color_name(us)
       << ": pawn-supported holes, ranked by the NNUE value of a knight there) ---\n";
    if (!outposts.empty())
    {
        ss << "[+] OUTPOSTS IDENTIFIED:\n";
        mr.outposts = "Aim to place your Knights on key outposts such as ";
        for (size_t i = 0; i < outposts.size() && i < 4; ++i)
        {
            ss << "    * square " << UCIEngine::square(outposts[i].second) << " (knight "
               << std::showpos << ctx.cp(outposts[i].first) << std::noshowpos
               << " cp above a typical square)\n";
            mr.outposts += (i ? ", " : "") + UCIEngine::square(outposts[i].second);
        }
        mr.outposts += ".";
    }
    else
    {
        ss << "[-] No supported outposts available at the moment.\n";
        mr.outposts = "No immediate outposts are available for the Knights.";
    }
    // Squares where the NNUE would like a piece most (any square, any type)
    const Bitboard advanced = us == WHITE ? (Rank4BB | Rank5BB | Rank6BB | Rank7BB)
                                          : (Rank5BB | Rank4BB | Rank3BB | Rank2BB);
    auto fav = std::find_if(ctx.insight.outposts[us].begin(), ctx.insight.outposts[us].end(),
                            [&](const auto& o) { return bool(advanced & o.sq); });
    if (fav != ctx.insight.outposts[us].end())
    {
        const auto& o = *fav;
        mr.f.favSquare = o.sq;
        mr.f.favType   = o.pt;
        ss << "[i] NNUE favourite advanced square for a piece: " << UCIEngine::square(o.sq) << " ("
           << (o.pt == KNIGHT ? "knight" : o.pt == BISHOP ? "bishop" : "rook") << ", "
           << std::showpos << ctx.cp(o.relative) << std::noshowpos << " cp above typical)\n";
    }
    ss << "\n";
    return ss.str();
}

std::string analyze_color_complexes(Context& ctx, MasterReportData& mr) {
    std::stringstream ss;
    const Position&   pos  = ctx.pos;
    const Color       us   = ctx.us;
    const Color       them = ctx.them;
    auto light = [](Square s) { return ((int(file_of(s)) + int(rank_of(s))) % 2) != 0; };

    double us_light = 0, us_dark = 0, them_light = 0, them_dark = 0;
    bool   them_light_b = false, them_dark_b = false;
    for (const auto& p : ctx.insight.pieces)
    {
        const double v = 0.01 * ctx.cp(p.value);
        if (color_of(p.pc) == them && type_of(p.pc) == BISHOP)
            (light(p.sq) ? them_light_b : them_dark_b) = true;
        if (color_of(p.pc) == us)
            (light(p.sq) ? us_light : us_dark) += v;
        else
            (light(p.sq) ? them_light : them_dark) += v;
    }
    // Pressure map on each color complex (NNUE relations, our point of view)
    double pressLight = 0, pressDark = 0;
    for (Square s = SQ_A1; s <= SQ_H8; ++s)
        (light(s) ? pressLight : pressDark) +=
          0.01 * ctx.cp(us == WHITE ? ctx.insight.pressure[s] : -ctx.insight.pressure[s]);

    bool us_light_b = false, us_dark_b = false;
    for (Bitboard b = pos.pieces(us, BISHOP); b;)
        (light(pop_lsb(b)) ? us_light_b : us_dark_b) = true;
    // Domination of a color complex: we have a bishop of that color, the opponent has none,
    // and our pieces on that color are worth more than their counterparts (paired ablation
    // values: material is balanced, so 1 pawn of difference is already significant) or our
    // relations press on it
    const bool light_dom =
      us_light_b && !them_light_b && (us_light - them_light > 1.0 || pressLight > 0.3);
    const bool dark_dom = us_dark_b && !them_dark_b && (us_dark - them_dark > 1.0 || pressDark > 0.3);

    ss << "--- COLOR COMPLEXES (Geometric Dominance and Weaknesses) ---\n";
    ss << std::fixed << std::setprecision(2) << "    NNUE piece value (paired ablation) on light/dark squares: "
       << color_name(us) << " " << us_light << "/" << us_dark << ", " << color_name(them) << " "
       << them_light << "/" << them_dark << "\n";
    ss << "    NNUE relation pressure (" << color_name(us) << " view): light " << std::showpos
       << pressLight << ", dark " << pressDark << std::noshowpos << "\n";
    mr.f.lightDom = light_dom;
    mr.f.darkDom  = dark_dom;
    if (light_dom)
        ss << "[!] DOMINANCE ON LIGHT SQUARES (only you have a light-squared bishop)\n";
    if (dark_dom)
        ss << "[!] DOMINANCE ON DARK SQUARES (only you have a dark-squared bishop)\n";
    if (light_dom)
        mr.color_complex = "Exploit the structural dominance on the light squares: the opponent "
                           "lacks suitable means to defend them.";
    else if (dark_dom)
        mr.color_complex = "You have established clear control over the dark squares: use these "
                           "voids for an attack.";
    else
        mr.color_complex = "The control of light and dark squares is balanced.";
    ss << "\n";
    (void) pos;
    return ss.str();
}

std::string analyze_piece_importance(Context& ctx, MasterReportData& mr) {
    std::stringstream ss;
    Position&         pos  = ctx.pos;
    const Color       us   = ctx.us;
    const Color       them = ctx.them;
    const auto&       in   = ctx.insight;

    ss << "--- COMPACT PIECE IMPORTANCE & TRADE ADVISORY ---\n";
    for (Color c : {us, them})
    {
        std::vector<const Eval::NNUE::PieceInsight*> v;
        for (const auto& p : in.pieces)
            if (color_of(p.pc) == c && type_of(p.pc) != PAWN)
                v.push_back(&p);
        std::sort(v.begin(), v.end(),
                  [](auto a, auto b) { return a->efficiency > b->efficiency; });
        ss << (c == us ? "[+] " : "[-] ") << color_name(c) << " FORCES (NNUE efficiency from the paired ablation, '*' = no counterpart; activity):\n";
        if (v.empty())
            ss << "    (no pieces besides king and pawns)\n";
        for (const auto* p : v)
        {
            std::string pattern;
            const int   r = relative_rank(c, p->sq);
            if (type_of(p->pc) == BISHOP)
            {
                if ((c == WHITE && (p->sq == SQ_B2 || p->sq == SQ_G2))
                    || (c == BLACK && (p->sq == SQ_B7 || p->sq == SQ_G7)))
                    pattern = "fianchettoed";
                else
                {
                    const bool lightB = (int(file_of(p->sq)) + int(rank_of(p->sq))) % 2;
                    int        same   = 0;
                    for (Bitboard pw = pos.pieces(c, PAWN); pw;)
                    {
                        Square s = pop_lsb(pw);
                        same += ((int(file_of(s)) + int(rank_of(s))) % 2) == lightB;
                    }
                    if (same >= 4 && p->efficiency < 100)
                        pattern = "blocked by own pawns";
                }
            }
            else if (type_of(p->pc) == KNIGHT && r >= RANK_4 && r <= RANK_6
                     && (pawn_attacks(c, pos.pieces(c, PAWN)) & p->sq)
                     && !(pawn_attacks(~c, pos.pieces(~c, PAWN)) & p->sq))
                pattern = "on a solid outpost";
            ss << "    " << std::left << std::setw(4) << piece_label(p->pc, p->sq) << std::right
               << " efficiency " << std::setw(4) << p->efficiency << "%, activity "
               << std::showpos << std::setw(4) << ctx.cp(p->activity) << std::noshowpos << " cp"
               << (p->counterparts ? "" : "*")
               << (p->efficiency >= (p->counterparts ? 110 : 115) && ctx.cp(p->activity) > -5
                     ? " -> [DOMINANT: Structural Pillar]"
                   : p->efficiency < (p->counterparts ? 90 : 80) || ctx.cp(p->activity) <= -15
                     ? " -> [PASSIVE: Positional Liability]"
                     : " -> [ACTIVE]")
               << (pattern.empty() ? "" : " [" + pattern + "]") << "\n";
        }
    }

    // Trades: remove one piece of each side (same class) and probe the result
    struct Trade {
        std::string ours, theirs;
        int         cpUs;
        PieceType   pt;
    };
    std::vector<Trade> trades;
    auto cls  = [](PieceType pt) { return pt == BISHOP ? KNIGHT : pt; };
    auto name = [](Piece pc, Square s) {
        static const char* N[] = {"", "Pawn", "Knight", "Bishop", "Rook", "Queen", "King"};
        return std::string(N[type_of(pc)]) + " on " + UCIEngine::square(s);
    };
    auto lname = [&](const std::string& x) {
        std::string r = x;
        r[0]          = char(std::tolower(r[0]));
        return r;
    };
    const Value base = ctx.base_us();
    auto        probe_board = [&](const std::array<Piece, SQUARE_NB>& b, Value& out) {
        StateInfo st;
        Position  tmp;
        if (!make_position(b, us, pos, tmp, st))
            return false;
        out = ctx.probe_us(tmp);
        return true;
    };
    for (Square a = SQ_A1; a <= SQ_H8; ++a)
    {
        Piece pa = pos.piece_on(a);
        if (pa == NO_PIECE || color_of(pa) != us || type_of(pa) == PAWN || type_of(pa) == KING)
            continue;
        for (Square b = SQ_A1; b <= SQ_H8; ++b)
        {
            Piece pb = pos.piece_on(b);
            if (pb == NO_PIECE || color_of(pb) != them || cls(type_of(pb)) != cls(type_of(pa)))
                continue;
            auto bd = pos.piece_array();
            bd[a] = bd[b] = NO_PIECE;
            Value v;
            if (probe_board(bd, v))
                trades.push_back({name(pa, a), name(pb, b), ctx.cp(v - base), type_of(pa)});
        }
    }
    auto mass = [&](PieceType pt, int& out) {
        if (!pos.count<ALL_PIECES>() || !(pos.pieces(us, pt) && pos.pieces(them, pt)))
            return false;
        auto bd = pos.piece_array();
        for (auto& p : bd)
            if (p != NO_PIECE && type_of(p) == pt)
                p = NO_PIECE;
        Value v;
        if (!probe_board(bd, v))
            return false;
        out = ctx.cp(v - base);
        return true;
    };

    std::sort(trades.begin(), trades.end(), [](auto& x, auto& y) { return x.cpUs > y.cpUs; });
    for (const auto& t : trades)
        if (t.cpUs > 5 && mr.f.goodTrades.size() < 3)
            mr.f.goodTrades.emplace_back("your " + lname(t.ours) + " for the " + lname(t.theirs),
                                         t.cpUs);
    for (auto it = trades.rbegin(); it != trades.rend(); ++it)
        if (it->cpUs < -5 && mr.f.badTrades.size() < 2)
            mr.f.badTrades.emplace_back("your " + lname(it->ours) + " for the " + lname(it->theirs),
                                        it->cpUs);
    ss << "\n    Trade advisory (" << color_name(us) << " point of view, cp):\n";
    int queenTrade = 0, rookTrade = 0;
    const bool hasQ = mass(QUEEN, queenTrade), hasR = mass(ROOK, rookTrade);
    mr.f.hasQueenTrade = hasQ;
    mr.f.hasRookTrade  = hasR;
    mr.f.queenTrade    = queenTrade;
    mr.f.rookTrade     = rookTrade;
    if (hasQ)
        ss << "      Queen trade: " << std::showpos << queenTrade << std::noshowpos << "\n";
    if (hasR)
        ss << "      All rooks off: " << std::showpos << rookTrade << std::noshowpos << "\n";
    for (size_t i = 0; i < trades.size() && i < 3; ++i)
        if (trades[i].cpUs > 5)
            ss << "      Favourable: your " << trades[i].ours << " for the " << trades[i].theirs
               << " (" << std::showpos << trades[i].cpUs << std::noshowpos << ")\n";
    for (size_t i = 0, n = 0; i < trades.size() && n < 2; ++i)
    {
        const auto& t = trades[trades.size() - 1 - i];
        if (t.cpUs < -5)
        {
            ss << "      Avoid: your " << t.ours << " for the " << t.theirs << " (" << t.cpUs
               << ")\n";
            ++n;
        }
    }

    // Master report
    const Eval::NNUE::PieceInsight *usBest = nullptr, *usWorst = nullptr, *thBest = nullptr,
                                   *thWorst = nullptr;
    for (const auto& p : in.pieces)
    {
        if (type_of(p.pc) == PAWN)
            continue;
        auto& best  = color_of(p.pc) == us ? usBest : thBest;
        auto& worst = color_of(p.pc) == us ? usWorst : thWorst;
        if (!best || p.activity > best->activity)
            best = &p;
        if (!worst || p.activity < worst->activity)
            worst = &p;
    }
    if (usBest && thWorst && ctx.cp(usBest->activity) >= 10
        && usBest->activity - thWorst->activity > 0)
        mr.pieces = "The piece comparison favors you: your " + name(usBest->pc, usBest->sq)
                  + " dominates the position, while the opponent's "
                  + name(thWorst->pc, thWorst->sq) + " is its most passive unit.";
    else if (thBest && usWorst && ctx.cp(thBest->activity) >= 10)
        mr.pieces = "The opponent possesses a well-placed " + name(thBest->pc, thBest->sq)
                  + ", while you are penalized by the passivity of your "
                  + name(usWorst->pc, usWorst->sq) + ".";
    else if (usWorst)
        mr.pieces = "Forces are balanced; your least active unit is the "
                  + name(usWorst->pc, usWorst->sq) + " (Makogonov: improve it first).";
    else
        mr.pieces = "Minor forces are balanced.";
    if (!trades.empty() && trades.front().cpUs > 5)
        mr.pieces += " The most profitable trade is your " + trades.front().ours + " for the "
                   + trades.front().theirs + ".";
    else if (usBest)
        mr.pieces += " In case of simplifications, ensure your best piece ("
                   + name(usBest->pc, usBest->sq) + ") survives.";

    if (hasR)
        mr.rooks = rookTrade > 10  ? "It is advantageous to trade all Rooks."
                 : rookTrade < -10 ? "Keep at least one Rook: a total exchange of Rooks favors "
                                     "the opponent."
                                   : "A total exchange of Rooks does not significantly alter the "
                                     "balance.";
    else
        mr.rooks = "Rooks are absent or asymmetrical.";

    if (!pos.pieces(QUEEN))
        mr.queens = "With Queens off the board, precision in maneuvering is essential.";
    else if (!hasQ)
        mr.queens = "Only one side has a Queen.";
    else
        mr.queens = queenTrade > 10 ? "Actively seek a Queen trade: it drains the opponent's "
                                      "counterplay and leads to an advantageous endgame."
                  : queenTrade < -10 ? "Keep your Queen: the Queen trade favors the opponent."
                                     : "Queens are in play: evaluate trading them on a "
                                       "case-by-case basis without forcing it.";
    ss << "\n";
    return ss.str();
}

std::string analyze_open_files(const Position& pos, MasterReportData& mr) {
    std::stringstream ss;
    Color             us          = pos.side_to_move();
    Color             them        = ~us;
    Bitboard          our_pawns   = pos.pieces(us, PAWN);
    Bitboard          their_pawns = pos.pieces(them, PAWN);
    std::string       open, semiUs, semiThem;

    for (int f = FILE_A; f <= FILE_H; ++f)
    {
        const Bitboard fb   = file_bb(File(f));
        const char     name = char('a' + f);
        if (!(our_pawns & fb) && !(their_pawns & fb))
            open += name;
        else if (!(our_pawns & fb))
            semiUs += name;
        else if (!(their_pawns & fb))
            semiThem += name;
    }
    auto list = [](const std::string& s) {
        std::string r;
        for (char c : s)
            r += std::string(r.empty() ? "" : ", ") + c;
        return r.empty() ? std::string("none") : r;
    };
    mr.f.open     = open;
    mr.f.semiUs   = semiUs;
    mr.f.semiThem = semiThem;
    ss << "--- FILES (" << color_name(us) << ") ---\n";
    ss << "    Open: " << list(open) << " | half-open for " << color_name(us) << ": "
       << list(semiUs) << " | half-open for " << color_name(them) << ": " << list(semiThem)
       << "\n\n";

    if (!open.empty())
        mr.files = "Fight for the open " + list(open) + " file(s) with the Rooks.";
    else if (!semiUs.empty() && !semiThem.empty())
        mr.files = "Exploit the half-open " + list(semiUs)
                 + " file(s) for your Rooks and watch the " + list(semiThem)
                 + " file(s) used by the opponent.";
    else if (!semiUs.empty())
        mr.files = "Exploit the strategic artery of the half-open " + list(semiUs) + " file(s).";
    else if (!semiThem.empty())
        mr.files = "Hinder the opponent's action on the half-open " + list(semiThem) + " file(s).";
    else
        mr.files = "Pawn chains close the main files.";
    return ss.str();
}

std::string analyze_tension_and_breaks(Context& ctx, MasterReportData& mr) {
    std::stringstream ss;
    const Position&   pos  = ctx.pos;
    const Color       us   = ctx.us;
    const Color       them = ctx.them;

    // Structure (same definitions as Branch-Max, with correct pawn spans)
    auto weaknesses = [&](Color c, std::vector<std::string>& iso, std::vector<std::string>& back,
                          std::vector<std::string>& passed) {
        const Bitboard pawns = pos.pieces(c, PAWN), enemy = pos.pieces(~c, PAWN);
        for (Bitboard it = pawns; it;)
        {
            const Square s   = pop_lsb(it);
            const File   f   = file_of(s);
            Bitboard     adj = adjacent_files(f);
            Bitboard     front = forward_file(c, s);
            for (Bitboard a = adj; a;)
                front |= forward_file(c, make_square(file_of(pop_lsb(a)), rank_of(s)));
            if (!(enemy & front))
                passed.push_back(UCIEngine::square(s));
            if (!(pawns & adj))
                iso.push_back(UCIEngine::square(s));
            else
            {
                // backward: all the neighbours are ahead and the file is half-open for the enemy
                bool supported = false;
                for (Bitboard a = pawns & adj; a;)
                    if (relative_rank(c, pop_lsb(a)) <= relative_rank(c, s))
                        supported = true;
                const Square stop = s + (c == WHITE ? NORTH : SOUTH);
                if (!supported && !(enemy & file_bb(f))
                    && (pawn_attacks(~c, enemy) & stop))
                    back.push_back(UCIEngine::square(s));
            }
        }
    };
    std::vector<std::string> us_iso, us_back, us_passed, them_iso, them_back, them_passed;
    weaknesses(us, us_iso, us_back, us_passed);
    weaknesses(them, them_iso, them_back, them_passed);
    mr.f.passed[0]   = us_passed;
    mr.f.passed[1]   = them_passed;
    mr.f.isolated[0] = us_iso;
    mr.f.isolated[1] = them_iso;
    mr.f.backward[0] = us_back;
    mr.f.backward[1] = them_back;

    auto join = [](const std::vector<std::string>& v) {
        std::string r;
        for (const auto& x : v)
            r += x + " ";
        return r;
    };
    ss << "--- DIRECT ACTION: STRUCTURE AND PAWN BREAKS (" << color_name(us) << ") ---\n";
    ss << "[!] YOUR STRUCTURE: passed " << (us_passed.empty() ? "- " : join(us_passed))
       << "| isolated " << (us_iso.empty() ? "- " : join(us_iso)) << "| backward "
       << (us_back.empty() ? "-" : join(us_back)) << "\n";
    ss << "[+] OPPONENT STRUCTURE: passed " << (them_passed.empty() ? "- " : join(them_passed))
       << "| isolated " << (them_iso.empty() ? "- " : join(them_iso)) << "| backward "
       << (them_back.empty() ? "-" : join(them_back)) << "\n";

    // Tension: pawn x pawn captures, compared with the best non-capturing alternative
    Value bestOther = -VALUE_INFINITE;
    for (const auto& c : ctx.cands)
        if (!(c.capture && c.pawnMove && type_of(pos.piece_on(c.m.to_sq())) == PAWN))
        {
            bestOther = c.q;
            break;
        }
    std::vector<const Candidate*> tension, goodBreaks, badBreaks;
    for (const auto& c : ctx.cands)
    {
        if (!c.pawnMove)
            continue;
        if (c.capture && type_of(pos.piece_on(c.m.to_sq())) == PAWN)
            tension.push_back(&c);
        else if (!c.capture
                 && (Attacks::attacks_bb<PAWN>(c.m.to_sq(), us) & pos.pieces(them, PAWN)))
            (c.gain >= 0 ? goodBreaks : badBreaks).push_back(&c);
    }
    if (!tension.empty())
    {
        ss << "--- TENSION MANAGEMENT (Pawns Only) ---\n";
        const Candidate* best = tension.front();
        const int        diff = ctx.cp(best->q - bestOther);
        mr.f.tension        = true;
        mr.f.releaseTension = diff >= 0;
        mr.f.tensionMove    = best->m;
        mr.f.tensionCp      = diff;
        if (diff >= 0)
        {
            ss << "[+] RELEASE TENSION: " << ctx.mv(best->m) << " is at least as good as any "
               << "alternative (" << std::showpos << diff << std::noshowpos << " cp).\n";
            mr.tension = "Resolve the pawn tension by capturing (" + ctx.mv(best->m) + ").";
        }
        else
        {
            std::string sqs;
            for (const auto* t : tension)
                sqs += (sqs.empty() ? "" : ", ") + UCIEngine::square(t->m.from_sq()) + "-"
                     + UCIEngine::square(t->m.to_sq());
            ss << "[-] MAINTAIN TENSION: Do not capture first on " << sqs << " (" << diff
               << " cp versus the best alternative)!\n";
            mr.tension = "Maintain pawn tension on " + sqs + ".";
        }
    }
    else
        mr.tension = "No pawn tension is present in the position.";

    for (const auto* c : goodBreaks)
        mr.f.goodBreaks.push_back(c->m);
    for (const auto* c : badBreaks)
        mr.f.badBreaks.push_back(c->m);
    if (!goodBreaks.empty())
    {
        ss << "[+] RECOMMENDED PUSHES/BREAKS:\n";
        for (size_t i = 0; i < goodBreaks.size() && i < 3; ++i)
            ss << "    * " << ctx.mv(goodBreaks[i]->m) << " (" << ctx.cps(goodBreaks[i]->q)
               << ", gain " << std::showpos << ctx.cp(goodBreaks[i]->gain) << std::noshowpos
               << " cp versus passing)\n";
    }
    else
        ss << "[+] RECOMMENDED PUSHES/BREAKS: No clear break available.\n";
    if (!badBreaks.empty())
    {
        ss << "[-] PREMATURE BREAKS:";
        for (size_t i = 0; i < badBreaks.size() && i < 3; ++i)
            ss << " " << ctx.mv(badBreaks[i]->m) << " (" << ctx.cp(badBreaks[i]->gain) << ")";
        ss << "\n";
    }
    // Shelter pushes that lose a lot
    const Square ksq = pos.square<KING>(us);
    bool         shelterBad = false;
    for (const auto& c : ctx.cands)
        if (c.pawnMove && !c.capture
            && std::abs(int(file_of(c.m.from_sq())) - int(file_of(ksq))) <= 1
            && std::abs(int(rank_of(c.m.from_sq())) - int(rank_of(ksq))) <= 2
            && ctx.cp(c.gain) <= -25)
        {
            if (!shelterBad)
                ss << "[-] PUSHES TO AVOID (king shelter):";
            ss << " " << ctx.mv(c.m) << " (" << ctx.cp(c.gain) << ")";
            shelterBad = true;
            mr.f.shelterBad.push_back(c.m);
        }
    if (shelterBad)
        ss << "\n";

    std::string pw;
    if (!them_passed.empty())
        pw += "The opponent's passed pawn on " + them_passed[0]
            + " is dangerous and must be blockaded. ";
    if (!us_passed.empty())
        pw += "Exploit your passed pawn on " + us_passed[0] + " by supporting its advance. ";
    if (!them_iso.empty() || !them_back.empty())
        pw += "Target the enemy weaknesses ("
            + (them_iso.empty() ? "" : "isolated " + them_iso[0])
            + (them_iso.empty() || them_back.empty() ? "" : ", ")
            + (them_back.empty() ? "" : "backward " + them_back[0]) + "). ";
    if (!us_iso.empty() || !us_back.empty())
        pw += "Defend your vulnerabilities ("
            + (us_iso.empty() ? "" : "isolated " + us_iso[0])
            + (us_iso.empty() || us_back.empty() ? "" : ", ")
            + (us_back.empty() ? "" : "backward " + us_back[0]) + "). ";
    if (!goodBreaks.empty())
        pw += "The plan involves the break " + ctx.mv(goodBreaks.front()->m) + ". ";
    else
        pw += "At the moment no advantageous pawn break is available. ";
    if (shelterBad)
        pw += "Avoid reckless pushes in front of your King.";
    mr.pawns = pw;
    ss << "\n";
    return ss.str();
}

std::string analyze_king_safety(Context& ctx, MasterReportData& mr) {
    std::stringstream ss;
    const Position&   pos = ctx.pos;
    const Color       us = ctx.us, them = ctx.them;
    const Square      ksq = pos.square<KING>(us);
    const File        kf  = file_of(ksq);

    const Bitboard shieldRanks = us == WHITE ? (Rank2BB | Rank3BB) : (Rank7BB | Rank6BB);
    int missing = 0;
    for (int f = std::max(0, int(kf) - 1); f <= std::min(7, int(kf) + 1); ++f)
        missing += !(pos.pieces(us, PAWN) & file_bb(File(f)) & shieldRanks);
    int      ringAttacks = 0;
    Bitboard ring        = Attacks::attacks_bb<KING>(ksq) | ksq;
    while (ring)
        ringAttacks += popcount(pos.attackers_to(pop_lsb(ring)) & pos.pieces(them));

    // NNUE: value of the enemy relations that hit the king zone (attacks on the pieces and
    // pawns around our king), our point of view
    Value    zonePressure = 0;
    Bitboard zone         = Attacks::attacks_bb<KING>(ksq) | ksq;
    for (const auto& t : ctx.insight.threats)
        if ((zone & t.rel.sqB) && color_of(t.rel.pcA) == them)
            zonePressure += us == WHITE ? t.value : -t.value;

    ss << "--- KING SAFETY AND PROPHYLAXIS (" << color_name(us) << ") ---\n";
    ss << "    * Missing Shield Pawns: " << missing << "\n";
    ss << "    * Combined attacks on the King Ring: " << ringAttacks << "\n";
    ss << "    * NNUE value of the enemy relations on the king zone: " << ctx.cps(zonePressure)
       << "\n";

    const Candidate* bestK = nullptr;
    for (const auto& c : ctx.cands)
        if (c.kingMove && (!bestK || c.gain > bestK->gain))
            bestK = &c;
    if (bestK && ctx.cp(bestK->gain) > 5)
        ss << "    -> Recommended prophylaxis: " << ctx.mv(bestK->m) << " (gain " << std::showpos
           << ctx.cp(bestK->gain) << std::noshowpos << " cp versus passing)\n";

    const int danger = missing + (ringAttacks >= 5 ? 2 : ringAttacks >= 3 ? 1 : 0)
                     + (ctx.cp(zonePressure) <= -30 ? 1 : 0);
    mr.f.missingShield  = missing;
    mr.f.ringAttacks    = ringAttacks;
    mr.f.zonePressureCp = ctx.cp(zonePressure);
    mr.f.kingDanger     = danger >= 2;
    if (bestK && ctx.cp(bestK->gain) > 5)
    {
        mr.f.kingMove   = bestK->m;
        mr.f.kingGainCp = ctx.cp(bestK->gain);
    }
    mr.king = danger >= 2 ? "The King is under dangerous pressure."
                          : "The King is safe and shows no imminent weaknesses.";
    if (bestK && ctx.cp(bestK->gain) > 5)
        mr.king += " It might be wise to spend a prophylactic tempo on " + ctx.mv(bestK->m) + ".";
    else if (danger < 2)
        mr.king += " Relocating it is not recommended in this phase.";
    ss << "\n";
    return ss.str();
}

// ============================================================================================
// Natural language helpers
// ============================================================================================
const char* piece_word(PieceType pt) {
    static const char* N[] = {"", "pawn", "knight", "bishop", "rook", "queen", "king"};
    return N[pt];
}

std::string capitalize(std::string s) {
    if (!s.empty())
        s[0] = char(std::toupper(s[0]));
    return s;
}

// "the knight on f3"
std::string piece_on(Piece pc, Square s) {
    return std::string("the ") + piece_word(type_of(pc)) + " on " + UCIEngine::square(s);
}

// Standard algebraic notation of a legal move (Nf3, exd5, O-O, e8=Q+, Qxf7#)
std::string san(Position& pos, Move m) {
    const Square    from = m.from_sq(), to = m.to_sq();
    const PieceType pt   = type_of(pos.piece_on(from));
    std::string     s;
    if (m.type_of() == CASTLING)
        s = to > from ? "O-O" : "O-O-O";
    else
    {
        const bool capture = pos.capture(m);
        if (pt == PAWN)
        {
            if (capture)
                s += char('a' + file_of(from));
        }
        else
        {
            s += "PNBRQK"[pt - 1];
            // Disambiguation
            bool sameFile = false, sameRank = false, ambiguous = false;
            for (const auto& o : MoveList<LEGAL>(pos))
                if (o != m && o.to_sq() == to && o.from_sq() != from
                    && type_of(pos.piece_on(o.from_sq())) == pt)
                {
                    ambiguous = true;
                    sameFile |= file_of(o.from_sq()) == file_of(from);
                    sameRank |= rank_of(o.from_sq()) == rank_of(from);
                }
            if (ambiguous)
            {
                if (!sameFile)
                    s += char('a' + file_of(from));
                else if (!sameRank)
                    s += char('1' + rank_of(from));
                else
                    s += UCIEngine::square(from);
            }
        }
        if (capture)
            s += 'x';
        s += UCIEngine::square(to);
        if (m.type_of() == PROMOTION)
            s += std::string("=") + "PNBRQK"[m.promotion_type() - 1];
    }
    if (pos.gives_check(m))
    {
        StateInfo st;
        pos.do_move(m, st);
        const bool mate = MoveList<LEGAL>(pos).size() == 0;
        pos.undo_move(m);
        s += mate ? '#' : '+';
    }
    return s;
}

std::string join_words(const std::vector<std::string>& v, const std::string& last = " and ") {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i)
        s += (i == 0 ? "" : i + 1 == v.size() ? last : ", ") + v[i];
    return s;
}

std::string join_all(const std::vector<std::string>& v, const std::string& sep) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i)
        s += (i ? sep : "") + v[i];
    return s;
}

// "the bishop on g5 attacks the knight on f6"
std::string relation_sentence(const Eval::NNUE::Explain::Relation& r) {
    const bool same = color_of(r.pcA) == color_of(r.pcB);
    return piece_on(r.pcA, r.sqA) + (same ? " defends " : " attacks ") + piece_on(r.pcB, r.sqB);
}

// Signed pawns with two decimals: "+0.35"
std::string pawns_str(const Position& pos, Value v) {
    std::stringstream ss;
    ss << std::showpos << std::fixed << std::setprecision(2) << 0.01 * UCIEngine::to_cp(v, pos);
    return ss.str();
}

// ============================================================================================
// NNUE explanation of a move: how the activity of the pieces changes, and the most valuable
// new relation of the moved piece (both computed with the explanation engine)
// ============================================================================================
struct ActivityChange {
    Value  delta;  // mover's point of view
    Piece  pc;
    Square sq;
    bool   own;
};

struct MoveExplanation {
    bool                          castling = false;
    bool                          kingMove = false;
    PieceType                     promotion = NO_PIECE_TYPE;
    Piece                         moved    = NO_PIECE;
    Square                        to       = SQ_NONE;
    Value                         movedDelta = 0;
    std::vector<ActivityChange>   others;  // sorted by |delta|
    bool                          hasRelation = false;
    Eval::NNUE::Explain::Relation relation{};
    Value                         relationValue = 0;
};

MoveExplanation explain_move_data(Context& ctx, Move m) {
    using namespace Eval::NNUE;
    using namespace Eval::NNUE::Explain;
    MoveExplanation e;
    if (m.type_of() == CASTLING)
    {
        e.castling = true;
        return e;
    }

    Explainer   ex(ctx.net);
    const Board before = ctx.pos.piece_array();
    StateInfo   st;
    ctx.pos.do_move(m, st);
    const Board after = ctx.pos.piece_array();
    ctx.pos.undo_move(m);

    // Activity of every piece (value of its attack/defence relations, owner to move, natural
    // bucket, owner's point of view): unlike the ablation values, the pieces stay on the board,
    // so the comparison before/after the move is not distorted by the saturation of the network
    auto activities = [&](const Board& b) {
        std::array<Value, SQUARE_NB> v{};
        const int                    bk   = natural_bucket(b);
        const FeatureSet             fs   = Explainer::collect(b);
        const auto                   full = std::make_unique<WideAccumulator>(ex.accumulate(fs));
        for (Color o : {WHITE, BLACK})
        {
            const Value ob = ex.evaluate_white(*full, o, bk).total();
            for (Square s = SQ_A1; s <= SQ_H8; ++s)
            {
                if (b[s] == NO_PIECE || color_of(b[s]) != o || type_of(b[s]) == KING)
                    continue;
                std::vector<const Relation*> rs;
                for (const auto& r : fs.relations)
                    if (r.kind == REL_THREAT && (r.sqA == s || r.sqB == s))
                        rs.push_back(&r);
                for (auto* r : rs)
                    ex.add_relation(*full, *r, -1);
                const Value w = ex.evaluate_white(*full, o, bk).total();
                for (auto* r : rs)
                    ex.add_relation(*full, *r, +1);
                v[s] = o == WHITE ? ob - w : w - ob;
            }
        }
        return v;
    };
    const auto  vb = activities(before);
    const auto  va = activities(after);
    const Color us = ctx.us;

    const Square from = m.from_sq();
    if (m.type_of() == PROMOTION)
        e.promotion = m.promotion_type();
    e.to        = m.to_sq();
    e.moved     = after[e.to];
    e.kingMove  = type_of(e.moved) == KING;
    if (!e.kingMove)
        e.movedDelta = va[e.to] - vb[from];
    for (Square s = SQ_A1; s <= SQ_H8; ++s)
    {
        if (s == from || s == e.to || after[s] == NO_PIECE || after[s] != before[s]
            || type_of(after[s]) == KING || type_of(after[s]) == PAWN)
            continue;
        const bool own = color_of(after[s]) == us;
        e.others.push_back({(own ? 1 : -1) * (va[s] - vb[s]), after[s], s, own});
    }
    std::sort(e.others.begin(), e.others.end(),
              [](auto& a, auto& b) { return std::abs(a.delta) > std::abs(b.delta); });

    // Most valuable relation of the moved piece after the move (occlusion, mover's view)
    if (!e.kingMove)
    {
        const int        bk   = natural_bucket(after);
        const FeatureSet fs   = Explainer::collect(after);
        auto             full = std::make_unique<WideAccumulator>(ex.accumulate(fs));
        const Color      stm  = ~us;
        const Value      base = ex.evaluate_white(*full, stm, bk).total();
        for (const auto& r : fs.relations)
            if (r.kind == REL_THREAT && r.sqA == e.to)
            {
                ex.add_relation(*full, r, -1);
                Value v =
                  (us == WHITE ? 1 : -1) * (base - ex.evaluate_white(*full, stm, bk).total());
                ex.add_relation(*full, r, +1);
                if (v > e.relationValue && ctx.cp(v) >= 5)
                {
                    e.relationValue = v;
                    e.relation      = r;
                    e.hasRelation   = true;
                }
            }
    }
    return e;
}

// Technical one-line form, used in the GAME PLANS list
std::string explanation_line(const Context& ctx, const MoveExplanation& e) {
    if (e.castling)
        return "castling: king safety and rook activation";
    std::stringstream ss;
    int               n = 0;
    auto add = [&](Value d, const std::string& what) {
        if (n < 3 && std::abs(ctx.cp(d)) >= 3)
            ss << (n++ ? ", " : "") << what << " " << std::showpos << ctx.cp(d) << std::noshowpos;
    };
    std::vector<std::pair<Value, std::string>> items;
    if (!e.kingMove)
        items.emplace_back(e.movedDelta, piece_label(e.moved, e.to) + " activity");
    for (const auto& o : e.others)
        items.emplace_back(o.delta, (o.own ? "own " : "enemy ") + piece_label(o.pc, o.sq));
    std::stable_sort(items.begin(), items.end(),
                     [](auto& a, auto& b) { return std::abs(a.first) > std::abs(b.first); });
    for (const auto& [d, what] : items)
        add(d, what);
    if (e.hasRelation)
        ss << (n ? "; " : "") << "new key relation " << Eval::NNUE::relation_label(e.relation)
           << " " << std::showpos << ctx.cp(e.relationValue) << std::noshowpos;
    if (ss.str().empty())
        ss << (e.kingMove ? "king move: no significant change in the piece relations"
                          : "small improvements spread over many features");
    return ss.str() + " (NNUE activity changes, cp, mover's view)";
}

// Natural language form: "it activates the queen, improves your knight on c3 and restrains the
// enemy bishop on c8; afterwards the queen on c2 defends the knight on c3"
std::string explanation_sentence(const Context& ctx, const MoveExplanation& e) {
    if (e.castling)
        return "it brings the king to safety and connects the rooks";
    std::vector<std::string> pros, cons;
    const std::string        moved = piece_word(type_of(e.moved));
    if (e.promotion != NO_PIECE_TYPE)
        pros.push_back(std::string("promotes the pawn to a ") + piece_word(e.promotion));
    else if (!e.kingMove && ctx.cp(e.movedDelta) >= 8)
        pros.push_back(type_of(e.moved) == PAWN ? "makes the pawn more active"
                                                : "activates the " + moved);
    else if (!e.kingMove && ctx.cp(e.movedDelta) <= -8)
        cons.push_back("a less active " + moved);
    for (const auto& o : e.others)
    {
        if (std::abs(ctx.cp(o.delta)) < 8)
            continue;
        const std::string what = std::string(piece_word(type_of(o.pc))) + " on " + UCIEngine::square(o.sq);
        if (o.delta > 0 && pros.size() < 3)
            pros.push_back(o.own ? "improves your " + what : "restrains the enemy " + what);
        else if (o.delta < 0 && cons.size() < 2)
            cons.push_back(o.own ? "a slightly worse " + what : "a freer enemy " + what);
    }
    std::string s;
    if (!pros.empty())
        s = "it " + join_words(pros);
    if (e.hasRelation)
        s += (s.empty() ? "afterwards " : "; afterwards ") + relation_sentence(e.relation);
    if (s.empty())
        s = e.kingMove ? "it improves the king's position"
                       : "it makes a small all-round improvement";
    if (!cons.empty())
        s += " (at the cost of " + join_words(cons) + ")";
    return s;
}

std::string analyze_game_plans(Context& ctx, MasterReportData& mr) {
    std::stringstream ss;
    ss << "--- GAME PLANS AND PIECE MANEUVERS (" << color_name(ctx.us) << ") ---\n";
    ss << "  Top candidates (quiet value; gain = cp gained versus passing; NNUE explanation):\n";
    if (ctx.cands.empty())
    {
        ss << "  * No legal moves.\n\n";
        return ss.str();
    }
    const int topN = std::min<int>(6, int(ctx.cands.size()));
    for (int i = 0; i < topN; ++i)
    {
        const auto& c = ctx.cands[i];
        ss << "  " << i + 1 << ". " << std::left << std::setw(6) << ctx.mv(c.m) << std::right << " "
           << std::setw(9) << ctx.cps(c.q) << " (" << std::setw(2) << c.wp << "%)  gain "
           << (is_mate(c.q) ? std::string("  -") : ctx.cps(c.gain));
        std::string tags;
        if (c.m.type_of() == PROMOTION)
            tags += " promotion";
        if (c.capture && ctx.pos.capture(c.m))
            tags += " capture";
        if (c.check)
            tags += " check";
        if (c.kingMove)
            tags += " king";
        if (c.sacrifice)
            tags += " SACRIFICE(SEE<0)";
        if (!tags.empty())
            ss << " [" << tags.substr(1) << "]";
        ss << "\n     why: " << explanation_line(ctx, explain_move_data(ctx, c.m)) << "\n";
    }
    const auto& b = ctx.cands.front();
    mr.plan = "The most promising continuation is " + ctx.mv(b.m) + " (" + ctx.cps(b.q) + ")"
            + (ctx.cands.size() > 1
                 ? ", followed by " + ctx.mv(ctx.cands[1].m)
                     + (ctx.cands.size() > 2 ? " and " + ctx.mv(ctx.cands[2].m) : "")
                     + " as alternatives."
                 : ".");
    ss << "\n";
    return ss.str();
}

// ============================================================================================
// THE MASTER'S VERDICT: high-level synthesis in natural language
//
// Built only from the structured facts (Context, NNUE insight, Facts), never from the text of
// the sections above, so that every combination of cases (checkmate, stalemate, check, mate
// available, tactics, threats, material imbalances, endgames without pieces, ...) produces
// coherent sentences. Addressed to the side to move ("you"), naming the colors explicitly.
// ============================================================================================

// SAN of a move of the opponent, played after a pass of the side to move
std::string san_after_null(Context& ctx, Move m) {
    if (!m || ctx.inCheck)
        return ctx.mv(m);
    StateInfo st;
    ctx.pos.do_null_move(st);
    std::string s = san(ctx.pos, m);
    ctx.pos.undo_null_move();
    return s;
}


// Material balance in words, from the side to move's point of view
std::string material_sentence(const Position& pos, Color us) {
    const Color them = ~us;
    int         d[PIECE_TYPE_NB]{};
    for (PieceType pt : {PAWN, KNIGHT, BISHOP, ROOK, QUEEN})
        d[pt] = popcount(pos.pieces(us, pt)) - popcount(pos.pieces(them, pt));
    const int points = d[PAWN] + 3 * (d[KNIGHT] + d[BISHOP]) + 5 * d[ROOK] + 9 * d[QUEEN];

    auto describe = [&](int sign) {
        std::vector<std::string> v;
        for (PieceType pt : {QUEEN, ROOK, BISHOP, KNIGHT, PAWN})
        {
            const int n = sign * d[pt];
            if (n == 1)
                v.push_back(std::string("a ") + piece_word(pt));
            else if (n > 1)
                v.push_back(std::to_string(n) + " " + piece_word(pt) + "s");
        }
        return join_words(v);
    };
    const std::string ours = describe(+1), theirs = describe(-1);
    if (ours.empty() && theirs.empty())
        return "Material is level.";
    auto pawnsWord = [](int n) {
        return n == 1 ? std::string("a pawn") : std::to_string(n) + " pawns";
    };
    if (d[ROOK] == 1 && d[KNIGHT] + d[BISHOP] == -1 && !d[QUEEN] && std::abs(d[PAWN]) <= 2)
        return "You are the exchange up"
             + (d[PAWN] < 0 ? " for " + pawnsWord(-d[PAWN]) : d[PAWN] > 0 ? " and " + pawnsWord(d[PAWN]) + " up" : std::string())
             + ".";
    if (d[ROOK] == -1 && d[KNIGHT] + d[BISHOP] == 1 && !d[QUEEN] && std::abs(d[PAWN]) <= 2)
        return "You are the exchange down"
             + (d[PAWN] > 0 ? ", with " + pawnsWord(d[PAWN]) + " as compensation"
                : d[PAWN] < 0 ? " and " + pawnsWord(-d[PAWN]) + " down" : std::string())
             + ".";
    if (theirs.empty())
        return "You are " + ours + " up (" + std::to_string(points) + " in classical points).";
    if (ours.empty())
        return "You are " + theirs + " down (" + std::to_string(-points) + " in classical points).";
    return "You have " + ours + " for " + theirs + " ("
         + (points > 0   ? "+" + std::to_string(points)
            : points < 0 ? std::to_string(points)
                         : "even")
         + " in classical points): a material imbalance.";
}

std::string compose_verdict(Context&                ctx,
                            const MasterReportData& mr,
                            uint8_t                 wpWhite,
                            const WDLModel::WDL&    wdl,
                            Value                   v_white,
                            int                     complexity) {
    std::stringstream ss;
    Position&         pos      = ctx.pos;
    const Facts&      f        = mr.f;
    const Color       us       = ctx.us;
    const Color       them     = ctx.them;
    const std::string Us       = color_name(us);
    const std::string Them     = color_name(them);
    const int         wpUs     = us == WHITE ? wpWhite : 100 - wpWhite;
    const Value       vUs      = us == WHITE ? v_white : -v_white;
    const auto&       in       = ctx.insight;
    const int         bucket   = in.bucket;
    const bool        endgame  = bucket <= 3 || !pos.pieces(QUEEN);
    const bool        noPieces = !(pos.pieces() & ~pos.pieces(PAWN, KING));

    ss << "================================================================\n";
    ss << ">>> COMPLETE HUMAN EVALUATION (The Master's Verdict) <<<\n";
    ss << "Seen from " << Us << "'s side: " << Us << " is to move.\n\n";

    // ---- Terminal positions ------------------------------------------------------------------
    if (ctx.cands.empty())
    {
        if (ctx.inCheck)
            ss << "CHECKMATE: " << Us << " is mated, " << Them << " has won the game.\n";
        else
            ss << "STALEMATE: " << Us << " has no legal move and is not in check: the game is "
                  "drawn.\n";
        ss << "================================================================\n";
        return ss.str();
    }

    const Candidate& best    = ctx.cands.front();
    // The pawn tension is still a topic only if the main move does not change it
    const bool keepTension = f.tension && !f.releaseTension && f.tensionCp <= -25
                          && best.m.from_sq() != f.tensionMove.from_sq()
                          && !(best.capture && type_of(pos.piece_on(best.m.from_sq())) == PAWN);
    const bool        mateFor = is_mate(best.q) && best.q > 0;
    const bool        mated   = is_mate(best.q) && best.q < 0;
    const std::string bestSan = san(pos, best.m);

    // ---- Overall evaluation ------------------------------------------------------------------
    const bool kingPhase = noPieces
                        || (endgame && !pos.pieces(QUEEN)
                            && popcount(pos.pieces() & ~pos.pieces(PAWN, KING)) <= 4);
    // The value used for the assessment: after an immediate tactic the quiet value is the real
    // one, the static evaluation is only the starting point
    const bool  tacticalWin = !mateFor && ctx.tactical && ctx.quietUs > ctx.staticUs
                           && ctx.rootTactic && !ctx.inCheck;
    const Value vAssess = tacticalWin ? ctx.quietUs : vUs;
    const Value vAssessWhite = us == WHITE ? vAssess : -vAssess;

    ss << "OVERALL EVALUATION:\n";
    if (mateFor || mated)
    {
        ss << (mateFor ? Us + " has a forced mate (" + bestSan + "): the game is decided."
                       : Us + " cannot avoid mate: the position is lost.")
           << "\n\n";
        ss << "THE PLAN:\n  1. "
           << (mateFor ? "Deliver mate: " + bestSan + "." : "Prolong the resistance with " + bestSan + ".")
           << "\n\nIN A NUTSHELL:\n"
           << (mateFor ? Us + " mates with " + bestSan + "." : Us + " is getting mated.") << "\n";
        ss << "================================================================\n";
        return ss.str();
    }
    {
        const int   cpA    = ctx.cp(vAssess);
        const int   a      = std::abs(cpA);
        const std::string leader = cpA >= 0 ? Us : Them;
        const int         wpA    = WDLModel::get_win_probability(vAssessWhite, pos);
        const int         wpLead = cpA >= 0 ? (us == WHITE ? wpA : 100 - wpA)
                                            : (us == WHITE ? 100 - wpA : wpA);
        if (tacticalWin)
            ss << "The static evaluation (" << pawns_str(pos, v_white)
               << " for White) does not tell the whole story: after the tactic "
               << san(pos, ctx.rootTactic) << ", ";
        if (a < 20)
            ss << (tacticalWin ? "the" : "The") << " position is about equal (evaluation "
               << pawns_str(pos, vAssessWhite) << " from White's point of view, expected score "
               << wpA << "% for White). ";
        else
            ss << leader
               << (a < 50    ? " is slightly better"
                   : a < 100 ? " is better"
                   : a < 200 ? " is clearly better"
                   : a < 400 ? " has a large advantage"
                             : " is winning")
               << " (evaluation " << pawns_str(pos, vAssessWhite)
               << " from White's point of view, expected score " << wpLead << "% for " << leader
               << "). ";
        ss << material_sentence(pos, us) << " ";

        if (noPieces)
            ss << "It is a pure king and pawn ending: count the tempi and use the opposition. ";
        else if (bucket <= 2)
            ss << "In this endgame technique and king activity matter most. ";
        else
            ss << (complexity > 250  ? "The position is sharp: concrete calculation matters more "
                                       "than general principles. "
                   : complexity < 75 ? "The position is quiet and suited to maneuvering: "
                                       "long-term factors decide. "
                                     : "The position mixes tactics and strategy: plan, but check "
                                       "every move concretely. ");
        const int thr = ctx.cp(in.familyThreat), pp = ctx.cp(in.familyPair);
        if (std::abs(thr) >= 10 || std::abs(pp) >= 10)
            ss << "The network's judgement rests mainly on "
               << (std::abs(thr) >= std::abs(pp) ? "the activity and coordination of the pieces"
                                                 : "the pawn structure")
               << " (piece relations " << std::showpos << thr << " cp, pawn structure " << pp
               << " cp, White's view)" << std::noshowpos << ". ";
        ss << "Phase: "
           << (bucket >= 7   ? "opening"
               : bucket >= 5 ? "middlegame"
               : bucket >= 4 ? "late middlegame"
               : bucket >= 2 ? "endgame"
                             : "late endgame")
           << ". ";
        if (wdl.draw >= 70 && a < 100 && !tacticalWin)
            ss << "A draw is the most likely result (" << int(wdl.draw)
               << "%): to play for a win, someone must take calculated risks. ";
        if (wpUs >= 85)
            ss << "With accurate play the advantage should be decisive: convert it without "
                  "allowing counterplay.";
        else if (wpUs <= 15)
            ss << "The position is very difficult: look for complications, active counterplay or "
                  "a fortress.";
        else
            ss << "Shashin's advice: " << mr.phase_advice;
        ss << "\n\n";
    }

    // ---- Urgent matters ----------------------------------------------------------------------
    ss << "URGENT MATTERS:\n";
    {
        std::vector<std::string> u;
        if (ctx.inCheck)
            u.push_back("You are in check: the only question is how to answer it. The best reply "
                        "is " + bestSan + ".");
        if (mateFor)
            u.push_back("There is a forced mate: " + bestSan + "!");
        else if (mated)
            u.push_back("Every move loses to a mating attack: the position is lost.");
        else if (tacticalWin)
            u.push_back("There is a tactic to exploit: " + san(pos, ctx.rootTactic)
                        + " wins material (about " + pawns_str(pos, ctx.quietUs - ctx.staticUs)
                        + ").");
        if (f.threat)
            u.push_back(Them + " threatens " + san_after_null(ctx, f.threatMove)
                        + (f.threatMate ? ", with mate"
                                        : ", which would cost about " + std::to_string(f.threatCp)
                                            + " cp")
                        + ": deal with it first.");
        if (best.sacrifice && !mateFor && !ctx.inCheck)
            u.push_back("The engine's favourite " + bestSan
                        + " is a sacrifice (it loses material by exchange count, but the "
                          "resulting position is judged good): calculate it carefully.");
        if (u.empty())
            u.push_back("Nothing urgent: neither side has an immediate tactic, so there is time to "
                        "improve the position.");
        ss << join_all(u, " ") << "\n\n";
    }

    // ---- Pieces --------------------------------------------------------------------------------
    ss << "PIECE EVALUATION:\n";
    {
        const Eval::NNUE::PieceInsight *ub = nullptr, *uw = nullptr, *tb = nullptr, *tw = nullptr;
        std::vector<const Eval::NNUE::PieceInsight*> blindUs, blindThem;
        for (const auto& p : in.pieces)
        {
            if (type_of(p.pc) == PAWN)
                continue;
            const bool mine = color_of(p.pc) == us;
            auto&      b    = mine ? ub : tb;
            auto&      w    = mine ? uw : tw;
            if (!b || p.activity > b->activity)
                b = &p;
            if (!w || p.activity < w->activity)
                w = &p;
            if (p.single < 0)
                (mine ? blindUs : blindThem).push_back(&p);
        }
        if (noPieces)
            ss << "Only kings and pawns are left: the king is now a fighting piece, and the race "
                  "of the passed pawns (or the opposition) decides. ";
        else
        {
            if (ub && uw && ub != uw)
                ss << "Your most active piece is " << piece_on(ub->pc, ub->sq)
                   << "; your least active is " << piece_on(uw->pc, uw->sq)
                   << ": improving it is the first positional task (Makogonov's principle). ";
            else if (ub)
                ss << "Your only piece, " << piece_on(ub->pc, ub->sq)
                   << ", must be kept as active as possible. ";
            else
                ss << "You have no pieces left besides the king and pawns. ";
            if (tb && tw && tb != tw)
                ss << "On the other side, " << piece_on(tw->pc, tw->sq) << " is " << Them
                   << "'s problem piece, while " << piece_on(tb->pc, tb->sq)
                   << " is its most active unit. ";
            for (const auto* p : blindUs)
                ss << "The network would even prefer you without " << piece_on(p->pc, p->sq)
                   << ": it is badly placed, tied down or in the way. ";
            for (const auto* p : blindThem)
                ss << "Likewise " << piece_on(p->pc, p->sq) << " is a burden for " << Them
                   << ". ";
        }
        const int ubish = popcount(pos.pieces(us, BISHOP)), tbish = popcount(pos.pieces(them, BISHOP));
        if (ubish >= 2 && tbish < 2)
            ss << "You have the bishop pair: open the position to make it count. ";
        else if (tbish >= 2 && ubish < 2)
            ss << Them << " has the bishop pair: keep the position closed and trade a bishop if "
                          "possible. ";
        if (ubish == 1 && tbish == 1)
        {
            auto light = [](Square s) { return (int(file_of(s)) + int(rank_of(s))) % 2 != 0; };
            if (light(lsb(pos.pieces(us, BISHOP))) != light(lsb(pos.pieces(them, BISHOP))))
                ss << "The bishops are of opposite colours: this favours the attacker while "
                      "other pieces remain, but makes the endgame drawish. ";
        }
        ss << "\n\n";
    }

    // ---- Trades --------------------------------------------------------------------------------
    ss << "TRADES AND SIMPLIFICATION:\n";
    if (noPieces)
        ss << "No pieces are left to trade: every pawn exchange changes the nature of the "
              "ending, so calculate them precisely.\n\n";
    else
    {
        if (!pos.pieces(QUEEN))
            ss << "The queens are off: king activity and the pawn structure gain weight. ";
        else if (f.hasQueenTrade)
            ss << (f.queenTrade > 10   ? "A queen trade favours you (about +" + std::to_string(f.queenTrade) + " cp): welcome it. "
                   : f.queenTrade < -10 ? "Avoid the queen trade: it would cost about " + std::to_string(-f.queenTrade) + " cp. "
                                        : "A queen trade would not change the balance much. ");
        else
            ss << "Only one side has a queen. ";
        if (f.hasRookTrade)
            ss << (f.rookTrade > 10    ? "Trading all the rooks would help you. "
                   : f.rookTrade < -10 ? "Keep at least one pair of rooks: trading them all helps " + Them + ". "
                                       : "");
        if (!f.goodTrades.empty())
            ss << "The most useful exchange is " << f.goodTrades.front().first << " (+"
               << f.goodTrades.front().second << " cp). ";
        if (!f.badTrades.empty())
            ss << "Do not give " << f.badTrades.front().first << " ("
               << f.badTrades.front().second << " cp). ";
        int points = 0;
        for (PieceType pt : {PAWN, KNIGHT, BISHOP, ROOK, QUEEN})
            points += (pt == PAWN ? 1 : pt == ROOK ? 5 : pt == QUEEN ? 9 : 3)
                    * (popcount(pos.pieces(us, pt)) - popcount(pos.pieces(them, pt)));
        if (points >= 2)
            ss << "Being ahead in material, trade pieces (not pawns) to approach a won endgame.";
        else if (points <= -2)
            ss << "Being behind in material, avoid trades and keep pieces for counterplay.";
        else if (f.spaceZone == "-")
            ss << "Being cramped, piece trades will relieve the pressure.";
        else if (!f.spaceZone.empty())
            ss << "With more space, avoid unnecessary trades: they would free the opponent.";
        ss << "\n\n";
    }

    // ---- Structure -----------------------------------------------------------------------------
    ss << "GEOMETRY AND STRUCTURE:\n";
    {
        // Pawns
        ss << "Pawns: ";
        std::vector<std::string> p;
        if (!f.passed[0].empty())
            p.push_back("your passed pawn on " + join_words(f.passed[0])
                        + " is an asset: support its advance"
                        + (endgame ? " (in the endgame it is often decisive)" : ""));
        if (!f.passed[1].empty())
            p.push_back(Them + "'s passed pawn on " + join_words(f.passed[1])
                        + " must be blockaded, ideally by a knight");
        // Weak pawns (a passed pawn is listed as passed, not as weak)
        auto weak = [&](int side) {
            std::vector<std::string> w;
            auto passed = [&](const std::string& x) {
                return std::find(f.passed[side].begin(), f.passed[side].end(), x)
                    != f.passed[side].end();
            };
            for (auto& x : f.isolated[side])
                if (!passed(x))
                    w.push_back("isolated " + x);
            for (auto& x : f.backward[side])
                if (!passed(x))
                    w.push_back("backward " + x);
            return w;
        };
        if (!weak(0).empty())
            p.push_back("defend your weak pawns (" + join_words(weak(0)) + ") or trade them off");
        if (!weak(1).empty())
            p.push_back("target the enemy weaknesses (" + join_words(weak(1))
                        + "), blockading and piling up on them");
        if (p.empty())
            p.push_back("both structures are healthy, without passed or weak pawns");
        if (f.tension)
            p.push_back(f.releaseTension ? "resolving the pawn tension with " + san(pos, f.tensionMove)
                                             + " is fine"
                        : f.tensionCp > -25 ? "there is no hurry to resolve the pawn tension with "
                                                + san(pos, f.tensionMove)
                                            : "keep the pawn tension: capturing first with "
                                                + san(pos, f.tensionMove) + " would help the opponent");
        if (!f.goodBreaks.empty())
            p.push_back("the pawn break " + san(pos, f.goodBreaks.front()) + " is playable now");
        else if (!f.badBreaks.empty())
            p.push_back("the break " + san(pos, f.badBreaks.front())
                        + " is premature: prepare it first"
                        + (!f.shelterBad.empty() && f.shelterBad.front() == f.badBreaks.front()
                             ? " (it would also weaken your king)"
                             : ""));
        if (!f.shelterBad.empty()
            && (f.badBreaks.empty() || f.badBreaks.front() != f.shelterBad.front()
                || !f.goodBreaks.empty()))
            p.push_back("do not push the pawns in front of your king (" + san(pos, f.shelterBad.front())
                        + " would weaken it)");
        ss << capitalize(join_all(p, "; ")) << ".\n";

        // Space and files
        ss << "Space and files: ";
        if (f.spaceZone == "-")
            ss << Them << " controls more space; ";
        else if (!f.spaceZone.empty())
            ss << "you have more space on the " << f.spaceZone
               << (f.spaceKeyPawn != SQ_NONE ? " (spearheaded by the pawn on " + UCIEngine::square(f.spaceKeyPawn) + ")" : "")
               << ": play there; ";
        else
            ss << "space is balanced; ";
        auto files = [](const std::string& s) {
            std::vector<std::string> v;
            for (char c : s)
                v.push_back(std::string(1, c));
            return join_words(v);
        };
        const bool ourHeavy = bool(pos.pieces(us, ROOK, QUEEN));
        const bool theirHeavy = bool(pos.pieces(them, ROOK, QUEEN));
        if (!ourHeavy)
            ss << "the files matter little without rooks and queens";
        else if (!f.open.empty())
            ss << "the open " << files(f.open) << "-file" << (f.open.size() > 1 ? "s are" : " is")
               << " the main road for the rooks";
        else if (!f.semiUs.empty())
            ss << "your rooks belong on the half-open " << files(f.semiUs) << "-file"
               << (f.semiUs.size() > 1 ? "s" : "");
        else
            ss << "the files are closed: the rooks need a pawn break to come into play";
        if (!f.semiThem.empty() && theirHeavy)
            ss << ", while " << Them << " will use the " << files(f.semiThem) << "-file"
               << (f.semiThem.size() > 1 ? "s" : "");
        ss << ".\n";

        // Squares
        ss << "Key squares: ";
        std::vector<std::string> k;
        if (!f.outposts.empty())
        {
            std::vector<std::string> sq;
            for (size_t i = 0; i < f.outposts.size() && i < 3; ++i)
                sq.push_back(UCIEngine::square(f.outposts[i]));
            k.push_back("the outpost" + std::string(sq.size() > 1 ? "s " : " ") + join_words(sq)
                        + " would suit a knight");
        }
        if (f.favSquare != SQ_NONE && !noPieces)
            k.push_back("the network would most like a " + std::string(piece_word(f.favType))
                        + " on " + UCIEngine::square(f.favSquare));
        if (f.lightDom)
            k.push_back("you dominate the light squares (only you have a light-squared bishop)");
        if (f.darkDom)
            k.push_back("you dominate the dark squares (only you have a dark-squared bishop)");
        Value  hot = 0;
        Square hs  = SQ_NONE;
        for (Square s = SQ_A1; s <= SQ_H8; ++s)
            if (std::abs(in.pressure[s]) > std::abs(hot))
            {
                hot = in.pressure[s];
                hs  = s;
            }
        if (hs != SQ_NONE && std::abs(ctx.cp(hot)) >= 10)
            k.push_back("the fight revolves around " + UCIEngine::square(hs) + " (pressure in "
                        + ((hot > 0) == (us == WHITE) ? "your" : Them + "'s") + " favour)");
        if (k.empty())
            k.push_back("no square is critical yet");
        ss << capitalize(join_words(k, "; ")) << ".\n\n";
    }

    // ---- King safety ---------------------------------------------------------------------------
    ss << "KING SAFETY:\n";
    {
        if (kingPhase)
            ss << "With few pieces left the king is no longer a target but an active piece: "
                  "centralize it and use it to support the pawns. ";
        else if (f.kingDanger)
            ss << "Your king is under dangerous pressure (" << f.missingShield
               << " missing shield pawn" << (f.missingShield == 1 ? "" : "s") << ", "
               << f.ringAttacks << " attacks on the squares around it): defence comes first. ";
        else
            ss << "Your king is safe for now. ";
        if (f.kingMove && !noPieces)
            ss << (f.kingMove.type_of() == CASTLING
                     ? "Castling (" + san(pos, f.kingMove) + ") is useful. "
                     : "A prophylactic king move such as " + san(pos, f.kingMove) + " is useful. ");

        // Opponent's king: missing shield pawns and our pressure on its zone
        const Square   ek          = pos.square<KING>(them);
        const Bitboard shieldRanks = them == WHITE ? (Rank2BB | Rank3BB) : (Rank7BB | Rank6BB);
        int            missing     = 0;
        for (int fl = std::max(0, int(file_of(ek)) - 1); fl <= std::min(7, int(file_of(ek)) + 1); ++fl)
            missing += !(pos.pieces(them, PAWN) & file_bb(File(fl)) & shieldRanks);
        Value    press = 0;
        Bitboard zone  = Attacks::attacks_bb<KING>(ek) | ek;
        for (const auto& t : in.threats)
            if ((zone & t.rel.sqB) && color_of(t.rel.pcA) == us)
                press += us == WHITE ? t.value : -t.value;
        if (!kingPhase && pos.pieces(us, QUEEN) && (missing >= 2 || ctx.cp(press) >= 30))
            ss << "The enemy king is exposed (" << missing << " open or missing shield file"
               << (missing == 1 ? "" : "s") << "): bring more pieces towards it and keep the "
                                               "queens on.";
        else if (!kingPhase)
            ss << "The enemy king is well protected: a direct attack is not justified yet.";
        ss << "\n\n";
    }

    // ---- The plan ------------------------------------------------------------------------------
    ss << "THE PLAN:\n";
    {
        std::vector<std::string> steps;
        const MoveExplanation    e = explain_move_data(ctx, best.m);
        if (mateFor)
            steps.push_back("Deliver mate: " + bestSan + ".");
        else
        {
            const std::string after =
              " (your evaluation afterwards: " + pawns_str(pos, best.q) + "): "
              + explanation_sentence(ctx, e) + ".";
            if (ctx.inCheck)
                steps.push_back("Get out of check with " + bestSan + after);
            else if (f.threat)
                steps.push_back("Parry the threat " + san_after_null(ctx, f.threatMove)
                                + "; the best way is " + bestSan + after);
            else
                steps.push_back("Play " + bestSan + after);
            std::vector<std::string> alts;
            for (size_t i = 1; i < ctx.cands.size() && alts.size() < 2; ++i)
                if (ctx.cp(best.q - ctx.cands[i].q) <= 25 && !is_mate(ctx.cands[i].q))
                    alts.push_back(san(pos, ctx.cands[i].m));
            if (!alts.empty())
                steps.push_back("Good alternatives: " + join_words(alts, " or ") + ".");

            // Improve the least active piece
            const Eval::NNUE::PieceInsight* uw = nullptr;
            for (const auto& p : in.pieces)
                if (color_of(p.pc) == us && type_of(p.pc) != PAWN
                    && (!uw || p.activity < uw->activity))
                    uw = &p;
            if (uw && !(best.m.from_sq() == uw->sq))
                steps.push_back("Then improve " + piece_on(uw->pc, uw->sq)
                                + (f.favSquare != SQ_NONE && f.favType == type_of(uw->pc)
                                     ? ", ideally towards " + UCIEngine::square(f.favSquare)
                                     : "")
                                + ".");

            if (!f.goodBreaks.empty() && f.goodBreaks.front() != best.m)
                steps.push_back("Consider the pawn break " + san(pos, f.goodBreaks.front()) + ".");
            else if (keepTension)
                steps.push_back("Keep the pawn tension until it can be resolved favourably.");

            if (f.oppIdea && !ctx.inCheck)
                steps.push_back("Prophylaxis: watch " + Them + "'s idea "
                                + san_after_null(ctx, f.oppIdea) + ", its most useful move if "
                                + "it had a free tempo.");
            if (endgame && !f.passed[0].empty()
                && UCIEngine::square(best.m.from_sq()) != f.passed[0].front())
                steps.push_back("In the endgame, push the passed pawn on " + f.passed[0].front()
                                + " with the support of the king.");
        }
        for (size_t i = 0; i < steps.size(); ++i)
            ss << "  " << i + 1 << ". " << steps[i] << "\n";
        ss << "\n";
    }

    // ---- In a nutshell -------------------------------------------------------------------------
    ss << "IN A NUTSHELL:\n";
    {
        if (mateFor)
            ss << Us << " mates with " << bestSan << ".";
        else if (mated)
            ss << Us << " cannot avoid mate.";
        else
        {
            const int cpA = ctx.cp(vAssess);
            const int aA  = std::abs(cpA);
            ss << (aA < 20 ? "Balanced position" : (cpA > 0 ? "You are " : Them + " is ")
                                                     + std::string(aA < 50 ? "slightly better" : aA < 200 ? "better" : "much better"))
               << (bucket <= 2 || noPieces ? ""
                   : complexity > 250      ? " in a sharp position"
                   : complexity < 75       ? " in a quiet position"
                                           : "")
               << ": " << (ctx.inCheck ? "answer the check with " : f.threat ? "meet the threat with " : "play ")
               << bestSan;
            if (keepTension)
                ss << ", keep the pawn tension";
            else if (!f.goodBreaks.empty())
                ss << ", aim for the break " << san(pos, f.goodBreaks.front());
            const Eval::NNUE::PieceInsight* uw = nullptr;
            for (const auto& p : in.pieces)
                if (color_of(p.pc) == us && type_of(p.pc) != PAWN && (!uw || p.activity < uw->activity))
                    uw = &p;
            if (uw && uw->sq != best.m.from_sq())
                ss << " and improve " << piece_on(uw->pc, uw->sq);
            ss << ".";
        }
        ss << "\n";
    }
    ss << "================================================================\n";
    // No trailing blanks at the end of the paragraphs
    std::string out = ss.str();
    for (size_t k; (k = out.find(" \n")) != std::string::npos;)
        out.erase(k, 1);
    return out;
}

}  // namespace

// Like evaluate(), but instead of returning a value, it returns a string (suitable for
// outputting to stdout) that contains the detailed analysis of the position.
// Trace scores are from White's point of view unless stated otherwise.
std::string Eval::trace(Position& pos, const Eval::NNUE::Network& network, bool showWDL) {

    // Without UCI_ShowWDL the trace keeps the original behaviour in check. With it, the
    // analysis is still produced (the verdict then focuses on the answer to the check); the
    // static evaluation is computed without the "not in check" assertion.
    if (pos.checkers() && !showWDL)
        return "Final evaluation: none (in check)";

    auto accumulators = std::make_unique<Eval::NNUE::AccumulatorStack>();
    auto caches       = std::make_unique<Eval::NNUE::AccumulatorCaches>(network);

    std::stringstream ss;

    if (!WDLModel::is_initialized())
        WDLModel::init();

    Value v       = evaluate_unchecked(network, pos, *accumulators, *caches, VALUE_ZERO);
    Value v_white = pos.side_to_move() == WHITE ? v : -v;

    if (showWDL)
    {
        uint8_t       winProbWhite = WDLModel::get_win_probability(v_white, pos);
        WDLModel::WDL wdl          = WDLModel::get_wdl(v_white, pos);
        std::string   shashinZone  = get_shashin_zone(winProbWhite, WHITE);

        // =====================================================================================
        // BLOCK 1: NNUE MECHANISTIC TRACE + SHASHIN STATIC ANALYSIS (master)
        // =====================================================================================
        // The technical sections are collected apart and printed after the verdict
        std::stringstream tech;
        tech << std::showpoint << std::noshowpos << std::fixed << std::setprecision(2);
        if (pos.checkers())
            tech << "*** " << color_name(pos.side_to_move())
                 << " is in check: static values below are indicative only. ***\n";
        tech << '\n' << NNUE::trace(pos, network, *caches) << '\n';

        // Context of all the analyses below: NNUE insight, quiet probes of all the legal moves,
        // null-move threat
        Context ctx(pos, network, *caches);

        tech << "=== SHASHIN STATIC ANALYSIS ===\n";
        tech << "*** Static activity evaluation based on NNUE network ***\n";
        tech << "*** All NNUE values shown in centipawn for technical accuracy ***\n";
        tech << "*** Win probability and Shashin zone always from White's perspective ***\n\n";

        tech << "--- PIECE ACTIVITY ANALYSIS (Makogonov Principle) ---\n";
        tech << analyze_piece_activity_horizontal(pos) << "\n";

        tech << "--- LEGAL MOVES ANALYSIS ---\n";
        tech << analyze_legal_moves(ctx) << "\n";

        auto [psqt_syn, positional_syn] = network.evaluate(pos, *accumulators, *caches);
        const int persp_syn  = pos.side_to_move() == WHITE ? 1 : -1;
        int       wp_mat_syn = WDLModel::get_win_probability(Value(persp_syn * psqt_syn), pos);
        int       wp_tot_syn =
          WDLModel::get_win_probability(Value(persp_syn * (psqt_syn + positional_syn)), pos);
        int structural_delta = wp_tot_syn - wp_mat_syn;

        tech << "--- STRATEGIC SYNTHESIS ---\n";
        tech << generate_strategic_synthesis(winProbWhite, wdl, shashinZone, v_white, pos,
                                             structural_delta, std::abs(psqt_syn - positional_syn))
             << "\n";

        tech << "================================\n";
        tech << "Final Static Activity Evaluation: " << 0.01 * UCIEngine::to_cp(v_white, pos)
             << " pawns (White perspective)\n";
        tech << "WDL (Win/Draw/Loss): " << static_cast<int>(wdl.win) << "%/"
             << static_cast<int>(wdl.draw) << "%/" << static_cast<int>(wdl.loss) << "%\n";
        tech << "Shashin Zone: " << shashinZone << "\n";
        tech << "*** Note: Static activity analysis based on current piece placement ***\n";

        // =====================================================================================
        // BLOCK 2: ADVANCED SHASHIN MODULES (Branch-Max, adapted to the single network)
        // =====================================================================================
        MasterReportData mr;
        tech << "\n================================================================\n";
        tech << "=== ADVANCED SHASHIN MODULES ===\n";
        tech << "(point of view: " << color_name(pos.side_to_move())
             << ", the side to move; cp on the UCI scale)\n\n";
        tech << get_shashin_advice(winProbWhite, pos.side_to_move(), mr) << "\n";
        tech << analyze_engine_flags(ctx, mr);
        tech << analyze_threats(ctx, mr);
        tech << analyze_space_advantage(pos, mr);
        tech << analyze_knight_outposts(ctx, mr);
        tech << analyze_color_complexes(ctx, mr);
        tech << analyze_piece_importance(ctx, mr);
        tech << analyze_open_files(pos, mr);
        tech << analyze_tension_and_breaks(ctx, mr);
        tech << analyze_king_safety(ctx, mr);
        tech << analyze_game_plans(ctx, mr);

        // Output order: the plain-language verdict first, then all the technical parameters
        ss << "\n*** Reading guide: the MASTER'S VERDICT (plain language) comes first; all the "
              "technical parameters it is based on follow after the separator. ***\n\n";
        ss << compose_verdict(ctx, mr, winProbWhite, wdl, v_white,
                              std::abs(psqt_syn - positional_syn));
        ss << "\n\n"
           << "################################################################\n"
           << "####            TECHNICAL PARAMETERS AND DETAILS            ####\n"
           << "####   (data behind the verdict above, for deeper study)    ####\n"
           << "################################################################\n";
        ss << tech.str();
    }
    else
    {
        ss << std::showpoint << std::noshowpos << std::fixed << std::setprecision(2);
        ss << '\n' << NNUE::trace(pos, network, *caches) << '\n';
        ss << std::showpoint << std::showpos << std::fixed << std::setprecision(2) << std::setw(15);

        auto [psqt, positional] = network.evaluate(pos, *accumulators, *caches);
        Value v_nnue            = psqt + positional;
        ss << "NNUE evaluation         " << v_nnue << " (side to move, internal units)\n";
        v_nnue = pos.side_to_move() == WHITE ? v_nnue : -v_nnue;
        ss << "NNUE evaluation       " << 0.01 * UCIEngine::to_cp(v_nnue, pos)
           << " (white side)\n";

        ss << "Final evaluation      ";
        ss << 0.01 * UCIEngine::to_cp(v_white, pos) << " (white side)";
        ss << " [with scaled NNUE, ...]\n";
    }

    return ss.str();
}

}  // namespace ShashChess
