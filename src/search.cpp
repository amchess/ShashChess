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
#include <random>  //variety
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

#include "evaluate.h"
#include "misc.h"
#include "movegen.h"
#include "movepick.h"
#include "nnue/nnue_accumulator.h"
#include "nnue/nnue_common.h"
#include "nnue/nnue_misc.h"
#include "position.h"
#include "syzygy/tbprobe.h"
#include "thread.h"
#include "timeman.h"
#include "tt.h"
#include "types.h"  //learning
#include "uci.h"
#include "ucioption.h"
#include "win_probability.h"  //livebook +shashin theory
#include "learn/learn.h"      //Khalid
#include "mcts/montecarlo.h"  //Montecarlo
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
#include "shashin/shashin_manager.h"  //shashin
namespace ShashChess {
// Kelly begin
bool                       useLearning          = true;
bool                       enabledLearningProbe = false;
std::vector<QLearningMove> qLearningTrajectory;
// Kelly end


// from Shashin
namespace Search {
ShashinConfig shashinConfig;
}
// end from Shashin

namespace TB = Tablebases;

void syzygy_extend_pv(const OptionsMap&             options,
                      const Search::LimitsType&     limits,
                      ShashChess::Position&         pos,
                      ShashChess::Search::RootMove& rootMove,
                      Value&                        v);

using namespace Search;

namespace {

// Futility margin
Value futility_margin(Depth d, bool noTtCutNode, bool improving, bool oppWorsening) {
    Value futilityMult       = 112 - 26 * noTtCutNode;
    Value improvingDeduction = improving * futilityMult * 2;
    Value worseningDeduction = oppWorsening * futilityMult / 3;

    return futilityMult * d - improvingDeduction - worseningDeduction;
}

constexpr int futility_move_count(bool improving, Depth depth) {
    return (3 + depth * depth) / (2 - improving);
}

int correction_value(const Worker& w, const Position& pos, const Stack* ss) {
    const Color us    = pos.side_to_move();
    const auto  m     = (ss - 1)->currentMove;
    const auto  pcv   = w.pawnCorrectionHistory[us][pawn_structure_index<Correction>(pos)];
    const auto  macv  = w.majorPieceCorrectionHistory[us][major_piece_index(pos)];
    const auto  micv  = w.minorPieceCorrectionHistory[us][minor_piece_index(pos)];
    const auto  wnpcv = w.nonPawnCorrectionHistory[WHITE][us][non_pawn_index<WHITE>(pos)];
    const auto  bnpcv = w.nonPawnCorrectionHistory[BLACK][us][non_pawn_index<BLACK>(pos)];
    const auto  cntcv =
      m.is_ok() ? (*(ss - 2)->continuationCorrectionHistory)[pos.piece_on(m.to_sq())][m.to_sq()]
                 : 0;

    return (6922 * pcv + 3837 * macv + 6238 * micv + 7490 * (wnpcv + bnpcv) + 6270 * cntcv);
}

// Add correctionHistory value to raw staticEval and guarantee evaluation
// does not hit the tablebase range.
Value to_corrected_static_eval(Value v, const int cv) {
    return std::clamp(v + cv / 131072, VALUE_TB_LOSS_IN_MAX_PLY + 1, VALUE_TB_WIN_IN_MAX_PLY - 1);
}

// History and stats update bonus, based on depth
int stat_bonus(Depth d) { return std::min(154 * d - 102, 1661); }

// History and stats update malus, based on depth
int stat_malus(Depth d) { return std::min(831 * d - 269, 2666); }

// Add a small random component to draw evaluations to avoid 3-fold blindness
Value value_draw(size_t nodes) { return VALUE_DRAW - 1 + Value(nodes & 0x2); }

int variety;  //variety


Value value_to_tt(Value v, int ply);
Value value_from_tt(Value v, int ply, int r50c);
void  update_pv(Move* pv, Move move, const Move* childPv);
void  update_continuation_histories(Stack* ss, Piece pc, Square to, int bonus);
void  update_quiet_histories(
   const Position& pos, Stack* ss, Search::Worker& workerThread, Move move, int bonus);
void update_all_stats(const Position&      pos,
                      Stack*               ss,
                      Search::Worker&      workerThread,
                      Move                 bestMove,
                      Square               prevSq,
                      ValueList<Move, 32>& quietsSearched,
                      ValueList<Move, 32>& capturesSearched,
                      Depth                depth);

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
        { proxy->set_action(Livebook::Action::QUERY); }
        else
        { proxy->set_action(Livebook::Action::QUERY_BEST); }
        opening_livebooks.push_back(proxy);
    }

    if (!_lichess_player.empty())
    {
        if (_lichess_player_color.empty())
        { _lichess_player_color = "white"; }

        opening_livebooks.push_back(
          new Livebook::LichessPlayer(_lichess_player, _lichess_player_color));
    }

    if (_use_lichess_games)
    { opening_livebooks.push_back(new Livebook::LichessGames()); }

    if (_use_lichess_masters)
    { opening_livebooks.push_back(new Livebook::LichessMaster()); }

    if (_use_chess_db)
    { opening_livebooks.push_back(new Livebook::ChessDb()); }
}

void Search::update_online_tablebases() {
    // Clear the existing endgame livebooks
    endgame_livebooks.clear();

    if (_use_chess_db_tablebase)  // ChessDb tablebase
    { endgame_livebooks.push_back(new Livebook::ChessDb()); }

    if (_use_lichess_tablebase)  // Lichess tablebase
    { endgame_livebooks.push_back(new Livebook::LichessEndgame()); }
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
    { variety = 0; };
    if (varietyOption == "Standard")
    { variety = 1; };
    if (varietyOption == "Psychological")
    { variety = 2; };
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
    shashinManager(std::make_unique<ShashinManager>()) {  //shashin
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

    // Non-main threads go directly to iterative_deepening()
    if (!is_mainthread())
    {
        iterative_deepening();
        return;
    }

    main_manager()->tm.init(limits, rootPos.side_to_move(), rootPos.game_ply(), options,
                            main_manager()->originalTimeAdjust);
    tt.new_search();
    // Kelly begin
    enabledLearningProbe = false;
    useLearning          = true;
    // Kelly end

    set_variety(options["Variety"]);  // variety
    // begin from Shashin
    shashinConfig.highTal         = options["High Tal"];
    shashinConfig.middleTal       = options["Middle Tal"];
    shashinConfig.lowTal          = options["Low Tal"];
    shashinConfig.capablanca      = options["Capablanca"];
    shashinConfig.highPetrosian   = options["High Petrosian"];
    shashinConfig.middlePetrosian = options["Middle Petrosian"];
    shashinConfig.lowPetrosian    = options["Low Petrosian"];
    // end from Shashin
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
                                { bestMoves.push_back(move); }
                                else
                                { break; }
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
                { livebooks = endgame_livebooks; }
                // If we are in the first max_book_depth plies, use the opening livebooks
                else if (livebook_depth_count < max_book_depth)
                { livebooks = opening_livebooks; }

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
                            { livebook_depth_count++; }
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
            { bookMove = Move::none(); }
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


    // kelly begin
    if (!bookMove)
    {
        if (bestThread->completedDepth > 4 && LD.is_enabled() && !LD.is_paused())  // from Khalid
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
            { LD.add_new_learning(plm.key, plm.learningMove); }
        }
        if (!enabledLearningProbe)
        { useLearning = false; }
    }
    // Kelly end

    // Send again PV info if we have a new best thread
    if (bestThread != this)
        main_manager()->pv(*bestThread, threads, tt, bestThread->completedDepth);

    std::string ponder;

    if (bestThread->rootMoves[0].pv.size() > 1
        || bestThread->rootMoves[0].extract_ponder_from_tt(tt, rootPos))
        ponder = UCIEngine::move(bestThread->rootMoves[0].pv[1], rootPos.is_chess960());

    auto bestmove = UCIEngine::move(bestThread->rootMoves[0].pv[0], rootPos.is_chess960());
    main_manager()->updates.onBestmove(bestmove, ponder);
    // from Khalid begin
    // Save learning data if game is already decided
    if (!bookMove)
    {
        if (is_game_decided(rootPos, (bestThread->rootMoves[0].score)) && LD.is_enabled()
            && !LD.is_paused())
        {
            // Perform Q-learning if enabled
            if (LD.learning_mode() == LearningMode::Self)
            { putQLearningTrajectoryIntoLearningTable(); }

            // Save to learning file
            if (!LD.is_readonly())
            { LD.persist(options); }
            // Stop learning until we receive *ucinewgame* command
            LD.pause();
        }
    }

    // from Khalid end

    // livebook begin
#ifdef USE_LIVEBOOK
    if (_chess_db_contribute)
    { contributor.contribute(rootPos, bestThread->rootMoves[0].pv[0]); }
#endif
    // livebook end
}

