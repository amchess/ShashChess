/*
  ShashChess, a UCI chess playing engine derived from Stockfish
  Copyright (C) 2004-2024 Andrea Manzo, K.Kiniama and ShashChess developers (see AUTHORS file)

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
#include <random>  //for opening variety
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <utility>
#include <sstream>

#include "evaluate.h"
#include "misc.h"
#include "movegen.h"
#include "movepick.h"
#include "nnue/evaluate_nnue.h"
#include "nnue/nnue_common.h"
#include "position.h"
#include "syzygy/tbprobe.h"
#include "thread.h"
#include "timeman.h"
#include "tt.h"
#include "uci.h"
#include "ucioption.h"
#include "learn/learn.h"      //Khalid
#include "mcts/montecarlo.h"  //Montecarlo

namespace ShashChess {
// Kelly begin
bool                               useLearning          = true;
bool                               enabledLearningProbe = false;
std::vector<PersistedLearningMove> gameLine;
// Kelly end

namespace Search {
// from Shashin
bool highTal, middleTal, lowTal, capablanca, highPetrosian, middlePetrosian, lowPetrosian;
// end from Shashin
}

namespace TB = Tablebases;

using Eval::evaluate;
using namespace Search;

namespace {


// Futility margin
Value futility_margin(Depth d, bool noTtCutNode, bool improving) {
    Value futilityMult = 117 - 44 * noTtCutNode;
    return (futilityMult * d - 3 * futilityMult / 2 * improving);
}


//from ShashChess begin
// 8001 * 241 = 1928241
#define WIN_PROBABILITY_SIZE 1928241

uint8_t WinProbability[WIN_PROBABILITY_SIZE];

// GET_WIN_PROBABILITY(v, m) WinProbability[(v) + 4000][(m) - 10]
#define GET_WIN_PROBABILITY(v, m) WinProbability[((v) + 4000) * 241 + m]

#undef WIN_PROBABILITY_SIZE
// uint8_t WinProbability[8001][241];

//from ShashChess end


constexpr int futility_move_count(bool improving, Depth depth) {
    return improving ? (3 + depth * depth) : (3 + depth * depth) / 2;
}

// Add correctionHistory value to raw staticEval and guarantee evaluation does not hit the tablebase range
Value to_corrected_static_eval(Value v, const Worker& w, const Position& pos) {
    auto cv = w.correctionHistory[pos.side_to_move()][pawn_structure_index<Correction>(pos)];
    v += cv * std::abs(cv) / 12475;
    return std::clamp(v, VALUE_TB_LOSS_IN_MAX_PLY + 1, VALUE_TB_WIN_IN_MAX_PLY - 1);
}

// History and stats update bonus, based on depth
int stat_bonus(Depth d) { return std::min(246 * d - 351, 1136); }

// History and stats update malus, based on depth
int stat_malus(Depth d) { return std::min(519 * d - 306, 1258); }

// Add a small random component to draw evaluations to avoid 3-fold blindness
Value value_draw(size_t nodes) { return VALUE_DRAW - 1 + Value(nodes & 0x2); }

//opening variety begin
int openingVariety;
//opening variety end


Value value_to_tt(Value v, int ply);
Value value_from_tt(Value v, int ply, int r50c);
void  update_pv(Move* pv, Move move, const Move* childPv);
void  update_continuation_histories(Stack* ss, Piece pc, Square to, int bonus);
void  update_quiet_stats(
   const Position& pos, Stack* ss, Search::Worker& workerThread, Move move, int bonus);
void update_all_stats(const Position& pos,
                      Stack*          ss,
                      Search::Worker& workerThread,
                      Move            bestMove,
                      Value           bestValue,
                      Value           beta,
                      Square          prevSq,
                      Move*           quietsSearched,
                      int             quietCount,
                      Move*           capturesSearched,
                      int             captureCount,
                      Depth           depth);

}  // namespace


// livebook begin
#ifdef USE_LIVEBOOK
CURL*       g_cURL;
std::string g_szRecv;
std::string g_livebookURL = "http://www.chessdb.cn/cdb.php";
int         g_inBook;
//livebook depth begin
int         livebook_depth_count = 0;
int         max_book_depth;
//livebook depth end
size_t cURL_WriteFunc(void* contents, size_t size, size_t nmemb, std::string* s) {
    size_t newLength = size * nmemb;
    try
    { s->append((char*) contents, newLength); } catch (std::bad_alloc&)
    {
        // handle memory problem
        return 0;
    }
    return newLength;
}
    bool egtbs=false;
    bool noEgtbs=false;
    bool both=false;
    bool off=false;
void Search::setLiveBookURL(const std::string& newURL) { g_livebookURL = newURL; };
void Search::set_livebook(const std::string& livebook) 
{
    egtbs=(livebook=="Egtbs")||(livebook=="Both");
    noEgtbs=(livebook=="NoEgtbs");
    both=(livebook=="Both");
    off=(livebook=="Off");
};
void Search::setLiveBookTimeout(size_t newTimeoutMS) {
    curl_easy_setopt(g_cURL, CURLOPT_TIMEOUT_MS, newTimeoutMS);
};
void Search::set_livebook_depth(int book_depth) { max_book_depth = book_depth;}; //livebook depth
void Search::set_g_inBook(int livebook_retry){g_inBook = livebook_retry;};
#endif
// livebook end
//for learning begin
inline bool is_game_decided(const Position& pos, Value lastScore) {
    static constexpr const Value DecidedGameEvalThreeshold = PawnValue * 5;
    static constexpr const int   DecidedGameMaxPly         = 150;
    static constexpr const int   DecidedGameMaxPieceCount  = 5;

    //Assume game is decided if |last sent score| is above DecidedGameEvalThreeshold
    if (lastScore != VALUE_NONE && std::abs(lastScore) > DecidedGameEvalThreeshold)
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

//from ShashChess begin
void Search::initWinProbability() {
    for (int value = -4000; value <= 4000; ++value)
    {
        for (Depth depth = 0; depth <= 240; ++depth)
        {
            // WinProbability[value + 4000][depth] =
            GET_WIN_PROBABILITY(value, depth) = UCI::getWinProbability(value, depth);

        }
    }
}
//from ShashChess end

// from Shashin
inline Value static_value(Position& pos, Stack* ss, int optimism) {
    // Check if MAX_PLY is reached
    if (ss->ply >= MAX_PLY)
        return VALUE_DRAW;

    // Check for immediate draw
    if (pos.is_draw(ss->ply) && !pos.checkers())
        return VALUE_DRAW;

    // Detect mate and stalemate situations
    if (MoveList<LEGAL>(pos).size() == 0)
        return pos.checkers() ? VALUE_MATE : VALUE_DRAW;

    //Should not call evaluate() if the side to move is under check!
    if (pos.checkers())
        return VALUE_DRAW;  //TODO: Not sure if VALUE_DRAW is correct!

    // Evaluate the position statically
    return evaluate(pos, optimism);
}

inline int8_t getShashinRange(Value value, int ply) {
    short   capturedValue  = (std::clamp(value, (Value) (-4000), (Value) (4000)));
    uint8_t capturedPly    = std::min(240, ply);
    //uint8_t winProbability = WinProbability[capturedValue + 4000][capturedPly];
    uint8_t winProbability = GET_WIN_PROBABILITY(capturedValue + 4000, capturedPly);
    if (winProbability <= SHASHIN_HIGH_PETROSIAN_THRESHOLD)
    {
        return SHASHIN_POSITION_HIGH_PETROSIAN;
    }
    if ((winProbability > SHASHIN_HIGH_PETROSIAN_THRESHOLD)
        && (winProbability <= SHASHIN_MIDDLE_HIGH_PETROSIAN_THRESHOLD))
    {
        return SHASHIN_POSITION_MIDDLE_HIGH_PETROSIAN;
    }
    if ((winProbability > SHASHIN_MIDDLE_HIGH_PETROSIAN_THRESHOLD)
        && (winProbability <= SHASHIN_MIDDLE_PETROSIAN_THRESHOLD))
    {
        return SHASHIN_POSITION_MIDDLE_PETROSIAN;
    }
    if ((winProbability > SHASHIN_MIDDLE_PETROSIAN_THRESHOLD)
        && (winProbability <= SHASHIN_MIDDLE_LOW_PETROSIAN_THRESHOLD))
    {
        return SHASHIN_POSITION_MIDDLE_LOW_PETROSIAN;
    }
    if ((winProbability > SHASHIN_MIDDLE_LOW_PETROSIAN_THRESHOLD)
        && (winProbability <= SHASHIN_LOW_PETROSIAN_THRESHOLD))
    {
        return SHASHIN_POSITION_LOW_PETROSIAN;
    }
    if ((winProbability > SHASHIN_LOW_PETROSIAN_THRESHOLD)
        && (winProbability <= 100 - SHASHIN_CAPABLANCA_THRESHOLD))
    {
        return SHASHIN_POSITION_CAPABLANCA_PETROSIAN;
    }
    if ((winProbability > (100 - SHASHIN_CAPABLANCA_THRESHOLD))
        && (winProbability < SHASHIN_CAPABLANCA_THRESHOLD))
    {
        return SHASHIN_POSITION_CAPABLANCA;
    }
    if ((winProbability < SHASHIN_LOW_TAL_THRESHOLD)
        && (winProbability >= SHASHIN_CAPABLANCA_THRESHOLD))
    {
        return SHASHIN_POSITION_CAPABLANCA_TAL;
    }
    if ((winProbability < SHASHIN_MIDDLE_LOW_TAL_THRESHOLD)
        && (winProbability >= SHASHIN_LOW_TAL_THRESHOLD))
    {
        return SHASHIN_POSITION_LOW_TAL;
    }
    if ((winProbability < SHASHIN_MIDDLE_TAL_THRESHOLD)
        && (winProbability >= SHASHIN_MIDDLE_LOW_TAL_THRESHOLD))
    {
        return SHASHIN_POSITION_MIDDLE_LOW_TAL;
    }
    if ((winProbability < SHASHIN_MIDDLE_HIGH_TAL_THRESHOLD)
        && (winProbability >= SHASHIN_MIDDLE_TAL_THRESHOLD))
    {
        return SHASHIN_POSITION_MIDDLE_TAL;
    }
    if ((winProbability < SHASHIN_HIGH_TAL_THRESHOLD)
        && (winProbability >= SHASHIN_MIDDLE_HIGH_TAL_THRESHOLD))
    {
        return SHASHIN_POSITION_MIDDLE_HIGH_TAL;
    }
    if (winProbability >= SHASHIN_HIGH_TAL_THRESHOLD)
    {
        return SHASHIN_POSITION_HIGH_TAL;
    }
    return SHASHIN_POSITION_TAL_CAPABLANCA_PETROSIAN;
}

inline bool isShashinHigh(Worker* worker) {
    return (worker->shashinWinProbabilityRange != SHASHIN_POSITION_HIGH_PETROSIAN)
        && (worker->shashinWinProbabilityRange != SHASHIN_POSITION_HIGH_TAL);
}
inline bool isShashinHighMiddle(Worker* worker) {
    return isShashinHigh(worker)
        && (worker->shashinWinProbabilityRange != SHASHIN_POSITION_MIDDLE_HIGH_PETROSIAN)
        && (worker->shashinWinProbabilityRange != SHASHIN_POSITION_MIDDLE_HIGH_TAL);
}
inline bool isShashinMiddle(Worker* worker) {
    return isShashinHighMiddle(worker)
        && (worker->shashinWinProbabilityRange != SHASHIN_POSITION_MIDDLE_PETROSIAN)
        && (worker->shashinWinProbabilityRange != SHASHIN_POSITION_MIDDLE_TAL);
}
inline bool isShashinMiddleLow(Worker* worker) {
    return isShashinMiddle(worker)
        && (worker->shashinWinProbabilityRange != SHASHIN_POSITION_MIDDLE_LOW_PETROSIAN)
        && (worker->shashinWinProbabilityRange != SHASHIN_POSITION_MIDDLE_LOW_TAL);
}
inline bool isShashinLow(Worker* worker) {
    return isShashinMiddleLow(worker)
        && (worker->shashinWinProbabilityRange != SHASHIN_POSITION_LOW_PETROSIAN)
        && (worker->shashinWinProbabilityRange != SHASHIN_POSITION_LOW_TAL);
}

inline void updateShashinValues(Value score, int ply, Worker* worker) {
    if ((((ply > worker->shashinPly) || (ply == 0))))
    {
        worker->shashinWinProbabilityRange = getShashinRange(score, ply);
        worker->shashinPly                 = ply;
    }
}

inline bool isShashinPositionPetrosian(Worker* worker) {
    if ((worker->shashinWinProbabilityRange == SHASHIN_POSITION_HIGH_PETROSIAN)
        || (worker->shashinWinProbabilityRange == SHASHIN_POSITION_MIDDLE_HIGH_PETROSIAN)
        || (worker->shashinWinProbabilityRange == SHASHIN_POSITION_MIDDLE_PETROSIAN)
        || (worker->shashinWinProbabilityRange == SHASHIN_POSITION_MIDDLE_LOW_PETROSIAN)
        || (worker->shashinWinProbabilityRange == SHASHIN_POSITION_LOW_PETROSIAN))
    {
        return true;
    }
    return false;
}
inline bool isShashinPositionTal(Worker* worker) {
    if ((worker->shashinWinProbabilityRange == SHASHIN_POSITION_HIGH_TAL)
        || (worker->shashinWinProbabilityRange == SHASHIN_POSITION_MIDDLE_HIGH_TAL)
        || (worker->shashinWinProbabilityRange == SHASHIN_POSITION_MIDDLE_TAL)
        || (worker->shashinWinProbabilityRange == SHASHIN_POSITION_MIDDLE_LOW_TAL)
        || (worker->shashinWinProbabilityRange == SHASHIN_POSITION_LOW_TAL))
    {
        return true;
    }
    return false;
}
inline int8_t getInitialShashinWinProbabilityRange(Position& pos, Stack* ss, int optimism) {
    if (!highTal && !middleTal && !lowTal && !capablanca && !highPetrosian && !middlePetrosian
        && !lowPetrosian)
        return getShashinRange(static_value(pos, ss, optimism), std::max(pos.game_ply(), ss->ply));
    if (highPetrosian && !middlePetrosian && !lowPetrosian && !capablanca && !lowTal && !middleTal
        && !highTal)
        return SHASHIN_POSITION_HIGH_PETROSIAN;
    if (highPetrosian && middlePetrosian && !lowPetrosian && !capablanca && !lowTal && !middleTal
        && !highTal)
        return SHASHIN_POSITION_MIDDLE_HIGH_PETROSIAN;
    if (!highPetrosian && middlePetrosian && !lowPetrosian && !capablanca && !lowTal && !middleTal
        && !highTal)
        return SHASHIN_POSITION_MIDDLE_PETROSIAN;
    if (!highPetrosian && middlePetrosian && lowPetrosian && !capablanca && !lowTal && !middleTal
        && !highTal)
        return SHASHIN_POSITION_MIDDLE_LOW_PETROSIAN;
    if (!highPetrosian && !middlePetrosian && lowPetrosian && !capablanca && !lowTal && !middleTal
        && !highTal)
        return SHASHIN_POSITION_LOW_PETROSIAN;
    if (!highPetrosian && !middlePetrosian && lowPetrosian && capablanca && !lowTal && !middleTal
        && !highTal)
        return SHASHIN_POSITION_CAPABLANCA_PETROSIAN;
    if (!highPetrosian && !middlePetrosian && !lowPetrosian && capablanca && !lowTal && !middleTal
        && !highTal)
        return SHASHIN_POSITION_CAPABLANCA;
    if (!highPetrosian && !middlePetrosian && !lowPetrosian && capablanca && lowTal && !middleTal
        && !highTal)
        return SHASHIN_POSITION_CAPABLANCA_TAL;
    if (!highPetrosian && !middlePetrosian && !lowPetrosian && !capablanca && lowTal && !middleTal
        && !highTal)
        return SHASHIN_POSITION_LOW_TAL;
    if (!highPetrosian && !middlePetrosian && !lowPetrosian && !capablanca && lowTal && middleTal
        && !highTal)
        return SHASHIN_POSITION_MIDDLE_LOW_TAL;
    if (!highPetrosian && !middlePetrosian && !lowPetrosian && !capablanca && !lowTal && middleTal
        && !highTal)
        return SHASHIN_POSITION_MIDDLE_TAL;
    if (!highPetrosian && !middlePetrosian && !lowPetrosian && !capablanca && !lowTal && middleTal
        && highTal)
        return SHASHIN_POSITION_MIDDLE_HIGH_TAL;
    if (!highPetrosian && !middlePetrosian && !lowPetrosian && !capablanca && !lowTal && !middleTal
        && highTal)
        return SHASHIN_POSITION_HIGH_TAL;
    return SHASHIN_POSITION_TAL_CAPABLANCA_PETROSIAN;
}

inline void initShashinValues(Position& pos, Stack* ss, Worker* worker, int optimism) {
    worker->shashinPly                 = std::max(pos.game_ply(), ss->ply);
    worker->shashinWinProbabilityRange = getInitialShashinWinProbabilityRange(pos, ss, optimism);
}
// end from Shashin

Search::Worker::Worker(SharedState&                    sharedState,
                       std::unique_ptr<ISearchManager> sm,
                       size_t                          thread_id) :
    // Unpack the SharedState struct into member variables
    thread_idx(thread_id),
    manager(std::move(sm)),
    bookMan(sharedState.bookMan),
    evalFiles(sharedState.evalFiles),
    options(sharedState.options),
    threads(sharedState.threads),
    tt(sharedState.tt) {
    clear();
}

void Search::Worker::start_searching() {
    // Non-main threads go directly to iterative_deepening()
    if (!is_mainthread())
    {
        iterative_deepening();
        return;
    }

    main_manager()->tm.init(limits, rootPos.side_to_move(), rootPos.game_ply(), options);
    tt.new_search();
    // Kelly begin
    enabledLearningProbe = false;
    useLearning          = true;
    // Kelly end

    openingVariety = options["Opening variety"];  // from Sugar
    // begin from Shashin
    highTal         = options["High Tal"];
    middleTal       = options["Middle Tal"];
    lowTal          = options["Low Tal"];
    capablanca      = options["Capablanca"];
    highPetrosian   = options["High Petrosian"];
    middlePetrosian = options["Middle Petrosian"];
    lowPetrosian    = options["Low Petrosian"];
    // end from Shashin

    Move bookMove = Move::none();  //Books management

    if (rootMoves.empty())
    {
        rootMoves.emplace_back(Move::none());
        sync_cout << "info depth 0 score "
                  << UCI::value(rootPos.checkers() ? -VALUE_MATE : VALUE_DRAW) << sync_endl;
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
            if (bookMove != Move::none()
                && std::find(rootMoves.begin(), rootMoves.end(), bookMove) != rootMoves.end())
            {
                think = false;

                for (Thread* th : threads)
                    std::swap(th->worker->rootMoves[0],
                              *std::find(th->worker->rootMoves.begin(), th->worker->rootMoves.end(),
                                         bookMove));
            }
        }
// Live Book begin
#ifdef USE_LIVEBOOK
        if ((think) && (!bookMove))
        {  
            if (!off && g_inBook && !limits.infinite && !limits.mate)
            {
                //livebook _depth begin
                if (rootPos.game_ply() == 0)
                    livebook_depth_count = 0;
                if ((livebook_depth_count < max_book_depth)||(rootPos.count<ALL_PIECES>() <=7) )
                {
                //livebook _depth end
                    CURLcode    res;
                    char*       szFen = curl_easy_escape(g_cURL, rootPos.fen().c_str(), 0);
                    std::string szURL = g_livebookURL + "?action="
                                        + (options["Live Book Diversity"] ? "query" : "querybest")
                                        + "&board=" + szFen;
                    curl_free(szFen);
                    curl_easy_setopt(g_cURL, CURLOPT_URL, szURL.c_str());
                    g_szRecv.clear();
                    res = curl_easy_perform(g_cURL);
                    if (res == CURLE_OK)
                    {
                        g_szRecv.erase(std::find(g_szRecv.begin(), g_szRecv.end(), '\0'),
                                        g_szRecv.end());
                        std::string tmp ="";
                        if (((noEgtbs || both) && (g_szRecv.find("move:") != std::string::npos))||
                            ((egtbs || both) &&(g_szRecv.find("egtb:") != std::string::npos)))
                        {
                            tmp = g_szRecv.substr(5);
                        }
                        if ((egtbs || both) && (g_szRecv.find("search:") != std::string::npos))
                        {
                            std::string delimiter = "|";
                            size_t pos = g_szRecv.find(delimiter);
                            tmp = g_szRecv.substr(g_szRecv.find(":") + 1, pos - g_szRecv.find(":") - 1);
                        }
                        if(!tmp.empty())
                        {
                            bookMove        = UCI::to_move(rootPos, tmp);
                            livebook_depth_count++;//livebook_depth
                        }
                    }
                }
                if (bookMove && std::count(rootMoves.begin(), rootMoves.end(), bookMove))
                {
                    g_inBook = options["Live Book Retry"];
                    think    = false;
                    for (Thread* th : threads)
                        std::swap(th->worker->rootMoves[0],
                                    *std::find(th->worker->rootMoves.begin(),
                                                th->worker->rootMoves.end(), bookMove));
                }
                else
                {
                    bookMove = Move::none();
                    g_inBook--;
                }
            }
        }
#endif
        // Live Book end
        
        //from Book and live book management begin
        if (!bookMove || think)
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
    // "ponderhit" just reset threads.ponder).
    threads.stop = true;

    // Wait until all threads have finished
    threads.wait_for_search_finished();

    // When playing in 'nodes as time' mode, subtract the searched nodes from
    // the available ones before exiting.
    if (limits.npmsec)
        main_manager()->tm.advance_nodes_time(limits.inc[rootPos.side_to_move()]
                                              - threads.nodes_searched());

    Worker* bestThread = this;

    if (int(options["MultiPV"]) == 1 && !limits.depth && rootMoves[0].pv[0] != Move::none())
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
            if (LD.learning_mode() == LearningMode::Self)
            {
                const LearningMove* existingMove = LD.probe_move(plm.key, plm.learningMove.move);
                if (existingMove)
                    plm.learningMove.score = existingMove->score;
                gameLine.push_back(plm);
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
    // Kelly end

    // Send again PV info if we have a new best thread
    if (bestThread != this)
        sync_cout << main_manager()->pv(*bestThread, threads, tt, bestThread->completedDepth)
                  << sync_endl;

    sync_cout << "bestmove " << UCI::move(bestThread->rootMoves[0].pv[0], rootPos.is_chess960());

    if (bestThread->rootMoves[0].pv.size() > 1
        || bestThread->rootMoves[0].extract_ponder_from_tt(tt, rootPos))
        std::cout << " ponder " << UCI::move(bestThread->rootMoves[0].pv[1], rootPos.is_chess960());

    std::cout << sync_endl;

    // from Khalid begin
    // Save learning data if game is already decided
    if (!bookMove)
    {
        if (is_game_decided(rootPos, (bestThread->rootMoves[0].score)) && LD.is_enabled()
            && !LD.is_paused())
        {
            // Perform Q-learning if enabled
            if (LD.learning_mode() == LearningMode::Self)
                putGameLineIntoLearningTable();
            // Save to learning file
            if (!LD.is_readonly())
            {

                LD.persist(options);
            }
            // Stop learning until we receive *ucinewgame* command
            LD.pause();
        }
    }

    // from Khalid end
// livebook begin
#ifdef USE_LIVEBOOK
    if (!g_inBook && options["Live Book Contribute"])
    {
        char*       szFen = curl_easy_escape(g_cURL, rootPos.fen().c_str(), 0);
        std::string szURL = g_livebookURL + "?action=store" + "&board=" + szFen + "&move=move:"
                          + UCI::move(bestThread->rootMoves[0].pv[0], rootPos.is_chess960());
        curl_free(szFen);
        curl_easy_setopt(g_cURL, CURLOPT_URL, szURL.c_str());
        curl_easy_perform(g_cURL);
    }
#endif
    // livebook end
}

// Main iterative deepening loop. It calls search()
// repeatedly with increasing depth until the allocated thinking time has been
// consumed, the user stops the search, or the maximum search depth is reached.
void Search::Worker::iterative_deepening() {

    SearchManager* mainThread = (thread_idx == 0 ? main_manager() : nullptr);

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
    // (ss + 2) is needed for initialization of cutOffCnt and killers.
    Stack  stack[MAX_PLY + 10] = {};
    Stack* ss                  = stack + 7;

    for (int i = 7; i > 0; --i)
    {
        (ss - i)->continuationHistory =
          &this->continuationHistory[0][0][NO_PIECE][0];  // Use as a sentinel
        (ss - i)->staticEval = VALUE_NONE;
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

    multiPV = std::min(multiPV, rootMoves.size());
    initShashinValues(rootPos, ss, this, VALUE_ZERO);  //from Shashin

    int searchAgainCounter = 0;

    // from mcts begin
    optimism[WHITE] = optimism[BLACK] =
      VALUE_ZERO;  //Must initialize optimism before calling static_value(). Not sure if 'VALUE_ZERO' is the right value

    bool  maybeDraw    = rootPos.rule50_count() >= 90 || rootPos.has_game_cycle(2);
    Value rootPosValue = static_value(rootPos, ss, this->optimism[us]);
    bool  possibleMCTSByValue =
      ((this->shashinWinProbabilityRange == SHASHIN_POSITION_HIGH_PETROSIAN)
       || (this->shashinWinProbabilityRange == SHASHIN_POSITION_MIDDLE_HIGH_PETROSIAN)
       || (this->shashinWinProbabilityRange == SHASHIN_POSITION_MIDDLE_PETROSIAN));
    if (!mainThread && bool(options["MCTS by Shashin"]) && multiPV == 1 && !maybeDraw
        && possibleMCTSByValue && (this->thread_idx) <= (size_t) (mctsThreads)
        && !is_game_decided(rootPos, rootPosValue))
    {
        MonteCarlo* monteCarlo = new MonteCarlo(rootPos, this);

        if (monteCarlo)
        {
#if !defined(NDEBUG) && !defined(_NDEBUG)
            sync_cout << "info string *** Thread[" << thread_idx << "] is running MCTS search"
                      << sync_endl;
#endif

            monteCarlo->search(threads, limits, is_mainthread(), this, tt);
            if ((this->thread_idx) == 1 && limits.infinite
                && threads.stop.load(std::memory_order_relaxed))
                monteCarlo->print_children();

            delete monteCarlo;

#if !defined(NDEBUG) && !defined(_NDEBUG)
            sync_cout << "info string *** Thread[" << thread_idx << "] finished MCTS search"
                      << sync_endl;
#endif

            return;
        }
    }

#if !defined(NDEBUG) && !defined(_NDEBUG)
    sync_cout << "info string *** Thread[" << thread_idx << "] is running A/B search" << sync_endl;
#endif

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
        for (pvIdx = 0; pvIdx < multiPV && !threads.stop; ++pvIdx)
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
            Value avg = rootMoves[pvIdx].averageScore;
            delta     = 9 + avg * avg / 12487;
            alpha     = std::max(avg - delta, -VALUE_INFINITE);
            beta      = std::min(avg + delta, VALUE_INFINITE);

            // Adjust optimism based on root move's averageScore (~4 Elo)
            optimism[us]  = 134 * avg / (std::abs(avg) + 97);
            optimism[~us] = -optimism[us];

            // Start with a small aspiration window and, in the case of a fail
            // high/low, re-search with a bigger window until we don't fail
            // high/low anymore.
            int failedHighCnt = 0;
            while (true)
            {
                // Adjust the effective depth searched, but ensure at least one effective increment
                // for every four searchAgain steps (see issue #2717).
                Depth adjustedDepth =
                  (this->shashinWinProbabilityRange != SHASHIN_POSITION_HIGH_TAL)
                    ? std::max(1, rootDepth - failedHighCnt - 3 * (searchAgainCounter + 1) / 4)
                    : rootDepth;  //from Crystal yes:HT,TC, C, CP no:LT,MHP,P
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

                // When failing high/low give some update (without cluttering
                // the UI) before a re-search.
                if (mainThread && multiPV == 1 && (bestValue <= alpha || bestValue >= beta)
                    && mainThread->tm.elapsed(threads.nodes_searched()) > 3000)
                    sync_cout << main_manager()->pv(*this, threads, tt, rootDepth) << sync_endl;

                // In case of failing low/high increase aspiration window and
                // re-search, otherwise exit the loop.
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
                && (threads.stop || pvIdx + 1 == multiPV
                    || mainThread->tm.elapsed(threads.nodes_searched()) > 3000)
                // A thread that aborted search can have mated-in/TB-loss PV and score
                // that cannot be trusted, i.e. it can be delayed or refuted if we would have
                // had time to fully search other root-moves. Thus we suppress this output and
                // below pick a proven score/PV for this thread (from the previous iteration).
                && !(threads.abortedSearch && rootMoves[0].uciScore <= VALUE_TB_LOSS_IN_MAX_PLY))
                sync_cout << main_manager()->pv(*this, threads, tt, rootDepth) << sync_endl;
        }

        if (!threads.stop)
            completedDepth = rootDepth;

        // We make sure not to pick an unproven mated-in score,
        // in case this thread prematurely stopped search (aborted-search).
        if (threads.abortedSearch && rootMoves[0].score != -VALUE_INFINITE
            && rootMoves[0].score <= VALUE_TB_LOSS_IN_MAX_PLY)
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

        // Have we found a "mate in x"?
        if (limits.mate && bestValue >= VALUE_MATE_IN_MAX_PLY
            && VALUE_MATE - bestValue <= 2 * limits.mate)
            threads.stop = true;

        if (!mainThread)
            continue;

        // Use part of the gained time from a previous stable move for the current move
        for (Thread* th : threads)
        {
            totBestMoveChanges += th->worker->bestMoveChanges;
            th->worker->bestMoveChanges = 0;
        }

        // Do we have time for the next iteration? Can we stop searching now?
        if (limits.use_time_management() && !threads.stop && !mainThread->stopOnPonderhit)
        {
            auto bestmove    = rootMoves[0].pv[0];
            int  nodesEffort = effort[bestmove.from_sq()][bestmove.to_sq()] * 100
                            / std::max(size_t(1), size_t(nodes));

            double fallingEval = (66 + 14 * (mainThread->bestPreviousAverageScore - bestValue)
                                  + 6 * (mainThread->iterValue[iterIdx] - bestValue))
                               / 616.6;
            fallingEval = std::clamp(fallingEval, 0.51, 1.51);

            // If the bestMove is stable over several iterations, reduce time accordingly
            timeReduction    = lastBestMoveDepth + 8 < completedDepth ? 1.56 : 0.69;
            double reduction = (1.4 + mainThread->previousTimeReduction) / (2.17 * timeReduction);
            double bestMoveInstability = 1 + 1.79 * totBestMoveChanges / threads.size();

            double totalTime =
              mainThread->tm.optimum() * fallingEval * reduction * bestMoveInstability;

            // Cap used time in case of a single legal move for a better viewer experience
            if (rootMoves.size() == 1)
                totalTime = std::min(500.0, totalTime);

            if (completedDepth >= 10 && nodesEffort >= 95
                && mainThread->tm.elapsed(threads.nodes_searched()) > totalTime * 3 / 4
                && !mainThread->ponder)
            {
                threads.stop = true;
            }

            // Stop the search if we have exceeded the totalTime
            if (mainThread->tm.elapsed(threads.nodes_searched()) > totalTime)
            {
                // If we are allowed to ponder do not stop the search now but
                // keep pondering until the GUI sends "ponderhit" or "stop".
                if (mainThread->ponder)
                    mainThread->stopOnPonderhit = true;
                else
                    threads.stop = true;
            }
            else if (!mainThread->ponder
                     && mainThread->tm.elapsed(threads.nodes_searched()) > totalTime * 0.50)
                threads.increaseDepth = false;
            else
                threads.increaseDepth = true;
        }

        mainThread->iterValue[iterIdx] = bestValue;
        iterIdx                        = (iterIdx + 1) & 3;
    }

    if (!mainThread)
        return;

    mainThread->previousTimeReduction = timeReduction;
}

void Search::Worker::clear() {
    counterMoves.fill(Move::none());
    mainHistory.fill(0);
    captureHistory.fill(0);
    pawnHistory.fill(0);
    correctionHistory.fill(0);

    for (bool inCheck : {false, true})
        for (StatsType c : {NoCaptures, Captures})
            for (auto& to : continuationHistory[inCheck][c])
                for (auto& h : to)
                    h->fill(-71);

    for (size_t i = 1; i < reductions.size(); ++i)
        reductions[i] = int((18.79 + std::log(size_t(options["Threads"])) / 2) * std::log(i));
// livebook begin
#ifdef USE_LIVEBOOK
    curl_global_init(CURL_GLOBAL_DEFAULT);
    g_cURL = curl_easy_init();
    curl_easy_setopt(g_cURL, CURLOPT_TIMEOUT_MS, 1500L);
    curl_easy_setopt(g_cURL, CURLOPT_WRITEFUNCTION, cURL_WriteFunc);
    curl_easy_setopt(g_cURL, CURLOPT_WRITEDATA, &g_szRecv);
    g_inBook = (int)options["Live Book Retry"];
    set_livebook_depth((int) options["Live Book Depth"]);//livebook depth
#endif
    // livebook end
}


// Main search function for both PV and non-PV nodes.
template<NodeType nodeType>
Value Search::Worker::search(
  Position& pos, Stack* ss, Value alpha, Value beta, Depth depth, bool cutNode) {

    constexpr bool PvNode    = nodeType != NonPV;
    constexpr bool rootNode  = nodeType == Root;
    bool           gameCycle = false;  // from Crystal

    // Dive into quiescence search when the depth reaches zero
    if (depth <= 0)
        return qsearch < PvNode ? PV : NonPV > (pos, ss, alpha, beta);

    // Check if we have an upcoming move that draws by repetition, or
    // if the opponent had an alternative move earlier to this position.
    // from Crystal begin
    if (!rootNode)
    {
        if (pos.has_game_cycle(ss->ply))
        {
            if (alpha < VALUE_DRAW)
            {

                alpha = value_draw(this->nodes);
                if (alpha >= beta)
                    return alpha;
            }
            gameCycle = true;
        }
    }
    // from Crystal end

    assert(-VALUE_INFINITE <= alpha && alpha < beta && beta <= VALUE_INFINITE);
    assert(PvNode || (alpha == beta - 1));
    assert(0 < depth && depth < MAX_PLY);
    assert(!(PvNode && cutNode));

    Move      pv[MAX_PLY + 1], capturesSearched[32], quietsSearched[32];
    StateInfo st;
    ASSERT_ALIGNED(&st, Eval::NNUE::CacheLineSize);

    TTEntry* tte;
    Key      posKey;
    Move     ttMove, move, excludedMove = Move::none(), bestMove,
                       expTTMove = Move::none();  // from Kelly
    Depth extension, newDepth;
    // from Kelly begin
    Value bestValue, value, ttValue, eval = VALUE_NONE, maxValue, expTTValue = VALUE_NONE,
                                     probCutBeta;
    bool givesCheck, improving, priorCapture, expTTHit = false;
    bool isMate;  // from Crystal
    //from Kelly End
    bool  capture, moveCountPruning, ttCapture, kingDanger, nullParity;  // from Crystal
    Piece movedPiece;
    int   moveCount, captureCount, quietCount, ourMove;  // from Crystal
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
    moveCount = captureCount = quietCount = ss->moveCount = 0;
    bestValue                                             = -VALUE_INFINITE;
    maxValue                                              = VALUE_INFINITE;
    // from Crystal begin
    kingDanger        = false;
    ourMove           = !(ss->ply & 1);
    nullParity        = (ourMove == thisThread->nmpSide);
    ss->secondaryLine = false;
    ss->mainLine      = false;
    // from Crystal end
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

    if (!rootNode)
    {
        // Step 2. Check for aborted search and immediate draw
        if (threads.stop.load(std::memory_order_relaxed) || pos.is_draw(ss->ply)
            || ss->ply >= MAX_PLY)
            return (ss->ply >= MAX_PLY && !ss->inCheck) ? evaluate(pos, thisThread->optimism[us])
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
    else
        thisThread->rootDelta = beta - alpha;

    assert(0 <= ss->ply && ss->ply < MAX_PLY);

    (ss + 1)->excludedMove = bestMove = Move::none();
    (ss + 2)->killers[0] = (ss + 2)->killers[1] = Move::none();
    (ss + 2)->cutoffCnt                         = 0;
    ss->multipleExtensions                      = (ss - 1)->multipleExtensions;
    Square prevSq = ((ss - 1)->currentMove).is_ok() ? ((ss - 1)->currentMove).to_sq() : SQ_NONE;
    ss->statScore = 0;

    // Step 4. Transposition table lookup.
    excludedMove = ss->excludedMove;
    posKey       = pos.key();
    tte          = tt.probe(posKey, ss->ttHit);
    ttValue   = ss->ttHit ? value_from_tt(tte->value(), ss->ply, pos.rule50_count()) : VALUE_NONE;
    ttMove    = rootNode  ? thisThread->rootMoves[thisThread->pvIdx].pv[0]
              : ss->ttHit ? tte->move()
                          : Move::none();
    ttCapture = ttMove && pos.capture_stage(ttMove);

    // At this point, if excluded, skip straight to step 6, static eval. However,
    // to save indentation, we list the condition in all code between here and there.
    if (!excludedMove)
        ss->ttPv = PvNode || (ss->ttHit && tte->is_pv());

    // At non-PV nodes we check for an early TT cutoff
    if (!PvNode && !excludedMove && tte->depth() > depth
        && ttValue != VALUE_NONE  // Possible in case of TT access race or if !ttHit
        // from Crystal begin  Yes: HT C CCT No:MHP, HP
        && (tte->bound() & (ttValue >= beta ? BOUND_LOWER : BOUND_UPPER))
        && ((!gameCycle)
            || ((this->shashinWinProbabilityRange != SHASHIN_POSITION_HIGH_TAL)
                && (this->shashinWinProbabilityRange != SHASHIN_POSITION_CAPABLANCA))))
    // from Crystal end
    {
        // If ttMove is quiet, update move sorting heuristics on TT hit (~2 Elo)
        if (ttMove)
        {
            if (ttValue >= beta)
            {
                // Bonus for a quiet ttMove that fails high (~2 Elo)
                if (!ttCapture)
                    update_quiet_stats(pos, ss, *this, ttMove, stat_bonus(depth));

                // Extra penalty for early quiet moves of
                // the previous ply (~0 Elo on STC, ~2 Elo on LTC).
                if (prevSq != SQ_NONE && (ss - 1)->moveCount <= 2 && !priorCapture)
                    update_continuation_histories(ss - 1, pos.piece_on(prevSq), prevSq,
                                                  -stat_malus(depth + 1));
            }
        }

        // Partial workaround for the graph history interaction problem
        // For high rule50 counts don't produce transposition table cutoffs.
        if (pos.rule50_count() < 90)
            return ttValue >= beta && std::abs(ttValue) < VALUE_TB_WIN_IN_MAX_PLY
                   ? (ttValue * 3 + beta) / 4
                   : ttValue;
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
            if (!ttMove)
            {
                ttMove = learningMove->move;
            }

            if (learningMove->depth >= depth)
            {
                expTTMove       = learningMove->move;
                expTTValue      = learningMove->score;
                updatedLearning = true;
            }

            if ((learningMove->depth == 0))
                updatedLearning = false;

            if (updatedLearning && expTTValue != VALUE_NONE)


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

            // At non-PV nodes we check for an early Global Learning Table cutoff
            // If expTTMove is quiet, update move sorting heuristics on global learning table hit
            if (!PvNode && updatedLearning
                && expTTValue
                     != VALUE_NONE  // Possible in case of Global Learning Table access race
                && (learningMove->depth >= depth))
            {
                if (expTTValue >= beta)
                {
                    if (!pos.capture_stage(learningMove->move))
                        update_quiet_stats(pos, ss, *this, learningMove->move, stat_bonus(depth));

                    // Extra penalty for early quiet moves of the previous ply
                    if (prevSq != SQ_NONE && (ss - 1)->moveCount <= 2 && !priorCapture)
                        update_continuation_histories(ss - 1, pos.piece_on(prevSq), prevSq,
                                                      -stat_bonus(depth + 1));
                }
                // Penalty for a quiet ttMove that fails low
                else
                {
                    if (!pos.capture_stage(expTTMove))
                    {
                        int penalty = -stat_bonus(depth);
                        thisThread->mainHistory[us][expTTMove.from_to()] << penalty;
                        update_continuation_histories(ss, pos.moved_piece(expTTMove),
                                                      expTTMove.to_sq(), penalty);
                    }
                }

                // thisThread->tbHits.fetch_add(1, std::memory_order_relaxed);
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

                // use the range VALUE_TB to VALUE_TB_WIN_IN_MAX_PLY to score
                value = wdl < -drawScore ? -tbValue
                      : wdl > drawScore  ? tbValue
                                         : VALUE_DRAW + 2 * wdl * drawScore;

                Bound b = wdl < -drawScore ? BOUND_UPPER
                        : wdl > drawScore  ? BOUND_LOWER
                                           : BOUND_EXACT;

                if (b == BOUND_EXACT || (b == BOUND_LOWER ? value >= beta : value <= alpha))
                {
                    tte->save(posKey, value_to_tt(value, ss->ply), ss->ttPv, b,
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

    kingDanger = !ourMove && pos.king_danger(us);  //from crystal

    // Step 6. Static evaluation of the position
    Value unadjustedStaticEval = VALUE_NONE;
    if (ss->inCheck)
    {
        // Skip early pruning when in check
        ss->staticEval = eval = VALUE_NONE;
        improving             = false;
        goto moves_loop;
    }
    else if (excludedMove)
    {
        // Providing the hint that this node's accumulator will be used often
        // brings significant Elo gain (~13 Elo).
        Eval::NNUE::hint_common_parent_position(pos);
        unadjustedStaticEval = eval = ss->staticEval;
    }
    else if (ss->ttHit)
    {
        // Never assume anything about values stored in TT
        unadjustedStaticEval = tte->eval();
        if (unadjustedStaticEval == VALUE_NONE)
            unadjustedStaticEval = evaluate(pos, thisThread->optimism[us]);
        else if (PvNode)
            Eval::NNUE::hint_common_parent_position(pos);

        ss->staticEval = eval = to_corrected_static_eval(unadjustedStaticEval, *thisThread, pos);

        // ttValue can be used as a better position evaluation (~7 Elo)
        if (ttValue != VALUE_NONE && (tte->bound() & (ttValue > eval ? BOUND_LOWER : BOUND_UPPER)))
            eval = ttValue;
    }
    else
    {
        //Kelly begin
        unadjustedStaticEval = evaluate(pos, thisThread->optimism[us]);
        if (!LD.is_enabled() || !expTTHit || !updatedLearning)
        {
            ss->staticEval = eval =
              to_corrected_static_eval(unadjustedStaticEval, *thisThread, pos);

            // Static evaluation is saved as it was before adjustment by correction history
            tte->save(posKey, VALUE_NONE, ss->ttPv, BOUND_NONE, DEPTH_NONE, Move::none(),
                      unadjustedStaticEval, tt.generation());
        }
        else  // learning
        {
            // Never assume anything on values stored in Global Learning Table
            ss->staticEval = eval = expTTValue;
            if (eval == VALUE_NONE)
            {
                ss->staticEval = eval =
                  to_corrected_static_eval(unadjustedStaticEval, *thisThread, pos);
            }
            if (eval == VALUE_DRAW)
            {
                eval = value_draw(this->nodes);
            }
            // Can expTTValue be used as a better position evaluation?
            if (expTTValue != VALUE_NONE)
            {
                eval = expTTValue;
            }
        }
    }
    // from Kelly end

    // Use static evaluation difference to improve quiet move ordering (~9 Elo)
    if (((ss - 1)->currentMove).is_ok() && !(ss - 1)->inCheck && !priorCapture)
    {
        int bonus = std::clamp(-14 * int((ss - 1)->staticEval + ss->staticEval), -1723, 1455);
        bonus     = bonus > 0 ? 2 * bonus : bonus / 2;
        thisThread->mainHistory[~us][((ss - 1)->currentMove).from_to()] << bonus;
        if (type_of(pos.piece_on(prevSq)) != PAWN && ((ss - 1)->currentMove).type_of() != PROMOTION)
            thisThread->pawnHistory[pawn_structure_index(pos)][pos.piece_on(prevSq)][prevSq]
              << bonus / 4;
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
        // check at our previous move we look at static evaluation at move prior to it
        // and if we were in check at move prior to it flag is set to true) and is
        // false otherwise. The improving flag is used in various pruning heuristics.
        improving = (ss - 2)->staticEval != VALUE_NONE
                    ? ss->staticEval > (ss - 2)->staticEval
                    : (ss - 4)->staticEval != VALUE_NONE && ss->staticEval > (ss - 4)->staticEval;
    }
    // from Kelly end

    // Begin early pruning from Crystal by Shashin Yes: HT C No: MHP HP
    if ((!PvNode && (ourMove || !excludedMove) && !thisThread->nmpGuardV
         && abs(eval) < 2 * VALUE_KNOWN_WIN && abs(beta) < VALUE_MAX_EVAL && eval >= beta)
        || (this->shashinWinProbabilityRange != SHASHIN_POSITION_HIGH_TAL))
    {
        // Step 7. Razoring (~1 Elo)
        // If eval is really low check with qsearch if it can exceed alpha, if it can't,
        // return a fail low.
        // Adjust razor margin according to cutoffCnt. (~1 Elo)


        if (((this->shashinWinProbabilityRange != SHASHIN_POSITION_HIGH_TAL)
             && (this->shashinWinProbabilityRange != SHASHIN_POSITION_CAPABLANCA))
            && (eval < alpha - 438
                         - (332 - 154 * ((ss + 1)->cutoffCnt > 3)) * depth
                             * depth))  //from Crystal Yes: HT C No: MHP
        {
            value = qsearch<NonPV>(pos, ss, alpha - 1, alpha);
            if (value < alpha)
                return value;
        }

        // Step 8. Futility pruning: child node (~40 Elo)
        // The depth condition is important for mate finding.
        // from Crystal begin yes HT HMT no C MHP
        bool fromCrystalFutilityPruning =
          ((!(isShashinHighMiddle(this))) && (isShashinPositionTal(this)));
        if (!ss->ttPv && depth < fromCrystalFutilityPruning
              ? (9 - 2 * ((ss - 1)->mainLine || (ss - 1)->secondaryLine || (ttMove && !ttCapture)))
              : 11  //crystal
                  && eval - futility_margin(depth, cutNode && !ss->ttHit, improving)
                         - (ss - 1)->statScore / 314
                       >= beta
                  && eval >= beta && (!ttMove || ttCapture)

                  && (((!kingDanger && !gameCycle && !(thisThread->nmpGuard && nullParity)
                        && abs(alpha) < VALUE_KNOWN_WIN)
                       && fromCrystalFutilityPruning)
                      || ((eval >= beta && eval < 30016  // smaller than TB wins
                           && (!ttMove || ttCapture))
                          && (!fromCrystalFutilityPruning))))
            return fromCrystalFutilityPruning      ? eval
                 : beta > VALUE_TB_LOSS_IN_MAX_PLY ? (eval + beta) / 2
                                                   : eval;
        // from Crystal end yes HT HMT no C MHP

        // Step 9. Null move search with verification search (~35 Elo)
        //from Crystal begin yes: HT HMT C No: LT TC HP
        bool nullMoveSearchFromCrystal =
          ((!(isShashinHighMiddle(this))) && isShashinPositionTal(this));
        if (((ss - 1)->statScore < 16620) && eval >= ss->staticEval
            && ss->staticEval >= beta - 21 * depth + 330 && pos.non_pawn_material(us)
            && !disableNMAndPC  //Kelly
            && ((!nullMoveSearchFromCrystal && !PvNode && (ss - 1)->currentMove != Move::null()
                 && eval >= beta && !excludedMove && pos.non_pawn_material(us)
                 && ss->ply >= thisThread->nmpMinPly && beta > VALUE_TB_LOSS_IN_MAX_PLY)
                || (nullMoveSearchFromCrystal && !thisThread->nmpGuard && !gameCycle && !kingDanger
                    && (rootDepth < 11 || ourMove || MoveList<LEGAL>(pos).size() > 5))))
        //from Crystal end
        {
            assert(eval - beta >= 0);
            thisThread->nmpSide = ourMove;  // from Crystal
            // Null move dynamic reduction based on depth and eval
            Depth R = std::min(int(eval - beta) / 154, 6) + depth / 3 + 4;
            //from Crystal begin
            /*if (!ourMove && ((ss-1)->secondaryLine) && ((!(isShashinMiddle(this)))
                                             && isShashinPositionTal(this) ))
        R = std::min(R, 8);*/
            //from Crystal end
            /*if (   depth < 11
       	|| ttValue >= beta
        || (tte->depth()) < depth-R
        || !((tte->bound()) & BOUND_UPPER)        
        || (isShashinMiddle(this)) || (!isShashinPositionTal(this))) //from Crystal
    { */
            ss->currentMove         = Move::null();
            ss->continuationHistory = &thisThread->continuationHistory[0][0][NO_PIECE][0];

            pos.do_null_move(st, tt);
            thisThread->nmpGuard = true;  // from Crystal
            Value nullValue = -search<NonPV>(pos, ss + 1, -beta, -beta + 1, depth - R, !cutNode);
            thisThread->nmpGuard = false;  // from Crystal
            pos.undo_null_move();

            // Do not return unproven mate or TB scores
            if (nullValue >= beta && nullValue < VALUE_TB_WIN_IN_MAX_PLY)
            {
                if (thisThread->nmpMinPly || depth < 16)
                    return nullValue;

                assert(!thisThread->nmpMinPly);  // Recursive verification is not allowed

                // Do verification search at high depths, with null move pruning disabled
                // until ply exceeds nmpMinPly.
                //from Crystal begin yes TC C No HT MLT MHP HP
                if ((this->shashinWinProbabilityRange != SHASHIN_POSITION_CAPABLANCA))
                {
                    thisThread->nmpMinPly = ss->ply + 3 * (depth - R) / 4;
                }
                //from Crystal end
                thisThread->nmpGuardV = true;  // from Crystal
                Value v               = search<NonPV>(pos, ss, beta - 1, beta, depth - R, false);
                thisThread->nmpGuardV = false;  // from Crystal
                //from Crystal begin Yes HT HMT CCP No MLT MHP HP
                if (this->shashinWinProbabilityRange != SHASHIN_POSITION_HIGH_TAL)
                {
                    thisThread->nmpMinPly = 0;
                }
                //from Crystal end
                if (v >= beta)
                    return nullValue;
            }
        }

        // Step 10. Internal iterative reductions (~9 Elo)
        // For PV nodes without a ttMove, we decrease depth by 2,
        // or by 4 if the current position is present in the TT and
        // the stored depth is greater than or equal to the current depth.
        // Use qsearch if depth <= 0.
        if (PvNode && !ttMove
            && ((!gameCycle && depth >= 3 && (ss - 1)->moveCount > 1)
                || ((this->shashinWinProbabilityRange
                     != SHASHIN_POSITION_HIGH_TAL))))  //from Crystal yes HT C No MLT TC MHP HP
        {
            depth -= 2 + (this->shashinWinProbabilityRange != SHASHIN_POSITION_HIGH_TAL)
                     ? 2 * (ss->ttHit && tte->depth() >= depth)
                     : 0;  //from Crystal yes HT CCT No CCP MHP HP
        }
        if (depth <= 0)
        {
            return qsearch<PV>(pos, ss, alpha, beta);
        }
        // For cutNodes without a ttMove, we decrease depth by 2 if depth is high enough.
        if (cutNode && depth >= 8 && !ttMove)
            depth -= 2;


        // Step 11. ProbCut (~10 Elo)
        // Step 11. ProbCut (~10 Elo)
        // If we have a good enough capture (or queen promotion) and a reduced search returns a value
        // much above beta, we can (almost) safely prune the previous move.
        probCutBeta = beta + 181 - 68 * improving;
        //From Crystal begin yes HT
        bool probCutFromCrystal = (this->shashinWinProbabilityRange == SHASHIN_POSITION_HIGH_TAL);
        if (
          !disableNMAndPC  // Kelly
          && ((!probCutFromCrystal
               && std::abs(beta) < VALUE_TB_WIN_IN_MAX_PLY
               // If value from transposition table is lower than probCutBeta, don't attempt probCut
               // there and in further interactions with transposition table cutoff depth is set to depth - 3
               // because probCut search has depth set to depth - 4 but we also do a move before it
               // So effective depth is equal to depth - 3
               && !(tte->depth() >= depth - 3 && ttValue != VALUE_NONE && ttValue < probCutBeta))
              || (probCutFromCrystal && depth > 4
                  && (ttCapture || !ttMove)
                  // If we don't have a ttHit or our ttDepth is not greater our
                  // reduced depth search, continue with the probcut.
                  && (!ss->ttHit || tte->depth() < depth - 3))))
        //From Crystal end yes HT
        {
            assert(probCutBeta < VALUE_INFINITE && probCutBeta > beta);

            MovePicker mp(pos, ttMove, probCutBeta - ss->staticEval, &thisThread->captureHistory);

            while ((move = mp.next_move()) != Move::none())
                if (move != excludedMove && pos.legal(move))
                {
                    assert(pos.capture_stage(move));

                    // Prefetch the TT entry for the resulting position
                    prefetch(tt.first_entry(pos.key_after(move)));

                    ss->currentMove = move;
                    ss->continuationHistory =
                      &this->continuationHistory[ss->inCheck][true][pos.moved_piece(move)]
                                                [move.to_sq()];

                    thisThread->nodes.fetch_add(1, std::memory_order_relaxed);
                    pos.do_move(move, st);

                    // Perform a preliminary qsearch to verify that the move holds
                    value = -qsearch<NonPV>(pos, ss + 1, -probCutBeta, -probCutBeta + 1);

                    // If the qsearch held, perform the regular search
                    if (value >= probCutBeta)
                        value = -search<NonPV>(pos, ss + 1, -probCutBeta, -probCutBeta + 1,
                                               depth - 4, !cutNode);

                    pos.undo_move(move);

                    if (value >= probCutBeta)
                    {
                        // Save ProbCut data into transposition table
                        tte->save(posKey, value_to_tt(value, ss->ply), ss->ttPv, BOUND_LOWER,
                                  depth - 3, move, unadjustedStaticEval, tt.generation());
                        return std::abs(value) < VALUE_TB_WIN_IN_MAX_PLY
                               ? value - (probCutBeta - beta)
                               : value;
                    }
                }

            Eval::NNUE::hint_common_parent_position(pos);
        }
    }        // End early Pruning from Crystal by Shashin
