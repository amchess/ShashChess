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

#ifndef MONTECARLO_H_INCLUDED
#define MONTECARLO_H_INCLUDED

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../misc.h"
#include "../position.h"
#include "../search.h"
#include "../types.h"

namespace ShashChess {

class ThreadPool;
class TranspositionTable;

// A reward is the expected result of the game for the side to move, in [0, 1]
using Reward = double;

constexpr Reward REWARD_MATED = 0.0;
constexpr Reward REWARD_DRAW  = 0.5;
constexpr Reward REWARD_MATE  = 1.0;

// Set by the main thread from the UCI options before every search
extern size_t mctsThreads;
extern size_t mctsMultiStrategy;
extern double mctsMultiMinVisits;

///////////////////////////////////////////////////////////////////////////////////////
/// MctsSpinLock: a small non-recursive lock. A node is locked only for the few
/// instructions that read or write its statistics, never during a search.
///////////////////////////////////////////////////////////////////////////////////////
class MctsSpinLock {
    std::atomic<bool> locked{false};

   public:
    void lock() {
        for (int spins = 0;; ++spins)
        {
            if (!locked.load(std::memory_order_relaxed)
                && !locked.exchange(true, std::memory_order_acquire))
                return;

            if (spins > 64)
                std::this_thread::yield();
        }
    }

    void unlock() { locked.store(false, std::memory_order_release); }
};

struct MctsNode;

///////////////////////////////////////////////////////////////////////////////////////
/// MctsEdge: a move of a node. Until the position after the move is expanded
/// (child == nullptr) the edge is a leaf of the tree, valued by an alpha-beta search.
/// All the fields are protected by the lock of the node that owns the edge.
///////////////////////////////////////////////////////////////////////////////////////
struct MctsEdge {
    enum : uint8_t {
        BUSY   = 1,  // a thread is searching or expanding this leaf
        PROVEN = 2   // score is a proven mate, tablebase result or stalemate
    };

    Move      move     = Move::none();
    uint8_t   depth    = 0;  // depth of the search that gave score
    uint8_t   flags    = 0;
    uint8_t   rolls    = 0;  // deepening searches made on this leaf
    uint16_t  inflight = 0;  // virtual losses: threads below this edge right now
    uint32_t  visits   = 0;
    float     policy   = 0;           // prior probability of the move
    float     value    = 0.5f;        // reward of score
    Value     score    = VALUE_ZERO;  // for the side to move in the node, mates from the node
    double    sum      = 0;           // sum of the rewards backed up through the edge
    MctsNode* child    = nullptr;
};

///////////////////////////////////////////////////////////////////////////////////////
/// MctsNode: an expanded position. The edges follow the node in the same block of
/// memory, so a node takes what its legal moves need and nothing more.
///////////////////////////////////////////////////////////////////////////////////////
struct MctsNode {
    Key                key     = 0;
    Key                pawnKey = 0;
    std::atomic<float> best{0.5f};               // reward of the best move
    std::atomic<Value> provenScore{VALUE_NONE};  // mates from the node, or VALUE_NONE
    uint32_t           visits = 0;
    uint32_t           mark   = 0;  // generation of the last garbage collection
    uint16_t           sons   = 0;
    MctsSpinLock       lock;

    MctsEdge*       edges() { return reinterpret_cast<MctsEdge*>(this + 1); }
    const MctsEdge* edges() const { return reinterpret_cast<const MctsEdge*>(this + 1); }

    static size_t    bytes(int sons) { return sizeof(MctsNode) + sons * sizeof(MctsEdge); }
    static MctsNode* create(Key key, Key pawnKey, int sons);
    static void      destroy(MctsNode* node);
};

///////////////////////////////////////////////////////////////////////////////////////
/// MctsTree: the Monte-Carlo tree, shared by the threads and kept from one move to
/// the next. The nodes are found by the key of the position (transpositions join),
/// in a table split in shards so that the threads do not queue on a single lock.
///////////////////////////////////////////////////////////////////////////////////////
class MctsTree {
   public:
    ~MctsTree() { clear(); }

    void      clear();
    MctsNode* find(Key key, Key pawnKey);
    MctsNode* insert(MctsNode* node);  // the node now in the table, nullptr on a key clash

    // Before a search, with no thread in the tree: sets the memory the tree may use
    // and, when more than half is taken, frees what the new root does not reach
    void new_search(const Position& rootPos, size_t megabytes);