// Main iterative deepening loop. It calls search()
// repeatedly with increasing depth until the allocated thinking time has been
// consumed, the user stops the search, or the maximum search depth is reached.
void Search::Worker::iterative_deepening() {

    SearchManager* mainThread = (is_mainthread() ? main_manager() : nullptr);

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
          &this->continuationHistory[0][0][NO_PIECE][0];  // Use as a sentinel
        (ss - i)->continuationCorrectionHistory = &this->continuationCorrectionHistory[NO_PIECE][0];
        (ss - i)->staticEval                    = VALUE_NONE;
    }

    for (int i = 0; i <= MAX_PLY + 2; ++i)
        (ss + i)->ply = i;

    ss->pv = pv;

    if (mainThread)
    {
        if (mainThread->bestPreviousScore == VALUE_INFINITE)
            mainThread->iterValue.fill(VALUE_ZERO);
        else
            mainThread->iterValue.fill(mainThread->bestPreviousScore);
    }

    size_t multiPV = size_t(options["MultiPV"]);
    //no skill

    multiPV = std::min(multiPV, rootMoves.size());
    //from shashin begin
    auto& localShashinManager = this->getShashinManager();
    localShashinManager.initShashinValues(networks[numaAccessToken], refreshTable, rootPos, ss,
                                          VALUE_ZERO, shashinConfig);
    //from shashin end

    int searchAgainCounter = 0;

    lowPlyHistory.fill(97);
    // from mcts begin
    // Initialize optimism for MCTS
    optimism[WHITE] = optimism[BLACK] =
      VALUE_ZERO;  //Must initialize optimism before calling static_value(). Not sure if 'VALUE_ZERO' is the right value

    if (bool(options["MCTS by Shashin"]))
    {
        bool maybeDraw = rootPos.rule50_count() >= 90 || rootPos.upcoming_repetition(2);
        // Calculate static evaluation for root position
        Value rootPosValue = ShashinManager::static_value(
          networks[numaAccessToken], refreshTable, rootPos, ss, this->optimism[us]);  //shashin
        bool possibleMCTSByValue = localShashinManager.isMCTSApplicableByValue();
        bool mctsExplore         = options["MCTS Explore"]
                        && (localShashinManager.isMCTSExplorationApplicable()
                            || localShashinManager.isHighPieceDensityCapablancaPosition());

        // Check if MCTS can be applied
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
            delta     = 5 + std::abs(rootMoves[pvIdx].meanSquaredScore) / 12991;
            Value avg = rootMoves[pvIdx].averageScore;
            alpha     = std::max(avg - delta, -VALUE_INFINITE);
            beta      = std::min(avg + delta, VALUE_INFINITE);

            // Adjust optimism based on root move's averageScore (~4 Elo)
            optimism[us]  = 141 * avg / (std::abs(avg) + 83);
            optimism[~us] = -optimism[us];

            // Start with a small aspiration window and, in the case of a fail
            // high/low, re-search with a bigger window until we don't fail
            // high/low anymore.
            int  failedHighCnt = 0;
            bool isHighPieceDensityCapablancaPosition =
              localShashinManager.isHighPieceDensityCapablancaPosition();  //from Shashin-Crystal
            bool isStrategical = localShashinManager.isStrategical();
            while (true)
            {
                // Adjust the effective depth searched, but ensure at least one
                // effective increment for every four searchAgain steps (see issue #2717).
                //shashin-crystal begin
                // Shashin-Crystal adjustments for search depth
                Depth adjustedDepth = rootDepth;

                if (isStrategical)
                {
                    // Determine reduction factor based on Capablanca zone and move density
                    int reductionFactor = (!isHighPieceDensityCapablancaPosition
                                           || localShashinManager.getState().legalMoveCount < 25)
                                          ? 3
                                          : 2;

                    adjustedDepth = std::max(1, rootDepth - failedHighCnt
                                                  - reductionFactor * (searchAgainCounter + 1) / 4);
                }
                // Update rootDelta and perform the search
                rootDelta = beta - alpha;
                bestValue = search<Root>(rootPos, ss, alpha, beta, adjustedDepth, false);

                //shashin crystal end


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
                    main_manager()->pv(*this, threads, tt, rootDepth);

                // In case of failing low/high increase aspiration window and re-search,
                // otherwise exit the loop.
                if (bestValue <= alpha)
                {
                    beta  = (alpha + beta) / 2;
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
                main_manager()->pv(*this, threads, tt, rootDepth);

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
            int nodesEffort = rootMoves[0].effort * 100 / std::max(size_t(1), size_t(nodes));

            double fallingEval = (11 + 2 * (mainThread->bestPreviousAverageScore - bestValue)
                                  + (mainThread->iterValue[iterIdx] - bestValue))
                               / 100.0;
            fallingEval = std::clamp(fallingEval, 0.580, 1.667);

            // If the bestMove is stable over several iterations, reduce time accordingly
            timeReduction    = lastBestMoveDepth + 8 < completedDepth ? 1.495 : 0.687;
            double reduction = (1.48 + mainThread->previousTimeReduction) / (2.17 * timeReduction);
            double bestMoveInstability = 1 + 1.88 * totBestMoveChanges / threads.size();

            double totalTime =
              mainThread->tm.optimum() * fallingEval * reduction * bestMoveInstability;

            // Cap used time in case of a single legal move for a better viewer experience
            if (rootMoves.size() == 1)
                totalTime = std::min(500.0, totalTime);

            auto elapsedTime = elapsed();

            if (completedDepth >= 10 && nodesEffort >= 97 && elapsedTime > totalTime * 0.739
                && !mainThread->ponder)
                threads.stop = true;

            // Stop the search if we have exceeded the totalTime
            if (elapsedTime > totalTime)
            {
                // If we are allowed to ponder do not stop the search now but
                // keep pondering until the GUI sends "ponderhit" or "stop".
                if (mainThread->ponder)
                    mainThread->stopOnPonderhit = true;
                else
                    threads.stop = true;
            }
            else
                threads.increaseDepth = mainThread->ponder || elapsedTime <= totalTime * 0.506;
        }

        mainThread->iterValue[iterIdx] = bestValue;
        iterIdx                        = (iterIdx + 1) & 3;
    }

    if (!mainThread)
        return;

    mainThread->previousTimeReduction = timeReduction;
    //no Skill
}

// Reset histories, usually before a new game
void Search::Worker::clear() {
    mainHistory.fill(63);
    lowPlyHistory.fill(108);
    captureHistory.fill(-631);
    pawnHistory.fill(-1210);
    pawnCorrectionHistory.fill(0);
    majorPieceCorrectionHistory.fill(0);
    minorPieceCorrectionHistory.fill(0);
    nonPawnCorrectionHistory[WHITE].fill(0);
    nonPawnCorrectionHistory[BLACK].fill(0);

    for (auto& to : continuationCorrectionHistory)
        for (auto& h : to)
            h.fill(0);

    for (bool inCheck : {false, true})
        for (StatsType c : {NoCaptures, Captures})
            for (auto& to : continuationHistory[inCheck][c])
                for (auto& h : to)
                    h.fill(-479);

    for (size_t i = 1; i < reductions.size(); ++i)
        reductions[i] = int(2143 / 100.0 * std::log(i));

    refreshTable.clear(networks[numaAccessToken]);
}


