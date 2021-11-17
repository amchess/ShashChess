/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2015 Marco Costalba, Joona Kiiski, Tord Romstad
  Copyright (C) 2015-2017 Marco Costalba, Joona Kiiski, Gary Linscott, Tord Romstad

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef MONTECARLO_H_INCLUDED
#define MONTECARLO_H_INCLUDED

#include <unordered_map>

#include "position.h"
#include "thread.h"
#include "types.h"

namespace Stockfish {
// The data structures for the Monte Carlo algorithm

struct mctsNodeInfo;
struct Edge;

typedef double Reward;
typedef mctsNodeInfo* mctsNode;

enum EdgeStatistic {STAT_UCB, STAT_VISITS, STAT_MEAN, STAT_PRIOR};

class MonteCarlo {
public:

  // Constructors
  MonteCarlo(Position& p);

  // The main function of the class
  void search();

  // The high-level description of the Monte-Carlo algorithm
  void create_root();
  bool computational_budget();
  mctsNode tree_policy();
  Reward playout_policy(mctsNode node);
  void backup(Reward r, bool AB_Mode);
  Edge* best_child(mctsNode node, EdgeStatistic statistic);

  // The UCB formula
  double ucb(mctsNode node, Edge& edge, bool priorMode);

  // Playing moves
  mctsNode current_node();
  bool is_root(mctsNode node);
  bool is_terminal(mctsNode node);
  void do_move(Move move);
  void undo_move();
  void generate_moves();

  // Evaluations of nodes in the tree
  Reward value_to_reward(Value v);
  Value reward_to_value(Reward r);
  Value evaluate_with_minimax(Depth d);
  Value evaluate_with_minimax(Depth d, mctsNode node);
  Reward evaluate_terminal();
  Reward calculate_prior(Move m);
  void add_prior_to_node(mctsNode node, Move m, Reward prior);

  // Tweaking the exploration algorithm
  void default_parameters();
  void set_exploration_constant(double C);
  double exploration_constant();

  // Output of results
  bool should_output_result();
  void emit_principal_variation();

  // Testing and debugging
  std::string params();
  void debug_node();
  void debug_edge();
  void debug_tree_stats();
  void test();

private:

  // Data members
  Position&       pos;                  // The current position of the tree
  mctsNode            root;                 // A pointer to the root

  // Counters and statistics
  int             ply;
  int             maximumPly;
  long            descentCnt;
  long            playoutCnt;
  long            doMoveCnt;
  long            priorCnt;
  TimePoint       startTime;
  TimePoint       lastOutputTime;
  
  double max_epsilon = 0.99;
  double min_epsilon = 0.00;
  double decay_rate = 0.8;
  bool AB_Rollout;

  // Flags and limits to tweak the algorithm
  long            MAX_DESCENTS;
  double          BACKUP_MINIMAX;
  double          UCB_UNEXPANDED_NODE;
  double          UCB_EXPLORATION_CONSTANT;
  double          UCB_LOSSES_AVOIDANCE;
  double          UCB_LOG_TERM_FACTOR;
  bool            UCB_USE_FATHER_VISITS;
  int             PRIOR_FAST_EVAL_DEPTH;
  int             PRIOR_SLOW_EVAL_DEPTH;

  // Some stacks to do/undo the moves: for compatibility with the alpha-beta search
  // implementation, we want to be able to reference from stack[-4] to stack[MAX_PLY+2].
  mctsNode            nodesBuffer [MAX_PLY+10],  *nodes   = nodesBuffer  + 7;
  Edge*           edgesBuffer [MAX_PLY+10],  **edges  = edgesBuffer  + 7;
  Search::Stack   stackBuffer [MAX_PLY+10],  *stack   = stackBuffer  + 7;
  StateInfo       statesBuffer[MAX_PLY+10],  *states  = statesBuffer + 7;
};


/// Edge struct stores the statistics of one edge between nodes in the Monte-Carlo tree
struct Edge {
  Move    move;
  double  visits;
  Reward  prior;
  Reward  actionValue;
  Reward  meanActionValue;
};

// Comparison functions for edges
struct COMPARE_PRIOR {
  bool operator()(Edge a, Edge b) const
      { return a.prior > b.prior; }
};

struct COMPARE_ROBUST_CHOICE {
  bool operator()(Edge a, Edge b) const
      { return (10 * a.visits + a.prior > 10 * b.visits + b.prior); }
};

extern COMPARE_PRIOR ComparePrior;
extern COMPARE_ROBUST_CHOICE CompareRobustChoice;

const int MAX_CHILDREN = 128;

/// NodeInfo struct stores information in a node of the Monte-Carlo tree
struct mctsNodeInfo {
public:
  Move  last_move()      { return lastMove; }
  Edge* children_list()  { return &(children[0]); }
  Spinlock lock;
  // Data members
  Key         key1            = 0;         // Zobrist hash of all pieces, including pawns
  Key         key2            = 0;         // Zobrist hash of pawns
  long        node_visits     = 0;         // number of visits by the Monte-Carlo algorithm
  int         number_of_sons  = 0;         // total number of legal moves
  int         expandedSons    = 0;         // number of sons expanded by the Monte-Carlo algorithm
  Move        lastMove        = MOVE_NONE; // the move between the parent and this node
  Edge        children[MAX_CHILDREN] = {};
  Depth deep = 1;
  Value ttValue = VALUE_NONE;
  bool AB = false;
};


// The Monte-Carlo tree is stored implicitly in one big hash table
typedef std::unordered_multimap<Key, mctsNodeInfo> MCTSHashTable;
mctsNode get_node(const Position& pos, bool createMode);
extern MCTSHashTable MCTS;
}
#endif // #ifndef MONTECARLO_H_INCLUDED
