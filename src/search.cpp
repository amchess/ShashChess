/*
  ShashChess, a UCI chess playing engine derived from Stockfish
  Copyright (C) 2004-2025 Andrea Manzo, F. Ferraguti, K.Kiniama and ShashChess developers (see AUTHORS file)

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
/*
search.cpp is the heart of the modifications for Shashin, as this is where the heuristics are applied and the engine's behavior during search is guided.
We can integrate information from Shashin in a targeted way to influence:
Pruning and search rationalization: Deciding which branches to explore based on the Shashin area.
Heuristics for move selection: Improve move prioritization based on tactical or strategic context.
MCTS: Intelligently activate or avoid it.
Local assessments: Penalize or reward based on king's security, tactical complexity, etc.
*/
#include "search.h"
//variety begin
#include <random>
#include <chrono>
#include <cstdint>
//variety end
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
//from shashchess
#include <iostream>
#include <list>
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
//shashin
#include "syzygy/tbprobe.h"
#include "thread.h"
#include "timeman.h"
#include "tt.h"
#include "types.h"  //learning
#include "uci.h"
#include "ucioption.h"
#include "wdl/win_probability.h"  //livebook +shashin theory
#include "learn/learn.h"          //learning
#include "mcts/montecarlo.h"      //Montecarlo
//Livebook begin
#include "livebook/BaseLivebook.h"
#include "livebook/LichessEndgame.h"
#include "livebook/LichessMaster.h"
#include "livebook/ChessDb.h"
#include "livebook/LichessPlayer.h"
#include "livebook/LichessGames.h"
#include "livebook/Proxy.h"
#include "livebook/ChessDBContributor.h"
//Livebook end
namespace ShashChess {
using namespace ShashChess::Shashin;  //shashin
// learning begin
bool                                    useLearning          = true;
bool                                    enabledLearningProbe = false;
thread_local std::vector<QLearningMove> qLearningTrajectory;
// learning end

namespace TB = Tablebases;

void syzygy_extend_pv(const OptionsMap&             options,
                      const Search::LimitsType&     limits,
                      ShashChess::Position&         pos,
                      ShashChess::Search::RootMove& rootMove,
                      Value&                        v);

using namespace Search;

namespace {

constexpr int SEARCHEDLIST_CAPACITY = 32;
using SearchedList                  = ValueList<Move, SEARCHEDLIST_CAPACITY>;

// (*Scalers):
// The values with Scaler asterisks have proven non-linear scaling.
// They are optimized to time controls of 180 + 1.8 and longer,
// so changing them or adding conditions that are similar requires
// tests at these types of time controls.

// (*Scaler) All tuned parameters at time controls shorter than
// optimized for require verifications at longer time controls

int correction_value(const Worker& w, const Position& pos, const Stack* const ss) {
    const Color us    = pos.side_to_move();
    const auto  m     = (ss - 1)->currentMove;
    const auto  pcv   = w.pawnCorrectionHistory[pawn_correction_history_index(pos)][us];
    const auto  micv  = w.minorPieceCorrectionHistory[minor_piece_index(pos)][us];
    const auto  wnpcv = w.nonPawnCorrectionHistory[non_pawn_index<WHITE>(pos)][WHITE][us];
    const auto  bnpcv = w.nonPawnCorrectionHistory[non_pawn_index<BLACK>(pos)][BLACK][us];
    const auto  cntcv =
      m.is_ok() ? (*(ss - 2)->continuationCorrectionHistory)[pos.piece_on(m.to_sq())][m.to_sq()]
                 : 8;

    return 9536 * pcv + 8494 * micv + 10132 * (wnpcv + bnpcv) + 7156 * cntcv;
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

    constexpr int nonPawnWeight = 165;

    workerThread.pawnCorrectionHistory[pawn_correction_history_index(pos)][us] << bonus;
    workerThread.minorPieceCorrectionHistory[minor_piece_index(pos)][us] << bonus * 145 / 128;
    workerThread.nonPawnCorrectionHistory[non_pawn_index<WHITE>(pos)][WHITE][us]
      << bonus * nonPawnWeight / 128;
    workerThread.nonPawnCorrectionHistory[non_pawn_index<BLACK>(pos)][BLACK][us]
      << bonus * nonPawnWeight / 128;

    if (m.is_ok())
        (*(ss - 2)->continuationCorrectionHistory)[pos.piece_on(m.to_sq())][m.to_sq()]
          << bonus * 137 / 128;
}

// Add a small random component to draw evaluations to avoid 3-fold blindness
Value value_draw(size_t nodes) { return VALUE_DRAW - 1 + Value(nodes & 0x2); }

int variety;  //variety


Value value_to_tt(Value v, int ply);
Value value_from_tt(Value v, int ply, int r50c);
void  update_pv(Move* pv, Move move, const Move* childPv);
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
                      Move            TTMove);

}  // namespace


// livebook begin
#ifdef USE_LIVEBOOK
//livebook depth begin
int livebook_depth_count = 0;
int max_book_depth       = 255;
//livebook depth end

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
    // Clear the existing opening livebooks
    opening_livebooks.clear();

    if (!_proxy_url.empty())
    {
        Livebook::Proxy* proxy = new Livebook::Proxy(_proxy_url);
        if (_proxy_diversity)
        {
            proxy->set_action(Livebook::Action::QUERY);
        }
        else
        {
            proxy->set_action(Livebook::Action::QUERY_BEST);
        }
        opening_livebooks.push_back(proxy);
    }

    if (!_lichess_player.empty())
    {
        if (_lichess_player_color.empty())
        {
            _lichess_player_color = "white";
        }

        opening_livebooks.push_back(
          new Livebook::LichessPlayer(_lichess_player, _lichess_player_color));
    }

    if (_use_lichess_games)
    {
        opening_livebooks.push_back(new Livebook::LichessGames());
    }

    if (_use_lichess_masters)
    {
        opening_livebooks.push_back(new Livebook::LichessMaster());
    }

    if (_use_chess_db)
    {
        opening_livebooks.push_back(new Livebook::ChessDb());
    }
}

void Search::update_online_tablebases() {
    // Clear the existing endgame livebooks
    endgame_livebooks.clear();

    if (_use_chess_db_tablebase)  // ChessDb tablebase
    {
        endgame_livebooks.push_back(new Livebook::ChessDb());
    }

    if (_use_lichess_tablebase)  // Lichess tablebase
    {
        endgame_livebooks.push_back(new Livebook::LichessEndgame());
    }
}

void Search::set_chess_db_contribute(const bool chess_db_contribute) {
    _chess_db_contribute = chess_db_contribute;
}

void Search::set_proxy_diversity(const bool proxy_diversity) {
    _proxy_diversity = proxy_diversity;
    update_livebooks();
}
#endif
// livebook end


//variety begin
void Search::set_variety(const std::string& varietyOption) {
    if (varietyOption == "Off")
    {
        variety = 0;
    };
    if (varietyOption == "Standard")
    {
        variety = 1;
    };
    if (varietyOption == "Psychological")
    {
        variety = 2;
    };
};
//variety end
//for learning begin
inline bool is_game_decided(const Position& pos, Value lastScore) {
    static constexpr const Value DecidedGameEvalThreeshold = PawnValue * 5;
    static constexpr const int   DecidedGameMaxPly         = 150;
    static constexpr const int   DecidedGameMaxPieceCount  = 5;

    //Assume game is decided if |last sent score| is above DecidedGameEvalThreeshold
    if (is_valid(lastScore) && std::abs(lastScore) > DecidedGameEvalThreeshold)
        return true;

    //Assume game is decided (draw) if game ply is above 150
    if (pos.game_ply() > DecidedGameMaxPly)
        return true;

    if (pos.count<ALL_PIECES>() < DecidedGameMaxPieceCount)
        return true;

    //Assume game is not decided!
    return false;
}
//for learning end

Search::Worker::Worker(SharedState&                    sharedState,
                       std::unique_ptr<ISearchManager> sm,
                       size_t                          threadId,
                       NumaReplicatedAccessToken       token) :
    // Unpack the SharedState struct into member variables
    threadIdx(threadId),
    numaAccessToken(token),
    manager(std::move(sm)),
    bookMan(sharedState.bookMan),  //from book management
    options(sharedState.options),
    threads(sharedState.threads),
    tt(sharedState.tt),
    networks(sharedState.networks),
    refreshTable(networks[token]),
    //Shashin begin
    shashinManager(std::make_unique<ShashinManager>()),
    shConfig(sharedState.shashinConfig) {
    //Shashin end
    clear();
}
//from shashin begin
Search::Worker::~Worker() = default;
ShashinManager& Worker::getShashinManager() { return *shashinManager; }
//fom Shashin end

void Search::Worker::ensure_network_replicated() {
    // Access once to force lazy initialization.
    // We do this because we want to avoid initialization during search.
    (void) (networks[numaAccessToken]);
}

void Search::Worker::start_searching() {

    accumulatorStack.reset();

    // Non-main threads go directly to iterative_deepening()
    if (!is_mainthread())
    {
        iterative_deepening();
        return;
    }
    else
    {
        // It inizializes the rootPosition static state
        shashinManager->setStaticState(rootPos);
    }
    main_manager()->tm.init(limits, rootPos.side_to_move(), rootPos.game_ply(), options,
                            main_manager()->originalTimeAdjust);
    tt.new_search();
    // learning begin
    enabledLearningProbe = false;
    useLearning          = true;
    // learning end

    set_variety(options["Variety"]);  // variety

    Move bookMove = Move::none();  //Books management

    if (rootMoves.empty())
    {
        rootMoves.emplace_back(Move::none());
        main_manager()->updates.onUpdateNoMoves(
          {0, {rootPos.checkers() ? -VALUE_MATE : VALUE_DRAW, rootPos}});
    }
    else
    // Books management begin
    {

        bool think = true;
        if (!(limits.infinite || limits.mate || limits.depth || limits.nodes || limits.perft)
            && !main_manager()->ponder)
        {
            //Probe the configured books
            bookMove = bookMan.probe(rootPos, options);
            //Probe experience book begin
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
                                {
                                    bestMoves.push_back(move);
                                }
                                else
                                {
                                    break;
                                }
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
            //Probe experience book end
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
// Live Book begin
#ifdef USE_LIVEBOOK
        if (think && !bookMove)
        {
            if (!limits.infinite && !limits.mate)
            {
                livebook_depth_count = rootPos.game_ply();

                std::vector<Livebook::BaseLivebook*> livebooks;

                // If there are no more than 7 units on the board and there aren't the Syzygy Tbs, use the endgame livebooks
                const std::string& syzygyPath = static_cast<std::string>(options["SyzygyPath"]);

                int totalUnits = rootPos.count<ALL_PIECES>();

                if ((totalUnits <= 7)
                    && (syzygyPath.empty()
                        || ((!syzygyPath.empty())
                            && ((int(options["SyzygyProbeLimit"]) < totalUnits)))))
                {
                    livebooks = endgame_livebooks;
                }
                // If we are in the first max_book_depth plies, use the opening livebooks
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

                        // Iterate through the vector of moves and their corresponding analysis
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
                            {
                                livebook_depth_count++;
                            }
                        }

                        break;
                    }
                }
            }


            if (bookMove && std::count(rootMoves.begin(), rootMoves.end(), bookMove))
            {
                think = false;

                auto best_thread = threads.get_best_thread()->worker.get();
                std::swap(best_thread->rootMoves[0],
                          *std::find(best_thread->rootMoves.begin(), best_thread->rootMoves.end(),
                                     bookMove));

                /*
                for (auto th : threads)
                {
                    std::swap(th->worker->rootMoves[0],
                              *std::find(th->worker->rootMoves.begin(), th->worker->rootMoves.end(),
                                         bookMove));
                }
                */
            }
            else
            {
                bookMove = Move::none();
            }
        }
