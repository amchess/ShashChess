/*
  ShashChess, a UCI chess playing engine derived from Stockfish
  Copyright (C) 2004-2022 The Stockfish developers (see AUTHORS file)

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

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>   // For std::memset
#include <iostream>
#include <sstream>
#include <random>  //opening variety sugar
#include <fstream> // kelly
#include "polybook.h"//from BrainFish
#include "evaluate.h"
#include "misc.h"
#include "mcts/montecarlo.h" //Montecarlo
#include "movegen.h"
#include "movepick.h"
#include "position.h"
#include "search.h"
#include "thread.h"
#include "timeman.h"
#include "tt.h"
#include "learn.h"
#include "uci.h"
#include "syzygy/tbprobe.h"

namespace Stockfish {
//livebook begin
#define CURL_STATICLIB
extern "C" {
#include <curl/curl.h>
}
#undef min
#undef max
//livebook end
bool pawnsToEvaluate, winnableToEvaluate, imbalancesToEvaluate; //from Handicap mode
//Kelly begin
bool useLearning = true;
bool enabledLearningProbe=false;
std::vector<PersistedLearningMove> gameLine;
//Kelly end
bool goldDigger = false; //from ShashChess

namespace Search {

  LimitsType Limits;
  int uciElo, depthLimit;//from handicap mode
  //from Shashin
  bool highTal,mediumTal, lowTal, capablanca,highPetrosian,mediumPetrosian,lowPetrosian;
  //end from Shashin
}

namespace Tablebases {

  int Cardinality;
  bool RootInTB;
  bool UseRule50;
  Depth ProbeDepth;
}

namespace TB = Tablebases;

using std::string;
using Eval::evaluate;
using namespace Search;

namespace {

  //s20red4c4_t4 begin
  int cpx_margin[7] = { 3214, 	1230, 428, 563, 946, 3501, 217};
  int cpx_mult[7] = { 0, 0, 0, 1, 0 ,2, 2 };
  int s20const[7] = { 0, 0, 0, 0, 1 ,2, 2 };
  //s20red4c4_t4 end
  
  //nodeTypeHistory

  // Futility margin
  Value futility_margin(Depth d, bool improving) {
    return Value(168 * d - 250 * improving);//futmAsy1
  }
  //int skillLevel;//from true handicap mode

  // Reductions lookup table, initialized at startup
  int Reductions[MAX_MOVES]; // [depth or moveNumber]

  Depth reduction(bool i, Depth d, int mn, Value delta, Value rootDelta) {
    int r = Reductions[d] * Reductions[mn];
    if (rootDelta != 0)
		return (r + 1463 - int(delta) * 1024 / int(rootDelta)) / 1024 + (!i && r > 1010);
    else  // avoid divide by zero error
        return (r + 1463 - int(delta) * 1024) / 1024 + (!i && r > 1010);
  }

  constexpr int futility_move_count(bool improving, Depth depth) {
    return improving ? (3 + depth * depth)
                     : (3 + depth * depth) / 2;
  }

  // History and stats update bonus, based on depth
  int stat_bonus(Depth d) {
    return std::min((8 * d + 240) * d - 276 , 1907);
  }

  // Add a small random component to draw evaluations to avoid 3-fold blindness
  Value value_draw(const Thread* thisThread) {
    return VALUE_DRAW - 1 + Value(thisThread->nodes & 0x2);
  }

  // Skill structure is used to implement strength limit. If we have an uci_elo then
  // we convert it to a suitable fractional skill level using anchoring to CCRL Elo
  // (goldfish 1.13 = 2000) and a fit through Ordo derived Elo for match (TC 60+0.6)
  // results spanning a wide range of k values.
//from true handicap mode begin  
/*  struct Skill {
	Skill(int skill_level, int uci_elo) {
        if (uci_elo)
            level = std::clamp(std::pow((uci_elo - 1346.6) / 143.4, 1 / 0.806), 0.0, 20.0);
        else
            level = double(skill_level);
    }
    bool enabled() const { return level < 20.0; }
    bool time_to_pick(Depth depth) const { return depth == 1 + int(level); } 
    Move pick_best(size_t multiPV);

    double level;
    Move best = MOVE_NONE;
  };*/
  //from true handicap mode end
  bool limitStrength ;//from handicap mode
  int openingVariety;//from Sugar

  template <NodeType nodeType>
  Value search(Position& pos, Stack* ss, Value alpha, Value beta, Depth depth, bool cutNode, bool mcts);//mcts

  template <NodeType nodeType>
  Value qsearch(Position& pos, Stack* ss, Value alpha, Value beta, Depth depth = 0);

  Value value_to_tt(Value v, int ply);
  Value value_from_tt(Value v, int ply, int r50c);
  void update_pv(Move* pv, Move move, const Move* childPv);
  void update_continuation_histories(Stack* ss, Piece pc, Square to, int bonus);
  void update_quiet_stats(const Position& pos, Stack* ss, Move move, int bonus);
  void update_all_stats(const Position& pos, Stack* ss, Move bestMove, Value bestValue, Value beta, Square prevSq,
                        Move* quietsSearched, int quietCount, Move* capturesSearched, int captureCount, Depth depth);

  // perft() is our utility to verify move generation. All the leaf nodes up
  // to the given depth are generated and counted, and the sum is returned.
  template<bool Root>
  uint64_t perft(Position& pos, Depth depth) {

    StateInfo st;
    ASSERT_ALIGNED(&st, Eval::NNUE::CacheLineSize);

    uint64_t cnt, nodes = 0;
    const bool leaf = (depth == 2);

    for (const auto& m : MoveList<LEGAL>(pos))
    {
        if (Root && depth <= 1)
            cnt = 1, nodes++;
        else
        {
            pos.do_move(m, st);
            cnt = leaf ? MoveList<LEGAL>(pos).size() : perft<false>(pos, depth - 1);
            nodes += cnt;
            pos.undo_move(m);
        }
        if (Root)
            sync_cout << UCI::move(m, pos.is_chess960()) << ": " << cnt << sync_endl;
    }
    return nodes;
  }

} // namespace


/// Search::init() is called at startup to initialize various lookup tables
//livebook begin
CURL *g_cURL;
std::string g_szRecv;
std::string g_livebookURL = "http://www.chessdb.cn/cdb.php";
int g_inBook;
int livebook_depth_count=0;
int max_book_depth;

size_t cURL_WriteFunc(void *contents, size_t size, size_t nmemb, std::string *s)
{
  size_t newLength = size * nmemb;
  try
  {
    s->append((char*)contents, newLength);
  }
  catch (std::bad_alloc &)
  {
    //handle memory problem
    return 0;
  }
  return newLength;
}
void Search::setLiveBookURL(const std::string &newURL) {
	g_livebookURL = newURL;
}
void Search::setLiveBookTimeout(size_t newTimeoutMS) {
	curl_easy_setopt(g_cURL, CURLOPT_TIMEOUT_MS, newTimeoutMS);
}
void Search::set_livebook_retry(int retry) {
    g_inBook = retry;
}
void Search::set_livebook_depth(int book_depth) {
    max_book_depth = book_depth;
}
//livebook end
void Search::init() {

  for (int i = 1; i < MAX_MOVES; ++i)
      Reductions[i] = int((20.81 + std::log(Threads.size()) / 2) * std::log(i));

  //livebook begin
  curl_global_init(CURL_GLOBAL_DEFAULT);
  g_cURL = curl_easy_init();
  curl_easy_setopt(g_cURL, CURLOPT_TIMEOUT_MS, 1000L);
  curl_easy_setopt(g_cURL, CURLOPT_WRITEFUNCTION, cURL_WriteFunc);
  curl_easy_setopt(g_cURL, CURLOPT_WRITEDATA, &g_szRecv);
  set_livebook_retry((int)Options["Live Book Retry"]);
  set_livebook_depth((int)Options["Live Book Depth"]);
  //livebook end
}
/// Search::clear() resets search state to its initial value

void Search::clear() {

  Threads.main()->wait_for_search_finished();
  //livebook begin
  set_livebook_retry((int)Options["Live Book Retry"]);
  set_livebook_depth((int)Options["Live Book Depth"]);
  //livebook end
  Time.availableNodes = 0;
  TT.clear();
  MCTS.clear();//mcts
  Threads.clear();
  Tablebases::init(Options["SyzygyPath"]); // Free mapped files
}

//handicapMode begin
inline int getHandicapDepth(int elo){
    if(elo<=1350)
    {
        return (int)(3*elo/1350 +1 );
    }
    if(elo<=1999)
    {
        return (int)((2*elo-104)/649 );
    }    
    if(elo<=2199)
    {
        return (int)((2*elo-2607)/199);
    }
    if(elo<=2399)
    {
        return (int)((2*elo-2410)/199);
    }
    return (int)((7*elo-10950)/450);
}
//handicapMode end
/// MainThread::search() is started when the program receives the UCI 'go'
/// command. It searches from the root position and outputs the "bestmove".

