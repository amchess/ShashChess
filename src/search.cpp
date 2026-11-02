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

#include "search.h"

// [SC:variety] begin
#include <chrono>
#include <cstdint>
#include <random>
// [SC:variety] end

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <list>
#include <mutex>
#include <ratio>
#include <string>
#include <utility>

#include "bitboard.h"
#include "evaluate.h"
#include "history.h"
#include "misc.h"
#include "movegen.h"
#include "movepick.h"
#include "nnue/network.h"
#include "nnue/nnue_accumulator.h"
#include "position.h"
#include "syzygy/tbprobe.h"
#include "thread.h"
#include "timeman.h"
#include "tt.h"
#include "types.h"
#include "uci.h"
#include "ucioption.h"

// [SC:shashchess] begin
#include "wdl/win_probability.h"
#include "learn/learn.h"
#include "mcts/montecarlo.h"
// [SC:livebook] begin
#include "livebook/BaseLivebook.h"
#include "livebook/LichessEndgame.h"
#include "livebook/LichessMaster.h"
#include "livebook/ChessDb.h"
#include "livebook/LichessPlayer.h"
#include "livebook/LichessGames.h"
#include "livebook/Proxy.h"
#include "livebook/ChessDBContributor.h"
// [SC:livebook] end
// [SC:shashchess] end