#endif
        // Live Book end

        //from Book and live book management begin
        if (!bookMove && think)
        {
            //Initialize `mctsThreads` threads only once before any thread have begun searching
            mctsThreads        = size_t(int(options["MCTSThreads"]));
            mctsMultiStrategy  = size_t(int(options["MCTS Multi Strategy"]));
            mctsMultiMinVisits = double(int(options["MCTS Multi MinVisits"]));

            threads.start_searching();  // start non-main threads
            iterative_deepening();      // main thread start searching
        }
        //from Book and live book management end
    }
    // Books management end

    // When we reach the maximum depth, we can arrive here without a raise of
    // threads.stop. However, if we are pondering or in an infinite search,
    // the UCI protocol states that we shouldn't print the best move before the
    // GUI sends a "stop" or "ponderhit" command. We therefore simply wait here
    // until the GUI sends one of those commands.
    while (!threads.stop && (main_manager()->ponder || limits.infinite))
    {}  // Busy wait for a stop or a ponder reset

    // Stop the threads if not already stopped (also raise the stop if
    // "ponderhit" just reset threads.ponder)
    threads.stop = true;

    // Wait until all threads have finished
    threads.wait_for_search_finished();

    // When playing in 'nodes as time' mode, subtract the searched nodes from
    // the available ones before exiting.
    if (limits.npmsec)
        main_manager()->tm.advance_nodes_time(threads.nodes_searched()
                                              - limits.inc[rootPos.side_to_move()]);

    Worker* bestThread = this;
    //no Skill

    if (int(options["MultiPV"]) == 1 && !limits.depth && !limits.mate  //no skill
        && rootMoves[0].pv[0] != Move::none())
        bestThread = threads.get_best_thread()->worker.get();

    main_manager()->bestPreviousScore        = bestThread->rootMoves[0].score;
    main_manager()->bestPreviousAverageScore = bestThread->rootMoves[0].averageScore;


    // learning begin
    if (!bookMove)
    {
        if (bestThread->completedDepth > 4 && LD.is_enabled() && !LD.is_paused())  // from learning
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
                const LearningMove* existingMove = LD.probe_move(plm.key, plm.learningMove.move);
                if (existingMove)
                    plm.learningMove.score = existingMove->score;
                QLearningMove qLearningMove;
                qLearningMove.persistedLearningMove = plm;
                const int qLearningMoveMaterial =
                  rootPos.count<PAWN>() + 3 * rootPos.count<KNIGHT>() + 3 * rootPos.count<BISHOP>()
                  + 5 * rootPos.count<ROOK>() + 9 * rootPos.count<QUEEN>();
                const int qLearningMoveMaterialClamp = std::clamp(qLearningMoveMaterial, 17, 78);
                qLearningMove.materialClamp          = qLearningMoveMaterialClamp;
                qLearningTrajectory.push_back(qLearningMove);
            }
            else
            {
                LD.add_new_learning(plm.key, plm.learningMove);
            }
        }
        if (!enabledLearningProbe)
        {
            useLearning = false;
        }
    }
    // learning end

    // Send again PV info if we have a new best thread
    if (bestThread != this)
    {                                                                                    //learning
        main_manager()->pv(*bestThread, threads, tt, bestThread->completedDepth, true);  //shashin
    }  //learning

    std::string ponder;

    if (bestThread->rootMoves[0].pv.size() > 1
        || bestThread->rootMoves[0].extract_ponder_from_tt(tt, rootPos))
        ponder = UCIEngine::move(bestThread->rootMoves[0].pv[1], rootPos.is_chess960());

    auto bestmove = UCIEngine::move(bestThread->rootMoves[0].pv[0], rootPos.is_chess960());
    main_manager()->updates.onBestmove(bestmove, ponder);
    // from learning begin
    // Save learning data if game is already decided
    if (!bookMove)
    {
        if (is_game_decided(rootPos, (bestThread->rootMoves[0].score)) && LD.is_enabled()
            && !LD.is_paused())
        {
            // Perform Q-learning if enabled
            if (LD.learning_mode() == LearningMode::Self)
            {
                putQLearningTrajectoryIntoLearningTable();
            }

            // Save to learning file
            if (!LD.is_readonly())
            {

                LD.persist(options);
            }
            // Stop learning until we receive *ucinewgame* command
            LD.pause();
        }
    }

    // from learning end

    // livebook begin
#ifdef USE_LIVEBOOK
    if (_chess_db_contribute)
    {
        contributor.contribute(rootPos, bestThread->rootMoves[0].pv[0]);
    }
#endif
    // livebook end
}

// Main iterative deepening loop. It calls search()
// repeatedly with increasing depth until the allocated thinking time has been
// consumed, the user stops the search, or the maximum search depth is reached.
void Search::Worker::iterative_deepening() {
    auto&          localShashinManager = *this->shashinManager;
    SearchManager* mainThread          = (is_mainthread() ? main_manager() : nullptr);

    Move pv[MAX_PLY + 1];

    Depth lastBestMoveDepth = 0;
    Value lastBestScore     = -VALUE_INFINITE;
    auto  lastBestPV        = std::vector{Move::none()};

    Value  alpha, beta;
    Value  bestValue     = -VALUE_INFINITE;
    Color  us            = rootPos.side_to_move();
    double timeReduction = 1, totBestMoveChanges = 0;
    int    delta, iterIdx                        = 0;

    // Allocate stack with extra size to allow access from (ss - 7) to (ss + 2):
    // (ss - 7) is needed for update_continuation_histories(ss - 1) which accesses (ss - 6),
    // (ss + 2) is needed for initialization of cutOffCnt.
    Stack  stack[MAX_PLY + 10] = {};
    Stack* ss                  = stack + 7;

    for (int i = 7; i > 0; --i)
    {
        (ss - i)->continuationHistory =
          &continuationHistory[0][0][NO_PIECE][0];  // Use as a sentinel
        (ss - i)->continuationCorrectionHistory = &continuationCorrectionHistory[NO_PIECE][0];
        (ss - i)->staticEval                    = VALUE_NONE;
    }

    for (int i = 0; i <= MAX_PLY + 2; ++i)
        (ss + i)->ply = i;

    ss->pv = pv;
    localShashinManager.initDynamicRootShashinState(networks[numaAccessToken], accumulatorStack,
                                                    refreshTable, rootPos, ss, VALUE_ZERO, shConfig,
                                                    rootDepth);

    const RootShashinState& rootShashinState = localShashinManager.getState();
    const auto&             dynamicDerived   = rootShashinState.dynamicDerived;
    bool                    isStrategical    = localShashinManager.isStrategical();
    bool                    isAggressive     = localShashinManager.isAggressive();
    bool                    isHighTal        = dynamicDerived.isHighTal;

    if (mainThread)
    {
        if (mainThread->bestPreviousScore == VALUE_INFINITE)
            mainThread->iterValue.fill(VALUE_ZERO);
        else
            mainThread->iterValue.fill(mainThread->bestPreviousScore);
    }

    size_t multiPV = size_t(options["MultiPV"]);
    //no skill

    multiPV                = std::min(multiPV, rootMoves.size());
    int searchAgainCounter = 0;

    lowPlyHistory.fill(89);
    // from mcts begin
    // Initialize optimism for MCTS
    optimism[WHITE] = optimism[BLACK] =
      VALUE_ZERO;  //Must initialize optimism before calling static_value(). Not sure if 'VALUE_ZERO' is the right value

    if (bool(options["MCTS by Shashin"]))
    {
        bool maybeDraw = rootPos.rule50_count() >= 90 || rootPos.upcoming_repetition(2);
        // Calculate static evaluation for root position
        Value rootPosValue = localShashinManager.static_value(
          networks[numaAccessToken], accumulatorStack, refreshTable, rootPos, ss,
          optimism[rootPos.side_to_move()]);  //shashin
        bool isMCTSExplorationApplicable = localShashinManager.isMCTSExplorationApplicable();
        bool mctsExplore                 = options["MCTS Explore"]
                        && (isMCTSExplorationApplicable
                            || localShashinManager.isHighPieceDensityCapablancaPosition());

        // Check if MCTS can be applied
        bool possibleMCTSByValue = localShashinManager.isMCTSApplicableByValue();
        if (!mainThread && multiPV == 1 && !maybeDraw && (possibleMCTSByValue || mctsExplore)
            && (rootPos.count<ALL_PIECES>() > 7) && (this->threadIdx) <= (size_t) (mctsThreads)
            && !is_game_decided(rootPos, rootPosValue))
        {
            MonteCarlo* monteCarlo = new MonteCarlo(rootPos, this, tt);

            if (monteCarlo)
            {
#if !defined(NDEBUG) && !defined(_NDEBUG)
                sync_cout << "info string *** Thread[" << threadIdx << "] is running MCTS search"
                          << sync_endl;
#endif

                monteCarlo->search(threads, limits, is_mainthread(), this);
                if ((this->threadIdx) == 1 && limits.infinite
                    && threads.stop.load(std::memory_order_relaxed))
                    monteCarlo->print_children();

                delete monteCarlo;

#if !defined(NDEBUG) && !defined(_NDEBUG)
                sync_cout << "info string *** Thread[" << threadIdx << "] finished MCTS search"
                          << sync_endl;
#endif

                return;
            }
        }

#if !defined(NDEBUG) && !defined(_NDEBUG)
        sync_cout << "info string *** Thread[" << threadIdx << "] is running A/B search"
                  << sync_endl;
#endif
    }
    // from mcts end
    // Iterative deepening loop until requested to stop or the target depth is reached
    while (++rootDepth < MAX_PLY && !threads.stop
           && !(limits.depth && mainThread && rootDepth > limits.depth))
    {
        // Age out PV variability metric
        if (mainThread)
            totBestMoveChanges /= 2;

        // Save the last iteration's scores before the first PV line is searched and
        // all the move scores except the (new) PV are set to -VALUE_INFINITE.
        for (RootMove& rm : rootMoves)
            rm.previousScore = rm.score;

        size_t pvFirst = 0;
        pvLast         = 0;

        if (!threads.increaseDepth)
            searchAgainCounter++;

        // MultiPV loop. We perform a full root search for each PV line
        for (pvIdx = 0; pvIdx < multiPV; ++pvIdx)
        {
            if (pvIdx == pvLast)
            {
                pvFirst = pvLast;
                for (pvLast++; pvLast < rootMoves.size(); pvLast++)
                    if (rootMoves[pvLast].tbRank != rootMoves[pvFirst].tbRank)
                        break;
            }

            // Reset UCI info selDepth for each depth and each PV line
            selDepth = 0;

            // Reset aspiration window starting size
            // Dynamic aspiration window based on Shashin style
            if (isStrategical)
            {
                // Narrow window for strategical positions (Capablanca/Petrosian)
                delta = 5 + threadIdx % 4;

                // Widen the window as depth increases to reduce re-searches
                if (rootDepth > 10)
                    delta += (rootDepth - 10) / 4;
                // Even narrower window for fortresses
                if (MoveConfig::isFortress)
                {
                    delta = 3 + threadIdx % 2;
                    if (rootDepth > 10)
                        delta += (rootDepth - 10) / 4;  // Same for fortresses
                }
            }
            else if (isAggressive)
            {
                // Wide window for aggressive positions (Tal)
                delta = 19 + threadIdx % 11;

                // Adjust for specific aggressive subtypes
                if (isHighTal)
                {
                    delta = 23 + threadIdx % 14;  // Even wider for High Tal
                }
            }
            else
            {
                // Default: original Stockfish calculation
                delta = 5 + threadIdx % 8 + std::abs(rootMoves[pvIdx].meanSquaredScore) / 9000;
            }

            // Additional adjustment based on depth
            if (rootDepth < 10)
            {
                delta = std::min(delta, 15);  // Limit window at low depths
            }

            Value avg = rootMoves[pvIdx].averageScore;
            alpha     = std::max(avg - delta, -VALUE_INFINITE);
            beta      = std::min(avg + delta, VALUE_INFINITE);

            // Adjust optimism based on root move's averageScore
            optimism[us]  = 137 * avg / (std::abs(avg) + 91);
            optimism[~us] = -optimism[us];

            // Start with a small aspiration window and, in the case of a fail
            // high/low, re-search with a bigger window until we don't fail
            // high/low anymore.
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
                // at nodes > 10M (rather than depth N, which can be reached quickly)
                if (mainThread && multiPV == 1 && (bestValue <= alpha || bestValue >= beta)
                    && nodes > 10000000)
                {                                                              //no warning
                    main_manager()->pv(*this, threads, tt, rootDepth, false);  //shashin
                }  //no warning
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
                    beta = std::min(bestValue + delta, VALUE_INFINITE);
                    ++failedHighCnt;
                }
                else
                    break;

                delta += delta / 3;

                assert(alpha >= -VALUE_INFINITE && beta <= VALUE_INFINITE);
            }

            // Sort the PV lines searched so far and update the GUI
            std::stable_sort(rootMoves.begin() + pvFirst, rootMoves.begin() + pvIdx + 1);

            if (mainThread
                && (threads.stop || pvIdx + 1 == multiPV || nodes > 10000000)
                // A thread that aborted search can have mated-in/TB-loss PV and
                // score that cannot be trusted, i.e. it can be delayed or refuted
                // if we would have had time to fully search other root-moves. Thus
                // we suppress this output and below pick a proven score/PV for this
                // thread (from the previous iteration).
                && !(threads.abortedSearch && is_loss(rootMoves[0].uciScore)))
            {                                                             //no warning
                main_manager()->pv(*this, threads, tt, rootDepth, true);  //shashin
            }  //no warning
            if (threads.stop)
                break;
        }

        if (!threads.stop)
            completedDepth = rootDepth;

        // We make sure not to pick an unproven mated-in score,
        // in case this thread prematurely stopped search (aborted-search).
        if (threads.abortedSearch && rootMoves[0].score != -VALUE_INFINITE
            && is_loss(rootMoves[0].score))
        {
            // Bring the last best move to the front for best thread selection.
            Utility::move_to_front(rootMoves, [&lastBestPV = std::as_const(lastBestPV)](
                                                const auto& rm) { return rm == lastBestPV[0]; });
            rootMoves[0].pv    = lastBestPV;
            rootMoves[0].score = rootMoves[0].uciScore = lastBestScore;
        }
        else if (rootMoves[0].pv[0] != lastBestPV[0])
        {
            lastBestPV        = rootMoves[0].pv;
            lastBestScore     = rootMoves[0].score;
            lastBestMoveDepth = rootDepth;
        }

        if (!mainThread)
            continue;

        // Have we found a "mate in x"?
        if (limits.mate && rootMoves[0].score == rootMoves[0].uciScore
            && ((rootMoves[0].score >= VALUE_MATE_IN_MAX_PLY
                 && VALUE_MATE - rootMoves[0].score <= 2 * limits.mate)
                || (rootMoves[0].score != -VALUE_INFINITE
                    && rootMoves[0].score <= VALUE_MATED_IN_MAX_PLY
                    && VALUE_MATE + rootMoves[0].score <= 2 * limits.mate)))
            threads.stop = true;

        //no Skill

        // Use part of the gained time from a previous stable move for the current move
        for (auto&& th : threads)
        {
            totBestMoveChanges += th->worker->bestMoveChanges;
            th->worker->bestMoveChanges = 0;
        }

        // Do we have time for the next iteration? Can we stop searching now?
        if (limits.use_time_management() && !threads.stop && !mainThread->stopOnPonderhit)
        {
            uint64_t nodesEffort =
              rootMoves[0].effort * 100000 / std::max(size_t(1), size_t(nodes));

            double fallingEval =
              (11.325 + 2.115 * (mainThread->bestPreviousAverageScore - bestValue)
               + 0.987 * (mainThread->iterValue[iterIdx] - bestValue))
              / 100.0;
            fallingEval = std::clamp(fallingEval, 0.5688, 1.5698);

            // If the bestMove is stable over several iterations, reduce time accordingly
            double k      = 0.5189;
            double center = lastBestMoveDepth + 11.57;
            timeReduction = 0.723 + 0.79 / (1.104 + std::exp(-k * (completedDepth - center)));
            double reduction =
              (1.455 + mainThread->previousTimeReduction) / (2.2375 * timeReduction);
            double bestMoveInstability = 1.04 + 1.8956 * totBestMoveChanges / threads.size();

            double totalTime =
              mainThread->tm.optimum() * fallingEval * reduction * bestMoveInstability;

            // Cap used time in case of a single legal move for a better viewer experience
            if (rootMoves.size() == 1)
                totalTime = std::min(502.0, totalTime);

            auto elapsedTime = elapsed();

            if (completedDepth >= 10 && nodesEffort >= 92425 && elapsedTime > totalTime * 0.666
                && !mainThread->ponder)
                threads.stop = true;

            // Stop the search if we have exceeded the totalTime or maximum
            if (elapsedTime > std::min(totalTime, double(mainThread->tm.maximum())))
            {
                // If we are allowed to ponder do not stop the search now but
                // keep pondering until the GUI sends "ponderhit" or "stop".
                if (mainThread->ponder)
                    mainThread->stopOnPonderhit = true;
                else
                    threads.stop = true;
            }
            else
                threads.increaseDepth = mainThread->ponder || elapsedTime <= totalTime * 0.503;
        }

        mainThread->iterValue[iterIdx] = bestValue;
        iterIdx                        = (iterIdx + 1) & 3;
    }

    if (!mainThread)
        return;

    mainThread->previousTimeReduction = timeReduction;
    //no Skill
}


