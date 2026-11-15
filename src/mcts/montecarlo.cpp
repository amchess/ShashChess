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

#include "montecarlo.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <new>

#include "../movepick.h"
#include "../shashin/shashin_position.h"
#include "../thread.h"
#include "../tt.h"
#include "../uci.h"

namespace ShashChess {

// MonteCarlo implements a Monte-Carlo Tree Search whose leaves are valued by the
// alpha-beta search of ShashChess, so the tree and the alpha-beta threads share
// transposition table, histories and network.
//
// - A node is an expanded position; its moves (edges) start as leaves valued by a
//   shallow search. The values of the sons also give the policy of the node.
// - A selected leaf is first searched deeper, one ply at a time, then expanded
//   ("MCTS Multi Strategy" is the percentage of expansions).
// - The moves are selected with PUCT; the value of a move is a mix of the minimax
//   value of its subtree and of the mean of the values backed up through it.
// - Mates, tablebase results and stalemates are proven and propagated (MCTS-Solver).
//
// Bibliography:
//     http://mcts.ai/pubs/mcts-survey-master.pdf
//     https://dke.maastrichtuniversity.nl/m.winands/publications.html
//     https://www.ru.is/faculty/yngvi/pdf/WinandsB11a.pdf
//     https://arxiv.org/abs/1712.01815

MctsTree MCTS;
size_t   mctsThreads;
size_t   mctsMultiStrategy;
double   mctsMultiMinVisits;

namespace {

constexpr double Cpuct             = 0.5;   // exploration
constexpr double PolicyTemperature = 0.08;  // of the softmax over the rewards of the sons
constexpr double MinimaxWeight     = 0.5;   // minimax value against mean of the backups
constexpr Value  PolicyMargin      = 200;   // sons worse than the best by more get a bound
constexpr Value  DeepenDelta       = 18;    // aspiration window of the deepening searches
constexpr Depth  PriorFastDepth    = 2;
constexpr Depth  PriorSlowDepth    = 3;
constexpr int    MaxTreePly        = MAX_PLY - 16;

// Serializes the creation of the root among the threads of a search
std::mutex rootMutex;

// Mate and tablebase scores count the plies from the root in the search and from the
// node in the tree, as in the transposition table: the tree outlives the root.
Value to_node(Value v, int ply) { return is_win(v) ? v + ply : is_loss(v) ? v - ply : v; }
Value from_node(Value v, int ply) { return is_win(v) ? v - ply : is_loss(v) ? v + ply : v; }

// A proven score seen from the father of the node, one ply farther
Value proven_for_father(Value v) {
    v = -v;
    return is_win(v) ? v - 1 : is_loss(v) ? v + 1 : v;
}

}  // namespace

///////////////////////////////////////////////////////////////////////////////////////
/// Nodes and tree
///////////////////////////////////////////////////////////////////////////////////////

MctsNode* MctsNode::create(Key k, Key pk, int n) {
    void*     mem  = ::operator new(bytes(n));
    MctsNode* node = new (mem) MctsNode();

    node->key     = k;
    node->pawnKey = pk;
    node->sons    = uint16_t(n);

    for (int i = 0; i < n; ++i)
        new (node->edges() + i) MctsEdge();

    return node;
}

void MctsNode::destroy(MctsNode* node) {
    node->~MctsNode();
    ::operator delete(node);
}

void MctsTree::clear() {
    for (Shard& s : shards)
    {
        for (auto& entry : s.map)
            MctsNode::destroy(entry.second);

        s.map.clear();
    }

    bytes = 0;
    count = 0;
}

MctsNode* MctsTree::find(Key key, Key pawnKey) {
    Shard&                        s = shard_of(key);
    std::lock_guard<MctsSpinLock> guard(s.lock);

    auto it = s.map.find(key);
    return it != s.map.end() && it->second->pawnKey == pawnKey ? it->second : nullptr;
}

MctsNode* MctsTree::insert(MctsNode* node) {
    Shard& s = shard_of(node->key);

    {
        std::lock_guard<MctsSpinLock> guard(s.lock);

        auto [it, inserted] = s.map.emplace(node->key, node);
        if (inserted)
        {
            bytes.fetch_add(MctsNode::bytes(node->sons), std::memory_order_relaxed);
            count.fetch_add(1, std::memory_order_relaxed);
            return node;
        }

        // Another thread expanded the same position first (or the keys clash)
        MctsNode*  other = it->second;
        const bool same  = other->pawnKey == node->pawnKey;
        MctsNode::destroy(node);
        return same ? other : nullptr;
    }
}

void MctsTree::new_search(const Position& rootPos, size_t megabytes) {

    budget = std::max<size_t>(megabytes, 64) << 20;

    if (bytes.load(std::memory_order_relaxed) < budget / 2)
        return;

    // Mark what the new root reaches (the tree is a graph: positions can repeat)...
    ++generation;

    std::vector<MctsNode*> todo;
    if (MctsNode* root = find(rootPos.key(), rootPos.pawn_key()))
    {
        root->mark = generation;
        todo.push_back(root);
    }

    while (!todo.empty())
    {
        MctsNode* node = todo.back();
        todo.pop_back();

        for (int i = 0; i < node->sons; ++i)
        {
            MctsNode* child = node->edges()[i].child;
            if (child && child->mark != generation)
            {
                child->mark = generation;
                todo.push_back(child);
            }
        }
    }

    // ... and free the rest: no marked node points to an unmarked one
    for (Shard& s : shards)
        for (auto it = s.map.begin(); it != s.map.end();)
        {
            if (it->second->mark != generation)
            {
                bytes.fetch_sub(MctsNode::bytes(it->second->sons), std::memory_order_relaxed);
                count.fetch_sub(1, std::memory_order_relaxed);
                MctsNode::destroy(it->second);
                it = s.map.erase(it);
            }
            else
                ++it;
        }
}

///////////////////////////////////////////////////////////////////////////////////////
/// Values and rewards
///////////////////////////////////////////////////////////////////////////////////////

/// MonteCarlo::value_to_reward() transforms a ShashChess value to a reward in [0..1].
/// We scale the logistic function such that a value of 600 is given a probability of
/// win of 0.95. Proven results stay above (below) any evaluation, shorter mates first.
Reward MonteCarlo::value_to_reward(Value v) {
    if (is_win(v))
        return std::min(1.0, 1.0 - 1e-6 * (VALUE_MATE - v));
    if (is_loss(v))
        return std::max(0.0, 1e-6 * (VALUE_MATE + v));

    constexpr double k = -0.00490739829861;
    const double     r = 1.0 / (1.0 + std::exp(k * double(v)));

    // The small linear term keeps the reward increasing where the logistic is flat
    return std::clamp(r, 5e-4, 1.0 - 5e-4) + 1e-9 * double(v);
}

/// MonteCarlo::reward_to_value() is the inverse, for the scores sent to the GUI
Value MonteCarlo::reward_to_value(Reward r) {
    constexpr double g = 203.77396313709564;  // this is 1 / k

    r = std::clamp(r, 5e-4, 1.0 - 5e-4);
    return Value(std::lround(g * std::log(r / (1.0 - r))));
}

/// The minimax value of a move: of its subtree, or of the search of the leaf
Reward MonteCarlo::edge_minimax(const MctsEdge& e) const {
    if (e.child && !(e.flags & MctsEdge::PROVEN))
        return 1.0 - e.child->best.load(std::memory_order_relaxed);

    return e.value;
}

/// The value of a move used to select and to choose: for an expanded move the
/// minimax value mixed with the mean of the values backed up through the move
Reward MonteCarlo::edge_reward(const MctsEdge& e) const {
    const Reward minimax = edge_minimax(e);

    if (!e.child || (e.flags & MctsEdge::PROVEN) || !e.visits)
        return minimax;

    return MinimaxWeight * minimax + (1.0 - MinimaxWeight) * e.sum / e.visits;
}

///////////////////////////////////////////////////////////////////////////////////////
/// The search
///////////////////////////////////////////////////////////////////////////////////////

MonteCarlo::MonteCarlo(Position& p, Search::Worker* w, ThreadPool& pool) :
    pos(p),
    worker(w),
    threads(pool) {

    // Prepare the stack to go down and up in the game tree
    for (int i = -7; i <= MAX_PLY + 9; ++i)
    {
        stack[i]                     = Search::Stack();
        stack[i].continuationHistory = &worker->continuationHistory[0][0][NO_PIECE][0];
        stack[i].continuationCorrectionHistory =
          &worker->continuationCorrectionHistory[NO_PIECE][0];
        stack[i].staticEval = VALUE_NONE;
        stack[i].ply        = std::max(i, 0);
    }

    // The leaves are searched by the alpha-beta search of the worker, far from its root
    worker->accumulatorStack.reset();
    worker->pvIdx     = 0;
    worker->nmpMinPly = 0;

    // "MCTS Multi Strategy" is the percentage of expansions among the visits of a leaf
    rollsToExpand = mctsMultiStrategy ? int((100 - mctsMultiStrategy) / mctsMultiStrategy) : 255;
    rootMinVisits = std::max(1, int(mctsMultiMinVisits));
}

/// MonteCarlo::search() is the main function of the Monte-Carlo algorithm
bool MonteCarlo::search(const Search::LimitsType& limits, bool isMainThread) {

    if (!create_root())
        return false;

    startTime = lastOutputTime = now();

    while (!threads.stop.load(std::memory_order_relaxed))
    {
        iteration();

        if (isMainThread)
        {
            if (main_thread_stop(limits))
                break;

            if (should_emit_pv())
                emit_pv();
        }

        // Every move of the root is proven: nothing to search until the stop
        if (idle)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (isMainThread)
        emit_pv();

    return true;
}

/// MonteCarlo::create_root() finds the root in the tree, or builds it
bool MonteCarlo::create_root() {

    assert(ply == 0);

    {
        std::lock_guard<std::mutex> guard(rootMutex);

        root = MCTS.find(pos.key(), pos.pawn_key());

        if (!root)
        {
            bool      terminal;
            MctsNode* node = build_node(0, terminal);
            root           = node ? MCTS.insert(node) : nullptr;
        }
    }

    if (!root)
        return false;

    // The moves to search may be fewer than the legal ones ("go searchmoves", tablebases)
    const Search::RootMoves& rootMoves = worker->rootMoves;
    bool                     any       = false;

    rootAllowed.assign(root->sons, false);
    for (int i = 0; i < root->sons; ++i)
        if (std::find(rootMoves.begin(), rootMoves.end(), root->edges()[i].move) != rootMoves.end())
            rootAllowed[i] = any = true;

    if (int(rootMoves.size()) == root->sons)
        rootAllowed.clear();

    return any;
}

/// MonteCarlo::iteration() goes down the tree to a leaf, works on it and backs up
void MonteCarlo::iteration() {

    assert(ply == 0);

    idle           = false;
    MctsNode* node = root;

    for (;;)
    {
        const bool isRoot = ply == 0;

        node->lock.lock();

        const int idx = select(node, isRoot);

        if (idx < 0)
        {
            node->lock.unlock();

            if (idx == ALL_PROVEN && !isRoot)
                backup(ply - 1, false);  // the move that leads here becomes proven
            else
            {
                abandon(ply - 1);
                idle = idx == ALL_PROVEN;
                if (!idle)
                    std::this_thread::yield();  // the other threads hold every leaf
            }
            return;
        }

        MctsEdge& e = node->edges()[idx];
        ++e.inflight;  // virtual loss: sends the other threads elsewhere
        path[ply] = {node, idx};

        if (!e.child)
        {
            e.flags |= MctsEdge::BUSY;
            const MctsEdge snapshot = e;
            node->lock.unlock();

            leaf_work(path[ply], snapshot);
            return;
        }

        MctsNode*  child = e.child;
        const Move m     = e.move;
        node->lock.unlock();

        do_move(m, idx + 1);

        if (pos.is_draw(ply) || ply >= MaxTreePly)
        {
            backup(ply - 1, true);
            return;
        }

        node = child;
    }
}

/// MonteCarlo::select() returns the move to follow in a node, with the PUCT formula.
/// The node must be locked.
int MonteCarlo::select(const MctsNode* node, bool isRoot) const {

    const double sqrtVisits = std::sqrt(double(std::max<uint32_t>(node->visits, 1)));

    int    best      = ALL_PROVEN;
    double bestValue = -1e9;
    bool   bestBusy  = false;

    for (int i = 0; i < node->sons; ++i)
    {
        const MctsEdge& e = node->edges()[i];

        if ((e.flags & MctsEdge::PROVEN) || (isRoot && !root_allowed(i)))
            continue;

        const double n    = double(e.visits) + e.inflight;
        const bool   busy = !e.child && (e.flags & MctsEdge::BUSY);

        // A thread below the move counts as a visit that lost
        double value = edge_reward(e);
        if (e.inflight)
            value = value * (e.visits + 1.0) / (n + 1.0);

        value += Cpuct * e.policy * sqrtVisits / (1.0 + n);

        // Every move of the root is searched a few times: a hidden tactic costs the game
        if (isRoot && n < rootMinVisits)
            value += 10.0;

        // A leaf under search is the last choice
        if (busy)
            value -= 100.0;

        if (value > bestValue)
        {
            bestValue = value;
            best      = i;
            bestBusy  = busy;
        }
    }

    return bestBusy ? int(ALL_BUSY) : best;
}

/// MonteCarlo::leaf_work() searches deeper or expands the leaf, then backs up
void MonteCarlo::leaf_work(Step leaf, const MctsEdge& snapshot) {

    const int fatherPly = ply;

    do_move(snapshot.move, leaf.edge + 1);

    MctsNode* child    = nullptr;
    Value     newScore = snapshot.score;
    Depth     newDepth = snapshot.depth;
    bool      proven   = false;
    bool      rolled   = false;
    bool      done     = true;

    if (pos.is_draw(ply))
        // Repetition on this path (the tree is a graph): the search would say the same
        newScore = VALUE_DRAW;

    else if (snapshot.rolls >= rollsToExpand && !MCTS.full() && ply < MaxTreePly)
    {
        child = MCTS.find(pos.key(), pos.pawn_key());  // transposition?

        if (!child)
        {
            // The sons are searched one ply less than the leaf was: the new node knows
            // at least what the leaf knew
            bool      terminal;
            MctsNode* node = build_node(std::max<Depth>(snapshot.depth - 1, 0), terminal);

            if (node)
                child = MCTS.insert(node);
            else if (terminal)
            {
                newScore = pos.checkers() ? VALUE_MATE - 1 : VALUE_DRAW;
                proven   = true;
            }
            else
                done = false;
        }
    }
    else if (snapshot.depth + 1 < MAX_PLY - ply - 2 && ply < MAX_PLY - 8)
    {
        Value value;
        done = deepen(snapshot.depth + 1, -from_node(snapshot.score, fatherPly), value);

        if (done)
        {
            newScore = to_node(-value, fatherPly);
            newDepth = snapshot.depth + 1;
            proven   = is_decisive(newScore);
            rolled   = true;
        }
    }

    // A search cut by the stop gives no value: the tree must not remember it
    if (!done)
    {
        abandon(fatherPly, true);
        return;
    }

    {
        std::lock_guard<MctsSpinLock> guard(leaf.node->lock);
        MctsEdge&                     e = leaf.node->edges()[leaf.edge];

        e.flags &= ~MctsEdge::BUSY;

        if (child)
        {
            e.child = child;
            // The mean restarts from the value of the new subtree
            e.sum = e.visits * (1.0 - child->best.load(std::memory_order_relaxed));
        }
        else
        {
            e.score = newScore;
            e.value = float(value_to_reward(newScore));
            e.depth = uint8_t(newDepth);
            if (rolled && e.rolls < 255)
                ++e.rolls;
            if (proven)
                e.flags |= MctsEdge::PROVEN;
        }
    }

    ++iterations;
    depthSum += ply + newDepth;

    backup(fatherPly, false);
}

/// MonteCarlo::build_node() expands the current position: every legal move is valued
/// by a search of the position that follows, in the order of the move picker. After
/// the first move the window is closed above PolicyMargin under the best value: a
/// move that is clearly worse costs a cutoff, not an exact search.
/// Returns nullptr without moves (terminal is set) or when the search is stopped.
MctsNode* MonteCarlo::build_node(Depth wantedDepth, bool& terminal) {

    struct Candidate {
        Move   move;
        Value  score;
        Depth  depth;
        double weight;
    };

    Candidate candidates[MAX_MOVES];
    int       count = 0;

    terminal = false;

    {
        auto [ttHit, ttData, ttWriter] = worker->tt.probe(pos.key());

        const Move ttMove =
          ttHit && ttData.move && pos.pseudo_legal(ttData.move) && pos.legal(ttData.move)
            ? ttData.move
            : Move::none();

        const PieceToHistory* contHist[] = {
          stack[ply - 1].continuationHistory, stack[ply - 2].continuationHistory,
          stack[ply - 3].continuationHistory, stack[ply - 4].continuationHistory,
          stack[ply - 5].continuationHistory, stack[ply - 6].continuationHistory};

        MovePicker mp(pos, ttMove, Depth(30), &worker->mainHistory, &worker->lowPlyHistory,
                      &worker->captureHistory, contHist, &worker->sharedHistory, ply);

        Move move;
        while ((move = mp.next_move()) != Move::none())
            if (pos.legal(move) && count < MAX_MOVES)
                candidates[count++].move = move;
    }

    if (!count)
    {
        terminal = true;
        return nullptr;
    }

    const bool kingInDanger = Shashin::king_danger(pos, pos.side_to_move());
    Value      best         = -VALUE_INFINITE;

    for (int i = 0; i < count; ++i)
    {
        Candidate& c = candidates[i];

        const bool givesCheck = pos.gives_check(c.move);
        const bool slow       = ply <= 1 || givesCheck || pos.capture(c.move);

        // With the king in danger its moves and the checks come first (Shashin)
        c.weight = !kingInDanger                            ? 0.0
                 : type_of(pos.moved_piece(c.move)) == KING ? 0.9
                 : givesCheck                               ? 0.6
                                                            : 0.0;

        c.depth = std::max(slow ? PriorSlowDepth : PriorFastDepth, wantedDepth);
        c.depth = std::clamp(c.depth, 1, MAX_PLY - ply - 4);

        const Value beta = best == -VALUE_INFINITE
                           ? VALUE_INFINITE
                           : std::clamp(PolicyMargin - best, -VALUE_INFINITE + 1, VALUE_INFINITE);

        do_move(c.move, i + 1);
        const Value value = worker->mcts_value(pos, &stack[ply], c.depth, -VALUE_INFINITE, beta);
        undo_move();

        if (threads.stop.load(std::memory_order_relaxed))
        {
            // The root has no node, but the main thread still has to play a move
            if (ply == 0 && best != -VALUE_INFINITE && worker->is_mainthread())
            {
                auto bestSoFar = std::max_element(
                  candidates, candidates + i,
                  [](const Candidate& a, const Candidate& b) { return a.score < b.score; });

                Search::RootMoves& rootMoves = worker->rootMoves;
                auto it = std::find(rootMoves.begin(), rootMoves.end(), bestSoFar->move);
                if (it != rootMoves.end())
                {
                    it->score = it->uciScore = bestSoFar->score;
                    std::rotate(rootMoves.begin(), it, it + 1);
                }
            }
            return nullptr;
        }

        c.score = -value;
        best    = std::max(best, c.score);
    }

    std::stable_sort(candidates, candidates + count,
                     [](const Candidate& a, const Candidate& b) { return a.score > b.score; });

    // The policy: softmax of the rewards of the sons
    const Reward bestReward = value_to_reward(best);
    double       total      = 0.0;

    for (int i = 0; i < count; ++i)
    {
        Candidate& c = candidates[i];
        c.weight = std::exp((value_to_reward(c.score) - bestReward) / PolicyTemperature + c.weight);
        total += c.weight;
    }

    MctsNode* node = MctsNode::create(pos.key(), pos.pawn_key(), count);

    for (int i = 0; i < count; ++i)
    {
        const Candidate& c = candidates[i];
        MctsEdge&        e = node->edges()[i];

        e.move   = c.move;
        e.depth  = uint8_t(c.depth);
        e.score  = to_node(c.score, ply);
        e.value  = float(value_to_reward(e.score));
        e.policy = float(c.weight / total);

        if (is_decisive(c.score))
            e.flags |= MctsEdge::PROVEN;
    }

    update_node(node, false);

    return node;
}

/// MonteCarlo::deepen() searches the current position (the son of a leaf) one ply
/// deeper, with an aspiration window around the previous value. False when stopped.
bool MonteCarlo::deepen(Depth depth, Value center, Value& result) {

    Value delta = DeepenDelta;
    Value alpha = -VALUE_INFINITE;
    Value beta  = VALUE_INFINITE;

    if (!is_decisive(center))
    {
        alpha = std::max(center - delta, -VALUE_INFINITE);
        beta  = std::min(center + delta, VALUE_INFINITE);
    }

    for (;;)
    {
        result = worker->mcts_value(pos, &stack[ply], depth, alpha, beta);

        if (threads.stop.load(std::memory_order_relaxed))
            return false;

        if (result <= alpha && alpha > -VALUE_INFINITE)
        {
            beta = (alpha + beta) / 2;
            alpha =
              is_decisive(result) ? -VALUE_INFINITE : std::max(result - delta, -VALUE_INFINITE);
        }
        else if (result >= beta && beta < VALUE_INFINITE)
        {
            alpha = std::max(beta - delta, alpha);
            beta  = is_decisive(result) ? VALUE_INFINITE : std::min(result + delta, VALUE_INFINITE);
        }
        else
            return true;

        delta += delta / 2;
    }
}

/// MonteCarlo::update_node() recomputes the value of a node from its moves and
/// proves it when a move wins or every move is proven. The node must be locked
/// (or not yet in the tree).
void MonteCarlo::update_node(MctsNode* node, bool isRoot) const {

    Reward best       = -1.0;
    Value  bestProven = -VALUE_INFINITE;
    bool   allProven  = true;

    for (int i = 0; i < node->sons; ++i)
    {
        const MctsEdge& e = node->edges()[i];

        if (isRoot && !root_allowed(i))
            continue;

        best = std::max(best, edge_reward(e));

        if (e.flags & MctsEdge::PROVEN)
            bestProven = std::max(bestProven, e.score);
        else
            allProven = false;
    }

    if (bestProven != -VALUE_INFINITE && (allProven || is_win(bestProven)))
    {
        node->provenScore.store(bestProven, std::memory_order_relaxed);
        best = value_to_reward(bestProven);
    }
    else
        node->provenScore.store(VALUE_NONE, std::memory_order_relaxed);

    node->best.store(float(best), std::memory_order_relaxed);
}

/// MonteCarlo::backup() goes back to the root: every move of the path gets a visit
/// and the minimax value of its subtree (a draw for a repetition on the path)
void MonteCarlo::backup(int last, bool drawOnPath) {

    assert(ply == last + 1);

    for (int i = last; i >= 0; --i)
    {
        undo_move();

        MctsNode*                     node = path[i].node;
        std::lock_guard<MctsSpinLock> guard(node->lock);
        MctsEdge&                     e = node->edges()[path[i].edge];

        if (e.child && !(e.flags & MctsEdge::PROVEN))
        {
            const Value childScore = e.child->provenScore.load(std::memory_order_relaxed);
            if (childScore != VALUE_NONE)
            {
                e.score = proven_for_father(childScore);
                e.value = float(value_to_reward(e.score));
                e.flags |= MctsEdge::PROVEN;
            }
        }

        --e.inflight;
        ++e.visits;
        ++node->visits;
        e.sum += i == last && drawOnPath ? REWARD_DRAW : edge_minimax(e);

        update_node(node, i == 0);
    }
}

/// MonteCarlo::abandon() goes back to the root without a result
void MonteCarlo::abandon(int last, bool leafBusy) {

    while (ply > 0)
        undo_move();

    for (int i = last; i >= 0; --i)
    {
        std::lock_guard<MctsSpinLock> guard(path[i].node->lock);
        MctsEdge&                     e = path[i].node->edges()[path[i].edge];

        --e.inflight;
        if (i == last && leafBusy)
            e.flags &= ~MctsEdge::BUSY;
    }
}

/// MonteCarlo::do_move() plays a move in the tree. The move goes through the worker,
/// as in the alpha-beta search: nodes are counted and the accumulators follow.
void MonteCarlo::do_move(Move m, int moveCount) {

    assert(ply < MAX_PLY);

    Search::Stack* ss = &stack[ply];

    ss->ply          = ply;
    ss->inCheck      = bool(pos.checkers());
    ss->moveCount    = moveCount;
    ss->excludedMove = Move::none();
    ss->staticEval   = VALUE_NONE;
    ss->statScore    = 0;
    ss->cutoffCnt    = 0;
    ss->reduction    = 0;
    ss->ttPv         = true;
    ss->followPV     = false;

    worker->do_move(pos, m, states[ply], ss);

    ++ply;
    stack[ply].ply = ply;

    if (ply > maxPly)
        maxPly = ply;
}

/// MonteCarlo::undo_move() takes back the last move
void MonteCarlo::undo_move() {

    assert(ply > 0);

    --ply;
    worker->undo_move(pos, stack[ply].currentMove);
}

///////////////////////////////////////////////////////////////////////////////////////
/// The main thread: move to play, time and output
///////////////////////////////////////////////////////////////////////////////////////

/// MonteCarlo::root_choice() returns the move to play: a proven win, else the most
/// visited move, unless a proven result (a draw) is better than its value
MonteCarlo::RootChoice MonteCarlo::root_choice() const {

    std::lock_guard<MctsSpinLock> guard(root->lock);

    int    visited = -1, proven = -1;
    Reward visitedReward = 0, bestReward = -1;

    RootChoice choice;

    for (int i = 0; i < root->sons; ++i)
    {
        if (!root_allowed(i))
            continue;

        const MctsEdge& e = root->edges()[i];
        const Reward    r = edge_reward(e);

        if (e.flags & MctsEdge::PROVEN)
        {
            if (proven < 0 || e.score > root->edges()[proven].score)
                proven = i;
            continue;
        }

        if (r > bestReward)
            bestReward = r, choice.bestValued = i;

        if (visited < 0 || e.visits > root->edges()[visited].visits
            || (e.visits == root->edges()[visited].visits && r > visitedReward))
            visited = i, visitedReward = r;
    }

    choice.mostVisited = visited;

    if (proven >= 0
        && (visited < 0 || is_win(root->edges()[proven].score)
            || visitedReward < root->edges()[proven].value))
    {
        choice.edge   = proven;
        choice.proven = true;
        choice.score  = root->edges()[proven].score;
        choice.reward = root->edges()[proven].value;
    }
    else if (visited >= 0)
    {
        choice.edge   = visited;
        choice.reward = visitedReward;
        choice.score  = reward_to_value(visitedReward);
    }

    if (choice.edge >= 0)
        choice.move = root->edges()[choice.edge].move;

    choice.rootProven = root->provenScore.load(std::memory_order_relaxed) != VALUE_NONE;

    return choice;
}

/// MonteCarlo::main_thread_stop() does for the main thread what iterative deepening
/// and check_time() do in the alpha-beta search: limits of the "go" command and time
/// management. Returns true when the main thread has to leave the search.
bool MonteCarlo::main_thread_stop(const Search::LimitsType& limits) {

    Search::SearchManager* manager = threads.main_manager();
    const TimePoint        elapsed = worker->elapsed();
    const RootChoice       choice  = root_choice();

    if (choice.move != lastBestMove)
    {
        lastBestMove   = choice.move;
        lastChangeTime = elapsed;
        pvChanged      = true;
    }

    worker->rootDepth = reported_depth();

    if (limits.depth && (worker->rootDepth >= limits.depth || maxPly >= 2 * limits.depth))
        return true;

    if (limits.mate && choice.proven && is_win(choice.score)
        && VALUE_MATE - choice.score <= 2 * limits.mate)
    {
        threads.stop = true;
        return true;
    }

    bool stop = (limits.movetime && elapsed >= limits.movetime)
             || (limits.nodes && threads.nodes_searched() >= limits.nodes);

    if (limits.use_time_management() && !stop)
    {
        // More time while the move to play changes, the score falls or the most
        // visited move is not the best valued; less when the choice is stable
        const double stable = elapsed > 0 ? double(elapsed - lastChangeTime) / elapsed : 0.0;
        double       total  = manager->tm.optimum() * (1.35 - 0.75 * stable);

        if (manager->bestPreviousScore != VALUE_INFINITE && !choice.proven)
            total *=
              std::clamp(1.0 + (manager->bestPreviousScore - choice.score) / 400.0, 0.85, 1.35);

        if (choice.bestValued >= 0 && choice.bestValued != choice.edge)
            total *= 1.2;

        if (worker->rootMoves.size() == 1)
            total = std::min(500.0, total);

        const bool timeUp =
          elapsed > std::min(total, double(manager->tm.maximum())) || choice.rootProven;

        // We should not stop pondering until told so by the GUI
        if (manager->ponder)
        {
            if (timeUp)
                manager->stopOnPonderhit = true;
            return false;
        }

        stop = timeUp || manager->stopOnPonderhit;
    }

    if (manager->ponder)
        return false;

    if (stop)
        threads.stop = true;

    return stop;
}

/// MonteCarlo::reported_depth() is the depth sent to the GUI: the mean, over the
/// visits of this thread, of the plies in the tree plus the depth of the leaf search
int MonteCarlo::reported_depth() const {
    return iterations ? std::max(1, int(depthSum / iterations)) : 1;
}

/// MonteCarlo::should_emit_pv() checks how much time has elapsed since the last
/// output, to avoid cluttering the GUI
bool MonteCarlo::should_emit_pv() const {

    const TimePoint current     = now();
    const TimePoint elapsed     = current - startTime + 1;  // in milliseconds
    const TimePoint outputDelay = current - lastOutputTime;

    if (pvChanged && outputDelay >= 200)
        return true;

    if (elapsed < 1100)
        return outputDelay >= 100;
    if (elapsed < 11 * 1000)
        return outputDelay >= 1000;

    return outputDelay >= 5000;
}

/// MonteCarlo::emit_pv() writes the result of the tree in the root moves of the
/// worker and emits the principal variation, as requested by the UCI protocol
void MonteCarlo::emit_pv() {

    assert(ply == 0);

    const RootChoice choice = root_choice();

    lastOutputTime = now();
    pvChanged      = false;

    if (choice.edge < 0)
        return;

    Search::RootMoves& rootMoves = worker->rootMoves;
    const int          selDepth  = std::max(worker->selDepth, maxPly + 1);

    // Scores of all the moves
    {
        std::lock_guard<MctsSpinLock> guard(root->lock);

        for (int i = 0; i < root->sons; ++i)
        {
            const MctsEdge& e  = root->edges()[i];
            auto            it = std::find(rootMoves.begin(), rootMoves.end(), e.move);

            if (it == rootMoves.end())
                continue;

            const Value v = i == choice.edge             ? choice.score
                          : (e.flags & MctsEdge::PROVEN) ? e.score
                                                         : reward_to_value(edge_reward(e));

            it->score = it->uciScore = it->previousScore = it->averageScore = v;
            it->meanSquaredScore                                            = v * v;
            it->selDepth                                                    = selDepth;
            it->unset_inexact();
        }
    }

    // The move to play first, the others by score
    Utility::move_to_front(rootMoves,
                           [&](const Search::RootMove& rm) { return rm == choice.move; });
    std::stable_sort(rootMoves.begin() + 1, rootMoves.end());

    // The principal variation follows the most visited moves of the tree. With MultiPV
    // every printed line gets its own, from the son of its root move.
    const usize multiPV = std::min(usize(worker->options["MultiPV"]), rootMoves.size());

    for (usize line = 0; line < std::max(multiPV, usize(1)); ++line)
    {
        Search::RootMove& rm = rootMoves[line];
        rm.pv.resize(1);

        MctsNode* node = nullptr;

        if (line == 0)
            node = root->edges()[choice.edge].child;
        else
        {
            std::lock_guard<MctsSpinLock> guard(root->lock);

            for (int i = 0; i < root->sons; ++i)
                if (root->edges()[i].move == rm.pv[0])
                {
                    node = root->edges()[i].child;
                    break;
                }
        }

        while (node && int(rm.pv.size()) <= maxPly)
        {
            std::lock_guard<MctsSpinLock> guard(node->lock);

            const MctsEdge* next    = nullptr;
            double          nextKey = 0.0;

            for (int i = 0; i < node->sons; ++i)
            {
                const MctsEdge& e   = node->edges()[i];
                const bool      win = (e.flags & MctsEdge::PROVEN) && is_win(e.score);
                const double    key = win ? 1e12 + e.score : double(e.visits);

                if (key > nextKey)
                    nextKey = key, next = &e;
            }

            if (!next)
                break;

            rm.pv.push_back(next->move);
            node = next->child;
        }
    }

    worker->rootDepth = worker->completedDepth = reported_depth();

    threads.main_manager()->output_pv(*worker, threads, worker->tt, worker->completedDepth);
}

/// MonteCarlo::print_children() emits the statistics of the root moves
void MonteCarlo::print_children() {

    struct Line {
        Move     move;
        uint32_t visits;
        Reward   reward;
        float    policy;
        int      depth;
        bool     proven;
    };

    std::vector<Line> lines;

    {
        std::lock_guard<MctsSpinLock> guard(root->lock);

        for (int i = 0; i < root->sons; ++i)
            if (root_allowed(i))
            {
                const MctsEdge& e = root->edges()[i];
                lines.push_back({e.move, e.visits, edge_reward(e), e.policy, e.depth,
                                 bool(e.flags & MctsEdge::PROVEN)});
            }
    }

    std::stable_sort(lines.begin(), lines.end(),
                     [](const Line& a, const Line& b) { return a.visits < b.visits; });

    for (size_t i = 0; i < lines.size(); ++i)
    {
        const Line& l = lines[i];

        sync_cout << "info string move " << lines.size() - i << " "
                  << UCIEngine::move(l.move, pos.is_chess960()) << std::fixed
                  << std::setprecision(1) << " win% " << l.reward * 100 << " policy% "
                  << l.policy * 100 << " visits " << l.visits << (l.proven ? " proven" : "")
                  << sync_endl;
    }

    sync_cout << "info string mcts nodes " << MCTS.node_count() << " depth " << reported_depth()
              << " seldepth " << maxPly << sync_endl;
}

}  // namespace ShashChess
