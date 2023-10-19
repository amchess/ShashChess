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

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring> // For std::memset, std::memcmp
#include <iomanip>
#include <iostream>
#include <sstream>
#include <unordered_map>

#include "../misc.h"
#include "montecarlo.h"

#include <random>

#include "../position.h"
#include "../search.h"
#include "../thread.h"
#include "../uci.h"

namespace Stockfish {

	// MonteCarlo is a class implementing Monte-Carlo Tree Search for Stockfish.
	// We are following the survey http://mcts.ai/pubs/mcts-survey-master.pdf
	// for the notations and the description of the Monte-Carlo algorithm.

	// Bibliography:
	//     http://mcts.ai/pubs/mcts-survey-master.pdf
	//     https://www.ke.tu-darmstadt.de/lehre/arbeiten/bachelor/2012/Arenz_Oleg.pdf
	//     https://dke.maastrichtuniversity.nl/m.winands/publications.html
	//     https://www.ru.is/faculty/yngvi/pdf/WinandsB11a.pdf
	//     http://cassio.free.fr/pdf/alphago-zero-nature.pdf
	//     https://arxiv.org/abs/1712.01815

	using namespace std;
	using std::string;

	inline bool comp_float(const double a, const double b, const double epsilon = 0.005)
	{
		return fabs(a - b) < epsilon;
	}
	// Comparison functions for edges
	struct COMPARE_PRIOR {
		inline bool operator()(const Edge* a, const Edge* b) const
		{
			return a->prior > b->prior;
		}
	}ComparePrior;

	struct COMPARE_VISITS
	{
		inline bool operator()(const Edge* a, const Edge* b) const
		{
			return a->visits > b->visits || (comp_float(a->visits, b->visits, 0.005) && a->prior > b->prior);
		}
	}CompareVisits;

	struct COMPARE_MEAN_ACTION
	{
		inline bool operator()(const Edge* a, const Edge* b) const
		{
			return a->meanActionValue > b->meanActionValue;
		}
	}CompareMeanAction;

	struct COMPARE_ROBUST_CHOICE {
		inline bool operator()(const Edge* a, const Edge* b) const
		{
			return (10 * a->visits + a->prior > 10 * b->visits + b->prior);
		}
	}CompareRobustChoice;

	MCTSHashTable MCTS;
	Edge EDGE_NONE;
	Spinlock createLock;
	int mctsThreads, mctsGoldDigger;

	int intRand(const int& min, const int& max) {
		static std::random_device rd;
		static thread_local std::mt19937 gen(rd());
		uniform_int_distribution distribution(min, max);

		return distribution(gen);
	}

	mctsNodeInfo* create_node(Key key1, Key key2)
	{
		mctsNodeInfo* infos = new mctsNodeInfo();

		infos->key1 = key1;          // Zobrist hash of all pieces, including pawns
		infos->key2 = key2;          // Zobrist hash of pawns
		infos->node_visits = 0;      // number of visits by the Monte-Carlo algorithm
		infos->number_of_sons = 0;   // total number of legal moves
		infos->expandedSons = 0;     // number of sons expanded by the Monte-Carlo algorithm
		infos->lastMove = MOVE_NONE; // the move between the parent and this node
		infos->deep = 1;
		infos->ttValue = VALUE_NONE;
		infos->AB = false;

		const auto it = MCTS.insert(make_pair(key1, infos));
		return *(&it->second);

		return infos;
	}

	/// get_node() probes the Monte-Carlo hash table to find the node with the given
	/// position, creating a new entry if it doesn't exist yet in the table.
	/// The returned node is always valid.
	mctsNodeInfo* get_node(const Position& pos)
	{
		AutoSpinLock asl(createLock);

		Key key1 = pos.key();
		Key key2 = pos.pawn_key();

		// If the node already exists in the hash table, we want to return it.
		// We search in the range of all the hash table entries with key "key1".
		const auto [fst, snd] = MCTS.equal_range(key1);
		auto it1 = fst;
		const auto it2 = snd;
		while (it1 != it2)
		{
			mctsNodeInfo* node = *(&it1->second);

			if (node->key1 == key1 && node->key2 == key2)
				return node;

			++it1;
		}

		// Node was not found, so we have to create a new one
		return create_node(key1, key2);
	}