void Search::Worker::do_move(Position& pos, const Move move, StateInfo& st, Stack* const ss) {
    do_move(pos, move, st, pos.gives_check(move), ss);
}

void Search::Worker::do_move(
  Position& pos, const Move move, StateInfo& st, const bool givesCheck, Stack* const ss) {
    bool       capture = pos.capture_stage(move);
    DirtyPiece dp      = pos.do_move(move, st, givesCheck, &tt);
    nodes.fetch_add(1, std::memory_order_relaxed);
    accumulatorStack.push(dp);
    if (ss != nullptr)
    {
        ss->currentMove         = move;
        ss->continuationHistory = &continuationHistory[ss->inCheck][capture][dp.pc][move.to_sq()];
        ss->continuationCorrectionHistory = &continuationCorrectionHistory[dp.pc][move.to_sq()];
    }
}

void Search::Worker::do_null_move(Position& pos, StateInfo& st) { pos.do_null_move(st, tt); }

void Search::Worker::undo_move(Position& pos, const Move move) {
    pos.undo_move(move);
    accumulatorStack.pop();
}

void Search::Worker::undo_null_move(Position& pos) { pos.undo_null_move(); }


// Reset histories, usually before a new game
void Search::Worker::clear() {
    mainHistory.fill(68);
    captureHistory.fill(-689);
    pawnHistory.fill(-1238);
    pawnCorrectionHistory.fill(5);
    minorPieceCorrectionHistory.fill(0);
    nonPawnCorrectionHistory.fill(0);

    ttMoveHistory = 0;

    for (auto& to : continuationCorrectionHistory)
        for (auto& h : to)
            h.fill(8);

    for (bool inCheck : {false, true})
        for (StatsType c : {NoCaptures, Captures})
            for (auto& to : continuationHistory[inCheck][c])
                for (auto& h : to)
                    h.fill(-529);

    for (size_t i = 1; i < reductions.size(); ++i)
        reductions[i] = int(2809 / 128.0 * std::log(i));

    refreshTable.clear(networks[numaAccessToken]);
}