// Main search function for both PV and non-PV nodes
template<NodeType nodeType>
Value Search::Worker::search(
  Position& pos, Stack* ss, Value alpha, Value beta, Depth depth, bool cutNode) {

    constexpr bool PvNode   = nodeType != NonPV;
    constexpr bool rootNode = nodeType == Root;
    const bool     allNode  = !(PvNode || cutNode);

    // Dive into quiescence search when the depth reaches zero
    if (depth <= 0)
    {
        constexpr auto nt = PvNode ? PV : NonPV;
        return qsearch<nt>(pos, ss, alpha, beta);
    }

    // Limit the depth if extensions made it too large
    depth = std::min(depth, MAX_PLY - 1);
    //from crystal-shashin begin
    // Enhanced logic for draw repetitions
    auto& localShashinManager = this->getShashinManager();
    bool  isStrategical       = localShashinManager.isStrategical();
    bool  isIntermediate      = localShashinManager.isSimpleIntermediate();

    // Allow pruning in strategic or intermediate zones only at low depth
    if (!rootNode && alpha < VALUE_DRAW && pos.upcoming_repetition(ss->ply))
    {
        if (isStrategical || (isIntermediate && ss->ply < 6))
        {
            alpha = value_draw(this->nodes);
            if (alpha >= beta)
            {
                return alpha;  // Cut-off due to draw repetition
            }
        }
    }
    //from crystal-shashin end
    assert(-VALUE_INFINITE <= alpha && alpha < beta && beta <= VALUE_INFINITE);
    assert(PvNode || (alpha == beta - 1));
    assert(0 < depth && depth < MAX_PLY);
    assert(!(PvNode && cutNode));

    Move      pv[MAX_PLY + 1];
    StateInfo st;
    ASSERT_ALIGNED(&st, Eval::NNUE::CacheLineSize);

    Key  posKey;
    Move move, excludedMove = Move::none(), bestMove,
               expTTMove = Move::none();  // from Kelly
    Depth extension, newDepth;
    Value bestValue, value = VALUE_NONE, eval = VALUE_NONE, maxValue, expTTValue = VALUE_NONE,
                     probCutBeta = VALUE_NONE;  // from Kelly
    bool givesCheck, improving = false, priorCapture, opponentWorsening = false,
                     expTTHit = false;  //shashin
    bool isMate, gameCycle;             // from Crystal-Shashin
    //from Kelly End
    bool  capture, ttCapture, kingDanger, nullParity;  // from Crystal
    Piece movedPiece;
    int   ourMove;  // from Crystal

    ValueList<Move, 32> capturesSearched;
    ValueList<Move, 32> quietsSearched;
    // from Kelly begin
    bool updatedLearning = false;
    // flags to preserve node types
    bool disableNMAndPC = false;
    bool expectedPVNode = false;
    int  sibs           = 0;
    // from Kelly end
    // Step 1. Initialize node
    Worker* thisThread = this;
    ss->inCheck        = pos.checkers();
    priorCapture       = pos.captured_piece();
    Color us           = pos.side_to_move();
    ss->moveCount      = 0;
    bestValue          = -VALUE_INFINITE;
    maxValue           = VALUE_INFINITE;
    // from Crystal-shashin begin
    gameCycle = kingDanger = false;
    rootDepth              = thisThread->rootDepth;
    ourMove                = !(ss->ply & 1);
    nullParity             = (ourMove == thisThread->nmpSide);
    ss->secondaryLine      = false;
    ss->mainLine           = false;
    // from Crystal-shashin end
    // Full Threads patch begin
    if (thisThread->fullSearch)
        improving = true;
    // Full Threads patch end
    // Check for the available remaining time
    if (is_mainthread())
        main_manager()->check_time(*thisThread);
    // Used to send selDepth info to GUI (selDepth counts from 1, ply from 0)
    if (PvNode && thisThread->selDepth < ss->ply + 1)
        thisThread->selDepth = ss->ply + 1;
    // shashin-crystal begin
    // Adjust excludedMove based on Shashin positional analysis
    bool isComplexStrategical =
      localShashinManager.isStrategical() && localShashinManager.isComplexPosition();
    bool isDefensiveLowActivity =
      localShashinManager.isTacticalDefensive() && localShashinManager.isLowActivity();
    // Allow exclusion adjustments only in specific strategic or defensive scenarios
    if (isComplexStrategical || isDefensiveLowActivity)
    { excludedMove = ss->excludedMove; }
    // shashin-crystal end
    if (!rootNode)
    {
        // from Crystal-Shashin begin
        // Check if we have an upcoming move which draws by repetition, or
        // if the opponent had an alternative move earlier to this position.
        if (pos.upcoming_repetition(ss->ply) && !excludedMove)
        { gameCycle = true; }
        // from Crystal-Shashin end
        // Step 2. Check for aborted search and immediate draw
        if (threads.stop.load(std::memory_order_relaxed) || pos.is_draw(ss->ply)
            || ss->ply >= MAX_PLY)
            return (ss->ply >= MAX_PLY && !ss->inCheck) ? evaluate(pos)
                                                        : value_draw(thisThread->nodes);

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

    bestMove            = Move::none();
    (ss + 2)->cutoffCnt = 0;
    Square prevSq = ((ss - 1)->currentMove).is_ok() ? ((ss - 1)->currentMove).to_sq() : SQ_NONE;
    ss->statScore = 0;

    // Step 4. Transposition table lookup
    excludedMove                   = ss->excludedMove;
    posKey                         = pos.key();
    auto [ttHit, ttData, ttWriter] = tt.probe(posKey);
    // Need further processing of the saved data
    ss->ttHit    = ttHit;
    ttData.move  = rootNode ? thisThread->rootMoves[thisThread->pvIdx].pv[0]
                 : ttHit    ? ttData.move
                            : Move::none();
    ttData.value = ttHit ? value_from_tt(ttData.value, ss->ply, pos.rule50_count()) : VALUE_NONE;
    ss->ttPv     = excludedMove ? ss->ttPv : PvNode || (ttHit && ttData.is_pv);
    ttCapture    = ttData.move && pos.capture_stage(ttData.move);

    // At this point, if excluded, skip straight to step 6, static eval. However,
    // to save indentation, we list the condition in all code between here and there.
    RootShashinState rootShashinState = localShashinManager.getState();  //shashin
    // At non-PV nodes we check for an early TT cutoff
    if (!PvNode
        && !excludedMove
        // shashin-crystal begin
        && (!gameCycle || (!isStrategical || rootShashinState.legalMoveCount < 30))
        && (!(ss - 1)->mainLine || localShashinManager.isTactical())
        //shashin-crystal end
        && ttData.depth > depth - (ttData.value < beta)
        && is_valid(ttData.value)  // Può accadere quando !ttHit o accesso concorrente in probe()
        && (ttData.bound & (ttData.value >= beta ? BOUND_LOWER : BOUND_UPPER))
        && (cutNode == (ttData.value >= beta) || depth > 9))
    {
        // If ttMove is quiet, update move sorting heuristics on TT hit (~2 Elo)
        if (ttData.move && ttData.value >= beta)
        {
            // Bonus for a quiet ttMove that fails high (~2 Elo)
            if (!ttCapture)
                update_quiet_histories(pos, ss, *this, ttData.move, stat_bonus(depth) * 746 / 1024);

            // Extra penalty for early quiet moves of
            // the previous ply (~1 Elo on STC, ~2 Elo on LTC)
            if (prevSq != SQ_NONE && (ss - 1)->moveCount <= 2 && !priorCapture)
                update_continuation_histories(ss - 1, pos.piece_on(prevSq), prevSq,
                                              -stat_malus(depth + 1) * 1042 / 1024);
        }

        // Partial workaround for the graph history interaction problem
        // For high rule50 counts don't produce transposition table cutoffs.
        if (pos.rule50_count() < 90)
            return ttData.value;
    }
    // from Kelly begin
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
            { ttData.move = learningMove->move; }

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
                { disableNMAndPC = true; }
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
            if (!PvNode && updatedLearning && learningMove->depth > depth - (expTTValue <= beta)
                && is_valid(expTTValue)
                && (cutNode == (expTTValue >= beta)
                    || depth > 9))  // Possible in case of Global Learning Table access race
            {
                // If expTTMove is quiet, update move sorting heuristics on Global learning table hit
                if (expTTMove && expTTValue >= beta)
                {
                    // Bonus for a quiet ttMove that fails high
                    if (!expTTCapture)
                        update_quiet_histories(pos, ss, *this, expTTMove,
                                               stat_bonus(depth) * 746 / 1024);

                    // Extra penalty for early quiet moves of
                    // the previous ply
                    if (prevSq != SQ_NONE && (ss - 1)->moveCount <= 2 && !priorCapture)
                        update_continuation_histories(ss - 1, pos.piece_on(prevSq), prevSq,
                                                      -stat_malus(depth + 1) * 1042 / 1024);
                }

                // Partial workaround for the graph history interaction problem
                // For high rule50 counts don't produce transposition table cutoffs.
                if (pos.rule50_count() < 90)
                    return expTTValue;
            }
        }
    }
    // from Kelly end

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
                thisThread->tbHits.fetch_add(1, std::memory_order_relaxed);

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
    //from shashin-crystal begin
    kingDanger              = !ourMove && pos.king_danger(us);
    bool isAggressive       = localShashinManager.isTacticalAggressive();
    bool isReactive         = localShashinManager.isTacticalReactive();
    int  baseRazoringMargin = 0;
    bool applyRazoring      = true;
    bool isHighTal          = localShashinManager.isHighTal();
    //from shashin-crystal end
    // Step 6. Static evaluation of the position
    Value      unadjustedStaticEval = VALUE_NONE;
    const auto correctionValue      = correction_value(*thisThread, pos, ss);
    if (ss->inCheck)
    {
        // Skip early pruning when in check
        ss->staticEval = eval = (ss - 2)->staticEval;
        improving             = false;
        goto moves_loop;
    }
    else if (excludedMove)
    {
        // Providing the hint that this node's accumulator will be used often
        // brings significant Elo gain (~13 Elo).
        Eval::NNUE::hint_common_parent_position(pos, networks[numaAccessToken], refreshTable);
        unadjustedStaticEval = eval = ss->staticEval;
    }
    else if (ss->ttHit)
    {
        // Never assume anything about values stored in TT
        unadjustedStaticEval = ttData.eval;
        if (!is_valid(unadjustedStaticEval))
            unadjustedStaticEval = evaluate(pos);
        else if (PvNode)
            Eval::NNUE::hint_common_parent_position(pos, networks[numaAccessToken], refreshTable);

        ss->staticEval = eval = to_corrected_static_eval(unadjustedStaticEval, correctionValue);

        // ttValue can be used as a better position evaluation (~7 Elo)
        if (is_valid(ttData.value)
            && (ttData.bound & (ttData.value > eval ? BOUND_LOWER : BOUND_UPPER)))
            eval = ttData.value;
    }
    else
    {
        //Kelly begin
        unadjustedStaticEval = evaluate(pos);
        ss->staticEval = eval = to_corrected_static_eval(unadjustedStaticEval, correctionValue);

        if (!LD.is_enabled() || !expTTHit || !updatedLearning)
        {
            ss->staticEval = eval = to_corrected_static_eval(unadjustedStaticEval, correctionValue);

            // Static evaluation is saved as it was before adjustment by correction history
            ttWriter.write(posKey, VALUE_NONE, ss->ttPv, BOUND_NONE, DEPTH_UNSEARCHED, Move::none(),
                           unadjustedStaticEval, tt.generation());
            if (!is_decisive(value))
                return value - (probCutBeta - beta);
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
            { eval = value_draw(this->nodes); }
            // Can expTTValue be used as a better position evaluation?
            if (is_valid(expTTValue))
            { eval = expTTValue; }
        }
    }
    // from Kelly end

    // Use static evaluation difference to improve quiet move ordering (~9 Elo)
    if (((ss - 1)->currentMove).is_ok() && !(ss - 1)->inCheck && !priorCapture)
    {
        int bonus = std::clamp(-10 * int((ss - 1)->staticEval + ss->staticEval), -1881, 1413) + 616;
        thisThread->mainHistory[~us][((ss - 1)->currentMove).from_to()] << bonus * 1151 / 1024;
        if (type_of(pos.piece_on(prevSq)) != PAWN && ((ss - 1)->currentMove).type_of() != PROMOTION)
            thisThread->pawnHistory[pawn_structure_index(pos)][pos.piece_on(prevSq)][prevSq]
              << bonus * 1107 / 1024;
    }
    // full threads patch begin
    if (thisThread->fullSearch)
        goto moves_loop;  // full threads patch
    // full threads patch end
    // from Kelly begin
    if (!expectedPVNode)
    {
        // Set up the improving flag, which is true if current static evaluation is
        // bigger than the previous static evaluation at our turn (if we were in
        // check at our previous move we go back until we weren't in check) and is
        // false otherwise. The improving flag is used in various pruning heuristics.
        improving = ss->staticEval > (ss - 2)->staticEval;
    }
    opponentWorsening = ss->staticEval + (ss - 1)->staticEval > 2;
    // from Kelly end
    // Step 7. Razoring (~1 Elo)
    // If eval is really low, check with qsearch if we can exceed alpha. If the
    // search suggests we cannot exceed alpha, return a speculative fail low.
    // from shashin-crystal begin
    // Apply Razoring for positions that meet the base condition, adjusted for Shashin zones
    // Avoid Razoring in highly dynamic situations

    applyRazoring = !(isAggressive && rootShashinState.legalMoveCount >= 30)
                 && (localShashinManager.isTacticalInitiative() || isReactive || isStrategical);

    baseRazoringMargin = 469 + 307 * depth * depth;
    if (isAggressive)
        baseRazoringMargin -= 50;
    else if (isStrategical || isReactive)
        baseRazoringMargin += 30;
    if (!PvNode && eval < alpha - baseRazoringMargin && applyRazoring)
        return qsearch<NonPV>(pos, ss, alpha - 1, alpha);
    // from shashin-crystal end


    // Begin early pruning from Crystal by Shashin adapted

    if ((!PvNode && !is_decisive(eval) && !is_decisive(beta) && eval >= beta)
        || (!localShashinManager.useEarlyPruning()))
    {
        // Step 8. Futility pruning: child node (~40 Elo)
        // The depth condition is important for mate finding.
        // from Crystal-Shashin begin
        bool useCrystalFutilityPruningChild = localShashinManager.useCrystalFutilityPruningChild();
        int  crystalDepthStep8 =
          9 - 2 * ((ss - 1)->mainLine || (ss - 1)->secondaryLine || (ttData.move && !ttCapture));
        bool commonCrystalFutilityPruningChildConditions =
          !ss->ttPv
          && eval - futility_margin(depth, cutNode && !ss->ttHit, improving, opponentWorsening)
                 - (ss - 1)->statScore / 310
                 + (ss->staticEval == eval) * (40 - std::abs(correctionValue) / 131072)
               >= beta;
        if (commonCrystalFutilityPruningChildConditions
            && ((useCrystalFutilityPruningChild && depth < crystalDepthStep8 && !kingDanger
                 && !excludedMove && !gameCycle && !(thisThread->nmpGuard && nullParity))
                || (!useCrystalFutilityPruningChild && eval >= beta && (!ttData.move || ttCapture)
                    && !is_loss(beta) && !is_win(eval) && depth < 14)))
        { return beta + (eval - beta) / 3; }
        //from Crystal-Shashin end
        improving |= ss->staticEval >= beta + 97;
        // Step 9. Null move search with verification search (~35 Elo)
        //from Crystal Shashin begin
        // Shashin-inspired logic to fine-tune null move pruning based on positional zones
        bool useNullMoveByShashin = localShashinManager.useCrystalNullMoveSearch();
        bool isAggressiveZone     = isAggressive || localShashinManager.isTacticalAttacking();
        bool isPassiveZone        = localShashinManager.isTacticalPassive();
        if (ss->staticEval >= beta - 20 * depth + 440 && pos.non_pawn_material(us) && !excludedMove
            && cutNode && (!disableNMAndPC))  //Kelly
        {
            if ((!useNullMoveByShashin && (ss - 1)->currentMove != Move::null() && eval >= beta
                 && ss->ply >= thisThread->nmpMinPly && !is_loss(beta))
                || (useNullMoveByShashin && !thisThread->nmpGuard && !gameCycle
                    && eval >= ss->staticEval && !kingDanger
                    && (rootDepth < 11 || ourMove || MoveList<LEGAL>(pos).size() > 5)))
            {
                assert(eval - beta >= 0);
                thisThread->nmpSide = ourMove;  // from shashin Crystal
                // Null move dynamic reduction based on depth and eval
                //begin shashin
                // Adjust reduction based on Shashin zones
                // Dynamic reduction based on depth and eval
                Depth R = std::min(int(eval - beta) / 215, 7) + depth / 3 + 5;
                if (useNullMoveByShashin)
                {
                    if (isAggressiveZone)
                    {
                        R = std::min(R + 1, 9);  // More aggressive pruning
                    }
                    else if (isPassiveZone)
                    {
                        R = std::max(R - 2, 3);  // More conservative pruning
                    }
                }
                //end shashin
                // Additional adjustments for secondary lines and Shashin logic
                if (!ourMove && (ss - 1)->secondaryLine && useNullMoveByShashin)
                    R = std::min(R, 8);
                if (depth < 11 || ttData.value >= beta || ttData.depth < depth - R
                    || !(ttData.bound & BOUND_UPPER)
                    || !useNullMoveByShashin)  //from crystal-shashin
                {
                    ss->currentMove         = Move::null();
                    ss->continuationHistory = &thisThread->continuationHistory[0][0][NO_PIECE][0];
                    ss->continuationCorrectionHistory =
                      &thisThread->continuationCorrectionHistory[NO_PIECE][0];
                    pos.do_null_move(st, tt);
                    thisThread->nmpGuard = true;  //from crystal-shashin
                    Value nullValue =
                      -search<NonPV>(pos, ss + 1, -beta, -beta + 1, depth - R, false);
                    thisThread->nmpGuard = false;  //from crystal-shashin
                    pos.undo_null_move();
                    // Do not return unproven mate or TB scores
                    if (nullValue >= beta && (!is_win(nullValue) || useNullMoveByShashin))
                    {
                        //begin from shashin-crystal
                        if (!useNullMoveByShashin)
                        {
                            if (thisThread->nmpMinPly || depth < 16)
                                return nullValue;
                            assert(
                              !thisThread->nmpMinPly);  // Recursive verification is not allowed
                            // Do verification search at high depths, with null move pruning disabled
                            // until ply exceeds nmpMinPly.
                            thisThread->nmpMinPly = ss->ply + 3 * (depth - R) / 4;
                        }
                        //end from shashin-crystal
                        thisThread->nmpGuardV = true;  // from Shashin-Crystal
                        Value v = search<NonPV>(pos, ss, beta - 1, beta, depth - R, false);
                        thisThread->nmpGuardV = false;  // from Shashin-Crystal

                        if (!useNullMoveByShashin)
                            thisThread->nmpMinPly = 0;
                        //from crystal-shashin begin
                        if (v >= beta)
                            return (!useNullMoveByShashin)
                                   ? nullValue
                                   : (is_win(v) ? v : std::min(nullValue, VALUE_MATE_IN_MAX_PLY));
                    }
                }
            }
        }
        //from Crystal-Shashin end
        // Step 10. Internal iterative reductions (~9 Elo)
        // from Crystal-Shashin begin
        bool skipStep10 = localShashinManager.avoidStep10();
        if (!skipStep10)
        {
            // PV nodes or deep enough cutNodes without a ttMove: Decrease depth by 2
            if ((PvNode || (cutNode && depth >= 7)) && !ttData.move)
            {
                depth -= (isAggressive ? 1 : 2);  // Less aggressive reduction in aggressive zones
            }

            // Use qsearch if depth <= 0
            if (depth <= 0)
            { return qsearch<PV>(pos, ss, alpha, beta); }

            // CutNodes: Additional reduction based on ttMove and bound
            if (cutNode && depth >= 7 && ttData.move && ttData.bound == BOUND_UPPER)
            {
                if (isReactive)
                {
                    depth -= 1;  // Minimal reduction in reactive zones
                }
                else
                {
                    depth -= 1;  // Standard reduction
                }
            }
        }
        // from Crystal-Shashin end


        // Step 11. ProbCut (~10 Elo)
        // If we have a good enough capture (or queen promotion) and a reduced search
        // returns a value much above beta, we can (almost) safely prune the previous move.
        probCutBeta = beta + 174 - 56 * improving;
        if (!disableNMAndPC  // Kelly
            && ((depth > 3
                 && !is_decisive(beta)
                 // If value from transposition table is lower than probCutBeta, don't attempt
                 // probCut there and in further interactions with transposition table cutoff
                 // depth is set to depth - 3 because probCut search has depth set to depth - 4
                 // but we also do a move before it. So effective depth is equal to depth - 3.
                 && !(ttData.depth >= depth - 3 && is_valid(ttData.value)
                      && ttData.value < probCutBeta))))
        {
            assert(probCutBeta < VALUE_INFINITE && probCutBeta > beta);

            MovePicker mp(pos, ttData.move, probCutBeta - ss->staticEval,
                          &thisThread->captureHistory);

            while ((move = mp.next_move()) != Move::none())
            {
                assert(move.is_ok());

                if (move == excludedMove)
                    continue;

                if (!pos.legal(move))
                    continue;

                assert(pos.capture_stage(move));

                // Prefetch the TT entry for the resulting position
                prefetch(tt.first_entry(pos.key_after(move)));

                ss->currentMove = move;
                ss->continuationHistory =
                  &this
                     ->continuationHistory[ss->inCheck][true][pos.moved_piece(move)][move.to_sq()];
                ss->continuationCorrectionHistory =
                  &this->continuationCorrectionHistory[pos.moved_piece(move)][move.to_sq()];

                thisThread->nodes.fetch_add(1, std::memory_order_relaxed);
                pos.do_move(move, st);

                // Perform a preliminary qsearch to verify that the move holds
                value = -qsearch<NonPV>(pos, ss + 1, -probCutBeta, -probCutBeta + 1);

                // If the qsearch held, perform the regular search
                if (value >= probCutBeta)
                    value = -search<NonPV>(pos, ss + 1, -probCutBeta, -probCutBeta + 1, depth - 4,
                                           !cutNode);

                pos.undo_move(move);

                if (value >= probCutBeta)
                {

                    // Save ProbCut data into transposition table
                    ttWriter.write(posKey, value_to_tt(value, ss->ply), ss->ttPv, BOUND_LOWER,
                                   depth - 3, move, unadjustedStaticEval, tt.generation());
                    if (!is_decisive(value))
                        return value - (probCutBeta - beta);
                }
            }
        }
    }        // End early Pruning from Crystal by Shashin adapted