void MainThread::search() {
  goldDigger = Options["GoldDigger"];//from ShashChess
  if (Limits.perft)
  {
      nodes = perft<true>(rootPos, Limits.perft);
      sync_cout << "\nNodes searched: " << nodes << "\n" << sync_endl;
      return;
  }
  //from Sugar
  limitStrength = Options["UCI_LimitStrength"] || Options["LimitStrength_CB"] ;
  //end from Sugar

  Color us = rootPos.side_to_move();
  Time.init(Limits, us, rootPos.game_ply());
  TT.new_search();
  //Kelly begin
  enabledLearningProbe = false;
  useLearning = true;
  //Kelly end

  openingVariety = Options["Opening variety"];//from Sugar
  Eval::NNUE::verify();
  //from Shashin
  highTal=Options["High Tal"];
  mediumTal=Options["Medium Tal"];
  lowTal=Options["Low Tal"];
  capablanca=Options["Capablanca"];
  highPetrosian=Options["High Petrosian"];
  mediumPetrosian=Options["Medium Petrosian"];
  lowPetrosian=Options["Low Petrosian"];
  //end from Shashin
  //from handicap mode begin
  uciElo=Options["UCI_LimitStrength"] ? Options["UCI_Elo"]:Options["ELO_CB"];
  depthLimit=limitStrength && Options["Handicapped Depth"] ? getHandicapDepth(uciElo):Limits.depth;//handicap mode
  pawnsToEvaluate = limitStrength ? (uciElo >= 2000):1;
  winnableToEvaluate= limitStrength ? (uciElo>=2200):1;
  imbalancesToEvaluate = limitStrength ? (uciElo>=2400):1;
  //skillLevel= ((int)((uciElo-1350)/75)); //from true handicap mode
  //end from handicap mode
  Move bookMove = MOVE_NONE;

  if (rootMoves.empty())
  {
      rootMoves.emplace_back(MOVE_NONE);
      sync_cout << "info depth 0 score "
                << UCI::value(rootPos.checkers() ? -VALUE_MATE : VALUE_DRAW)
                << sync_endl;
  }
  //Books management begin
  else
  {
      if (!Limits.infinite && !Limits.mate)
      {
         //Check polyglot books first
          if ((bool)Options["Book1"] && rootPos.game_ply() / 2 < (int)Options["Book1 Depth"])
              bookMove = polybook[0].probe(rootPos, (bool)Options["Book1 BestBookMove"]);

          if(bookMove == MOVE_NONE && (bool)Options["Book2"] && rootPos.game_ply() / 2 < (int)Options["Book2 Depth"])
              bookMove = polybook[1].probe(rootPos, (bool)Options["Book1 BestBookMove"]);
		  if(!bookMove)
		  {
		    //Live Book begin
		    if (Options["Live Book"] && g_inBook)
		    {
		      if(rootPos.game_ply()==0)
			livebook_depth_count=0;
		      if (livebook_depth_count < max_book_depth)
		      {
			CURLcode res;
			char *szFen = curl_easy_escape(g_cURL, rootPos.fen().c_str(), 0);
			std::string szURL = g_livebookURL + "?action=" + (Options["Live Book Diversity"] ? "query" : "querybest") + "&board=" + szFen;
			curl_free(szFen);
			curl_easy_setopt(g_cURL, CURLOPT_URL, szURL.c_str());
			g_szRecv.clear();
			res = curl_easy_perform(g_cURL);
			if (res == CURLE_OK)
			{
			  g_szRecv.erase(std::find(g_szRecv.begin(), g_szRecv.end(), '\0'), g_szRecv.end());
			  if (g_szRecv.find("move:") != std::string::npos)
			  {
			    std::string tmp = g_szRecv.substr(5);
			    bookMove = UCI::to_move(rootPos, tmp);
			    livebook_depth_count++;
			  }
			}
		      }
		    }
		    //Live Book end
		  }
	      }
	      if (bookMove && std::count(rootMoves.begin(), rootMoves.end(), bookMove))
	      {
		  g_inBook = Options["Live Book Retry"];
	
		  for (Thread* th : Threads)
		    std::swap(th->rootMoves[0], *std::find(th->rootMoves.begin(), th->rootMoves.end(), bookMove));
	      }
	      else
	      {
		  bookMove = MOVE_NONE;
		  g_inBook--;
	      }
	      if (!bookMove)
	      {
			Threads.start_searching(); // start non-main threads
			Thread::search();          // main thread start searching
	      }
  }
  //Books management end

  // When we reach the maximum depth, we can arrive here without a raise of
  // Threads.stop. However, if we are pondering or in an infinite search,
  // the UCI protocol states that we shouldn't print the best move before the
  // GUI sends a "stop" or "ponderhit" command. We therefore simply wait here
  // until the GUI sends one of those commands.

  while (!Threads.stop && (ponder || Limits.infinite))
  {} // Busy wait for a stop or a ponder reset

  // Stop the threads if not already stopped (also raise the stop if
  // "ponderhit" just reset Threads.ponder).
  Threads.stop = true;

  // Wait until all threads have finished
  Threads.wait_for_search_finished();

  // When playing in 'nodes as time' mode, subtract the searched nodes from
  // the available ones before exiting.
  if (Limits.npmsec)
      Time.availableNodes += Limits.inc[us] - Threads.nodes_searched();

  Thread* bestThread = this;
  //Skill skill = Skill(skillLevel, (limitStrength) ? uciElo : 0); //from true handicap mode

  if (   int(Options["MultiPV"]) == 1
      && !depthLimit
	  //&& !skill.enabled() //from true handicap mode
      && rootMoves[0].pv[0] != MOVE_NONE)
      bestThread = Threads.get_best_thread();

  bestPreviousScore = bestThread->rootMoves[0].score;
  bestPreviousAverageScore = bestThread->rootMoves[0].averageScore;

  for (Thread* th : Threads)
    th->previousDepth = bestThread->completedDepth;

  //kelly begin
  if (bestThread->completedDepth > 4 && LD.is_enabled() && !LD.is_paused())// from Khalid
  {
      PersistedLearningMove plm;
      plm.key = rootPos.key();
      plm.learningMove.depth = bestThread->completedDepth;
      plm.learningMove.move = bestThread->rootMoves[0].pv[0];
      plm.learningMove.score = goldDigger? (bestThread->rootMoves[0].score):((Value)((float)(bestThread->rootMoves[0].score) / WEIGHTED_EVAL));

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
  //Kelly end
  
  // Send again PV info if we have a new best thread
  if (bestThread != this)
      sync_cout << UCI::pv(bestThread->rootPos, bestThread->completedDepth, -VALUE_INFINITE, VALUE_INFINITE) << sync_endl;

  sync_cout << "bestmove " << UCI::move(bestThread->rootMoves[0].pv[0], rootPos.is_chess960());

  if (bestThread->rootMoves[0].pv.size() > 1 || bestThread->rootMoves[0].extract_ponder_from_tt(rootPos))
      std::cout << " ponder " << UCI::move(bestThread->rootMoves[0].pv[1], rootPos.is_chess960());

  std::cout << sync_endl;

  //from Khalid begin
  //Save learning data if game is already decided
  if (Utility::is_game_decided(rootPos, (goldDigger? (bestThread->rootMoves[0].score):((Value)((float)(bestThread->rootMoves[0].score)/ WEIGHTED_EVAL)))) && LD.is_enabled() && !LD.is_paused())
  {
      //Perform Q-learning if enabled
      if(LD.learning_mode() == LearningMode::Self)
          putGameLineIntoLearningTable();

      //Save to learning file
	  if(!LD.is_readonly())
	  {
	  	LD.persist();
	  }

      //Stop learning until we receive *ucinewgame* command
      LD.pause();
  }
  //from Khalid end

  //livebook begin
  if (Options["Live Book"] && Options["Live Book Contribute"] && !g_inBook)
  {
    char *szFen = curl_easy_escape(g_cURL, rootPos.fen().c_str(), 0);
    std::string szURL = g_livebookURL + "?action=store" + "&board=" + szFen + "&move=move:" + UCI::move(bestThread->rootMoves[0].pv[0], rootPos.is_chess960());
    curl_free(szFen);
    curl_easy_setopt(g_cURL, CURLOPT_URL, szURL.c_str());
    curl_easy_perform(g_cURL);
  }
  //livebook end
}

//from Shashin
inline int getShashinValue(Value score) {
  if(!goldDigger)
  {
    score = (Value)((float)(score) / WEIGHTED_EVAL);
  }
  
  if ((int)score < -SHASHIN_TAL_THRESHOLD) {
	  return SHASHIN_POSITION_PETROSIAN;
  }
  if (((int)score >= -SHASHIN_TAL_THRESHOLD)
		  && ((int)score <= -SHASHIN_CAPABLANCA_THRESHOLD)) {
	  return SHASHIN_POSITION_CAPABLANCA_PETROSIAN;
  }
  if (((int)score < SHASHIN_CAPABLANCA_THRESHOLD)) {
	  return SHASHIN_POSITION_CAPABLANCA;
  }
  if (((int)score >= SHASHIN_CAPABLANCA_THRESHOLD)
		  && ((int)score <= SHASHIN_TAL_THRESHOLD)) {
	  return SHASHIN_POSITION_TAL_CAPABLANCA;
  }
  if ((int)score > SHASHIN_TAL_THRESHOLD) {
	  return SHASHIN_POSITION_TAL;
  }
  return SHASHIN_POSITION_TAL_CAPABLANCA_PETROSIAN;
}

inline int getShashinQuiescentCapablanca(Value score,int refScore) {
  if(!goldDigger)
  {
    score = (Value)((float)(score) / WEIGHTED_EVAL);
  }
  return abs(score) > refScore ? 0 : 1;
}

inline uint8_t getInitialShashinValue() {
  if (!highTal && !mediumTal && !lowTal && !capablanca && !highPetrosian && !mediumPetrosian && !lowPetrosian)
    return SHASHIN_POSITION_DEFAULT;
  if (lowTal && capablanca && !highPetrosian && !mediumPetrosian && !lowPetrosian)
    return SHASHIN_POSITION_TAL_CAPABLANCA;
  if ((highTal || mediumTal ||lowTal)&& !capablanca && !highPetrosian && !mediumPetrosian && !lowPetrosian)
    return SHASHIN_POSITION_TAL;
  if (!highTal && !mediumTal && !lowTal && capablanca && !highPetrosian && !mediumPetrosian && !lowPetrosian)
    return SHASHIN_POSITION_CAPABLANCA;
  if (!highTal && !mediumTal && !lowTal && capablanca && lowPetrosian)
    return SHASHIN_POSITION_CAPABLANCA_PETROSIAN;
  if (!highTal && !mediumTal && !lowTal && !capablanca && (highPetrosian || mediumPetrosian || lowPetrosian))
    return SHASHIN_POSITION_PETROSIAN;
  if (highTal && mediumTal && lowTal && capablanca && highPetrosian && mediumPetrosian && lowPetrosian)
    return SHASHIN_POSITION_TAL_CAPABLANCA_PETROSIAN;
  return SHASHIN_POSITION_DEFAULT;
}

inline int getInitialShashinQuiescentMiddleHighScore(){
  if((!highTal && !mediumTal && !lowTal && !capablanca && !highPetrosian && !mediumPetrosian && !lowPetrosian)
        ||
    (lowPetrosian||capablanca||lowTal))
	  return 1;
  return 0;
}

inline int getInitialShashinQuiescentMaxScore(){
  if((!highTal && !mediumTal && !lowTal && !capablanca && !highPetrosian && !mediumPetrosian && !lowPetrosian)
     ||(mediumPetrosian||lowPetrosian||capablanca||lowTal||mediumTal))
	  return 1;
  return 0;
}

inline void initShashinValues (Position& pos)
{
  pos.this_thread()->shashinValue = getInitialShashinValue ();
  pos.this_thread()->shashinQuiescentCapablancaMiddleHighScore = getInitialShashinQuiescentMiddleHighScore ();
  pos.this_thread()->shashinQuiescentCapablancaMaxScore = getInitialShashinQuiescentMaxScore ();
}

inline void updateShashinValues (const Position& pos,Value score)
{
  pos.this_thread()->shashinValue = getShashinValue (score);
  pos.this_thread()->shashinQuiescentCapablancaMiddleHighScore = getShashinQuiescentCapablanca (score, SHASHIN_MIDDLE_HIGH_SCORE);
  pos.this_thread()->shashinQuiescentCapablancaMaxScore = getShashinQuiescentCapablanca (score, SHASHIN_MAX_SCORE);
  pos.this_thread()->shashinPosKey=pos.key();
}
inline void revertShashinValues (Position& pos,int lastShashinValue, int lastShashinQuiescentCapablancaMiddleHighScore, int lastShashinQuiescentCapablancaMaxScore, Key lastShashinPosKey)
{
  pos.this_thread()->shashinValue = lastShashinValue;
  pos.this_thread()->shashinQuiescentCapablancaMiddleHighScore = lastShashinQuiescentCapablancaMiddleHighScore;
  pos.this_thread()->shashinQuiescentCapablancaMaxScore = lastShashinQuiescentCapablancaMaxScore;
  pos.this_thread()->shashinPosKey = lastShashinPosKey;
}

//end from Shashin
/// Thread::search() is the main iterative deepening loop. It calls search()
/// repeatedly with increasing depth until the allocated thinking time has been
/// consumed, the user stops the search, or the maximum search depth is reached.

void Thread::search() {

  // To allow access to (ss-7) up to (ss+2), the stack must be oversized.
  // The former is needed to allow update_continuation_histories(ss-1, ...),
  // which accesses its argument at ss-6, also near the root.
  // The latter is needed for statScore and killer initialization.
  Stack stack[MAX_PLY+10], *ss = stack+7;
  Move  pv[MAX_PLY+1];
  Value alpha, beta, delta;
  Move  lastBestMove = MOVE_NONE;
  Depth lastBestMoveDepth = 0;
  MainThread* mainThread = (this == Threads.main() ? Threads.main() : nullptr);
  double timeReduction = 1, totBestMoveChanges = 0;
  //Color us = rootPos.side_to_move();
  int iterIdx = 0;

  std::memset(ss-7, 0, 10 * sizeof(Stack));
  for (int i = 7; i > 0; i--)
      (ss-i)->continuationHistory = &this->continuationHistory[0][0][NO_PIECE][0]; // Use as a sentinel

  for (int i = 0; i <= MAX_PLY + 2; ++i)
      (ss+i)->ply = i;

  ss->pv = pv;

  bestValue = delta = alpha = -VALUE_INFINITE;
  beta = VALUE_INFINITE;

  if (mainThread)
  {
      if (mainThread->bestPreviousScore == VALUE_INFINITE)
          for (int i = 0; i < 4; ++i)
              mainThread->iterValue[i] = VALUE_ZERO;
      else
          for (int i = 0; i < 4; ++i)
              mainThread->iterValue[i] = mainThread->bestPreviousScore;
  }

  size_t multiPV = size_t(Options["MultiPV"]);
  
  //from true handicap mode begin 
  //Skill skill(skillLevel, limitStrength ? uciElo : 0);

  // When playing with strength handicap enable MultiPV search that we will
  // use behind the scenes to retrieve a set of possible moves.
  /*if (skill.enabled() && limitStrength)//from Shashin
      multiPV = std::max(multiPV, (size_t)4);*/
  //from true handicap mode end

  multiPV = std::min(multiPV, rootMoves.size());
  //from Shashin
  initShashinValues (rootPos);
  //end from Shashin

  complexityAverage.set(174, 1);
  lmrAverage.set(6, 100);//lmr_average2
  //trend         = SCORE_ZERO;
  //optimism[ us] = Value(39);
  //optimism[~us] = -optimism[us];

  int searchAgainCounter = 0;
  int failedAspirationsCurrentID = 0; //aspiration patch
  //mcts begin
  bool maybeDraw = rootPos.rule50_count() >= 90 || rootPos.has_game_cycle(2);
  int mctsThreads = Options["MCTSThreads"];
  if ((Options["MCTS"])&&((((mctsThreads==1)&&(idx == 1))||
	 ((mctsThreads > 1)&& (idx<=(size_t)mctsThreads)&&(!mainThread)))
	 && (!maybeDraw))
	 )
  {
	  MonteCarlo *monteCarlo = new MonteCarlo(rootPos);
	  if (!monteCarlo)
	  {
		  std::cerr << IO_LOCK << "Could not allocate " << sizeof(MonteCarlo) << " bytes for MonteCarlo search" << std::endl << IO_UNLOCK;
		  ::exit(EXIT_FAILURE);
	  }

	  monteCarlo->search();
	  if (idx == 1 && Limits.infinite)
		monteCarlo->print_children();
	  delete monteCarlo;
  }
  else
  {
  //from mcts end
    // Iterative deepening loop until requested to stop or the target depth is reached
    while (   ++rootDepth < MAX_PLY //handicap mode
           && !Threads.stop
           && !(depthLimit && mainThread && rootDepth > depthLimit))
    {
	// Age out PV variability metric
	if (mainThread)
	    totBestMoveChanges /= 2;

	// Save the last iteration's scores before first PV line is searched and
	// all the move scores except the (new) PV are set to -VALUE_INFINITE.
	for (RootMove& rm : rootMoves)
	    rm.previousScore = rm.score;

	size_t pvFirst = 0;
	pvLast = 0;

	if (!Threads.increaseDepth)
	   searchAgainCounter++;

	//aspiration patch begin
	int failedAspirationsPrevID = failedAspirationsCurrentID;
	failedAspirationsCurrentID = 0;
	//aspiration patch end
	
	// MultiPV loop. We perform a full root search for each PV line
	for (pvIdx = 0; pvIdx < multiPV && !Threads.stop; ++pvIdx)
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
	    if (rootDepth >= 4)
	    {
			Value prev = rootMoves[pvIdx].averageScore;
			delta = Value(12 + 4 * failedAspirationsPrevID)+ int(prev) * prev / 19178;//aspiration patch 
			alpha = std::max(prev - delta,-VALUE_INFINITE);
			beta  = std::min(prev + delta, VALUE_INFINITE);
			updateShashinValues(rootPos,prev);//from ShashChess
			
			// Adjust trend based on root move's previousScore
            /*int tr = 11 + 119 * (prev-4) / (abs(prev-4) + 92);
            trend = (us == WHITE ?  make_score(tr, tr / 2)
                                 : -make_score(tr, tr / 2));*/
            //int opt = sigmoid(prev, 8, 17, 144, 13966, 183);
            //optimism[ us] = Value(opt);
            //optimism[~us] = -optimism[us];                                 
	    }

	    // Start with a small aspiration window and, in the case of a fail
	    // high/low, re-search with a bigger window until we don't fail
	    // high/low anymore.
	    int failedHighCnt = 0;
	    while (true)
	    {
        // Adjust the effective depth searched, but ensuring at least one effective increment for every
        // four searchAgain steps (see issue #2717).
        Depth adjustedDepth = std::max(1, rootDepth - failedHighCnt - 3 * (searchAgainCounter + 1) / 4);
		bestValue = Stockfish::search<Root>(rootPos, ss, alpha, beta, adjustedDepth, false, false);//mcts

		// Bring the best move to the front. It is critical that sorting
		// is done with a stable algorithm because all the values but the
		// first and eventually the new best one are set to -VALUE_INFINITE
		// and we want to keep the same order for all the moves except the
		// new PV that goes to the front. Note that in case of MultiPV
		// search the already searched PV lines are preserved.
		std::stable_sort(rootMoves.begin() + pvIdx, rootMoves.begin() + pvLast);

		// If search has been stopped, we break immediately. Sorting is
		// safe because RootMoves is still valid, although it refers to
		// the previous iteration.
		if (Threads.stop)
		    break;

		// When failing high/low give some update (without cluttering
		// the UI) before a re-search.
		if (   mainThread
		    && multiPV == 1
		    && (bestValue <= alpha || bestValue >= beta)
		    && Time.elapsed() > 3000)
		    sync_cout << UCI::pv(rootPos, rootDepth, alpha, beta) << sync_endl;

		// In case of failing low/high increase aspiration window and
		// re-search, otherwise exit the loop.
		if (bestValue <= alpha)
		{
		    beta = (alpha + beta) / 2;
		    alpha = std::max(bestValue - delta, -VALUE_INFINITE);

		    failedHighCnt = 0;
			failedAspirationsCurrentID++;//aspiration patch
		    if (mainThread)
			mainThread->stopOnPonderhit = false;
		}
		else if (bestValue >= beta)
		{
		    beta = std::min(bestValue + delta, VALUE_INFINITE);
		    ++failedHighCnt;
			failedAspirationsCurrentID++; //aspiration patch
		}
		else
		    break;

		delta += delta / 4 + 2;

		assert(alpha >= -VALUE_INFINITE && beta <= VALUE_INFINITE);
	    }

	    // Sort the PV lines searched so far and update the GUI
	    std::stable_sort(rootMoves.begin() + pvFirst, rootMoves.begin() + pvIdx + 1);

	    if (    mainThread
		&& (Threads.stop || pvIdx + 1 == multiPV || Time.elapsed() > 3000))
		sync_cout << UCI::pv(rootPos, rootDepth, alpha, beta) << sync_endl;
	}

	if (!Threads.stop)
	    completedDepth = rootDepth;

	if (rootMoves[0].pv[0] != lastBestMove) {
	   lastBestMove = rootMoves[0].pv[0];
	   lastBestMoveDepth = rootDepth;
	}

	// Have we found a "mate in x"?
	if (   Limits.mate
	    && bestValue >= VALUE_MATE_IN_MAX_PLY
	    && VALUE_MATE - bestValue <= 2 * Limits.mate)
	    Threads.stop = true;

	if (!mainThread)
	    continue;
	
	//from true handicap mode begin
	// If skill level is enabled and time is up, pick a sub-optimal best move
	/*if (skill.enabled() && skill.time_to_pick(rootDepth) && limitStrength) //from handicap mode
	    skill.pick_best(multiPV);*/
	//from true handicap mode end
	
	// Use part of the gained time from a previous stable move for the current move
  	for (Thread* th : Threads)
  	{
      	totBestMoveChanges += th->bestMoveChanges;
      	th->bestMoveChanges = 0;
  	}

	// Do we have time for the next iteration? Can we stop searching now?
	if (    Limits.use_time_management()
	    && !Threads.stop
	    && !mainThread->stopOnPonderhit)
	{
	    double fallingEval = (69 + 12 * (mainThread->bestPreviousAverageScore - bestValue)
								  +  6 * (mainThread->iterValue[iterIdx] - bestValue)) / 781.4;
	    fallingEval = std::clamp(fallingEval, 0.5, 1.5);

	    // If the bestMove is stable over several iterations, reduce time accordingly
        timeReduction = lastBestMoveDepth + 10 < completedDepth ? 1.63 : 0.73;
        double reduction = (1.56 + mainThread->previousTimeReduction) / (2.20 * timeReduction);

        double bestMoveInstability = 1 + 1.7 * totBestMoveChanges / Threads.size();
        int complexity = mainThread->complexityAverage.value();
		double complexPosition = std::clamp(1.0 + (complexity - 277) / 1819.1, 0.5, 1.5);

        double totalTime = Time.optimum() * fallingEval * reduction * bestMoveInstability * complexPosition;

	    // Cap used time in case of a single legal move for a better viewer experience in tournaments
	    // yielding correct scores and sufficiently fast moves.
	    if (rootMoves.size() == 1)
		totalTime = std::min(500.0, totalTime);

	    // Stop the search if we have exceeded the totalTime
	    if (Time.elapsed() > totalTime)
	    {
		// If we are allowed to ponder do not stop the search now but
		// keep pondering until the GUI sends "ponderhit" or "stop".
		if (mainThread->ponder)
		    mainThread->stopOnPonderhit = true;
		else
		    Threads.stop = true;
	    }
	    else if (   Threads.increaseDepth
		     && !mainThread->ponder
		     && Time.elapsed() > totalTime * 0.43)
		     Threads.increaseDepth = false;
	    else
		     Threads.increaseDepth = true;
	}

	mainThread->iterValue[iterIdx] = bestValue;
	iterIdx = (iterIdx + 1) & 3;
    }
  }
  if (!mainThread)
      return;

  mainThread->previousTimeReduction = timeReduction;

  //from true handicap mode begin
  // If skill level is enabled, swap best PV line with the sub-optimal one
  /*if (skill.enabled() && limitStrength)//from Shashin
      std::swap(rootMoves[0], *std::find(rootMoves.begin(), rootMoves.end(),
                skill.best ? skill.best : skill.pick_best(multiPV)));*/
  //from true handicap mode end	
}