	// Helpers functions
	inline int number_of_sons(mctsNodeInfo* node) { return node->number_of_sons; }
	inline bool is_interrupted() { return Threads.stop.load(std::memory_order_relaxed); }

	// MonteCarlo::search() is the main function of Monte-Carlo algorithm.
	void MonteCarlo::search() {

		create_root();
		while (computational_budget()) {
			AB_Rollout = false;
			mctsNodeInfo* node = tree_policy();
			Reward reward;

			if (AB_Rollout)
			{
				Value value = evaluate_with_minimax(std::min<int>(node->deep, MAX_PLY - ply - 2), node);
				if (Threads.stop)
					break;

				if (value == VALUE_ZERO)
					value = node->ttValue;

				if (value >= VALUE_KNOWN_WIN)
					value = VALUE_KNOWN_WIN - node->deep - ply;

				if (value <= -VALUE_KNOWN_WIN)
					value = -(VALUE_KNOWN_WIN - node->deep - ply);

				reward = value_to_reward(value);
				node->ttValue = value;

				if ((mctsThreads == 1) && (node->deep + ply > maximumPly))
					maximumPly = node->deep + ply;
			}
			else
				reward = playout_policy(node);

			backup(reward, AB_Rollout);

			if (should_output_result())
				emit_principal_variation();
		}
	}

	/// MonteCarlo::MonteCarlo() is the constructor for the MonteCarlo class
	MonteCarlo::MonteCarlo(Position& p) : pos(p) {
		default_parameters();
		create_root();
	}

	/// MonteCarlo::create_root() initializes the Monte-Carlo tree with the given position
	void MonteCarlo::create_root() {

		// Initialize the global counters
		ply = 1;
		maximumPly = ply;
		doMoveCnt = 0;
		descentCnt = 0;
		playoutCnt = 0;
		priorCnt = 0;
		startTime = now();
		lastOutputTime = startTime;

		// Prepare the stack to go down and up in the game tree
		std::memset(stackBuffer, 0, sizeof stackBuffer);

		for (int i = -7; i <= MAX_PLY + 10; i++)
			stack[i].continuationHistory = &pos.this_thread()->continuationHistory[0][0][NO_PIECE][0]; // Use as a sentinel

		for (int i = 0; i <= MAX_PLY + 2; ++i)
			stack[i].ply = i;

		// TODO : what to do with killers ???

		// Erase the list of nodes, and set the current node to the root node
		std::memset(nodesBuffer, 0, sizeof nodesBuffer);
		root = nodes[ply] = get_node(pos);

		if (current_node()->node_visits == 0)
			generate_moves();

		assert(ply == 1);
		assert(root == nodes[ply]);
		assert(root == current_node());
	}

	/// MonteCarlo::computational_budget() returns true the search is still
	/// in the computational budget (time limit, or number of nodes, etc.)
	bool MonteCarlo::computational_budget()
	{
		assert(is_root(current_node()));

		if (pos.this_thread() == Threads.main())
			dynamic_cast<MainThread*>(pos.this_thread())->check_time();

		return descentCnt < MAX_DESCENTS && !is_interrupted();
	}

	/// MonteCarlo::tree_policy() selects the next node to be expanded
	mctsNodeInfo* MonteCarlo::tree_policy() {
		//   debug << "Entering tree_policy()..." << endl;

		assert(is_root(current_node()));
		descentCnt++;

		if (number_of_sons(root) == 0)
		{
			return root;
		}

		while (current_node()->node_visits > 0)
		{
			if (is_terminal(current_node()))
				return current_node();

			const int e_greedy = intRand(0, RAND_MAX) % 100;

			if (!is_root(current_node())
				&& current_node()->ttValue < VALUE_KNOWN_WIN
				&& current_node()->ttValue > -VALUE_KNOWN_WIN
				&& ((mctsThreads == 1)
					||
					(
						(mctsThreads > 1)
						&& number_of_sons(current_node()) > 5
						&& e_greedy >= Options["MCTS Multi Strategy"]
						))
				)
			{
				if (mctsThreads == 1)
					current_node()->deep++;
				else
					current_node()->deep = std::min(current_node()->deep + 1, MAX_PLY - ply - 2);

				AB_Rollout = true;
				return current_node();
			}

			edges[ply] = best_child(current_node(), STAT_UCB);

			const Move m = edges[ply]->move;

			Edge* edge = edges[ply];

			current_node()->node_visits++;

			// Add a virtual loss to this edge (for load balancing in the parallel MCTS)
			edge->visits = edge->visits + 1.0;
			edge->meanActionValue = edge->actionValue / edge->visits;

			/* debug << "edges[" << ply << "].move = "
				 << UCI::move(edges[ply]->move, pos.is_chess960())
				 << std::endl;
			*/
			assert(is_ok(m));
			assert(pos.legal(m));

			do_move(m);

			/*   debug << "stack[" << ply-1 << "].currentMove = "
				 << UCI::move(stack[ply-1].currentMove, pos.is_chess960())
				 << std::endl;
			*/
			nodes[ply] = get_node(pos);
		}

		assert(current_node()->node_visits == 0);

		//   debug << "... exiting tree_policy()" << endl;

		return current_node();
	}