moves_loop:  // When in check, search starts here

    // Step 12. A small Probcut idea (~4 Elo)
    probCutBeta = beta + 412;
    //from Shashin-Crystal begin
    bool probCutFromCrystal = localShashinManager.allowCrystalProbCut();
    // Check for Probcut applicability
    if ((ttData.bound & BOUND_LOWER) && ttData.depth >= depth - 4 && ttData.value >= probCutBeta
        && (
          // Crystal-inspired logic for dynamic positions
          (probCutFromCrystal && abs(beta) < VALUE_MAX_EVAL && abs(ttData.value) < VALUE_MAX_EVAL
           && ss->inCheck && !gameCycle && !kingDanger && !excludedMove && !PvNode
           && !(ss - 1)->secondaryLine && !(thisThread->nmpGuard && nullParity)
           && !(thisThread->nmpGuardV && nullParity) && ttCapture && ourMove)
          ||
          // Stockfish logic for simpler positions
          (!probCutFromCrystal && !is_decisive(beta) && is_valid(ttData.value)
           && !is_decisive(ttData.value))))
        return probCutBeta;
    const PieceToHistory* contHist[] = {(ss - 1)->continuationHistory,
                                        (ss - 2)->continuationHistory,
                                        (ss - 3)->continuationHistory,
                                        (ss - 4)->continuationHistory,
                                        nullptr,
                                        (ss - 6)->continuationHistory};


    MovePicker mp(pos, ttData.move, depth, &thisThread->mainHistory, &thisThread->lowPlyHistory,
                  &thisThread->captureHistory, contHist, &thisThread->pawnHistory, ss->ply);

    value = bestValue;

    int moveCount = 0;

    //from Crystal-Shashin begin
    bool lmrCapture = cutNode && (ss - 1)->moveCount > 1;

    bool kingDangerThem = ourMove && pos.king_danger(~us);

    bool allowLMR =
      depth > 1 && !gameCycle && (!kingDangerThem || ss->ply > 6) && (!PvNode || ss->ply > 1);

    bool doLMP = ss->ply > 2 && !PvNode;
    //from Crystal-Shashin end

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
        if (rootNode
            && !std::count(thisThread->rootMoves.begin() + thisThread->pvIdx,
                           thisThread->rootMoves.begin() + thisThread->pvLast, move))
            continue;

        ss->moveCount = ++moveCount;

        if (rootNode && is_mainthread() && nodes > 10000000)
        {
            main_manager()->updates.onIter(
              {depth, UCIEngine::move(move, pos.is_chess960()), moveCount + thisThread->pvIdx});
        }
        if (PvNode)
            (ss + 1)->pv = nullptr;

        extension  = 0;
        capture    = pos.capture_stage(move);
        movedPiece = pos.moved_piece(move);
        givesCheck = pos.gives_check(move);
        // from Crystal-Shashin begin
        bool useMateCrystalLogic = !localShashinManager.isHighTal();  //shashin-crystal
        isMate                   = false;
        // This tracks all of our possible responses to our opponent's best moves outside of the PV.
        // The reasoning here is that while we look for flaws in the PV, we must otherwise find an improvement
        // in a secondary root move in order to change the PV. Such an improvement must occur on the path of
        // our opponent's best moves or else it is meaningless.
        ss->secondaryLine =
          ((rootNode && moveCount > 1)
           || (!ourMove && (ss - 1)->secondaryLine && !excludedMove && moveCount == 1)
           || (ourMove && (ss - 1)->secondaryLine));
        ss->mainLine = ((rootNode && moveCount == 1) || (!ourMove && (ss - 1)->mainLine)
                        || (ourMove && (ss - 1)->mainLine && moveCount == 1 && !excludedMove));
        if (givesCheck)
        {
            pos.do_move(move, st, givesCheck);
            isMate = MoveList<LEGAL>(pos).size() == 0;
            pos.undo_move(move);
        }
        uint64_t nodeCount = 0;
        if (useMateCrystalLogic && isMate)
        {
            ss->currentMove = move;
            ss->continuationHistory =
              &thisThread->continuationHistory[ss->inCheck][capture][movedPiece][move.to_sq()];
            ss->continuationCorrectionHistory =
              &thisThread->continuationCorrectionHistory[movedPiece][move.to_sq()];
            value = mate_in(ss->ply + 1);

            if (PvNode && (moveCount == 1 || value > alpha))
            {
                (ss + 1)->pv    = pv;
                (ss + 1)->pv[0] = Move::none();
            }
        }
        else
        {  // end from Crystal-Shashin
            // Calculate new depth for this move
            newDepth = depth - 1;

            int delta = beta - alpha;

            Depth r = reduction(improving, depth, moveCount, delta);
            //shashin begin
            bool isPassive       = false;
            bool nonPawnMaterial = false;
            bool applyPruning    = false;
            //shahsin end
            // full threads patch begin
            if (thisThread->fullSearch)
            { goto skipExtensionAndPruning; }
            // full threads patch end

            // Step 14. Pruning at shallow depth (~120 Elo).
            // Depth conditions are important for mate finding.
            //from crystal-shashin begin
            isPassive       = localShashinManager.isTacticalPassive();
            nonPawnMaterial = pos.non_pawn_material(us);
            applyPruning    = !is_loss(bestValue)
                        && ((doLMP && isHighTal) || (!rootNode && nonPawnMaterial && (!isHighTal)));
            if (applyPruning)
            {
                // Skip quiet moves if movecount exceeds our FutilityMoveCount threshold (~8 Elo)
                if (moveCount >= futility_move_count(improving, depth))
                { mp.skip_quiet_moves(); }

                // Adjusted logic for aggressive/passive zones
                int lmrDepth = newDepth - r / 1024;

                if (capture || givesCheck)
                {
                    Piece capturedPiece = pos.piece_on(move.to_sq());
                    int   captHist =
                      thisThread->captureHistory[movedPiece][move.to_sq()][type_of(capturedPiece)];

                    // Futility pruning for captures (~2 Elo)
                    if (!givesCheck && lmrDepth < (isAggressive ? 6 : 7) && !ss->inCheck)
                    {
                        Value futilityValue = ss->staticEval + 271 + 243 * lmrDepth
                                            + PieceValue[capturedPiece] + captHist / 7;
                        if (futilityValue <= alpha)
                            continue;
                    }

                    // SEE based pruning for captures and checks (~11 Elo)
                    int seeHist      = std::clamp(captHist / 37, -152 * depth, 141 * depth);
                    int seeThreshold = -156 * depth - seeHist;

                    // Adjust SEE threshold based on zones
                    if (isAggressive)
                        seeThreshold += 15 * depth;
                    else if (isPassive)
                        seeThreshold -= 10 * depth;

                    if (!pos.see_ge(move, seeThreshold))
                        continue;
                }
                else
                {
                    int history =
                      (*contHist[0])[movedPiece][move.to_sq()]
                      + (*contHist[1])[movedPiece][move.to_sq()]
                      + thisThread
                          ->pawnHistory[pawn_structure_index(pos)][movedPiece][move.to_sq()];

                    // Continuation history-based pruning
                    if (history < -3901 * depth)
                        continue;

                    history += 2 * thisThread->mainHistory[us][move.from_to()];
                    lmrDepth += history / 3459;

                    Value futilityValue = ss->staticEval
                                        + (bestValue < ss->staticEval - 47 ? 137 : 47)
                                        + 142 * lmrDepth;

                    // Futility pruning: parent node (~13 Elo)
                    bool useCrystalFutilityPruningParent =
                      localShashinManager.useCrystalFutilityPruningParent();
                    if (!ss->inCheck && futilityValue <= alpha)
                    {
                        if (useCrystalFutilityPruningParent)
                        {
                            // Crystal-inspired pruning logic
                            int adjustedDepthLimit = 5 * (2 - (ourMove && (ss - 1)->secondaryLine));
                            int adjustedHistoryLimit = 20500 - 3875 * (depth - 1);

                            if (lmrDepth < adjustedDepthLimit && history < adjustedHistoryLimit)
                                continue;
                        }
                        else
                        {
                            // Stockfish-inspired pruning logic
                            if (lmrDepth < 12)
                            {
                                if (bestValue <= futilityValue && !is_decisive(bestValue)
                                    && !is_win(futilityValue))
                                    bestValue = futilityValue;
                                continue;
                            }
                        }
                    }

                    lmrDepth = std::max(lmrDepth, 0);

                    // SEE-based pruning for quiet moves
                    if (!pos.see_ge(move, -25 * lmrDepth * lmrDepth))
                        continue;
                }
                //from Crystal-shashin end
            }

            // Step 15. Extensions (~100 Elo)
            //Shashin-Crystal begin
            if (ss->ply < thisThread->rootDepth * 2)
            {
                // Singular extension search (~76 Elo, ~170 nElo). If all moves but one
                // fail low on a search of (alpha-s, beta-s), and just one fails high on
                // (alpha, beta), then that move is singular and should be extended. To
                // verify this we do a reduced search on the position excluding the ttMove
                // and if the result is lower than ttValue minus a margin, then we will
                //  extend the ttMove. Recursive singular search is avoided.

                // Note: the depth margin and singularBeta margin are known for having
                // non-linear scaling. Their values are optimized to time controls of
                // 180+1.8 and longer so changing them requires tests at these types of
                // time controls. Generally, higher singularBeta (i.e closer to ttValue)
                // and lower extension margins scale well.

                // Added Crystal logic for extensions in play cycles
                bool gameCycleExtension = gameCycle && extension >= 0 && (ourMove && PvNode);

                if (gameCycleExtension && isHighTal)
                {
                    extension = 2;  // Extension per play cycles in high tal zones
                }
                else
                {
                    bool commonExtensionCondition =
                      !rootNode && !excludedMove && is_valid(ttData.value)
                      && (ttData.bound & BOUND_LOWER) && move == ttData.move
                      && !is_decisive(ttData.value) && ttData.depth >= depth - 3
                      && depth >= 5 - (thisThread->completedDepth > 33) + ss->ttPv;
                    if (commonExtensionCondition)
                    {
                        Value singularBeta =
                          ttData.value - (52 + 74 * (ss->ttPv && !PvNode)) * depth / 64;
                        Depth singularDepth = newDepth / 2;

                        ss->excludedMove = move;
                        value            = search<NonPV>(pos, ss, singularBeta - 1, singularBeta,
                                              singularDepth, cutNode);
                        ss->excludedMove = Move::none();

                        if (value < singularBeta)
                        {
                            int corrValAdj   = std::abs(correctionValue) / 262144;
                            int doubleMargin = 249 * PvNode - 194 * !ttCapture - corrValAdj;
                            int tripleMargin =
                              94 + 287 * PvNode - 249 * !ttCapture + 99 * ss->ttPv - corrValAdj;
                            int quadMargin =
                              394 + 287 * PvNode - 249 * !ttCapture + 99 * ss->ttPv - corrValAdj;

                            extension = 1 + (value < singularBeta - doubleMargin)
                                      + (value < singularBeta - tripleMargin)
                                      + (value < singularBeta - quadMargin);


                            depth += ((!PvNode) && (depth < 15));
                        }
                        // Multi-cut pruning
                        // Our ttMove is assumed to fail high based on the bound of the TT entry,
                        // and if after excluding the ttMove with a reduced search we fail high
                        // over the original beta, we assume this expected cut-node is not
                        // singular (multiple moves fail high), and we can prune the whole
                        // subtree by returning a softbound.
                        else if (value >= beta && !is_decisive(value))
                        { return value; }
                        // Negative extensions
                        // If other moves failed high over (ttValue - margin) without the
                        // ttMove on a reduced search, but we cannot do multi-cut because
                        // (ttValue - margin) is lower than the original beta, we do not know
                        // if the ttMove is singular or can do a multi-cut, so we reduce the
                        // ttMove in favor of other moves based on some conditions:

                        // If the ttMove is assumed to fail high over current beta (~7 Elo)
                        else if (ttData.value >= beta)
                        { extension = -3; }
                        // If we are on a cutNode but the ttMove is not assumed to fail high
                        // over current beta (~1 Elo)
                        else if (cutNode)
                        { extension = -2; }
                    }
                    // Extension for capturing the previous moved piece (~1 Elo at LTC)
                    else if (PvNode && move.to_sq() == prevSq
                             && thisThread->captureHistory[movedPiece][move.to_sq()]
                                                          [type_of(pos.piece_on(move.to_sq()))]
                                  > 4126)
                    { extension = 1; }
                }
            }
            //Shashin-Crystal end

            // Add extension to new depth
            newDepth += extension;

skipExtensionAndPruning:  // full threads search patch

            // Speculative prefetch as early as possible
            prefetch(tt.first_entry(pos.key_after(move)));

            // Update the current move (this must be done after singular extension search)
            ss->currentMove = move;
            ss->continuationHistory =
              &thisThread->continuationHistory[ss->inCheck][capture][movedPiece][move.to_sq()];
            ss->continuationCorrectionHistory =
              &thisThread->continuationCorrectionHistory[movedPiece][move.to_sq()];
            nodeCount = rootNode ? uint64_t(nodes) : 0;
            // Step 16. Make the move
            thisThread->nodes.fetch_add(1, std::memory_order_relaxed);
            pos.do_move(move, st, givesCheck);
            bool doLMRStep = !(thisThread->fullSearch);  // full threads patch
            // These reduction adjustments have proven non-linear scaling.
            // They are optimized to time controls of 180 + 1.8 and longer,
            // so changing them or adding conditions that are similar requires
            // tests at these types of time controls.

            // Decrease reduction if position is or has been on the PV (~7 Elo)
            if (ss->ttPv)
                r -= 1037 + (ttData.value > alpha) * 965 + (ttData.depth >= depth) * 960;

            // Decrease reduction for PvNodes (~0 Elo on STC, ~2 Elo on LTC)
            if (PvNode)
                r -= 1018;

            // These reduction adjustments have no proven non-linear scaling

            r += 307;

            r -= std::abs(correctionValue) / 34112;

            // Increase reduction for cut nodes (~4 Elo)
            if (cutNode)
                r += 2355 - (ttData.depth >= depth && ss->ttPv) * 1141;

            // Increase reduction if ttMove is a capture but the current move is not a capture (~3 Elo)
            if (ttCapture && !capture)
                r += 1087 + (depth < 8) * 990;

            // Increase reduction if next ply has a lot of fail high (~5 Elo)
            if ((ss + 1)->cutoffCnt > 3)
                r += 940 + allNode * 887;

            // For first picked move (ttMove) reduce reduction (~3 Elo)
            else if (move == ttData.move)
                r -= 1960;

            if (capture)
                ss->statScore =
                  7 * int(PieceValue[pos.captured_piece()])
                  + thisThread
                      ->captureHistory[movedPiece][move.to_sq()][type_of(pos.captured_piece())]
                  - 4666;
            else
                ss->statScore = 2 * thisThread->mainHistory[us][move.from_to()]
                              + (*contHist[0])[movedPiece][move.to_sq()]
                              + (*contHist[1])[movedPiece][move.to_sq()] - 3874;

            // Decrease/increase reduction for moves with a good/bad history (~8 Elo)
            r -= ss->statScore * 1451 / 16384;

            // Step 17. Late moves reduction / extension (LMR, ~117 Elo)
            //from crystal-shashin begin
            bool useStep17CrystalLogic = localShashinManager.useMoveGenAndStep17CrystalLogic();
            if (doLMRStep && moveCount > sibs && moveCount > 1
                && ((depth >= 2 && !useStep17CrystalLogic)
                    || ((allowLMR && (!capture || lmrCapture)) && useStep17CrystalLogic)))
            {  ////from crystal-shashin end

                // In general we want to cap the LMR depth search at newDepth, but when
                // reduction is negative, we allow this move a limited search extension
                // beyond the first move depth.
                // To prevent problems when the max value is less than the min value,
                // std::clamp has been replaced by a more robust implementation.
                Depth d = std::max(
                  1, std::min(newDepth - r / 1024, newDepth + !allNode + (PvNode && !bestMove)));

                value = -search<NonPV>(pos, ss + 1, -(alpha + 1), -alpha, d, true);

                // Do a full-depth search when reduced LMR search fails high
                if (value > alpha && d < newDepth)
                {
                    // Adjust full-depth search based on LMR results - if the result was
                    // good enough search deeper, if it was bad enough search shallower.
                    //Crystal-Shashin begin
                    // Do a full-depth search when reduced LMR search fails high
                    // Use Shashin to determine dynamic extensions/reductions
                    const bool doDeeperSearch =
                      localShashinManager.isTal() && value > (bestValue + 40 + 2 * newDepth);
                    const bool doShallowerSearch =
                      localShashinManager.isPetrosian() && value < bestValue + 10;

                    //Crystal-Shashin end
                    newDepth += doDeeperSearch - doShallowerSearch;

                    if (newDepth > d)
                    {
                        value =
                          -search<NonPV>(pos, ss + 1, -(alpha + 1), -alpha, newDepth, !cutNode);
                    }

                    // Post LMR continuation history updates (~1 Elo)
                    int bonus = (value >= beta) * 2048;
                    update_continuation_histories(ss, movedPiece, move.to_sq(), bonus);
                }
            }

            // Step 18. Full-depth search when LMR is skipped
            else if (!PvNode || moveCount > 1)
            {
                // Increase reduction if ttMove is not present (~6 Elo)
                if (!ttData.move)
                    r += 2111;

                // Note that if expected reduction is high, we reduce search depth by 1 here (~9 Elo)
                value = -search<NonPV>(pos, ss + 1, -(alpha + 1), -alpha, newDepth - (r > 3444),
                                       !cutNode);
            }

            // For PV nodes only, do a full PV search on the first move or after a fail high,
            // otherwise let the parent node fail low with value <= alpha and try another move.
            if (PvNode && (moveCount == 1 || value > alpha))
            {
                (ss + 1)->pv    = pv;
                (ss + 1)->pv[0] = Move::none();

                // Extend move from transposition table if we are about to dive into qsearch.
                if (move == ttData.move && ss->ply <= thisThread->rootDepth * 2)
                    newDepth = std::max(newDepth, 1);

                value = -search<PV>(pos, ss + 1, -beta, -alpha, newDepth, false);
            }

            // Step 19. Undo move
            pos.undo_move(move);
        }  //crystal shashin
        assert(value > -VALUE_INFINITE && value < VALUE_INFINITE);

        // Step 20. Check for a new best move
        // Finished searching the move. If a stop occurred, the return value of
        // the search cannot be trusted, and we return immediately without updating
        // best move, principal variation nor transposition table.
        if (threads.stop.load(std::memory_order_relaxed))
            return VALUE_ZERO;

        if (rootNode)
        {
            RootMove& rm =
              *std::find(thisThread->rootMoves.begin(), thisThread->rootMoves.end(), move);

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
                rm.selDepth            = thisThread->selDepth;
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
                if (moveCount > 1 && !thisThread->pvIdx)
                    ++thisThread->bestMoveChanges;
            }
            else
                // All other moves but the PV, are set to the lowest value: this
                // is not a problem when sorting because the sort is stable and the
                // move position in the list is preserved - just the PV is pushed up.
                rm.score = -VALUE_INFINITE;
        }

        // In case we have an alternative move equal in eval to the current bestmove,
        // promote it to bestmove by pretending it just exceeds alpha (but not beta).
        int inc = (value == bestValue && ss->ply + 2 >= thisThread->rootDepth
                   && (int(nodes) & 15) == 0 && !is_win(std::abs(value) + 1));

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
                    ss->cutoffCnt += !ttData.move + (extension < 2);
                    assert(value >= beta);  // Fail high
                    break;
                }
                else
                {
                    //from crystal-shashin begin
                    // Step 20. Check for a new best move
                    bool useCrystalReduction = !localShashinManager.isTillMiddleHigh();
                    bool isSafeZone = beta < VALUE_MAX_EVAL / 16 && alpha > -VALUE_MAX_EVAL / 16;

                    // Reduce other moves if we have found at least one score improvement (~2 Elo)
                    if (depth > 2 && depth < 14 && !is_decisive(value))
                    {
                        if (useCrystalReduction && !gameCycle && isSafeZone)
                        {
                            depth -= 1;  // Crystal logic for tactical zones (minimal reduction)
                        }
                        else if (isStrategical && !gameCycle)
                        {
                            depth -= 2;  // Stockfish logic for strategic zones (deeper reduction)
                        }
                        else
                        {
                            depth -= 2;  // Default reduction for all other cases
                        }
                    }
                    //from crystal-shashin end

                    assert(depth > 0);
                    alpha = value;  // Update alpha! Always alpha < beta
                }
            }
        }

        // If the move is worse than some previously searched move,
        // remember it, to update its stats later.
        if (move != bestMove && moveCount <= 32)
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

    // Adjust best value for fail high cases at non-pv nodes
    if (!PvNode && bestValue >= beta && !is_decisive(bestValue) && !is_decisive(beta)
        && !is_decisive(alpha))
        bestValue = (bestValue * depth + beta) / (depth + 1);

    if (!moveCount)
        bestValue = excludedMove ? alpha : ss->inCheck ? mated_in(ss->ply) : VALUE_DRAW;

    // If there is a move that produces search value greater than alpha,
    // we update the stats of searched moves.
    else if (bestMove)
        update_all_stats(pos, ss, *this, bestMove, prevSq, quietsSearched, capturesSearched, depth);

    // Bonus for prior countermove that caused the fail low
    else if (!priorCapture && prevSq != SQ_NONE)
    {
        int bonusScale = (118 * (depth > 5) + 37 * !allNode + 169 * ((ss - 1)->moveCount > 8)
                          + 128 * (!ss->inCheck && bestValue <= ss->staticEval - 102)
                          + 115 * (!(ss - 1)->inCheck && bestValue <= -(ss - 1)->staticEval - 82));

        // Proportional to "how much damage we have to undo"
        bonusScale += std::min(-(ss - 1)->statScore / 106, 318);

        bonusScale = std::max(bonusScale, 0);

        const int scaledBonus = stat_bonus(depth) * bonusScale / 32;

        update_continuation_histories(ss - 1, pos.piece_on(prevSq), prevSq,
                                      scaledBonus * 436 / 1024);

        thisThread->mainHistory[~us][((ss - 1)->currentMove).from_to()] << scaledBonus * 207 / 1024;

        if (type_of(pos.piece_on(prevSq)) != PAWN && ((ss - 1)->currentMove).type_of() != PROMOTION)
            thisThread->pawnHistory[pawn_structure_index(pos)][pos.piece_on(prevSq)][prevSq]
              << scaledBonus * 1195 / 1024;
    }

    else if (priorCapture && prevSq != SQ_NONE)
    {
        // bonus for prior countermoves that caused the fail low
        Piece capturedPiece = pos.captured_piece();
        assert(capturedPiece != NO_PIECE);
        thisThread->captureHistory[pos.piece_on(prevSq)][prevSq][type_of(capturedPiece)]
          << stat_bonus(depth) * 2;
    }

    if (PvNode)
        bestValue = std::min(bestValue, maxValue);

    // If no good move is found and the previous position was ttPv, then the previous
    // opponent move is probably good and the new position is added to the search tree. (~7 Elo)
    if (bestValue <= alpha)
        ss->ttPv = ss->ttPv || ((ss - 1)->ttPv && depth > 3);

    // Write gathered information in transposition table. Note that the
    // static evaluation is saved as it was before correction history.
    if (!excludedMove && !(rootNode && thisThread->pvIdx))
        ttWriter.write(posKey, value_to_tt(bestValue, ss->ply), ss->ttPv,
                       bestValue >= beta    ? BOUND_LOWER
                       : PvNode && bestMove ? BOUND_EXACT
                                            : BOUND_UPPER,
                       depth, bestMove, unadjustedStaticEval, tt.generation());

    // Adjust correction history
    if (!ss->inCheck && !(bestMove && pos.capture(bestMove))
        && ((bestValue < ss->staticEval && bestValue < beta)  // negative correction & no fail high
            || (bestValue > ss->staticEval && bestMove)))     // positive correction & no fail low
    {
        const auto    m             = (ss - 1)->currentMove;
        constexpr int nonPawnWeight = 165;

        auto bonus = std::clamp(int(bestValue - ss->staticEval) * depth / 8,
                                -CORRECTION_HISTORY_LIMIT / 4, CORRECTION_HISTORY_LIMIT / 4);
        thisThread->pawnCorrectionHistory[us][pawn_structure_index<Correction>(pos)]
          << bonus * 114 / 128;
        thisThread->majorPieceCorrectionHistory[us][major_piece_index(pos)] << bonus * 163 / 128;
        thisThread->minorPieceCorrectionHistory[us][minor_piece_index(pos)] << bonus * 146 / 128;
        thisThread->nonPawnCorrectionHistory[WHITE][us][non_pawn_index<WHITE>(pos)]
          << bonus * nonPawnWeight / 128;
        thisThread->nonPawnCorrectionHistory[BLACK][us][non_pawn_index<BLACK>(pos)]
          << bonus * nonPawnWeight / 128;

        if (m.is_ok())
            (*(ss - 2)->continuationCorrectionHistory)[pos.piece_on(m.to_sq())][m.to_sq()] << bonus;
    }

    assert(bestValue > -VALUE_INFINITE && bestValue < VALUE_INFINITE);

    return bestValue;
}