    bool   full() const { return bytes.load(std::memory_order_relaxed) >= budget; }
    size_t node_count() const { return count.load(std::memory_order_relaxed); }

   private:
    static constexpr size_t ShardCount = 4096;

    struct alignas(64) Shard {
        MctsSpinLock                       lock;
        std::unordered_map<Key, MctsNode*> map;
    };

    Shard& shard_of(Key key) { return shards[key >> 52]; }

    Shard               shards[ShardCount];
    std::atomic<size_t> bytes{0};
    std::atomic<size_t> count{0};
    size_t              budget     = size_t(64) << 20;
    uint32_t            generation = 0;
};

extern MctsTree MCTS;

///////////////////////////////////////////////////////////////////////////////////////
/// MonteCarlo: the search of one thread in the shared tree
///////////////////////////////////////////////////////////////////////////////////////
class MonteCarlo {
   public:
    MonteCarlo(Position& p, Search::Worker* w, ThreadPool& pool);

    MonteCarlo(const MonteCarlo&)            = delete;
    MonteCarlo& operator=(const MonteCarlo&) = delete;

    // Searches until stopped. Returns false, having searched nothing, when the
    // root could not be built before the stop: the caller falls back on alpha-beta.
    bool search(const Search::LimitsType& limits, bool isMainThread);

    // "info string" lines with the statistics of the root moves
    void print_children();

    // Values and rewards
    static Reward value_to_reward(Value v);
    static Value  reward_to_value(Reward r);

   private:
    struct Step {
        MctsNode* node;
        int       edge;
    };

    struct RootChoice {
        int    edge        = -1;  // the move to play
        int    mostVisited = -1;
        int    bestValued  = -1;
        Move   move        = Move::none();
        Reward reward      = REWARD_DRAW;
        Value  score       = VALUE_ZERO;
        bool   proven      = false;  // the move to play is proven
        bool   rootProven  = false;  // nothing left to search
    };

    enum Select {
        ALL_PROVEN = -1,
        ALL_BUSY   = -2
    };

    // The tree
    bool      create_root();
    void      iteration();
    int       select(const MctsNode* node, bool isRoot) const;
    void      leaf_work(Step leaf, const MctsEdge& snapshot);
    MctsNode* build_node(Depth wantedDepth, bool& terminal);
    bool      deepen(Depth depth, Value center, Value& result);
    void      backup(int last, bool drawOnPath);
    void      abandon(int last, bool leafBusy = false);
    void      update_node(MctsNode* node, bool isRoot) const;
    Reward    edge_reward(const MctsEdge& e) const;
    Reward    edge_minimax(const MctsEdge& e) const;
    bool      root_allowed(int edge) const { return rootAllowed.empty() || rootAllowed[edge]; }

    // Moves on the board of the thread
    void do_move(Move m, int moveCount);
    void undo_move();

    // The main thread: time, limits and output
    RootChoice root_choice() const;
    bool       main_thread_stop(const Search::LimitsType& limits);
    bool       should_emit_pv() const;
    void       emit_pv();
    int        reported_depth() const;

    Position&       pos;
    Search::Worker* worker;
    ThreadPool&     threads;
    MctsNode*       root = nullptr;

    std::vector<bool> rootAllowed;  // "go searchmoves", tablebase root filtering

    int  ply           = 0;
    int  maxPly        = 0;
    bool idle          = false;  // the last iteration found nothing to search
    int  rollsToExpand = 0;      // deepening searches of a leaf before it is expanded
    int  rootMinVisits = 1;

    // Statistics of this thread, for the depth sent to the GUI
    uint64_t iterations = 0;
    uint64_t depthSum   = 0;

    // Time management of the main thread
    TimePoint startTime      = 0;
    TimePoint lastOutputTime = 0;
    TimePoint lastChangeTime = 0;
    Move      lastBestMove   = Move::none();
    bool      pvChanged      = false;

    // For compatibility with the alpha-beta search we need stack[-7 .. MAX_PLY + 2]
    Step          path[MAX_PLY + 1]{};
    Search::Stack stackBuffer[MAX_PLY + 17]{}, *stack = stackBuffer + 7;
    StateInfo     states[MAX_PLY + 1]{};
};

}  // namespace ShashChess

#endif  // #ifndef MONTECARLO_H_INCLUDED