	/// MonteCarlo::playout_policy() expands the selected node, plays a semi random game starting
	/// from there, and return the reward of this playout from the point of view of the
	/// player to move in the expanded move.
	Reward MonteCarlo::playout_policy(mctsNodeInfo* node) {

		playoutCnt++;
		assert(current_node() == node);

		// Step 0. Check for terminal nodes
		if (is_terminal(node))
			return evaluate_terminal();

		// Step 1. Expand the current node
		// We generate the legal moves and calculate their prior values.

		assert(current_node()->node_visits == 0);

		generate_moves();
		assert(current_node()->node_visits >= 1);

		if (number_of_sons(node) == 0)
			return evaluate_terminal();

		// Step 2. Play-out policy
		// Now implement a play-out policy from the newly expanded node

	   // debug_tree_stats();
		assert(current_node()->number_of_sons > 0);

		// Step 3. Return reward

		// Return the reward of the play-out from the point of view of the side to play.
		// Here we can just return the prior value of the first legal moves, because the
		// legal moves were sorted by prior in the generate_moves() call.

		return current_node()->children_list()[0]->prior;
	}

	/// MonteCarlo::backup() implements the strategy for accumulating rewards up the tree
	/// after a playout.
	void MonteCarlo::backup(Reward r, bool AB_Mode) {

		/* debug << "Entering backup()..." << endl;
		 debug << pos << endl;
		 debug << "reward r = " << r << endl;
		 debug_tree_stats();
		 debug_node();
		 */
		 //assert(node == current_node());
		assert(ply >= 1);
		double weight = 1.0;

		while (!is_root(current_node()))
		{
			undo_move();

			r = 1.0 - r;

			Edge* edge = edges[ply];

			/*  debug << "stack[" << ply << "].currentMove = "
				<< UCI::move(stack[ply].currentMove, pos.is_chess960())
				<< std::endl;
				debug_edge();
			*/

			if (AB_Mode)
			{
				edge->prior = r;
				AB_Mode = false;
			}

			// Compensate the virtual loss we had set in tree_policy()
			edge->visits = edge->visits - 1.0;


			// Update the statistics of the edge
			edge->visits = edge->visits + weight;
			edge->actionValue = edge->actionValue + weight * r;
			edge->meanActionValue = edge->actionValue / edge->visits;

			assert(edge->meanActionValue >= 0.0);
			assert(edge->meanActionValue <= 1.0);

			const double minimax = best_child(current_node(), STAT_MEAN)->meanActionValue;

			// Propagate the minimax value up the tree instead of the playout value ?
			r = r * (1.0 - BACKUP_MINIMAX) + minimax * BACKUP_MINIMAX;

			//debug_edge();

			assert(stack[ply].currentMove == edge->move);
		}

		// debug << "... exiting backup()" << endl;

		assert(is_root(current_node()));
	}


