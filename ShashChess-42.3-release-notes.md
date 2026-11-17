# ShashChess 42.3

## New Monte-Carlo Tree Search

ShashChess 42.3 replaces the previous MCTS with a new one. It is used only with "MCTS by Shashin" on (off by default), in the Petrosian positions where the side to move has a win probability up to 15% (MCTS Explore extends it to other kinds of positions). Everything else is as in 42.3.

- **Hybrid and pure modes.** With MCTSThreads < Threads the main thread searches with alpha-beta and the first MCTSThreads helpers search the tree, cooperating through the shared transposition table and histories. With MCTSThreads >= Threads every thread, the main one included, searches the tree and the move played comes from the tree. In 42.3 the main thread always searched with alpha-beta, so with a single thread MCTS never ran.
- **A new tree.** Moves are selected with the PUCT formula, with priors from shallow alpha-beta searches. A visited leaf is first searched deeper by alpha-beta, one ply per visit, and then expanded. Mates, tablebase results and stalemates are proven and propagated up the tree (MCTS-Solver). The tree is kept from one move to the next and may use as much memory as the Hash option (64 MB at least), besides the hash table.
- **MCTS Multi Strategy and MCTS Multi MinVisits** keep their names and defaults but have a new meaning: the percentage of expansions among the visits of a leaf, and the minimum number of visits of every root move. Both now apply with any number of MCTS threads (see the README).
- **Info line.** At the start of every search an "info string" line tells whether MCTS was started, with the mode and the number of MCTS threads, or not, with the reason.
- **MultiPV.** The hybrid mode now works with MultiPV greater than 1 too (before, MCTS was off with MultiPV > 1). Pure MCTS stays off with MultiPV > 1, because the tree would only report the lines without searching them: the search is then the usual alpha-beta one.
- **Unchanged without MCTS.** With "MCTS by Shashin" off the search is the same as in 42.3: same bench, 2443546 nodes.

## Test results

Peter M. tested the new MCTS with his test-suite rater, mostly on the 55 positions of a 506-position suite where MCTS starts (Petrosian zone), with MCTSThreads 1 and 10 runs per side.

| 55 Petrosian positions, solved with full points (mean of 10 runs) | New MCTS | Compared with | p |
|---|---|---|---|
| 5 s, Threads 3, against MCTS off | 37.0 | 34.6 | < 0.001 |
| 5 s, Threads 3, against the previous MCTS (42.3) | 35.9 | 33.0 | 0.001 |
| 5 s, Threads 3, MultiPV 3, against MCTS off | 42.3 | 41.0 | 0.03 |
| 5 s, Threads 2, against MCTS off (*) | 35.4 | 33.0 | 0.003 |
| 30 s, Threads 2, against MCTS off (*) | 42.1 | 39.1 | < 0.001 |

p: one-sided permutation test on the totals of the 10 + 10 runs. Each row is a separate series of runs, so figures from different rows are not directly comparable.

(*) With Threads 2 and MCTSThreads 1 the only helper runs MCTS, so the Shashin root analysis, otherwise done by the last helper, moves to the main thread, also where MCTS does not start. These runs measure both changes together; with Threads 3 the root analysis stays with the last helper.

- **Where the lead comes from.** At 5 s mostly from two Neghina studies (with Threads 3, solved in 9 runs out of 10 against 1, and 10 against 4), with smaller gains on Liburkin 1939 and ERET 52. At 30 s alpha-beta catches up on the Neghina studies, and the lead comes from positions it does not find even with more time: Lejeune 2016 (9 against 0), Popov (7 against 0) and Liburkin 1939 (6 against 0). Two positions went the other way at 30 s, the fortress CCC 14 (3 against 8) and HTC 131 (4 against 8), neither significant on its own.
- **MultiPV 3** already broadens the alpha-beta search at the root, so the extra gain of the hybrid mode is smaller there.
- **Whole suites.** On the whole 506-position suite and on a 4096-position suite (Threads 2, 3 runs per side), where MCTS starts in 11% and 7% of the positions, the differences were not significant: 389 against 398 and 2976 against 2964 solved on average.
- **Pure MCTS on a single thread** was not stronger than alpha-beta: 375 against 380 solved on the whole 506-position suite at 5 s (333 with MCTS Explore on too). On a 325-position suite at 90 s it solved 220 against 228 with MultiPV 1 and 252 against 268 with MultiPV 3, compared with ShashChess 42 (same alpha-beta search, same bench). Hence the MultiPV rule above.

Example: Liburkin 1939, `8/p7/qkpN1p2/8/1B1P4/PP3K2/5P2/8 w - -`, White wins with 1.Ba5+. With 30 threads, 6 of them MCTS helpers, ShashChess has 1.Ba5+ with a winning score after 0.01 s. With 30 threads and MCTS off it needed 83 s in one run, and two more runs did not get it either. Pure MCTS on a single thread has it from 0.12 s.

## Suggested settings

Use the hybrid mode, with more threads than MCTS threads: for instance Threads = 3 and MCTSThreads = 1. With the default values (Threads 1, MCTSThreads 1), turning on "MCTS by Shashin" gives pure MCTS on a single thread. MCTS Explore was tested only with pure MCTS on a single thread, where it solved fewer positions.

## Thanks

Many thanks to Peter M. for the extensive testing, and to Stephan Nicolet for the original MCTS work.
