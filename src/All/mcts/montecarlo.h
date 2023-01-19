/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2023 The Stockfish developers (see AUTHORS file)

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

#include <cmath>
#include <unordered_map>

#include "../position.h"
#include "../thread.h"

namespace Stockfish {
	// The data structures for the Monte Carlo algorithm
	typedef double Reward;

	constexpr Reward REWARD_NONE = 0.0;
	constexpr Reward REWARD_MATED = 0.0;
	constexpr Reward REWARD_DRAW = 0.5;
	constexpr Reward REWARD_MATE = 1.0;

	enum EdgeStatistic { STAT_UCB, STAT_VISITS, STAT_MEAN, STAT_PRIOR };
    int intRand(const int& min, const int& max);
	///////////////////////////////////////////////////////////////////////////////////////
	/// Edge struct stores the statistics of one edge between nodes in the Monte-Carlo tree
	///////////////////////////////////////////////////////////////////////////////////////
	struct Edge {
		//Default constructor
		Edge() : move(MOVE_NONE), visits(0), prior(REWARD_NONE), actionValue(REWARD_NONE), meanActionValue(REWARD_NONE) {}

		//Prevent copying of this struct type
		Edge(const Edge&) = delete;
		Edge& operator=(const Edge&) = delete;

		std::atomic<Move>    move;
		std::atomic<double>  visits;
		std::atomic<Reward>  prior;
		std::atomic<Reward>  actionValue;
		std::atomic<Reward>  meanActionValue;
	};

	constexpr int MAX_CHILDREN = 128;
	typedef std::array<Edge*, MAX_CHILDREN> EdgeArray;

	///////////////////////////////////////////////////////////////////////////////////////
	/// Spinlock class is a yielding spin-lock (compatible with hyperthreading machines)
	///////////////////////////////////////////////////////////////////////////////////////
	class Spinlock
	{
		std::atomic<int> lock;

	public:
		Spinlock() { lock = 1; }					// Init here to workaround a bug with MSVC 2013

		//Prevent copying of this class type
		Spinlock(const Spinlock&) = delete;
		Spinlock& operator=(const Spinlock&) = delete;

		void acquire()
		{
			while (lock.fetch_sub(1, std::memory_order_acquire) != 1)
			{
				while (lock.load(std::memory_order_relaxed) <= 0)
				{
					//Be nice to hyperthreading
					std::this_thread::yield();
				}
			}
		}

		void release()
		{
			lock.store(1, std::memory_order_release);
		}
	};

	extern int mctsThreads;

	class AutoSpinLock
	{
	private:
		Spinlock& _sl;

	public:
		AutoSpinLock(Spinlock& sl) : _sl(sl) { if (mctsThreads > 1) _sl.acquire(); }
		~AutoSpinLock() { if (mctsThreads > 1) _sl.release(); }
	};

	///////////////////////////////////////////////////////////////////////////////////////
	/// NodeInfo struct stores information in a node of the Monte-Carlo tree
	///////////////////////////////////////////////////////////////////////////////////////
	struct mctsNodeInfo {
		//Default constructor
		mctsNodeInfo()
		{
			for (size_t i = 0; i < children.size(); ++i)
				children[i] = new Edge();
		}

		~mctsNodeInfo()
		{
			for (size_t i = 0; i < children.size(); ++i)
				delete children[i];
		}

		//Prevent copying of this struct type
		mctsNodeInfo(const mctsNodeInfo&) = delete;
		mctsNodeInfo& operator=(const mctsNodeInfo&) = delete;

		Move        last_move()     const { return lastMove; }
		EdgeArray& children_list() { return children; }
		Spinlock lock;

		// Data members
		Key         key1 = 0;				// Zobrist hash of all pieces, including pawns
		Key         key2 = 0;				// Zobrist hash of pawns
		std::atomic<long>        node_visits = 0;		// number of visits by the Monte-Carlo algorithm
		std::atomic<int>         number_of_sons = 0;		// total number of legal moves
		std::atomic<int>         expandedSons = 0;		// number of sons expanded by the Monte-Carlo algorithm
		std::atomic<Move>        lastMove = MOVE_NONE;	// the move between the parent and this node
		std::atomic<Depth>       deep = 1;
		std::atomic<Value>       ttValue = VALUE_NONE;
		std::atomic<bool>        AB = false;
		EdgeArray   children;
	};
	mctsNodeInfo* create_node(Key key1, Key key2);
	mctsNodeInfo* get_node(const Position& pos);
	///////////////////////////////////////////////////////////////////////////////////////
	// The Monte-Carlo tree is stored implicitly in one big hash table
	///////////////////////////////////////////////////////////////////////////////////////
	typedef std::unordered_multimap<Key, mctsNodeInfo*> MCTS_MAP_BASE;
	class MCTSHashTable : public MCTS_MAP_BASE
	{
	public:
		~MCTSHashTable()
		{
			clear();
		}