	/// MonteCarlo::best_child() selects the best child of a node according
	/// the given statistic. For instance, the statistic can be the UCB
	/// formula or the number of visits.
	Edge* MonteCarlo::best_child(mctsNodeInfo* node, EdgeStatistic statistic) {

		//debug << "Entering best_child()..." << endl;
		//debug << pos << endl;

		if (number_of_sons(node) <= 0)
		{
			// debug << "... exiting best_child() with EDGE_NONE" << endl;
			return &EDGE_NONE;
		}

		EdgeArray& children = node->children_list();

		//for (int k = 0 ; k < number_of_sons(node) ; k++)
		//{
		//   debug << "move #" << k << ": "
		//          << UCI::move(children[k].move, pos.is_chess960())
		//          << " with " << children[k].visits
		//          << (children[k].visits > 0 ? " visits":" visit")
		//          << " and prior " << children[k].prior
		//          << endl;
		//}

		int best = -1;
		double bestValue = -1000000000000.0;
		for (int k = 0; k < number_of_sons(node); k++)
		{
			const double r = statistic == STAT_VISITS ? children[k]->visits.load(std::memory_order_relaxed)
				: statistic == STAT_MEAN ? children[k]->meanActionValue.load(std::memory_order_relaxed)
				: statistic == STAT_UCB ? ucb(node, children[k], false)
				: statistic == STAT_PRIOR ? ucb(node, children[k], true)
				: 0.0;

			if (r > bestValue)
			{
				bestValue = r;
				best = k;
			}
		}

		/*  debug << "=> Selecting move " << UCI::move(children[best].move, pos.is_chess960())
			<< " with UCB " << bestValue
			<< endl;
			debug << "... exiting best_child()" << endl;
		*/
		return children[best];
	}

	/// MonteCarlo::should_output_result() checks if it should write the pv of the game tree
	bool MonteCarlo::should_output_result() const
	{
		if (pos.this_thread() != Threads.main())
			return false;

		const TimePoint elapsed = now() - startTime + 1;  // in milliseconds
		const TimePoint outputDelay = now() - lastOutputTime;

		if (elapsed < 1100)                                 return outputDelay >= 100;
		if (elapsed < static_cast<int64_t>(11 * 1000))      return outputDelay >= 1000;
		if (elapsed < static_cast<int64_t>(61 * 1000))      return outputDelay >= 10000;
		if (elapsed < static_cast<int64_t>(6 * 60 * 1000))  return outputDelay >= 30000;
		if (elapsed < static_cast<int64_t>(61 * 60 * 1000)) return outputDelay >= 60000;
		return outputDelay >= 60000;
	}


	/// MonteCarlo::emit_principal_variation() emits the pv of the game tree on the
	/// standard output stream, as requested by the UCI protocol.
	void MonteCarlo::emit_principal_variation() {

		//   debug << "Entering emit_principal_variation() ..." << endl;

		assert(is_root(current_node()));

		string pv;
		const EdgeArray& children = root->children_list();
		int n = number_of_sons(root);

		// Make a local copy of the children of the root, and sort by number of visits
		EdgeArray list(children);

		if (mctsThreads > 1)
			std::sort(list.begin(), list.begin() + n, ComparePrior);
		else
			std::sort(list.begin(), list.begin() + n, CompareRobustChoice);

		// Clear the global list of moves for root (Search::RootMoves)
		Search::RootMoves& rootMoves = pos.this_thread()->rootMoves;
		rootMoves.clear();

		if (n > 0)
		{
			// Copy the list of moves given by the Monte-Carlo algorithm to the global list
			for (int k = 0; k < n; k++)
			{
				rootMoves.push_back(Search::RootMove(list[k]->move));
				const size_t index = rootMoves.size() - 1;
				rootMoves[index].previousScore = reward_to_value(list[k]->meanActionValue);
				rootMoves[index].score = rootMoves[index].previousScore;
				rootMoves[index].selDepth = maximumPly;
			}

			// Extract from the tree the principal variation of the best move
			Move move = rootMoves[0].pv[0];
			int cnt = 0;
			while (pos.legal(move))
			{
				cnt++;
				do_move(move);
				nodes[ply] = get_node(pos);

				if ((mctsThreads > 1) && (nodes[ply]->deep + ply > maximumPly))
					maximumPly = nodes[ply]->deep + ply;

				if (is_terminal(current_node())
					|| number_of_sons(current_node()) <= 0
					|| current_node()->node_visits <= 0)
					break;

				move = best_child(current_node(), STAT_VISITS)->move;

				if (pos.legal(move))
					rootMoves[0].pv.push_back(move);
			}

			for (int k = 0; k < cnt; k++)
				undo_move();

			assert(int(rootMoves.size()) == number_of_sons(root));
			assert(is_root(current_node()));
			// debug << "Before calling UCI::pv()" << endl;

			pv = UCI::pv(pos, maximumPly);
		}
		else
		{
			// Mate or stalemate: we put a empty move in the global list of moves at root
			rootMoves.emplace_back(MOVE_NONE);
			pv = "info depth 0 score " + UCI::value(pos.checkers() ? -VALUE_MATE : VALUE_DRAW);
		}

		// Emit the principal variation!
		if (Search::Limits.depth)
			sync_cout << "info descents " << descentCnt << sync_endl;
		sync_cout << pv << sync_endl;

		lastOutputTime = now();

		/*  debug << "pv = " << pv << endl;
			debug << "descentCnt = " << descentCnt << endl;
			debug << "... exiting emit_principal_variation()" << endl;
		*/
	}

