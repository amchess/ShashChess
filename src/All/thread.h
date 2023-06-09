/*
  ShashChess, a UCI chess playing engine derived from Stockfish
  Copyright (C) 2004-2023 The Stockfish developers (see AUTHORS file)

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

#ifndef THREAD_H_INCLUDED
#define THREAD_H_INCLUDED

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "material.h"
#include "movepick.h"
#include "pawns.h"
#include "position.h"
#include "search.h"
#include "thread_win32_osx.h"

namespace Stockfish {

/// Thread class keeps together all the thread-related stuff. We use
/// per-thread pawn and material hash tables so that once we get a
/// pointer to an entry its life time is unlimited and we don't have
/// to care about someone changing the entry under our feet.

class Thread {

  std::mutex mutex;
  std::condition_variable cv;
  size_t idx;
  bool exit = false, searching = true; // Set before starting std::thread
  NativeThread stdThread;

public:
  explicit Thread(size_t);
  virtual ~Thread();
  virtual void search();
  void clear();
  void idle_loop();
  void start_searching();
  void wait_for_search_finished();
  size_t id() const { return idx; }
  bool is_mcts() const { return isMCTS; } //montecarlo

  Pawns::Table pawnsTable;
  Material::Table materialTable;
  size_t pvIdx, pvLast;
  std::atomic<uint64_t> nodes, tbHits, bestMoveChanges;
  int selDepth, nmpMinPly, nmpSide;//crystal
  Value bestValue, optimism[COLOR_NB];
  Value pvValue;//from Crystal
  
  bool nmpGuard,nmpGuardV; //from Crystal
  Position rootPos;
  StateInfo rootState;
  Search::RootMoves rootMoves;
  Depth rootDepth, completedDepth;
  Value rootDelta;
  CounterMoveHistory counterMoves;
  ButterflyHistory mainHistory;
  CapturePieceToHistory captureHistory;
  ContinuationHistory continuationHistory[2][2];
  bool fullSearch;//full threads patch
  //begin from Shashin
  int shashinWinProbabilityRange=0;
  Key shashinPosKey;
  Depth shashinDepth=0;
  //end from Shashin
  bool isMCTS; //from montecarlo
 };


/// MainThread is a derived class specific for main thread

struct MainThread : public Thread {

  using Thread::Thread;

  void search() override;
  void check_time();

  double previousTimeReduction;
  Value bestPreviousScore;
  Value bestPreviousAverageScore;
  Value iterValue[4];
  int callsCnt;
  bool stopOnPonderhit;
  std::atomic_bool ponder;
};


/// ThreadPool struct handles all the threads-related stuff like init, starting,
/// parking and, most importantly, launching a thread. All the access to threads
/// is done through this class.

struct ThreadPool {

  void start_thinking(Position&, StateListPtr&, const Search::LimitsType&, bool = false);
  void clear();
  void set(size_t);
  void setFull(size_t);//full threads patch

  MainThread* main()        const { return static_cast<MainThread*>(threads.front()); }
  uint64_t nodes_searched() const { return accumulate(&Thread::nodes); }
  uint64_t tb_hits()        const { return accumulate(&Thread::tbHits); }
  Thread* get_best_thread() const;
  void start_searching();
  void wait_for_search_finished() const;

  std::atomic_bool stop, increaseDepth;

  auto cbegin() const noexcept { return threads.cbegin(); }
  auto begin() noexcept { return threads.begin(); }
  auto end() noexcept { return threads.end(); }
  auto cend() const noexcept { return threads.cend(); }
  auto size() const noexcept { return threads.size(); }
  auto empty() const noexcept { return threads.empty(); }

private:
  StateListPtr setupStates;
  std::vector<Thread*> threads;

  uint64_t accumulate(std::atomic<uint64_t> Thread::* member) const {

    uint64_t sum = 0;
    for (Thread* th : threads)
        sum += (th->*member).load(std::memory_order_relaxed);
    return sum;
  }
};

extern ThreadPool Threads;

} // namespace Stockfish

#endif // #ifndef THREAD_H_INCLUDED
