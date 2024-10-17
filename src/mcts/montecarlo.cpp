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

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>  // For std::memset, std::memcmp
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
#include "../syzygy/tbprobe.h"

namespace ShashChess {

// MonteCarlo is a class implementing Monte-Carlo Tree Search for ShashChess.
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

inline bool comp_float(const double a, const double b, const double epsilon = 0.005) {
    return fabs(a - b) < epsilon;
}

///////////////////////////////////////////////////////////////////////////////////////
/// Comparison functions for edges
///////////////////////////////////////////////////////////////////////////////////////
struct COMPARE_PRIOR {
    inline bool operator()(const Edge* a, const Edge* b) const { return a->prior > b->prior; }
} ComparePrior;

struct COMPARE_VISITS {
    inline bool operator()(const Edge* a, const Edge* b) const {
        return a->visits > b->visits
            || (comp_float(a->visits, b->visits, 0.005) && a->prior > b->prior);
    }
} CompareVisits;

struct COMPARE_MEAN_ACTION {
    inline bool operator()(const Edge* a, const Edge* b) const {
        return a->meanActionValue > b->meanActionValue;
    }
} CompareMeanAction;

struct COMPARE_ROBUST_CHOICE {
    inline bool operator()(const Edge* a, const Edge* b) const {
        return (10 * a->visits + a->prior > 10 * b->visits + b->prior);
    }
} CompareRobustChoice;

///////////////////////////////////////////////////////////////////////////////////////
/// AutoSpinlock class: Spin-lock resource in a scope
///////////////////////////////////////////////////////////////////////////////////////
class AutoSpinLock {
   private:
    const MonteCarlo* _mcts;
    Spinlock&         _sl;

   public:
    AutoSpinLock(const MonteCarlo* mcts, mctsNodeInfo* node) :
        AutoSpinLock(mcts, node->lock) {}

    AutoSpinLock(const MonteCarlo* mcts, Spinlock& sl) :
        _mcts(mcts),
        _sl(sl) {
        _sl.acquire(_mcts->thisThread->threadIdx);
    }