	/// MonteCarlo::current_node() is the current node of our tree
	inline mctsNodeInfo* MonteCarlo::current_node()
	{
		return nodes[ply];
	}

	/// MonteCarlo::is_root() returns true when node is both the current node and the root
	inline bool MonteCarlo::is_root(mctsNodeInfo* node)
	{
		return ply == 1
			&& node == current_node()
			&& node == root;
	}

	/// MonteCarlo::is_terminal() checks whether a node is a terminal node for the search tree
	inline bool MonteCarlo::is_terminal(mctsNodeInfo* node)
	{
		assert(node == current_node());

		// Mate or stalemate?
		if (node->node_visits > 0 && number_of_sons(node) == 0)
			return true;

		// Have we have reached the search depth limit?
		if (ply >= MAX_PLY - 2)
			return true;

		// Draw by repetition or draw by 50 moves rule?
		if (pos.is_draw(ply - 1))
			return true;

		return false;
	}

	/// MonteCarlo::do_move() plays a move in the search tree from the current position
	void MonteCarlo::do_move(const Move m) {

		assert(ply < MAX_PLY);

		doMoveCnt++;

		stack[ply].ply = ply;
		stack[ply].currentMove = m;
		stack[ply].inCheck = pos.checkers();
		const bool capture = pos.capture(m);

		stack[ply].continuationHistory = &pos.this_thread()->continuationHistory[stack[ply].inCheck]
		                                                                        [capture]
		                                                                        [pos.moved_piece(m)]
		                                                                        [to_sq(m)];

		pos.do_move(m, states[ply]);

		ply++;
		if (ply > maximumPly)
			maximumPly = ply;
	}

	/// MonteCarlo::undo_move() undo the current move in the search tree
	void MonteCarlo::undo_move() {

		assert(ply > 1);

		ply--;
		pos.undo_move(stack[ply].currentMove);
	}

	/// MonteCarlo::add_prior_to_node() adds the given (move,prior) pair as a new son for a node
	void MonteCarlo::add_prior_to_node(mctsNodeInfo* node, Move m, Reward prior) {

		assert(node->number_of_sons < MAX_CHILDREN);
		assert(prior >= 0 && prior <= 1.0);

		int n = node->number_of_sons;
		if (n < MAX_CHILDREN)
		{
			node->children[n]->visits = 0;
			node->children[n]->move = m;
			node->children[n]->prior = prior;
			node->children[n]->actionValue = 0.0;
			node->children[n]->meanActionValue = 0.0;
			node->number_of_sons++;

			/* debug << "Adding move #" << n << ": "
				 << UCI::move(m, pos.is_chess960())
				 << " with " << 0 << " visit"
				 << " and prior " << prior
				 << endl;
			*/
			//assert(node->number_of_sons == moveCount);
		}
		else
		{
			//  debug << "ERROR : too many sons (" << node->number_of_sons << ") in add_prior_to_node()" << endl;
		}
	}