moves_loop:  // When in check, search starts here

    // Step 12. A small Probcut idea, when we are in check (~4 Elo)
    probCutBeta = beta + 452;
    //from Crystal begin
    bool probCutFromCrystal =
      (((this->shashinWinProbabilityRange == SHASHIN_POSITION_HIGH_TAL)
        || (this->shashinWinProbabilityRange == SHASHIN_POSITION_CAPABLANCA)));
    if (ss->inCheck && !PvNode && ttCapture && (tte->bound() & BOUND_LOWER)
        && tte->depth() >= depth - 4 && ttValue >= probCutBeta
        && ((probCutFromCrystal
             && (!gameCycle && !kingDanger && !(ss - 1)->secondaryLine
                 && !(thisThread->nmpGuard && nullParity) && !(thisThread->nmpGuardV && nullParity)
                 && abs(ttValue) < VALUE_MAX_EVAL && abs(beta) < VALUE_MAX_EVAL))
            || (!probCutFromCrystal && std::abs(ttValue) < VALUE_TB_WIN_IN_MAX_PLY
                && std::abs(beta) < VALUE_TB_WIN_IN_MAX_PLY)))
        //from Crystal end
        return probCutBeta;

    const PieceToHistory* contHist[] = {(ss - 1)->continuationHistory,
                                        (ss - 2)->continuationHistory,
                                        (ss - 3)->continuationHistory,
                                        (ss - 4)->continuationHistory,
                                        nullptr,
                                        (ss - 6)->continuationHistory};

    Move countermove =
      prevSq != SQ_NONE ? thisThread->counterMoves[pos.piece_on(prevSq)][prevSq] : Move::none();

    MovePicker mp(pos, ttMove, depth, &thisThread->mainHistory, &thisThread->captureHistory,
                  contHist, &thisThread->pawnHistory, countermove, ss->killers);

    value            = bestValue;
    moveCountPruning = false;

    //from Crystal begin
    // Indicate PvNodes that will probably fail low if the node was searched
    // at a depth equal to or greater than the current depth, and the result
    // of this search was a fail low.
    bool lmrCapture = cutNode && (ss - 1)->moveCount > 1;

    bool kingDangerThem = ourMove && pos.king_danger(~us);

    bool lmPrunable = (!ourMove || ss->ply > 6 || (ss - 1)->moveCount > 1 || (ss - 3)->moveCount > 1
                       || (ss - 5)->moveCount > 1);

    bool allowLMR =
      depth > 1 && !gameCycle && (!kingDangerThem || ss->ply > 6) && (!PvNode || ss->ply > 1);

    bool doLMP = !PvNode && (lmPrunable || ss->ply > 2) && pos.non_pawn_material(us);
    //from Crystal end

    // Step 13. Loop through all pseudo-legal moves until no moves remain
    // or a beta cutoff occurs.
    while ((move = mp.next_move(moveCountPruning)) != Move::none())
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

        if (rootNode && is_mainthread()
            && main_manager()->tm.elapsed(threads.nodes_searched()) > 3000)
            sync_cout << "info depth " << depth << " currmove "
                      << UCI::move(move, pos.is_chess960()) << " currmovenumber "
                      << moveCount + thisThread->pvIdx << sync_endl;
        if (PvNode)
            (ss + 1)->pv = nullptr;

        extension  = 0;
        capture    = pos.capture_stage(move);
        movedPiece = pos.moved_piece(move);
        givesCheck = pos.gives_check(move);
        // from Crystal begin Yes HMT C no HT TC MHP HP

        isMate = false;

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
        if (this->shashinWinProbabilityRange == SHASHIN_POSITION_MIDDLE_HIGH_TAL)
        {
            if (givesCheck)
            {
                pos.do_move(move, st, givesCheck);
                isMate = MoveList<LEGAL>(pos).size() == 0;
                pos.undo_move(move);
            }

            if (isMate)
            {
                ss->currentMove = move;
                ss->continuationHistory =
                  &thisThread->continuationHistory[ss->inCheck][capture][movedPiece][move.to_sq()];
                value = mate_in(ss->ply + 1);

                if (PvNode && (moveCount == 1 || value > alpha))
                {
                    (ss + 1)->pv    = pv;
                    (ss + 1)->pv[0] = Move::none();
                }
            }
            else
            {
                // If we already have a mate in 1 from the current position and the current
                // move isn't a mate in 1, continue as there is no point to searching it.
                if (bestValue >= mate_in(ss->ply + 1))
                    continue;
            }
        }
        // end from Crystal
        // Calculate new depth for this move
        newDepth = depth - 1;

        int delta = beta - alpha;

        Depth r = reduction(improving, depth, moveCount, delta);
        // full threads patch begin
        if (thisThread->fullSearch)
        {
            goto skipExtensionAndPruning;
        }
        // full threads patch end

        // Step 14. Pruning at shallow depth (~120 Elo).
        // Depth conditions are important for mate finding.
        if (  // from Crystal yes HT
          (((this->shashinWinProbabilityRange != SHASHIN_POSITION_HIGH_TAL) && !rootNode
            && pos.non_pawn_material(us) && bestValue > VALUE_TB_LOSS_IN_MAX_PLY)
           || ((this->shashinWinProbabilityRange == SHASHIN_POSITION_HIGH_TAL) && doLMP
               && bestValue > VALUE_MATED_IN_MAX_PLY))
          // end from Crystal
        )
        {
            // Skip quiet moves if movecount exceeds our FutilityMoveCount threshold (~8 Elo)
            if (!moveCountPruning)
                moveCountPruning = moveCount >= futility_move_count(improving, depth);

            //from Crystal begin yes HT MHT C NO TC HP MHP
            if (lmPrunable || (this->shashinWinProbabilityRange != SHASHIN_POSITION_HIGH_TAL))
            {
                // Reduced depth of the next LMR search
                int lmrDepth = newDepth - r;

                if (capture || givesCheck)
                {
                    // Futility pruning for captures (~2 Elo)
                    if (!givesCheck && lmrDepth < 7 && !ss->inCheck)
                    {
                        Piece capturedPiece = pos.piece_on(move.to_sq());
                        int   futilityEval =
                          ss->staticEval + 277 + 292 * lmrDepth + PieceValue[capturedPiece]
                          + thisThread
                                ->captureHistory[movedPiece][move.to_sq()][type_of(capturedPiece)]
                              / 7;
                        if (futilityEval < alpha)
                            continue;
                    }

                    // SEE based pruning for captures and checks (~11 Elo)
                    if (!pos.see_ge(move, -197 * depth))
                        continue;
                }
                else
                {
                    int history =
                      (*contHist[0])[movedPiece][move.to_sq()]
                      + (*contHist[1])[movedPiece][move.to_sq()]
                      + (*contHist[3])[movedPiece][move.to_sq()]
                      + thisThread
                          ->pawnHistory[pawn_structure_index(pos)][movedPiece][move.to_sq()];

                    // Continuation history based pruning (~2 Elo)
                    if (lmrDepth < 6 && history < -4211 * depth)
                        continue;

                    history += 2 * thisThread->mainHistory[us][move.from_to()];

                    lmrDepth += history / 6437;

                    // Futility pruning: parent node (~13 Elo)
                    //begin from Crystal
                    bool futilityPruningParentNodeFromCrystal =
                      ((this->shashinWinProbabilityRange != SHASHIN_POSITION_HIGH_TAL)
                       && (this->shashinWinProbabilityRange
                           != SHASHIN_POSITION_CAPABLANCA_PETROSIAN));
                    if (!ss->inCheck
                        && ss->staticEval + (bestValue < ss->staticEval - 57 ? 144 : 57)
                               + 121 * lmrDepth
                             <= alpha
                        && ((futilityPruningParentNodeFromCrystal && lmrDepth < (6 * (1 + !ourMove))
                             && history < 20500 - 3875 * (depth - 1))
                            || ((!futilityPruningParentNodeFromCrystal) && lmrDepth < 15)))
                        continue;
                    //end from Crystal

                    lmrDepth = std::max(lmrDepth, 0);

                    // Prune moves with negative SEE (~4 Elo)
                    if (!pos.see_ge(move, -26 * lmrDepth * lmrDepth))
                        continue;
                }
            }
            //from Crystal end
        }

        // Step 15. Extensions (~100 Elo)
        // We take care to not overdo to avoid search getting stuck.
        if (ss->ply < thisThread->rootDepth * 2)
        {
            // Singular extension search (~94 Elo). If all moves but one fail low on a
            // search of (alpha-s, beta-s), and just one fails high on (alpha, beta),
            // then that move is singular and should be extended. To verify this we do
            // a reduced search on the position excluding the ttMove and if the result
            // is lower than ttValue minus a margin, then we will extend the ttMove.

            // Note: the depth margin and singularBeta margin are known for having non-linear
            // scaling. Their values are optimized to time controls of 180+1.8 and longer
            // so changing them requires tests at these types of time controls.
            // Recursive singular search is avoided.
            if (!rootNode && move == ttMove && !excludedMove
                && depth >= 4 - (thisThread->completedDepth > 30) + ss->ttPv
                && std::abs(ttValue) < VALUE_TB_WIN_IN_MAX_PLY && (tte->bound() & BOUND_LOWER)
                && tte->depth() >= depth - 3)
            {
                Value singularBeta  = ttValue - (60 + 54 * (ss->ttPv && !PvNode)) * depth / 64;
                Depth singularDepth = newDepth / 2;

                ss->excludedMove = move;
                value =
                  search<NonPV>(pos, ss, singularBeta - 1, singularBeta, singularDepth, cutNode);
                ss->excludedMove = Move::none();

                if (value < singularBeta)
                {
                    extension = 1;

                    // We make sure to limit the extensions in some way to avoid a search explosion
                    if (!PvNode && ss->multipleExtensions <= 16)
                    {
                        extension = 2 + (value < singularBeta - 78 && !ttCapture);
                        depth += depth < 16;
                    }
                }

                // Multi-cut pruning
                // Our ttMove is assumed to fail high based on the bound of the TT entry,
                // and if after excluding the ttMove with a reduced search we fail high over the original beta,
                // we assume this expected cut-node is not singular (multiple moves fail high),
                // and we can prune the whole subtree by returning a softbound.
                else if (singularBeta >= beta)
                    return singularBeta;

                // Negative extensions
                // If other moves failed high over (ttValue - margin) without the ttMove on a reduced search,
                // but we cannot do multi-cut because (ttValue - margin) is lower than the original beta,
                // we do not know if the ttMove is singular or can do a multi-cut,
                // so we reduce the ttMove in favor of other moves based on some conditions:

                // If the ttMove is assumed to fail high over current beta (~7 Elo)
                else if (ttValue >= beta)
                    extension = -2 - !PvNode;

                // If we are on a cutNode but the ttMove is not assumed to fail high over current beta (~1 Elo)
                else if (cutNode)
                    extension = -2;

                // If the ttMove is assumed to fail low over the value of the reduced search (~1 Elo)
                else if (ttValue <= value)
                    extension = -1;
            }

            // Recapture extensions (~1 Elo)
            else if (PvNode && move == ttMove && move.to_sq() == prevSq
                     && thisThread->captureHistory[movedPiece][move.to_sq()]
                                                  [type_of(pos.piece_on(move.to_sq()))]
                          > 4394)
                extension = 1;
        }

        // Add extension to new depth
        newDepth += extension;
        ss->multipleExtensions = (ss - 1)->multipleExtensions + (extension >= 2);