    ~AutoSpinLock() { _sl.release(_mcts->thisThread->threadIdx); }
};

#define LOCK__(m, n, l) AutoSpinLock asl##l(m, n)
#define LOCK_(m, n, l) LOCK__(m, n, l)
#define LOCK(m, n) LOCK_(m, n, __LINE__)

MCTSHashTable MCTS;
Edge          EDGE_NONE;
Spinlock      createLock;
size_t        mctsThreads;
size_t        mctsMultiStrategy;
double        mctsMultiMinVisits;

template<typename T>
T TRand(const T min, const T max) {
    static std::random_device        rd;
    static thread_local std::mt19937 gen(rd());
    uniform_int_distribution<T>      distribution(min, max);

    return distribution(gen);
}

/// get_node() probes the Monte-Carlo hash table to find the node with the given
/// position, creating a new entry if it doesn't exist yet in the table.
/// The returned node is always valid.
mctsNodeInfo* get_node(const MonteCarlo* mcts, const Position& p) {

    Key           key1 = p.key();
    Key           key2 = p.pawn_key();
    mctsNodeInfo* node = nullptr;

    //Lock
    LOCK(mcts, createLock);

    // If the node already exists in the hash table, we want to return it.
    // We search in the range of all the hash table entries with key "key1".
    const auto [fst, snd] = MCTS.equal_range(key1);
    auto       it1        = fst;
    const auto it2        = snd;
    while (it1 != it2)
    {
        node = *(&it1->second);

        if (node->key1 == key1 && node->key2 == key2)
            return node;

        ++it1;
    }

    // Node was not found, so we have to create a new one
    node                 = new mctsNodeInfo();
    node->key1           = key1;          // Zobrist hash of all pieces, including pawns
    node->key2           = key2;          // Zobrist hash of pawns
    node->node_visits    = 0;             // number of visits by the Monte-Carlo algorithm
    node->number_of_sons = 0;             // total number of legal moves
    node->lastMove       = Move::none();  // the move between the parent and this node
    node->ttValue        = VALUE_NONE;
    node->AB             = false;

    //Insert into MCTS hash table
    MCTS.insert(make_pair(key1, node));

    return node;
}

/// MonteCarlo::add_prior_to_node() adds the given (move,prior) pair as a new son for a node
void MonteCarlo::add_prior_to_node(mctsNodeInfo* node, Move m, Reward prior) const {

    LOCK(this, node);

    assert(node->number_of_sons < MAX_CHILDREN);
    assert(prior >= 0 && prior <= 1.0);

    int n = node->number_of_sons;
    if (n < MAX_CHILDREN)
    {
        node->children[n]->visits          = 0;
        node->children[n]->move            = m;
        node->children[n]->prior           = prior;
        node->children[n]->actionValue     = 0.0;
        node->children[n]->meanActionValue = 0.0;
        node->number_of_sons++;
    }
    else
    {
        //Too many sons (should never come here)
        assert(false);
    }
}

// MonteCarlo::search() is the main function of Monte-Carlo algorithm.
void MonteCarlo::search(ShashChess::ThreadPool&        threads,
                        ShashChess::Search::LimitsType limits,
                        bool                           isMainThread,
                        Search::Worker*                worker) {

    mctsNodeInfo* node = nullptr;
    AB_Rollout         = false;
    Reward reward      = value_to_reward(
      VALUE_DRAW);  //TODO: Perhaps we should use static_value() here instead of 'VALUE_DRAW'

    while (computational_budget(threads, limits) && (node = tree_policy(threads, limits)))
    {
        LOCK(this, node);

        if (AB_Rollout)
        {
            Value value = evaluate_with_minimax(node, std::min(ply, MAX_PLY - ply - 2));
            if (threads.stop)
                break;

            if (value == VALUE_ZERO)
                value = node->ttValue;

            if (value >= VALUE_KNOWN_WIN)
                value = VALUE_KNOWN_WIN - ply;

            if (value <= -VALUE_KNOWN_WIN)
                value = -(VALUE_KNOWN_WIN - ply);

            reward        = value_to_reward(value);
            node->ttValue = value;

            if (ply > maximumPly)
                maximumPly = ply;
        }
        else
        {
            reward = playout_policy(node);
        }

        if (ply >= 1)
            node->ttValue = backup(reward, AB_Rollout);

        if (should_emit_pv(isMainThread))
            emit_pv(worker, threads);
    }

    if (ply >= 1)
        backup(reward, AB_Rollout);

    if (should_emit_pv(isMainThread))
        emit_pv(worker, threads);
}

/// MonteCarlo::MonteCarlo() is the constructor for the MonteCarlo class
MonteCarlo::MonteCarlo(Position&           p,
                       Search::Worker*     worker,
                       TranspositionTable& transpositionTable) :
    pos(p),
    thisThread(worker),
    tt(transpositionTable) {
    default_parameters();
    create_root(worker);
}

/// MonteCarlo::create_root(Search::Worker* worker) initializes the Monte-Carlo tree with the given position
void MonteCarlo::create_root(Search::Worker* worker) {

    assert(ply == 0);
    assert(nodes[1] == nullptr);
    assert(root == nullptr);

    // Initialize variables
    ply            = 1;
    maximumPly     = 1;
    startTime      = now();
    lastOutputTime = startTime;

    // Prepare the stack to go down and up in the game tree
    std::memset(stackBuffer, 0, sizeof stackBuffer);

    for (int i = -7; i <= MAX_PLY + 10; i++)
        stack[i].continuationHistory =
          &worker->continuationHistory[0][0][NO_PIECE][0];  // Use as a sentinel

    for (int i = 0; i <= MAX_PLY + 2; ++i)
        stack[i].ply = i;

    // TODO : what to do with killers ???

    // Erase the list of nodes, and set the current node to the root node
    std::memset(nodesBuffer, 0, sizeof(nodesBuffer));

    //Create or get the root node
    root = nodes[ply] = get_node(this, pos);

    LOCK(this, root);

    if (root->node_visits == 0)
        generate_moves(root);
}

/// MonteCarlo::computational_budget() returns true the search is still
/// in the computational budget (time limit, or number of nodes, etc.)
bool MonteCarlo::computational_budget(ShashChess::ThreadPool&        threads,
                                      ShashChess::Search::LimitsType limits) {

    if (limits.depth && maximumPly > limits.depth * 2)
        return false;

    return !threads.stop.load(std::memory_order_relaxed);
}

/// MonteCarlo::tree_policy() selects the next node to be expanded
mctsNodeInfo* MonteCarlo::tree_policy(ShashChess::ThreadPool&        threads,
                                      ShashChess::Search::LimitsType limits) {

    assert(ply == 1);

    if (root->number_of_sons == 0)
    {
        return root;
    }

    mctsNodeInfo* node = nullptr;
    while ((node = nodes[ply]))
    {
        LOCK(this, node);

        if (node->node_visits == 0)
            break;

        if (!computational_budget(threads, limits) || is_terminal(node))
            return nullptr;

        edges[ply] = best_child(node, STAT_UCB);

        const Move m = edges[ply]->move;

        Edge* edge = edges[ply];

        node->node_visits++;

        // Add a virtual loss to this edge (for load balancing in the parallel MCTS)
        edge->visits          = edge->visits + 1.0;
        edge->meanActionValue = edge->actionValue / edge->visits;

        assert(m.is_ok());
        assert(pos.legal(m));

        do_move(m);

        nodes[ply] = get_node(this, pos);
    }

    if (node)
    {
        LOCK(this, node);

        const size_t greedy = TRand<size_t>(0, 100);
        if (!is_root(node) && node->ttValue < VALUE_KNOWN_WIN && node->ttValue > -VALUE_KNOWN_WIN
            && (node->number_of_sons > 5 && greedy >= mctsMultiStrategy))
        {
            AB_Rollout = true;
        }
    }

    return node;
}

/// MonteCarlo::playout_policy() expands the selected node, plays a semi random game starting
/// from there, and return the reward of this playout from the point of view of the
/// player to move in the expanded move.
Reward MonteCarlo::playout_policy(mctsNodeInfo* node) {

    LOCK(this, node);

    // Step 0. Check for terminal nodes
    if (is_terminal(node))
        return evaluate_terminal(node);

    // Step 1. Expand the current node
    // We generate the legal moves and calculate their prior values.

    if (node->node_visits == 0)
    {
        generate_moves(node);
        assert(node->node_visits == 1);
    }

    if (node->number_of_sons == 0)
        return evaluate_terminal(node);

    // Step 2. Return reward
    // Return the reward of the play-out from the point of view of the side to play.
    // Here we can just return the prior value of the first legal moves, because the
    // legal moves were sorted by prior in the generate_moves() call.

    return node->children[0]->prior;
}

/// MonteCarlo::backup() implements the strategy for accumulating rewards up the tree
/// after a playout.
Value MonteCarlo::backup(Reward r, bool AB_Mode) {

    assert(ply >= 1);
    double weight = 1.0;

    while (ply != 1)
    {
        undo_move();

        r = 1.0 - r;

        Edge* edge = edges[ply];

        if (AB_Mode)
        {
            edge->prior = r;
            AB_Mode     = false;
        }

        // Compensate the virtual loss we had set in tree_policy()
        edge->visits = edge->visits - 1.0;

        // Update the statistics of the edge
        edge->visits          = edge->visits + weight;
        edge->actionValue     = edge->actionValue + weight * r;
        edge->meanActionValue = edge->actionValue / edge->visits;

        assert(edge->meanActionValue >= 0.0);
        assert(edge->meanActionValue <= 1.0);

        const double minimax = best_child(nodes[ply], STAT_MEAN)->meanActionValue;

        // Propagate the minimax value up the tree instead of the playout value ?
        r = r * (1.0 - BACKUP_MINIMAX) + minimax * BACKUP_MINIMAX;

        assert(stack[ply].currentMove == edge->move);
    }

    assert(ply == 1);

    return reward_to_value(r);
}


/// MonteCarlo::best_child() selects the best child of a node according
/// the given statistic. For instance, the statistic can be the UCB
/// formula or the number of visits.
Edge* MonteCarlo::best_child(mctsNodeInfo* node, EdgeStatistic statistic) const {

    LOCK(this, node);

    if (node->number_of_sons <= 0)
        return &EDGE_NONE;

    int    best      = -1;
    double bestValue = -1000000000000.0;
    for (int k = 0; k < node->number_of_sons; k++)
    {
        const double r =
          statistic == STAT_VISITS ? node->children[k]->visits.load(std::memory_order_relaxed)
          : statistic == STAT_MEAN
            ? node->children[k]->meanActionValue.load(std::memory_order_relaxed)
          : statistic == STAT_UCB
            ? ucb(node->children[k], node->node_visits.load(std::memory_order_relaxed), false)
          : statistic == STAT_PRIOR
            ? ucb(node->children[k], node->node_visits.load(std::memory_order_relaxed), true)
            : 0.0;

        if (r > bestValue)
        {
            bestValue = r;
            best      = k;
        }
    }

    return node->children[best];
}

/// MonteCarlo::should_emit_pv() checks if it should write the pv of the game tree.
/// This function checks if the current thread is the 'main' thread. It also checks how
/// much time has elapsed since the last time the principal variation has been printed
/// to avoid cluttering the GUI
bool MonteCarlo::should_emit_pv(bool isMainThread) const {

    if (!isMainThread)
        return false;

    if (ply != 1)
        return false;

    const TimePoint elapsed     = now() - startTime + 1;  // in milliseconds
    const TimePoint outputDelay = now() - lastOutputTime;

    if (elapsed < 1100)
        return outputDelay >= 100;
    else if (elapsed < static_cast<int64_t>(11 * 1000))
        return outputDelay >= 1000;
    else if (elapsed < static_cast<int64_t>(61 * 1000))
        return outputDelay >= 10000;
    else if (elapsed < static_cast<int64_t>(6 * 60 * 1000))
        return outputDelay >= 30000;
    else if (elapsed < static_cast<int64_t>(61 * 60 * 1000))
        return outputDelay >= 60000;

    return outputDelay >= 60000;
}


/// MonteCarlo::emit_pv() emits the principal variation (PV) of the game tree on the
/// standard output stream, as requested by the UCI protocol.
void MonteCarlo::emit_pv(Search::Worker* worker, ShashChess::ThreadPool& threads) {

    assert(ply == 1);

    LOCK(this, root);

    int n = root->number_of_sons;

    // Make a local copy of the children of the root, and sort
    EdgeArray list(root->children);

    if (mctsThreads > 1)
        std::sort(list.begin(), list.begin() + n, ComparePrior);
    else
        std::sort(list.begin(), list.begin() + n, CompareRobustChoice);

    // Clear the global list of moves for root (Search::RootMoves)
    Search::RootMoves& rootMoves = thisThread->rootMoves;
    rootMoves.clear();

    if (n > 0)
    {
        // Copy the list of moves given by the Monte-Carlo algorithm to the global list
        for (int k = 0; k < n; k++)
        {
            rootMoves.push_back(Search::RootMove(list[k]->move));
            const size_t index             = rootMoves.size() - 1;
            rootMoves[index].previousScore = reward_to_value(list[k]->meanActionValue);
            rootMoves[index].score         = rootMoves[index].previousScore;
            rootMoves[index].selDepth      = maximumPly;
        }

        // Extract from the tree the principal variation of the best move
        Move move = rootMoves[0].pv[0];
        int  cnt  = 0;
        while (pos.legal(move))
        {
            cnt++;
            do_move(move);
            mctsNodeInfo* node = nodes[ply] = get_node(this, pos);

            LOCK(this, node);

            if (ply > maximumPly)
                maximumPly = ply;

            if (is_terminal(node) || node->number_of_sons <= 0 || node->node_visits <= 0)
                break;

            move = best_child(node, STAT_VISITS)->move;

            if (pos.legal(move))
                rootMoves[0].pv.push_back(move);
        }

        for (int k = 0; k < cnt; k++)
            undo_move();

        assert(int(rootMoves.size()) == root->number_of_sons);
        assert(ply == 1);

        threads.main_manager()->pv(*worker, threads, tt, worker->completedDepth);
    }
    else
    {
        // Mate or stalemate: we put a empty move in the global list of moves at root
        rootMoves.emplace_back(Move::none());
        threads.main_manager()->updates.onUpdateNoMoves(
          {0, {pos.checkers() ? -VALUE_MATE : VALUE_DRAW, pos}});
    }

    lastOutputTime = now();
}

/// MonteCarlo::is_root() returns true when node is both the current node and the root
inline bool MonteCarlo::is_root(const mctsNodeInfo* node) const {
    if (node != root)
    {
        assert(ply != 1);
        assert(nodes[ply] != root);

        return false;
    }
    else
    {
        assert(ply == 1);
        assert(nodes[ply] == root);

        return true;
    }
}

/// MonteCarlo::is_terminal() checks whether a node is a terminal node for the search tree
inline bool MonteCarlo::is_terminal(mctsNodeInfo* node) const {

    // Mate or stalemate?
    {
        LOCK(this, node);

        if (node->node_visits > 0 && node->number_of_sons == 0)
            return true;
    }

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

    stack[ply].ply         = ply;
    stack[ply].currentMove = m;
    stack[ply].inCheck     = pos.checkers();
    const bool capture     = pos.capture(m);

    stack[ply].continuationHistory =
      &thisThread->continuationHistory[stack[ply].inCheck][capture][pos.moved_piece(m)][m.to_sq()];

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

/// MonteCarlo::generate_moves() does some ShashChess gimmick to iterate over legal moves
/// of the current position, in a sensible order.
/// For historical reasons, it is not so easy to get a MovePicker object to
/// generate moves if we want to have a decent order (captures first, then
/// quiet moves, etc.). We have to pass various history tables to the MovePicker
/// constructor, like in the alpha-beta implementation of move ordering.
void MonteCarlo::generate_moves(mctsNodeInfo* node) {

    LOCK(this, node);

    //Early exit if this node moves have already been generated
    if (node->node_visits != 0)
        return;

    auto [ttHit, ttData, ttWriter] = tt.probe(pos.key());
    Depth depth                    = 30;

    const PieceToHistory* contHist[] = {stack[ply - 1].continuationHistory,
                                        stack[ply - 2].continuationHistory,
                                        stack[ply - 3].continuationHistory,
                                        stack[ply - 4].continuationHistory,
                                        nullptr,
                                        stack[ply - 6].continuationHistory};
    MovePicker mp(pos, ttData.move, depth, &thisThread->mainHistory, &thisThread->lowPlyHistory,
                  &thisThread->captureHistory, contHist, &thisThread->pawnHistory, stack->ply);
    Move       move;
    int        moveCount = 0;

    // Generate the legal moves and calculate their priors
    Reward bestPrior = REWARD_MATED;
    while (((move = mp.next_move()) != Move::none()))
        if (pos.legal(move))
        {
            stack[ply].moveCount = ++moveCount;
            const Reward prior   = calculate_prior(move);
            if (prior > bestPrior)
            {
                node->ttValue = reward_to_value(prior);
                bestPrior     = prior;
            }

            add_prior_to_node(node, move, prior);
        }

    // Sort the moves according to their prior value
    int n = node->number_of_sons;
    if (n > 0)
    {
        EdgeArray& children = node->children;
        std::stable_sort(children.begin(), children.begin() + n, ComparePrior);
    }

    // Indicate that we have just expanded the current node
    node->node_visits++;
}


/// MonteCarlo::evaluate_terminal() evaluate a terminal node of the search tree
Reward MonteCarlo::evaluate_terminal(mctsNodeInfo* node) const {

    assert(is_terminal(node));

    LOCK(this, node);

    // Mate or stalemate?
    if (node->number_of_sons == 0)
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
Value MonteCarlo::evaluate_with_minimax(const Depth d) const {
    stack[ply].ply          = ply;
    stack[ply].currentMove  = Move::none();
    stack[ply].excludedMove = Move::none();

    return thisThread->minimax_value(pos, &stack[ply], d);
}
Value MonteCarlo::evaluate_with_minimax(mctsNodeInfo* node, Depth d) const {

    stack[ply].ply          = ply;
    stack[ply].currentMove  = Move::none();
    stack[ply].excludedMove = Move::none();

    constexpr auto delta = static_cast<Value>(18);

    Value alpha;
    Value beta;

    {
        LOCK(this, node);

        alpha = std::max(node->ttValue - delta, -VALUE_INFINITE);
        beta  = std::min(node->ttValue + delta, VALUE_INFINITE);
    }

    return thisThread->minimax_value(pos, &stack[ply], d, alpha, beta);
}

/// MonteCarlo::calculate_prior() returns the a-priori reward of the move leading to
/// the n-th son of the current node. Here we use the evaluation function to
/// estimate this prior, we could use other strategies too (like the rank n of
/// the son, or the type of the move (good capture/quiet/bad capture), etc).
Reward MonteCarlo::calculate_prior(const Move m) {

    const Depth depth = ply <= 2 || pos.capture(m) || pos.gives_check(m) ? PRIOR_SLOW_EVAL_DEPTH
                                                                         : PRIOR_FAST_EVAL_DEPTH;

    do_move(m);
    const Reward prior = value_to_reward(-evaluate_with_minimax(depth));
    undo_move();

    return prior;
}

/// MonteCarlo::value_to_reward() transforms a ShashChess value to a reward in [0..1].
/// We scale the logistic function such that a value of 600 (about three pawns) is
/// given a probability of win of 0.95, and a value of -600 is given a probability
/// of win of 0.05
Reward MonteCarlo::value_to_reward(Value v) const {
    constexpr double k = -0.00490739829861;
    const double     r = 1.0 / (1 + exp(k * static_cast<int>(v)));

    assert(REWARD_MATED <= r && r <= REWARD_MATE);
    return r;
}

/// MonteCarlo::reward_to_value() transforms a reward in [0..1] to a ShashChess value.
/// The scale is such that a reward of 0.95 corresponds to 600 (about three pawns),
/// and a reward of 0.05 corresponds to -600 (about minus three pawns).
Value MonteCarlo::reward_to_value(Reward r) const {
    if (r > 0.99)
        return VALUE_KNOWN_WIN;
    if (r < 0.01)
        return -VALUE_KNOWN_WIN;

    constexpr double g = 203.77396313709564;  //  this is 1 / k
    const double     v = g * log(r / (1.0 - r));
    return static_cast<Value>(static_cast<int>(v));
}

/// MonteCarlo::set_exploration_constant() changes the exploration constant of the UCB formula.
///
/// This constant sets the balance between the exploitation of past results and the
/// exploration of new branches in the Monte-Carlo tree. The higher the constant, the
/// more likely is the algorithm to explore new parts of the tree, whereas lower values
/// of the constant makes an algorithm which focuses more on the already explored
/// parts of the tree. Default value is 10.0
void MonteCarlo::set_exploration_constant(double c) { UCB_EXPLORATION_CONSTANT = c; }

/// MonteCarlo::exploration_constant() returns the exploration constant of the UCB formula
double MonteCarlo::exploration_constant() const { return UCB_EXPLORATION_CONSTANT; }

/// MonteCarlo::ucb() calculates the upper confidence bound formula for the son
/// which we reach from node "node" by following the edge "edge".
///
/// WARNING: The node for which the 'edge' belongs should be locked!
double MonteCarlo::ucb(const Edge* edge, long fatherVisits, bool priorMode) const {

    if (priorMode)
        return edge->prior;

    assert(fatherVisits > 0);

    double result = 0.0;
    if (((mctsThreads > 1) && (edge->visits > mctsMultiMinVisits))
        || ((mctsThreads == 1) && edge->visits))
    {
        result += edge->meanActionValue;
    }
    else
    {
        result += UCB_UNEXPANDED_NODE;
    }

    const double C =
      UCB_USE_FATHER_VISITS ? exploration_constant() * sqrt(fatherVisits) : exploration_constant();
    const double losses = edge->visits - edge->actionValue;
    const double visits = edge->visits;

    const double divisor = losses * UCB_LOSSES_AVOIDANCE + visits * (1.0 - UCB_LOSSES_AVOIDANCE);
    result += C * edge->prior / (1 + divisor);

    result += UCB_LOG_TERM_FACTOR * sqrt(log(fatherVisits) / (1 + visits));

    return result;
}

/// MonteCarlo::default_parameters() set the default parameters for the MCTS search
void MonteCarlo::default_parameters() {

    BACKUP_MINIMAX           = 1.0;
    PRIOR_FAST_EVAL_DEPTH    = 1;
    PRIOR_SLOW_EVAL_DEPTH    = 1;
    UCB_UNEXPANDED_NODE      = 1.0;
    UCB_EXPLORATION_CONSTANT = 1.0;
    UCB_LOSSES_AVOIDANCE     = 1.0;
    UCB_LOG_TERM_FACTOR      = 0.0;
    UCB_USE_FATHER_VISITS    = true;
}

/// MonteCarlo::print_children() emits the sorted move list and scores of the game tree to the
/// standard output stream, as requested by the UCI protocol.
void MonteCarlo::print_children() {

    LOCK(this, root);
    EdgeArray& children = root->children;

    // Sort the moves according to their prior value
    if (const int n = root->number_of_sons; n > 0)
        std::sort(children.begin(), children.begin() + n, CompareRobustChoice);

    for (int k = root->number_of_sons - 1; k >= 0; k--)
    {
        std::cout << "info string move " << k + 1 << " "
                  << UCIEngine::move(children[k]->move, pos.is_chess960())

                  << std::setprecision(2) << " win% " << children[k]->prior * 100

                  << std::fixed << std::setprecision(0) << " visits " << children[k]->visits
                  << std::endl;
    }

    lastOutputTime = now();
}

// List of FIXME/TODO for the monte-carlo branch
//
// 1. ttMove = Move::none() in generate_moves() ?
// 2. what to do with killers in create_root(Search::Worker* worker) ?
// 3. why do we get losses on time with small prior depths ?
// 4. should we set rm.score to -VALUE_INFINITE for moves >= 2 in emit_principal_variation() ?
// 5. r2qk2r/1p1b1pb1/4p2p/1p1p4/1n1P4/NQ3PP1/PP2N2P/R1B2RK1 b kq - 23 12
}