// Main search function for both PV and non-PV nodes
template<NodeType nodeType>
Value Search::Worker::search(
  Position& pos, Stack* ss, Value alpha, Value beta, Depth depth, bool cutNode) {

    constexpr bool PvNode   = nodeType != NonPV;
    constexpr bool rootNode = nodeType == Root;
    const bool     allNode  = !(PvNode || cutNode);

    auto&                   localShashinManager = *this->shashinManager;
    const RootShashinState& rootShashinState    = localShashinManager.getState();  //for ShashChess
    // From Shashin fortress begin

    bool isFortressNode = localShashinManager.isFortress(rootPos);
    if (depth > 24 && !PvNode && isFortressNode)
    {
        depth = std::max(depth - 1, Depth(10));

        int progress = pos.rule50_count();
        if (progress < 29)
        {
            return ss->staticEval;
        }
        if (pos.count<ALL_PIECES>() > 12)
        {
            return ss->staticEval;
        }
        int window = std::min(15, 50 - progress - 10);
        if (progress >= (50 - window))
        {
            double weight = (progress - (50 - window)) / double(window);
            weight        = std::clamp(weight, 0.0, 1.0);

            bool  useNNUE  = (nodeType != NonPV) && (depth > 25);
            Value baseEval = useNNUE ? evaluate(pos) : ss->staticEval;

            Value drawInf = static_cast<Value>((1.0 - weight) * baseEval + weight * VALUE_DRAW);
            constexpr Value DrawBoundary = VALUE_DRAW - 384;
            if (drawInf >= DrawBoundary)
                return std::clamp(drawInf, DrawBoundary, VALUE_DRAW);
        }

        if (depth < 15)
        {
            static constexpr int Bonus[16] = {330, 308, 286, 264, 242, 220, 198, 176,
                                              154, 132, 110, 88,  66,  44,  22,  0};
            ss->staticEval += Bonus[depth];
        }
    }
    // From Shashin fortress end

    // Dive into quiescence search when the depth reaches zero
    if (depth <= 0)
    {
        constexpr auto nt = PvNode ? PV : NonPV;
        return qsearch<nt>(pos, ss, alpha, beta);
    }

    // Limit the depth if extensions made it too large
    depth = std::min(depth, MAX_PLY - 1);

    // Check if we have an upcoming move that draws by repetition
    if (!rootNode && alpha < VALUE_DRAW && pos.upcoming_repetition(ss->ply))
    {
        alpha = value_draw(nodes);
        if (alpha >= beta)
            return alpha;
    }

    assert(-VALUE_INFINITE <= alpha && alpha < beta && beta <= VALUE_INFINITE);
    assert(PvNode || (alpha == beta - 1));
    assert(0 < depth && depth < MAX_PLY);
    assert(!(PvNode && cutNode));

    Move      pv[MAX_PLY + 1];
    StateInfo st;

    Key  posKey;
    Move move, excludedMove = Move::none(), bestMove,
               expTTMove = Move::none();  // from learning
    Depth extension, newDepth;
    Value bestValue, value = VALUE_NONE, eval = VALUE_NONE, maxValue, expTTValue = VALUE_NONE,
                     probCutBeta = VALUE_NONE;  // from learning
    bool givesCheck, improving = false, priorCapture, opponentWorsening = false,
                     expTTHit = false;  //shashin +learning
    //from Shashin-Crystal begin
    bool gameCycle, capture, ttCapture;
    //from shashin crystal end
    int          priorReduction;
    Piece        movedPiece;
    bool         ourMove = !(ss->ply & 1);  // from Shashin Crystal
    SearchedList capturesSearched;
    SearchedList quietsSearched;
    // from learning begin
    bool updatedLearning = false;
    // flags to preserve node types
    bool disableNMAndPC = false;
    bool expectedPVNode = false;
    int  sibs           = 0;
    // from learning end
    // Step 1. Initialize node
    ss->inCheck   = pos.checkers();
    priorCapture  = pos.captured_piece();
    Color us      = pos.side_to_move();
    ss->moveCount = 0;
    bestValue     = -VALUE_INFINITE;
    maxValue      = VALUE_INFINITE;
    // from Crystal-shashin begin
    gameCycle       = false;
    bool kingDanger = !ourMove && king_danger(pos, us);

    ss->secondaryLine = false;
    ss->mainLine      = false;
    // from Crystal-shashin end
    // Full Threads patch begin
    if (fullSearch)
        improving = true;
    // Full Threads patch end
    // Check for the available remaining time
    if (is_mainthread())
        main_manager()->check_time(*this);

    // Used to send selDepth info to GUI (selDepth counts from 1, ply from 0)
    if (PvNode && selDepth < ss->ply + 1)
        selDepth = ss->ply + 1;
    //from crystal shashin begin
    const auto& dynamicDerived      = rootShashinState.dynamicDerived;
    const auto& staticState         = rootShashinState.staticState;
    bool        isStrategical       = localShashinManager.isStrategical();
    bool        isTacticalReactive  = dynamicDerived.isTacticalReactive;
    bool        stmKingExposed      = staticState.stmKingExposed;
    bool        opponentKingExposed = staticState.opponentKingExposed;
    bool        isSacrificial       = staticState.isSacrificial;
    bool        quietKingSafe       = !isSacrificial && (!stmKingExposed);
    bool        isTal               = localShashinManager.isTal();
    bool        isPetrosian         = localShashinManager.isPetrosian();
    bool        isCapablanca        = localShashinManager.isCapablanca();
    bool        isAggressive        = localShashinManager.isAggressive();
    bool        isTactical          = localShashinManager.isTactical();
    bool        isComplex           = localShashinManager.isComplexPosition();
    uint8_t     legalMoveCount      = staticState.legalMoveCount;
    //from crystal shashin end
    if (!rootNode)
    {
        // from Crystal-Shashin begin
        bool allowCycleDetection =
          (isStrategical && isComplex)
          || (localShashinManager.isTacticalDefensive() && localShashinManager.isLowActivity());

        if (allowCycleDetection && !ss->excludedMove)
        {
            if (pos.upcoming_repetition(ss->ply))
            {
                gameCycle = true;
            }
        }
        // from Crystal-Shashin end
        // Step 2. Check for aborted search and immediate draw
        if (threads.stop.load(std::memory_order_relaxed) || pos.is_draw(ss->ply)
            || ss->ply >= MAX_PLY)
            return (ss->ply >= MAX_PLY && !ss->inCheck) ? evaluate(pos) : value_draw(nodes);

        // Step 3. Mate distance pruning. Even if we mate at the next move our score
        // would be at best mate_in(ss->ply + 1), but if alpha is already bigger because
        // a shorter mate was found upward in the tree then there is no need to search
        // because we will never beat the current alpha. Same logic but with reversed
        // signs apply also in the opposite condition of being mated instead of giving
        // mate. In this case, return a fail-high score.
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

    // Step 4. Transposition table lookup
    excludedMove                   = ss->excludedMove;
    posKey                         = pos.key();
    auto [ttHit, ttData, ttWriter] = tt.probe(posKey);
    // Need further processing of the saved data
    ss->ttHit    = ttHit;
    ttData.move  = rootNode ? rootMoves[pvIdx].pv[0] : ttHit ? ttData.move : Move::none();
    ttData.value = ttHit ? value_from_tt(ttData.value, ss->ply, pos.rule50_count()) : VALUE_NONE;
    ss->ttPv     = excludedMove ? ss->ttPv : PvNode || (ttHit && ttData.is_pv);
    ttCapture    = ttData.move && pos.capture_stage(ttData.move);

    // At this point, if excluded, skip straight to step 6, static eval. However,
    // to save indentation, we list the condition in all code between here and there.

    // At non-PV nodes we check for an early TT cutoff
    if (!PvNode && !excludedMove && ttData.depth > depth - (ttData.value <= beta)
        && is_valid(ttData.value)  // Can happen when !ttHit or when access race in probe()
        && (ttData.bound & (ttData.value >= beta ? BOUND_LOWER : BOUND_UPPER))
        && (cutNode == (ttData.value >= beta) || depth > 5)
        // avoid a TT cutoff if the rule50 count is high and the TT move is zeroing
        && (depth > 8 || ttData.move == Move::none() || pos.rule50_count() < 80
            || (!ttCapture && type_of(pos.moved_piece(ttData.move)) != PAWN)))
    {
        // If ttMove is quiet, update move sorting heuristics on TT hit
        if (ttData.move && ttData.value >= beta)
        {
            // Bonus for a quiet ttMove that fails high
            if (!ttCapture)
                update_quiet_histories(pos, ss, *this, ttData.move,
                                       std::min(130 * depth - 71, 1043));

            // Extra penalty for early quiet moves of the previous ply
            if (prevSq != SQ_NONE && (ss - 1)->moveCount < 4 && !priorCapture)
                update_continuation_histories(ss - 1, pos.piece_on(prevSq), prevSq, -2142);
        }

        // Partial workaround for the graph history interaction problem
        // For high rule50 counts don't produce transposition table cutoffs.
        if (pos.rule50_count() < 96)
        {
            if (depth >= 8 && ttData.move && pos.pseudo_legal(ttData.move) && pos.legal(ttData.move)
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
    }
    // from learning begin
    // Step 4Bis. Global Learning Table lookup
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
                {
                    disableNMAndPC = true;
                }
                if (expTTValue > alpha && expTTValue < beta)
                {
                    expectedPVNode = true;
                    improving      = true;
                }
            }
            bool expTTCapture = learningMove->move && pos.capture_stage(expTTMove);
            // At this point, if excluded, skip straight to step 6, static eval. However,
            // to save indentation, we list the condition in all code between here and there.
            // At non-PV nodes we check for an early Global Learning Table cutoff
            if (!PvNode && !excludedMove && updatedLearning
                && learningMove->depth > depth - (expTTValue <= beta) && is_valid(expTTValue)
                && (cutNode == (expTTValue >= beta) || depth > 5)
                // avoid a exp cutoff if the rule50 count is high and the exp move is zeroing
                && (depth > 8 || expTTMove == Move::none() || pos.rule50_count() < 80
                    || (!expTTCapture && type_of(pos.moved_piece(expTTMove)) != PAWN)))
            {
                // If expTTMove is quiet, update move sorting heuristics on Global learning table hit
                if (expTTMove && expTTValue >= beta)
                {
                    // Bonus for a quiet ttMove that fails high
                    if (!expTTCapture)
                        update_quiet_histories(pos, ss, *this, ttData.move,
                                               std::min(130 * depth - 71, 1043));

                    // Extra penalty for early quiet moves of
                    // the previous ply
                    if (prevSq != SQ_NONE && (ss - 1)->moveCount < 4 && !priorCapture)
                        update_continuation_histories(ss - 1, pos.piece_on(prevSq), prevSq, -2142);
                }

                // Partial workaround for the graph history interaction problem
                // For high rule50 counts don't produce transposition table cutoffs.
                if (pos.rule50_count() < 96)
                {
                    if (depth >= 8 && ttData.move && pos.pseudo_legal(ttData.move)
                        && pos.legal(ttData.move) && !is_decisive(expTTValue))
                    {
                        do_move(pos, ttData.move, st, nullptr);
                        Key nextPosKey                             = pos.key();
                        auto [ttHitNext, ttDataNext, ttWriterNext] = tt.probe(nextPosKey);
                        undo_move(pos, ttData.move);

                        // Check that the ttValue after the tt move would also trigger a cutoff
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
    // from learning end

    // Step 5. Tablebases probe
    if (!rootNode && !excludedMove && tbConfig.cardinality)
    {
        int piecesCount = pos.count<ALL_PIECES>();

        if (piecesCount <= tbConfig.cardinality
            && (piecesCount < tbConfig.cardinality || depth >= tbConfig.probeDepth)
            && pos.rule50_count() == 0 && !pos.can_castle(ANY_CASTLING))
        {
            TB::ProbeState err;
            TB::WDLScore   wdl = Tablebases::probe_wdl(pos, &err);

            // Force check of time on the next occasion
            if (is_mainthread())
                main_manager()->callsCnt = 0;

            if (err != TB::ProbeState::FAIL)
            {
                tbHits.fetch_add(1, std::memory_order_relaxed);

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

    // Step 6. Static evaluation of the position
    Value      unadjustedStaticEval = VALUE_NONE;
    const auto correctionValue      = correction_value(*this, pos, ss);
    if (ss->inCheck)
    {
        // Skip early pruning when in check
        ss->staticEval = eval = (ss - 2)->staticEval;
        improving             = false;
        goto moves_loop;
    }
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
        //learning begin
        unadjustedStaticEval = evaluate(pos);
        if (!LD.is_enabled() || !expTTHit || !updatedLearning)
        {
            ss->staticEval = eval = to_corrected_static_eval(unadjustedStaticEval, correctionValue);

            // Static evaluation is saved as it was before adjustment by correction history
            ttWriter.write(posKey, VALUE_NONE, ss->ttPv, BOUND_NONE, DEPTH_UNSEARCHED, Move::none(),
                           unadjustedStaticEval, tt.generation());
        }
        else  // learning
        {
            // Never assume anything on values stored in Global Learning Table
            ss->staticEval = eval = expTTValue;
            if (!is_valid(eval))
            {
                ss->staticEval = eval =
                  to_corrected_static_eval(unadjustedStaticEval, correctionValue);
            }
            if (eval == VALUE_DRAW)
            {
                eval = value_draw(this->nodes);
            }
            // Can expTTValue be used as a better position evaluation?
            if (is_valid(expTTValue))
            {
                eval = expTTValue;
            }
        }
    }
    // from learning end

    // Use static evaluation difference to improve quiet move ordering
    if (((ss - 1)->currentMove).is_ok() && !(ss - 1)->inCheck && !priorCapture)
    {
        int bonus = std::clamp(-10 * int((ss - 1)->staticEval + ss->staticEval), -2023, 1563) + 583;
        mainHistory[~us][((ss - 1)->currentMove).from_to()] << bonus * 944 / 1024;
        if (!ttHit && type_of(pos.piece_on(prevSq)) != PAWN
            && ((ss - 1)->currentMove).type_of() != PROMOTION)
            pawnHistory[pawn_history_index(pos)][pos.piece_on(prevSq)][prevSq]
              << bonus * 1438 / 1024;
    }
    // full threads patch begin
    if (fullSearch)
        goto moves_loop;  // full threads patch
    // full threads patch end
    // from learning begin
    if (!expectedPVNode)
    {
        // Set up the improving flag, which is true if current static evaluation is
        // bigger than the previous static evaluation at our turn (if we were in
        // check at our previous move we go back until we weren't in check) and is
        // false otherwise. The improving flag is used in various pruning heuristics.
        improving = ss->staticEval > (ss - 2)->staticEval;
    }
    // from learning end
    opponentWorsening = ss->staticEval > -(ss - 1)->staticEval;

    if (priorReduction >= 3 && !opponentWorsening)
        depth++;
    if (priorReduction >= 2 && depth >= 2 && ss->staticEval + (ss - 1)->staticEval > 173)
        depth--;

    // Step 7. Razoring
    // If eval is really low, skip search entirely and return the qsearch value.
    // For PvNodes, we must have a guard against mates being returned.
    if (!PvNode)
    {
        // Base razoring threshold
        Value razorThreshold = alpha - 514 - 294 * depth * depth;

        // Adjust based on Shashin style
        if (isAggressive)
        {
            // Less razoring in aggressive positions (Tal) to avoid cutting tactical lines
            razorThreshold -= 100;
        }
        else if (isStrategical)
        {
            // More razoring in strategical positions (Petrosian/Capablanca)
            razorThreshold += 50;
        }

        // Additional adjustment for fortress positions
        if (isFortressNode)
        {
            // Reduce razoring in fortresses to ensure thorough search
            razorThreshold -= 75;
        }

        if (eval < razorThreshold)
            return qsearch<NonPV>(pos, ss, alpha, beta);
    }
    // Step 8. Futility pruning: child node with Shashin adjustments
    {
        // Calculate dynamic margin adjustment based on Shashin style
        Value marginMultiplier = 100;  // Default 100%

        if (isAggressive)
        {
            // Reduce pruning in aggressive positions: smaller margin
            marginMultiplier = 85 - 10 * (legalMoveCount < 20);
        }
        else if (isStrategical)
        {
            // Increase pruning in strategical positions: larger margin
            marginMultiplier = 115 + 5 * staticState.highMaterial;
        }

        // Further adjustments for king danger and fortresses
        if (staticState.kingDanger)
        {
            marginMultiplier = std::min(marginMultiplier, 70);
        }

        if (isFortressNode)
        {
            marginMultiplier = 60;
        }

        // Adjust for king exposure
        if (stmKingExposed || opponentKingExposed)
        {
            marginMultiplier = marginMultiplier * 93 / 100;
        }

        // Adjust for mobility in strategical positions
        if (isStrategical && legalMoveCount > 30)
        {
            marginMultiplier = marginMultiplier * 98 / 100;
        }

        auto futility_margin = [&](Depth d) {
            Value baseMargin         = 91 - 21 * !ss->ttHit;
            Value adjustedBaseMargin = (baseMargin * marginMultiplier) / 100;

            return adjustedBaseMargin * d - 2094 * improving * adjustedBaseMargin / 1024
                 - 1324 * opponentWorsening * adjustedBaseMargin / 4096 + (ss - 1)->statScore / 331
                 + std::abs(correctionValue) / 158105 - 35 * legalMoveCount / 32;
        };

        if (!ss->ttPv && depth < 14 && eval - futility_margin(depth) >= beta && eval >= beta
            && (!ttData.move || ttCapture) && !is_loss(beta) && !is_win(eval))
        {
            Value reduction;
            if (isAggressive || staticState.kingDanger)
                reduction = (eval - beta) / 4;  // Less reduction in aggressive/dangerous positions
            else if (isStrategical)
                reduction = (eval - beta) / 2;  // More reduction in strategical positions
            else
                reduction = (eval - beta) / 3;  // Default reduction

            // Clamp reduction to avoid too large values
            reduction = std::min(reduction, Value(150));
            return beta + reduction;
        }
    }

    // Step 9. Null move search with verification search
    //crystal-shashin logic begin
    if (cutNode && !disableNMAndPC && ss->staticEval >= beta - 18 * depth + 390
        && !excludedMove  //learning
        && pos.non_pawn_material(us) && ss->ply >= nmpMinPly && !is_loss(beta))
    {
        // Check if we should skip null move based on Shashin style and depth
        bool skipNullMove = false;

        if (isStrategical && depth >= 14)
            skipNullMove = true;
        else if (isAggressive && depth >= 10)
            skipNullMove = true;
        else if (isFortressNode && depth >= 6)
            skipNullMove = true;
        else if (isTactical || isSacrificial)
            skipNullMove = true;
        if (!skipNullMove)
        {
            assert((ss - 1)->currentMove != Move::null());
            assert(eval - beta >= 0);

            // Null move dynamic reduction based on depth
            Depth R = 6 + depth / 3;

            // Shashin-based adjustments to reduction
            if (isStrategical)
            {
                // More reduction in strategical positions (shallower search)
                R = std::min(R + 1, 8);
            }
            else if (isAggressive)
            {
                // Less reduction in strategical positions
                R = std::max(R - 1, 3);
            }

            // For fortresses, we can use even more reduction
            if (isFortressNode)
            {
                R = std::min(R + 2, 10);
            }

            ss->currentMove                   = Move::null();
            ss->continuationHistory           = &continuationHistory[0][0][NO_PIECE][0];
            ss->continuationCorrectionHistory = &continuationCorrectionHistory[NO_PIECE][0];

            do_null_move(pos, st);
            nmpGuard        = true;
            Value nullValue = -search<NonPV>(pos, ss + 1, -beta, -beta + 1, depth - R, false);
            nmpGuard        = false;
            undo_null_move(pos);

            if (nullValue >= beta && !is_win(nullValue))
            {
                if (nmpMinPly || depth < 16)
                    return nullValue;
                assert(!nmpMinPly);  // Recursive verification is not allowed

                // Do verification search at high depths, with null move pruning disabled
                // until ply exceeds nmpMinPly.
                nmpMinPly = ss->ply + 3 * (depth - R) / 4;
                Value v   = search<NonPV>(pos, ss, beta - 1, beta, depth - R, false);
                nmpMinPly = 0;

                if (v >= beta)
                    return nullValue;
            }
        }
        //only from shashin crystal end
    }

    improving |= ss->staticEval >= beta;

    // Step 10. Internal iterative reductions
    // At sufficient depth, reduce depth for PV/Cut nodes without a TTMove.
    // (*Scaler) Especially if they make IIR less aggressive.
    if (!allNode && depth >= 6 && !ttData.move && priorReduction <= 3)
    {
        Depth originalDepth = depth;

        // Apply base Stockfish reduction
        depth--;

        // Shashin adaptive reduction: apply additional reductions based on position style
        const bool avoidStep10 = localShashinManager.avoidStep10();
        if (quietKingSafe && !avoidStep10
            && originalDepth >= 6)  // Use originalDepth for consistent conditions
        {
            const bool ttReduction = cutNode && ttData.move && ttData.bound == BOUND_UPPER;
            if (ttReduction)
            {
                int reduction = 0;

                // Determine reduction based on Shashin style
                // Strategical positions: more aggressive reduction
                if (isStrategical)
                {
                    reduction = 2;  // More aggressive reduction in strategical positions
                }
                // Tactical positions: preserve depth
                else if (isAggressive || isTacticalReactive || isTactical || isSacrificial)
                {
                    // Preserve depth, but allow some reduction at very high depths
                    if (depth > 12)
                        reduction = 1;
                    else
                        reduction = 0;
                }
                // Default: moderate reduction
                else
                {
                    reduction = 1;
                }

                // Additional adjustments for special conditions
                if (isFortressNode)
                {
                    // Extra reduction in fortresses
                    reduction = std::min(reduction + 1, 3);
                }

                if (gameCycle)
                {
                    reduction = std::max(reduction - 1, 0);  // Less reduction in game cycles
                }

                // Apply the additional reduction, ensuring depth doesn't drop below 1
                depth = std::max(originalDepth - reduction, 1);
            }
        }
    }
    //by Shashin end
    // Step 11. ProbCut
    // If we have a good enough capture (or queen promotion) and a reduced search
    // returns a value much above beta, we can (almost) safely prune the previous move.
    probCutBeta = beta + 224 - 64 * improving;
    if (depth >= 3 && !disableNMAndPC
        && !is_decisive(beta)  //learning
        // If value from transposition table is lower than probCutBeta, don't attempt
        // probCut there
        && !(is_valid(ttData.value) && ttData.value < probCutBeta))
    {
        // Shashin adjustment: skip ProbCut in inappropriate positions
        bool skipProbCut = isFortressNode || (isStrategical && !staticState.kingDanger);

        if (!skipProbCut)
        {
            assert(probCutBeta < VALUE_INFINITE && probCutBeta > beta);

            MovePicker mp(pos, ttData.move, probCutBeta - ss->staticEval, &captureHistory);
            Depth      dynamicReduction = std::max((ss->staticEval - beta) / 306, -1);
            Depth      probCutDepth     = std::max(depth - 5 - dynamicReduction, 0);

            // Shashin-based depth adjustments
            if (isCapablanca && depth > 8)
            {
                probCutDepth = std::max(depth - 7 - dynamicReduction, 0);
            }
            else
            {
                // Adjust depth based on Shashin style
                if (isStrategical)
                    probCutDepth = std::max(probCutDepth - 1, 0);  // Less aggressive in strategical
                else if (isAggressive)
                    probCutDepth = std::max(probCutDepth + 1, 0);  // More aggressive in tactical
            }

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
                {
                    value = -search<NonPV>(pos, ss + 1, -probCutBeta, -probCutBeta + 1,
                                           probCutDepth, !cutNode);
                }

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

    // Shashin: Reduce depth in fortress positions when appropriate
    if (isFortressNode && depth > 12 && !ss->inCheck)
    {
        depth = std::max(depth - 2, 9);
    }

moves_loop:  // When in check, search starts here

    // Step 12. A small Probcut idea
    //from Shashin Crystal begin
    probCutBeta              = beta + 418;
    const bool commonProbcut = (ttData.bound & BOUND_LOWER) && ttData.depth >= depth - 4
                            && ttData.value >= probCutBeta && !is_decisive(beta)
                            && is_valid(ttData.value) && !is_decisive(ttData.value);

    if (commonProbcut)
    {
        // 1. ProbCut Crystal: only for complex tactical positions
        if (localShashinManager.allowCrystalProbCut() && depth >= 8)
        {
            const bool crystalConditions = !PvNode && ttCapture && !gameCycle;

            if (crystalConditions)
            {
                // Depth reduction based on position type
                Depth reduction = 4;
                if (localShashinManager.isTalTacticalHighMiddle())
                {
                    reduction = 3;  // Less reduction for Tal-style positions
                }
                else if (isFortressNode)
                {
                    reduction = 5;  // More reduction in fortress positions
                }

                if (ttData.depth >= depth - reduction)
                    return probCutBeta;
            }
        }

        // 2. Standard ProbCut for other positions
        return probCutBeta;
    }
    //from Shashin Crystal end

    const PieceToHistory* contHist[] = {
      (ss - 1)->continuationHistory, (ss - 2)->continuationHistory, (ss - 3)->continuationHistory,
      (ss - 4)->continuationHistory, (ss - 5)->continuationHistory, (ss - 6)->continuationHistory};


    MovePicker mp(pos, ttData.move, depth, &mainHistory, &lowPlyHistory, &captureHistory, contHist,
                  &pawnHistory, ss->ply);

    value = bestValue;

    int moveCount = 0;

    // Step 13. Loop through all pseudo-legal moves until no moves remain
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

        if (rootNode && is_mainthread() && nodes > 10000000)
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
        ss->secondaryLine =
          ((rootNode && moveCount > 1)
           || (!ourMove && (ss - 1)->secondaryLine && !excludedMove && moveCount == 1)
           || (ourMove && (ss - 1)->secondaryLine));
        (ss + 1)->quietMoveStreak = capture ? 0 : (ss->quietMoveStreak + 1);

        // Calculate new depth for this move
        newDepth = depth - 1;

        int delta = beta - alpha;

        Depth r = reduction(improving, depth, moveCount, delta, isStrategical, isAggressive);

        // Increase reduction for ttPv nodes (*Scaler)
        // Smaller or even negative value is better for short time controls
        // Bigger value is better for long time controls
        if (ss->ttPv)
            r += 946;

        // full threads patch begin
        if (fullSearch)
        {
            goto skipExtensionAndPruning;
        }
        // full threads patch end

        // Step 14. Pruning at shallow depth.
        // Depth conditions are important for mate finding.
        if (!rootNode && pos.non_pawn_material(us) && !is_loss(bestValue))
        {
            // Determina se applicare il pruning
            const bool allowPruning = !kingDanger || (isStrategical && depth < 8);

            if (allowPruning)
            {
                // Calcola la soglia di pruning in base allo stile Shashin
                int moveCountThreshold = (3 + depth * depth) / (2 - improving);

                // Adjust threshold based on Shashin style
                if (isStrategical)
                {
                    // Pruning pi� aggressivo in posizioni strategiche
                    moveCountThreshold = std::max(8, moveCountThreshold - 4);

                    // Skip quiet moves earlier in strategical positions
                    if (depth > 6 && moveCount > 15)
                    {
                        mp.skip_quiet_moves();
                    }
                }
                else if (isAggressive)
                {
                    // Pruning meno aggressivo in posizioni aggressive
                    moveCountThreshold = std::min(30, moveCountThreshold + 6);
                }

                // Skip quiet moves if movecount exceeds our adapted threshold
                if (moveCount >= moveCountThreshold)
                {
                    mp.skip_quiet_moves();
                }

                // Reduced depth of the next LMR search
                int lmrDepth = newDepth - r / 1024;

                // Adjust LMR depth based on Shashin style
                if (isStrategical)
                {
                    lmrDepth = std::max(1, lmrDepth - 1);  // Riduzione pi� aggressiva
                }
                else if (isAggressive)
                {
                    lmrDepth = std::min(lmrDepth + 1, newDepth);  // Riduzione meno aggressiva
                }

                if (capture || givesCheck)
                {
                    Piece capturedPiece = pos.piece_on(move.to_sq());
                    int captHist = captureHistory[movedPiece][move.to_sq()][type_of(capturedPiece)];

                    // Futility pruning for captures
                    if (!givesCheck && lmrDepth < 7)
                    {
                        Value futilityValue = ss->staticEval + 231 + 211 * lmrDepth
                                            + PieceValue[capturedPiece] + 130 * captHist / 1024;

                        if (futilityValue <= alpha)
                            continue;
                    }

                    // SEE based pruning for captures and checks
                    // Avoid pruning sacrifices of our last piece for stalemate
                    // Adjust margin based on Shashin style
                    int marginBase = 157 * depth + captHist / 29;
                    int margin     = std::max(marginBase, 0);

                    if (isStrategical)
                    {
                        margin += 20;  // Margin pi� conservativo in posizioni strategiche
                    }
                    else if (isAggressive)
                    {
                        margin = std::max(0, margin - 15);  // Margin pi� aggressivo
                    }

                    if ((alpha >= VALUE_DRAW || pos.non_pawn_material(us) != PieceValue[movedPiece])
                        && !pos.see_ge(move, -margin))
                        continue;
                }
                else
                {
                    int history = (*contHist[0])[movedPiece][move.to_sq()]
                                + (*contHist[1])[movedPiece][move.to_sq()]
                                + pawnHistory[pawn_history_index(pos)][movedPiece][move.to_sq()];

                    // Continuation history based pruning
                    // Adjust threshold based on Shashin style
                    int historyThreshold = -4312 * depth;
                    if (isStrategical)
                    {
                        historyThreshold -= 500;  // Pruning pi� aggressivo
                    }
                    else if (isAggressive)
                    {
                        historyThreshold += 500;  // Pruning meno aggressivo
                    }

                    if (history < historyThreshold)
                        continue;

                    history += 76 * mainHistory[us][move.from_to()] / 32;

                    lmrDepth += history / 3220;

                    // Adjust futility value based on Shashin style
                    Value futilityValue = ss->staticEval + 47 + 171 * !bestMove + 134 * lmrDepth
                                        + 90 * (ss->staticEval > alpha);

                    if (isStrategical)
                    {
                        futilityValue += 30;  // Pi� propensi al pruning
                    }
                    else if (isAggressive)
                    {
                        futilityValue -= 30;  // Meno propensi al pruning
                    }

                    // Futility pruning: parent node
                    if (!ss->inCheck && lmrDepth < 11 && futilityValue <= alpha)
                    {
                        if (bestValue <= futilityValue && !is_decisive(bestValue)
                            && !is_win(futilityValue))
                            bestValue = futilityValue;
                        continue;
                    }

                    lmrDepth = std::max(lmrDepth, 0);

                    // Prune moves with negative SEE
                    // Adjust SEE threshold based on Shashin style
                    int seeThreshold = -27 * lmrDepth * lmrDepth;
                    if (isStrategical)
                    {
                        seeThreshold -= 10;  // Pi� propensi al pruning
                    }
                    else if (isAggressive)
                    {
                        seeThreshold += 10;  // Meno propensi al pruning
                    }

                    if (!pos.see_ge(move, seeThreshold))
                        continue;
                }
            }
        }

        // Step 15. Extensions
        // Singular extension search. If all moves but one fail low on a search of (alpha-s, beta-s),
        // and just one fails high on (alpha, beta), then that move is singular and should be extended.
        if (!rootNode && move == ttData.move && !excludedMove && depth >= 6 + ss->ttPv
            && is_valid(ttData.value) && !is_decisive(ttData.value) && (ttData.bound & BOUND_LOWER)
            && ttData.depth >= depth - 3)
        {
            // Adjust singularBeta based on Shashin style
            int styleMarginAdjust = 0;
            if (isAggressive)
            {
                styleMarginAdjust = -12;  // More aggressive for Tal-style positions
            }
            else if (isStrategical)
            {
                styleMarginAdjust = 8;  // More cautious for Petrosian-style positions
            }

            Value singularBeta =
              ttData.value - (56 + 81 * (ss->ttPv && !PvNode) + styleMarginAdjust) * depth / 60;
            Depth singularDepth = newDepth / 2;

            // Adjust depth based on complexity and Shashin style
            if (isComplex && depth > 8)
            {
                singularDepth += 1;
            }

            // Additional adjustment for aggressive positions
            if (isAggressive && depth > 10)
            {
                singularDepth += 1;  // Deeper search for tactical positions
            }

            ss->excludedMove = move;
            value = search<NonPV>(pos, ss, singularBeta - 1, singularBeta, singularDepth, cutNode);
            ss->excludedMove = Move::none();

            if (value < singularBeta)
            {
                int corrValAdj   = std::abs(correctionValue) / 229958;
                int doubleMargin = -4 + 198 * PvNode - 212 * !ttCapture - corrValAdj
                                 - 921 * ttMoveHistory / 127649 - (ss->ply > rootDepth) * 45;
                int tripleMargin = 76 + 308 * PvNode - 250 * !ttCapture + 92 * ss->ttPv - corrValAdj
                                 - (ss->ply * 2 > rootDepth * 3) * 52;

                // Shashin-based adjustments
                if (isAggressive)
                {
                    doubleMargin -= 35;  // Extend more in aggressive positions
                    tripleMargin -= 50;
                }
                else if (isStrategical)
                {
                    doubleMargin += 25;  // Extend less in strategical positions
                    tripleMargin += 35;
                }
                // Additional adjustments for tactical and sacrificial positions
                if (isTactical || isSacrificial)
                {
                    // Only extend if the position is critical (e.g., near alpha/beta or with checks)
                    if (abs(ss->staticEval - alpha) < 100 || abs(ss->staticEval - beta) < 100
                        || givesCheck)
                    {
                        doubleMargin -= 20;
                        tripleMargin -= 30;
                    }
                }
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
                return value;
            }
            // Negative extensions
            // If other moves failed high over (ttValue - margin) without the
            // ttMove on a reduced search, but we cannot do multi-cut because
            // (ttValue - margin) is lower than the original beta, we do not know
            // if the ttMove is singular or can do a multi-cut, so we reduce the
            // ttMove in favor of other moves based on some conditions:

            // If the ttMove is assumed to fail high over current beta
            else if (ttData.value >= beta)
            {
                extension = -3;
            }
            // If we are on a cutNode but the ttMove is not assumed to fail high
            // over current beta
            else if (cutNode)
            {
                extension = -2;
            }
        }

skipExtensionAndPruning:  // full threads search patch
        // Step 16. Make the move
        do_move(pos, move, st, givesCheck, ss);
        bool doLMRStep = !fullSearch;  // Full threads patch

        // Add extension to new depth
        newDepth += extension;
        uint64_t nodeCount = rootNode ? uint64_t(nodes) : 0;

        // Decrease reduction for PvNodes (*Scaler)
        if (ss->ttPv)
            r -= 2618 + PvNode * 991 + (ttData.value > alpha) * 903
               + (ttData.depth >= depth) * (978 + cutNode * 1051);
        // Shashin style based adjustements
        if (!fullSearch)
        {
            // Tactical and sacrificial positions: less reduction to preserve depth
            if ((isTactical || isSacrificial) && depth > 8)
            {
                r -= 256;  // Reduction by 0.5 plies
            }
            // Strategical style: more reduction in quiet phases
            else if (isStrategical && !ss->inCheck && depth > 6)
            {
                r += 256;  // Added reduction by 0.25 plies
            }
        }
        // These reduction adjustments have no proven non-linear scaling

        r += 543;  // Base reduction offset to compensate for other tweaks
        r -= moveCount * 66;
        r -= std::abs(correctionValue) / 30450;

        // Increase reduction for cut nodes
        if (cutNode)
            r += 3094 + 1056 * !ttData.move;

        // Increase reduction if ttMove is a capture
        if (ttCapture)
            r += 1415;

        // Increase reduction if next ply has a lot of fail high
        if ((ss + 1)->cutoffCnt > 2)
            r += 1051 + allNode * 814;

        r += (ss + 1)->quietMoveStreak * 50;

        // For first picked move (ttMove) reduce reduction
        if (move == ttData.move)
            r -= 2018;

        if (capture)
            ss->statScore = 803 * int(PieceValue[pos.captured_piece()]) / 128
                          + captureHistory[movedPiece][move.to_sq()][type_of(pos.captured_piece())];
        else
            ss->statScore = 2 * mainHistory[us][move.from_to()]
                          + (*contHist[0])[movedPiece][move.to_sq()]
                          + (*contHist[1])[movedPiece][move.to_sq()];

        // Decrease/increase reduction for moves with a good/bad history
        r -= ss->statScore * 794 / 8192;

        // Step 17. Late moves reduction / extension (LMR)
        if (depth >= 2 && moveCount > std::max(sibs, 1) && doLMRStep)
        {
            // Calculate base reduced depth
            Depth d = std::max(1, std::min(newDepth - r / 1024, newDepth + 2)) + PvNode;

            // Apply Shashin-style adjustments
            // Adjustments aggiuntivi basati sullo stile Shashin
            if (isStrategical)
            {
                // More reduction in strategical positions (Capablanca/Petrosian)
                d = std::max(1, d - 1);

                // Additional reduction for quiet moves in strategical positions
                if (!capture && depth > 8)
                    d = std::max(1, d - 1);
            }
            else if (isAggressive)
            {
                // Less reduction in aggressive positions (Tal)
                d = std::min(d + 1, newDepth);

                // Even less reduction for checks and tactical moves
                if (givesCheck || isTactical)
                    d = std::min(d + 1, newDepth);
            }

            // Special handling for fortress positions
            if (isFortressNode)
            {
                // Maximum reduction in fortress positions
                d = std::max(1, d - 2);
            }

            // Execute reduced search
            ss->reduction = newDepth - d;
            value         = -search<NonPV>(pos, ss + 1, -(alpha + 1), -alpha, d, true);
            ss->reduction = 0;

            if (value > alpha)
            {
                const bool doDeeperSearch = d < newDepth && value > (bestValue + 43 + 2 * newDepth);
                const bool doShallowerSearch = value < bestValue + 9;
                // Calcola una volta le informazioni Shashin rilevanti
                const bool isCrystal = localShashinManager.useStep17CrystalLogic();
                // Determine if we should use Crystal-style re-search
                bool useCrystalResearch = isCrystal && doDeeperSearch && isTactical;
                useCrystalResearch &= (ss->ply + depth < MAX_PLY - 2);
                useCrystalResearch &= (ss->ply < 2 * rootDepth);
                useCrystalResearch &= (value > alpha + 150);

                if (useCrystalResearch)
                {
                    // Crystal-style deeper research
                    Depth extDepth = newDepth + 2;
                    value = -search<NonPV>(pos, ss + 1, -(alpha + 1), -alpha, extDepth, !cutNode);
                }
                else
                {
                    // Standard re-search logic
                    newDepth += doDeeperSearch - doShallowerSearch;
                    if (newDepth > d)
                        value =
                          -search<NonPV>(pos, ss + 1, -(alpha + 1), -alpha, newDepth, !cutNode);
                }

                // Calculate history bonus based on Shashin style
                int bonus = 1365;  // Base Stockfish value

                // Increase bonus for aggressive positions
                if (isAggressive)
                    bonus += 150;

                // Moderate bonus for strategical positions
                else if (isStrategical)
                    bonus += 100;

                // Special bonus for fortress positions
                if (isFortressNode)
                    bonus += 200;

                // Post LMR continuation history updates
                update_continuation_histories(ss, movedPiece, move.to_sq(), bonus);
            }
        }

        // Step 18. Full-depth search when LMR is skipped
        else if (!PvNode || moveCount > 1)
        {
            // Increase reduction if ttMove is not present
            if (!ttData.move)
                r += 1118;

            // Note that if expected reduction is high, we reduce search depth here
            value = -search<NonPV>(pos, ss + 1, -(alpha + 1), -alpha,
                                   newDepth - (r > 3212) - (r > 4784 && newDepth > 2), !cutNode);
        }

        // For PV nodes only, do a full PV search on the first move or after a fail high,
        // otherwise let the parent node fail low with value <= alpha and try another move.
        if (PvNode && (moveCount == 1 || value > alpha))
        {
            (ss + 1)->pv    = pv;
            (ss + 1)->pv[0] = Move::none();

            // Extend move from transposition table if we are about to dive into qsearch.
            if (move == ttData.move && rootDepth > 8)
                newDepth = std::max(newDepth, 1);

            value = -search<PV>(pos, ss + 1, -beta, -alpha, newDepth, false);
        }

        // Step 19. Undo move
        undo_move(pos, move);

        assert(value > -VALUE_INFINITE && value < VALUE_INFINITE);

        // Step 20. Check for a new best move
        // Finished searching the move. If a stop occurred, the return value of
        // the search cannot be trusted, and we return immediately without updating
        // best move, principal variation nor transposition table.
        if (threads.stop.load(std::memory_order_relaxed))
            return VALUE_ZERO;

        if (rootNode)
        {
            RootMove& rm = *std::find(rootMoves.begin(), rootMoves.end(), move);

            rm.effort += nodes - nodeCount;

            rm.averageScore =
              rm.averageScore != -VALUE_INFINITE ? (value + rm.averageScore) / 2 : value;

            rm.meanSquaredScore = rm.meanSquaredScore != -VALUE_INFINITE * VALUE_INFINITE
                                  ? (value * std::abs(value) + rm.meanSquaredScore) / 2
                                  : value * std::abs(value);

            // PV move or new best move?
            if (moveCount == 1 || value > alpha)
            {
                rm.score = rm.uciScore = value;
                rm.selDepth            = selDepth;
                rm.scoreLowerbound = rm.scoreUpperbound = false;

                if (value >= beta)
                {
                    rm.scoreLowerbound = true;
                    rm.uciScore        = beta;
                }
                else if (value <= alpha)
                {
                    rm.scoreUpperbound = true;
                    rm.uciScore        = alpha;
                }

                rm.pv.resize(1);

                assert((ss + 1)->pv);

                for (Move* m = (ss + 1)->pv; *m != Move::none(); ++m)
                    rm.pv.push_back(*m);

                // We record how often the best move has been changed in each iteration.
                // This information is used for time management. In MultiPV mode,
                // we must take care to only do this for the first PV line.
                if (moveCount > 1 && !pvIdx)
                    ++bestMoveChanges;
            }
            else
                // All other moves but the PV, are set to the lowest value: this
                // is not a problem when sorting because the sort is stable and the
                // move position in the list is preserved - just the PV is pushed up.
                rm.score = -VALUE_INFINITE;
        }

        // In case we have an alternative move equal in eval to the current bestmove,
        // promote it to bestmove by pretending it just exceeds alpha (but not beta).
        int inc = (value == bestValue && ss->ply + 2 >= rootDepth && (int(nodes) & 14) == 0
                   && !is_win(std::abs(value) + 1));

        if (value + inc > bestValue)
        {
            bestValue = value;

            if (value + inc > alpha)
            {
                bestMove = move;

                if (PvNode && !rootNode)  // Update pv even in fail-high case
                    update_pv(ss->pv, move, (ss + 1)->pv);

                if (value >= beta)
                {
                    // (*Scaler) Especially if they make cutoffCnt increment more often.
                    ss->cutoffCnt += (extension < 2) || PvNode;
                    assert(value >= beta);  // Fail high
                    break;
                }

                //from crystal-shashin begin
                // Reduce other moves if we have found at least one score improvement
                if (depth > 2 && depth < 14 && !is_decisive(value))
                {
                    const int baseReduction = 2;
                    int       reduction     = baseReduction;

                    // Specific tactical conditions for style (Shashin-correct)
                    const bool criticalTactical =
                      (opponentKingExposed && isTal) || (stmKingExposed && isPetrosian);

                    // Hybrid Shashin Strategy
                    if (criticalTactical)
                    {
                        // Depth preserved in critical positions for style
                        reduction = 1;
                    }
                    else if (isStrategical && !stmKingExposed && !opponentKingExposed)
                    {
                        // Positional scenario: more aggressive reduction
                        reduction = (depth > 8) ? 3 : 2;

                        // Extra caution in congested positions
                        if (staticState.allPiecesCount > 20)
                        {
                            reduction = std::min(reduction, 2);
                        }
                    }
                    else if (gameCycle)
                    {
                        // Cycle management: balancing safety and progress
                        reduction = (pos.rule50_count() > 20) ? 3 : 1;
                    }

                    // Adaptive safety clamp
                    reduction = std::clamp(reduction, 1, depth - 1);

                    depth -= reduction;
                }
                //from crystal-shashin end

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

    // Step 21. Check for mate and stalemate
    // All legal moves have been searched and if there are no legal moves, it
    // must be a mate or a stalemate. If we are in a singular extension search then
    // return a fail low score.

    assert(moveCount || !ss->inCheck || excludedMove || !MoveList<LEGAL>(pos).size());

    // Adjust best value for fail high cases
    if (bestValue >= beta && !is_decisive(bestValue) && !is_decisive(alpha))
        bestValue = (bestValue * depth + beta) / (depth + 1);

    if (!moveCount)
        bestValue = excludedMove ? alpha : ss->inCheck ? mated_in(ss->ply) : VALUE_DRAW;

    // If there is a move that produces search value greater than alpha,
    // we update the stats of searched moves.
    else if (bestMove)
    {
        update_all_stats(pos, ss, *this, bestMove, prevSq, quietsSearched, capturesSearched, depth,
                         ttData.move);
        if (!PvNode)
            ttMoveHistory << (bestMove == ttData.move ? 809 : -865);
    }

    // Bonus for prior quiet countermove that caused the fail low
    else if (!priorCapture && prevSq != SQ_NONE)
    {
        int bonusScale = -228;
        bonusScale -= (ss - 1)->statScore / 104;
        bonusScale += std::min(63 * depth, 508);
        bonusScale += 184 * ((ss - 1)->moveCount > 8);
        bonusScale += 143 * (!ss->inCheck && bestValue <= ss->staticEval - 92);
        bonusScale += 149 * (!(ss - 1)->inCheck && bestValue <= -(ss - 1)->staticEval - 70);

        bonusScale = std::max(bonusScale, 0);

        const int scaledBonus = std::min(144 * depth - 92, 1365) * bonusScale;

        update_continuation_histories(ss - 1, pos.piece_on(prevSq), prevSq,
                                      scaledBonus * 400 / 32768);

        mainHistory[~us][((ss - 1)->currentMove).from_to()] << scaledBonus * 220 / 32768;

        if (type_of(pos.piece_on(prevSq)) != PAWN && ((ss - 1)->currentMove).type_of() != PROMOTION)
            pawnHistory[pawn_history_index(pos)][pos.piece_on(prevSq)][prevSq]
              << scaledBonus * 1164 / 32768;
    }

    // Bonus for prior capture countermove that caused the fail low
    else if (priorCapture && prevSq != SQ_NONE)
    {
        Piece capturedPiece = pos.captured_piece();
        assert(capturedPiece != NO_PIECE);
        captureHistory[pos.piece_on(prevSq)][prevSq][type_of(capturedPiece)] << 964;
    }

    if (PvNode)
        bestValue = std::min(bestValue, maxValue);

    // If no good move is found and the previous position was ttPv, then the previous
    // opponent move is probably good and the new position is added to the search tree.
    if (bestValue <= alpha)
        ss->ttPv = ss->ttPv || (ss - 1)->ttPv;

    // Write gathered information in transposition table. Note that the
    // static evaluation is saved as it was before correction history.
    if (!excludedMove && !(rootNode && pvIdx))
        ttWriter.write(posKey, value_to_tt(bestValue, ss->ply), ss->ttPv,
                       bestValue >= beta    ? BOUND_LOWER
                       : PvNode && bestMove ? BOUND_EXACT
                                            : BOUND_UPPER,
                       moveCount != 0 ? depth : std::min(MAX_PLY - 1, depth + 6), bestMove,
                       unadjustedStaticEval, tt.generation());

    // Adjust correction history
    if (!ss->inCheck && !(bestMove && pos.capture(bestMove))
        && ((bestValue < ss->staticEval && bestValue < beta)  // negative correction & no fail high
            || (bestValue > ss->staticEval && bestMove)))     // positive correction & no fail low
    {
        auto bonus = std::clamp(int(bestValue - ss->staticEval) * depth / 8,
                                -CORRECTION_HISTORY_LIMIT / 4, CORRECTION_HISTORY_LIMIT / 4);
        update_correction_history(pos, ss, *this, bonus);
    }

    assert(bestValue > -VALUE_INFINITE && bestValue < VALUE_INFINITE);

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

    // Check if we have an upcoming move that draws by repetition
    if (alpha < VALUE_DRAW && pos.upcoming_repetition(ss->ply))
    {
        alpha = value_draw(nodes);
        if (alpha >= beta)
            return alpha;
    }

    Move      pv[MAX_PLY + 1];
    StateInfo st;

    Key   posKey;
    Move  move, bestMove, expTTMove = Move::none();                               //learning
    Value bestValue, value, futilityBase, expTTValue = VALUE_NONE;                //learning
    bool  pvHit, givesCheck, capture, expTTHit = false, updatedLearning = false;  //learning
    int   moveCount;

    // Step 1. Initialize node
    if (PvNode)
    {
        (ss + 1)->pv = pv;
        ss->pv[0]    = Move::none();
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
    // Need further processing of the saved data
    ss->ttHit    = ttHit;
    ttData.move  = ttHit ? ttData.move : Move::none();
    ttData.value = ttHit ? value_from_tt(ttData.value, ss->ply, pos.rule50_count()) : VALUE_NONE;
    pvHit        = ttHit && ttData.is_pv;

    // Crystal-Shashin QSearch optimization begin
    const auto& staticState    = rootShashinState.staticState;
    const auto& dynamicDerived = rootShashinState.dynamicDerived;
    const bool  canUseTTValue  = !PvNode && is_valid(ttData.value)
                            && (ttData.bound & (ttData.value >= beta ? BOUND_LOWER : BOUND_UPPER));
    const bool isStrategical       = dynamicDerived.isStrategical;
    const bool isAggressive        = dynamicDerived.isAggressive;
    const bool stmKingExposed      = staticState.stmKingExposed;
    const bool opponentKingExposed = opponentKingExposed;
    if (canUseTTValue)
    {
        if (ttData.depth >= DEPTH_QS)
            return ttData.value;
        if (staticState.legalMoveCount < 35 && std::abs(ttData.value) > 60)
        {
            const bool areQuietKings = !stmKingExposed && !opponentKingExposed;
            if (!isStrategical && areQuietKings)
            {
                const bool depthCondition =
                  ttData.depth >= ADJUSTED_QS_DEPTH && ttData.depth > MIN_DEPTH_FOR_QS_CUTOFF;
                const bool kingSafety =
                  (!stmKingExposed && !opponentKingExposed) || ttData.depth > DEPTH_QS + 4;
                if (depthCondition && kingSafety)
                    return ttData.value;
            }
        }
    }
    // Crystal-Shashin QSearch optimization end
    // Step 3 bis learning begin
    expTTHit        = false;
    updatedLearning = false;

    if (useLearning && LD.is_enabled())
    {
        const LearningMove* learningMove = nullptr;
        int siblings = LD.probeByMaxDepthAndScore(posKey, learningMove);
        (void)siblings; // Sopprime l'avviso di variabile inutilizzata
        if (learningMove)
        {
            expTTHit   = true;
            expTTMove  = learningMove->move;
            expTTValue = learningMove->score;

            // Usa la mossa di learning per l'ordinamento se non abbiamo TT move
            if (!ttHit)
            {
                ttData.move = expTTMove;
            }
            const bool canUseExpTTValue = !PvNode && is_valid(expTTValue);
            if (canUseExpTTValue)
            {
                if (learningMove->depth >= DEPTH_QS)
                {
                    updatedLearning = true;
                    return expTTValue;
                }
                if (staticState.legalMoveCount < 35 && std::abs(expTTValue) > 60)
                {
                    const bool areQuietKings = !stmKingExposed && !opponentKingExposed;
                    if (!isStrategical && areQuietKings)
                    {
                        const bool depthCondition = learningMove->depth >= ADJUSTED_QS_DEPTH
                                                 && learningMove->depth > MIN_DEPTH_FOR_QS_CUTOFF;
                        const bool kingSafety = (!stmKingExposed && !opponentKingExposed)
                                             || learningMove->depth > DEPTH_QS + 4;
                        if (depthCondition && kingSafety)
                        {
                            updatedLearning = true;
                            return expTTValue;
                        }
                    }
                }
            }
        }
    }
    // Step 3 bis learning end
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
            //priority to the learning - only if we are not in check
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
                bestValue = (bestValue + beta) / 2;
            if (!ss->ttHit)
                ttWriter.write(posKey, value_to_tt(bestValue, ss->ply), false, BOUND_LOWER,
                               DEPTH_UNSEARCHED, Move::none(), unadjustedStaticEval,
                               tt.generation());
            return bestValue;
        }

        if (bestValue > alpha)
            alpha = bestValue;

        futilityBase = ss->staticEval + 352;
        // Shashin begin: Increase futility margin for aggressive positions to avoid cutting tactical moves
        // Utilizza rootShashinState per determinare se siamo in una posizione aggressiva
        if (isAggressive)
            futilityBase += 3;  // Increase margin by 20 points
        //Shashin end
    }

    const PieceToHistory* contHist[] = {(ss - 1)->continuationHistory,
                                        (ss - 2)->continuationHistory};

    Square prevSq = ((ss - 1)->currentMove).is_ok() ? ((ss - 1)->currentMove).to_sq() : SQ_NONE;

    // Initialize a MovePicker object for the current position, and prepare to search
    // the moves. We presently use two stages of move generator in quiescence search:
    // captures, or evasions only when in check.
    MovePicker mp(pos, ttData.move, DEPTH_QS, &mainHistory, &lowPlyHistory, &captureHistory,
                  contHist, &pawnHistory, ss->ply);

    // Step 5. Loop through all pseudo-legal moves until no moves remain or a beta
    // cutoff occurs.
    while ((move = mp.next_move()) != Move::none())
    {
        assert(move.is_ok());

        if (!pos.legal(move))
            continue;

        givesCheck = pos.gives_check(move);
        capture    = pos.capture_stage(move);

        moveCount++;

        // Step 6. Pruning with Shashin adjustments
        if (!is_loss(bestValue))
        {
            // Adjust move count threshold based on Shashin style and conditions
            int moveCountThreshold = 2;  // Default as in Stockfish

            if (dynamicDerived.isHighTal)
            {
                // In high Tal style, be more conservative in pruning to preserve tactical moves
                moveCountThreshold += PvNode ? 1 : 0;  // Allow one more move in PV nodes
            }
            else if (isStrategical)
            {
                // In strategical styles, we can prune more aggressively
                moveCountThreshold = 1;  // Prune after the first move
            }

            // Adjust for king danger - reduce pruning when king is in danger
            if (staticState.kingDanger)
            {
                moveCountThreshold++;  // Allow one more move
            }

            // Adjust for fortresses - reduce pruning in fortresses
            if (MoveConfig::isFortress)
            {
                moveCountThreshold++;
            }

            // Futility pruning and moveCount pruning
            if (!givesCheck && move.to_sq() != prevSq && !is_loss(futilityBase)
                && move.type_of() != PROMOTION)
            {
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

                // If static exchange evaluation is low enough
                // we can prune this move.
                if (!pos.see_ge(move, alpha - futilityBase))
                {
                    bestValue = std::min(alpha, futilityBase);
                    continue;
                }
            }

            // Continuation history based pruning - adjust threshold based on style
            int historyThreshold = 5475;
            if (isAggressive)
            {
                // In aggressive styles, require higher history value to prune
                historyThreshold += 1000;  // Make pruning harder
            }
            else if (isStrategical)
            {
                // In strategical styles, prune more easily
                historyThreshold -= 1000;  // Make pruning easier
            }

            if (!capture
                && (*contHist[0])[pos.moved_piece(move)][move.to_sq()]
                       + pawnHistory[pawn_history_index(pos)][pos.moved_piece(move)][move.to_sq()]
                     <= historyThreshold)
                continue;

            // SEE pruning - adjust SEE margin based on style
            int seeMargin = -78;
            if (isAggressive)
            {
                // In aggressive styles, be more tolerant of negative SEE
                seeMargin -= 20;  // Now -98, so less pruning
            }
            else if (isStrategical)
            {
                // In strategical styles, be less tolerant
                seeMargin += 20;  // Now -58, so more pruning
            }

            if (!pos.see_ge(move, seeMargin))
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

                if (PvNode)  // Update pv even in fail-high case
                    update_pv(ss->pv, move, (ss + 1)->pv);

                if (value < beta)  // Update alpha here!
                    alpha = value;
                else
                    break;  // Fail high
            }
        }
    }
    // new variety begin
    if (std::abs(bestValue) == VALUE_INFINITE)
    {
        variety = 0;
    }

    if (variety != 0)
    {
        Value       maxIncrement = (variety == 1) ? 13 : 309;
        static PRNG rng(now());
        if (((variety == 2) && (bestValue <= 309) && (bestValue >= -309))
            || ((variety == 1) && (bestValue <= 13) && (bestValue >= -13)))
        {
            int maxValidIncrement = maxIncrement - std::abs(bestValue);
            if (maxValidIncrement < 0)
            {
                maxValidIncrement = 0;
            }
            int increment = static_cast<int>(rng.rand<uint64_t>() % (maxValidIncrement + 1));
            bestValue += increment;
        }
    }
    // end new variety
    // Step 9. Check for mate
    // All legal moves have been searched. A special case: if we are
    // in check and no legal moves were found, it is checkmate.
    if (ss->inCheck && bestValue == -VALUE_INFINITE)
    {
        assert(!MoveList<LEGAL>(pos).size());
        return mated_in(ss->ply);  // Plies to mate from the root
    }

    if (!is_decisive(bestValue) && bestValue > beta)
        bestValue = (bestValue + beta) / 2;


    Color us = pos.side_to_move();
    if (!ss->inCheck && !moveCount && !pos.non_pawn_material(us)
        && type_of(pos.captured_piece()) >= ROOK)
    {
        if (!((us == WHITE ? shift<NORTH>(pos.pieces(us, PAWN))
                           : shift<SOUTH>(pos.pieces(us, PAWN)))
              & ~pos.pieces()))  // no pawn pushes available
        {
            pos.state()->checkersBB = Rank1BB;  // search for legal king-moves only
            if (!MoveList<LEGAL>(pos).size())   // stalemate
                bestValue = VALUE_DRAW;
            pos.state()->checkersBB = 0;
        }
    }

    // Save gathered info in transposition table. The static evaluation
    // is saved as it was before adjustment by correction history.
    ttWriter.write(posKey, value_to_tt(bestValue, ss->ply), pvHit,
                   bestValue >= beta ? BOUND_LOWER : BOUND_UPPER, DEPTH_QS, bestMove,
                   unadjustedStaticEval, tt.generation());

    assert(bestValue > -VALUE_INFINITE && bestValue < VALUE_INFINITE);

    return bestValue;
}

Depth Search::Worker::reduction(
  bool i, Depth d, int mn, int delta, bool isStrategical, bool isAggressive) const {
    int reductionScale = reductions[d] * reductions[mn];
    // Adjust based on Shashin style - slightly more reduction for strategical positions
    if (isStrategical)
        reductionScale = reductionScale * 0.95;
    else if (isAggressive)
        reductionScale = reductionScale * 1.05;

    //mcts begin

    if (rootDelta != 0)
        return reductionScale - delta * 757 / rootDelta + !i * reductionScale * 218 / 512 + 1200;
    else  // avoid divide by zero error
        return reductionScale - delta * 757 + !i * reductionScale * 218 / 512 + 1200;
    //mcts end
}

// elapsed() returns the time elapsed since the search started. If the
// 'nodestime' option is enabled, it will return the count of nodes searched
// instead. This function is called to check whether the search should be
// stopped based on predefined thresholds like time limits or nodes searched.
//
// elapsed_time() returns the actual time elapsed since the start of the search.
// This function is intended for use only when printing PV outputs, and not used
// for making decisions within the search algorithm itself.
TimePoint Search::Worker::elapsed() const {
    return main_manager()->tm.elapsed([this]() { return threads.nodes_searched(); });
}

TimePoint Search::Worker::elapsed_time() const { return main_manager()->tm.elapsed_time(); }

Value Search::Worker::evaluate(const Position& pos) {
    return Eval::evaluate(networks[numaAccessToken], pos, accumulatorStack, refreshTable,
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

    // handle TB win or better
    if (is_win(v))
    {
        // Downgrade a potentially false mate score
        if (v >= VALUE_MATE_IN_MAX_PLY && VALUE_MATE - v > 100 - r50c)
            return VALUE_TB_WIN_IN_MAX_PLY - 1;

        // Downgrade a potentially false TB score.
        if (VALUE_TB - v > 100 - r50c)
            return VALUE_TB_WIN_IN_MAX_PLY - 1;

        return v - ply;
    }

    // handle TB loss or worse
    if (is_loss(v))
    {
        // Downgrade a potentially false mate score.
        if (v <= VALUE_MATED_IN_MAX_PLY && VALUE_MATE + v > 100 - r50c)
            return VALUE_TB_LOSS_IN_MAX_PLY + 1;

        // Downgrade a potentially false TB score.
        if (VALUE_TB + v > 100 - r50c)
            return VALUE_TB_LOSS_IN_MAX_PLY + 1;

        return v + ply;
    }

    return v;
}


// Adds current move and appends child pv[]
void update_pv(Move* pv, Move move, const Move* childPv) {

    for (*pv++ = move; childPv && *childPv != Move::none();)
        *pv++ = *childPv++;
    *pv = Move::none();
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
                      Move            ttMove) {

    CapturePieceToHistory& captureHistory = workerThread.captureHistory;
    Piece                  movedPiece     = pos.moved_piece(bestMove);
    PieceType              capturedPiece;

    int bonus = std::min(151 * depth - 91, 1730) + 302 * (bestMove == ttMove);
    int malus = std::min(951 * depth - 156, 2468) - 30 * quietsSearched.size();

    if (!pos.capture_stage(bestMove))
    {
        update_quiet_histories(pos, ss, workerThread, bestMove, bonus * 957 / 1024);

        // Decrease stats for all non-best quiet moves
        for (Move move : quietsSearched)
            update_quiet_histories(pos, ss, workerThread, move, -malus);
    }
    else
    {
        // Increase stats for the best move in case it was a capture move
        capturedPiece = type_of(pos.piece_on(bestMove.to_sq()));
        captureHistory[movedPiece][bestMove.to_sq()][capturedPiece] << bonus;
    }

    // Extra penalty for a quiet early move that was not a TT move in
    // previous ply when it gets refuted.
    if (prevSq != SQ_NONE && ((ss - 1)->moveCount == 1 + (ss - 1)->ttHit) && !pos.captured_piece())
        update_continuation_histories(ss - 1, pos.piece_on(prevSq), prevSq, -malus * 503 / 1024);

    // Decrease stats for all non-best capture moves
    for (Move move : capturesSearched)
    {
        movedPiece    = pos.moved_piece(move);
        capturedPiece = type_of(pos.piece_on(move.to_sq()));
        captureHistory[movedPiece][move.to_sq()][capturedPiece] << -malus * 1157 / 1024;
    }
}


// Updates histories of the move pairs formed by moves
// at ply -1, -2, -3, -4, and -6 with current move.
void update_continuation_histories(Stack* ss, Piece pc, Square to, int bonus) {
    static constexpr std::array<ConthistBonus, 6> conthist_bonuses = {
      {{1, 1157}, {2, 648}, {3, 288}, {4, 576}, {5, 140}, {6, 441}}};

    for (const auto [i, weight] : conthist_bonuses)
    {
        // Only update the first 2 continuation histories if we are in check
        if (ss->inCheck && i > 2)
            break;
        if (((ss - i)->currentMove).is_ok())
            (*(ss - i)->continuationHistory)[pc][to] << (bonus * weight / 1024) + 88 * (i < 2);
    }
}

// Updates move sorting heuristics

void update_quiet_histories(
  const Position& pos, Stack* ss, Search::Worker& workerThread, Move move, int bonus) {

    Color us = pos.side_to_move();
    workerThread.mainHistory[us][move.from_to()] << bonus;  // Untuned to prevent duplicate effort

    if (ss->ply < LOW_PLY_HISTORY_SIZE)
        workerThread.lowPlyHistory[ss->ply][move.from_to()] << (bonus * 741 / 1024) + 38;

    update_continuation_histories(ss, pos.moved_piece(move), move.to_sq(), bonus * 955 / 1024);

    int pIndex = pawn_history_index(pos);
    workerThread.pawnHistory[pIndex][pos.moved_piece(move)][move.to_sq()]
      << (bonus * (bonus > 0 ? 704 : 439) / 1024) + 70;
}

}
// no skill


// mcts begin
//  minimax_value() is a wrapper around the search() and qsearch() functions
//  used to compute the minimax evaluation of a position at the given depth,
//  from the point of view of the side to move. It does not compute PV nor
//  emit anything on the output stream. Note: you can call this function
//  with depth == DEPTH_ZERO to compute the quiescence value of the position.

Value Search::Worker::minimax_value(Position& pos, Search::Stack* ss, Depth depth) {

    //    threads.stopOnPonderhit = threads.stop = false;
    Value alpha = -VALUE_INFINITE;
    Value beta  = VALUE_INFINITE;
    Move  pv[MAX_PLY + 1];
    ss->pv = pv;

    /*   if (pos.should_debug())
      {
          debug << "Entering minimax_value() for the following position:" << std::endl;
          debug << pos << std::endl;
          hit_any_key();
      }*/
    Value value = search<PV>(pos, ss, alpha, beta, depth, false);

    // Have we found a "mate in x"?
    if (limits.mate && value >= VALUE_MATE_IN_MAX_PLY && VALUE_MATE - value <= 2 * limits.mate)
        threads.stop = true;

    /*    if (pos.should_debug())
      {
          debug << pos << std::endl;
          debug << "... exiting minimax_value() with value = " << value << std::endl;
          hit_any_key();
      }
    */
    return value;
}

// minimax_value() is a wrapper around the search() and qsearch() functions
// used to compute the minimax evaluation of a position at the given depth,
// from the point of view of the side to move. It does not compute PV nor
// emit anything on the output stream. Note: you can call this function
// with depth == DEPTH_ZERO to compute the quiescence value of the position.

Value Search::Worker::minimax_value(
  Position& pos, Search::Stack* ss, Depth depth, Value alpha, Value beta) {

    //    threads.stopOnPonderhit = threads.stop = false;
    //   alpha = -VALUE_INFINITE;
    //   beta = VALUE_INFINITE;
    Move pv[MAX_PLY + 1];
    ss->pv = pv;

    /*   if (pos.should_debug())
      {
          debug << "Entering minimax_value() for the following position:" << std::endl;
          debug << pos << std::endl;
          hit_any_key();
      }*/
    Value value = VALUE_ZERO;
    Value delta = Value(18);

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
            // ++failedHighCnt;
        }
        else
        {
            //++rootMoves[pvIdx].bestMoveCount;
            break;
        }
    }

    // Have we found a "mate in x"?
    if (limits.mate && value >= VALUE_MATE_IN_MAX_PLY && VALUE_MATE - value <= 2 * limits.mate)
        threads.stop = true;

    /*    if (pos.should_debug())
      {
          debug << pos << std::endl;
          debug << "... exiting minimax_value() with value = " << value << std::endl;
          hit_any_key();
      }
    */
    return value;
}
// mcts end


// Used to print debug info and, more importantly, to detect
// when we are out of available time and thus stop the search.
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

    if (
      // Later we rely on the fact that we can at least use the mainthread previous
      // root-search score and PV in a multithreaded environment to prove mated-in scores.
      worker.completedDepth >= 1
      && ((worker.limits.use_time_management() && (elapsed > tm.maximum() || stopOnPonderhit))
          || (worker.limits.movetime && elapsed >= worker.limits.movetime)
          || (worker.limits.nodes && worker.threads.nodes_searched() >= worker.limits.nodes)))
        worker.threads.stop = worker.threads.abortedSearch = true;
}

// Used to correct and extend PVs for moves that have a TB (but not a mate) score.
// Keeps the search based PV for as long as it is verified to maintain the game
// outcome, truncates afterwards. Finally, extends to mate the PV, providing a
// possible continuation (but not a proven mating line).
void syzygy_extend_pv(const OptionsMap&         options,
                      const Search::LimitsType& limits,
                      Position&                 pos,
                      RootMove&                 rootMove,
                      Value&                    v) {

    auto t_start      = std::chrono::steady_clock::now();
    int  moveOverhead = int(options["Move Overhead"]);
    bool rule50       = bool(options["Syzygy50MoveRule"]);

    // Do not use more than moveOverhead / 2 time, if time management is active
    auto time_abort = [&t_start, &moveOverhead, &limits]() -> bool {
        auto t_end = std::chrono::steady_clock::now();
        return limits.use_time_management()
            && 2 * std::chrono::duration<double, std::milli>(t_end - t_start).count()
                 > moveOverhead;
    };

    std::list<StateInfo> sts;

    // Step 0, do the rootMove, no correction allowed, as needed for MultiPV in TB.
    auto& stRoot = sts.emplace_back();
    pos.do_move(rootMove.pv[0], stRoot);
    int ply = 1;

    // Step 1, walk the PV to the last position in TB with correct decisive score
    while (size_t(ply) < rootMove.pv.size())
    {
        Move& pvMove = rootMove.pv[ply];

        RootMoves legalMoves;
        for (const auto& m : MoveList<LEGAL>(pos))
            legalMoves.emplace_back(m);

        Tablebases::Config config = Tablebases::rank_root_moves(options, pos, legalMoves);
        RootMove&          rm     = *std::find(legalMoves.begin(), legalMoves.end(), pvMove);

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

        // Full PV shown will thus be validated and end in TB.
        // If we cannot validate the full PV in time, we do not show it.
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
            // Give a score of each move to break DTZ ties restricting opponent mobility,
            // but not giving the opponent a capture.
            for (const auto& mOpp : MoveList<LEGAL>(pos))
                rm.tbRank -= pos.capture(mOpp) ? 100 : 1;
            pos.undo_move(m);
        }

        // Mate found
        if (legalMoves.size() == 0)
            break;

        // Sort moves according to their above assigned rank.
        // This will break ties for moves with equal DTZ in rank_root_moves.
        std::stable_sort(
          legalMoves.begin(), legalMoves.end(),
          [](const Search::RootMove& a, const Search::RootMove& b) { return a.tbRank > b.tbRank; });

        // The winning side tries to minimize DTZ, the losing side maximizes it
        Tablebases::Config config = Tablebases::rank_root_moves(options, pos, legalMoves, true);

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
    // rule if it has been been reached on the board with a non-optimal 50 move counter
    // (e.g. 8/8/6k1/3B4/3K4/4N3/8/8 w - - 54 106 ) which TB with dtz counter rounding
    // cannot always correctly rank. See also
    // https://github.com/official-stockfish/Stockfish/issues/5175#issuecomment-2058893495
    // We adjust the score to match the found PV. Note that a TB loss score can be
    // displayed if the engine did not find a drawing move yet, but eventually search
    // will figure it out (e.g. 1kq5/q2r4/5K2/8/8/8/8/7Q w - - 96 1 )
    if (pos.is_draw(0))
        v = VALUE_DRAW;

    // Undo the PV moves
    for (auto it = rootMove.pv.rbegin(); it != rootMove.pv.rend(); ++it)
        pos.undo_move(*it);

    // Inform if we couldn't get a full extension in time
    if (time_abort())
        sync_cout
          << "info string Syzygy based PV extension requires more time, increase Move Overhead as needed."
          << sync_endl;
}

void SearchManager::pv(Search::Worker&           worker,
                       const ThreadPool&         threads,
                       const TranspositionTable& tt,
                       Depth                     depth,
                       bool                      updateShashin) {  //shashin

    const auto nodes     = threads.nodes_searched();
    auto&      rootMoves = worker.rootMoves;
    auto&      pos       = worker.rootPos;
    size_t     pvIdx     = worker.pvIdx;
    size_t     multiPV   = std::min(size_t(worker.options["MultiPV"]), rootMoves.size());
    uint64_t   tbHits    = threads.tb_hits() + (worker.tbConfig.rootInTB ? rootMoves.size() : 0);

    for (size_t i = 0; i < multiPV; ++i)
    {
        bool updated = rootMoves[i].score != -VALUE_INFINITE;

        if (depth == 1 && !updated && i > 0)
            continue;

        Depth d = updated ? depth : std::max(1, depth - 1);
        Value v = updated ? rootMoves[i].uciScore : rootMoves[i].previousScore;

        if (v == -VALUE_INFINITE)
            v = VALUE_ZERO;

        bool tb = worker.tbConfig.rootInTB && std::abs(v) <= VALUE_TB;
        v       = tb ? rootMoves[i].tbScore : v;

        bool isExact = i != pvIdx || tb || !updated;  // tablebase- and previous-scores are exact

        // Potentially correct and extend the PV, and in exceptional cases v
        if (is_decisive(v) && std::abs(v) < VALUE_MATE_IN_MAX_PLY
            && ((!rootMoves[i].scoreLowerbound && !rootMoves[i].scoreUpperbound) || isExact))
            syzygy_extend_pv(worker.options, worker.limits, pos, rootMoves[i], v);
        //from shashin begin
        // Update the root Shashin state only for the main (first) PV line.
        if (updateShashin && (i == 0) && d > worker.lastShashinUpdatedDepth)
        {
            worker.getShashinManager().updateRootShashinState(v, pos, d, worker.rootDepth);
            worker.lastShashinUpdatedDepth = d;
        }
        //from shashin end
        std::string pv;
        for (Move m : rootMoves[i].pv)
            pv += UCIEngine::move(m, pos.is_chess960()) + " ";

        // Remove last whitespace
        if (!pv.empty())
            pv.pop_back();

        auto wdl   = worker.options["UCI_ShowWDL"] ? WDLModel::wdl(v, pos) : "";
        auto bound = rootMoves[i].scoreLowerbound
                     ? "lowerbound"
                     : (rootMoves[i].scoreUpperbound ? "upperbound" : "");

        InfoFull info;

        info.depth    = d;
        info.selDepth = rootMoves[i].selDepth;
        info.multiPV  = i + 1;
        info.score    = {v, pos};
        info.wdl      = wdl;

        if (!isExact)
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
// We try hard to have a ponder move to return to the GUI,
// otherwise in case of 'ponder on' we have nothing to think about.
bool RootMove::extract_ponder_from_tt(const TranspositionTable& tt, Position& pos) {

    StateInfo st;

    assert(pv.size() == 1);
    if (pv[0] == Move::none())
        return false;

    pos.do_move(pv[0], st, &tt);

    auto [ttHit, ttData, ttWriter] = tt.probe(pos.key());
    if (ttHit)
    {
        if (MoveList<LEGAL>(pos).contains(ttData.move))
            pv.push_back(ttData.move);
    }

    pos.undo_move(pv[0]);
    return pv.size() > 1;
}


// learning begin
void putQLearningTrajectoryIntoLearningTable() {
    const double learning_rate = 0.5;
    const double gamma         = 0.99;

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
    qLearningTrajectory.clear();
}
// learning end

}  // namespace ShashChess