// Quiescence search function, which is called by the main search function with
// depth zero, or recursively with further decreasing depth. With depth <= 0, we
// "should" be using static eval only, but tactical moves may confuse the static eval.
// To fight this horizon effect, we implement this qsearch of tactical moves (~155 Elo).
// See https://www.chessprogramming.org/Horizon_Effect
// and https://www.chessprogramming.org/Quiescence_Search
template<NodeType nodeType>
Value Search::Worker::qsearch(Position& pos, Stack* ss, Value alpha, Value beta) {

    static_assert(nodeType != Root);
    constexpr bool PvNode = nodeType == PV;

    assert(alpha >= -VALUE_INFINITE && alpha < beta && beta <= VALUE_INFINITE);
    assert(PvNode || (alpha == beta - 1));

    // Check if we have an upcoming move that draws by repetition (~1 Elo)
    if (alpha < VALUE_DRAW && pos.upcoming_repetition(ss->ply))
    {
        alpha = value_draw(this->nodes);
        if (alpha >= beta)
            return alpha;
    }

    Move      pv[MAX_PLY + 1];
    StateInfo st;
    ASSERT_ALIGNED(&st, Eval::NNUE::CacheLineSize);

    Key   posKey;
    Move  move, bestMove;
    Value bestValue, value, futilityBase;
    bool  pvHit, givesCheck, capture, gameCycle = false;  // from Crystal
    int   moveCount;
    Color us = pos.side_to_move();

    // Step 1. Initialize node
    if (PvNode)
    {
        (ss + 1)->pv = pv;
        ss->pv[0]    = Move::none();
    }

    Worker* thisThread = this;
    bestMove           = Move::none();
    ss->inCheck        = pos.checkers();
    moveCount          = 0;

    // Used to send selDepth info to GUI (selDepth counts from 1, ply from 0)
    if (PvNode && thisThread->selDepth < ss->ply + 1)
        thisThread->selDepth = ss->ply + 1;

    // from Crystal begin
    if (pos.upcoming_repetition(ss->ply))
    { gameCycle = true; }
    // from Crystal end

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
    auto& localShashinManager = this->getShashinManager();
    bool  isStrategical       = localShashinManager.isStrategical();
    // At non-PV nodes we check for an early TT cutoff
    //from Shashin-Crystal begin
    if (!PvNode && is_valid(ttData.value)  // Can happen when !ttHit or when access race in probe()
        && (ttData.bound & (ttData.value >= beta ? BOUND_LOWER : BOUND_UPPER))
        && (ttData.depth >= DEPTH_QS) && (!gameCycle || !isStrategical))
        return ttData.value;
    //from Shashin-Crystal end

    // Step 4. Static evaluation of the position
    Value      unadjustedStaticEval = VALUE_NONE;
    const auto correctionValue      = correction_value(*thisThread, pos, ss);
    if (ss->inCheck)
        bestValue = futilityBase = -VALUE_INFINITE;
    else
    {
        if (ss->ttHit)
        {
            // Never assume anything about values stored in TT
            unadjustedStaticEval = ttData.eval;
            if (!is_valid(unadjustedStaticEval))
                unadjustedStaticEval = evaluate(pos);
            ss->staticEval = bestValue =
              to_corrected_static_eval(unadjustedStaticEval, correctionValue);

            // ttValue can be used as a better position evaluation (~13 Elo)
            if (is_valid(ttData.value) && !is_decisive(ttData.value)
                && (ttData.bound & (ttData.value > bestValue ? BOUND_LOWER : BOUND_UPPER)))
                bestValue = ttData.value;
        }
        else
        {
            // In case of null move search, use previous static eval with opposite sign
            unadjustedStaticEval =
              (ss - 1)->currentMove != Move::null() ? evaluate(pos) : -(ss - 1)->staticEval;
            ss->staticEval = bestValue =
              to_corrected_static_eval(unadjustedStaticEval, correctionValue);
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

        futilityBase = ss->staticEval + 301;
    }

    const PieceToHistory* contHist[] = {(ss - 1)->continuationHistory,
                                        (ss - 2)->continuationHistory};

    Square prevSq = ((ss - 1)->currentMove).is_ok() ? ((ss - 1)->currentMove).to_sq() : SQ_NONE;

    // Initialize a MovePicker object for the current position, and prepare to search
    // the moves. We presently use two stages of move generator in quiescence search:
    // captures, or evasions only when in check.
    MovePicker mp(pos, ttData.move, DEPTH_QS, &thisThread->mainHistory, &thisThread->lowPlyHistory,
                  &thisThread->captureHistory, contHist, &thisThread->pawnHistory, ss->ply);

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

        // Step 6. Pruning
        if (!is_loss(bestValue) && pos.non_pawn_material(us))
        {
            // Futility pruning and moveCount pruning (~10 Elo)
            if (!givesCheck && move.to_sq() != prevSq && !is_loss(futilityBase)
                && move.type_of() != PROMOTION)
            {
                if (moveCount > 2
                                  + ((!(localShashinManager.isHighTal()))
                                       ? 0
                                       : PvNode))  //from Crystal-shashin
                    continue;

                Value futilityValue = futilityBase + PieceValue[pos.piece_on(move.to_sq())];

                // If static eval + value of piece we are going to capture is
                // much lower than alpha, we can prune this move. (~2 Elo)
                if (futilityValue <= alpha)
                {
                    bestValue = std::max(bestValue, futilityValue);
                    continue;
                }

                // If static exchange evaluation is low enough
                // we can prune this move. (~2 Elo)
                if (!pos.see_ge(move, alpha - futilityBase))
                {
                    bestValue = std::min(alpha, futilityBase);
                    continue;
                }
            }

            // Continuation history based pruning (~3 Elo)
            if (!capture
                && (*contHist[0])[pos.moved_piece(move)][move.to_sq()]
                       + (*contHist[1])[pos.moved_piece(move)][move.to_sq()]
                       + thisThread->pawnHistory[pawn_structure_index(pos)][pos.moved_piece(move)]
                                                [move.to_sq()]
                     <= 5228)
                continue;

            // Do not search moves with bad enough SEE values (~5 Elo)
            if (!pos.see_ge(move, -80))
                continue;
        }

        // Speculative prefetch as early as possible
        prefetch(tt.first_entry(pos.key_after(move)));

        // Update the current move
        ss->currentMove = move;
        ss->continuationHistory =
          &thisThread
             ->continuationHistory[ss->inCheck][capture][pos.moved_piece(move)][move.to_sq()];
        ss->continuationCorrectionHistory =
          &thisThread->continuationCorrectionHistory[pos.moved_piece(move)][move.to_sq()];

        // Step 7. Make and search the move
        thisThread->nodes.fetch_add(1, std::memory_order_relaxed);
        pos.do_move(move, st, givesCheck);
        value = -qsearch<nodeType>(pos, ss + 1, -beta, -alpha);
        pos.undo_move(move);

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
    { variety = 0; }

    if (variety != 0)
    {
        Value       maxIncrement = (variety == 1) ? 13 : 309;
        static PRNG rng(now());
        if (((variety == 2) && (bestValue <= 309) && (bestValue >= -309))
            || ((variety == 1) && (bestValue <= 13) && (bestValue >= -13)))
        {
            int maxValidIncrement = maxIncrement - std::abs(bestValue);
            if (maxValidIncrement < 0)
            { maxValidIncrement = 0; }
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

    if (!is_decisive(bestValue) && bestValue >= beta)
        bestValue = (3 * bestValue + beta) / 4;

    // Save gathered info in transposition table. The static evaluation
    // is saved as it was before adjustment by correction history.
    ttWriter.write(posKey, value_to_tt(bestValue, ss->ply), pvHit,
                   bestValue >= beta ? BOUND_LOWER : BOUND_UPPER, DEPTH_QS, bestMove,
                   unadjustedStaticEval, tt.generation());

    assert(bestValue > -VALUE_INFINITE && bestValue < VALUE_INFINITE);

    return bestValue;
}

Depth Search::Worker::reduction(bool i, Depth d, int mn, int delta) const {
    int reductionScale = reductions[d] * reductions[mn];
    //mcts begin
    if (rootDelta != 0)
        return reductionScale - delta * 768 / rootDelta + !i * reductionScale * 108 / 300 + 1168;
    else  // avoid divide by zero error
        return reductionScale - delta * 768 + !i * reductionScale * 108 / 300 + 1168;
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
    return Eval::evaluate(networks[numaAccessToken], pos, refreshTable,
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
void update_all_stats(const Position&      pos,
                      Stack*               ss,
                      Search::Worker&      workerThread,
                      Move                 bestMove,
                      Square               prevSq,
                      ValueList<Move, 32>& quietsSearched,
                      ValueList<Move, 32>& capturesSearched,
                      Depth                depth) {

    CapturePieceToHistory& captureHistory = workerThread.captureHistory;
    Piece                  moved_piece    = pos.moved_piece(bestMove);
    PieceType              captured;

    int bonus = stat_bonus(depth);
    int malus = stat_malus(depth);

    if (!pos.capture_stage(bestMove))
    {
        update_quiet_histories(pos, ss, workerThread, bestMove, bonus * 1216 / 1024);

        // Decrease stats for all non-best quiet moves
        for (Move move : quietsSearched)
            update_quiet_histories(pos, ss, workerThread, move, -malus * 1062 / 1024);
    }
    else
    {
        // Increase stats for the best move in case it was a capture move
        captured = type_of(pos.piece_on(bestMove.to_sq()));
        captureHistory[moved_piece][bestMove.to_sq()][captured] << bonus * 1272 / 1024;
    }

    // Extra penalty for a quiet early move that was not a TT move in
    // previous ply when it gets refuted.
    if (prevSq != SQ_NONE && ((ss - 1)->moveCount == 1 + (ss - 1)->ttHit) && !pos.captured_piece())
        update_continuation_histories(ss - 1, pos.piece_on(prevSq), prevSq, -malus * 966 / 1024);

    // Decrease stats for all non-best capture moves
    for (Move move : capturesSearched)
    {
        moved_piece = pos.moved_piece(move);
        captured    = type_of(pos.piece_on(move.to_sq()));
        captureHistory[moved_piece][move.to_sq()][captured] << -malus * 1205 / 1024;
    }
}


// Updates histories of the move pairs formed by moves
// at ply -1, -2, -3, -4, and -6 with current move.
void update_continuation_histories(Stack* ss, Piece pc, Square to, int bonus) {
    static constexpr std::array<ConthistBonus, 5> conthist_bonuses = {
      {{1, 1025}, {2, 621}, {3, 325}, {4, 512}, {6, 534}}};

    for (const auto [i, weight] : conthist_bonuses)
    {
        // Only update the first 2 continuation histories if we are in check
        if (ss->inCheck && i > 2)
            break;
        if (((ss - i)->currentMove).is_ok())
            (*(ss - i)->continuationHistory)[pc][to] << bonus * weight / 1024;
    }
}

// Updates move sorting heuristics

void update_quiet_histories(
  const Position& pos, Stack* ss, Search::Worker& workerThread, Move move, int bonus) {

    Color us = pos.side_to_move();
    workerThread.mainHistory[us][move.from_to()] << bonus;  // Untuned to prevent duplicate effort

    if (ss->ply < LOW_PLY_HISTORY_SIZE)
        workerThread.lowPlyHistory[ss->ply][move.from_to()] << bonus * 879 / 1024;

    update_continuation_histories(ss, pos.moved_piece(move), move.to_sq(), bonus * 888 / 1024);

    int pIndex = pawn_structure_index(pos);
    workerThread.pawnHistory[pIndex][pos.moved_piece(move)][move.to_sq()] << bonus * 634 / 1024;
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
        if (config.rootInTB && pos.is_draw(ply))
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
    while (!pos.is_draw(0))
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

    // Finding a draw in this function is an exceptional case, that cannot happen
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
                       Depth                     depth) {

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

        worker.getShashinManager().updateShashinValues((Value) v, pos, d);  //by shashin
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

        TimePoint time = tm.elapsed_time() + 1;
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
    ASSERT_ALIGNED(&st, Eval::NNUE::CacheLineSize);

    assert(pv.size() == 1);
    if (pv[0] == Move::none())
        return false;

    pos.do_move(pv[0], st);

    auto [ttHit, ttData, ttWriter] = tt.probe(pos.key());
    if (ttHit)
    {
        if (MoveList<LEGAL>(pos).contains(ttData.move))
            pv.push_back(ttData.move);
    }

    pos.undo_move(pv[0]);
    return pv.size() > 1;
}


// Kelly begin
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
}
// Kelly end

}  // namespace ShashChess