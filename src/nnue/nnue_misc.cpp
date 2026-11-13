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

// [SC:explain] NNUE mechanistic trace of ShashChess 42.
//
// The previous version worked in the win probability domain: removing any minor piece moved the
// expected score to 0% or 100%, so almost every piece showed the same saturated "impact" (+46%,
// +54%, ...), and the ghost-piece probes were disabled with 32 pieces on the board. This version
// works in centipawns, keeps the layer stack of the position fixed during the ablations
// (otherwise removing a piece also switches sub-network) and, thanks to the Explainer, opens the
// input layer of the network: piece placement, piece relations (threats/defences) and pawn
// pairs can be measured separately, relation by relation.

#include "nnue_misc.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string_view>
#include <tuple>
#include <vector>

#include "../attacks.h"
#include "../misc.h"
#include "../position.h"
#include "../types.h"
#include "../uci.h"
#include "../wdl/win_probability.h"
#include "network.h"
#include "nnue_accumulator.h"
#include "nnue_explain.h"

namespace ShashChess::Eval::NNUE {

constexpr std::string_view PieceToChar(" PNBRQK  pnbrqk");

namespace {

using namespace Explain;

constexpr int ClassicalPawns[PIECE_TYPE_NB] = {0, 1, 3, 3, 5, 9, 0, 0};

std::string piece_str(Piece pc, Square s) {
    return std::string(1, PieceToChar[pc]) + UCIEngine::square(s);
}

// Pawns (cp / 100) with sign, 2 decimals
std::string pawns(Value v, const Position& pos) {
    std::stringstream ss;
    ss << std::showpos << std::fixed << std::setprecision(2) << 0.01 * UCIEngine::to_cp(v, pos)
       << std::noshowpos;
    return ss.str();
}

const char* color_name(Color c) { return c == WHITE ? "White" : "Black"; }

const char* phase_name(int bucket) {
    switch (bucket)
    {
    case 7 :
        return "Opening (29-32 pieces): maximum structural complexity";
    case 6 :
        return "Early middlegame (25-28 pieces): plans still fluid";
    case 5 :
        return "Middlegame (21-24 pieces): central strategic phase";
    case 4 :
        return "Late middlegame (17-20 pieces): simplifications under way";
    case 3 :
        return "Endgame transition (13-16 pieces)";
    case 2 :
        return "Endgame (9-12 pieces): active king, pawns decisive";
    case 1 :
        return "Late endgame (5-8 pieces)";
    default :
        return "Pawn/King endgame (2-4 pieces)";
    }
}

// Role of a piece. Paired efficiency (100% = as good as the equivalent enemy pieces): bands of
// 5% / 15% of the nominal value; single efficiency (no counterpart): the original bands
const char* role_name(int efficiency, Value single, bool paired) {
    if (single < 0)
        return "BLINDSPOT";
    const int k = paired ? 115 : 125, a = paired ? 105 : 110, st = paired ? 95 : 90,
              ps = paired ? 85 : 70;
    if (efficiency >= k)
        return "Keystone";
    if (efficiency >= a)
        return "Active";
    if (efficiency >= st)
        return "Standard";
    if (efficiency >= ps)
        return "Passive";
    return "Liability";
}

}  // namespace

// Short label of a relation
std::string relation_label(const Relation& r) {
    std::stringstream ss;
    if (r.kind == REL_THREAT)
    {
        const bool same = color_of(r.pcA) == color_of(r.pcB);
        ss << piece_str(r.pcA, r.sqA) << (same ? "=" : ">") << piece_str(r.pcB, r.sqB)
           << (same ? " defence" : " attack");
        return ss.str();
    }
    const int df = std::abs(int(file_of(r.sqA)) - int(file_of(r.sqB)));
    const int dr = std::abs(int(rank_of(r.sqA)) - int(rank_of(r.sqB)));
    ss << piece_str(r.pcA, r.sqA) << "/" << piece_str(r.pcB, r.sqB) << " ";
    if (color_of(r.pcA) == color_of(r.pcB))
        ss << (df == 0           ? "doubled"
               : dr == 0         ? "phalanx"
               : dr == 1         ? "chain"
                                 : "same-wing group");
    else
    {
        const Square w = color_of(r.pcA) == WHITE ? r.sqA : r.sqB;
        const Square b = color_of(r.pcA) == WHITE ? r.sqB : r.sqA;
        const int    ahead = int(rank_of(b)) - int(rank_of(w));  // > 0: black pawn in front
        if (df == 0)
            ss << (ahead == 1 ? "ram (blocked)" : ahead > 1 ? "opposed" : "passed by each other");
        else if (ahead == 1)
            ss << "lever (tension)";
        else if (ahead > 1)
            ss << "sentry (controls the path)";
        else
            ss << "passed by each other";
    }
    return ss.str();
}

NnueInsight analyze(const Position& pos, const Network& network, AccumulatorCaches& caches) {
    NnueInsight in;
    Explainer   ex(network);
    const Board board = pos.piece_array();
    const Color stm   = pos.side_to_move();
    const int   b     = natural_bucket(board);
    in.bucket         = b;

    FeatureSet fs   = Explainer::collect(board);
    auto       full = std::make_unique<WideAccumulator>(ex.accumulate(fs));
    in.base         = ex.evaluate_white(*full, stm, b);

    // Consistency check against the engine's own incremental accumulators
    {
        auto accumulators       = std::make_unique<AccumulatorStack>();
        auto [psqt, positional] = network.evaluate(pos, *accumulators, caches);
        Output o                = ex.evaluate(*full, stm, b);
        in.consistent           = psqt == o.psqt && positional == o.positional;
    }

    const Value baseW = in.base.total();

    // Feature families: Shapley values of the threat and pawn-pair families, the piece placement
    // (PSQ) family being the baseline that is always present
    {
        const Value onlyPsq = ex.evaluate_white(ex.accumulate(fs, false, false), stm, b).total();
        const Value psqThr  = ex.evaluate_white(ex.accumulate(fs, true, false), stm, b).total();
        const Value psqPair = ex.evaluate_white(ex.accumulate(fs, false, true), stm, b).total();
        in.familyPsq        = onlyPsq;
        in.familyThreat     = ((psqThr - onlyPsq) + (baseW - psqPair)) / 2;
        in.familyPair       = ((psqPair - onlyPsq) + (baseW - psqThr)) / 2;
    }

    // Piece-level measures (virtual insertion, ablation, activity, synergy) use the convention
    // "the owner of the piece is to move": with the actual side to move, every piece of the
    // other side would look vulnerable (the opponent could take it at once), and the comparison
    // between the two sides would be biased by the tempo. Real threats are analyzed elsewhere.
    Value ownerBase[COLOR_NB];
    for (Color c : {WHITE, BLACK})
        ownerBase[c] = ex.evaluate_white(*full, c, b).total();

    // Virtual insertion of a knight, a bishop or a rook on every empty square, for both sides.
    // The median value of each piece type over all the squares is the "reference" value of that
    // piece type in this position: a piece (real or virtual) is judged against it.
    Bitboard occupied = 0;
    for (Square s = SQ_A1; s <= SQ_H8; ++s)
        if (board[s] != NO_PIECE)
            occupied |= s;
    for (Color c : {WHITE, BLACK})
    {
        const Square       enemyKing = pos.square<KING>(~c);
        std::vector<Value> samples[3];
        for (Square s = SQ_A1; s <= SQ_H8; ++s)
        {
            if (board[s] != NO_PIECE)
                continue;
            ProbeInsight pr;
            pr.sq   = s;
            pr.pt   = NO_PIECE_TYPE;
            pr.gain = pr.relative = 0;
            bool valid            = false;
            for (PieceType pt : {KNIGHT, BISHOP, ROOK})
            {
                pr.byType[pt - KNIGHT] = VALUE_NONE;
                if (Attacks::attacks_bb(pt, s, occupied) & enemyKing)
                    continue;  // the virtual piece would give check: not a quiet square
                Board b2 = board;
                b2[s]    = make_piece(c, pt);
                Output o = ex.evaluate_board_white(b2, c, b);
                Value  g = c == WHITE ? o.total() - ownerBase[c] : ownerBase[c] - o.total();
                pr.byType[pt - KNIGHT] = g;
                samples[pt - KNIGHT].push_back(g);
                valid = true;
            }
            if (valid)
                in.outposts[c].push_back(pr);
        }
        for (PieceType pt : {KNIGHT, BISHOP, ROOK})
        {
            auto& v = samples[pt - KNIGHT];
            if (v.empty())
                continue;
            std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
            in.reference[c][pt] = v[v.size() / 2];
        }
        for (auto& pr : in.outposts[c])
        {
            pr.relative = -VALUE_INFINITE;
            for (PieceType pt : {KNIGHT, BISHOP, ROOK})
                if (pr.byType[pt - KNIGHT] != VALUE_NONE
                    && pr.byType[pt - KNIGHT] - in.reference[c][pt] > pr.relative)
                {
                    pr.relative = pr.byType[pt - KNIGHT] - in.reference[c][pt];
                    pr.pt       = pt;
                    pr.gain     = pr.byType[pt - KNIGHT];
                }
        }
        std::sort(in.outposts[c].begin(), in.outposts[c].end(),
                  [](const ProbeInsight& x, const ProbeInsight& y) {
                      return x.relative > y.relative;
                  });
    }

    // Single ablations with the layer stack of the position: kept for the BLINDSPOT test (is the
    // owner better off without the piece?), for the typical values of pawns and queens and as
    // the fallback of the pieces without an equivalent enemy piece
    Output ownerOut[COLOR_NB];
    for (Color c : {WHITE, BLACK})
        ownerOut[c] = ex.evaluate_white(*full, c, b);
    for (Square s = SQ_A1; s <= SQ_H8; ++s)
    {
        const Piece pc = board[s];
        if (pc == NO_PIECE || type_of(pc) == KING)
            continue;
        const Color   owner = color_of(pc);
        const Output& ob    = ownerOut[owner];
        Board         b2    = board;
        b2[s]               = NO_PIECE;
        Output o            = ex.evaluate_board_white(b2, owner, b);
        const int    sign   = owner == WHITE ? 1 : -1;
        PieceInsight pi;
        pi.sq           = s;
        pi.pc           = pc;
        pi.single       = sign * (ob.total() - o.total());
        pi.value        = pi.single;
        pi.psqt         = sign * (ob.psqt - o.psqt);
        pi.positional   = sign * (ob.positional - o.positional);
        pi.counterparts = 0;
        pi.activity     = 0;
        pi.efficiency   = 100;
        in.pieces.push_back(pi);
    }

    // References of pawns and queens (no virtual insertion for them): the average single value
    // of the pawns / queens on the board, both colors
    for (PieceType pt : {PAWN, QUEEN})
    {
        i64 sum = 0;
        int n   = 0;
        for (const auto& p : in.pieces)
            if (type_of(p.pc) == pt)
            {
                sum += p.single;
                ++n;
            }
        for (Color c : {WHITE, BLACK})
            in.reference[c][pt] = n ? Value(sum / n) : 0;
    }

    // Paired ablations: the piece and an equivalent enemy piece are removed together, so that
    // the material stays balanced and the network is not pushed into the regime of a material
    // imbalance: the change measures how much better (or worse) the piece is than its
    // counterpart. The value is the average over all the equivalent enemy pieces.
    auto pairClass = [](PieceType pt) { return pt == BISHOP ? KNIGHT : pt; };
    for (auto& p : in.pieces)
    {
        const Color owner = color_of(p.pc);
        const int   sign  = owner == WHITE ? 1 : -1;
        i64         tot = 0, psq = 0, pos2 = 0;
        int         n   = 0;
        for (const auto& q : in.pieces)
        {
            if (color_of(q.pc) == owner || pairClass(type_of(q.pc)) != pairClass(type_of(p.pc)))
                continue;
            Board b2  = board;
            b2[p.sq]  = NO_PIECE;
            b2[q.sq]  = NO_PIECE;
            Output o  = ex.evaluate_board_white(b2, owner, b);
            tot      += sign * (ownerOut[owner].total() - o.total());
            psq      += sign * (ownerOut[owner].psqt - o.psqt);
            pos2     += sign * (ownerOut[owner].positional - o.positional);
            ++n;
        }
        if (n)
        {
            p.value        = Value(tot / n);
            p.psqt         = Value(psq / n);
            p.positional   = Value(pos2 / n);
            p.counterparts = n;
        }
    }
    // Efficiency. Paired: 100% + the advantage over the counterparts in % of the nominal value
    // of the piece (1, 3, 3, 5, 9 pawns): the same scale is used by Alexander, so that the two
    // engines can be compared. Single (no counterpart): value / typical value of the type here
    // (the typical value already includes the saturation of the network).
    for (auto& p : in.pieces)
    {
        static constexpr int Nominal[] = {0, 1, 3, 3, 5, 9, 0};
        const Value          ref       = in.reference[color_of(p.pc)][type_of(p.pc)];
        if (p.counterparts)
            p.efficiency =
              100 + int(std::lround(double(UCIEngine::to_cp(p.value, pos)) / Nominal[type_of(p.pc)]));
        else
            p.efficiency = ref > 0 ? int(std::lround(100.0 * p.value / ref)) : 100;
    }

    // Occlusion of the single relations (threats and pawn pairs)
    for (const auto& r : fs.relations)
    {
        ex.add_relation(*full, r, -1);
        const Value v = baseW - ex.evaluate_white(*full, stm, b).total();
        ex.add_relation(*full, r, +1);
        if (r.kind == REL_THREAT)
        {
            in.threats.push_back({r, v});
            in.pressure[r.sqB] += v;
        }
        else
        {
            in.pairs.push_back({r, v});
            in.pawnStructure[r.sqA] += v / 2;
            in.pawnStructure[r.sqB] += v - v / 2;
        }
    }
    auto byAbs = [](const RelationInsight& x, const RelationInsight& y) {
        return std::abs(x.value) > std::abs(y.value);
    };
    std::sort(in.threats.begin(), in.threats.end(), byAbs);
    std::sort(in.pairs.begin(), in.pairs.end(), byAbs);

    // Activity of a set of pieces: value (White's view) of all the threat relations that involve
    // them, measured by occluding those relations together
    auto activity_white = [&](Bitboard squares, Color owner) {
        std::vector<const Relation*> rs;
        for (const auto& r : fs.relations)
            if (r.kind == REL_THREAT && ((square_bb(r.sqA) | square_bb(r.sqB)) & squares))
                rs.push_back(&r);
        for (auto* r : rs)
            ex.add_relation(*full, *r, -1);
        const Value v = ownerBase[owner] - ex.evaluate_white(*full, owner, b).total();
        for (auto* r : rs)
            ex.add_relation(*full, *r, +1);
        return v;
    };
    for (auto& p : in.pieces)
        p.activity =
          (color_of(p.pc) == WHITE ? 1 : -1) * activity_white(square_bb(p.sq), color_of(p.pc));

    // Synergy of two pieces of the same side (no pawns): activity of the pair minus the two
    // single activities. Positive: their activity reinforces each other (battery, mutual
    // protection, converging attack); negative: redundancy.
    for (Color c : {WHITE, BLACK})
    {
        std::vector<const PieceInsight*> ps;
        for (const auto& p : in.pieces)
            if (color_of(p.pc) == c && type_of(p.pc) != PAWN)
                ps.push_back(&p);
        for (size_t i = 0; i < ps.size(); ++i)
            for (size_t j = i + 1; j < ps.size(); ++j)
            {
                const Value both =
                  (c == WHITE ? 1 : -1)
                  * activity_white(square_bb(ps[i]->sq) | square_bb(ps[j]->sq), c);
                in.synergies[c].push_back(
                  {ps[i]->sq, ps[j]->sq, both - ps[i]->activity - ps[j]->activity});
            }
        std::sort(in.synergies[c].begin(), in.synergies[c].end(),
                  [](const SynergyInsight& x, const SynergyInsight& y) {
                      return std::abs(x.synergy) > std::abs(y.synergy);
                  });
    }
    return in;
}

std::string
trace(Position& pos, const Eval::NNUE::Network& network, Eval::NNUE::AccumulatorCaches& caches) {

    if (!WDLModel::is_initialized())
        WDLModel::init();

    std::stringstream ss;
    const NnueInsight in   = analyze(pos, network, caches);
    const Color       stm  = pos.side_to_move();
    const Value       base = in.base.total();
    const int         wp   = WDLModel::get_win_probability(base, pos);
    auto              P    = [&](Value v) { return pawns(v, pos); };
    auto              cp   = [&](Value v) { return UCIEngine::to_cp(v, pos); };

    ss << "=== NNUE MECHANISTIC TRACE (centipawn domain, layer stack held fixed) ===\n";
    ss << "Network: HalfKAv2_hm (piece placement) + FullThreats (piece relations) + PP_3Wide "
          "(pawn pairs)\n"
       << "         -> 2 x " << L1 << " accumulator -> " << LayerStacks
       << " layer stacks (32-32-1), one per piece-count bucket\n";
    ss << "Explanation engine: "
       << (in.consistent ? "rebuilt accumulators reproduce the engine output exactly [OK]"
                         : "WARNING: rebuilt accumulators differ from the engine output")
       << "\n";
    ss << "Raw NNUE output (White): " << P(base) << " = PSQT " << P(in.base.psqt)
       << " + positional " << P(in.base.positional) << " | expected score White " << wp
       << "%\n";
    ss << "*** Base Win Probability (White): " << wp << "% ***\n";
    ss << "Units: pawns (cp/100, UCI scale), White point of view unless stated otherwise.\n\n";

    // ---------------------------------------------------------------------------------------
    ss << "1. NNUE DERIVED PIECE VALUES (paired ablation: evaluation lost by the owner if the "
          "piece and an\n   equivalent enemy piece are removed together, average over all the "
          "equivalent enemy pieces;\n   '*' = no counterpart: the piece alone is removed)\n";
    {
        char board[3 * 8 + 1][8 * 8 + 2];
        std::memset(board, ' ', sizeof(board));
        for (int row = 0; row < 3 * 8 + 1; ++row)
            board[row][8 * 8 + 1] = '\0';
        for (int f = 0; f < 8; ++f)
            for (int r = 0; r < 8; ++r)
            {
                const int x = f * 8, y = (7 - r) * 3;
                for (int i = 1; i < 8; ++i)
                    board[y][x + i] = board[y + 3][x + i] = '-';
                for (int i = 1; i < 3; ++i)
                    board[y + i][x] = board[y + i][x + 8] = '|';
                board[y][x] = board[y][x + 8] = board[y + 3][x + 8] = board[y + 3][x] = '+';
                const Square s  = make_square(File(f), Rank(r));
                const Piece  pc = pos.piece_on(s);
                if (pc != NO_PIECE)
                    board[y + 1][x + 4] = PieceToChar[pc];
                if (const auto* pi = in.piece_on(s))
                {
                    char buf[16];
                    std::snprintf(buf, sizeof(buf), "%+6.2f", 0.01 * cp(pi->value));
                    std::memcpy(&board[y + 2][x + 1], buf, std::min<size_t>(6, std::strlen(buf)));
                    if (!pi->counterparts)
                        board[y + 2][x + 7] = '*';
                }
            }
        for (int row = 0; row < 3 * 8 + 1; ++row)
            ss << board[row] << '\n';
        ss << '\n';
    }

    // ---------------------------------------------------------------------------------------
    ss << "2. PIECE ROLES\n"
          "   value      = PAIRED ABLATION: evaluation lost by the owner when the piece and an "
          "equivalent\n"
          "                enemy piece are removed together (pawn-pawn, minor-minor with knight "
          "and bishop\n"
          "                interchangeable, rook-rook, queen-queen), averaged over all the "
          "equivalent enemy\n"
          "                pieces (= PSQT part + positional part). Material stays balanced: > 0 "
          "means the\n"
          "                piece is worth more than its counterparts. '*' = no counterpart: "
          "single ablation\n"
          "   single     = evaluation lost by the owner removing only the piece (BLINDSPOT if < "
          "0)\n"
          "   activity   = value of the piece's attacks/defences only (the piece stays on the "
          "board)\n"
          "   efficiency = 100% + value in % of the nominal value of the piece (1/3/3/5/9 "
          "pawns): 100% = as\n"
          "                good as its counterparts; '*': value / typical value of that piece "
          "type here\n"
          "   (piece-level measures are taken with the owner of the piece to move, so that the "
          "two\n    sides are compared without the tempo bias; real threats: see the plans)\n";
    ss << "   Keystone >= 115% | Active 105-114% | Standard 95-104% | Passive 85-94% | "
          "Liability < 85%\n"
          "   ('*': Keystone >= 125% | Active 110-124% | Standard 90-109% | Passive 70-89% | "
          "Liability < 70%) | BLINDSPOT: single < 0\n";
    for (Color c : {WHITE, BLACK})
    {
        std::vector<const PieceInsight*> v;
        for (const auto& p : in.pieces)
            if (color_of(p.pc) == c && type_of(p.pc) != PAWN)
                v.push_back(&p);
        std::sort(v.begin(), v.end(),
                  [](auto a, auto b2) { return a->efficiency > b2->efficiency; });
        ss << "   " << color_name(c) << ":            value   (PSQT / positional)    single  "
           << "activity  efficiency\n";
        for (const auto* p : v)
            ss << "     " << std::left << std::setw(4) << piece_str(p->pc, p->sq) << std::right
               << std::setw(13) << P(p->value) << (p->counterparts ? " " : "*") << "  ("
               << std::setw(6) << P(p->psqt) << " / " << std::setw(6) << P(p->positional)
               << ")   " << std::setw(6) << P(p->single) << "   " << std::setw(6)
               << P(p->activity) << "   " << std::setw(4) << p->efficiency << "%  "
               << role_name(p->efficiency, p->single, p->counterparts > 0)
               << "\n";
        if (v.empty())
            ss << "     (no pieces)\n";
        std::vector<const PieceInsight*> pw;
        for (const auto& p : in.pieces)
            if (color_of(p.pc) == c && type_of(p.pc) == PAWN)
                pw.push_back(&p);
        std::sort(pw.begin(), pw.end(), [](auto a, auto b2) { return a->value > b2->value; });
        if (!pw.empty())
        {
            ss << "     Pawns (most to least valuable, paired): ";
            for (size_t i = 0; i < pw.size(); ++i)
                ss << (i ? ", " : "") << UCIEngine::square(pw[i]->sq) << " " << P(pw[i]->value)
                   << (pw[i]->counterparts ? "" : "*");
            ss << "\n";
        }
    }
    ss << "   Reference values (White/Black): N " << P(in.reference[WHITE][KNIGHT]) << "/"
       << P(in.reference[BLACK][KNIGHT]) << ", B " << P(in.reference[WHITE][BISHOP]) << "/"
       << P(in.reference[BLACK][BISHOP]) << ", R " << P(in.reference[WHITE][ROOK]) << "/"
       << P(in.reference[BLACK][ROOK]) << ", Q " << P(in.reference[WHITE][QUEEN]) << ", P "
       << P(in.reference[WHITE][PAWN]) << "\n";
    ss << "   (Removing a single piece pushes the network into its saturated regime, so in "
          "'single' and in\n    the reference values heavy pieces look cheaper than their "
          "material; the paired 'value'\n    keeps the material balanced and does not suffer "
          "from it.)\n\n";

    ss << "3. NEURAL NETWORK INTERNAL ROUTING\n";
    ss << "   Active Sub-Network (Bucket) : " << in.bucket << " out of " << LayerStacks - 1
       << "\n";
    ss << "   Piece Count Detected        : " << pos.count<ALL_PIECES>() << "\n";
    ss << "   Deduced Game Phase          : " << phase_name(in.bucket) << "\n\n";

    // ---------------------------------------------------------------------------------------
    ss << "4. EVALUATION COMPOSITION (what every sub-network thinks of this position)\n";
    ss << "+--------+--------------+------------------+------------+----------+\n"
       << "| Bucket | PSQT (pawns) | Positional (pwn) | Total      | Total WP |\n"
       << "+--------+--------------+------------------+------------+----------+\n";
    {
        Explainer  ex(network);
        const auto full = std::make_unique<WideAccumulator>(
          ex.accumulate(Explainer::collect(pos.piece_array())));
        for (int bk = 0; bk < int(LayerStacks); ++bk)
        {
            Output o = ex.evaluate_white(*full, stm, bk);
            ss << "| " << (bk == in.bucket ? "->" : "  ") << " " << bk << "  | " << std::setw(12)
               << P(o.psqt) << " | " << std::setw(16) << P(o.positional) << " | "
               << std::setw(10) << P(o.total()) << " | " << std::setw(7)
               << int(WDLModel::get_win_probability(o.total(), pos)) << "% |\n";
        }
    }
    ss << "+--------+--------------+------------------+------------+----------+\n";
    const int wpPsqt     = WDLModel::get_win_probability(in.base.psqt, pos);
    const int neuralDelta = wp - wpPsqt;
    ss << "   Material perception (PSQT)  : " << P(in.base.psqt) << " (" << wpPsqt << "% WP)\n";
    ss << "   Structural abstraction      : " << P(in.base.positional) << " ("
       << std::showpos << neuralDelta << std::noshowpos << "% WP)\n";
    ss << "   (Lower buckets show how the endgame sub-networks would judge the same position:\n"
          "    a trend, not a forecast.)\n\n";

    // ---------------------------------------------------------------------------------------
    const Value famTotal = std::abs(in.familyThreat) + std::abs(in.familyPair);
    ss << "5. INPUT FEATURE FAMILIES (Shapley attribution, White point of view)\n";
    ss << "   Piece relations (FullThreats: attacks and defences) : " << std::setw(6)
       << P(in.familyThreat) << "\n";
    ss << "   Pawn structure (PP_3Wide: pawn pairs)              : " << std::setw(6)
       << P(in.familyPair) << "\n";
    ss << "   Piece placement only (HalfKAv2_hm, baseline)       : " << std::setw(6)
       << P(in.familyPsq) << "\n";
    ss << "   Share of the dynamic component (relations)         : "
       << (famTotal ? 100 * std::abs(in.familyThreat) / famTotal : 0) << "%\n";
    ss << "   (The baseline evaluates the pieces without any relation, a situation the network\n"
          "    never saw in training: read it as a reference point, not as an evaluation.)\n\n";

    // ---------------------------------------------------------------------------------------
    ss << "6. KEY PIECE RELATIONS (value of a single attack/defence feature, by occlusion)\n";
    ss << "   ('>' = attacks an enemy piece, '=' = defends an own piece; a relation of a side "
          "can\n    also be a liability for it, e.g. a piece tied to a passive defensive "
          "duty.\n    Single-feature occlusion: very common relations of the initial setup can "
          "be over-weighted.)\n";
    for (Color c : {WHITE, BLACK})
    {
        const int sign = c == WHITE ? 1 : -1;
        for (bool assets : {true, false})
        {
            ss << "   " << color_name(c) << (assets ? " assets     : " : " liabilities: ");
            int n = 0;
            for (const auto& t : in.threats)
                if (color_of(t.rel.pcA) == c && (assets ? sign * t.value > 0 : sign * t.value < 0)
                    && n < 5 && std::abs(cp(t.value)) >= 2)
                    ss << (n++ ? " | " : "") << relation_label(t.rel) << " "
                       << pawns(sign * t.value, pos);
            ss << (n ? "\n" : "none\n");
        }
    }
    ss << '\n';

    ss << "7. PAWN STRUCTURE AS SEEN BY THE NETWORK (pawn-pair features, by occlusion)\n";
    ss << "   Most important pawn relations: ";
    for (size_t i = 0; i < in.pairs.size() && i < 6; ++i)
        ss << (i ? " | " : "") << relation_label(in.pairs[i].rel) << " "
           << P(in.pairs[i].value);
    ss << (in.pairs.empty() ? "none\n" : "\n");
    for (Color c : {WHITE, BLACK})
    {
        std::vector<std::pair<Value, Square>> v;
        for (Square s = SQ_A1; s <= SQ_H8; ++s)
            if (pos.piece_on(s) == make_piece(c, PAWN))
                v.emplace_back((c == WHITE ? 1 : -1) * in.pawnStructure[s], s);
        if (v.empty())
            continue;
        std::sort(v.begin(), v.end(), [](auto& a, auto& b2) { return a.first > b2.first; });
        ss << "   " << color_name(c) << " structural pawns: strongest "
           << UCIEngine::square(v.front().second) << " " << P(v.front().first) << ", weakest "
           << UCIEngine::square(v.back().second) << " " << P(v.back().first) << "\n";
    }
    ss << '\n';

    // ---------------------------------------------------------------------------------------
    ss << "8. PRESSURE MAP (relation values summed on the attacked/defended square; + = White)\n";
    {
        std::vector<std::pair<Value, Square>> hot;
        for (int r = 7; r >= 0; --r)
        {
            ss << "   " << (r + 1) << " ";
            for (int f = 0; f < 8; ++f)
            {
                const Square s = make_square(File(f), Rank(r));
                const int    v = cp(in.pressure[s]);
                if (std::abs(v) < 5)
                    ss << "    . ";
                else
                    ss << std::setw(5) << std::showpos << v << std::noshowpos << " ";
                if (in.pressure[s])
                    hot.emplace_back(in.pressure[s], s);
            }
            ss << "\n";
        }
        ss << "        a     b     c     d     e     f     g     h   (cp)\n";
        std::sort(hot.begin(), hot.end(),
                  [](auto& a, auto& b2) { return std::abs(a.first) > std::abs(b2.first); });
        ss << "   Hottest squares: ";
        for (size_t i = 0; i < hot.size() && i < 5; ++i)
            ss << (i ? ", " : "") << UCIEngine::square(hot[i].second) << " (" << P(hot[i].first)
               << ")";
        if (hot.empty())
            ss << "none (no piece relations)";
        ss << "\n\n";
    }

    // ---------------------------------------------------------------------------------------
    ss << "9. VIRTUAL PIECE INSERTION (latent outposts: squares where a piece would be worth "
          "most\n   above the typical value of its type; layer stack fixed, no 32-pieces limit)\n";
    for (Color c : {WHITE, BLACK})
    {
        ss << "   " << color_name(c) << ":\n";
        for (size_t i = 0; i < in.outposts[c].size() && i < 5; ++i)
        {
            const auto& o = in.outposts[c][i];
            ss << "    - " << UCIEngine::square(o.sq) << ": " << "NBR"[o.pt - KNIGHT] << " "
               << P(o.relative) << " above typical (value " << P(o.gain) << ")";
            ss << "  [N " << (o.byType[0] == VALUE_NONE ? std::string("  -  ") : P(o.byType[0]))
               << ", B " << (o.byType[1] == VALUE_NONE ? std::string("  -  ") : P(o.byType[1]))
               << ", R " << (o.byType[2] == VALUE_NONE ? std::string("  -  ") : P(o.byType[2]))
               << "]\n";
        }
    }
    ss << '\n';

    ss << "10. PAIRWISE SYNERGY (activity of the pair minus the two single activities, owner's "
          "view)\n";
    for (Color c : {WHITE, BLACK})
    {
        ss << "   " << color_name(c) << ": ";
        int n = 0;
        for (const auto& sy : in.synergies[c])
            if (n < 4 && std::abs(cp(sy.synergy)) >= 3)
                ss << (n++ ? " | " : "") << piece_str(pos.piece_on(sy.a), sy.a) << "+"
                   << piece_str(pos.piece_on(sy.b), sy.b) << " " << P(sy.synergy)
                   << (sy.synergy > 0 ? " (coordination)" : " (redundancy)");
        ss << (n ? "\n" : "no significant interaction\n");
    }
    ss << "\n";

    // ---------------------------------------------------------------------------------------
    // Natural language interpretation
    const PieceInsight* best[COLOR_NB]  = {nullptr, nullptr};
    const PieceInsight* worst[COLOR_NB] = {nullptr, nullptr};
    std::vector<const PieceInsight*> blind[COLOR_NB];
    // Keystone: highest efficiency; Makogonov's worst piece: lowest activity (the relations of
    // the piece are what the network values, its material is the same wherever it stands)
    for (const auto& p : in.pieces)
    {
        const Color c = color_of(p.pc);
        if (p.single < 0 && type_of(p.pc) != PAWN)
            blind[c].push_back(&p);
        if (type_of(p.pc) == PAWN)
            continue;
        if (!best[c] || p.efficiency > best[c]->efficiency)
            best[c] = &p;
        if (!worst[c] || p.activity < worst[c]->activity)
            worst[c] = &p;
    }

    ss << "11. NATURAL LANGUAGE NNUE INTERPRETATION (SCAP Framework)\n";
    ss << "   " << std::string(66, '~') << "\n";
    {
        const int   wpUs = stm == WHITE ? wp : 100 - wp;
        const char* side = color_name(stm);
        ss << "   [ASSESSMENT] ";
        if (wpUs >= 90)
            ss << side << " is winning (" << wpUs << "% expected score): technical conversion.\n";
        else if (wpUs >= 75)
            ss << side << " has a clear advantage (" << wpUs << "%).\n";
        else if (wpUs >= 62)
            ss << side << " is clearly better (" << wpUs << "%): press the advantage.\n";
        else if (wpUs >= 52)
            ss << side << " holds a slight edge (" << wpUs << "%).\n";
        else if (wpUs >= 48)
            ss << "Dynamically balanced position (White " << wp << "%).\n";
        else if (wpUs >= 38)
            ss << side << " is slightly worse (" << wpUs << "%): seek counterplay.\n";
        else if (wpUs >= 25)
            ss << side << " is clearly worse (" << wpUs << "%): find practical resources.\n";
        else
            ss << side << " is losing (" << wpUs << "%): extreme defensive measures.\n";
    }
    ss << "   [PHASE] Sub-network #" << in.bucket << " -> " << phase_name(in.bucket) << ".\n";
    ss << "   [NEURAL SIGNAL] PSQT " << P(in.base.psqt) << " -> total " << P(base) << ": ";
    if (std::abs(cp(in.base.positional)) >= 50)
        ss << "STRONG positional judgment (" << P(in.base.positional)
           << "): the network sees patterns that the material/placement does not explain.\n";
    else if (std::abs(cp(in.base.positional)) >= 15)
        ss << "moderate positional correction (" << P(in.base.positional) << ").\n";
    else
        ss << "the evaluation is mostly material and placement.\n";
    ss << "   [DYNAMICS] Relations " << P(in.familyThreat) << ", pawn structure "
       << P(in.familyPair) << ": "
       << (std::abs(cp(in.familyThreat)) > std::abs(cp(in.familyPair))
             ? "the evaluation is driven by piece activity and tactics (keep the tension).\n"
             : "the evaluation is driven by the pawn structure (long-term factors).\n");
    for (Color c : {WHITE, BLACK})
    {
        if (best[c])
            ss << "   [" << (c == WHITE ? "WHITE" : "BLACK") << " KEYSTONE] "
               << piece_str(best[c]->pc, best[c]->sq) << " " << P(best[c]->value) << " ("
               << best[c]->efficiency << "%): keep it, avoid trading it.\n";
        if (worst[c])
            ss << "   [" << (c == WHITE ? "WHITE" : "BLACK") << " MAKOGONOV] "
               << piece_str(worst[c]->pc, worst[c]->sq) << " (activity " << P(worst[c]->activity)
               << ", efficiency " << worst[c]->efficiency
               << "%): the piece to improve or to trade.\n";
        for (const auto* p : blind[c])
            ss << "   [*** BLINDSPOT/" << (c == WHITE ? "WHITE" : "BLACK") << " ***] "
               << piece_str(p->pc, p->sq) << " has a negative single value (" << P(p->single)
               << "): the network judges " << color_name(c)
               << " better without it (bad piece, overloaded, or in the way).\n";
    }
    // Key relation: the most valuable ATTACK (pressure on an enemy unit). Defences of the
    // initial position (e.g. a rook in the corner defending its pawn) are very common features
    // whose occlusion is less reliable, so they are listed in section 6 but not chosen here.
    auto keyAttack = std::find_if(in.threats.begin(), in.threats.end(), [](const auto& t) {
        return color_of(t.rel.pcA) != color_of(t.rel.pcB);
    });
    if (keyAttack != in.threats.end())
        ss << "   [KEY RELATION] " << relation_label(keyAttack->rel) << " ("
           << P(keyAttack->value) << " for White: "
           << ((keyAttack->value > 0) == (color_of(keyAttack->rel.pcA) == WHITE)
                 ? "pressure that works for its owner"
                 : "an attack that backfires on its owner")
           << ").\n";
    if (!in.pairs.empty())
        ss << "   [KEY PAWN RELATION] " << relation_label(in.pairs.front().rel) << " ("
           << P(in.pairs.front().value) << " for White).\n";
    if (!in.outposts[stm].empty())
    {
        const auto& o = in.outposts[stm].front();
        ss << "   [OUTPOST] " << UCIEngine::square(o.sq) << " is the best square for a "
           << (o.pt == KNIGHT ? "knight" : o.pt == BISHOP ? "bishop" : "rook") << " of "
           << color_name(stm) << " (" << P(o.relative) << " above a typical square).\n";
    }
    for (Color c : {WHITE, BLACK})
        if (!in.synergies[c].empty() && cp(in.synergies[c].front().synergy) >= 10)
        {
            const auto& sy = in.synergies[c].front();
            ss << "   [BATTERY/" << (c == WHITE ? "WHITE" : "BLACK") << "] "
               << piece_str(pos.piece_on(sy.a), sy.a) << "+" << piece_str(pos.piece_on(sy.b), sy.b)
               << " work together (" << P(sy.synergy) << ").\n";
        }
    ss << "   [STRATEGIC PLAN] ";
    if (wp >= 70 || wp <= 30)
        ss << "Technical phase: the side ahead should simplify and target weaknesses.";
    else if (std::abs(cp(in.familyThreat)) > std::abs(cp(in.familyPair)))
        ss << "Dynamic balance: improve the worst piece, keep the key relations, contest the "
              "hottest squares of the pressure map.";
    else
        ss << "Structural balance: play around the key pawn relations (breaks, blockades).";
    ss << "\n";

    // ---------------------------------------------------------------------------------------
    // Compact trace (format unchanged) + extended line with the new information
    auto zone = [](int w) {
        return w >= 95   ? "HighTal"
             : w >= 90   ? "HighMidTal"
             : w >= 85   ? "MidTal"
             : w >= 80   ? "MidLowTal"
             : w >= 76   ? "LowTal"
             : w > 50    ? "Capablanca-Tal"
             : w == 50   ? "Capablanca"
             : w >= 25   ? "Capablanca-Petrosian"
             : w >= 21   ? "LowPetrosian"
             : w >= 16   ? "MidLowPetrosian"
             : w >= 11   ? "MidPetrosian"
             : w >= 6    ? "HighMidPetrosian"
                         : "HighPetrosian";
    };
    ss << "\n   [COMPACT TRACE FORMAT]\n";
    ss << "   WP_White=" << wp << "% | Zone=" << zone(wp) << " | ";
    if (worst[WHITE])
        ss << "WorstWhite=" << piece_str(worst[WHITE]->pc, worst[WHITE]->sq) << "("
           << P(worst[WHITE]->activity) << ") | ";
    if (worst[BLACK])
        ss << "WorstBlack=" << piece_str(worst[BLACK]->pc, worst[BLACK]->sq) << "("
           << P(worst[BLACK]->activity) << ") | ";
    if (!in.outposts[stm].empty())
        ss << "BestOutpost=" << UCIEngine::square(in.outposts[stm].front().sq) << "("
           << P(in.outposts[stm].front().relative) << ") | ";
    if (!blind[WHITE].empty())
        ss << "BLINDSPOT_W=" << piece_str(blind[WHITE].front()->pc, blind[WHITE].front()->sq)
           << " | ";
    if (!blind[BLACK].empty())
        ss << "BLINDSPOT_B=" << piece_str(blind[BLACK].front()->pc, blind[BLACK].front()->sq)
           << " | ";
    ss << "Bucket=" << in.bucket << " | NeuralDelta=" << std::showpos << neuralDelta
       << std::noshowpos << "%\n";
    ss << "   [EXTENDED TRACE]\n   ";
    ss << "Eval=" << P(base) << " | PSQT=" << P(in.base.psqt) << " | Positional="
       << P(in.base.positional) << " | Threats=" << P(in.familyThreat) << " | PawnPairs="
       << P(in.familyPair);
    if (best[WHITE])
        ss << " | KeyWhite=" << piece_str(best[WHITE]->pc, best[WHITE]->sq);
    if (best[BLACK])
        ss << " | KeyBlack=" << piece_str(best[BLACK]->pc, best[BLACK]->sq);
    if (keyAttack != in.threats.end())
        ss << " | KeyRelation=" << relation_label(keyAttack->rel);
    if (!in.pairs.empty())
        ss << " | KeyPawnPair=" << relation_label(in.pairs.front().rel);
    ss << "\n   " << std::string(66, '~') << "\n\n";

    return ss.str();
}

}  // namespace ShashChess::Eval::NNUE