	/// MonteCarlo::generate_moves() does some Stockfish gimmick to iterate over legal moves
	/// of the current position, in a sensible order.
	/// For historical reasons, it is not so easy to get a MovePicker object to
	/// generate moves if we want to have a decent order (captures first, then
	/// quiet moves, etc.). We have to pass various history tables to the MovePicker
	/// constructor, like in the alpha-beta implementation of move ordering.
	void MonteCarlo::generate_moves() {

		/*debug << "Entering generate_moves()..." << endl;
		debug << pos << endl;

		if (pos.should_debug())
		   hit_any_key();

		debug_node();
		*/

		AutoSpinLock asl(current_node()->lock);

		if (current_node()->node_visits == 0)
		{
			const Thread* thread = pos.this_thread();
			const Square   prevSq = to_sq(stack[ply - 1].currentMove);
			const Move     countermove = thread->counterMoves[pos.piece_on(prevSq)][prevSq];
			constexpr Move     ttMove = MOVE_NONE;  // FIXME
			const Move* killers = stack[ply].killers;
			constexpr Depth    depth = 30;

			const CapturePieceToHistory* cph = &thread->captureHistory;
			const ButterflyHistory* mh = &thread->mainHistory;
			const PieceToHistory* contHist[] = { stack[ply - 1].continuationHistory, stack[ply - 2].continuationHistory,
												nullptr, stack[ply - 4].continuationHistory,
												nullptr, stack[ply - 6].continuationHistory };

			MovePicker mp(pos, ttMove, depth, mh, cph, contHist, countermove, killers);
			Move move;
			int moveCount = 0;

			// Generate the legal moves and calculate their priors
			Reward bestPrior = REWARD_MATED;
			while ((move = mp.next_move()) != MOVE_NONE)
				if (pos.legal(move))
				{
					stack[ply].moveCount = ++moveCount;
					const Reward prior = calculate_prior(move);
					if (Threads.stop)
						break;
					if (prior > bestPrior)
					{
						current_node()->ttValue = reward_to_value(prior);
						bestPrior = prior;
					}
					add_prior_to_node(current_node(), move, prior);
				}

			// Sort the moves according to their prior value
			int n = number_of_sons(current_node());
			if (n > 0)
			{
				EdgeArray& children = current_node()->children_list();
				std::sort(children.begin(), children.begin() + n, ComparePrior);
			}

			// Indicate that we have just expanded the current node
			mctsNodeInfo* s = current_node();
			s->node_visits = 1;
			s->expandedSons = 0;
		}

		// debug << "... exiting generate_moves()" << endl;
	}


	/// MonteCarlo::evaluate_terminal() evaluate a terminal node of the search tree
	Reward MonteCarlo::evaluate_terminal()
	{
		mctsNodeInfo* node = current_node();

		assert(is_terminal(node));

		// Mate or stalemate?
		if (number_of_sons(node) == 0)
			return pos.checkers() ? REWARD_MATED : REWARD_DRAW;

		// Have we reached search depth limit?
		if (ply >= MAX_PLY - 2)
			return REWARD_DRAW;

		// This must be draw by repetition or draw by 50 moves rule (no need to check again!)
		return REWARD_DRAW;
	}

	/// MonteCarlo::evaluate_with_minimax() evaluates the current position in the tree
	/// with a small minimax search of the given depth. Note : you can use
	/// depth==DEPTH_ZERO for a direct quiescence value.
	Value MonteCarlo::evaluate_with_minimax(const Depth d) const
	{
		stack[ply].ply = ply;
		stack[ply].currentMove = MOVE_NONE;
		stack[ply].excludedMove = MOVE_NONE;

		const Value v = minimax_value(pos, &stack[ply], d);

		/*  debug << pos << endl;
			debug << "minimax value = " << v << endl;
		*/
		return v;
	}
	Value MonteCarlo::evaluate_with_minimax(Depth d, mctsNodeInfo* node) const
	{

		stack[ply].ply = ply;
		stack[ply].currentMove = MOVE_NONE;
		stack[ply].excludedMove = MOVE_NONE;

		constexpr auto delta = static_cast<Value>(18);
		const Value alpha = std::max(node->ttValue - delta, -VALUE_INFINITE);
		const Value beta = std::min(node->ttValue + delta, VALUE_INFINITE);

		const Value v = minimax_value(pos, &stack[ply], d, alpha, beta);

		/*  debug << pos << endl;
			debug << "minimax value = " << v << endl;
		*/
		return v;
	}

	/// MonteCarlo::calculate_prior() returns the a-priori reward of the move leading to
	/// the n-th son of the current node. Here we use the evaluation function to
	/// estimate this prior, we could use other strategies too (like the rank n of
	/// the son, or the type of the move (good capture/quiet/bad capture), etc).
	Reward MonteCarlo::calculate_prior(const Move m) {

		//assert(n >= 0);

		priorCnt++;

		const Depth depth = ply <= 2 || pos.capture(m) || pos.gives_check(m) ? PRIOR_SLOW_EVAL_DEPTH : PRIOR_FAST_EVAL_DEPTH;

		do_move(m);
		const Reward prior = value_to_reward(-evaluate_with_minimax(depth));
		undo_move();

		return prior;
	}