namespace {

  // search<>() is the main search function for both PV and non-PV nodes

  template <NodeType nodeType>
  Value search(Position& pos, Stack* ss, Value alpha, Value beta, Depth depth, bool cutNode, bool mcts) { //mcts

    constexpr bool PvNode = nodeType != NonPV;
    constexpr bool rootNode = nodeType == Root;
    const Depth maxNextDepth = rootNode ? depth : depth + 1;
	ss->depth = depth; //qs_update_end
    pos.set_leaf( !PvNode && depth <= 7 ); //leafDepth7
    //from Shashin begin
    int lastShashinValue, lastShashinQuiescentCapablancaMiddleHighScore, lastShashinQuiescentCapablancaMaxScore;
    Key lastShashinPosKey;
    //from Shashin end
    bool gameCycle = false;//from Crystal

    // Check if we have an upcoming move which draws by repetition, or
    // if the opponent had an alternative move earlier to this position.
    if (   !rootNode
        && pos.rule50_count() >= 3
        && alpha < VALUE_DRAW
        && pos.has_game_cycle(ss->ply))
    {
        alpha = value_draw(pos.this_thread());
        if (alpha >= beta)
            return alpha;
    }

    //from Crystal begin
    if (   pos.rule50_count() >= 3
        && !rootNode
        && pos.has_game_cycle(ss->ply))
    {
	    gameCycle = true;
    }
    //from Crystal end
    // Dive into quiescence search when the depth reaches zero
    if (depth <= 0)
        return qsearch<PvNode ? PV : NonPV>(pos, ss, alpha, beta);

    assert(-VALUE_INFINITE <= alpha && alpha < beta && beta <= VALUE_INFINITE);
    assert(PvNode || (alpha == beta - 1));
    assert(0 < depth && depth < MAX_PLY);
    assert(!(PvNode && cutNode));

    Move pv[MAX_PLY+1], capturesSearched[32], quietsSearched[64];
    StateInfo st;
    ASSERT_ALIGNED(&st, Eval::NNUE::CacheLineSize);

    TTEntry* tte;
    Key posKey;
    Move ttMove, move, excludedMove=MOVE_NONE, bestMove,expTTMove=MOVE_NONE;//from Kelly
    Depth extension, newDepth;
    //from Kelly begin
    Value bestValue, value, ttValue, eval=VALUE_NONE, maxValue, expTTValue=VALUE_NONE, probCutBeta;
    bool givesCheck, improving, didLMR, priorCapture, singularQuietLMR, expTTHit=false, isMate; //from Crystal
    //from Kelly End
    bool capture, captureOrPromotion, doFullDepthSearch, moveCountPruning, 
		 ttCapture, kingDanger, ourMove; //from Crystal + official from Shashin

    Piece movedPiece;
    int moveCount, captureCount, quietCount, improvement=0, rootDepth, complexity=0; //from Crystal
    //from Kelly begin
    bool updatedLearning = false;

    //flags to preserve node tyepes
    bool disableNMAndPC = false;
    bool expectedPVNode =  false;
    int sibs = 0;
    //from Kelly end
    // Step 1. Initialize node
    Thread* thisThread = pos.this_thread();
    ss->inCheck        = pos.checkers();
    ss->nodeType       = nodeType;//nodeTypeHistory
    priorCapture       = pos.captured_piece();
    Color us           = pos.side_to_move();
    moveCount          = captureCount = quietCount = ss->moveCount = 0;
    bestValue          = -VALUE_INFINITE;
    //from Crystal begin
    kingDanger=false;
    rootDepth = thisThread->rootDepth;
    ourMove             = !(ss->ply & 1);
    //from Crystal end
    maxValue           = VALUE_INFINITE;
	ss->distanceFromPv = (PvNode ? 0 : ss->distanceFromPv);//dd^^
    //Full Threads patch begin
    if(thisThread->fullSearch)
      improving = true;
    //Full Threads patch end

    // Check for the available remaining time
    if (thisThread == Threads.main())
        static_cast<MainThread*>(thisThread)->check_time();

    // Used to send selDepth info to GUI (selDepth counts from 1, ply from 0)
    if (PvNode && thisThread->selDepth < ss->ply + 1)
        thisThread->selDepth = ss->ply + 1;

    if (!rootNode)
    {
        // Step 2. Check for aborted search and immediate draw
	if (   Threads.stop.load(std::memory_order_relaxed)
            || pos.is_draw(ss->ply)
            || ss->ply >= MAX_PLY)
       {
           Value draw = value_draw(pos.this_thread());
	       //from ShashChess begin
           if(ss->ply >= MAX_PLY && !ss->inCheck)
           {
               Value evalPos=evaluate(pos);
               if(pos.key()!=pos.this_thread()->shashinPosKey)
               {
                   updateShashinValues(pos,evalPos);
               }
               return evalPos;
            }
            updateShashinValues(pos,draw);
            return draw;
            //from ShashChess end
        }

        // Step 3. Mate distance pruning. Even if we mate at the next move our score
        // would be at best mate_in(ss->ply+1), but if alpha is already bigger because
        // a shorter mate was found upward in the tree then there is no need to search
        // because we will never beat the current alpha. Same logic but with reversed
        // signs applies also in the opposite condition of being mated instead of giving
        // mate. In this case return a fail-high score.
        alpha = std::max(mated_in(ss->ply), alpha);
        beta = std::min(mate_in(ss->ply+1), beta);
        if (alpha >= beta)
            return alpha;
    }
    else
        thisThread->rootDelta = beta - alpha;

    assert(0 <= ss->ply && ss->ply < MAX_PLY);

    (ss+1)->ttPv         = false;
    (ss+1)->excludedMove = bestMove = MOVE_NONE;
    (ss+2)->killers[0]   = (ss+2)->killers[1] = MOVE_NONE;
    (ss+2)->cutoffCnt    = 0;
    ss->doubleExtensions = (ss-1)->doubleExtensions;
    //qs_update_end
    Square prevSq        = to_sq((ss-1)->currentMove);
    ss->currentReduction = 0; //useRinSMS

    // Initialize statScore to zero for the grandchildren of the current position.
    // So statScore is shared between all grandchildren and only the first grandchild
    // starts with statScore = 0. Later grandchildren start with the last calculated
    // statScore of the previous grandchild. This influences the reduction rules in
    // LMR which are based on the statScore of parent position.
    //pvNode1Ply3 begin
    if (!rootNode){
		if (PvNode && ss->ply < 2)
			(ss+3)->statScore = 0;
		else{
       		(ss+2)->statScore = 0;
		}
     }		
     //pvNode1Ply3 end
    // Step 4. Transposition table lookup. We don't want the score of a partial
    // search to overwrite a previous full search TT value, so we use a different
    // position key in case of an excluded move.
    excludedMove = ss->excludedMove;
    posKey = excludedMove == MOVE_NONE ? pos.key() : pos.key() ^ make_key(excludedMove);
    tte = TT.probe(posKey, ss->ttHit);
    ttValue = ss->ttHit ? value_from_tt(tte->value(), ss->ply, pos.rule50_count()) : VALUE_NONE;
    ttMove =  rootNode ? thisThread->rootMoves[thisThread->pvIdx].pv[0]
            : ss->ttHit    ? tte->move() : MOVE_NONE;
    ttCapture = ttMove && pos.capture(ttMove);
    if (!excludedMove)
        ss->ttPv = PvNode || (ss->ttHit && tte->is_pv());

    // At non-PV nodes we check for an early TT cutoff
    if (  !PvNode
        && ss->ttHit
	&& ((!gameCycle)||(pos.this_thread()->shashinQuiescentCapablancaMaxScore)) //from Crystal
        && tte->depth() > depth - ((int)thisThread->id() & 0x1) - (tte->bound() == BOUND_EXACT)
        && ttValue != VALUE_NONE // Possible in case of TT access race
        && (tte->bound() & (ttValue >= beta ? BOUND_LOWER : BOUND_UPPER)))
    {
        // If ttMove is quiet, update move sorting heuristics on TT hit (~1 Elo)
        if (ttMove)
        {
            if (ttValue >= beta)
            {
                // Bonus for a quiet ttMove that fails high (~3 Elo)
                if (!ttCapture)
                    update_quiet_stats(pos, ss, ttMove, stat_bonus(depth));

                // Extra penalty for early quiet moves of the previous ply (~0 Elo)
                if ((ss-1)->moveCount <= 2 && !priorCapture)
                    update_continuation_histories(ss-1, pos.piece_on(prevSq), prevSq, -stat_bonus(depth + 1));
            }
            // Penalty for a quiet ttMove that fails low (~1 Elo)
            else if (!ttCapture)
            {
                int penalty = -stat_bonus(depth);
                thisThread->mainHistory[us][from_to(ttMove)] << penalty;
                update_continuation_histories(ss, pos.moved_piece(ttMove), to_sq(ttMove), penalty);
            }
        }

        // Partial workaround for the graph history interaction problem
        // For high rule50 counts don't produce transposition table cutoffs.
        if (pos.rule50_count() < 90)
            return ttValue;
    }
    //from Kelly begin
    //Step 4Bis. Global Learning Table lookup
    expTTHit = false;
    updatedLearning = false;

    if (!excludedMove && LD.is_enabled()&& useLearning)
    {
        const LearningMove *learningMove = nullptr;
        sibs = LD.probe(posKey, learningMove);
        if (learningMove)
        {
            assert(sibs);

            enabledLearningProbe = true;
            expTTHit = true;
            if (!ttMove)
            {
                ttMove = learningMove->move;
            }

            if (learningMove->depth >= depth)
            {
                expTTMove = learningMove->move;
                expTTValue = (goldDigger? (learningMove->score) : ((Value)(((float)(learningMove->score)) * WEIGHTED_EVAL)));
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
                    improving = true;
                }
            }

            // At non-PV nodes we check for an early Global Learning Table cutoff
            // If expTTMove is quiet, update move sorting heuristics on global learning table hit
            if (!PvNode
                && updatedLearning
                && expTTValue != VALUE_NONE // Possible in case of Global Learning Table access race
                && (learningMove->depth >= depth))
            {
                if (expTTValue >= beta)
                {
                    if (!pos.capture(learningMove->move))
                        update_quiet_stats(pos, ss, learningMove->move, stat_bonus(depth));

                    // Extra penalty for early quiet moves of the previous ply
                    if ((ss - 1)->moveCount <= 2 && !priorCapture)
                        update_continuation_histories(ss - 1, pos.piece_on(prevSq), prevSq, -stat_bonus(depth + 1));
                }
                // Penalty for a quiet ttMove that fails low
                else
                {
                    if (!pos.capture(expTTMove))
                    {
                        int penalty = -stat_bonus(depth);
                        thisThread->mainHistory[us][from_to(expTTMove)] << penalty;
                        update_continuation_histories(ss, pos.moved_piece(expTTMove), to_sq(expTTMove), penalty);
                    }
                }

                //thisThread->tbHits.fetch_add(1, std::memory_order_relaxed);
                if (pos.rule50_count() < 90)
                    return expTTValue;
            }
        }
    }
    //from Kelly end

