/*
  ShashChess, a UCI chess playing engine derived from Stockfish
  Copyright (C) 2004-2024 Andrea Manzo, F. Ferraguti, K.Kiniama and ShashChess developers (see AUTHORS file)

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

#include <cmath>
#include <unordered_map>

#include "../position.h"
#include "../thread.h"

namespace ShashChess {
// The data structures for the Monte Carlo algorithm
typedef double Reward;

constexpr Reward REWARD_NONE  = 0.0;
constexpr Reward REWARD_MATED = 0.0;
constexpr Reward REWARD_DRAW  = 0.5;
constexpr Reward REWARD_MATE  = 1.0;

enum EdgeStatistic {
    STAT_UCB,
    STAT_VISITS,
    STAT_MEAN,
    STAT_PRIOR
};

///////////////////////////////////////////////////////////////////////////////////////
/// Edge struct stores the statistics of one edge between nodes in the Monte-Carlo tree
///////////////////////////////////////////////////////////////////////////////////////
struct Edge {
    //Default constructor
    Edge() :
        move(Move::none()),
        visits(0),
        prior(REWARD_NONE),
        actionValue(REWARD_NONE),
        meanActionValue(REWARD_NONE) {}

    //Prevent copying of this struct type
    Edge(const Edge&)            = delete;
    Edge& operator=(const Edge&) = delete;

    std::atomic<Move>   move;
    std::atomic<double> visits;
    std::atomic<Reward> prior;
    std::atomic<Reward> actionValue;
    std::atomic<Reward> meanActionValue;
};

extern size_t                           mctsThreads;
extern size_t                           mctsMultiStrategy;
extern double                           mctsMultiMinVisits;
constexpr int                           MAX_CHILDREN = MAX_MOVES;
typedef std::array<Edge*, MAX_CHILDREN> EdgeArray;

///////////////////////////////////////////////////////////////////////////////////////
/// Spinlock class: A yielding spin-lock that allows the same thread to lock the
///                 resource more than once. The resource is released only when the
///                 owner thread releases all the locks it has placed on the resource
///////////////////////////////////////////////////////////////////////////////////////
class Spinlock {

    static const size_t NO_THREAD = 0;

    std::atomic<size_t> owner;
    int                 lockCount;

   public:
    Spinlock() :
        owner(0),
        lockCount(0) {}

    //Prevent copying of this class type
    Spinlock(const Spinlock&)            = delete;
    Spinlock& operator=(const Spinlock&) = delete;

    void acquire(size_t threadId) {
        if (mctsThreads > 1)
        {
            size_t currentOwner = NO_THREAD;

            while (!owner.compare_exchange_weak(currentOwner, threadId, std::memory_order_acquire,
                                                std::memory_order_relaxed)
                   && currentOwner != threadId)
            {
                currentOwner = NO_THREAD;
                std::this_thread::yield();  //Be nice
            }

            lockCount++;
        }
    }

    void release([[maybe_unused]] size_t threadId) {
        if (mctsThreads > 1)
        {
            assert(owner.load(std::memory_order_relaxed) == threadId);

            if (--lockCount == 0)
                owner.store(0);
        }
    }
};

///////////////////////////////////////////////////////////////////////////////////////
/// NodeInfo struct stores information in a node of the Monte-Carlo tree
///////////////////////////////////////////////////////////////////////////////////////
struct mctsNodeInfo {

    //Default constructor
    mctsNodeInfo() {
        for (size_t i = 0; i < children.size(); ++i)
            children[i] = new Edge();
    }

    ~mctsNodeInfo() {
        for (size_t i = 0; i < children.size(); ++i)
            delete children[i];
    }

    //Prevent copying of this struct type
    mctsNodeInfo(const mctsNodeInfo&)            = delete;
    mctsNodeInfo& operator=(const mctsNodeInfo&) = delete;

    Spinlock lock;

    // Data members
    Key                key1           = 0;  // Zobrist hash of all pieces, including pawns
    Key                key2           = 0;  // Zobrist hash of pawns
    std::atomic<long>  node_visits    = 0;  // number of visits by the Monte-Carlo algorithm
    std::atomic<int>   number_of_sons = 0;  // total number of legal moves
    std::atomic<Move>  lastMove       = Move::none();  // the move between the parent and this node
    std::atomic<Value> ttValue        = VALUE_NONE;
    std::atomic<bool>  AB             = false;
    EdgeArray          children;
};

class MonteCarlo;
mctsNodeInfo* get_node(const MonteCarlo* mcts, const Position& pos);

///////////////////////////////////////////////////////////////////////////////////////
// The Monte-Carlo tree is stored implicitly in one big hash table
///////////////////////////////////////////////////////////////////////////////////////
typedef std::unordered_multimap<Key, mctsNodeInfo*> MCTS_MAP_BASE;
class MCTSHashTable: public MCTS_MAP_BASE {
   public:
    ~MCTSHashTable() { clear(); }

    void clear() {
        for (unsigned i = 0; i < bucket_count(); ++i)
        {
            for (auto it = begin(i); it != end(i); ++it)
                delete it->second;
        }

        MCTS_MAP_BASE::clear();
    }
};
extern MCTSHashTable MCTS;

///////////////////////////////////////////////////////////////////////////////////////
// Main MCTS search class
///////////////////////////////////////////////////////////////////////////////////////
class MonteCarlo {
    friend class AutoSpinLock;

   public:
    // Constructors
    MonteCarlo(Position& p, Search::Worker* worker, TranspositionTable& transpositionTable);

    //Prevent copying of this class type
    MonteCarlo(const MonteCarlo&)            = delete;
    MonteCarlo& operator=(const MonteCarlo&) = delete;

    // The main function of the class
    void search(ShashChess::ThreadPool&        threads,
                ShashChess::Search::LimitsType limits,
                bool                           isMainThread,
                Search::Worker*                worker);

    // The high-level description of the Monte-Carlo algorithm
    void          create_root(Search::Worker* worker);
    bool          computational_budget(ShashChess::ThreadPool&        threads,
                                       ShashChess::Search::LimitsType limits);
    mctsNodeInfo* tree_policy(ShashChess::ThreadPool&        threads,
                              ShashChess::Search::LimitsType limits);
    Reward        playout_policy(mctsNodeInfo* node);
    Value         backup(Reward r, bool AB_Mode);
    Edge*         best_child(mctsNodeInfo* node, EdgeStatistic statistic) const;

    // The UCB formula
    double ucb(const Edge* edge, long fatherVisits, bool priorMode) const;

    // Nodes and moves
    bool is_root(const mctsNodeInfo* node) const;
    bool is_terminal(mctsNodeInfo* node) const;
    void do_move(Move m);
    void undo_move();
    void generate_moves(mctsNodeInfo* node);

    // Evaluations of nodes in the tree
    [[nodiscard]] Reward value_to_reward(Value v) const;
    [[nodiscard]] Value  reward_to_value(Reward r) const;
    [[nodiscard]] Value  evaluate_with_minimax(Depth d) const;
    Value                evaluate_with_minimax(mctsNodeInfo* node, Depth d) const;
    [[nodiscard]] Reward evaluate_terminal(mctsNodeInfo* node) const;
    Reward               calculate_prior(Move m);
    void                 add_prior_to_node(mctsNodeInfo* node, Move m, Reward prior) const;

    // Tweaking the exploration algorithm
    void                 default_parameters();
    void                 set_exploration_constant(double c);
    [[nodiscard]] double exploration_constant() const;

    // Output of results
    [[nodiscard]] bool should_emit_pv(bool isMainThread) const;
    void               emit_pv(Search::Worker* worker, ShashChess::ThreadPool& threads);
    void               print_children();

   private:
    Position&                   pos;  // The current position of the tree
    ShashChess::Search::Worker* thisThread;
    TranspositionTable&         tt;
    mctsNodeInfo*               root{};  // A pointer to the root

    // Counters and statistics
    int       ply{};
    int       maximumPly{};
    TimePoint startTime{};
    TimePoint lastOutputTime{};

    double max_epsilon = 0.99;
    double min_epsilon = 0.00;
    double decay_rate  = 0.8;
    bool   AB_Rollout{};

    // Flags and limits to tweak the algorithm
    double BACKUP_MINIMAX{};
    double UCB_UNEXPANDED_NODE{};
    double UCB_EXPLORATION_CONSTANT{};
    double UCB_LOSSES_AVOIDANCE{};
    double UCB_LOG_TERM_FACTOR{};
    bool   UCB_USE_FATHER_VISITS{};
    int    PRIOR_FAST_EVAL_DEPTH{};
    int    PRIOR_SLOW_EVAL_DEPTH{};

    // Some stacks to do/undo the moves: for compatibility with the alpha-beta search
    // implementation, we want to be able to reference from stack[-4] to stack[MAX_PLY+2].
    mctsNodeInfo *nodesBuffer[MAX_PLY + 10]{}, **nodes  = nodesBuffer + 7;
    Edge *        edgesBuffer[MAX_PLY + 10]{}, **edges  = edgesBuffer + 7;
    Search::Stack stackBuffer[MAX_PLY + 10]{}, *stack   = stackBuffer + 7;
    StateInfo     statesBuffer[MAX_PLY + 10]{}, *states = statesBuffer + 7;
};
}
#endif  // #ifndef MONTECARLO_H_INCLUDED