skipExtensionAndPruning:  // full threads search patch

        // Speculative prefetch as early as possible
        prefetch(tt.first_entry(pos.key_after(move)));

        // Update the current move (this must be done after singular extension search)
        ss->currentMove = move;
        ss->continuationHistory =
          &thisThread->continuationHistory[ss->inCheck][capture][movedPiece][move.to_sq()];

        uint64_t nodeCount = rootNode ? uint64_t(nodes) : 0;

        // Step 16. Make the move
        thisThread->nodes.fetch_add(1, std::memory_order_relaxed);
        pos.do_move(move, st, givesCheck);
        bool doLMRStep = !(thisThread->fullSearch);  // full threads patch
        // Decrease reduction if position is or has been on the PV (~7 Elo)
        if (ss->ttPv)
            r -= 1 + (ttValue > alpha) + (tte->depth() >= depth);

        // Increase reduction for cut nodes (~4 Elo)
        if (cutNode)
            r += 2 - (tte->depth() >= depth && ss->ttPv);

        // Increase reduction if ttMove is a capture (~3 Elo)
        if (ttCapture)
            r++;

        // Decrease reduction for PvNodes (~3 Elo)
        if (PvNode)
            r--;

        // Increase reduction on repetition (~1 Elo)
        if (move == (ss - 4)->currentMove && pos.has_repeated())
            r += 2;

        // Increase reduction if next ply has a lot of fail high (~5 Elo)
        if ((ss + 1)->cutoffCnt > 3)
            r++;

        // Set reduction to 0 for first picked move (ttMove) (~2 Elo)
        // Nullifies all previous reduction adjustments to ttMove and leaves only history to do them
        else if (move == ttMove)
            r = 0;

        ss->statScore = 2 * thisThread->mainHistory[us][move.from_to()]
                      + (*contHist[0])[movedPiece][move.to_sq()]
                      + (*contHist[1])[movedPiece][move.to_sq()]
                      + (*contHist[3])[movedPiece][move.to_sq()] - 4392;

        // Decrease/increase reduction for moves with a good/bad history (~8 Elo)
        r -= ss->statScore / 14189;

        // Step 17. Late moves reduction / extension (LMR, ~117 Elo)
        // We use various heuristics for the sons of a node after the first son has
        // been searched. In general we would like to reduce them, but there are many
        // cases where we extend a son if it has good chances to be "interesting".
        //from Crystal begin
        bool lmrFromCrystal = (this->shashinWinProbabilityRange == SHASHIN_POSITION_CAPABLANCA_TAL);
        if (doLMRStep && moveCount > sibs &&  //full threads patch + kelly
            ((lmrFromCrystal && (allowLMR && moveCount > 1 && (!capture || lmrCapture)))
             || ((!lmrFromCrystal) && (depth >= 2 && moveCount > 1 + rootNode))))
        //from Crystal end
        {
            // In general we want to cap the LMR depth search at newDepth, but when
            // reduction is negative, we allow this move a limited search extension
            // beyond the first move depth. This may lead to hidden multiple extensions.
            // To prevent problems when the max value is less than the min value,
            // std::clamp has been replaced by a more robust implementation.
            Depth d = std::max(1, std::min(newDepth - r, newDepth + 1));

            value = -search<NonPV>(pos, ss + 1, -(alpha + 1), -alpha, d, true);

            // Do a full-depth search when reduced LMR search fails high
            if (value > alpha && d < newDepth)
            {
                // Adjust full-depth search based on LMR results - if the result
                // was good enough search deeper, if it was bad enough search shallower.
                const bool doDeeperSearch    = value > (bestValue + 49 + 2 * newDepth);  // (~1 Elo)
                const bool doShallowerSearch = value < bestValue + newDepth;             // (~2 Elo)

                newDepth += doDeeperSearch - doShallowerSearch;

                if (newDepth > d)
                    value = -search<NonPV>(pos, ss + 1, -(alpha + 1), -alpha, newDepth, !cutNode);

                // Post LMR continuation history updates (~1 Elo)
                int bonus = value <= alpha ? -stat_malus(newDepth)
                          : value >= beta  ? stat_bonus(newDepth)
                                           : 0;

                update_continuation_histories(ss, movedPiece, move.to_sq(), bonus);
            }
        }

        // Step 18. Full-depth search when LMR is skipped
        else if (!PvNode || moveCount > 1)
        {
            // Increase reduction if ttMove is not present (~1 Elo)
            if (!ttMove)
                r += 2;

            // Note that if expected reduction is high, we reduce search depth by 1 here (~9 Elo)
            value = -search<NonPV>(pos, ss + 1, -(alpha + 1), -alpha, newDepth - (r > 3), !cutNode);
        }

        // For PV nodes only, do a full PV search on the first move or after a fail high,
        // otherwise let the parent node fail low with value <= alpha and try another move.
        if (PvNode && (moveCount == 1 || value > alpha))
        {
            (ss + 1)->pv    = pv;
            (ss + 1)->pv[0] = Move::none();

            value = -search<PV>(pos, ss + 1, -beta, -alpha, newDepth, false);
        }

        // Step 19. Undo move
        pos.undo_move(move);

        if (rootNode)
            effort[move.from_sq()][move.to_sq()] += nodes - nodeCount;

        assert(value > -VALUE_INFINITE && value < VALUE_INFINITE);

        // Step 20. Check for a new best move
        // Finished searching the move. If a stop occurred, the return value of
        // the search cannot be trusted, and we return immediately without
        // updating best move, PV and TT.
        if (threads.stop.load(std::memory_order_relaxed))
            return VALUE_ZERO;

        if (rootNode)
        {
            RootMove& rm =
              *std::find(thisThread->rootMoves.begin(), thisThread->rootMoves.end(), move);

            rm.averageScore =
              rm.averageScore != -VALUE_INFINITE ? (2 * value + rm.averageScore) / 3 : value;

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

        if (value > bestValue)
        {
            bestValue = value;

            if (value > alpha)
            {
                bestMove = move;

                if (PvNode && !rootNode)  // Update pv even in fail-high case
                    update_pv(ss->pv, move, (ss + 1)->pv);

                if (value >= beta)
                {
                    ss->cutoffCnt += 1 + !ttMove;
                    assert(value >= beta);  // Fail high
                    break;
                }
                else
                {
                    // Reduce other moves if we have found at least one score improvement (~2 Elo)
                    if (depth > 2 && depth < 13
                        && ((!gameCycle)
                            || (this->shashinWinProbabilityRange
                                != SHASHIN_POSITION_CAPABLANCA))  //from Crystal
                        && beta < 13652 && value > -12761)
                        depth -= 2;

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
                capturesSearched[captureCount++] = move;
            else
                quietsSearched[quietCount++] = move;
        }
    }

    // Step 21. Check for mate and stalemate
    // All legal moves have been searched and if there are no legal moves, it
    // must be a mate or a stalemate. If we are in a singular extension search then
    // return a fail low score.

    assert(moveCount || !ss->inCheck || excludedMove || !MoveList<LEGAL>(pos).size());

    // Adjust best value for fail high cases at non-pv nodes
    if (!PvNode && bestValue >= beta && std::abs(bestValue) < VALUE_TB_WIN_IN_MAX_PLY
        && std::abs(beta) < VALUE_TB_WIN_IN_MAX_PLY && std::abs(alpha) < VALUE_TB_WIN_IN_MAX_PLY)
        bestValue = (bestValue * (depth + 2) + beta) / (depth + 3);

    if (!moveCount)
        bestValue = excludedMove ? alpha : ss->inCheck ? mated_in(ss->ply) : VALUE_DRAW;

    // If there is a move that produces search value greater than alpha we update the stats of searched moves
    else if (bestMove)
        update_all_stats(pos, ss, *this, bestMove, bestValue, beta, prevSq, quietsSearched,
                         quietCount, capturesSearched, captureCount, depth);

    // Bonus for prior countermove that caused the fail low
    else if (!priorCapture && prevSq != SQ_NONE)
    {
        int bonus = (depth > 5) + (PvNode || cutNode) + ((ss - 1)->statScore < -15736)
                  + ((ss - 1)->moveCount > 11);
        update_continuation_histories(ss - 1, pos.piece_on(prevSq), prevSq,
                                      stat_bonus(depth) * bonus);
        thisThread->mainHistory[~us][((ss - 1)->currentMove).from_to()]
          << stat_bonus(depth) * bonus / 2;
    }

    if (PvNode
        && (this->shashinWinProbabilityRange
            != SHASHIN_POSITION_CAPABLANCA))  //From Crystal yes C No TC
        bestValue = std::min(bestValue, maxValue);

    // If no good move is found and the previous position was ttPv, then the previous
    // opponent move is probably good and the new position is added to the search tree. (~7 Elo)
    if (bestValue <= alpha)
        ss->ttPv = ss->ttPv || ((ss - 1)->ttPv && depth > 3);

    // Write gathered information in transposition table
    // Static evaluation is saved as it was before correction history
    if (!excludedMove && !(rootNode && thisThread->pvIdx))
        tte->save(posKey, value_to_tt(bestValue, ss->ply), ss->ttPv,
                  bestValue >= beta    ? BOUND_LOWER
                  : PvNode && bestMove ? BOUND_EXACT
                                       : BOUND_UPPER,
                  depth, bestMove, unadjustedStaticEval, tt.generation());

    // Adjust correction history
    if (!ss->inCheck && (!bestMove || !pos.capture(bestMove))
        && !(bestValue >= beta && bestValue <= ss->staticEval)
        && !(!bestMove && bestValue >= ss->staticEval))
    {
        auto bonus = std::clamp(int(bestValue - ss->staticEval) * depth / 8,
                                -CORRECTION_HISTORY_LIMIT / 4, CORRECTION_HISTORY_LIMIT / 4);
        thisThread->correctionHistory[us][pawn_structure_index<Correction>(pos)] << bonus;
    }

    assert(bestValue > -VALUE_INFINITE && bestValue < VALUE_INFINITE);

    return bestValue;
}


// Quiescence search function, which is called by the main search
// function with zero depth, or recursively with further decreasing depth per call.
// (~155 Elo)
template<NodeType nodeType>
Value Search::Worker::qsearch(Position& pos, Stack* ss, Value alpha, Value beta, Depth depth) {

    static_assert(nodeType != Root);
    constexpr bool PvNode = nodeType == PV;

    assert(alpha >= -VALUE_INFINITE && alpha < beta && beta <= VALUE_INFINITE);
    assert(PvNode || (alpha == beta - 1));
    assert(depth <= 0);

    // Check if we have an upcoming move that draws by repetition, or if
    // the opponent had an alternative move earlier to this position. (~1 Elo)
    if (alpha < VALUE_DRAW && pos.has_game_cycle(ss->ply))
    {
        alpha = value_draw(this->nodes);
        if (alpha >= beta)
            return alpha;
    }

    Move      pv[MAX_PLY + 1];
    StateInfo st;
    ASSERT_ALIGNED(&st, Eval::NNUE::CacheLineSize);

    TTEntry* tte;
    Key      posKey;
    Move     ttMove, move, bestMove;
    Depth    ttDepth;
    Value    bestValue, value, ttValue, futilityValue, futilityBase;
    bool     pvHit, givesCheck, capture, gameCycle = false;  // from Crystal
    int      moveCount;
    Color    us = pos.side_to_move();

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
    if (pos.has_game_cycle(ss->ply))
    {
        gameCycle = true;
    }
    // from Crystal end

    // Step 2. Check for an immediate draw or maximum ply reached
    if (pos.is_draw(ss->ply) || ss->ply >= MAX_PLY)
        return (ss->ply >= MAX_PLY && !ss->inCheck) ? evaluate(pos, thisThread->optimism[us])
                                                    : VALUE_DRAW;

    assert(0 <= ss->ply && ss->ply < MAX_PLY);

    // Decide the replacement and cutoff priority of the qsearch TT entries
    ttDepth = ss->inCheck || depth >= DEPTH_QS_CHECKS ? DEPTH_QS_CHECKS : DEPTH_QS_NO_CHECKS;

    // Step 3. Transposition table lookup
    posKey  = pos.key();
    tte     = tt.probe(posKey, ss->ttHit);
    ttValue = ss->ttHit ? value_from_tt(tte->value(), ss->ply, pos.rule50_count()) : VALUE_NONE;
    ttMove  = ss->ttHit ? tte->move() : Move::none();
    pvHit   = ss->ttHit && tte->is_pv();

    // At non-PV nodes we check for an early TT cutoff
    //from Crystal begin
    bool earlyTTCutOffFromCrystal = (this->shashinWinProbabilityRange == SHASHIN_POSITION_HIGH_TAL);
    if (!PvNode && tte->depth() >= ttDepth
        && ttValue != VALUE_NONE  // Only in case of TT access race or if !ttHit
        && ((!earlyTTCutOffFromCrystal) || (ss->ttHit && !gameCycle))
        && (tte->bound() & (ttValue >= beta ? BOUND_LOWER : BOUND_UPPER)))
        return ttValue;
    //from Crystal end

    // Step 4. Static evaluation of the position
    Value unadjustedStaticEval = VALUE_NONE;
    if (ss->inCheck)
        bestValue = futilityBase = -VALUE_INFINITE;
    else
    {
        if (ss->ttHit)
        {
            // Never assume anything about values stored in TT
            unadjustedStaticEval = tte->eval();
            if (unadjustedStaticEval == VALUE_NONE)
                unadjustedStaticEval = evaluate(pos, thisThread->optimism[us]);
            ss->staticEval = bestValue =
              to_corrected_static_eval(unadjustedStaticEval, *thisThread, pos);

            // ttValue can be used as a better position evaluation (~13 Elo)
            if (ttValue != VALUE_NONE
                && (tte->bound() & (ttValue > bestValue ? BOUND_LOWER : BOUND_UPPER)))
                bestValue = ttValue;
        }
        else
        {
            // In case of null move search, use previous static eval with a different sign
            unadjustedStaticEval = (ss - 1)->currentMove != Move::null()
                                   ? evaluate(pos, thisThread->optimism[us])
                                   : -(ss - 1)->staticEval;
            ss->staticEval       = bestValue =
              to_corrected_static_eval(unadjustedStaticEval, *thisThread, pos);
        }

        // Stand pat. Return immediately if static value is at least beta
        if (bestValue >= beta)
        {
            if (!ss->ttHit)
                tte->save(posKey, value_to_tt(bestValue, ss->ply), false, BOUND_LOWER, DEPTH_NONE,
                          Move::none(), unadjustedStaticEval, tt.generation());

            return bestValue;
        }

        if (bestValue > alpha)
            alpha = bestValue;

        futilityBase = ss->staticEval + 206;
    }

    const PieceToHistory* contHist[] = {(ss - 1)->continuationHistory,
                                        (ss - 2)->continuationHistory};

    // Initialize a MovePicker object for the current position, and prepare
    // to search the moves. Because the depth is <= 0 here, only captures,
    // queen promotions, and other checks (only if depth >= DEPTH_QS_CHECKS)
    // will be generated.
    Square     prevSq = ((ss - 1)->currentMove).is_ok() ? ((ss - 1)->currentMove).to_sq() : SQ_NONE;
    MovePicker mp(pos, ttMove, depth, &thisThread->mainHistory, &thisThread->captureHistory,
                  contHist, &thisThread->pawnHistory);

    int quietCheckEvasions = 0;

    // Step 5. Loop through all pseudo-legal moves until no moves remain
    // or a beta cutoff occurs.
    while ((move = mp.next_move()) != Move::none())
    {
        assert(move.is_ok());

        // Check for legality
        if (!pos.legal(move))
            continue;

        givesCheck = pos.gives_check(move);
        capture    = pos.capture_stage(move);

        moveCount++;

        // Step 6. Pruning
        if (bestValue > VALUE_TB_LOSS_IN_MAX_PLY && pos.non_pawn_material(us))
        {
            // Futility pruning and moveCount pruning (~10 Elo)
            if (!givesCheck && move.to_sq() != prevSq && futilityBase > VALUE_TB_LOSS_IN_MAX_PLY
                && move.type_of() != PROMOTION)
            {
                if (moveCount > 2
                                  + ((this->shashinWinProbabilityRange != SHASHIN_POSITION_HIGH_TAL)
                                       ? 0
                                       : PvNode))  //from Crystal yes HT CP no LT MHP HP
                    continue;

                futilityValue = futilityBase + PieceValue[pos.piece_on(move.to_sq())];

                // If static eval + value of piece we are going to capture is much lower
                // than alpha we can prune this move. (~2 Elo)
                if (futilityValue <= alpha)
                {
                    bestValue = std::max(bestValue, futilityValue);
                    continue;
                }

                // If static eval is much lower than alpha and move is not winning material
                // we can prune this move. (~2 Elo)
                if (futilityBase <= alpha && !pos.see_ge(move, 1))
                {
                    bestValue = std::max(bestValue, futilityBase);
                    continue;
                }

                // If static exchange evaluation is much worse than what is needed to not
                // fall below alpha we can prune this move.
                if (futilityBase > alpha && !pos.see_ge(move, (alpha - futilityBase) * 4))
                {
                    bestValue = alpha;
                    continue;
                }
            }

            // We prune after the second quiet check evasion move, where being 'in check' is
            // implicitly checked through the counter, and being a 'quiet move' apart from
            // being a tt move is assumed after an increment because captures are pushed ahead.
            if (quietCheckEvasions > 1)
                break;

            // Continuation history based pruning (~3 Elo)
            if (!capture && (*contHist[0])[pos.moved_piece(move)][move.to_sq()] < 0
                && (*contHist[1])[pos.moved_piece(move)][move.to_sq()] < 0)
                continue;

            // Do not search moves with bad enough SEE values (~5 Elo)
            if (!pos.see_ge(move, -74))
                continue;
        }

        // Speculative prefetch as early as possible
        prefetch(tt.first_entry(pos.key_after(move)));

        // Update the current move
        ss->currentMove = move;
        ss->continuationHistory =
          &thisThread
             ->continuationHistory[ss->inCheck][capture][pos.moved_piece(move)][move.to_sq()];

        quietCheckEvasions += !capture && ss->inCheck;

        // Step 7. Make and search the move
        thisThread->nodes.fetch_add(1, std::memory_order_relaxed);
        pos.do_move(move, st, givesCheck);
        value = -qsearch<nodeType>(pos, ss + 1, -beta, -alpha, depth - 1);
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
    // from Hypnos adapted
    if (openingVariety)
    {
        const auto normalizedVariety = openingVariety * NormalizeToPawnValue / 100;

        if (bestValue + normalizedVariety >= 0 && pos.count<PAWN>() > 12)
        {
            // Range for openingVariety bonus
            const auto openingVarietyMinRange = thisThread->nodes / 2;
            const auto openingVarietyMaxRange = thisThread->nodes * 2;
            static PRNG rng(now());
            bestValue +=
              static_cast<Value>(rng.rand<std::uint64_t>() % (openingVarietyMaxRange - openingVarietyMinRange + 1)
                                 + openingVarietyMinRange)
              % (openingVariety + 1);
        }
    }
    // end from Hypnos adapted
    // Step 9. Check for mate
    // All legal moves have been searched. A special case: if we're in check
    // and no legal moves were found, it is checkmate.
    if (ss->inCheck && bestValue == -VALUE_INFINITE)
    {
        assert(!MoveList<LEGAL>(pos).size());
        return mated_in(ss->ply);  // Plies to mate from the root
    }

    if (std::abs(bestValue) < VALUE_TB_WIN_IN_MAX_PLY && bestValue >= beta)
        bestValue = (3 * bestValue + beta) / 4;

    // Save gathered info in transposition table
    // Static evaluation is saved as it was before adjustment by correction history
    tte->save(posKey, value_to_tt(bestValue, ss->ply), pvHit,
              bestValue >= beta ? BOUND_LOWER : BOUND_UPPER, ttDepth, bestMove,
              unadjustedStaticEval, tt.generation());

    assert(bestValue > -VALUE_INFINITE && bestValue < VALUE_INFINITE);

    return bestValue;
}

Depth Search::Worker::reduction(bool i, Depth d, int mn, int delta) {
    int reductionScale = reductions[d] * reductions[mn];
    return (reductionScale + 1118 - delta * 793 / rootDelta) / 1024 + (!i && reductionScale > 863);
}

namespace {
// Adjusts a mate or TB score from "plies to mate from the root"
// to "plies to mate from the current position". Standard scores are unchanged.
// The function is called before storing a value in the transposition table.
Value value_to_tt(Value v, int ply) {

    assert(v != VALUE_NONE);
    return v >= VALUE_TB_WIN_IN_MAX_PLY ? v + ply : v <= VALUE_TB_LOSS_IN_MAX_PLY ? v - ply : v;
}


// Inverse of value_to_tt(): it adjusts a mate or TB score
// from the transposition table (which refers to the plies to mate/be mated from
// current position) to "plies to mate/be mated (TB win/loss) from the root".
// However, to avoid potentially false mate or TB scores related to the 50 moves rule
// and the graph history interaction, we return the highest non-TB score instead.
Value value_from_tt(Value v, int ply, int r50c) {

    if (v == VALUE_NONE)
        return VALUE_NONE;

    // handle TB win or better
    if (v >= VALUE_TB_WIN_IN_MAX_PLY)
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
    if (v <= VALUE_TB_LOSS_IN_MAX_PLY)
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
                      Value           bestValue,
                      Value           beta,
                      Square          prevSq,
                      Move*           quietsSearched,
                      int             quietCount,
                      Move*           capturesSearched,
                      int             captureCount,
                      Depth           depth) {

    Color                  us             = pos.side_to_move();
    CapturePieceToHistory& captureHistory = workerThread.captureHistory;
    Piece                  moved_piece    = pos.moved_piece(bestMove);
    PieceType              captured;

    int quietMoveBonus = stat_bonus(depth + 1);
    int quietMoveMalus = stat_malus(depth);

    if (!pos.capture_stage(bestMove))
    {
        int bestMoveBonus = bestValue > beta + 166 ? quietMoveBonus      // larger bonus
                                                   : stat_bonus(depth);  // smaller bonus

        // Increase stats for the best move in case it was a quiet move
        update_quiet_stats(pos, ss, workerThread, bestMove, bestMoveBonus);

        int pIndex = pawn_structure_index(pos);
        workerThread.pawnHistory[pIndex][moved_piece][bestMove.to_sq()] << quietMoveBonus;

        // Decrease stats for all non-best quiet moves
        for (int i = 0; i < quietCount; ++i)
        {
            workerThread
                .pawnHistory[pIndex][pos.moved_piece(quietsSearched[i])][quietsSearched[i].to_sq()]
              << -quietMoveMalus;

            workerThread.mainHistory[us][quietsSearched[i].from_to()] << -quietMoveMalus;
            update_continuation_histories(ss, pos.moved_piece(quietsSearched[i]),
                                          quietsSearched[i].to_sq(), -quietMoveMalus);
        }
    }
    else
    {
        // Increase stats for the best move in case it was a capture move
        captured = type_of(pos.piece_on(bestMove.to_sq()));
        captureHistory[moved_piece][bestMove.to_sq()][captured] << quietMoveBonus;
    }

    // Extra penalty for a quiet early move that was not a TT move or
    // main killer move in previous ply when it gets refuted.
    if (prevSq != SQ_NONE
        && ((ss - 1)->moveCount == 1 + (ss - 1)->ttHit
            || ((ss - 1)->currentMove == (ss - 1)->killers[0]))
        && !pos.captured_piece())
        update_continuation_histories(ss - 1, pos.piece_on(prevSq), prevSq, -quietMoveMalus);

    // Decrease stats for all non-best capture moves
    for (int i = 0; i < captureCount; ++i)
    {
        moved_piece = pos.moved_piece(capturesSearched[i]);
        captured    = type_of(pos.piece_on(capturesSearched[i].to_sq()));
        captureHistory[moved_piece][capturesSearched[i].to_sq()][captured] << -quietMoveMalus;
    }
}


// Updates histories of the move pairs formed
// by moves at ply -1, -2, -3, -4, and -6 with current move.
void update_continuation_histories(Stack* ss, Piece pc, Square to, int bonus) {

    for (int i : {1, 2, 3, 4, 6})
    {
        // Only update the first 2 continuation histories if we are in check
        if (ss->inCheck && i > 2)
            break;
        if (((ss - i)->currentMove).is_ok())
            (*(ss - i)->continuationHistory)[pc][to] << bonus / (1 + 3 * (i == 3));
    }
}


// Updates move sorting heuristics
void update_quiet_stats(
  const Position& pos, Stack* ss, Search::Worker& workerThread, Move move, int bonus) {

    // Update killers
    if (ss->killers[0] != move)
    {
        ss->killers[1] = ss->killers[0];
        ss->killers[0] = move;
    }

    Color us = pos.side_to_move();
    workerThread.mainHistory[us][move.from_to()] << bonus;
    update_continuation_histories(ss, pos.moved_piece(move), move.to_sq(), bonus);

    // Update countermove history
    if (((ss - 1)->currentMove).is_ok())
    {
        Square prevSq                                           = ((ss - 1)->currentMove).to_sq();
        workerThread.counterMoves[pos.piece_on(prevSq)][prevSq] = move;
    }
}
}

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


// Used to print debug info and, more importantly,
// to detect when we are out of available time and thus stop the search.
void SearchManager::check_time(Search::Worker& worker) {
    if (--callsCnt > 0)
        return;

    // When using nodes, ensure checking rate is not lower than 0.1% of nodes
    callsCnt = worker.limits.nodes ? std::min(512, int(worker.limits.nodes / 1024)) : 512;

    static TimePoint lastInfoTime = now();

    TimePoint elapsed = tm.elapsed(worker.threads.nodes_searched());
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

std::string SearchManager::pv(Search::Worker&           worker,  //from ShashChess
                              const ThreadPool&         threads,
                              const TranspositionTable& tt,
                              Depth                     depth) const {
    std::stringstream ss;

    const auto  nodes     = threads.nodes_searched();
    const auto& rootMoves = worker.rootMoves;
    const auto& pos       = worker.rootPos;
    size_t      pvIdx     = worker.pvIdx;
    TimePoint   time      = tm.elapsed(nodes) + 1;
    size_t      multiPV   = std::min(size_t(worker.options["MultiPV"]), rootMoves.size());
    uint64_t    tbHits    = threads.tb_hits() + (worker.tbConfig.rootInTB ? rootMoves.size() : 0);

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

        if (ss.rdbuf()->in_avail())  // Not at first line
            ss << "\n";

        ss << "info"
           << " depth " << d << " seldepth " << rootMoves[i].selDepth << " multipv " << i + 1
           << " score " << UCI::value(v);
        updateShashinValues((Value) v, pos.game_ply(), &worker);  //by shashin
        if (worker.options["UCI_ShowWDL"])
            ss << UCI::wdl(v, pos.game_ply());

        if (i == pvIdx && !tb && updated)  // tablebase- and previous-scores are exact
            ss << (rootMoves[i].scoreLowerbound
                     ? " lowerbound"
                     : (rootMoves[i].scoreUpperbound ? " upperbound" : ""));

        ss << " nodes " << nodes << " nps " << nodes * 1000 / time << " hashfull " << tt.hashfull()
           << " tbhits " << tbHits << " time " << time << " pv";

        for (Move m : rootMoves[i].pv)
            ss << " " << UCI::move(m, pos.is_chess960());
    }

    return ss.str();
}

// Called in case we have no ponder move before exiting the search,
// for instance, in case we stop the search during a fail high at root.
// We try hard to have a ponder move to return to the GUI,
// otherwise in case of 'ponder on' we have nothing to think about.
bool RootMove::extract_ponder_from_tt(const TranspositionTable& tt, Position& pos) {

    StateInfo st;
    ASSERT_ALIGNED(&st, Eval::NNUE::CacheLineSize);

    bool ttHit;

    assert(pv.size() == 1);
    if (pv[0] == Move::none())
        return false;

    pos.do_move(pv[0], st);
    TTEntry* tte = tt.probe(pos.key(), ttHit);

    if (ttHit)
    {
        Move m = tte->move();  // Local copy to be SMP safe
        if (MoveList<LEGAL>(pos).contains(m))
            pv.push_back(m);
    }

    pos.undo_move(pv[0]);
    return pv.size() > 1;
}


// Kelly begin
void putGameLineIntoLearningTable() {
    double learning_rate = 0.5;
    double gamma         = 0.99;

    if (gameLine.size() > 1)
    {
        for (size_t index = gameLine.size() - 1; index > 0; index--)
        {
            int currentScore = gameLine[index - 1].learningMove.score * 100 / NormalizeToPawnValue;
            int nextScore    = gameLine[index].learningMove.score * 100 / NormalizeToPawnValue;

            currentScore = currentScore * (1 - learning_rate) + learning_rate * (gamma * nextScore);

            gameLine[index - 1].learningMove.score =
              currentScore * (Value) (NormalizeToPawnValue) / 100;

            LD.add_new_learning(gameLine[index - 1].key, gameLine[index - 1].learningMove);
        }

        gameLine.clear();
    }
}

void setStartPoint() {
    useLearning = true;
    LD.resume();
}
// Kelly end

}  // namespace ShashChess