	/// MonteCarlo::value_to_reward() transforms a Stockfish value to a reward in [0..1].
	/// We scale the logistic function such that a value of 600 (about three pawns) is
	/// given a probability of win of 0.95, and a value of -600 is given a probability
	/// of win of 0.05
	Reward MonteCarlo::value_to_reward(Value v) const
	{
		constexpr double k = -0.00490739829861;
		const double r = 1.0 / (1 + exp(k * static_cast<int>(v)));

		assert(REWARD_MATED <= r && r <= REWARD_MATE);
		return r;
	}

	/// MonteCarlo::reward_to_value() transforms a reward in [0..1] to a Stockfish value.
	/// The scale is such that a reward of 0.95 corresponds to 600 (about three pawns),
	/// and a reward of 0.05 corresponds to -600 (about minus three pawns).
	Value MonteCarlo::reward_to_value(Reward r) const
	{
		if (r > 0.99) return  VALUE_KNOWN_WIN;
		if (r < 0.01) return -VALUE_KNOWN_WIN;

		constexpr double g = 203.77396313709564;  //  this is 1 / k
		const double v = g * log(r / (1.0 - r));
		return static_cast<Value>(static_cast<int>(v));
	}

	/// MonteCarlo::set_exploration_constant() changes the exploration constant of the UCB formula.
	///
	/// This constant sets the balance between the exploitation of past results and the
	/// exploration of new branches in the Monte-Carlo tree. The higher the constant, the
	/// more likely is the algorithm to explore new parts of the tree, whereas lower values
	/// of the constant makes an algorithm which focuses more on the already explored
	/// parts of the tree. Default value is 10.0
	void MonteCarlo::set_exploration_constant(double C) {
		UCB_EXPLORATION_CONSTANT = C;
	}

	/// MonteCarlo::exploration_constant() returns the exploration constant of the UCB formula
	double MonteCarlo::exploration_constant() const
	{
		return UCB_EXPLORATION_CONSTANT;
	}

	/// MonteCarlo::params() returns a debug string with the current Monte Carlo parameters.
	/// Note: to see it in a terminal, type "./stockfish" then "params".
	std::string MonteCarlo::params() const
	{
		stringstream s;

		s << "\nMAX_DESCENTS = " << MAX_DESCENTS << endl;
		s << "BACKUP_MINIMAX = " << BACKUP_MINIMAX << endl;
		s << "PRIOR_FAST_EVAL_DEPTH = " << PRIOR_FAST_EVAL_DEPTH << endl;
		s << "PRIOR_SLOW_EVAL_DEPTH = " << PRIOR_SLOW_EVAL_DEPTH << endl;
		s << "UCB_UNEXPANDED_NODE = " << UCB_UNEXPANDED_NODE << endl;
		s << "UCB_EXPLORATION_CONSTANT = " << UCB_EXPLORATION_CONSTANT << endl;
		s << "UCB_LOSSES_AVOIDANCE = " << UCB_LOSSES_AVOIDANCE << endl;
		s << "UCB_LOG_TERM_FACTOR = " << UCB_LOG_TERM_FACTOR << endl;
		s << "UCB_USE_FATHER_VISITS = " << UCB_USE_FATHER_VISITS << endl;

		return s.str();
	}

	/// MonteCarlo::debug_tree_stats()
	void MonteCarlo::debug_tree_stats() {
		/* debug << "ply        = " << ply             << endl;
		debug << "maximumPly = " << maximumPly      << endl;
		debug << "descentCnt = " << descentCnt      << endl;
		debug << "playoutCnt = " << playoutCnt      << endl;
		debug << "doMoveCnt  = " << doMoveCnt       << endl;
		debug << "priorCnt   = " << priorCnt        << endl;
		debug << "hash size  = " << MCTS.size()     << endl;
		*/
	}


	/// MonteCarlo::debug_node()
	void MonteCarlo::debug_node() {
		/*  debug << "isCurrent    = " << (node == current_node()) << endl;
		debug << "isRoot       = " << is_root(current_node())  << endl;
		debug << "key1         = " << node->key1               << endl;
		debug << "key2         = " << node->key2               << endl;
		debug << "visits       = " << node->node_visits        << endl;
		debug << "sons         = " << node->number_of_sons     << endl;
		debug << "expandedSons = " << node->expandedSons       << endl;
		*/
	}