namespace ShashChess {

using namespace ShashChess::Shashin;

// [SC:learning] begin
bool useLearning          = true;
bool enabledLearningProbe = false;
// NOT thread_local: the trajectory is filled by the main search thread at the end of
// every search, but it is also flushed by the UCI thread on "ucinewgame" and "quit".
// With thread_local each thread had its own copy: the UCI thread always flushed an
// empty vector, so in Self mode nothing was saved unless the game was "decided"
// inside the search (is_game_decided) - e.g. never with GUI adjudications.
std::vector<QLearningMove> qLearningTrajectory;
std::mutex                 qLearningTrajectoryMutex;
// [SC:learning] end

static constexpr std::array<int, 16> lmrDivisor = {3637, 2787, 2761, 2939, 3171, 3347, 3147, 2762,
                                                   2772, 3106, 3107, 3060, 3112, 2991, 3090, 3542};

namespace TB = Tablebases;

void syzygy_extend_pv(const OptionsMap&             options,
                      const Search::LimitsType&     limits,
                      ShashChess::Position&         pos,
                      ShashChess::Search::RootMove& rootMove,
                      Value&                        v,
                      const usize                   multiPV);

using namespace Search;

namespace {

constexpr u64 NODES_LIMIT_OUTPUT    = 10'000'000;
constexpr int SEARCHEDLIST_CAPACITY = 32;
using SearchedList                  = ValueList<Move, SEARCHEDLIST_CAPACITY>;

// (*Scalers):
// The values with Scaler asterisks have proven non-linear scaling.
// They are optimized to time controls of 180 + 1.8 and longer,
// so changing them or adding conditions that are similar requires
// tests at these types of time controls.

// (*Scaler) All tuned parameters at time controls shorter than
// optimized for require verifications at longer time controls.

int correction_value(const Worker& w, const Position& pos, const Stack* const ss) {
    const Color us     = pos.side_to_move();
    const auto  m      = (ss - 1)->currentMove;
    const auto& shared = w.sharedHistory;
    const int   pcv    = shared.pawn_correction_entry(pos)[us].pawn;
    const int   micv   = shared.minor_piece_correction_entry(pos)[us].minor;
    const int   wnpcv  = shared.nonpawn_correction_entry<WHITE>(pos)[us].nonPawnWhite;
    const int   bnpcv  = shared.nonpawn_correction_entry<BLACK>(pos)[us].nonPawnBlack;
    const int   cntcv =
      m.is_ok()
          ? 8761
            * ((*(ss - 2)->continuationCorrectionHistory)[pos.piece_on(m.to_sq())][m.to_sq()]
               + (*(ss - 4)->continuationCorrectionHistory)[pos.piece_on(m.to_sq())][m.to_sq()])
          : 64049;

    return 15341 * pcv + 10569 * micv + 12906 * (wnpcv + bnpcv) + cntcv;
}

// Add correctionHistory value to raw staticEval and guarantee evaluation
// does not hit the tablebase range.
Value to_corrected_static_eval(const Value v, const int cv) {
    return std::clamp(v + cv / 131072, VALUE_TB_LOSS_IN_MAX_PLY + 1, VALUE_TB_WIN_IN_MAX_PLY - 1);
}

void update_correction_history(const Position& pos,
                               Stack* const    ss,
                               Search::Worker& workerThread,
                               const int       bonus) {
    const Move  m  = (ss - 1)->currentMove;
    const Color us = pos.side_to_move();

    constexpr int nonPawnWeight = 186;
    auto&         shared        = workerThread.sharedHistory;

    shared.pawn_correction_entry(pos)[us].pawn << bonus;
    shared.minor_piece_correction_entry(pos)[us].minor << bonus * 150 / 128;
    shared.nonpawn_correction_entry<WHITE>(pos)[us].nonPawnWhite << bonus * nonPawnWeight / 128;
    shared.nonpawn_correction_entry<BLACK>(pos)[us].nonPawnBlack << bonus * nonPawnWeight / 128;

    if (m.is_ok())
    {
        const Square to = m.to_sq();
        const Piece  pc = pos.piece_on(to);
        (*(ss - 2)->continuationCorrectionHistory)[pc][to] << bonus * 130 / 128;
        (*(ss - 4)->continuationCorrectionHistory)[pc][to] << bonus * 70 / 128;
    }
}

// Add a small random component to draw evaluations to avoid 3-fold blindness
Value value_draw(usize nodes) { return VALUE_DRAW - 1 + Value(nodes & 0x2); }

int variety;  // variety

Value value_to_tt(Value v, int ply);
Value value_from_tt(Value v, int ply, int r50c);
void  update_continuation_histories(Stack* ss, Piece pc, Square to, int bonus);
void  update_quiet_histories(
   const Position& pos, Stack* ss, Search::Worker& workerThread, Move move, int bonus);
void update_all_stats(const Position& pos,
                      Stack*          ss,
                      Search::Worker& workerThread,
                      Move            bestMove,
                      Square          prevSq,
                      SearchedList&   quietsSearched,
                      SearchedList&   capturesSearched,
                      Depth           depth,
                      Move            ttMove,
                      bool            PvNode);

// Detect shuffling moves in order to limit search explosions. This code was
// added in #6447 as non-regression, and so its parameters should not be tuned.
bool is_shuffling(Move move, Stack* const ss, const Position& pos) {
    if (pos.capture_stage(move) || pos.rule50_count() < 10)
        return false;
    if (pos.state()->pliesFromNull < 6 || ss->ply < 20)
        return false;
    return move.from_sq() == (ss - 2)->currentMove.to_sq()
        && (ss - 2)->currentMove.from_sq() == (ss - 4)->currentMove.to_sq();
}

}  // namespace

// [SC:livebook] begin
#ifdef USE_LIVEBOOK
int livebook_depth_count = 0;
int max_book_depth       = 255;

std::vector<Livebook::BaseLivebook*> opening_livebooks, endgame_livebooks;

std::string _proxy_url;
bool        _use_lichess_games   = false;
bool        _use_lichess_masters = false;
std::string _lichess_player;
std::string _lichess_player_color;
bool        _use_chess_db = false;

bool _use_chess_db_tablebase = false;
bool _use_lichess_tablebase  = false;

bool _chess_db_contribute = false;
bool _proxy_diversity     = false;

auto contributor = Livebook::ChessDBContributor();

void Search::set_livebook_depth(const int book_depth) { max_book_depth = book_depth; }

void Search::set_proxy_url(const std::string& proxy_url) {
    _proxy_url = proxy_url;
    update_livebooks();
}

void Search::set_use_lichess_games(const bool lichess_games) {
    _use_lichess_games = lichess_games;
    update_livebooks();
}

void Search::set_use_lichess_masters(const bool lichess_masters) {
    _use_lichess_masters = lichess_masters;
    update_livebooks();
}

void Search::set_lichess_player(const std::string& lichess_player) {
    _lichess_player = lichess_player;
    update_livebooks();
}

void Search::set_lichess_player_color(const std::string& lichess_player_color) {
    _lichess_player_color = lichess_player_color;
    update_livebooks();
}

void Search::set_use_chess_db(const bool chess_db) {
    _use_chess_db = chess_db;
    update_livebooks();
}

void Search::set_use_chess_db_tablebase(const bool chess_db) {
    _use_chess_db_tablebase = chess_db;
    update_online_tablebases();
}

void Search::set_use_lichess_tablebase(const bool lichess_tablebase) {
    _use_lichess_tablebase = lichess_tablebase;
    update_online_tablebases();
}

void Search::update_livebooks() {
    opening_livebooks.clear();

    if (!_proxy_url.empty())
    {
        Livebook::Proxy* proxy = new Livebook::Proxy(_proxy_url);
        if (_proxy_diversity)
            proxy->set_action(Livebook::Action::QUERY);
        else
            proxy->set_action(Livebook::Action::QUERY_BEST);
        opening_livebooks.push_back(proxy);
    }

    if (!_lichess_player.empty())
    {
        if (_lichess_player_color.empty())
            _lichess_player_color = "white";
        opening_livebooks.push_back(
          new Livebook::LichessPlayer(_lichess_player, _lichess_player_color));
    }

    if (_use_lichess_games)
        opening_livebooks.push_back(new Livebook::LichessGames());

    if (_use_lichess_masters)
        opening_livebooks.push_back(new Livebook::LichessMaster());

    if (_use_chess_db)
        opening_livebooks.push_back(new Livebook::ChessDb());
}

void Search::update_online_tablebases() {
    endgame_livebooks.clear();

    if (_use_chess_db_tablebase)
        endgame_livebooks.push_back(new Livebook::ChessDb());

    if (_use_lichess_tablebase)
        endgame_livebooks.push_back(new Livebook::LichessEndgame());
}

void Search::set_chess_db_contribute(const bool chess_db_contribute) {
    _chess_db_contribute = chess_db_contribute;
}

void Search::set_proxy_diversity(const bool proxy_diversity) {
    _proxy_diversity = proxy_diversity;
    update_livebooks();
}
#endif
// [SC:livebook] end

// [SC:variety] begin
void Search::set_variety(const std::string& varietyOption) {
    if (varietyOption == "Off")
        variety = 0;
    else if (varietyOption == "Standard")
        variety = 1;
    else if (varietyOption == "Psychological")
        variety = 2;
}
// [SC:variety] end

// [SC:learning] begin
inline bool is_game_decided(const Position& pos, Value lastScore) {
    static constexpr const Value DecidedGameEvalThreeshold = PawnValue * 5;
    static constexpr const int   DecidedGameMaxPly         = 150;
    static constexpr const int   DecidedGameMaxPieceCount  = 5;

    if (is_valid(lastScore) && std::abs(lastScore) > DecidedGameEvalThreeshold)
        return true;

    if (pos.game_ply() > DecidedGameMaxPly)
        return true;

    if (pos.count<ALL_PIECES>() < DecidedGameMaxPieceCount)
        return true;

    return false;
}
// [SC:learning] end


Search::Worker::Worker(SharedState&                    sharedState,
                       std::unique_ptr<ISearchManager> sm,
                       usize                           threadId,
                       usize                           numaThreadId,
                       usize                           numaTotalThreads,
                       NumaReplicatedAccessToken       token) :
    // Unpack the SharedState struct into member variables
    sharedHistory(sharedState.sharedHistories.at(token.get_numa_index())),
    continuationHistory(sharedHistory.continuationHistory()),
    threadIdx(threadId),
    numaThreadIdx(numaThreadId),
    numaTotal(numaTotalThreads),
    numaAccessToken(token),
    manager(std::move(sm)),
    bookMan(sharedState.bookMan),
    options(sharedState.options),
    threads(sharedState.threads),
    tt(sharedState.tt),
    network(sharedState.network),
    refreshTable(network[token]),
    shashinManager(std::make_unique<ShashinManager>()),
    shConfig(sharedState.shashinConfig) {
    clear();
}

Search::Worker::~Worker() = default;
ShashinManager& Worker::getShashinManager() { return *shashinManager; }

void Search::Worker::ensure_network_replicated() {
    // Access once to force lazy initialization, avoiding initialization during search
    (void) (network[numaAccessToken]);
}

void Search::Worker::start_searching() {

    accumulatorStack.reset();

    // [SC] Shashin roles. One thread analyses the root (static state: sacrifices, pawns
    // near promotion, king danger, fortress, mobility) and follows the WDL zone of its own
    // search: the main thread when it is alone (hard positions, suite), otherwise the
    // last helper, so that the main thread, which drives time management, keeps a search
    // close to Stockfish 19. The other threads follow only the WDL zone of the root, as
    // the helpers of ShashChess 41 (initDynamicRootShashinState in iterative_deepening).
    // With the static flags on every thread, null move and futility of the whole pool
    // were driven by the "hot" root flags, true in most game positions.
    // With "MCTS by Shashin" the helpers 1..MCTSThreads may run MCTS instead of alpha-beta:
    // if the last helper is one of them, the main thread analyses the root.
    usize rootAnalyst = threads.size() - 1;
    if (bool(options["MCTS by Shashin"]) && rootAnalyst <= usize(int(options["MCTSThreads"])))
        rootAnalyst = 0;

    shashinRootAnalysis = threadIdx == rootAnalyst;

    if (shashinRootAnalysis)
        shashinManager->setStaticState(rootPos);
    else
        shashinManager->setNeutralStaticState();

    // Non-main threads go directly to iterative_deepening()
    if (!is_mainthread())
    {
        iterative_deepening();
        return;
    }

    main_manager()->tm.init(limits, rootPos.side_to_move(), rootPos.game_ply(), options,
                            main_manager()->originalTimeAdjust);
    tt.new_search();
    main_manager()->updates.onStart();

    enabledLearningProbe = false;
    useLearning          = true;
    set_variety(options["Variety"]);

    Move bookMove = Move::none();

    if (rootMoves.empty())
    {
        main_manager()->updates.onUpdateNoMoves(
          {0, {rootPos.checkers() ? -VALUE_MATE : VALUE_DRAW, rootPos}});
        main_manager()->updates.onBestmove(UCIEngine::move(Move::none()), "");
        return;
    }
    else
    {
        bool think = true;
        if (!(limits.infinite || limits.mate || limits.depth || limits.nodes || limits.perft)
            && !main_manager()->ponder)
        {
            bookMove = bookMan.probe(rootPos, options);
            if (bookMove == Move::none() && (bool) options["Experience Book"]
                && rootPos.game_ply() / 2 < (int) options["Experience Book Max Moves"])
            {
                Depth expBookMinDepth = (Depth) options["Experience Book Min Depth"];
                std::vector<LearningMove*> learningMoves = LD.probe(rootPos.key());
                if (!learningMoves.empty())
                {
                    LD.sortLearningMoves(learningMoves);
                    std::vector<LearningMove*> bestMoves;
                    Depth                      bestDepth = learningMoves[0]->depth;
                    if (bestDepth >= expBookMinDepth)
                    {
                        int   bestPerformance = learningMoves[0]->performance;
                        Value bestScore       = learningMoves[0]->score;
                        if (bestPerformance >= 50)
                        {
                            for (const auto& move : learningMoves)
                            {
                                if (move->depth == bestDepth && move->performance == bestPerformance
                                    && move->score == bestScore)
                                    bestMoves.push_back(move);
                                else
                                    break;
                            }
                            if (!bestMoves.empty())
                            {
                                std::random_device              rd;
                                std::mt19937                    gen(rd());
                                std::uniform_int_distribution<> distr(0, bestMoves.size() - 1);
                                bookMove = bestMoves[distr(gen)]->move;
                            }
                        }
                    }
                }
            }
            if (bookMove != Move::none()
                && std::find(rootMoves.begin(), rootMoves.end(), bookMove) != rootMoves.end())
            {
                think = false;
                for (auto&& th : threads)
                    std::swap(th->worker->rootMoves[0],
                              *std::find(th->worker->rootMoves.begin(), th->worker->rootMoves.end(),
                                         bookMove));
            }
        }

#ifdef USE_LIVEBOOK
        if (think && !bookMove)
        {
            if (!limits.infinite && !limits.mate)
            {
                livebook_depth_count = rootPos.game_ply();
                std::vector<Livebook::BaseLivebook*> livebooks;

                const std::string& syzygyPath = static_cast<std::string>(options["SyzygyPath"]);
                int                totalUnits = rootPos.count<ALL_PIECES>();

                if ((totalUnits <= 7)
                    && (syzygyPath.empty()
                        || ((!syzygyPath.empty())
                            && ((int(options["SyzygyProbeLimit"]) < totalUnits)))))
                {
                    livebooks = endgame_livebooks;
                }
                else if (livebook_depth_count < max_book_depth)
                {
                    livebooks = opening_livebooks;
                }

                for (const auto livebook : livebooks)
                {
                    if (std::vector<std::pair<std::string, Analysis>> output =
                          livebook->lookup(rootPos);
                        !output.empty())
                    {
                        std::string     uci;
                        const Analysis* best = nullptr;

                        for (auto& [move, analysis] : output)
                        {
                            if (best == nullptr || analysis > *best)
                            {
                                best = &analysis;
                                uci  = move;
                            }
                        }

                        if (best != nullptr)
                        {
                            bookMove = UCIEngine::to_move(rootPos, uci);
                            if (bookMove)
                                livebook_depth_count++;
                        }
                        break;
                    }
                }
            }

            if (bookMove && std::count(rootMoves.begin(), rootMoves.end(), bookMove))
            {
                think = false;
                for (auto&& th : threads)
                {
                    auto& rm = th->worker->rootMoves;
                    auto  it = std::find(rm.begin(), rm.end(), bookMove);
                    if (it != rm.end())
                        std::swap(rm[0], *it);
                }
            }
            else
            {
                bookMove = Move::none();
            }
        }
#endif

        bool uciPvSent = false;
        if (!bookMove && think)
        {
            mctsThreads        = size_t(int(options["MCTSThreads"]));
            mctsMultiStrategy  = size_t(int(options["MCTS Multi Strategy"]));
            mctsMultiMinVisits = double(int(options["MCTS Multi MinVisits"]));

            threads.start_searching();
            uciPvSent = iterative_deepening();
        }

        while (!threads.stop && (main_manager()->ponder || limits.infinite))
        {}
        threads.stop = true;
        threads.wait_for_search_finished();

        if (limits.npmsec)
            main_manager()->tm.advance_nodes_time(threads.nodes_searched()
                                                  - limits.inc[rootPos.side_to_move()]);

        Worker* bestThread = this;
        Skill   skill =
          Skill(options["Skill Level"], options["UCI_LimitStrength"] ? int(options["UCI_Elo"]) : 0);

        if (!limits.depth && !skill.enabled() && !bookMove)
            bestThread = threads.get_best_thread()->worker.get();

        main_manager()->bestPreviousScore        = bestThread->rootMoves[0].score;
        main_manager()->bestPreviousAverageScore = bestThread->rootMoves[0].averageScore;

        if (!bookMove)
        {
            if (bestThread->completedDepth > 4 && LD.is_enabled() && !LD.is_paused())
            {
                PersistedLearningMove plm;
                plm.key                = rootPos.key();
                plm.learningMove.depth = bestThread->completedDepth;
                plm.learningMove.move  = bestThread->rootMoves[0].pv[0];
                plm.learningMove.score = bestThread->rootMoves[0].score;
                plm.learningMove.performance =
                  WDLModel::get_win_probability(bestThread->rootMoves[0].score, rootPos);

                if (LD.learning_mode() == LearningMode::Self)
                {
                    const LearningMove* existingMove =
                      LD.probe_move(plm.key, plm.learningMove.move);
                    if (existingMove)
                        plm.learningMove.score = existingMove->score;
                    QLearningMove qLearningMove;
                    qLearningMove.persistedLearningMove = plm;
                    const int qLearningMoveMaterial =
                      rootPos.count<PAWN>() + 3 * rootPos.count<KNIGHT>()
                      + 3 * rootPos.count<BISHOP>() + 5 * rootPos.count<ROOK>()
                      + 9 * rootPos.count<QUEEN>();
                    qLearningMove.materialClamp = std::clamp(qLearningMoveMaterial, 17, 78);
                    std::lock_guard<std::mutex> lock(qLearningTrajectoryMutex);
                    qLearningTrajectory.push_back(qLearningMove);
                }
                else
                {
                    LD.add_new_learning(plm.key, plm.learningMove);
                }
            }
            if (!enabledLearningProbe)
                useLearning = false;

            if (is_game_decided(rootPos, (bestThread->rootMoves[0].score)) && LD.is_enabled()
                && !LD.is_paused())
            {
                if (LD.learning_mode() == LearningMode::Self)
                    putQLearningTrajectoryIntoLearningTable();

                if (!LD.is_readonly())
                    LD.persist(options);
                LD.pause();
            }
        }

        if (bestThread->rootMoves[0].pv.size() == 1
            && bestThread->rootMoves[0].extract_ponder_from_tt(tt, rootPos) && !bookMove)
            uciPvSent = false;

        if (!bookMove && (!uciPvSent || bestThread != this))
            main_manager()->output_pv(*bestThread, threads, tt, bestThread->rootDepth);

        std::string ponder;
        if (bestThread->rootMoves[0].pv.size() > 1)
            ponder = UCIEngine::move(bestThread->rootMoves[0].pv[1], rootPos.is_chess960());

        auto bestmove = UCIEngine::move(bestThread->rootMoves[0].pv[0], rootPos.is_chess960());
        main_manager()->updates.onBestmove(bestmove, ponder);

#ifdef USE_LIVEBOOK
        if (_chess_db_contribute && !bookMove)
            contributor.contribute(rootPos, bestThread->rootMoves[0].pv[0]);
#endif
    }
}

bool Search::Worker::iterative_deepening() {
    auto&          localShashinManager = *this->shashinManager;
    SearchManager* mainThread          = (is_mainthread() ? main_manager() : nullptr);

    PVMoves pv;

    RootPVMoves lastBestMovePV;
    Depth       lastBestMoveDepth = 0;
    Value       lastBestMoveScore = -VALUE_INFINITE;

    Value  alpha, beta;
    Value  bestValue     = -VALUE_INFINITE;
    Color  us            = rootPos.side_to_move();
    double timeReduction = 1, totBestMoveChanges = 0;
    int    delta, iterIdx                        = 0;

    Stack  stack[MAX_PLY + 10] = {};
    Stack* ss                  = stack + 7;

    for (int i = 7; i > 0; --i)
    {
        (ss - i)->continuationHistory           = &continuationHistory[0][0][NO_PIECE][0];
        (ss - i)->continuationCorrectionHistory = &continuationCorrectionHistory[NO_PIECE][0];
        (ss - i)->staticEval                    = VALUE_NONE;
    }

    for (int i = 0; i <= MAX_PLY + 2; ++i)
        (ss + i)->ply = i;

    ss->pv = &pv;

    // New searcha: the output_pv() guard restarts from zero.
    lastShashinUpdatedDepth = 0;

    localShashinManager.initDynamicRootShashinState(network[numaAccessToken], accumulatorStack,
                                                    refreshTable, rootPos, ss, VALUE_ZERO, shConfig,
                                                    rootDepth);

    const RootShashinState& rootShashinState = localShashinManager.getState();
    const auto&             dynamicDerived   = rootShashinState.dynamicDerived;
    bool                    isAggressive     = localShashinManager.isAggressive();
    bool                    isHighTal        = dynamicDerived.isHighTal;

    if (mainThread)
    {
        if (mainThread->bestPreviousScore == VALUE_INFINITE)
            mainThread->iterValue.fill(VALUE_ZERO);
        else
            mainThread->iterValue.fill(mainThread->bestPreviousScore);
    }

    usize multiPV   = usize(options["MultiPV"]);
    antiDancingMode = bool(options["Anti Dancing Analysis"]);

    Skill skill(options["Skill Level"], options["UCI_LimitStrength"] ? int(options["UCI_Elo"]) : 0);

    // When playing with strength handicap enable MultiPV search that we will
    // use behind-the-scenes to retrieve a set of possible moves.
    if (skill.enabled())
        multiPV = std::max(multiPV, usize(4));

    multiPV = std::min(multiPV, rootMoves.size());

    int  searchAgainCounter = 0;
    bool uciPvSent          = false;

    lowPlyHistory.fill(102);

    for (Color c : {WHITE, BLACK})
        for (int i = 0; i < UINT_16_HISTORY_SIZE; i++)
            mainHistory[c][i] = mainHistory[c][i] * 729 / 1024;

    optimism[WHITE] = optimism[BLACK] = VALUE_ZERO;

    if (bool(options["MCTS by Shashin"]))
    {
        bool  maybeDraw = rootPos.rule50_count() >= 90 || rootPos.upcoming_repetition(2);
        Value rootPosValue =
          localShashinManager.static_value(network[numaAccessToken], accumulatorStack, refreshTable,
                                           rootPos, ss, optimism[rootPos.side_to_move()]);
        bool isMCTSExplorationApplicable = localShashinManager.isMCTSExplorationApplicable();
        bool mctsExplore                 = options["MCTS Explore"]
                        && (isMCTSExplorationApplicable
                            || localShashinManager.isHighPieceDensityCapablancaPosition());

        bool possibleMCTSByValue = localShashinManager.isMCTSApplicableByValue();
        if (!mainThread && multiPV == 1 && !maybeDraw && (possibleMCTSByValue || mctsExplore)
            && (rootPos.count<ALL_PIECES>() > 7) && (this->threadIdx) <= (usize) (mctsThreads)
            && !is_game_decided(rootPos, rootPosValue))
        {
            MonteCarlo* monteCarlo = new MonteCarlo(rootPos, this, tt);
            if (monteCarlo)
            {
                monteCarlo->search(threads, limits, is_mainthread(), this);
                if ((this->threadIdx) == 1 && limits.infinite
                    && threads.stop.load(std::memory_order_relaxed))
                    monteCarlo->print_children();
                delete monteCarlo;
                return uciPvSent;
            }
        }
    }
    // Iterative deepening loop until requested to stop or the target depth is reached
    while (rootDepth + 1 < MAX_PLY && !threads.stop
           && !(limits.depth && mainThread && rootDepth >= limits.depth))
    {
        rootDepth++;

        // Age out PV variability metric and signal the start of a new iteration
        if (mainThread)
        {
            totBestMoveChanges /= 2;
            uciPvSent = false;
        }

        // Save the last iteration's scores before the first PV line is searched and
        // all the move scores except the (new) PV are set to -VALUE_INFINITE.
        for (usize i = 0; i < rootMoves.size(); ++i)
        {
            rootMoves[i].previousScore      = rootMoves[i].score;
            rootMoves[i].previousPV         = rootMoves[i].pv;
            rootMoves[i].previousScoreExact = i < multiPV;
        }

        usize pvFirst = pvLast = 0;

        if (!threads.increaseDepth)
            searchAgainCounter++;

        // MultiPV loop: we perform a full root search for each PV line
        for (pvIdx = 0; pvIdx < multiPV; ++pvIdx)
        {
            if (pvIdx == pvLast)
            {
                pvFirst = pvLast;
                for (pvLast++; pvLast < rootMoves.size(); pvLast++)
                    if (rootMoves[pvLast].tbRank != rootMoves[pvFirst].tbRank)
                        break;
            }

            lastIterationIdxPV = rootMoves[pvIdx].previousPV;

            // Reset UCI info selDepth for each depth and each PV line
            selDepth = 0;

            // Reset aspiration window starting size
            delta = 5 + threadIdx % 8 + std::abs(rootMoves[pvIdx].meanSquaredScore) / 10193;

            // [SC] Only Tal widens the window (+12.5%), as in ShashChess 41; otherwise the
            // Stockfish 19 window. The narrower window for Capablanca/fortress belonged to
            // the Gold Digger build only, the cap at depth < 10 was not style dependent.
            if (isAggressive || isHighTal)
                delta = (delta * 9) / 8;

            Value avg = rootMoves[pvIdx].averageScore;
            alpha     = std::max(avg - delta, -VALUE_INFINITE);
            beta      = std::min(avg + delta, VALUE_INFINITE);

            // Adjust optimism based on root move's averageScore
            optimism[us]  = 114 * avg / (std::abs(avg) + 85);
            optimism[~us] = -optimism[us];

            // Start with a small aspiration window and, in the case of a fail
            // high/low, enlarge the window progressively.
            int failedHighCnt = 0;
            while (true)
            {
                // Adjust the effective depth searched, but ensure at least one
                // effective increment for every four searchAgain steps (see issue #2717).
                Depth adjustedDepth =
                  std::max(1, rootDepth - failedHighCnt - 3 * (searchAgainCounter + 1) / 4);
                rootDelta = beta - alpha;
                bestValue = search<Root>(rootPos, ss, alpha, beta, adjustedDepth, false);

                // Bring the best move to the front. It is critical that sorting
                // is done with a stable algorithm because all the values but the
                // first and eventually the new best one is set to -VALUE_INFINITE
                // and we want to keep the same order for all the moves except the
                // new PV that goes to the front. Note that in the case of MultiPV
                // search the already searched PV lines are preserved.
                std::stable_sort(rootMoves.begin() + pvIdx, rootMoves.begin() + pvLast);

                // If search has been stopped, we break immediately. Sorting is
                // safe because RootMoves is still valid, although it refers to
                // the previous iteration.
                if (threads.stop)
                    break;

                // When failing high/low give some update before a re-search. To avoid
                // excessive output that could hang GUIs like Fritz 19, only start
                // at nodes > 10M (rather than depth N, which can be reached quickly).
                if (mainThread && multiPV == 1 && (bestValue <= alpha || bestValue >= beta)
                    && nodes > NODES_LIMIT_OUTPUT)
                    main_manager()->output_pv(*this, threads, tt, rootDepth);

                // In case of failing low/high increase aspiration window and re-search,
                // otherwise exit the loop.
                if (bestValue <= alpha)
                {
                    beta  = alpha;
                    alpha = std::max(bestValue - delta, -VALUE_INFINITE);

                    failedHighCnt = 0;
                    if (mainThread)
                        mainThread->stopOnPonderhit = false;
                }
                else if (bestValue >= beta)
                {
                    alpha = std::max(beta - delta, alpha);
                    beta  = std::min(bestValue + delta, VALUE_INFINITE);
                    ++failedHighCnt;
                }
                else
                    break;

                delta += 47 * delta / 128;

                assert(alpha >= -VALUE_INFINITE && beta <= VALUE_INFINITE);
            }

            if (threads.stop && pvIdx)
            {
                // In multiPV analysis we do not let aborted searches spoil
                // mated-in/TB loss scores from a completed search in an earlier
                // PV line. Hence we guard against an aborted pvIdx line overtaking
                // pvIdx - 1 when pvIdx - 1 is a proven loss. Moreover, we do not
                // trust an exact loss score from an aborted search.
                if ((is_loss(rootMoves[pvIdx - 1].score) && rootMoves[pvIdx] < rootMoves[pvIdx - 1])
                    || rootMoves[pvIdx].is_exact_loss())
                {
                    // If previousScore is exact and worse than pvIdx - 1, we
                    // can safely use it. If it is equal, we make sure it cannot
                    // overtake pvIdx - 1.
                    if (rootMoves[pvIdx].previousScore != -VALUE_INFINITE
                        && rootMoves[pvIdx].previousScoreExact
                        && rootMoves[pvIdx].previousScore <= rootMoves[pvIdx - 1].score)
                    {
                        rootMoves[pvIdx].score = rootMoves[pvIdx].uciScore =
                          rootMoves[pvIdx].previousScore;
                        rootMoves[pvIdx].previousScore = -VALUE_INFINITE;
                        rootMoves[pvIdx].pv            = rootMoves[pvIdx].previousPV;
                        rootMoves[pvIdx].unset_inexact();
                    }

                    // Otherwise, if we can, we cap the score to the best possible, and mark
                    // the score as inexact (also a valid excuse for the incomplete PV).
                    else
                    {
                        if (is_loss(rootMoves[pvIdx - 1].score))
                        {
                            rootMoves[pvIdx].score = rootMoves[pvIdx].uciScore =
                              rootMoves[pvIdx - 1].score;
                            rootMoves[pvIdx].previousScore = -VALUE_INFINITE;
                            rootMoves[pvIdx].pv.resize(1);
                            rootMoves[pvIdx].inexactUpper = true;
                        }
                        else
                            rootMoves[pvIdx].inexactUpper = false;

                        rootMoves[pvIdx].inexactLower = !rootMoves[pvIdx].inexactUpper;
                    }
                }

                // Finally, we mark all loss scores from partially searched moves as inexact.
                for (usize i = pvIdx + 1; i < multiPV; ++i)
                    if (rootMoves[i].is_exact_loss())
                        rootMoves[i].inexactLower = true;
            }

            // Sort the PV lines searched so far and update the GUI
            std::stable_sort(rootMoves.begin() + pvFirst, rootMoves.begin() + pvIdx + 1);

            if (mainThread && !threads.stop && (pvIdx + 1 == multiPV || nodes > NODES_LIMIT_OUTPUT))
            {
                main_manager()->output_pv(*this, threads, tt, rootDepth);
                uciPvSent = (pvIdx + 1 == multiPV);
            }

            if (threads.stop)
                break;
        }

        const bool forgottenMate = lastBestMoveScore != -VALUE_INFINITE
                                && is_mate_or_mated(lastBestMoveScore)
                                && (std::abs(rootMoves[0].score) < std::abs(lastBestMoveScore)
                                    || rootMoves[0].is_inexact());

        if (!threads.stop)
        {
            completedDepth = rootDepth;  // [SC] learning + MCTS

            // [SC] A helper that analyses the root follows the WDL zone of its own search
            // after every completed iteration (for the main thread output_pv() does it)
            if (shashinRootAnalysis && !mainThread && rootDepth > lastShashinUpdatedDepth
                && !rootMoves[0].is_inexact())
            {
                localShashinManager.updateRootShashinState(rootMoves[0].uciScore, rootPos,
                                                           rootDepth, rootDepth);
                lastShashinUpdatedDepth = rootDepth;
            }

            if (lastBestMovePV.empty() || lastBestMovePV[0] != rootMoves[0].pv[0])
                lastBestMoveDepth = rootDepth;

            // Do not replace (shorter) mate scores from a previous iteration
            if (!forgottenMate)
            {
                lastBestMovePV    = rootMoves[0].pv;
                lastBestMoveScore = rootMoves[0].score;
            }
        }

        const bool abortedLossSearch = threads.stop && !pvIdx && rootMoves[0].is_exact_loss();

        // An exact mated-in/TB-loss score from an aborted search cannot be
        // trusted: the loss could be delayed or refuted upon exploring the
        // remaining root-moves. Thus here we roll back to the score from the
        // previous iteration. We do the same if a search has failed to recover
        // a mate score that was found in a previous iteration.
        if (abortedLossSearch || (rootMoves[0].score != -VALUE_INFINITE && forgottenMate))
        {
            // Bring the last best move to the front for best thread selection
            if (!lastBestMovePV.empty())
            {
                Utility::move_to_front(rootMoves, [&lastPV = std::as_const(lastBestMovePV)](
                                                    const auto& rm) { return rm == lastPV[0]; });
                rootMoves[0].score = rootMoves[0].uciScore = lastBestMoveScore;
                rootMoves[0].pv                            = lastBestMovePV;
                rootMoves[0].unset_inexact();

                if (mainThread)
                    uciPvSent = false;
            }
            // For an aborted d1 search we label the loss score as inexact
            else if (abortedLossSearch)
                rootMoves[0].inexactLower = true;
        }

        // Have we found a "mate in x" after a completed iteration?
        if (limits.mate && !threads.stop && is_mate_or_mated(rootMoves[0].score)
            && VALUE_MATE - std::abs(rootMoves[0].score) <= 2 * limits.mate)
            threads.stop = true;

        if (!mainThread)
            continue;

        // If the skill level is enabled and time is up, pick a sub-optimal best move
        if (skill.enabled() && skill.time_to_pick(rootDepth))
            skill.pick_best(rootMoves, multiPV);

        // Use part of the gained time from a previous stable move for the current move
        for (auto&& th : threads)
        {
            totBestMoveChanges += th->worker->bestMoveChanges;
            th->worker->bestMoveChanges = 0;
        }

        // Do we have time for the next iteration? Can we stop searching now?
        if (limits.use_time_management() && !threads.stop && !mainThread->stopOnPonderhit)
        {
            u64 nodesEffort = rootMoves[0].effort * 100000 / std::max(u64(1), u64(nodes));

            double fallingEval = (11.48 + 2.30 * (mainThread->bestPreviousAverageScore - bestValue)
                                  + 1.1 * (mainThread->iterValue[iterIdx] - bestValue))
                               / 100.0;
            fallingEval = std::clamp(fallingEval, 0.576, 1.728);

            // If the bestMove is stable over several iterations, reduce time accordingly
            timeReduction = std::clamp(
              interpolate(double(rootDepth - lastBestMoveDepth), 4.96, 18.79, 0.639, 1.712), 0.629,
              1.544);

            double reduction =
              (1.468 + mainThread->previousTimeReduction) / (2.284 * timeReduction);

            double bestMoveInstability = 1.077 + 2.229 * totBestMoveChanges / threads.size();

            double highBestMoveEffort = std::clamp(
              interpolate(i64(nodesEffort), i64(75800), i64(104510), 0.969, 0.714), 0.693, 0.838);

            double totalTime = mainThread->tm.optimum() * fallingEval * reduction
                             * bestMoveInstability * highBestMoveEffort;

            if (rootMoves.size() == 1)
                // Cap used time to 0.5s for a better viewer experience
                totalTime = std::min(500.0, totalTime);

            auto elapsedTime = elapsed();

            // Stop the search if we have exceeded totalTime or maximum time,
            // or if we know that there are no better moves in the analysed line(s).
            if (elapsedTime > std::min(totalTime, double(mainThread->tm.maximum()))
                || rootMoves[multiPV - 1].score >= mate_in(3) || rootMoves[0].score == mated_in(2))
            {
                // If we are allowed to ponder do not stop the search now but
                // keep pondering until the GUI sends "ponderhit" or "stop".
                if (mainThread->ponder)
                    mainThread->stopOnPonderhit = true;
                else
                    threads.stop = true;
            }
            else
                threads.increaseDepth = mainThread->ponder || elapsedTime <= totalTime * 0.50;
        }

        mainThread->iterValue[iterIdx] = bestValue;
        iterIdx                        = (iterIdx + 1) & 3;
    }

    if (!mainThread)
        return false;

    mainThread->previousTimeReduction = timeReduction;

    // If the skill level is enabled, swap the best PV line with the sub-optimal one
    if (skill.enabled())
        std::swap(rootMoves[0],
                  *std::find(rootMoves.begin(), rootMoves.end(),
                             skill.best ? skill.best : skill.pick_best(rootMoves, multiPV)));

    return uciPvSent;
}

void Search::Worker::do_move(Position& pos, const Move move, StateInfo& st, Stack* const ss) {
    do_move(pos, move, st, pos.gives_check(move), ss);
}

void Search::Worker::do_move(
  Position& pos, const Move move, StateInfo& st, const bool givesCheck, Stack* const ss) {

    // prefetch_key() does not model castling, en passant or promotion exactly.
    // The correction-history prefetches also approximate castling and promotion.
    // For these rare moves the prefetches land on unused lines.
    prefetch(tt.first_entry(pos.prefetch_key(move)));

    bool capture = pos.capture_stage(move);

    if (ss != nullptr)
    {
        const Piece  pc = pos.moved_piece(move);
        const Square to = move.to_sq();

        prefetch(&(*(ss - 1)->continuationCorrectionHistory)[pc][to]);
        prefetch(&(*(ss - 3)->continuationCorrectionHistory)[pc][to]);
    }

    ++nodes;

    Dirties& dirties = accumulatorStack.push();
    pos.do_move(move, st, givesCheck, dirties, &tt, &sharedHistory);

    if (ss != nullptr)
    {
        auto& dirtyPiece = dirties.dirtyPiece;
        ss->currentMove  = move;
        ss->continuationHistory =
          &continuationHistory[ss->inCheck][capture][dirtyPiece.pc][move.to_sq()];
        ss->continuationCorrectionHistory =
          &continuationCorrectionHistory[dirtyPiece.pc][move.to_sq()];
    }
}

void Search::Worker::do_null_move(Position& pos, StateInfo& st, Stack* const ss) {
    pos.do_null_move(st);
    ss->currentMove                   = Move::null();
    ss->continuationHistory           = &continuationHistory[0][0][NO_PIECE][0];
    ss->continuationCorrectionHistory = &continuationCorrectionHistory[NO_PIECE][0];
}

void Search::Worker::undo_move(Position& pos, const Move move) {
    pos.undo_move(move);
    accumulatorStack.pop();
}

void Search::Worker::undo_null_move(Position& pos) { pos.undo_null_move(); }


// Reset histories, usually before a new game
void Search::Worker::clear() {
    mainHistory.fill(-5);
    captureHistory.fill(-742);

    // Each thread clears its part of the dynamically-sized shared histories.
    // The constant-size continuation history is initialized by thread 0 of each NUMA node.
    sharedHistory.correctionHistory.clear_range(-5, numaThreadIdx, numaTotal);
    sharedHistory.pawnHistory.clear_range(-1338, numaThreadIdx, numaTotal);

    if (numaThreadIdx == 0)
        for (bool inCheck : {false, true})
            for (StatsType c : {NoCaptures, Captures})
                for (auto& to : continuationHistory[inCheck][c])
                    for (auto& h : to)
                        h.fill(-586);

    ttMoveHistory = 0;

    for (auto& to : continuationCorrectionHistory)
        for (auto& h : to)
            h.fill(5);

    for (usize i = 1; i < reductions.size(); ++i)
        reductions[i] = int(2872 / 128.0 * std::log(i));

    refreshTable.clear(network[numaAccessToken]);
}


// Main search function for both PV and non-PV nodes
template<NodeType nodeType>
Value Search::Worker::search(
  Position& pos, Stack* ss, Value alpha, Value beta, Depth depth, const bool cutNode) {

    constexpr bool PvNode   = nodeType != NonPV;
    constexpr bool rootNode = nodeType == Root;
    const bool     allNode  = !(PvNode || cutNode);
    const bool     seekMate = rootDepth >= 16 && std::abs(rootMoves[pvIdx].score) >= 2000;

    auto&                   localShashinManager = *this->shashinManager;
    const RootShashinState& rootShashinState    = localShashinManager.getState();
    bool                    isFortressNode      = localShashinManager.isFortress(rootPos);

    // Dive into quiescence search when the depth reaches zero
    if (depth <= 0)
        return qsearch<PvNode ? PV : NonPV>(pos, ss, alpha, beta);

    // Limit the depth if extensions made it too large
    depth = std::min(depth, MAX_PLY - 1);

    const bool antiDancing = antiDancingMode;

    // Check if we have an upcoming move that draws by repetition
    if (!rootNode && (alpha < VALUE_DRAW || (antiDancing && true))
        && pos.upcoming_repetition(ss->ply))
    {
        if (antiDancing && alpha >= VALUE_DRAW) [[unlikely]]
        {
            return value_draw(nodes);
        }

        if (alpha < VALUE_DRAW)
        {
            alpha = value_draw(nodes);
            if (alpha >= beta)
                return alpha;
        }
    }

    assert(-VALUE_INFINITE <= alpha && alpha < beta && beta <= VALUE_INFINITE);
    assert(PvNode || (alpha == beta - 1));
    assert(0 < depth && depth < MAX_PLY);
    assert(!(PvNode && cutNode));

    PVMoves   pv;
    StateInfo st;

    Key   posKey;
    Move  move, excludedMove, bestMove, expTTMove = Move::none();
    Depth extension, newDepth;
    Value bestValue, value, eval, maxValue, probCutBeta, expTTValue = VALUE_NONE;
    bool  givesCheck, improving, priorCapture, opponentWorsening, expTTHit = false;
    bool  capture, ttCapture, gameCycle = false, updatedLearning = false;
    int   priorReduction;
    Piece movedPiece;

    bool disableNMAndPC = false;
    bool expectedPVNode = false;
    int  sibs           = 0;

    SearchedList capturesSearched;
    SearchedList quietsSearched;

    // Step 1. Initialize node
    ss->inCheck   = pos.checkers();
    priorCapture  = pos.captured_piece();
    Color us      = pos.side_to_move();
    ss->moveCount = 0;
    bestValue     = -VALUE_INFINITE;
    maxValue      = VALUE_INFINITE;

    if (fullSearch)
        improving = true;

    ss->followPV =
      rootNode
      || ((ss - 1)->followPV && static_cast<usize>(ss->ply - 1) < lastIterationIdxPV.size()
          && (ss - 1)->currentMove == lastIterationIdxPV[ss->ply - 1]);

    // Check for the available remaining time
    if (is_mainthread())
        main_manager()->check_time(*this);

    // Used to send selDepth info to GUI (selDepth counts from 1, ply from 0)
    if (PvNode && selDepth < ss->ply + 1)
        selDepth = ss->ply + 1;

    const auto& dynamicDerived      = rootShashinState.dynamicDerived;
    const auto& staticState         = rootShashinState.staticState;
    bool        isStrategical       = localShashinManager.isStrategical();
    bool        isTacticalReactive  = dynamicDerived.isTacticalReactive;
    bool        stmKingExposed      = staticState.stmKingExposed;
    bool        opponentKingExposed = staticState.opponentKingExposed;
    bool        isSacrificial       = staticState.isSacrificial;
    bool        isTal               = localShashinManager.isTal();
    bool        isPetrosian         = localShashinManager.isPetrosian();
    bool        isAggressive        = localShashinManager.isAggressive();
    bool        isTactical          = localShashinManager.isTactical();
    bool        isComplex           = localShashinManager.isComplexPosition();
    uint8_t     legalMoveCount      = staticState.legalMoveCount;
    (void) legalMoveCount;

    if (!rootNode)
    {
        bool allowCycleDetection =
          (isStrategical && isComplex)
          || (localShashinManager.isTacticalDefensive() && localShashinManager.isLowActivity());

        if (allowCycleDetection && !ss->excludedMove)
        {
            if (pos.upcoming_repetition(ss->ply))
                gameCycle = true;
        }

        // Step 2. Check for aborted search or immediate draw
        if (threads.stop.load(std::memory_order_relaxed) || pos.is_draw(ss->ply)
            || ss->ply >= MAX_PLY)
            return (ss->ply >= MAX_PLY && !ss->inCheck) ? evaluate(pos) : value_draw(nodes);

        // Step 3. Mate distance pruning. Even if we mate at the next move our score
        // would be at best mate_in(ss->ply + 1), but if alpha is already bigger because
        // a shorter mate was found upward in the tree then there is no need to search
        // because we will never beat the current alpha. Equal and opposite logic applies
        // when being mated. In either case, return a fail-high score.
        alpha = std::max(mated_in(ss->ply), alpha);
        beta  = std::min(mate_in(ss->ply + 1), beta);
        if (alpha >= beta)
            return alpha;
    }

    assert(0 <= ss->ply && ss->ply < MAX_PLY);

    Square prevSq  = ((ss - 1)->currentMove).is_ok() ? ((ss - 1)->currentMove).to_sq() : SQ_NONE;
    bestMove       = Move::none();
    priorReduction = (ss - 1)->reduction;
    (ss - 1)->reduction = 0;
    ss->statScore       = 0;
    (ss + 2)->cutoffCnt = 0;

    const auto correctionValue = correction_value(*this, pos, ss);

    // Step 4. Transposition table lookup
    excludedMove                   = ss->excludedMove;
    posKey                         = pos.key();
    auto [ttHit, ttData, ttWriter] = tt.probe(posKey);

    ss->ttHit    = ttHit;
    ttData.move  = rootNode ? rootMoves[pvIdx].pv[0] : ttHit ? ttData.move : Move::none();
    ttData.value = ttHit ? value_from_tt(ttData.value, ss->ply, pos.rule50_count()) : VALUE_NONE;
    ss->ttPv     = excludedMove ? ss->ttPv : PvNode || (ttHit && ttData.is_pv);
    ttCapture    = ttData.move && pos.capture_stage(ttData.move);

    // Step 5. Static evaluation of the position
    Value unadjustedStaticEval = VALUE_NONE;

    // Skip early pruning when in check
    if (ss->inCheck)
        ss->staticEval = eval = (ss - 2)->staticEval;
    else if (excludedMove)
        unadjustedStaticEval = eval = ss->staticEval;
    else if (ss->ttHit)
    {
        // Never assume anything about values stored in TT
        unadjustedStaticEval = ttData.eval;
        if (!is_valid(unadjustedStaticEval))
            unadjustedStaticEval = evaluate(pos);

        ss->staticEval = eval = to_corrected_static_eval(unadjustedStaticEval, correctionValue);

        // ttValue can be used as a better position evaluation
        if (is_valid(ttData.value)
            && (ttData.bound & (ttData.value > eval ? BOUND_LOWER : BOUND_UPPER)))
            eval = ttData.value;
    }
    else
    {
        unadjustedStaticEval = evaluate(pos);
        if (!LD.is_enabled() || !expTTHit || !updatedLearning)
        {
            ss->staticEval = eval = to_corrected_static_eval(unadjustedStaticEval, correctionValue);
            // Static evaluation is saved as it was before adjustment by correction history
            ttWriter.write(posKey, VALUE_NONE, ss->ttPv, BOUND_NONE, DEPTH_UNSEARCHED, Move::none(),
                           unadjustedStaticEval, tt.generation());
        }
        else
        {
            ss->staticEval = eval = expTTValue;
            if (!is_valid(eval))
                ss->staticEval = eval =
                  to_corrected_static_eval(unadjustedStaticEval, correctionValue);
            if (eval == VALUE_DRAW)
                eval = value_draw(this->nodes);
            if (is_valid(expTTValue))
                eval = expTTValue;
        }
    }

    if (!expectedPVNode)
        // Set up the improving flag, which is true if current static evaluation is
        // bigger than the previous static evaluation at our turn (if we were in
        // check at our previous move we go back until we weren't in check) and is
        // false otherwise. The improving flag is used in various pruning heuristics.
        // Similarly, opponentWorsening is true if our static evaluation is better
        // for us than at the last ply.
        improving = ss->staticEval > (ss - 2)->staticEval;
    opponentWorsening = ss->staticEval > -(ss - 1)->staticEval;

    // Hindsight adjustment of reductions based on static evaluation difference
    if (priorReduction >= 3 && !opponentWorsening)
        depth++;
    if (priorReduction >= 2 && depth >= 2 && ss->staticEval + (ss - 1)->staticEval > 166)
        depth--;

    // Step 6. At non-PV nodes we check for an early TT cutoff. Note that we
    //         always check the validity of the TT value because of access races.
    if (!PvNode && !excludedMove && ttData.depth > depth - (ttData.value <= beta)
        && is_valid(ttData.value)
        && (ttData.bound & (ttData.value >= beta ? BOUND_LOWER : BOUND_UPPER))
        && (cutNode == (ttData.value >= beta) || depth > 4))
    {
        // If the ttMove is quiet, update move sorting heuristics on TT hit
        if (ttData.move && ttData.value >= beta)
        {
            // Bonus for a quiet ttMove that fails high
            if (!ttCapture)
                update_quiet_histories(pos, ss, *this, ttData.move, std::min(112 * depth, 695));

            // Extra penalty for early quiet moves of the previous ply
            if (prevSq != SQ_NONE && (ss - 1)->moveCount < 5 && !priorCapture)
                update_continuation_histories(ss - 1, pos.piece_on(prevSq), prevSq, -2210);
        }

        // Partial workaround for the graph history interaction problem.
        // For high rule50 counts don't produce transposition table cutoffs.
        if (pos.rule50_count() < 96)
        {
            if (depth >= 7 && ttData.move && pos.pseudo_legal(ttData.move) && pos.legal(ttData.move)
                && !is_decisive(ttData.value))
            {
                pos.do_move(ttData.move, st);
                Key nextPosKey                             = pos.key();
                auto [ttHitNext, ttDataNext, ttWriterNext] = tt.probe(nextPosKey);
                pos.undo_move(ttData.move);

                // Check that the ttValue after the tt move would also trigger a cutoff
                if (!is_valid(ttDataNext.value))
                    return ttData.value;

                if ((ttData.value >= beta) == (-ttDataNext.value >= beta))
                    return ttData.value;
            }
            else
                return ttData.value;
        }
    }  // No cutoff, but why? Compare the aspiration window to the inexact bound
    else if (!PvNode && !excludedMove && ttData.depth > depth - (ttData.value <= beta)
             && is_valid(ttData.value) && ttData.bound != BOUND_EXACT
             && ttData.bound & (ttData.value >= beta ? BOUND_UPPER : BOUND_LOWER) && depth > 5)
    {
        // If such a mismatch is the only reason cutoff failed, the TT entry is now useless
        ttWriter.penalize(1);
    }

    // [SC:learning] begin
    expTTHit        = false;
    updatedLearning = false;

    if (!excludedMove && LD.is_enabled() && useLearning)
    {
        const LearningMove* learningMove = nullptr;
        sibs                             = LD.probeByMaxDepthAndScore(posKey, learningMove);
        if (learningMove)
        {
            assert(sibs);

            enabledLearningProbe = true;
            expTTHit             = true;
            if (!ttData.move)
            {
                ttData.move = learningMove->move;
                ttCapture   = ttData.move && pos.capture_stage(ttData.move);
            }

            if (learningMove->depth >= depth)
            {
                expTTMove       = learningMove->move;
                expTTValue      = learningMove->score;
                updatedLearning = true;
            }

            if ((learningMove->depth == 0))
                updatedLearning = false;

            if (updatedLearning && is_valid(expTTValue))
            {
                if (expTTValue < alpha)
                    disableNMAndPC = true;
                if (expTTValue > alpha && expTTValue < beta)
                {
                    expectedPVNode = true;
                    improving      = true;
                }
            }
            bool expTTCapture = learningMove->move && pos.capture_stage(expTTMove);

            if (!PvNode && !excludedMove && updatedLearning
                && learningMove->depth > depth - (expTTValue <= beta) && is_valid(expTTValue)
                && (cutNode == (expTTValue >= beta) || depth > 4)
                && (depth > 8 || expTTMove == Move::none() || pos.rule50_count() < 80
                    || (!expTTCapture && type_of(pos.moved_piece(expTTMove)) != PAWN)))
            {
                if (expTTMove && expTTValue >= beta)
                {
                    if (!expTTCapture)
                        update_quiet_histories(pos, ss, *this, expTTMove,
                                               std::min(112 * depth, 695));

                    if (prevSq != SQ_NONE && (ss - 1)->moveCount < 5 && !priorCapture)
                        update_continuation_histories(ss - 1, pos.piece_on(prevSq), prevSq, -2210);
                }

                if (pos.rule50_count() < 96)
                {
                    if (depth >= 7 && ttData.move && pos.pseudo_legal(ttData.move)
                        && pos.legal(ttData.move) && !is_decisive(expTTValue))
                    {
                        do_move(pos, ttData.move, st, nullptr);
                        Key nextPosKey                             = pos.key();
                        auto [ttHitNext, ttDataNext, ttWriterNext] = tt.probe(nextPosKey);
                        undo_move(pos, ttData.move);

                        if (!is_valid(ttDataNext.value))
                            return expTTValue;
                        if ((expTTValue >= beta) == (-ttDataNext.value >= beta))
                            return expTTValue;
                    }
                    else
                        return expTTValue;
                }
            }
        }
    }
    // [SC:learning] end
    // Step 7. Tablebases probe
    if (!rootNode && !excludedMove && tbConfig.cardinality)
    {
        int piecesCount = pos.count<ALL_PIECES>();

        if (piecesCount <= tbConfig.cardinality
            && (piecesCount < tbConfig.cardinality || depth >= tbConfig.probeDepth)
            && pos.rule50_count() == 0 && !pos.can_castle(ANY_CASTLING))
        {
            TB::ProbeState err;
            TB::WDLScore   wdl = TB::probe_wdl(pos, &err);

            // Force check of time on the next occasion
            if (is_mainthread())
                main_manager()->callsCnt = 0;

            if (err != TB::ProbeState::FAIL)
            {
                ++tbHits;

                int drawScore = tbConfig.useRule50 ? 1 : 0;

                Value tbValue = VALUE_TB - ss->ply;

                // Use the range VALUE_TB to VALUE_TB_WIN_IN_MAX_PLY to score
                value = wdl < -drawScore ? -tbValue
                      : wdl > drawScore  ? tbValue
                                         : VALUE_DRAW + 2 * wdl * drawScore;

                Bound b = wdl < -drawScore ? BOUND_UPPER
                        : wdl > drawScore  ? BOUND_LOWER
                                           : BOUND_EXACT;

                if (b == BOUND_EXACT || (b == BOUND_LOWER ? value >= beta : value <= alpha))
                {
                    ttWriter.write(posKey, value_to_tt(value, ss->ply), ss->ttPv, b,
                                   std::min(MAX_PLY - 1, depth + 6), Move::none(), VALUE_NONE,
                                   tt.generation());

                    return value;
                }

                if (PvNode)
                {
                    if (b == BOUND_LOWER)
                        bestValue = value, alpha = std::max(alpha, bestValue);
                    else
                        maxValue = value;
                }
            }
        }
    }

    if (ss->inCheck)
        goto moves_loop;

    // Use static evaluation difference to improve quiet move ordering
    if (((ss - 1)->currentMove).is_ok() && !(ss - 1)->inCheck && !priorCapture)
    {
        int evalDiff = std::clamp(-int((ss - 1)->staticEval + ss->staticEval), -189, 194) + 60;
        mainHistory[~us][((ss - 1)->currentMove).raw()] << evalDiff * 11;
        if (!ttHit && type_of(pos.piece_on(prevSq)) != PAWN
            && ((ss - 1)->currentMove).type_of() != PROMOTION)
            sharedHistory.pawn_entry(pos)[pos.piece_on(prevSq)][prevSq] << evalDiff * 13;
    }

    if (fullSearch)
        goto moves_loop;

    // Step 8. Razoring
    // If eval is really low, skip search entirely and return the qsearch value
    if (!PvNode)
    {
        Value razorThreshold = alpha - 482 * depth * depth;

        if (isAggressive)
            razorThreshold -= 50;

        if (isFortressNode)
            razorThreshold -= 60;

        if (legalMoveCount > 30 && !isStrategical)
            razorThreshold -= 20;

        if (eval < razorThreshold)
            return qsearch<NonPV>(pos, ss, alpha, beta);
    }

    // Step 9. Futility pruning: child node
    {
        int marginMultiplier = 100;

        // [SC] As in ShashChess 41 (not the Gold Digger build): Tal 90%, king danger at
        // most 85%, fortress 60%, otherwise the Stockfish 19 margin. The previous 130%
        // whenever pawnsNearPromotion or isSacrificial held (true in most game positions)
        // removed most of the child-node futility pruning of the main thread.
        if (isAggressive)
            marginMultiplier = 90;

        if (staticState.kingDanger)
            marginMultiplier = std::min(marginMultiplier, 85);

        if (isFortressNode)
            marginMultiplier = 60;

        if (isStrategical && legalMoveCount > 30)
            marginMultiplier = std::min(marginMultiplier + 5, 115);

        auto futility_margin = [&](Depth d) {
            Value futilityMult = std::min(45 + depth * 4, 85);
            futilityMult -= 20 * !ss->ttHit;

            futilityMult = (futilityMult * marginMultiplier) / 100;

            return futilityMult * d
                 - (2789 * improving + 335 * opponentWorsening) * futilityMult / 1024
                 + std::abs(correctionValue) / 198435;
        };

        if (!ss->ttPv && depth < (seekMate ? 6 : 19) && eval - futility_margin(depth) >= beta
            && eval >= beta && (!ttData.move || ttCapture) && !is_loss(beta) && !is_win(eval))
        {
            return (661 * beta + 363 * eval) / 1024;
        }
    }

    // Step 10. Null move search with verification search
    if (cutNode && !disableNMAndPC && ss->staticEval >= beta - 13 * depth - 47 * improving + 365
        && !excludedMove && pos.non_pawn_material(us) && ss->ply >= nmpMinPly && beta >= -2000)
    {
        bool skipNullMove = false;
        bool nodeHot = staticState.kingDanger || staticState.pawnsNearPromotion || isSacrificial;

        if (nodeHot || (isAggressive && staticState.kingDanger && depth < 10))
            skipNullMove = true;

        if (isPetrosian && staticState.stmKingDanger)
            skipNullMove = true;

        if (!skipNullMove)
        {
            assert((ss - 1)->currentMove != Move::null());

            Depth R = 7 + depth / 3 + std::max((ss->staticEval - beta) / 256, 0);

            do_null_move(pos, st, ss);
            Value nullValue = -search<NonPV>(pos, ss + 1, -beta, -beta + 1, depth - R, false);
            undo_null_move(pos);
            // Do not return unproven mate or TB scores
            if (nullValue >= beta && !is_win(nullValue))
            {
                if (nmpMinPly || depth < 16)
                    return nullValue;

                // Recursive verification is not allowed
                assert(!nmpMinPly);

                // Do verification search at high depths, with null move pruning
                // disabled until ply exceeds nmpMinPly.
                nmpMinPly = ss->ply + 3 * (depth - R) / 4;
                Value v   = search<NonPV>(pos, ss, beta - 1, beta, depth - R, false);
                nmpMinPly = 0;

                if (v >= beta)
                    return nullValue;
            }
        }
    }

    improving |= ss->staticEval >= beta;

    // Internal iterative reductions
    // Step 11. Internal iterative reductions
    // At sufficient depth, reduce depth for PV/Cut nodes without a TTMove.
    // (*Scaler) Making IIR more aggressive scales poorly.
    // Internal iterative reductions
    if (!ss->followPV && !allNode && depth >= 6 && !ttData.move)  // [SC] condizione SF19
    {
        depth--;  // [SC] rimosso il blocco Shashin irraggiungibile (richiedeva ttData.move)
    }

    // Step 12. ProbCut
    // If we have a good enough capture (or queen promotion) and a reduced search
    // returns a value much above beta, we can (almost) safely prune the previous move.
    probCutBeta = beta + 241 - 64 * improving;
    if (depth >= 3 && !disableNMAndPC && !ss->inCheck && !is_decisive(beta)
        && !(is_valid(ttData.value) && ttData.value < probCutBeta))
    {
        bool skipProbCut = isFortressNode || (isStrategical && !staticState.kingDanger);

        if (!skipProbCut)
        {
            assert(probCutBeta < VALUE_INFINITE && probCutBeta > beta);

            MovePicker mp(pos, ttData.move, probCutBeta - ss->staticEval, &captureHistory);
            Depth      probCutDepth = depth - (improving ? 5 : 3);

            // [SC] Shashin offsets on the Stockfish 19 depth, without the old SF18 clamp to
            // [0, depth]: SF19 handles probCutDepth <= 0 (qsearch only, TT depth + 1 >= -2)
            if (isAggressive)
                probCutDepth++;
            else if (isStrategical)
                probCutDepth--;

            while ((move = mp.next_move()) != Move::none())
            {
                assert(move.is_ok());

                if (move == excludedMove || !pos.legal(move))
                    continue;

                assert(pos.capture_stage(move));

                do_move(pos, move, st, ss);

                // Perform a preliminary qsearch to verify that the move holds
                value = -qsearch<NonPV>(pos, ss + 1, -probCutBeta, -probCutBeta + 1);

                // If the qsearch held, perform the regular search
                if (value >= probCutBeta && probCutDepth > 0)
                    value = -search<NonPV>(pos, ss + 1, -probCutBeta, -probCutBeta + 1,
                                           probCutDepth, !cutNode);

                undo_move(pos, move);

                if (value >= probCutBeta)
                {
                    // Save ProbCut data into transposition table
                    ttWriter.write(posKey, value_to_tt(value, ss->ply), ss->ttPv, BOUND_LOWER,
                                   probCutDepth + 1, move, unadjustedStaticEval, tt.generation());

                    if (!is_decisive(value))
                        return value - (probCutBeta - beta);
                }
            }
        }
    }

moves_loop:  // When in check, search starts here

    // Step 13. A small ProbCut idea
    probCutBeta              = beta + 428;
    const bool commonProbcut = (ttData.bound & BOUND_LOWER) && ttData.depth >= depth - 4
                            && ttData.value >= probCutBeta && !is_decisive(beta)
                            && is_valid(ttData.value) && !is_decisive(ttData.value);

    if (commonProbcut)
    {
        bool tacticalRisk = staticState.kingDanger || staticState.pawnsNearPromotion
                         || isSacrificial || stmKingExposed || opponentKingExposed;

        if (!tacticalRisk || ttData.depth >= depth - 2)
            return probCutBeta;
    }

    const PieceToHistory* contHist[] = {
      (ss - 1)->continuationHistory, (ss - 2)->continuationHistory, (ss - 3)->continuationHistory,
      (ss - 4)->continuationHistory, (ss - 5)->continuationHistory, (ss - 6)->continuationHistory};


    MovePicker mp(pos, ttData.move, depth, &mainHistory, &lowPlyHistory, &captureHistory, contHist,
                  &sharedHistory, ss->ply);

    value         = bestValue;
    int moveCount = 0;

    // Step 14. Loop through all pseudo-legal moves until no moves remain
    // or a beta cutoff occurs.
    while ((move = mp.next_move()) != Move::none())
    {
        assert(move.is_ok());

        if (move == excludedMove)
            continue;

        // Check for legality
        if (!pos.legal(move))
            continue;

        // At root obey the "searchmoves" option and skip moves not listed in Root
        // Move List. In MultiPV mode we also skip PV moves that have been already
        // searched and those of lower "TB rank" if we are in a TB root position.
        if (rootNode && !std::count(rootMoves.begin() + pvIdx, rootMoves.begin() + pvLast, move))
            continue;

        ss->moveCount = ++moveCount;

        if (rootNode && is_mainthread() && nodes > NODES_LIMIT_OUTPUT)
        {
            main_manager()->updates.onIter(
              {depth, UCIEngine::move(move, pos.is_chess960()), moveCount + pvIdx});
        }
        if (PvNode)
            (ss + 1)->pv = nullptr;

        extension  = 0;
        capture    = pos.capture_stage(move);
        movedPiece = pos.moved_piece(move);
        givesCheck = pos.gives_check(move);

        newDepth = depth - 1;

        int delta = beta - alpha;
        int r     = reduction(improving, depth, moveCount, delta);

        // Increase reduction for ttPv nodes
        // (*Scaler) Larger values scale well.
        if (ss->ttPv)
            r += 929;

        if (fullSearch)
            goto skipExtensionAndPruning;

        // Step 15. Pruning at shallow depths.
        // Depth conditions are important for mate finding.
        if (!rootNode && pos.non_pawn_material(us) && !is_loss(bestValue))
        {
            int moveCountThreshold = (3 + depth * depth) / (2 - improving);

            if (depth > 8)
            {
                if (isStrategical)
                {
                    int extraReduction = 1;
                    moveCountThreshold -= extraReduction;
                }
                else if (isAggressive || isPetrosian)
                {
                    int extraExtension = 1;
                    if (staticState.pawnsNearPromotion || staticState.kingDanger)
                        extraExtension = 2;
                    moveCountThreshold += extraExtension;
                }
            }
            // Skip quiet moves if movecount exceeds our threshold
            if (moveCount >= moveCountThreshold)
                mp.skip_quiet_moves();

            // Reduced depth of the next LMR search
            int lmrDepth = newDepth - r / 1024;

            if (capture || givesCheck)
            {
                Piece capturedPiece = pos.piece_on(move.to_sq());
                int   captHist = captureHistory[movedPiece][move.to_sq()][type_of(capturedPiece)];

                // Futility pruning for captures
                if (!givesCheck && lmrDepth < 8)
                {
                    Value futilityValue = ss->staticEval + 234 + 247 * lmrDepth
                                        + PieceValue[capturedPiece] + 134 * captHist / 1024;

                    if (futilityValue <= alpha)
                        continue;
                }

                // SEE based pruning for captures and checks.
                // Avoid pruning sacrifices of our last piece for stalemate.
                int margin = 177 * depth + captHist * 34 / 1024;
                if ((alpha >= VALUE_DRAW || pos.non_pawn_material(us) != PieceValue[movedPiece])
                    && !pos.see_ge(move, -margin))
                    continue;
            }
            else if (!ss->followPV || !PvNode)
            {
                int dIndex  = std::min(int(depth), int(lmrDivisor.size())) - 1;
                int history = (*contHist[0])[movedPiece][move.to_sq()]
                            + (*contHist[1])[movedPiece][move.to_sq()]
                            + sharedHistory.pawn_entry(pos)[movedPiece][move.to_sq()];

                // Continuation history based pruning
                if (history < -4136 * depth)
                    continue;

                history += 69 * mainHistory[us][move.raw()] / 32;

                // (*Scaler): Generally, lower divisors scale well
                lmrDepth += history / lmrDivisor[dIndex];

                Value futilityValue =
                  ss->staticEval + 119 * lmrDepth + 90 * (ss->staticEval > alpha) + 164;

                // Futility pruning: parent node
                // (*Scaler): Generally, more frequent futility pruning scales well
                if (!ss->inCheck && lmrDepth < 12 && futilityValue <= alpha)
                {
                    if (bestValue <= futilityValue && !is_decisive(bestValue)
                        && !is_win(futilityValue))
                        bestValue = futilityValue;
                    continue;
                }

                lmrDepth = std::max(lmrDepth, 0);

                // Prune moves with negative SEE
                if (!pos.see_ge(move, -23 * lmrDepth * lmrDepth))
                    continue;
            }
        }

        // Step 16. Singular Extensions
        //
        // We check for "only moves": if one move fails high on (alpha, beta) but all
        // others fail low on (alpha-s, beta-s), then that move is singular. Singular
        // moves are extended to better estimate the result of the (putatively)
        // forced line. If it's non-singular, we may do some pruning or reduction.
        //
        // Recursive excluded search is avoided. The `excludedMove` mechanism
        // was historically a hack and remains a bit fragile.
        //
        // (*Scaler) Generally, higher singularBeta (i.e closer to ttValue)
        // and lower extension margins scale well.
        if (!rootNode && move == ttData.move && !excludedMove && depth >= 6 + ss->ttPv
            && is_valid(ttData.value) && !is_decisive(ttData.value) && (ttData.bound & BOUND_LOWER)
            && ttData.depth >= depth - 3 && !is_shuffling(move, ss, pos) && !seekMate)
        {
            Value singularBeta  = ttData.value - (59 + 66 * (ss->ttPv && !PvNode)) * depth / 63;
            Depth singularDepth = newDepth / 2;

            if ((isAggressive || isSacrificial) && staticState.kingDanger && depth > 8)
                singularDepth += std::max(1, singularDepth / 4);

            ss->excludedMove = move;
            value = search<NonPV>(pos, ss, singularBeta - 1, singularBeta, singularDepth, cutNode);
            ss->excludedMove = Move::none();

            if (value < singularBeta)
            {
                int corrValAdj   = std::abs(correctionValue) / 198368;
                int doubleMargin = -2 + 204 * PvNode - 152 * !ttCapture - corrValAdj
                                 - 1175 * ttMoveHistory / 114178 - (ss->ply > rootDepth) * 38;
                int tripleMargin = 70 + 279 * PvNode - 188 * !ttCapture + 81 * ss->ttPv - corrValAdj
                                 - (ss->ply > rootDepth) * 43;

                extension =
                  1 + (value < singularBeta - doubleMargin) + (value < singularBeta - tripleMargin);

                depth++;
            }

            // Multi-cut pruning
            // Our ttMove is assumed to fail high based on the bound of the TT entry,
            // and if after excluding the ttMove with a reduced search we fail high
            // over the original beta, we assume this expected cut-node is not
            // singular (multiple moves fail high), and we can prune the whole
            // subtree by returning a softbound.
            else if (value >= beta && !is_decisive(value))
            {
                ttMoveHistory << -421 - 110 * depth;

                if (!ss->inCheck && value > ss->staticEval)
                {
                    const int bonus =
                      std::clamp(int(value - ss->staticEval) * singularDepth * 177 / 1024,
                                 -CORRECTION_HISTORY_LIMIT / 4, CORRECTION_HISTORY_LIMIT / 4);
                    update_correction_history(pos, ss, *this, bonus);
                }

                return value;
            }

            // Negative extensions
            // If other moves failed high over (ttValue - margin) without the
            // ttMove on a reduced search, but we cannot do multi-cut because
            // (ttValue - margin) is lower than the original beta, we do not know
            // if the ttMove is singular or can do a multi-cut, so we reduce the
            // ttMove in favor of other moves based on some conditions:

            // If the ttMove is assumed to fail high over current beta or
            // if we are on a cutNode
            else if (ttData.value >= beta || cutNode)
                extension = -3;
        }

skipExtensionAndPruning:

        u64 nodeCount = rootNode ? u64(nodes) : 0;

        // Step 17. Make the move
        do_move(pos, move, st, givesCheck, ss);
        bool doLMRStep = !fullSearch;

        // Add extension to new depth
        newDepth += extension;

        // Step 18. Compute and apply late moves reductions/extensions (LMR)

        // Decrease reduction for PvNodes (*Scaler)
        if (ss->ttPv)
            r -= 3023 + PvNode * 1004 + (ttData.value > alpha) * 885
               + (ttData.depth >= depth) * (816 + cutNode * 940);

        if (!fullSearch)
        {
            if ((isTactical || isSacrificial) && depth > 8)
                r -= 256;
            else if (isStrategical && !ss->inCheck && depth > 6)
                r += 256;
        }

        // Base reduction offset to compensate for other tweaks
        r += 697;
        r -= moveCount * 65;
        r -= std::abs(correctionValue) / 26310;

        // Increase reduction for cut nodes
        if (cutNode)
            r += 4026 + 933 * !ttData.move;

        // Increase reduction if ttMove is a capture
        if (ttCapture)
            r += 1079;

        // Increase reduction if next ply has a lot of fail high
        if ((ss + 1)->cutoffCnt > 1)
        {
            // [SC] Tactical positions keep the conservative ShashChess formula; all the
            // others (Capablanca and no Shashin style) use the Stockfish 19 one.
            if (dynamicDerived.isHighTal || isTacticalReactive || staticState.isSacrificial)
                r += 120 + 1024 * ((ss + 1)->cutoffCnt > 2) + 100 * ((ss + 1)->cutoffCnt > 3)
                   + 1024 * allNode;
            else
                r += 264 + 1095 * ((ss + 1)->cutoffCnt > 2) + 1138 * allNode;
        }
        // For first picked move (ttMove) reduce reduction
        else if (move == ttData.move)
            r -= 2179;

        if (capture)
            ss->statScore = 873 * int(PieceValue[pos.captured_piece()]) / 128
                          + captureHistory[movedPiece][move.to_sq()][type_of(pos.captured_piece())];
        else
            ss->statScore =
              (2252 * mainHistory[us][move.raw()] + 1126 * (*contHist[0])[movedPiece][move.to_sq()]
               + 1093 * (*contHist[1])[movedPiece][move.to_sq()])
              / 1024;

        // Decrease/increase reduction for moves with a good/bad history
        r -= ss->statScore * 439 / 4096;

        if (!capture && !is_decisive(alpha))
            r += 3 * std::clamp(alpha - eval, -64, 96);

        // Scale up reductions for expected ALL nodes
        if (allNode)
            r += r * 276 / (256 * depth + 268);

        // Apply the computed LMR
        if (depth >= 2 && moveCount > std::max(sibs, 1) && doLMRStep)
        {
            int original_r = r;
            // [SC] Stockfish 19 formula, as ShashChess 41 used the SF18 one: the extra cap
            // "no extension beyond 2 * rootDepth plies" was not in either of them
            auto lmrDepth = [&](int red) {
                return std::max(1, std::min(newDepth - red / 1024, newDepth + 2)) + PvNode;
            };
            Depth d = lmrDepth(r);

            if (depth > 5 && ss->ply > 1)
            {
                if (isAggressive && (givesCheck || capture))
                    r -= 50;
                else if (isPetrosian && !capture && !givesCheck)
                    r -= 50;
                else if (isStrategical && !capture && !givesCheck && depth > 8)
                    r += 45;

                if (r != original_r)
                    d = lmrDepth(r);
            }

            if (isFortressNode)
                d = std::max(1, d - 1);

            ss->reduction = newDepth - d;
            value         = -search<NonPV>(pos, ss + 1, -(alpha + 1), -alpha, d, true);
            ss->reduction = 0;

            // Do a full-depth search when reduced LMR search fails high
            // (*Scaler) Shallower searches here don't scale well
            if (value > alpha)
            {
                // Adjust full-depth search based on LMR results - if the result was
                // good enough search deeper, if it was bad enough search shallower.
                const bool doDeeperSearch    = d < newDepth && value > bestValue + 53;
                const bool doShallowerSearch = value < bestValue + 8;

                newDepth += doDeeperSearch - doShallowerSearch;

                if (newDepth > d)
                    value = -search<NonPV>(pos, ss + 1, -(alpha + 1), -alpha, newDepth, !cutNode);

                int bonus = 1334;
                if (isAggressive)
                    bonus += 25;
                else if (isStrategical)
                    bonus += 12;
                if (isFortressNode)
                    bonus += 30;

                // Post LMR continuation history updates
                update_continuation_histories(ss, movedPiece, move.to_sq(), bonus);
            }
        }

        // Step 19. Full-depth search when LMR is skipped
        else if (!PvNode || moveCount > 1)
        {
            // Increase reduction if ttMove is not present
            if (!ttData.move)
                r += 1127;

            // If expected reduction is high, we reduce search depth here
            value = -search<NonPV>(pos, ss + 1, -(alpha + 1), -alpha,
                                   newDepth - (r > 5234) - (r > 5487 && newDepth > 2), !cutNode);
        }

        // Step 20. For PV nodes only, do a full PV search on the first move
        // or after a fail high, otherwise let the parent node fail low with
        // value <= alpha and try another move.
        if (PvNode && (moveCount == 1 || value > alpha))
        {
            (ss + 1)->pv = &pv;
            (ss + 1)->pv->clear();

            // Extend move from transposition table if we are about to dive
            // into qsearch. Decisive score handling improves mate finding
            // and retrograde analysis.
            if (move == ttData.move
                && ((is_valid(ttData.value) && is_decisive(ttData.value) && ttData.depth > 0)
                    || ttData.depth > 1))
                newDepth = std::max(newDepth, 1);

            value = -search<PV>(pos, ss + 1, -beta, -alpha, newDepth, false);
        }

        // Step 21. Undo move
        undo_move(pos, move);

        assert(value > -VALUE_INFINITE && value < VALUE_INFINITE);

        // Step 22. Check for a new best move
        // If a stop occurred, the value of the search cannot be trusted,
        // and we return immediately without updating the best move,
        // principal variation or transposition table.
        if (threads.stop.load(std::memory_order_relaxed))
            return VALUE_ZERO;

        if (rootNode)
        {
            RootMove& rm = *std::find(rootMoves.begin(), rootMoves.end(), move);

            rm.effort += nodes - nodeCount;

            u64 N      = nodes - nodeCount;
            u64 E_prev = std::max(u64(1), rm.effort - N);

            // Exponential moving average parameters for root move
            constexpr u64 Scale          = 32;
            constexpr u64 ChiNumerator   = 3;
            constexpr u64 ChiDenominator = 2;
            constexpr u64 MinWeight      = 12;
            constexpr u64 MaxWeight      = 24;

            u64 w     = std::clamp((Scale * N * ChiDenominator)
                                     / (N * ChiDenominator + ChiNumerator * E_prev),
                                   MinWeight, MaxWeight);
            u64 w_mss = std::min(w, u64(16));
            i64 v2    = i64(value) * std::abs(value);

            if (rm.averageScore == -VALUE_INFINITE)
                rm.averageScore = value;
            else
                rm.averageScore = Value((value * w + rm.averageScore * (Scale - w)) / Scale);

            if (rm.meanSquaredScore == -VALUE_INFINITE * VALUE_INFINITE)
                rm.meanSquaredScore = value * std::abs(value);
            else
                rm.meanSquaredScore =
                  Value((v2 * w_mss + int64_t(rm.meanSquaredScore) * (Scale - w_mss)) / Scale);

            // PV move or new best move?
            if (moveCount == 1 || value > alpha)
            {
                rm.score = rm.uciScore = value;
                rm.selDepth            = selDepth;
                rm.unset_inexact();

                if (value >= beta)
                {
                    rm.inexactLower = true;
                    rm.uciScore     = beta;
                }
                else if (value <= alpha)
                {
                    rm.inexactUpper = true;
                    rm.uciScore     = alpha;
                }

                rm.pv.resize(1);

                assert((ss + 1)->pv);

                for (Move pvMove : *(ss + 1)->pv)
                    rm.pv.push_back(pvMove);

                // We record how often the best move has been changed in each iteration.
                // This information is used for time management. In MultiPV mode,
                // we must take care to only do this for the first PV line.
                if (moveCount > 1 && !pvIdx)
                    ++bestMoveChanges;
            }
            else
                // All other moves but the PV are set to the lowest value: this
                // is not a problem when sorting because the sort is stable and the
                // move position in the list is preserved -- just the PV is pushed up.
                rm.score = -VALUE_INFINITE;
        }

        // If we have an alternative move equal in value to the current bestmove,
        // we sometimes promote it to bestmove by pretending it just exceeds
        // alpha (but not beta).
        int inc = (value == bestValue && ss->ply + 2 >= rootDepth && (int(nodes) & 14) == 0
                   && !is_win(std::abs(value) + 1));

        if (value + inc > bestValue)
        {
            bestValue = value;

            if (value + inc > alpha)
            {
                bestMove = move;

                // Update PV even in fail-high case
                if (PvNode && !rootNode)
                    ss->pv->update(move, (ss + 1)->pv);

                if (value >= beta)
                {
                    // (*Scaler) Infrequent and small updates scale well
                    ss->cutoffCnt += (extension < 2) || PvNode;
                    assert(value >= beta);  // Fail high
                    break;
                }

                // Reduce other moves if we have found at least one score improvement
                if (depth > 3 && depth < 12 && !is_decisive(value))
                {
                    // Base Stockfish 19 (prima era 2)
                    int reduction = 3;

                    const bool criticalTactical =
                      (opponentKingExposed && isTal) || (stmKingExposed && isPetrosian);

                    // Shashin layern
                    if (criticalTactical)
                        reduction -= 1;
                    else if (isStrategical && !stmKingExposed && !opponentKingExposed)
                    {
                        if (depth > 8)
                            reduction += 1;
                        if (staticState.allPiecesCount > 20)
                            reduction = std::min(reduction, 3);
                    }
                    else if (gameCycle)
                        reduction += (pos.rule50_count() > 20) ? 1 : -1;

                    reduction = std::clamp(reduction, 1, depth - 1);
                    depth -= reduction;
                }

                assert(depth > 0);
                alpha = value;  // Update alpha! Always alpha < beta
            }
        }

        // If the move is worse than some previously searched move,
        // remember it, to update its stats later.
        if (move != bestMove && moveCount <= SEARCHEDLIST_CAPACITY)
        {
            if (capture)
                capturesSearched.push_back(move);
            else
                quietsSearched.push_back(move);
        }
    }

    // Step 23. Check for mate and stalemate, otherwise update bestmove/countermove stats

    assert(moveCount || !ss->inCheck || excludedMove || !MoveList<LEGAL>(pos).size());

    // Adjust best value for fail high cases
    if (bestValue >= beta && !is_decisive(bestValue) && !is_decisive(alpha))
        bestValue = (bestValue * depth + beta) / (depth + 1);

    // All legal moves have been searched: if there are no legal moves, it
    // must be a mate or a stalemate (just a fail low score if we are in a
    // singular extension search).
    if (!moveCount)
        bestValue = excludedMove ? alpha : ss->inCheck ? mated_in(ss->ply) : VALUE_DRAW;

    // If there is a move that produces search value greater than alpha,
    // we update the stats of searched moves.
    else if (bestMove)
    {
        update_all_stats(pos, ss, *this, bestMove, prevSq, quietsSearched, capturesSearched, depth,
                         ttData.move, PvNode);
        if (!PvNode)
            ttMoveHistory << (bestMove == ttData.move ? 918 : -747);
    }

    // Bonus for prior quiet countermove that caused the fail low
    else if (!priorCapture && prevSq != SQ_NONE)
    {
        int bonusScale = -241;
        bonusScale -= (ss - 1)->statScore / 98;
        bonusScale += std::min(59 * depth, 420);
        bonusScale += 186 * ((ss - 1)->moveCount > 9);
        bonusScale += 142 * (!ss->inCheck && bestValue <= ss->staticEval - 106);
        bonusScale += 159 * (!(ss - 1)->inCheck && bestValue <= -(ss - 1)->staticEval - 68);

        bonusScale = std::max(bonusScale, 0);

        // scaledBonus ranges from 0 to roughly 2.3M, overflows happen for
        // multipliers larger than 900
        const int scaledBonus = std::min(150 * depth - 85, 1337) * bonusScale;

        update_continuation_histories(ss - 1, pos.piece_on(prevSq), prevSq,
                                      scaledBonus * 263 / 16384);

        mainHistory[~us][((ss - 1)->currentMove).raw()] << scaledBonus * 215 / 32768;

        if (type_of(pos.piece_on(prevSq)) != PAWN && ((ss - 1)->currentMove).type_of() != PROMOTION)
            sharedHistory.pawn_entry(pos)[pos.piece_on(prevSq)][prevSq] << scaledBonus * 324 / 8192;
    }

    // Bonus for prior capture countermove that caused the fail low
    else if (priorCapture && prevSq != SQ_NONE)
    {
        Piece capturedPiece = pos.captured_piece();
        assert(capturedPiece != NO_PIECE);
        captureHistory[pos.piece_on(prevSq)][prevSq][type_of(capturedPiece)] << 892;
    }

    if (PvNode)
        bestValue = std::min(bestValue, maxValue);

    // If no good move is found and the previous position was ttPv, then the previous
    // opponent move is probably good and the new position is added to the search tree.
    if (bestValue <= alpha)
        ss->ttPv = ss->ttPv || (ss - 1)->ttPv;

    // Step 24. Write gathered information in transposition table. Note that the
    // static evaluation is saved as it was before correction history.
    if (!excludedMove && !(rootNode && pvIdx))
        ttWriter.write(posKey, value_to_tt(bestValue, ss->ply), ss->ttPv,
                       bestValue >= beta    ? BOUND_LOWER
                       : PvNode && bestMove ? BOUND_EXACT
                                            : BOUND_UPPER,
                       moveCount != 0 ? depth : std::min(MAX_PLY - 1, depth + 6), bestMove,
                       unadjustedStaticEval, tt.generation());

    // Adjust correction history if the best move is not a capture and
    // the error direction matches whether we are above/below bounds.
    if (!ss->inCheck && !(bestMove && pos.capture(bestMove))
        && (bestValue > ss->staticEval) == bool(bestMove))
    {
        auto bonus =
          std::clamp(int(bestValue - ss->staticEval) * depth * (bestMove ? 12 : 18) / 128,
                     -CORRECTION_HISTORY_LIMIT / 4, CORRECTION_HISTORY_LIMIT / 4);
        update_correction_history(pos, ss, *this, 1061 * bonus / 1024);
    }

    // The search is now complete
    assert(-VALUE_INFINITE < bestValue && bestValue < VALUE_INFINITE);
    return bestValue;
}


// Quiescence search function, which is called by the main search function with
// depth zero, or recursively with further decreasing depth. With depth <= 0, we
// "should" be using static eval only, but tactical moves may confuse the static eval.
// To fight this horizon effect, we implement this qsearch of tactical moves.
// See https://www.chessprogramming.org/Horizon_Effect
// and https://www.chessprogramming.org/Quiescence_Search
template<NodeType nodeType>
Value Search::Worker::qsearch(Position& pos, Stack* ss, Value alpha, Value beta) {

    static_assert(nodeType != Root);
    const RootShashinState& rootShashinState = shashinManager->getState();
    constexpr bool          PvNode           = nodeType == PV;

    assert(alpha >= -VALUE_INFINITE && alpha < beta && beta <= VALUE_INFINITE);
    assert(PvNode || (alpha == beta - 1));

    const bool antiDancing = antiDancingMode;

    // Check if we have an upcoming move that draws by repetition
    if ((alpha < VALUE_DRAW || (antiDancing && true)) && pos.upcoming_repetition(ss->ply))
    {
        if (antiDancing && alpha >= VALUE_DRAW) [[unlikely]]
        {
            return value_draw(nodes);
        }

        if (alpha < VALUE_DRAW)
        {
            alpha = value_draw(nodes);
            if (alpha >= beta)
                return alpha;
        }
    }

    PVMoves   pv;
    StateInfo st;

    Key   posKey;
    Move  move, bestMove, expTTMove = Move::none();
    Value bestValue, value, futilityBase, expTTValue = VALUE_NONE;
    bool  pvHit, givesCheck, capture, expTTHit = false, updatedLearning = false;
    int   moveCount;

    // Step 1. Initialize node
    if (PvNode)
    {
        (ss + 1)->pv = &pv;
        ss->pv->clear();
    }

    bestMove    = Move::none();
    ss->inCheck = pos.checkers();
    moveCount   = 0;

    // Used to send selDepth info to GUI (selDepth counts from 1, ply from 0)
    if (PvNode && selDepth < ss->ply + 1)
        selDepth = ss->ply + 1;

    // Step 2. Check for an immediate draw or maximum ply reached
    if (pos.is_draw(ss->ply) || ss->ply >= MAX_PLY)
        return (ss->ply >= MAX_PLY && !ss->inCheck) ? evaluate(pos) : VALUE_DRAW;

    assert(0 <= ss->ply && ss->ply < MAX_PLY);

    // Step 3. Transposition table lookup
    posKey                         = pos.key();
    auto [ttHit, ttData, ttWriter] = tt.probe(posKey);

    ss->ttHit    = ttHit;
    ttData.move  = ttHit ? ttData.move : Move::none();
    ttData.value = ttHit ? value_from_tt(ttData.value, ss->ply, pos.rule50_count()) : VALUE_NONE;
    pvHit        = ttHit && ttData.is_pv;

    const auto& staticState = rootShashinState.staticState;
    // At non-PV nodes we check for an early TT cutoff
    const auto& dynamicDerived = rootShashinState.dynamicDerived;
    const bool  isStrategical  = dynamicDerived.isStrategical;
    const bool  isAggressive   = dynamicDerived.isAggressive;

    // [SC] identico a SF19: il ramo Shashin successivo era irraggiungibile
    if (!PvNode && ttData.depth >= DEPTH_QS && is_valid(ttData.value)
        && (ttData.bound & (ttData.value >= beta ? BOUND_LOWER : BOUND_UPPER)))
        return ttData.value;

    // [SC:learning] begin
    expTTHit        = false;
    updatedLearning = false;

    if (useLearning && LD.is_enabled())
    {
        const LearningMove* learningMove = nullptr;
        int                 siblings     = LD.probeByMaxDepthAndScore(posKey, learningMove);
        (void) siblings;
        if (learningMove)
        {
            expTTHit   = true;
            expTTMove  = learningMove->move;
            expTTValue = learningMove->score;

            if (!ttHit)
                ttData.move = expTTMove;

            const bool canUseExpTTValue = !PvNode && is_valid(expTTValue);
            // [SC] il ramo Shashin successivo era irraggiungibile (depth >= 2 dopo depth < 0)
            if (canUseExpTTValue && learningMove->depth >= DEPTH_QS)
            {
                updatedLearning = true;
                return expTTValue;
            }
        }
    }
    // [SC:learning] end

    // Step 4. Static evaluation of the position
    Value unadjustedStaticEval = VALUE_NONE;
    if (ss->inCheck)
        bestValue = futilityBase = -VALUE_INFINITE;
    else
    {
        const auto correctionValue = correction_value(*this, pos, ss);

        if (ss->ttHit)
        {
            // Never assume anything about values stored in TT
            unadjustedStaticEval = ttData.eval;

            if (!is_valid(unadjustedStaticEval))
                unadjustedStaticEval = evaluate(pos);

            ss->staticEval = bestValue =
              to_corrected_static_eval(unadjustedStaticEval, correctionValue);

            // ttValue can be used as a better position evaluation
            if (is_valid(ttData.value) && !is_decisive(ttData.value)
                && (ttData.bound & (ttData.value > bestValue ? BOUND_LOWER : BOUND_UPPER)))
                bestValue = ttData.value;
        }
        else
        {
            if (expTTHit && updatedLearning && is_valid(expTTValue))
            {
                ss->staticEval = bestValue = expTTValue;
            }
            else
            {
                unadjustedStaticEval = evaluate(pos);
                ss->staticEval       = bestValue =
                  to_corrected_static_eval(unadjustedStaticEval, correctionValue);
            }
        }

        // Stand pat. Return immediately if static value is at least beta
        if (bestValue >= beta)
        {
            if (!is_decisive(bestValue))
                bestValue = (441 * bestValue + 583 * beta) / 1024;

            if (!ss->ttHit)
                ttWriter.write(posKey, VALUE_NONE, false, BOUND_LOWER, DEPTH_UNSEARCHED,
                               Move::none(), unadjustedStaticEval, tt.generation());
            return bestValue;
        }

        if (bestValue > alpha)
            alpha = bestValue;

        futilityBase = ss->staticEval + 306;
        if (dynamicDerived.isHighTal)
            futilityBase += 30;
        else if (isStrategical)
            futilityBase -= 20;
    }

    const PieceToHistory* contHist[] = {(ss - 1)->continuationHistory};

    Square prevSq = ((ss - 1)->currentMove).is_ok() ? ((ss - 1)->currentMove).to_sq() : SQ_NONE;

    // Initialize a MovePicker object for the current position, and prepare
    // to search the moves. We presently use two stages of move generator in
    // quiescence search: captures, or evasions only when in check.
    MovePicker mp(pos, ttData.move, DEPTH_QS, &mainHistory, &lowPlyHistory, &captureHistory,
                  contHist, &sharedHistory, ss->ply);

    int moveCountThreshold = 2;
    if (dynamicDerived.isHighTal)
        moveCountThreshold += PvNode ? 1 : 0;
    else if (isStrategical)
        moveCountThreshold = 1;
    if (staticState.kingDanger)
        moveCountThreshold++;
    if (MoveConfig::isFortress)
        moveCountThreshold++;

    // Base Stockfish 19
    int seeMargin = -74;
    if (isAggressive || dynamicDerived.isHighTal)
        seeMargin = staticState.kingDanger ? -104 : -84;
    else if (isStrategical)
        seeMargin = -44;
    else if (MoveConfig::isFortress)
        seeMargin = -4;


    const bool extremelyVolatile = dynamicDerived.isHighTal && staticState.kingDanger;

    // Step 5. Loop through all pseudo-legal moves until no moves remain
    // or a beta cutoff occurs.
    while ((move = mp.next_move()) != Move::none())
    {
        assert(move.is_ok());
        Piece movedPiece = pos.moved_piece(move);

        if (!pos.legal(move))
            continue;

        givesCheck = pos.gives_check(move);
        capture    = pos.capture_stage(move);

        moveCount++;

        // Step 6. Pruning
        if (!is_loss(bestValue))
        {
            // Futility pruning and moveCount pruning
            if (!givesCheck && move.to_sq() != prevSq && !is_loss(futilityBase)
                && move.type_of() != PROMOTION)
            {
                // [SC] As in ShashChess 41: Shashin threshold (base 2 as Stockfish 19).
                // The constant 2 had left moveCountThreshold computed but unused.
                if (moveCount > moveCountThreshold)
                    continue;

                Value futilityValue = futilityBase + PieceValue[pos.piece_on(move.to_sq())];

                // If static eval + value of piece we are going to capture is
                // much lower than alpha, we can prune this move.
                if (futilityValue <= alpha)
                {
                    bestValue = std::max(bestValue, futilityValue);
                    continue;
                }

                // If static exchange evaluation is low enough, we can prune
                if (!pos.see_ge(move, alpha - futilityBase))
                {
                    const bool useTacticalLogic = dynamicDerived.isHighTal
                                               || dynamicDerived.isTacticalReactive
                                               || (staticState.kingDanger && !isStrategical);

                    // [SC fix] in modalità tattica non si accredita nulla alla mossa potata,
                    // ma bestValue non deve mai scendere sotto un valore già ottenuto
                    if (useTacticalLogic)
                        bestValue = std::min(alpha, futilityBase);
                    else
                        bestValue = std::max(bestValue, std::min(alpha, futilityBase));
                    continue;
                }
            }

            // Skip non-captures
            if (!capture)
            {
                if (extremelyVolatile)
                {
                    int historyScore =
                      sharedHistory.pawn_entry(pos)[pos.moved_piece(move)][move.to_sq()];
                    if (historyScore < 4000)
                        continue;
                }
                else
                {
                    continue;
                }
            }

            bool forceCheckSearch = false;
            if (givesCheck
                && (dynamicDerived.isHighTal || (isAggressive && staticState.kingDanger)))
            {
                if (pos.see_ge(move, -200))
                {
                    if (type_of(movedPiece) != QUEEN || type_of(pos.piece_on(move.to_sq())) != PAWN)
                        forceCheckSearch = true;
                }
            }

            // Do not search moves with bad enough SEE values
            if (!forceCheckSearch && !pos.see_ge(move, seeMargin))
                continue;
        }

        // Step 7. Make and search the move
        do_move(pos, move, st, givesCheck, ss);

        value = -qsearch<nodeType>(pos, ss + 1, -beta, -alpha);
        undo_move(pos, move);

        assert(value > -VALUE_INFINITE && value < VALUE_INFINITE);

        // Step 8. Check for a new best move
        if (value > bestValue)
        {
            bestValue = value;

            if (value > alpha)
            {
                bestMove = move;

                // Update pv even in fail-high case
                if (PvNode)
                    ss->pv->update(move, (ss + 1)->pv);

                if (value < beta)  // Update alpha here!
                    alpha = value;
                else
                    break;  // Fail high
            }
        }
    }

    // [SC:variety] begin
    if (variety != 0 && std::abs(bestValue) != VALUE_INFINITE)
    {
        Value             maxIncrement = (variety == 1) ? 13 : 309;
        thread_local PRNG rng((u64(now()) * 6364136223846793005ULL + threadIdx) | 1);
        if (((variety == 2) && (bestValue <= 309) && (bestValue >= -309))
            || ((variety == 1) && (bestValue <= 13) && (bestValue >= -13)))
        {
            int maxValidIncrement = maxIncrement - std::abs(bestValue);
            if (maxValidIncrement < 0)
                maxValidIncrement = 0;
            int increment = static_cast<int>(rng.rand<uint64_t>() % (maxValidIncrement + 1));
            bestValue += increment;
        }
    }
    // [SC:variety] end
    // Step 9. Check for mate and stalemate
    // All legal moves have been searched. A special case: if we are
    // in check and no legal moves were found, it is checkmate.
    if (!moveCount)
    {
        if (ss->inCheck)  // Checkmate!
        {
            assert(!MoveList<LEGAL>(pos).size());
            return mated_in(ss->ply);  // Plies to mate from the root
        }

        // Only check for stalemate under specific conditions
        Color us = pos.side_to_move();
        if (!(pawn_single_push_bb(us, pos.pieces(us, PAWN)) & ~pos.pieces())
            && !pos.non_pawn_material(us) && type_of(pos.captured_piece()) >= KNIGHT
            && !MoveList<LEGAL>(pos).size())
            bestValue = VALUE_DRAW;
    }

    if (!is_decisive(bestValue) && bestValue > beta)
        bestValue = (462 * bestValue + 562 * beta) / 1024;

    // Step 10. Save gathered info in transposition table. The static evaluation
    // is saved as it was before adjustment by correction history.
    ttWriter.write(posKey, value_to_tt(bestValue, ss->ply), pvHit,
                   bestValue >= beta ? BOUND_LOWER : BOUND_UPPER, DEPTH_QS, bestMove,
                   unadjustedStaticEval, tt.generation());

    // The search is now complete
    assert(-VALUE_INFINITE < bestValue && bestValue < VALUE_INFINITE);
    return bestValue;
}

int Search::Worker::reduction(bool i, Depth d, int mn, int delta) const {
    int reductionScale = reductions[d] * reductions[mn];
    if (shashinManager->isAggressive())
        reductionScale = (reductionScale * 98) / 100;
    else if (shashinManager->isStrategical())
        reductionScale = (reductionScale * 104) / 100;
    else if (shashinManager->isPetrosian())
        reductionScale = (reductionScale * 99) / 100;

    if (rootDelta != 0)
        return reductionScale - delta * 577 / rootDelta + !i * reductionScale * 197 / 512 + 982;
    else
        return reductionScale - delta * 577 + !i * reductionScale * 197 / 512 + 982;
}

// elapsed() returns the time elapsed since the search started. If the
// 'nodestime' option is enabled, it will return the count of nodes searched
// instead. This function is called to check whether the search should be
// stopped based on predefined thresholds like time limits or nodes searched.
TimePoint Search::Worker::elapsed() const {
    return main_manager()->tm.elapsed([this]() { return threads.nodes_searched(); });
}


// Evaluate the current position of the game tree, from the point of view of
// the side to move.
Value Search::Worker::evaluate(const Position& pos) {
    return Eval::evaluate(network[numaAccessToken], pos, accumulatorStack, refreshTable,
                          optimism[pos.side_to_move()]);
}

namespace {

// Adjusts a mate or TB score from "plies to mate from the root" to
// "plies to mate from the current position". Standard scores are unchanged.
// The function is called before storing a value in the transposition table.
Value value_to_tt(Value v, int ply) { return is_win(v) ? v + ply : is_loss(v) ? v - ply : v; }


// Inverse of value_to_tt(): it adjusts a mate or TB score from the transposition
// table (which refers to the plies to mate/be mated from current position) to
// "plies to mate/be mated (TB win/loss) from the root". However, to avoid
// potentially false mate or TB scores related to the 50 moves rule and the
// graph history interaction, we return the highest non-TB score instead.
Value value_from_tt(Value v, int ply, int r50c) {

    if (!is_valid(v))
        return VALUE_NONE;

    // Handle TB win or better
    if (is_win(v))
    {
        // Downgrade a potentially false mate score
        if (is_mate(v) && VALUE_MATE - v > 100 - r50c)
            return VALUE_TB_WIN_IN_MAX_PLY - 1;

        // Downgrade a potentially false TB score
        if (VALUE_TB - v > 100 - r50c)
            return VALUE_TB_WIN_IN_MAX_PLY - 1;

        return v - ply;
    }

    // Handle TB loss or worse
    if (is_loss(v))
    {
        // Downgrade a potentially false mate score
        if (is_mated(v) && VALUE_MATE + v > 100 - r50c)
            return VALUE_TB_LOSS_IN_MAX_PLY + 1;

        // Downgrade a potentially false TB score
        if (VALUE_TB + v > 100 - r50c)
            return VALUE_TB_LOSS_IN_MAX_PLY + 1;

        return v + ply;
    }

    return v;
}


// Updates stats at the end of search() when a bestMove is found
void update_all_stats(const Position& pos,
                      Stack*          ss,
                      Search::Worker& workerThread,
                      Move            bestMove,
                      Square          prevSq,
                      SearchedList&   quietsSearched,
                      SearchedList&   capturesSearched,
                      Depth           depth,
                      Move            ttMove,
                      bool            PvNode) {

    CapturePieceToHistory& captureHistory = workerThread.captureHistory;
    Piece                  movedPiece     = pos.moved_piece(bestMove);
    PieceType              capturedPiece;

    int bonus =
      std::min(133 * depth - 81, 1487) + 364 * (bestMove == ttMove) + (ss - 1)->statScore / 28;
    int malus = std::min(968 * depth - 235, 2244);

    if (!PvNode)
        // Important: don't remove the cast to a 64-bit number else the multiplication
        // can overflow on 32-bit platforms which would change the bench signature
        bonus += int(bonus * u64(quietsSearched.size() + capturesSearched.size()) / 256);

    if (!pos.capture_stage(bestMove))
    {
        update_quiet_histories(pos, ss, workerThread, bestMove, bonus * 899 / 1024);

        // Decrease stats for all non-best quiet moves
        int actualMalus = malus * 1159 / 1024;
        for (Move move : quietsSearched)
        {
            actualMalus = actualMalus * 921 / 1024;
            update_quiet_histories(pos, ss, workerThread, move, -actualMalus);
        }
    }
    else
    {
        // Increase stats for the best move in case it was a capture move
        capturedPiece = type_of(pos.piece_on(bestMove.to_sq()));
        captureHistory[movedPiece][bestMove.to_sq()][capturedPiece] << bonus * 1427 / 1024;
    }

    // Extra penalty for a quiet early move that was not a TT move in
    // previous ply when it gets refuted.
    if (prevSq != SQ_NONE && ((ss - 1)->moveCount == 1 + (ss - 1)->ttHit) && !pos.captured_piece())
        update_continuation_histories(ss - 1, pos.piece_on(prevSq), prevSq, -malus * 713 / 1024);

    // Decrease stats for all non-best capture moves
    for (Move move : capturesSearched)
    {
        movedPiece    = pos.moved_piece(move);
        capturedPiece = type_of(pos.piece_on(move.to_sq()));
        captureHistory[movedPiece][move.to_sq()][capturedPiece] << -malus * 1489 / 1024;
    }
}


// Updates the continuation histories for the move pairs formed by
// the current move and the moves played in previous plies.
void update_continuation_histories(Stack* ss, Piece pc, Square to, int bonus) {
    static constexpr std::array<ConthistBonus, 6> conthist_bonuses = {
      {{1, 520}, {2, 390}, {3, 145}, {4, 251}, {5, 66}, {6, 209}}};

    // Multipliers for positive history consistency
    constexpr int CMHCMultipliers[] = {94, 103, 110, 106, 119, 126, 121};
    int           positiveCount     = 0;

    for (const auto [i, weight] : conthist_bonuses)
    {
        // Only update the first 2 continuation histories if we are in check
        if (ss->inCheck && i > 2)
            break;

        if (((ss - i)->currentMove).is_ok())
        {
            auto& historyEntry = (*(ss - i)->continuationHistory)[pc][to];
            if (historyEntry > 0)
                positiveCount++;

            int multiplier = CMHCMultipliers[positiveCount];
            historyEntry << bonus * weight * multiplier / 65536 + 73 * (i < 2);
        }
    }
}

// Updates move sorting heuristics

void update_quiet_histories(
  const Position& pos, Stack* ss, Search::Worker& workerThread, Move move, int bonus) {

    Color us = pos.side_to_move();
    workerThread.mainHistory[us][move.raw()] << bonus;  // Untuned to prevent duplicate effort

    if (ss->ply < LOW_PLY_HISTORY_SIZE)
        workerThread.lowPlyHistory[ss->ply][move.raw()] << bonus * 712 / 1024;

    update_continuation_histories(ss, pos.moved_piece(move), move.to_sq(), bonus * 750 / 1024);

    workerThread.sharedHistory.pawn_entry(pos)[pos.moved_piece(move)][move.to_sq()]
      << bonus * (bonus > -4 ? 1104 : 459) / 1024;
}
}

// When playing with strength handicap, choose the best move among a set of
// RootMoves using a statistical rule dependent on 'level'.
// Idea by Heinz van Saanen.
Move Skill::pick_best(const RootMoves& rootMoves, usize multiPV) {
    static PRNG rng(now());  // PRNG sequence should be non-deterministic

    // With tablebases at the root, rootMoves are ordered by tbRank rather
    // than by score, so compute the score range explicitly to keep 'delta'
    // non-negative.
    Value topScore = rootMoves[0].score;
    Value minScore = rootMoves[0].score;
    for (usize i = 1; i < multiPV; ++i)
    {
        topScore = std::max(topScore, rootMoves[i].score);
        minScore = std::min(minScore, rootMoves[i].score);
    }
    int    delta    = std::min(topScore - minScore, int(PawnValue));
    int    maxScore = -VALUE_INFINITE;
    double weakness = 120 - 2 * level;

    // Choose best move. For each move score we add two terms dependent on
    // weakness. One is deterministic and bigger for weaker levels, and one
    // is random. Then we choose the move with the resulting highest score.
    for (usize i = 0; i < multiPV; ++i)
    {
        // This is our magic formula
        int push = int(weakness * int(topScore - rootMoves[i].score)
                       + delta * (rng.rand<unsigned>() % int(weakness)))
                 / 128;

        if (rootMoves[i].score + push >= maxScore)
        {
            maxScore = rootMoves[i].score + push;
            best     = rootMoves[i].pv[0];
        }
    }

    return best;
}

// Function to detect when we are out of available time and stop the search,
// and to print debug info.
void SearchManager::check_time(Search::Worker& worker) {

    if (--callsCnt > 0)
        return;

    // When using nodes, ensure checking rate is not lower than 0.1% of nodes
    callsCnt = worker.limits.nodes ? std::min(512, int(worker.limits.nodes / 1024)) : 512;

    static TimePoint lastInfoTime = now();

    TimePoint elapsed = tm.elapsed([&worker]() { return worker.threads.nodes_searched(); });
    TimePoint tick    = worker.limits.startTime + elapsed;

    if (tick - lastInfoTime >= 1000)
    {
        lastInfoTime = tick;
        dbg_print();
    }

    // We should not stop pondering until told so by the GUI
    if (ponder)
        return;

    if ((worker.limits.use_time_management() && (elapsed > tm.maximum() || stopOnPonderhit))
        || (worker.limits.movetime && elapsed >= worker.limits.movetime)
        || (worker.limits.nodes && worker.threads.nodes_searched() >= worker.limits.nodes))
        worker.threads.stop = true;
}

// Used to correct and extend PVs for moves that have a TB (but not a mate) score.
// Keeps the search based PV for as long as it is verified to maintain the game
// outcome, truncates afterwards. Finally, extends to mate the PV, providing a
// possible continuation (but not a proven mating line).
void syzygy_extend_pv(const OptionsMap&         options,
                      const Search::LimitsType& limits,
                      Position&                 pos,
                      RootMove&                 rootMove,
                      Value&                    v,
                      const usize               multiPV) {

    auto t_start      = std::chrono::steady_clock::now();
    int  moveOverhead = int(options["Move Overhead"]);
    bool rule50       = bool(options["Syzygy50MoveRule"]);

    // Do not use more than moveOverhead / 2 ms, if time management is active.
    // Under 'nodestime' the pos.do_move() calls come for free.
    auto time_abort = [&t_start, &moveOverhead, &limits, &multiPV]() -> bool {
        auto t_end = std::chrono::steady_clock::now();
        return !limits.npmsec && limits.use_time_management()
            && 2 * multiPV * std::chrono::duration<double, std::milli>(t_end - t_start).count()
                 >= moveOverhead;
    };

    if (time_abort())
        return;

    std::list<StateInfo> sts;

    // Step 0, do the rootMove, no correction allowed, as needed for MultiPV in TB
    auto& stRoot = sts.emplace_back();
    pos.do_move(rootMove.pv[0], stRoot);
    int ply = 1;

    // Step 1, walk the PV to the last position in TB with correct decisive score
    while (usize(ply) < rootMove.pv.size())
    {
        Move& pvMove = rootMove.pv[ply];

        RootMoves legalMoves;
        for (const auto& m : MoveList<LEGAL>(pos))
            legalMoves.emplace_back(m);

        TB::Config config = TB::rank_root_moves(options, pos, legalMoves, false, time_abort);
        RootMove&  rm     = *std::find(legalMoves.begin(), legalMoves.end(), pvMove);

        if (legalMoves[0].tbRank != rm.tbRank)
            break;

        ply++;

        auto& st = sts.emplace_back();
        pos.do_move(pvMove, st);

        // Do not allow for repetitions or drawing moves along the PV in TB regime
        if (config.rootInTB && ((rule50 && pos.is_draw(ply)) || pos.is_repetition(ply)))
        {
            pos.undo_move(pvMove);
            ply--;
            break;
        }

        // Full PV shown will thus be validated and end in TB. If we cannot
        // validate the full PV in time, we do not show it.
        if (config.rootInTB && time_abort())
            break;
    }

    // Resize the PV to the correct part
    rootMove.pv.resize(ply);

    // Step 2, now extend the PV to mate, as if the user explored syzygy-tables.info
    // using top ranked moves (minimal DTZ), which gives optimal mates only for simple
    // endgames e.g. KRvK.
    while (!(rule50 && pos.is_draw(0)))
    {
        if (time_abort())
            break;

        RootMoves legalMoves;
        for (const auto& m : MoveList<LEGAL>(pos))
        {
            auto&     rm = legalMoves.emplace_back(m);
            StateInfo tmpSI;
            pos.do_move(m, tmpSI);
            // Give a score of each move to break DTZ ties restricting opponent
            // mobility, but not giving the opponent a capture.
            for (const auto& mOpp : MoveList<LEGAL>(pos))
                rm.tbRank -= pos.capture(mOpp) ? 100 : 1;
            pos.undo_move(m);
        }

        // Mate found
        if (legalMoves.empty())
            break;

        // Sort moves according to their above assigned rank.
        // This will break ties for moves with equal DTZ in rank_root_moves.
        std::stable_sort(
          legalMoves.begin(), legalMoves.end(),
          [](const Search::RootMove& a, const Search::RootMove& b) { return a.tbRank > b.tbRank; });

        // The winning side tries to minimize DTZ, the losing side maximizes it
        TB::Config config = TB::rank_root_moves(options, pos, legalMoves, true, time_abort);

        // If DTZ is not available we might not find a mate, so we bail out
        if (!config.rootInTB || config.cardinality > 0)
            break;

        ply++;

        Move& pvMove = legalMoves[0].pv[0];
        rootMove.pv.push_back(pvMove);
        auto& st = sts.emplace_back();
        pos.do_move(pvMove, st);
    }

    // Finding a draw in this function is an exceptional case, that cannot happen when rule50 is false or
    // during engine game play, since we have a winning score, and play correctly
    // with TB support. However, it can be that a position is draw due to the 50 move
    // rule if it has been reached on the board with a non-optimal 50 move counter
    // (e.g. 8/8/6k1/3B4/3K4/4N3/8/8 w - - 54 106 ) which TB with dtz counter rounding
    // cannot always correctly rank. See also
    // https://github.com/official-stockfish/Stockfish/issues/5175#issuecomment-2058893495
    // We adjust the score to match the found PV. Note that a TB loss score can be
    // displayed if the engine did not find a drawing move yet, but eventually search
    // will figure it out (e.g. 1kq5/q2r4/5K2/8/8/8/8/7Q w - - 96 1 )
    if (pos.is_draw(0))
        v = VALUE_DRAW;

    // Undo the PV moves
    for (usize i = rootMove.pv.size(); i > 0; --i)
        pos.undo_move(rootMove.pv[i - 1]);

    // Inform if we couldn't get a full extension in time
    if (time_abort())
        sync_cout
          << "info string Syzygy based PV extension requires more time, increase Move Overhead as needed."
          << sync_endl;
}

void SearchManager::output_pv(Search::Worker&           worker,
                              const ThreadPool&         threads,
                              const TranspositionTable& tt,
                              Depth                     depth) {

    const auto nodes     = threads.nodes_searched();
    auto&      rootMoves = worker.rootMoves;
    auto&      pos       = worker.rootPos;
    usize      multiPV   = std::min(usize(worker.options["MultiPV"]), rootMoves.size());
    u64        tbHits    = threads.tb_hits() + (worker.tbConfig.rootInTB ? rootMoves.size() : 0);

    for (usize i = 0; i < multiPV; ++i)
    {
        bool usePreviousScore = rootMoves[i].score == -VALUE_INFINITE;

        if (depth == 1 && usePreviousScore && i > 0)
            continue;

        Depth d = usePreviousScore ? std::max(1, depth - 1) : depth;
        Value v = usePreviousScore ? rootMoves[i].previousScore : rootMoves[i].uciScore;

        if (v == -VALUE_INFINITE)
            v = VALUE_ZERO;

        bool isTBScore = worker.tbConfig.rootInTB && !is_mate_or_mated(v);
        v              = isTBScore ? rootMoves[i].tbScore : v;

        // Potentially correct and extend the PV, and in exceptional cases v.
        // Previous PVs have already been extended. Inexact flags indicate an unreliable PV.
        if (is_decisive(v) && !is_mate_or_mated(v) && !usePreviousScore
            && (!rootMoves[i].is_inexact() || isTBScore))
            syzygy_extend_pv(worker.options, worker.limits, pos, rootMoves[i], v, multiPV);

        // [SC:shashin] begin
        // Solo punteggi esatti: evita che fail-high/low inquinino lo stato Shashin.
        // [SC] Only the thread that analyses the root follows the zone of its own search.
        if (worker.shashinRootAnalysis && (i == 0) && d > worker.lastShashinUpdatedDepth
            && !rootMoves[i].is_inexact())
        {
            worker.getShashinManager().updateRootShashinState(v, pos, d, worker.rootDepth);
            worker.lastShashinUpdatedDepth = d;
        }
        // [SC:shashin] end

        std::string pv;
        for (Move m : usePreviousScore ? rootMoves[i].previousPV : rootMoves[i].pv)
            pv += UCIEngine::move(m, pos.is_chess960()) + " ";

        if (!pv.empty())
            pv.pop_back();

        auto wdl = worker.options["UCI_ShowWDL"] ? UCIEngine::wdl(v, pos) : "";

        // Scores cannot be both exact and inexact
        assert(!(rootMoves[i].inexactLower && rootMoves[i].inexactUpper));
        auto bound = rootMoves[i].inexactLower ? "lowerbound"
                   : rootMoves[i].inexactUpper ? "upperbound"
                                               : "";

        InfoFull info;

        info.depth    = d;
        info.selDepth = rootMoves[i].selDepth;
        info.multiPV  = i + 1;
        info.score    = {v, pos};
        info.wdl      = wdl;

        // TB and previous scores are exact, even though their flags may say otherwise
        if (!(isTBScore || usePreviousScore))
            info.bound = bound;

        TimePoint time = std::max(TimePoint(1), tm.elapsed_time());
        info.timeMs    = time;
        info.nodes     = nodes;
        info.nps       = nodes * 1000 / time;
        info.tbHits    = tbHits;
        info.pv        = pv;
        info.hashfull  = tt.hashfull();

        updates.onUpdateFull(info);
    }
}

// Called in case we have no ponder move before exiting the search,
// for instance, in case we stop the search during a fail high at root.
// We try hard to have a ponder move to return to the GUI, otherwise
// in case of 'ponder on' we have nothing to think about.
bool RootMove::extract_ponder_from_tt(const TranspositionTable& tt, Position& pos) {

    assert(pv.size() == 1 && pv[0] != Move::none());

    StateInfo st;
    pos.do_move(pv[0], st, &tt);

    if (!pos.is_draw(1))
    {
        auto [ttHit, ttData, ttWriter] = tt.probe(pos.key());
        if (ttHit && MoveList<LEGAL>(pos).contains(ttData.move))
            pv.push_back(ttData.move);
    }

    pos.undo_move(pv[0]);
    return pv.size() > 1;
}

// [SC:learning] begin
void putQLearningTrajectoryIntoLearningTable() {
    const double learning_rate = 0.5;
    const double gamma         = 0.99;

    std::lock_guard<std::mutex> lock(qLearningTrajectoryMutex);
    if (qLearningTrajectory.size() <= 1)
        return;

    for (size_t index = qLearningTrajectory.size() - 1; index > 0; --index)
    {
        LearningMove prevLearningMove =
          qLearningTrajectory[index - 1].persistedLearningMove.learningMove;
        const LearningMove currentLearningMove =
          qLearningTrajectory[index].persistedLearningMove.learningMove;

        prevLearningMove.score = prevLearningMove.score * (1 - learning_rate)
                               + learning_rate * (gamma * currentLearningMove.score);
        prevLearningMove.performance = WDLModel::get_win_probability_by_material(
          prevLearningMove.score, qLearningTrajectory[index - 1].materialClamp);

        LD.add_new_learning(qLearningTrajectory[index - 1].persistedLearningMove.key,
                            prevLearningMove);
    }

    qLearningTrajectory.clear();
}

void setStartPoint() {
    useLearning = true;
    LD.resume();
    std::lock_guard<std::mutex> lock(qLearningTrajectoryMutex);
    qLearningTrajectory.clear();
}
// [SC:learning] end

// [SC:shashin] begin
Value ShashinManager::static_value(const Eval::NNUE::Network&     network,
                                   Eval::NNUE::AccumulatorStack&  accumulators,
                                   Eval::NNUE::AccumulatorCaches& refreshTable,
                                   Position&                      rootPos,
                                   Search::Stack*                 ss,
                                   Value                          optimism) {
    if (ss->ply >= MAX_PLY || rootPos.is_draw(ss->ply))
        return VALUE_DRAW;

    MoveList<LEGAL> legalMoves(rootPos);
    if (legalMoves.size() == 0)
        return rootPos.checkers() ? -VALUE_MATE : VALUE_DRAW;  // il lato che muove e' matto

    if (!rootPos.checkers())
        return Eval::evaluate(network, rootPos, accumulators, refreshTable, optimism);

    Value bestValue = -VALUE_INFINITE;
    for (const auto& move : legalMoves)
    {
        StateInfo st;
        auto&     dirties = accumulators.push();
        rootPos.do_move(move, st, rootPos.gives_check(move), dirties, nullptr, nullptr);

        (ss + 1)->ply = ss->ply + 1;

        Value val = -static_value(network, accumulators, refreshTable, rootPos, ss + 1, -optimism);

        rootPos.undo_move(move);
        accumulators.pop();

        if (val > bestValue)
        {
            bestValue = val;
            if (bestValue == VALUE_MATE)
                break;
        }
    }

    return bestValue;
}

Value Worker::minimax_value(Position& pos, Search::Stack* ss, Depth depth) {
    Value   alpha = -VALUE_INFINITE;
    Value   beta  = VALUE_INFINITE;
    PVMoves pv;
    ss->pv = &pv;

    // MCTS avanza la posizione con Position::do_move() senza push sullo stack degli
    // accumulatori: senza reset si valuterebbe con l'accumulatore di un'altra posizione.
    accumulatorStack.reset();

    Value value = search<PV>(pos, ss, alpha, beta, depth, false);

    if (limits.mate && value >= VALUE_MATE_IN_MAX_PLY && VALUE_MATE - value <= 2 * limits.mate)
        threads.stop = true;

    return value;
}

Value Worker::minimax_value(
  Position& pos, Search::Stack* ss, Depth depth, Value alpha, Value beta) {

    PVMoves pv;
    ss->pv = &pv;

    Value value = VALUE_ZERO;
    Value delta = Value(18);

    accumulatorStack.reset();  // vedi l'overload precedente

    while (!threads.stop.load(std::memory_order_relaxed))
    {
        value = search<PV>(pos, ss, alpha, beta, depth, false);
        if (value <= alpha)
        {
            beta  = (alpha + beta) / 2;
            alpha = std::max(value - delta, -VALUE_INFINITE);
        }
        else if (value >= beta)
        {
            beta = std::min(value + delta, VALUE_INFINITE);
        }
        else
        {
            break;
        }
    }

    if (limits.mate && value >= VALUE_MATE_IN_MAX_PLY && VALUE_MATE - value <= 2 * limits.mate)
        threads.stop = true;

    return value;
}
// [SC:shashin] end

}  // namespace ShashChess