    // Step 5. Tablebases probe
    if (!rootNode && TB::Cardinality)
    {
        int piecesCount = pos.count<ALL_PIECES>();

        if (    piecesCount <= TB::Cardinality
            && (piecesCount <  TB::Cardinality || depth >= TB::ProbeDepth)
            &&  pos.rule50_count() == 0
            && !pos.can_castle(ANY_CASTLING))
        {
            TB::ProbeState err;
            TB::WDLScore wdl = Tablebases::probe_wdl(pos, &err);

            // Force check of time on the next occasion
            if (thisThread == Threads.main())
                static_cast<MainThread*>(thisThread)->callsCnt = 0;

            if (err != TB::ProbeState::FAIL)
            {
                thisThread->tbHits.fetch_add(1, std::memory_order_relaxed);

                int drawScore = TB::UseRule50 ? 1 : 0;

                // use the range VALUE_MATE_IN_MAX_PLY to VALUE_TB_WIN_IN_MAX_PLY to score
                value =  wdl < -drawScore ? VALUE_MATED_IN_MAX_PLY + ss->ply + 1
                       : wdl >  drawScore ? VALUE_MATE_IN_MAX_PLY - ss->ply - 1
                                          : VALUE_DRAW + 2 * wdl * drawScore;

                Bound b =  wdl < -drawScore ? BOUND_UPPER
                         : wdl >  drawScore ? BOUND_LOWER : BOUND_EXACT;

                if (    b == BOUND_EXACT
                    || (b == BOUND_LOWER ? value >= beta : value <= alpha))
                {
                    tte->save(posKey, value_to_tt(value, ss->ply), ss->ttPv, b,
                              std::min(MAX_PLY - 1, depth + 6),
                              MOVE_NONE, VALUE_NONE);

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

    CapturePieceToHistory& captureHistory = thisThread->captureHistory;

    // Step 6. Static evaluation of the position
    if (ss->inCheck)
    {
        // Skip early pruning when in check
        ss->staticEval = eval = VALUE_NONE;
        //from ShashChess begin
        if(pos.key()!=pos.this_thread()->shashinPosKey)
        {
      	    updateShashinValues(pos,ss->staticEval);//from ShashChess
        }
        //from ShashChess end
        improving = false;
        improvement = 0;
        complexity = 0;
        goto moves_loop;
    }
    else if (ss->ttHit)
    {
        // Never assume anything about values stored in TT
        ss->staticEval = eval = tte->eval();
		if (eval == VALUE_NONE)
		{ 
		  ss->staticEval = eval = evaluate(pos, &complexity);
          //from ShashChess begin
          if(pos.key()!=pos.this_thread()->shashinPosKey)
          {
            updateShashinValues(pos,eval);     
          }          
          //from ShashChess end
		}
        else // Fall back to (semi)classical complexity for TT hits, the NNUE complexity is lost
            complexity = abs(ss->staticEval - pos.psq_eg_stm());


        // ttValue can be used as a better position evaluation (~4 Elo)
        if (    ttValue != VALUE_NONE
            && (tte->bound() & (ttValue > eval ? BOUND_LOWER : BOUND_UPPER)))
        {
            eval = ttValue;
            updateShashinValues(pos,eval);
        }
    }
    else
    {
      //from kelly begin
      if (!LD.is_enabled() || !expTTHit|| !updatedLearning)
      {
		ss->staticEval = eval = evaluate(pos, &complexity);
        if(pos.key()!=pos.this_thread()->shashinPosKey)
        {
      	    updateShashinValues(pos,ss->staticEval);//from ShashChess
        }
		
        // Save static evaluation into transposition table
        if (!excludedMove)
            tte->save(posKey, VALUE_NONE, ss->ttPv, BOUND_NONE, DEPTH_NONE, MOVE_NONE, eval);
      }
      else //learning
      {
		// Never assume anything on values stored in Global Learning Table
		ss->staticEval = eval = expTTValue;
		if (eval == VALUE_NONE)
		{ //from ShashChess begin
		  ss->staticEval = eval = evaluate(pos, &complexity);
          if(pos.key()!=pos.this_thread()->shashinPosKey)
          {
            updateShashinValues(pos,eval);     
          }          
		}
		if (eval == VALUE_DRAW)
        {
            eval = value_draw(thisThread);
            updateShashinValues(pos,eval);
        }
		// Can expTTValue be used as a better position evaluation?
		if(expTTValue != VALUE_NONE)
        {
		  eval = expTTValue;
          updateShashinValues(pos,eval);
        }  
      }
    }
    //from Kelly end
    //from ShashChess end

    thisThread->complexityAverage.update(complexity);
    
    // Use static evaluation difference to improve quiet move ordering (~3 Elo)
    if (is_ok((ss-1)->currentMove) && !(ss-1)->inCheck && !priorCapture)
    {
        int bonus = std::clamp(-16 * int((ss-1)->staticEval + ss->staticEval), -2000, 2000);
        thisThread->mainHistory[~us][from_to((ss-1)->currentMove)] << bonus;
    }
    if (thisThread->fullSearch) goto moves_loop; //full threads patch

    //from Kelly begin
    if(!expectedPVNode)
    {
		// Set up the improvement variable, which is the difference between the current
		// static evaluation and the previous static evaluation at our turn (if we were
		// in check at our previous move we look at the move prior to it). The improvement
		// margin and the improving flag are used in various pruning heuristics.
		improvement =   (ss-2)->staticEval != VALUE_NONE ? ss->staticEval - (ss-2)->staticEval
					  : (ss-4)->staticEval != VALUE_NONE ? ss->staticEval - (ss-4)->staticEval
					  :                                    175;

	    improving = improvement > 0;
    }
    //from Kelly end
    //from Crystal begin
    if (!PvNode)
    {
        bool razoringFromCrystal= (ourMove || !excludedMove)
                    && !gameCycle
                    && !thisThread->nmpGuard
                    &&  abs(eval) < 26305;
        if (razoringFromCrystal && (rootDepth > 10) && !ourMove)
            kingDanger = pos.king_danger();
        // Step 7. Razoring.
        // If eval is really low check with qsearch if it can exceed alpha, if it can't,
        // return a fail low.               
        if (
            (depth <= 7)
            && eval < alpha - 348 - 258 * depth * depth + ((pos.this_thread()->shashinQuiescentCapablancaMiddleHighScore) ? ((ss-1)->statScore / 1024):0)) //rzrPrevSs4
        {
            value = qsearch<NonPV>(pos, ss, alpha - 1, alpha);
            if (value < alpha || ((depth == 1) && (pos.this_thread()->shashinQuiescentCapablancaMaxScore) )) //razFullReturn1
                return value;
        }    
    }
    
    //from Crystal end   
 
    // Step 8. Futility pruning: child node (~25 Elo).
    // The depth condition is important for mate finding.
    if (//from Crystal begin
	((pos.this_thread()->shashinQuiescentCapablancaMiddleHighScore && (!ss->ttPv))
	||
	(!pos.this_thread()->shashinQuiescentCapablancaMiddleHighScore && !kingDanger)) 
	//from Crystal end
		&&  depth < 8
        &&  eval - futility_margin(depth, improving) - (ss-1)->statScore / 256 >= beta
        &&  eval >= beta
        &&  eval < 26305) // larger than VALUE_KNOWN_WIN, but smaller than TB wins.
        return eval;

    // Step 9. Null move search with verification search (~22 Elo)
    if (
	(   //from Crystal begin
	    ((pos.this_thread()->shashinQuiescentCapablancaMiddleHighScore
		&& !PvNode && !excludedMove)
	    ||
	    (!pos.this_thread()->shashinQuiescentCapablancaMiddleHighScore
	       && !kingDanger
	    ))
	    //from Crystal end
	)
	//from Crystal begin
	&& ( (pos.this_thread()->shashinQuiescentCapablancaMaxScore)
	     ||
	     (!(rootDepth > 10 && MoveList<LEGAL>(pos).size() < 6))
	   )
	//from Crystal end
	&& (ss->ply >= thisThread->nmpMinPly || us != thisThread->nmpColor)
	&& (ss-1)->currentMove != MOVE_NULL
        && (ss-1)->statScore < 14695
        &&  eval >= beta
        &&  eval >= ss->staticEval
        &&  ss->staticEval >= beta - 15 * depth - improvement / 15 + 201 + complexity / 24
    &&  pos.non_pawn_material(us)
	&& ((pos.this_thread()->shashinQuiescentCapablancaMaxScore)||(!gameCycle)) //from Crystal
	&&  !disableNMAndPC //Kelly
	)
    {
        assert(eval - beta >= 0);

        // Null move dynamic reduction based on depth, eval and complexity of position
        Depth R = std::min(int(eval - beta) / 147, 5) + depth / 3 + 4 - (complexity > 650);

        ss->currentMove = MOVE_NULL;
        ss->continuationHistory = &thisThread->continuationHistory[0][0][NO_PIECE][0];

        //begin from Shashin
        lastShashinValue=pos.this_thread()->shashinValue,
        lastShashinQuiescentCapablancaMiddleHighScore=pos.this_thread()->shashinQuiescentCapablancaMiddleHighScore,
	    lastShashinQuiescentCapablancaMaxScore=pos.this_thread()->shashinQuiescentCapablancaMaxScore;
        lastShashinPosKey=pos.this_thread()->shashinPosKey;
        //end from Shashin
        pos.do_null_move(st);
        updateShashinValues(pos,- ss->staticEval); //from Shashin
        Value nullValue = -search<NonPV>(pos, ss+1, -beta, -beta+1, depth-R, !cutNode, mcts);//mcts

        pos.undo_null_move();

        revertShashinValues(pos,lastShashinValue, lastShashinQuiescentCapablancaMiddleHighScore, lastShashinQuiescentCapablancaMaxScore, lastShashinPosKey);//from Shashin
        if (nullValue >= beta)
        {
            // Do not return unproven mate or TB scores
            if (nullValue >= VALUE_TB_WIN_IN_MAX_PLY)
                nullValue = beta;

            if (thisThread->nmpMinPly || (abs(beta) < VALUE_KNOWN_WIN && depth < 14))
                return nullValue;

            assert(!thisThread->nmpMinPly); // Recursive verification is not allowed

            // Do verification search at high depths, with null move pruning disabled
            // for us, until ply exceeds nmpMinPly.
            thisThread->nmpMinPly = ss->ply + 3 * (depth-R) / 4;
            thisThread->nmpColor = us;

            //From Crystal begin
            // Do verification search at high depths
            thisThread->nmpGuard = true;
            //From Crystal end

            Value v = search<NonPV>(pos, ss, beta-1, beta, depth-R, false, mcts);//mcts

            thisThread->nmpGuard = false; //from Crystal

            thisThread->nmpMinPly = 0;

            if (v >= beta)
                return nullValue;
        }
    }

    probCutBeta = beta + 179 - 46 * improving;

    // Step 10. ProbCut (~4 Elo)
    // If we have a good enough capture and a reduced search returns a value
    // much above beta, we can (almost) safely prune the previous move.
    if (   !PvNode		
		&&  !disableNMAndPC //Kelly
        &&  depth > 4
        &&  abs(beta) < VALUE_TB_WIN_IN_MAX_PLY
        // if value from transposition table is lower than probCutBeta, don't attempt probCut
        // there and in further interactions with transposition table cutoff depth is set to depth - 3
        // because probCut search has depth set to depth - 4 but we also do a move before it
        // so effective depth is equal to depth - 3
        && !(   ss->ttHit
             && tte->depth() >= depth - 3
             && ttValue != VALUE_NONE
             && ttValue < probCutBeta))
    {
        assert(probCutBeta < VALUE_INFINITE);

        MovePicker mp(pos, ttMove, probCutBeta - ss->staticEval, depth - 3, &captureHistory);

        while ((move = mp.next_move()) != MOVE_NONE)
            if (move != excludedMove && pos.legal(move))
            {
                assert(pos.capture(move) || promotion_type(move) == QUEEN);

                captureOrPromotion = true;

                ss->currentMove = move;
                ss->continuationHistory = &thisThread->continuationHistory[ss->inCheck]
                                                                          [captureOrPromotion]
                                                                          [pos.moved_piece(move)]
                                                                          [to_sq(move)];
                //begin from Shashin
                lastShashinValue=pos.this_thread()->shashinValue,
                lastShashinQuiescentCapablancaMiddleHighScore=pos.this_thread()->shashinQuiescentCapablancaMiddleHighScore,
		        lastShashinQuiescentCapablancaMaxScore=pos.this_thread()->shashinQuiescentCapablancaMaxScore;
                lastShashinPosKey=pos.this_thread()->shashinPosKey;
                //end from Shashin
                pos.do_move(move, st);
                updateShashinValues(pos,- ss->staticEval); //from Shashin
                // Perform a preliminary qsearch to verify that the move holds
                value = -qsearch<NonPV>(pos, ss+1, -probCutBeta, -probCutBeta+1);

                // If the qsearch held, perform the regular search
                if (value >= probCutBeta)
                    value = -search<NonPV>(pos, ss+1, -probCutBeta, -probCutBeta+1, depth - 4, !cutNode, mcts);//mcts

                pos.undo_move(move);
                revertShashinValues(pos,lastShashinValue, lastShashinQuiescentCapablancaMiddleHighScore, lastShashinQuiescentCapablancaMaxScore,lastShashinPosKey);//from Shashin

                if (value >= probCutBeta)
                {
                    // Save ProbCut data into transposition table
                    tte->save(posKey, value_to_tt(value, ss->ply), ss->ttPv, BOUND_LOWER, depth - 3, move, ss->staticEval);
                    return value;
                }
            }
    }

    // Step 11. If the position is not in TT, decrease depth by 3.
    // Use qsearch if depth is equal or below zero (~4 Elo)
    if (    PvNode
        && !ttMove)
        depth -= 3 +((pos.this_thread()->shashinQuiescentCapablancaMaxScore)?0: (- (!ss->ttHit && complexity > 1000)));//cmplxDR1

    if (depth <= 0)
        return qsearch<PV>(pos, ss, alpha, beta);

    if (    cutNode
        &&  depth >= 8
        && !ttMove)
         depth -= 1 + (((pos.this_thread()->shashinQuiescentCapablancaMaxScore))?((ss-1)->moveCount > 0):0);//	depth2
moves_loop: // When in check, search starts from here

    // Step 12. A small Probcut idea, when we are in check (~0 Elo)
    probCutBeta = beta + 481;
    if (   ss->inCheck
        && !PvNode
        && depth >= 2
        && ttCapture
        && (tte->bound() & BOUND_LOWER)
        && tte->depth() >= depth - 3
        && ttValue >= probCutBeta
        && abs(ttValue) <= VALUE_KNOWN_WIN
        && abs(beta) <= VALUE_KNOWN_WIN
       )
        return probCutBeta;

    const PieceToHistory* contHist[] = { (ss-1)->continuationHistory, (ss-2)->continuationHistory,
                                          nullptr                   , (ss-4)->continuationHistory,
                                          nullptr                   , (ss-6)->continuationHistory };

    Move countermove = thisThread->counterMoves[pos.piece_on(prevSq)][prevSq];
    //goodStatic6 begin
    int MCP_limit = futility_move_count(improving, depth);
    if (!ss->inCheck && !excludedMove)
        MCP_limit += std::max(int(ss->staticEval - beta), 0) / 64;
    //goodStatic6 end
    MovePicker mp(pos, ttMove, depth, &thisThread->mainHistory,
                                      &captureHistory,
                                      contHist,
                                      countermove,
                                      ss->killers);

    value = bestValue;
    moveCountPruning = singularQuietLMR = false;

    // Indicate PvNodes that will probably fail low if the node was searched
    // at a depth equal or greater than the current depth, and the result of this search was a fail low.
    bool likelyFailLow =    PvNode
                         && ttMove
                         && (tte->bound() & BOUND_UPPER)
                         && tte->depth() >= depth;

    bool negExt = false; //negExtR4
    // Step 13. Loop through all pseudo-legal moves until no moves remain
    // or a beta cutoff occurs.
    while ((move = mp.next_move(moveCountPruning)) != MOVE_NONE)
    {
      assert(is_ok(move));

      if (move == excludedMove)
          continue;

      // At root obey the "searchmoves" option and skip moves not listed in Root
      // Move List. As a consequence any illegal move is also skipped. In MultiPV
      // mode we also skip PV moves which have been already searched and those
      // of lower "TB rank" if we are in a TB root position.
      if (rootNode && !std::count(thisThread->rootMoves.begin() + thisThread->pvIdx,
                                  thisThread->rootMoves.begin() + thisThread->pvLast, move))
          continue;

      // Check for legality
      if (!rootNode && !pos.legal(move))
          continue;

      ss->moveCount = ++moveCount;

      if (rootNode && thisThread == Threads.main() && Time.elapsed() > 3000)
          sync_cout << "info depth " << depth
                    << " currmove " << UCI::move(move, pos.is_chess960())
                    << " currmovenumber " << moveCount + thisThread->pvIdx << sync_endl;
      if (PvNode)
          (ss+1)->pv = nullptr;

      extension = 0;
      captureOrPromotion = pos.capture_or_promotion(move);
      capture = pos.capture(move);
      movedPiece = pos.moved_piece(move);
      givesCheck = pos.gives_check(move);
	  //from Crystal begin
	  isMate = false;

      if (givesCheck)
      {
          pos.do_move(move, st, givesCheck);
          isMate = MoveList<LEGAL>(pos).size() == 0;
          pos.undo_move(move);
      }

      if (isMate && (pos.this_thread()->shashinValue == SHASHIN_POSITION_CAPABLANCA))
      {
          ss->currentMove = move;
          ss->continuationHistory = &thisThread->continuationHistory[ss->inCheck]
                                                                    [captureOrPromotion]
                                                                    [movedPiece]
                                                                    [to_sq(move)];
          value = mate_in(ss->ply+1);

          if (PvNode && (moveCount == 1 || (value > alpha && (rootNode || value < beta))))
          {
              (ss+1)->pv = pv;
              (ss+1)->pv[0] = MOVE_NONE;
          }
      }
      else
      { //end from Crystal     
	  // Calculate new depth for this move
      newDepth = depth - 1;

      Value delta = beta - alpha;
      
      //full threads patch begin
      if(thisThread->fullSearch)
      {
          goto skipExtensionAndPruning;
      }
      //full threads patch end
	  
      // Step 14. Pruning at shallow depth (~98 Elo). Depth conditions are important for mate finding.
      if (//from Crystal
	  (((pos.this_thread()->shashinQuiescentCapablancaMaxScore) && !rootNode)
	  ||
	  ((!pos.this_thread()->shashinQuiescentCapablancaMaxScore) && !PvNode) )
	  //end from Crystal
          && pos.non_pawn_material(us)
          && bestValue > VALUE_TB_LOSS_IN_MAX_PLY)
      {
          // Skip quiet moves if movecount exceeds our FutilityMoveCount threshold (~7 Elo)
	  	  //goodStatic6 + nodeTypeHistory begin
          if (!PvNode && (ss-1)->nodeType == NonPV)
            moveCountPruning = moveCount >=
                (((pos.this_thread()->shashinValue!=SHASHIN_POSITION_PETROSIAN)
                ||
            (pos.this_thread()->shashinQuiescentCapablancaMaxScore))?MCP_limit:futility_move_count(improving, depth));
          //goodStatic6 + nodeTypeHistory end
          // Reduced depth of the next LMR search
          int lmrDepth = std::max(newDepth - reduction(improving, depth, moveCount, delta, thisThread->rootDelta), 0);

          if (   capture
              || givesCheck)
          {
              // Futility pruning for captures (~0 Elo)
              if (   !pos.empty(to_sq(move))
                  && !givesCheck
                  && !PvNode
                  && lmrDepth < 6
                  && !ss->inCheck
                  && ss->staticEval + 281 + 179 * lmrDepth + PieceValue[EG][pos.piece_on(to_sq(move))]
                   + captureHistory[movedPiece][to_sq(move)][type_of(pos.piece_on(to_sq(move)))] / 6 < alpha)
                  continue;

              // SEE based pruning (~9 Elo)
              if (!pos.see_ge(move, Value(-203) * depth))
                  continue;
          }
          else
          {
              int history =   (*contHist[0])[movedPiece][to_sq(move)]
                            + (*contHist[1])[movedPiece][to_sq(move)]
                            + (*contHist[3])[movedPiece][to_sq(move)];

              // Continuation history based pruning (~2 Elo)
              if (   lmrDepth < 5
                  && history < -3875 * (depth - 1))
                  continue;

               history += 2 * thisThread->mainHistory[us][from_to(move)];

              // Futility pruning: parent node (~9 Elo)
              if (   !ss->inCheck
                  && lmrDepth < 11
                  && ss->staticEval + 122 + 138 * lmrDepth + history / 60 <= alpha)
                  continue;

              // Prune moves with negative SEE (~3 Elo)
              if (!pos.see_ge(move, Value(-25 * lmrDepth * lmrDepth - 20 * lmrDepth)))
                  continue;
          }
      }

      // Step 15. Extensions (~66 Elo)
      // We take care to not overdo to avoid search getting stuck.
      if (ss->ply < thisThread->rootDepth * 2)
      {
          // Singular extension search (~58 Elo). If all moves but one fail low on a
          // search of (alpha-s, beta-s), and just one fails high on (alpha, beta),
          // then that move is singular and should be extended. To verify this we do
          // a reduced search on all the other moves but the ttMove and if the
          // result is lower than ttValue minus a margin, then we will extend the ttMove.
          if (   !rootNode
              &&  depth >= 4 - (thisThread->previousDepth > 27) + 2 * (PvNode && tte->is_pv())
              &&  move == ttMove
              && !excludedMove // Avoid recursive singular search
           /* &&  ttValue != VALUE_NONE Already implicit in the next condition */
              &&  abs(ttValue) < VALUE_KNOWN_WIN
              && (tte->bound() & BOUND_LOWER)
              &&  tte->depth() >= depth - 3)
          {
              //useRinSMS begin
              int singularBetaConstant = (ss-1)->currentReduction > 3 ? 2 :
                                         (ss-1)->currentReduction > 0 ? 3 : 4; 
              Value singularBeta = ttValue - singularBetaConstant * depth;                                        
              //useRinSMS end
              Depth singularDepth = (depth - 1 + (!ss->inCheck && eval - ss->staticEval < -250)) / 2;//sextDepthEval2

              ss->excludedMove = move;
              value = search<NonPV>(pos, ss, singularBeta - 1, singularBeta, singularDepth, cutNode, mcts);//mcts
              ss->excludedMove = MOVE_NONE;

              if (value < singularBeta)
              {
                  extension = 1;
                  singularQuietLMR = !ttCapture;
                  
                  // Avoid search explosion by limiting the number of double extensions
                  if (  !PvNode
                      && value < singularBeta - 26
                      && ss->doubleExtensions <= 8)
                      extension = 2;
              }

              // Multi-cut pruning
              // Our ttMove is assumed to fail high, and now we failed high also on a reduced
              // search without the ttMove. So we assume this expected Cut-node is not singular,
              // that multiple moves fail high, and we can prune the whole subtree by returning
              // a soft bound.
              else if (singularBeta >= beta)
                  return singularBeta;

              // If the eval of ttMove is greater than beta, we reduce it (negative extension)
              else if (ttValue >= beta && ((goldDigger ||(pos.this_thread()->shashinQuiescentCapablancaMaxScore))||(((ss-1)->moveCount > 1) && (!gameCycle)))) //from Crystal
                  extension = std::max(-4, -(newDepth - 3)), negExt = true; //negExtR4 negExtTw12

              // If the eval of ttMove is less than alpha and value, we reduce it (negative extension)
              else if (ttValue <= alpha && ttValue <= value)
              //extDepthRed4 begin
              {
                  if (ttValue < alpha - 100 && value < alpha - 100 && (pos.this_thread()->shashinQuiescentCapablancaMaxScore))
                      depth--;
                  else
                      extension = -1;
              }
              //extDepthRed4 end
      	  }
          // Check extensions (~1 Elo)	  
          else if (   givesCheck
                   && depth > 9
                   && abs(ss->staticEval) > 71)
              extension = 1;

          // Quiet ttMove extensions (~0 Elo)
          else if (   PvNode
                   && move == ttMove
                   && move == ss->killers[0]
                   && (*contHist[0])[movedPiece][to_sq(move)] >= 5491)
              extension = 1;
      }

      // Add extension to new depth
      newDepth += extension;
      ss->doubleExtensions = (ss-1)->doubleExtensions + (extension == 2);

      skipExtensionAndPruning: //full threads search patch

      // Speculative prefetch as early as possible
      prefetch(TT.first_entry(pos.key_after(move)));

      // Update the current move (this must be done after singular extension search)
      ss->currentMove = move;
      ss->continuationHistory = &thisThread->continuationHistory[ss->inCheck]
                                                                [capture]
                                                                [movedPiece]
                                                                [to_sq(move)];

      // Step 16. Make the move
      //from Shashin begin
      lastShashinValue=pos.this_thread()->shashinValue,
      lastShashinQuiescentCapablancaMiddleHighScore=pos.this_thread()->shashinQuiescentCapablancaMiddleHighScore,
      lastShashinQuiescentCapablancaMaxScore=pos.this_thread()->shashinQuiescentCapablancaMaxScore;
      lastShashinPosKey=pos.this_thread()->shashinPosKey;
      //from Shashin end
      pos.do_move(move, st, givesCheck);
      updateShashinValues(pos,- ss->staticEval); //from Shashin
      bool doLMRStep = !(thisThread->fullSearch);//full threads patch
	  bool doDeeperSearch = false;
	  (ss+1)->distanceFromPv = ss->distanceFromPv + moveCount - 1;//dd^^

      // Step 17. Late moves reduction / extension (LMR, ~98 Elo)
      // We use various heuristics for the sons of a node after the first son has
      // been searched. In general we would like to reduce them, but there are many
      // cases where we extend a son if it has good chances to be "interesting".
      if ( doLMRStep &&   depth >= 2 &&  moveCount > sibs //full threads patch + Kelly
	   &&  moveCount > 1 + (PvNode && ss->ply <= 1)
       &&  (   !ss->ttPv
            || !capture
            || (cutNode && (ss-1)->moveCount > 1)))
      {
          Depth r = reduction(improving, depth, moveCount, delta, thisThread->rootDelta);

          // Decrease reduction if position is or has been on the PV
          // and node is not likely to fail low. (~3 Elo)
          if (   ss->ttPv
              && !likelyFailLow)
              r -= 2 + ((!(pos.this_thread()->shashinQuiescentCapablancaMaxScore))?ss->inCheck:0);//lmr_tweak^ by Shashin

          //from Crystal begin
          if ((!((pos.this_thread()->shashinQuiescentCapablancaMiddleHighScore)))
              && rootDepth > 10 && pos.king_danger())
            r -= 1;
          //from Crystal end

          // Decrease reduction if opponent's move count is high (~1 Elo)
          if ((ss-1)->moveCount > 7 && ((pos.this_thread()->shashinValue!=SHASHIN_POSITION_TAL)||(ss->moveCount <= (ss-1)->moveCount + 5))) //mcLMRint2
            r -= 1 + (captureOrPromotion && depth < 12); //lmr_moveCountCapt

		  //clcnlmr2b3d2+cnlmr14 begin
          // Increase reduction for cut nodes (~3 Elo)
		  if(cutNode){
			if(move == ss->killers[0]){
				r += 1;
			}
			else{
               r += std::clamp(!ss->ttPv - ss->ttPv 
                + !capture - capture 
                + (move != ss->killers[0]) - (move == ss->killers[0]), 1, complexity > 650 ? 2 : 3);			
			}
		  }
		  //clcnlmr2b3d2+cnlmr14 end
		  
          // Increase reduction if ttMove is a capture (~3 Elo)
          if (ttCapture && (pos.this_thread()->shashinQuiescentCapablancaMaxScore)) //official from Shashin
              r++;

          //negExtR4 begin
          if (negExt && !PvNode)
              r += 2;
          //negExtR4 end
          
          //official with Shashin begin
          // Decrease reduction at PvNodes if bestvalue
          // is vastly different from static evaluation
          if (PvNode && !ss->inCheck && abs(ss->staticEval - bestValue) > 250 
              && (!(pos.this_thread()->shashinQuiescentCapablancaMaxScore)))
              r--;
          //official with Shashin end

          // Decrease reduction for PvNodes based on depth
          //official with Shashin begin
          if (PvNode && pos.this_thread()->shashinValue!=SHASHIN_POSITION_CAPABLANCA)
          {
              r -= 1 + 15 / (3 + depth);
          }       
          //official with Shashin end
          
          //official with Shashin begin
          // Decrease reduction if ttMove has been singularly extended (~1 Elo)
          if (singularQuietLMR && pos.this_thread()->shashinValue!=SHASHIN_POSITION_CAPABLANCA)
              r--;
          //official with Shashin end

          // Increase reduction if next ply has a lot of fail high else reset count to 0
          //official with Shashin begin
          if ((ss+1)->cutoffCnt > 3 && !PvNode && (pos.this_thread()->shashinQuiescentCapablancaMaxScore) )
              r++;
          //official with Shashin end
          ss->statScore =  2 * thisThread->mainHistory[us][from_to(move)]
                         + (*contHist[0])[movedPiece][to_sq(move)]
                         + (*contHist[1])[movedPiece][to_sq(move)]
                         + (*contHist[3])[movedPiece][to_sq(move)]
                         - 4334;

          // Decrease/increase reduction for moves with a good/bad history (~30 Elo)
          r -= ss->statScore / 15914;
		  
		  //lmr_average2 begin
          if (thisThread->lmrAverage.is_greater(11, 100))
              r--;		  
		  //lmr_average2 end

          // In general we want to cap the LMR depth search at newDepth, but when
          // reduction is negative, we allow this move a limited search extension
          // beyond the first move depth. This may lead to hidden double extensions.
          Depth d = std::clamp(newDepth - r, 1, newDepth + 1);
          
          ss->currentReduction = newDepth - d;//useRinSMS
          
          value = -search<NonPV>(pos, ss+1, -(alpha+1), -alpha, d, true, mcts);//mcts

          ss->currentReduction = 0;//useRinSMS
          
		  thisThread->lmrAverage.update(value > alpha);//lmr_average2
          
		  // If the son is reduced and fails high it will be re-searched at full depth
          doFullDepthSearch = value > alpha && d < newDepth;
          doDeeperSearch = value > (alpha + 78 + 11 * (newDepth - d));
          didLMR = true;
      }
      else
      {
          doFullDepthSearch = !doLMRStep ||  !PvNode || moveCount > 1;//full threads patch
          //deeperPvS12 begin
          if(!(((pos.this_thread()->shashinValue==SHASHIN_POSITION_TAL)&&(!(pos.this_thread()->shashinQuiescentCapablancaMiddleHighScore))
                &&(pos.this_thread()->shashinQuiescentCapablancaMaxScore))
                ||
                (pos.this_thread()->shashinValue==SHASHIN_POSITION_CAPABLANCA_PETROSIAN)
                ||
                ((pos.this_thread()->shashinValue==SHASHIN_POSITION_PETROSIAN)&&(pos.this_thread()->shashinQuiescentCapablancaMiddleHighScore))))
          {
              doDeeperSearch = PvNode && captureOrPromotion && captureHistory[movedPiece][to_sq(move)][type_of(pos.captured_piece())] > 0;
          }
          //deeperPvS12 end
          didLMR = false;
      }

      // Step 18. Full depth search when LMR is skipped or fails high
      if (doFullDepthSearch)
      {
          value = -search<NonPV>(pos, ss+1, -(alpha+1), -alpha, newDepth + doDeeperSearch, !cutNode, mcts);//mcts

          // If the move passed LMR update its stats
          if (didLMR)
          {
			  //begin deepContHist
              int bonus = value > alpha ?  stat_bonus(newDepth + (pos.this_thread()->shashinQuiescentCapablancaMaxScore)?doDeeperSearch:0)
                                        : -stat_bonus(newDepth + (pos.this_thread()->shashinQuiescentCapablancaMaxScore)?doDeeperSearch:0);
			  //end deepContHist
              if (capture)
                  bonus /= 6;

              update_continuation_histories(ss, movedPiece, to_sq(move), bonus);
          }
      }

      // For PV nodes only, do a full PV search on the first move or after a fail
      // high (in the latter case search only if value < beta), otherwise let the
      // parent node fail low with value <= alpha and try another move.
      if (PvNode && (moveCount == 1 || (value > alpha && (rootNode || value < beta))))
      {
          (ss+1)->pv = pv;
          (ss+1)->pv[0] = MOVE_NONE;
          value = -search<PV>(pos, ss+1, -beta, -alpha,
							  std::min(maxNextDepth, newDepth + (rootNode && moveCount > 1 && value >= beta + 88)), false, mcts);//mcts+rnPseudofh3
      }

      // Step 19. Undo move
      pos.undo_move(move);
      revertShashinValues(pos,lastShashinValue, lastShashinQuiescentCapablancaMiddleHighScore, lastShashinQuiescentCapablancaMaxScore, lastShashinPosKey);//from Shashin
	  }//from Crystal
      assert(value > -VALUE_INFINITE && value < VALUE_INFINITE);

      // Step 20. Check for a new best move
      // Finished searching the move. If a stop occurred, the return value of
      // the search cannot be trusted, and we return immediately without
      // updating best move, PV and TT.
      if (Threads.stop.load(std::memory_order_relaxed))
      {
	  	updateShashinValues(pos,VALUE_ZERO);
	  	return VALUE_ZERO;
      }

      if (rootNode)
      {
          RootMove& rm = *std::find(thisThread->rootMoves.begin(),
                                    thisThread->rootMoves.end(), move);

          rm.averageScore = rm.averageScore != -VALUE_INFINITE ? (2 * value + rm.averageScore) / 3 : value;

          // PV move or new best move?
          if (moveCount == 1 || value > alpha)
          {
              rm.score = value;
              rm.selDepth = thisThread->selDepth;
              rm.pv.resize(1);

              assert((ss+1)->pv);

              for (Move* m = (ss+1)->pv; *m != MOVE_NONE; ++m)
                  rm.pv.push_back(*m);

              // We record how often the best move has been changed in each iteration.
              // This information is used for time management. In MultiPV mode,
              // we must take care to only do this for the first PV line.
              if (   moveCount > 1
                  && !thisThread->pvIdx)
                  ++thisThread->bestMoveChanges;
          }
          else
              // All other moves but the PV are set to the lowest value: this
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

              if (PvNode && !rootNode) // Update pv even in fail-high case
                  update_pv(ss->pv, move, (ss+1)->pv);

              if (PvNode && value < beta) // Update alpha! Always alpha < beta
              {
                  alpha = value;

                  // Reduce other moves if we have found at least one score improvement
                  if (   depth > 2
                      && depth < 7
                      && beta  <  VALUE_KNOWN_WIN
                      && alpha > -VALUE_KNOWN_WIN)
                  //redDepCmp begin
                  {
                     if ((complexity > 650) && (pos.this_thread()->shashinQuiescentCapablancaMaxScore))
                          depth /= 2;
                     else
                          depth -= s20const[depth] + (complexity <= cpx_margin[depth]) * cpx_mult[depth];//s20red4c4_t4
                  }
                  //redDepCmp end
                 assert(depth > 0);                
              }
              else
              {
                  ss->cutoffCnt++;
                  assert(value >= beta); // Fail high
                  break;
              }
          }
      }
      else
        ss->cutoffCnt = 0;

      // If the move is worse than some previously searched move, remember it to update its stats later
      if (move != bestMove)
      {
          if (capture && captureCount < 32)
              capturesSearched[captureCount++] = move;

          else if (!capture && quietCount < 64)
              quietsSearched[quietCount++] = move;
      }
    }

    // The following condition would detect a stop only after move loop has been
    // completed. But in this case bestValue is valid because we have fully
    // searched our subtree, and we can anyhow save the result in TT.
    /*
       if (Threads.stop)
        return VALUE_DRAW;
    */

    // Step 21. Check for mate and stalemate
    // All legal moves have been searched and if there are no legal moves, it
    // must be a mate or a stalemate. If we are in a singular extension search then
    // return a fail low score.

    assert(moveCount || !ss->inCheck || excludedMove || !MoveList<LEGAL>(pos).size());

    if (!moveCount)
        bestValue = excludedMove ? alpha :
                    ss->inCheck  ? mated_in(ss->ply)
                                 : VALUE_DRAW;

    // If there is a move which produces search value greater than alpha we update stats of searched moves
    else if (bestMove)
        update_all_stats(pos, ss, bestMove, bestValue, beta, prevSq,
                         quietsSearched, quietCount, capturesSearched, captureCount, depth);

    // Bonus for prior countermove that caused the fail low
    else if (   (depth >= 4 || PvNode)
             && !priorCapture)
    {
		//Assign extra bonus if current node is PvNode or cutNode
        //or fail low was really bad
        bool extraBonus =    PvNode
                          || cutNode
                          || bestValue < alpha - 70 * depth;
		update_continuation_histories(ss-1, pos.piece_on(prevSq), prevSq, stat_bonus(depth) * (1 + ((pos.this_thread()->shashinValue == SHASHIN_POSITION_CAPABLANCA)?
		(extraBonus):0)));//official with Shashin		
	}

    if (PvNode)
        bestValue = std::min(bestValue, maxValue);

    // If no good move is found and the previous position was ttPv, then the previous
    // opponent move is probably good and the new position is added to the search tree.
    if (bestValue <= alpha)
        ss->ttPv = ss->ttPv || ((ss-1)->ttPv && depth > 3);

    // Write gathered information in transposition table
    if (!excludedMove && !(rootNode && thisThread->pvIdx))
        tte->save(posKey, value_to_tt(bestValue, ss->ply), ss->ttPv,
                  bestValue >= beta ? BOUND_LOWER :
                  PvNode && bestMove ? BOUND_EXACT : BOUND_UPPER,
                  depth, bestMove, ss->staticEval);

    assert(bestValue > -VALUE_INFINITE && bestValue < VALUE_INFINITE);

    return bestValue;
  }


  // qsearch() is the quiescence search function, which is called by the main search
  // function with zero depth, or recursively with further decreasing depth per call.
  // (~155 elo)
  template <NodeType nodeType>
  Value qsearch(Position& pos, Stack* ss, Value alpha, Value beta, Depth depth) {

    static_assert(nodeType != Root);
    constexpr bool PvNode = nodeType == PV;

    assert(alpha >= -VALUE_INFINITE && alpha < beta && beta <= VALUE_INFINITE);
    assert(PvNode || (alpha == beta - 1));
    assert(depth <= 0);

    Move pv[MAX_PLY+1];
    StateInfo st;
    ASSERT_ALIGNED(&st, Eval::NNUE::CacheLineSize);

    TTEntry* tte;
    Key posKey;
    Move ttMove, move, bestMove;
    Depth ttDepth;
    Value bestValue, value, ttValue, futilityValue, futilityBase;
    bool pvHit, givesCheck, capture, gameCycle=false; //from Crystal
    int moveCount;

    if (PvNode)
    {
        (ss+1)->pv = pv;
        ss->pv[0] = MOVE_NONE;
    }

    Thread* thisThread = pos.this_thread();
    bestMove = MOVE_NONE;
    ss->inCheck = pos.checkers();
    moveCount = 0;

    //from Crystal begin
    if (pos.has_game_cycle(ss->ply))
    {
		gameCycle = true;
    }
    //from Crystal end

    // Check for an immediate draw or maximum ply reached
    if (   pos.is_draw(ss->ply)
        || ss->ply >= MAX_PLY)
    {
      //from Shashin begin
      if(ss->ply >= MAX_PLY && !ss->inCheck)
      { 
          //from ShashChess begin
	      Value evalPos=evaluate(pos);
          if(pos.key()!=pos.this_thread()->shashinPosKey)
          {
             updateShashinValues(pos,evalPos);
	      }
          return evalPos;
      }
      updateShashinValues(pos,VALUE_DRAW);
      return VALUE_DRAW;
      //from ShashChess end
    }
    assert(0 <= ss->ply && ss->ply < MAX_PLY);

    // Decide whether or not to include checks: this fixes also the type of
    // TT entry depth that we are going to use. Note that in qsearch we use
    // only two types of depth in TT: DEPTH_QS_CHECKS or DEPTH_QS_NO_CHECKS.
    ttDepth = ss->inCheck || depth >= DEPTH_QS_CHECKS ? DEPTH_QS_CHECKS
                                                  : DEPTH_QS_NO_CHECKS;
    // Transposition table lookup
    posKey = pos.key();
    tte = TT.probe(posKey, ss->ttHit);
    ttValue = ss->ttHit ? value_from_tt(tte->value(), ss->ply, pos.rule50_count()) : VALUE_NONE;
    ttMove = ss->ttHit ? tte->move() : MOVE_NONE;
    pvHit = ss->ttHit && tte->is_pv();

    if (  !PvNode
        && ss->ttHit
	&& ((pos.this_thread()->shashinQuiescentCapablancaMaxScore)||(!gameCycle)) //from Crystal
        && tte->depth() >= ttDepth
        && ttValue != VALUE_NONE // Only in case of TT access race
        && (tte->bound() & (ttValue >= beta ? BOUND_LOWER : BOUND_UPPER)))
        return ttValue;

    // Evaluate the position statically
    if (ss->inCheck)
    {
        ss->staticEval = VALUE_NONE;
        //Begin from ShashChess
        if(pos.key()!=pos.this_thread()->shashinPosKey)
        {
            updateShashinValues(pos,ss->staticEval);
        }
        //End from ShashChess
        bestValue = futilityBase = -VALUE_INFINITE;
    }
    else
    {
        if (ss->ttHit)
        {

	  // Never assume anything on values stored in TT
	  if ((ss->staticEval = bestValue = tte->eval()) == VALUE_NONE)
	    //from ShashChess begin
	    {
	      ss->staticEval = bestValue = evaluate(pos);
          if(pos.key()!=pos.this_thread()->shashinPosKey)
          {
	        updateShashinValues(pos,ss->staticEval);
          }
	    }
	    //from ShashChess end

// ttValue can be used as a better position evaluation (~7 Elo)
	  if (ttValue != VALUE_NONE
	      && (tte->bound() & (ttValue > bestValue ? BOUND_LOWER : BOUND_UPPER)))
	  {
	      bestValue = ttValue;
	      updateShashinValues(pos,ttValue);//from ShashChess
	  }

        }
        else
        {
            // In case of null move search use previous static eval with a different sign
            if((ss-1)->currentMove != MOVE_NULL)
            { //from ShashChess begin
                ss->staticEval = bestValue = evaluate(pos);
            } //from ShashChess end
            else
            {
                ss->staticEval = bestValue = -(ss-1)->staticEval;
            }
            if(pos.key()!=pos.this_thread()->shashinPosKey)
            {
                updateShashinValues(pos,ss->staticEval);//from ShashChess
            }
        }
        // Stand pat. Return immediately if static value is at least beta
        if (bestValue >= beta)
        {
            // Save gathered info in transposition table
            if (!ss->ttHit)
                tte->save(posKey, value_to_tt(bestValue, ss->ply), false, BOUND_LOWER,
                          DEPTH_NONE, MOVE_NONE, ss->staticEval);

            return bestValue;
        }

        if (PvNode && bestValue > alpha)
            alpha = bestValue;

        futilityBase = bestValue + 118;
    }

    const PieceToHistory* contHist[] = { (ss-1)->continuationHistory, (ss-2)->continuationHistory,
                                          nullptr                   , (ss-4)->continuationHistory,
                                          nullptr                   , (ss-6)->continuationHistory };

    // Initialize a MovePicker object for the current position, and prepare
    // to search the moves. Because the depth is <= 0 here, only captures,
    // queen promotions, and other checks (only if depth >= DEPTH_QS_CHECKS)
    // will be generated.
    Square prevSq = to_sq((ss-1)->currentMove);
    MovePicker mp(pos, ttMove, depth, &thisThread->mainHistory,
                                      &thisThread->captureHistory,
                                      contHist,
                                      prevSq);

    int quietCheckEvasions = 0;

    // Loop through the moves until no moves remain or a beta cutoff occurs
    while ((move = mp.next_move()) != MOVE_NONE)
    {
      assert(is_ok(move));

      // Check for legality
      if (!pos.legal(move))
          continue;

      givesCheck = pos.gives_check(move);
      capture = pos.capture(move);

      moveCount++;

      // Futility pruning and moveCount pruning (~5 Elo)
      if (    bestValue > VALUE_TB_LOSS_IN_MAX_PLY
          && !givesCheck
          &&  to_sq(move) != prevSq
          &&  futilityBase > -VALUE_KNOWN_WIN
          &&  type_of(move) != PROMOTION)
      {

          if (moveCount > 2)
              continue;

          futilityValue = futilityBase + PieceValue[EG][pos.piece_on(to_sq(move))];

          if (futilityValue <= alpha)
          {
              bestValue = std::max(bestValue, futilityValue);
              continue;
          }

          if (futilityBase <= alpha && !pos.see_ge(move, VALUE_ZERO + 1))
          {
              bestValue = std::max(bestValue, futilityBase);
              continue;
          }
      }

      // Do not search moves with negative SEE values (~5 Elo)
      if (    bestValue > VALUE_TB_LOSS_IN_MAX_PLY
          && !pos.see_ge(move))
          continue;

      // Speculative prefetch as early as possible
      prefetch(TT.first_entry(pos.key_after(move)));

      ss->currentMove = move;
      ss->continuationHistory = &thisThread->continuationHistory[ss->inCheck]
                                                                [capture]
                                                                [pos.moved_piece(move)]
                                                                [to_sq(move)];

      // Continuation history based pruning (~2 Elo)
      if (  !capture
          && bestValue > VALUE_TB_LOSS_IN_MAX_PLY
          && (*contHist[0])[pos.moved_piece(move)][to_sq(move)] < CounterMovePruneThreshold
          && (*contHist[1])[pos.moved_piece(move)][to_sq(move)] < CounterMovePruneThreshold)
          continue;

      // movecount pruning for quiet check evasions
      if (  bestValue > VALUE_TB_LOSS_IN_MAX_PLY
          && quietCheckEvasions > 1
          && !capture
          && ss->inCheck)
          continue;

      quietCheckEvasions += !capture && ss->inCheck;
      
      //from Shashin begin
      int lastShashinValue=pos.this_thread()->shashinValue,
      lastShashinQuiescentCapablancaMiddleHighScore=pos.this_thread()->shashinQuiescentCapablancaMiddleHighScore,
      lastShashinQuiescentCapablancaMaxScore=pos.this_thread()->shashinQuiescentCapablancaMaxScore,
      lastShashinPosKey=pos.this_thread()->shashinPosKey;
      //from Shashin end
      // Make and search the move
      pos.do_move(move, st, givesCheck);
      //from Shashin begin
      updateShashinValues(pos,- ss->staticEval);
      //From Shashin end
      value = -qsearch<nodeType>(pos, ss+1, -beta, -alpha, depth - 1);
      pos.undo_move(move);
      //from Shashin begin
      revertShashinValues(pos,lastShashinValue, lastShashinQuiescentCapablancaMiddleHighScore, lastShashinQuiescentCapablancaMaxScore, lastShashinPosKey);
      //from Shashin end
      assert(value > -VALUE_INFINITE && value < VALUE_INFINITE);

      // Check for a new best move
      if (value > bestValue)
      {
          bestValue = value;

          if (value > alpha)
          {
              bestMove = move;

              if (PvNode) // Update pv even in fail-high case
                  update_pv(ss->pv, move, (ss+1)->pv);

              if (PvNode && value < beta) // Update alpha here!
                  alpha = value;
              else
                  break; // Fail high
          }
       }
    }
    //from Sugar
    if (openingVariety && (bestValue + (openingVariety * PawnValueEg / 100) >= 0 ) && (pos.count<PAWN>() > 12))
	  bestValue += rand() % (openingVariety + 1);
    //end from Sugar
    // All legal moves have been searched. A special case: if we're in check
    // and no legal moves were found, it is checkmate.
    if (ss->inCheck && bestValue == -VALUE_INFINITE)
    {
        assert(!MoveList<LEGAL>(pos).size());

        return mated_in(ss->ply); // Plies to mate from the root
    }

    //qs_update_end begin
    if (bestMove && ss->depth <= 0)
        update_all_stats(pos, ss, bestMove, bestValue, beta, prevSq,
                         NULL, 0, NULL, 0, 2);
    //qs_update_end end	
	
    // Save gathered info in transposition table
    tte->save(posKey, value_to_tt(bestValue, ss->ply), pvHit,
              bestValue >= beta ? BOUND_LOWER : BOUND_UPPER,
              ttDepth, bestMove, ss->staticEval);

    assert(bestValue > -VALUE_INFINITE && bestValue < VALUE_INFINITE);

    return bestValue;
  }


  // value_to_tt() adjusts a mate or TB score from "plies to mate from the root" to
  // "plies to mate from the current position". Standard scores are unchanged.
  // The function is called before storing a value in the transposition table.

  Value value_to_tt(Value v, int ply) {

    assert(v != VALUE_NONE);

    return  v >= VALUE_TB_WIN_IN_MAX_PLY  ? v + ply
          : v <= VALUE_TB_LOSS_IN_MAX_PLY ? v - ply : v;
  }


  // value_from_tt() is the inverse of value_to_tt(): it adjusts a mate or TB score
  // from the transposition table (which refers to the plies to mate/be mated from
  // current position) to "plies to mate/be mated (TB win/loss) from the root". However,
  // for mate scores, to avoid potentially false mate scores related to the 50 moves rule
  // and the graph history interaction, we return an optimal TB score instead.

  Value value_from_tt(Value v, int ply, int r50c) {

    if (v == VALUE_NONE)
        return VALUE_NONE;

    if (v >= VALUE_TB_WIN_IN_MAX_PLY)  // TB win or better
    {
        if (v >= VALUE_MATE_IN_MAX_PLY && VALUE_MATE - v > 99 - r50c)
            return VALUE_MATE_IN_MAX_PLY - 1; // do not return a potentially false mate score

        return v - ply;
    }

    if (v <= VALUE_TB_LOSS_IN_MAX_PLY) // TB loss or worse
    {
        if (v <= VALUE_MATED_IN_MAX_PLY && VALUE_MATE + v > 99 - r50c)
            return VALUE_MATED_IN_MAX_PLY + 1; // do not return a potentially false mate score

        return v + ply;
    }

    return v;
  }


  // update_pv() adds current move and appends child pv[]

  void update_pv(Move* pv, Move move, const Move* childPv) {

    for (*pv++ = move; childPv && *childPv != MOVE_NONE; )
        *pv++ = *childPv++;
    *pv = MOVE_NONE;
  }


  // update_all_stats() updates stats at the end of search() when a bestMove is found

  void update_all_stats(const Position& pos, Stack* ss, Move bestMove, Value bestValue, Value beta, Square prevSq,
                        Move* quietsSearched, int quietCount, Move* capturesSearched, int captureCount, Depth depth) {

    Color us = pos.side_to_move();
    Thread* thisThread = pos.this_thread();
    CapturePieceToHistory& captureHistory = thisThread->captureHistory;
    Piece moved_piece = pos.moved_piece(bestMove);
    PieceType captured = type_of(pos.piece_on(to_sq(bestMove)));
    int bonus1 = stat_bonus(depth + 1);

    if (!pos.capture(bestMove))
    {
        int bonus2 = bestValue > beta + PawnValueMg ? bonus1               // larger bonus
                                                    : stat_bonus(depth);   // smaller bonus

        // Increase stats for the best move in case it was a quiet move
        update_quiet_stats(pos, ss, bestMove, bonus2);

        // Decrease stats for all non-best quiet moves
        for (int i = 0; i < quietCount; ++i)
        {
            thisThread->mainHistory[us][from_to(quietsSearched[i])] << -bonus2;
            update_continuation_histories(ss, pos.moved_piece(quietsSearched[i]), to_sq(quietsSearched[i]), -bonus2);
        }
    }
    else
        // Increase stats for the best move in case it was a capture move
        captureHistory[moved_piece][to_sq(bestMove)][captured] << bonus1;

    // Extra penalty for a quiet early move that was not a TT move or
    // main killer move in previous ply when it gets refuted.
    if (   ((ss-1)->moveCount == 1 + (ss-1)->ttHit || ((ss-1)->currentMove == (ss-1)->killers[0]))
        && !pos.captured_piece())
            update_continuation_histories(ss-1, pos.piece_on(prevSq), prevSq, -bonus1);

    // Decrease stats for all non-best capture moves
    for (int i = 0; i < captureCount; ++i)
    {
        moved_piece = pos.moved_piece(capturesSearched[i]);
        captured = type_of(pos.piece_on(to_sq(capturesSearched[i])));
        captureHistory[moved_piece][to_sq(capturesSearched[i])][captured] << -bonus1;
    }
  }


  // update_continuation_histories() updates histories of the move pairs formed
  // by moves at ply -1, -2, -4, and -6 with current move.

  void update_continuation_histories(Stack* ss, Piece pc, Square to, int bonus) {

    for (int i : {1, 2, 4, 6})
    {
        // Only update first 2 continuation histories if we are in check
        if (ss->inCheck && i > 2)
            break;
        if (is_ok((ss-i)->currentMove))
            (*(ss-i)->continuationHistory)[pc][to] << bonus;
    }
  }


  // update_quiet_stats() updates move sorting heuristics

  void update_quiet_stats(const Position& pos, Stack* ss, Move move, int bonus) {

    // Update killers
    if (ss->killers[0] != move)
    {
        ss->killers[1] = ss->killers[0];
        ss->killers[0] = move;
    }

    Color us = pos.side_to_move();
    Thread* thisThread = pos.this_thread();
    thisThread->mainHistory[us][from_to(move)] << bonus;
    update_continuation_histories(ss, pos.moved_piece(move), to_sq(move), bonus);

    // Update countermove history
    if (is_ok((ss-1)->currentMove))
    {
        Square prevSq = to_sq((ss-1)->currentMove);
        thisThread->counterMoves[pos.piece_on(prevSq)][prevSq] = move;
    }
  }

  //from true handicap mode begin
  // When playing with strength handicap, choose best move among a set of RootMoves
  // using a statistical rule dependent on 'level'. Idea by Heinz van Saanen.
  
  /*Move Skill::pick_best(size_t multiPV) {

    const RootMoves& rootMoves = Threads.main()->rootMoves;
    static PRNG rng(now()); // PRNG sequence should be non-deterministic

    // RootMoves are already sorted by score in descending order
    Value topScore = rootMoves[0].score;
    int delta = std::min(topScore - rootMoves[multiPV - 1].score, PawnValueMg);
    double weakness = 120 - 2 * level;
    int maxScore = -VALUE_INFINITE;

    // Choose best move. For each move score we add two terms, both dependent on
    // weakness. One is deterministic and bigger for weaker levels, and one is
    // random. Then we choose the move with the resulting highest score.
    for (size_t i = 0; i < multiPV; ++i)
    {
        // This is our magic formula
        int push = int((  weakness * int(topScore - rootMoves[i].score)
                        + delta * (rng.rand<unsigned>() % int(weakness))) / 128);

        if (rootMoves[i].score + push >= maxScore)
        {
            maxScore = rootMoves[i].score + push;
            best = rootMoves[i].pv[0];
        }
    }

    return best;
  }*/
  //from true handicap mode end

} // namespace


//mcts begin
// minimax_value() is a wrapper around the search() and qsearch() functions
// used to compute the minimax evaluation of a position at the given depth,
// from the point of view of the side to move. It does not compute PV nor
// emit anything on the output stream. Note: you can call this function
// with depth == DEPTH_ZERO to compute the quiescence value of the position.

Value minimax_value(Position& pos, Search::Stack* ss, Depth depth) {

//    Threads.stopOnPonderhit = Threads.stop = false;
  Value alpha = -VALUE_INFINITE;
  Value beta = VALUE_INFINITE;
  Move pv[MAX_PLY+1];
  ss->pv = pv;

/*   if (pos.should_debug())
  {
      debug << "Entering minimax_value() for the following position:" << std::endl;
      debug << pos << std::endl;
      hit_any_key();
  }*/

  Value value = search<PV>(pos, ss, alpha, beta, depth, false, true);

  // Have we found a "mate in x"?
  if (   Limits.mate
         && value >= VALUE_MATE_IN_MAX_PLY
         && VALUE_MATE - value <= 2 * Limits.mate)
	Threads.stop = true;

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

Value minimax_value(Position& pos, Search::Stack* ss, Depth depth, Value alpha, Value beta) {

//    Threads.stopOnPonderhit = Threads.stop = false;
//   alpha = -VALUE_INFINITE;
//   beta = VALUE_INFINITE;
  Move pv[MAX_PLY+1];
  ss->pv = pv;

/*   if (pos.should_debug())
  {
      debug << "Entering minimax_value() for the following position:" << std::endl;
      debug << pos << std::endl;
      hit_any_key();
  }*/
  Value value=VALUE_ZERO;
  Value delta = Value(18);

  while (!Threads.stop.load(std::memory_order_relaxed))
  {
      value = search<PV>(pos, ss, alpha, beta, depth, false, false);
      if (value <= alpha)
      {
          beta = (alpha + beta) / 2;
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
  if (   Limits.mate
		 && value >= VALUE_MATE_IN_MAX_PLY
		 && VALUE_MATE - value <= 2 * Limits.mate)
	Threads.stop = true; 

/*    if (pos.should_debug())
  {
      debug << pos << std::endl;
      debug << "... exiting minimax_value() with value = " << value << std::endl;
      hit_any_key();
  }
*/
  return value;
}
//mcts end

/// MainThread::check_time() is used to print debug info and, more importantly,
/// to detect when we are out of available time and thus stop the search.

void MainThread::check_time() {

  if (--callsCnt > 0)
      return;

  // When using nodes, ensure checking rate is not lower than 0.1% of nodes
  callsCnt = Limits.nodes ? std::min(1024, int(Limits.nodes / 1024)) : 1024;

  static TimePoint lastInfoTime = now();

  TimePoint elapsed = Time.elapsed();
  TimePoint tick = Limits.startTime + elapsed;

  if (tick - lastInfoTime >= 1000)
  {
      lastInfoTime = tick;
      dbg_print();
  }

  // We should not stop pondering until told so by the GUI
  if (ponder)
      return;

  if (   (Limits.use_time_management() && (elapsed > Time.maximum() - 10 || stopOnPonderhit))
      || (Limits.movetime && elapsed >= Limits.movetime)
      || (Limits.nodes && Threads.nodes_searched() >= (uint64_t)Limits.nodes))
      Threads.stop = true;
}


/// UCI::pv() formats PV information according to the UCI protocol. UCI requires
/// that all (if any) unsearched PV lines are sent using a previous search score.

string UCI::pv(const Position& pos, Depth depth, Value alpha, Value beta) {

  std::stringstream ss;
  TimePoint elapsed = Time.elapsed() + 1;
  const RootMoves& rootMoves = pos.this_thread()->rootMoves;
  size_t pvIdx = pos.this_thread()->pvIdx;
  size_t multiPV = std::min((size_t)Options["MultiPV"], rootMoves.size());
  uint64_t nodesSearched = Threads.nodes_searched();
  uint64_t tbHits = Threads.tb_hits() + (TB::RootInTB ? rootMoves.size() : 0);

  for (size_t i = 0; i < multiPV; ++i)
  {
      bool updated = rootMoves[i].score != -VALUE_INFINITE;

      if (depth == 1 && !updated && i > 0)
          continue;

      Depth d = updated ? depth : std::max(1, depth - 1);
      Value v = updated ? rootMoves[i].score : rootMoves[i].previousScore;

      if (v == -VALUE_INFINITE)
          v = VALUE_ZERO;

      bool tb = TB::RootInTB && abs(v) < VALUE_MATE_IN_MAX_PLY;
      v = tb ? rootMoves[i].tbScore : v;

      if (ss.rdbuf()->in_avail()) // Not at first line
          ss << "\n";

      ss << "info"
         << " depth "    << d
         << " seldepth " << rootMoves[i].selDepth
         << " multipv "  << i + 1
         << " score "    << UCI::value(v);

      if (Options["UCI_ShowWDL"])
          ss << UCI::wdl(v, pos.game_ply());

      if (!tb && i == pvIdx)
          ss << (v >= beta ? " lowerbound" : v <= alpha ? " upperbound" : "");

      ss << " nodes "    << nodesSearched
         << " nps "      << nodesSearched * 1000 / elapsed;

      if (elapsed > 1000) // Earlier makes little sense
          ss << " hashfull " << TT.hashfull();

      ss << " tbhits "   << tbHits
         << " time "     << elapsed
         << " pv";

      for (Move m : rootMoves[i].pv)
          ss << " " << UCI::move(m, pos.is_chess960());
  }

  return ss.str();
}


/// RootMove::extract_ponder_from_tt() is called in case we have no ponder move
/// before exiting the search, for instance, in case we stop the search during a
/// fail high at root. We try hard to have a ponder move to return to the GUI,
/// otherwise in case of 'ponder on' we have nothing to think on.

bool RootMove::extract_ponder_from_tt(Position& pos) {

    StateInfo st;
    ASSERT_ALIGNED(&st, Eval::NNUE::CacheLineSize);

    bool ttHit;

    assert(pv.size() == 1);

    if (pv[0] == MOVE_NONE)
        return false;

    pos.do_move(pv[0], st);
    TTEntry* tte = TT.probe(pos.key(), ttHit);

    if (ttHit)
    {
        Move m = tte->move(); // Local copy to be SMP safe
        if (MoveList<LEGAL>(pos).contains(m))
            pv.push_back(m);
    }

    pos.undo_move(pv[0]);
    return pv.size() > 1;
}

void Tablebases::rank_root_moves(Position& pos, Search::RootMoves& rootMoves) {

    RootInTB = false;
    UseRule50 = bool(Options["Syzygy50MoveRule"]);
    ProbeDepth = int(Options["SyzygyProbeDepth"]);
    Cardinality = int(Options["SyzygyProbeLimit"]);
    bool dtz_available = true;

    // Tables with fewer pieces than SyzygyProbeLimit are searched with
    // ProbeDepth == DEPTH_ZERO
    if (Cardinality > MaxCardinality)
    {
        Cardinality = MaxCardinality;
        ProbeDepth = 0;
    }

    if (Cardinality >= popcount(pos.pieces()) && !pos.can_castle(ANY_CASTLING))
    {
        // Rank moves using DTZ tables
        RootInTB = root_probe(pos, rootMoves);

        if (!RootInTB)
        {
            // DTZ tables are missing; try to rank moves using WDL tables
            dtz_available = false;
            RootInTB = root_probe_wdl(pos, rootMoves);
        }
    }

    if (RootInTB)
    {
        // Sort moves according to TB rank
        std::stable_sort(rootMoves.begin(), rootMoves.end(),
                  [](const RootMove &a, const RootMove &b) { return a.tbRank > b.tbRank; } );

        // Probe during search only if DTZ is not available and we are winning
        if (dtz_available || rootMoves[0].tbScore <= VALUE_DRAW)
            Cardinality = 0;
    }
    else
    {
        // Clean up if root_probe() and root_probe_wdl() have failed
        for (auto& m : rootMoves)
            m.tbRank = 0;
    }
}

//Kelly begin
void putGameLineIntoLearningTable()
{
  double learning_rate = 0.5;
  double gamma = 0.99;

  if (gameLine.size() > 1)
  {
      for (size_t index = gameLine.size() - 1; index > 0; index--)
      {
          int currentScore = gameLine[index - 1].learningMove.score * 100 / PawnValueEg;
          int nextScore = gameLine[index].learningMove.score * 100 / PawnValueEg;

          currentScore = currentScore * (1 - learning_rate) +
              learning_rate * (gamma * nextScore);

          gameLine[index - 1].learningMove.score = currentScore * PawnValueEg / 100;

          LD.add_new_learning(gameLine[index - 1].key, gameLine[index - 1].learningMove);
      }

      gameLine.clear();
  }
}

void setStartPoint()
{
  useLearning = true;
  LD.resume();
}
//Kelly end

} // namespace Stockfish