		void clear()
		{
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
	public:

		// Constructors
		MonteCarlo(Position& p);

		//Prevent copying of this class type
		MonteCarlo(const MonteCarlo&) = delete;
		MonteCarlo& operator=(const MonteCarlo&) = delete;

		// The main function of the class
		void search();

		// The high-level description of the Monte-Carlo algorithm
		void create_root();
		bool computational_budget();
		mctsNodeInfo* tree_policy();
		Reward playout_policy(mctsNodeInfo* node);
		void backup(Reward r, bool AB_Mode);
		Edge* best_child(mctsNodeInfo* node, EdgeStatistic statistic);

		// The UCB formula
		double ucb(mctsNodeInfo* node, const Edge* edge, bool priorMode);

		// Playing moves
		mctsNodeInfo* current_node();
		bool is_root(mctsNodeInfo* node);
		bool is_terminal(mctsNodeInfo* node);
		void do_move(Move m);
		void undo_move();
		void generate_moves();

		// Evaluations of nodes in the tree
		[[nodiscard]] Reward value_to_reward(Value v) const;
		[[nodiscard]] Value reward_to_value(Reward r) const;
		[[nodiscard]] Value evaluate_with_minimax(Depth d) const;
		Value evaluate_with_minimax(Depth d, mctsNodeInfo* node) const;
		[[nodiscard]] Reward evaluate_terminal();
		Reward calculate_prior(Move m);
		static void add_prior_to_node(mctsNodeInfo* node, Move m, Reward prior);

		// Tweaking the exploration algorithm
		void default_parameters();
		void set_exploration_constant(double C);
		[[nodiscard]] double exploration_constant() const;

		// Output of results
		[[nodiscard]] bool should_output_result() const;
		void emit_principal_variation();
		void print_children();

		// Testing and debugging
		[[nodiscard]] std::string params() const;
		static void debug_node();
		static void debug_edge();
		static void debug_tree_stats();
		void test();

	private:
		Position& pos;                  // The current position of the tree
		mctsNodeInfo* root{};               // A pointer to the root

		// Counters and statistics
		int             ply{};
		int             maximumPly{};
		long            descentCnt{};
		long            playoutCnt{};
		long            doMoveCnt{};
		long            priorCnt{};
		TimePoint       startTime{};
		TimePoint       lastOutputTime{};

		double          max_epsilon = 0.99;
		double          min_epsilon = 0.00;
		double          decay_rate = 0.8;
		bool            AB_Rollout{};

		// Flags and limits to tweak the algorithm
		long            MAX_DESCENTS{};
		double          BACKUP_MINIMAX{};
		double          UCB_UNEXPANDED_NODE{};
		double          UCB_EXPLORATION_CONSTANT{};
		double          UCB_LOSSES_AVOIDANCE{};
		double          UCB_LOG_TERM_FACTOR{};
		bool            UCB_USE_FATHER_VISITS{};
		int             PRIOR_FAST_EVAL_DEPTH{};
		int             PRIOR_SLOW_EVAL_DEPTH{};

		// Some stacks to do/undo the moves: for compatibility with the alpha-beta search
		// implementation, we want to be able to reference from stack[-4] to stack[MAX_PLY+2].
		mctsNodeInfo* nodesBuffer[MAX_PLY + 10]{}, ** nodes = nodesBuffer + 7;
		Edge* edgesBuffer[MAX_PLY + 10]{}, ** edges = edgesBuffer + 7;
		Search::Stack   stackBuffer[MAX_PLY + 10]{}, * stack = stackBuffer + 7;
		StateInfo       statesBuffer[MAX_PLY + 10]{}, * states = statesBuffer + 7;
	};
}
#endif // #ifndef MONTECARLO_H_INCLUDED