	/// MonteCarlo::debug_edge()
	void MonteCarlo::debug_edge() {
		/*debug << "edge = { "
		<< UCI::move(e.move, pos.is_chess960())   << " , "
		<< "N = " << e.visits                     << " , "
		<< "P = " << e.prior                      << " , "
		<< "W = " << e.actionValue                << " , "
		<< "Q = " << e.meanActionValue            << " }"
		<< endl;
		*/
	}

	/// MonteCarlo::test()
	void MonteCarlo::test() {
		/*debug << "---------------------------------------------------------------------------------" << endl;
		debug << "Testing MonteCarlo for position..." << endl;
		debug << pos << endl;
		*/
		search();

		/* debug << "... end of MonteCarlo testing!" << endl;
		debug << "---------------------------------------------------------------------------------" << endl;
		*/
	}

	/// MonteCarlo::ucb() calculates the upper confidence bound formula for the son
	/// which we reach from node "node" by following the edge "edge".
	double MonteCarlo::ucb(mctsNodeInfo* node, const Edge* edge, bool priorMode) {

		if (priorMode)
			return edge->prior;

		const long fatherVisits = node->node_visits;
		assert(fatherVisits > 0);

		double result = 0.0;
		if (((mctsThreads > 1) && (edge->visits > Options["MCTS Multi MinVisits"]))
			||
			((mctsThreads == 1) && edge->visits)
			)
		{
			result += edge->meanActionValue;
		}
		else
		{
			result += UCB_UNEXPANDED_NODE;
		}
		const double C = UCB_USE_FATHER_VISITS ? exploration_constant() * sqrt(fatherVisits) : exploration_constant();
		const double losses = edge->visits - edge->actionValue;
		const double visits = edge->visits;

		const double divisor = losses * UCB_LOSSES_AVOIDANCE + visits * (1.0 - UCB_LOSSES_AVOIDANCE);
		result += C * edge->prior / (1 + divisor);

		result += UCB_LOG_TERM_FACTOR * sqrt(log(fatherVisits) / (1 + visits));

		return result;
	}

	/// MonteCarlo::default_parameters() set the default parameters for the MCTS search
	void MonteCarlo::default_parameters() {

		MAX_DESCENTS = Search::Limits.depth ? Search::Limits.depth : static_cast<long>(100000000000000);
		BACKUP_MINIMAX = 1.0;
		PRIOR_FAST_EVAL_DEPTH = 1;
		PRIOR_SLOW_EVAL_DEPTH = 1;
		UCB_UNEXPANDED_NODE = 1.0;
		UCB_EXPLORATION_CONSTANT = 1.0;
		UCB_LOSSES_AVOIDANCE = 1.0;
		UCB_LOG_TERM_FACTOR = 0.0;
		UCB_USE_FATHER_VISITS = true;
	}

	/// MonteCarlo::print_children() emits the sorted move list and scores of the game tree to the
	/// standard output stream, as requested by the UCI protocol.
	void MonteCarlo::print_children() {

		//debug << "Entering print_children() ..." << endl;

		//assert(is_root(current_node()));

		EdgeArray& children = root->children_list();

		// Sort the moves according to their prior value
		if (const int n = number_of_sons(root); n > 0)
			std::sort(children.begin(), children.begin() + n, CompareRobustChoice);

		for (int k = number_of_sons(root) - 1; k >= 0; k--)
		{
			std::cout << "info string move "
				<< k + 1
				<< " "
				<< UCI::move(children[k]->move, pos.is_chess960())

				<< std::setprecision(2)
				<< " win% "
				<< children[k]->prior * 100

				<< std::fixed
				<< std::setprecision(0)
				<< " visits "
				<< children[k]->visits
				<< std::endl;
		}

		lastOutputTime = now();

		/*
		debug << "pv = " << pv << endl;
		debug << "descentCnt = " << descentCnt << endl;
		debug << "... exiting print_children()" << endl;
		*/
	}

	// List of FIXME/TODO for the monte-carlo branch
	//
	// 1. ttMove = MOVE_NONE in generate_moves() ?
	// 2. what to do with killers in create_root() ?
	// 3. why do we get losses on time with small prior depths ?
	// 4. should we set rm.score to -VALUE_INFINITE for moves >= 2 in emit_principal_variation() ?
	// 5. r2qk2r/1p1b1pb1/4p2p/1p1p4/1n1P4/NQ3PP1/PP2N2P/R1B2RK1 b kq - 23 12
}





