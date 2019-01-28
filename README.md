# Introduction

ShashChess is a free UCI chess engine derived from Stockfish family chess engines.
The goal is to apply Alexander Shashin theory exposed on the following book :
https://www.amazon.com/Best-Play-Method-Discovering-Strongest/dp/1936277468
to improve

- base engine strength
- engine’s behaviour on the different positions types (requiring the corresponding algorithm) :
    - Tal
    - Capablanca
    - Petrosian
    - the mixed ones
       * Tal-Capablanca
       * Capablanca-Petrosian
       * Tal-Capablanca-Petrosian

## Terms of use

Shashchess is free, and distributed under the **GNU General Public License** (GPL). Essentially, this

means that you are free to do almost exactly what you want with the program, including distributing

it among your friends, making it available for download from your web site, selling it (either by

itself or as part of some bigger software package), or using it as the starting point for a software

project of your own.

The only real limitation is that whenever you distribute ShashChess in some way, you must always

include the full source code, or a pointer to where the source code can be found. If you make any

changes to the source code, these changes must also be made available under the GPL.

For full details, read the copy of the GPL found in the file named _Copying.txt_.

## Files

This distribution of ShashChessPro consists of the following files:

- Readme.md, the file you are currently reading.
- Copying.txt, a text file containing the GNU General Public License.
- src, a subdirectory containing the full source code, including a Makefile and the compilation
    scripts makeAll.bat (Windows) and makeAll.sh (Linux).

## Uci options

## Hash Memory

### Hash

_Integer, Default: 16, Min: 1, Max: 131072 MB (64-bit) : 2048 MB (32-bit)_


The amount of memory to use for the hash during search, specified in MB (megabytes). This
number should be smaller than the amount of physical memory for your system.
A modern formula to determine it is the following:

_(T x S / 100) MB_
where
_T = the average move time (in seconds)
S = the average node speed of your hardware_
A traditional formula is the following:
_(N x F x T) / 512_
where
_N = logical threads number
F = clock single processor frequency (MB)
T = the average move time (in seconds)_

### Clear Hash

Button to clear the Hash Memory.
If the Never Clear Hash option is enabled, this button doesn't do anything.

## Analysis Contempt

This option has no effect in the playing mode.
A non-zero contempt is determined by Shashin's options and used only during game play, not during infinite analysis where it's turned off.
This helps make analysis consistent when switching sides and exploring various lines and lets you include a non-zero Contempt in your analysis. 
Note when playing against the computer, if you wish to use a non-zero Contempt, either turn off “White Contempt” so that Contempt will apply to the Computer’s side, or you can use the above description to set an appropriate Contempt for the specific side that ShashChess is playing. 
Please note if “White Contempt” is off, in infinite search or analysis mode, ShashChess will always use a value of 0 for Contempt.
If you use this option, you can analyse with contempt settled for white, black or for all points of view.
Obviously, this option can produce an asymmetry in the evaluations (the evaluation changes when you switch sides). So, be aware!

## Threads

_Integer, Default: 1, Min: 1, Max: 512_
The number of threads to use during the search. This number should be set to the number of cores
(physical+logical) in your CPU.

## Large Pages

Use of the large memory pages if provided by the operating system, gaining in speed.
To enable this feature in Windows, you need to modify the _Group Policy_ for your account:

1. _Run: gpedit.msc_ (or search for "Group Policy").
2. Under "Computer Configuration", "Windows Settings", "Security Settings", "Local Policies"
click on "User Rights Assignment".
3. In the right pane double-click the option "Lock Pages in Memory".
4. Click on "Add User or Group" and add your account or "Everyone".
5. You may have to logoff or reboot for the change to take effect.

IMPORTANT: You'll also need to run your chess GUI with administrative rights ("Run as
Administrator") or disable UAC in Windows.
Very often large memory pages will only be available shortly after booting Windows.

## Ponder (checkbox)

_Boolean, Default: True_
Also called "Permanent Brain" : whether or not the engine should analyze when it is the opponent's
turn.


Usually not on the configuration window.

## MultiPV

_Integer, Default: 1, Min: 1, Max: 500_
The number of alternate lines of analysis to display. Specify 1 to just get the best line. Asking for
more lines slows down the search.
Usually not on the configuration window.

## UCI_Chess960 (checkbox)

Whether or not ShashChess should play using Chess 960 mode. Usually not on the configuration
window.

## Move overhead 

_Default 30, min 0, max 5000_
In ms, the default value seems to be the best on Linux systems, but must be increased for slow GUI like Fritz. In general, on Windows system it seems a good value to be 100.

## Slow mover

_Default 84, min 10, max 1000_
 "Time usage percent": how much the engine thinks on a move. Many engines seem to move faster and the engine is behind in time clock. With lower values it plays faster, with higher values slower - of course always within the time control.

## Handicap mode

### UCI_LimitStrength

Activate the strength limit by a weaker play in a random fashion to simulate human blunders.

### UCI_Elo

_Default 2800, min 1500, max 2800_
UCI-protocol compliant version of Strength parameter.
A very refined handicap mode based on the four famous sovietic chess school levels:
Internally the UCI_Elo value will be converted to a Strength value according to the following table:

- _beginner: elo < 2000_
- _intermediate: 2000 <= elo < 2200_
- _advanced: 2200 <= elo < 2400_
- _expert: elo > 2400_

Every school correspond to a different evaluation function, more and more refined.
The UCI_Elo feature is controlled by the chess GUI, and usually doesn't appear in the configuration
window.

## Sygyzy End Game table bases

Download at [http://olympuschess.com/egtb/sbases](http://olympuschess.com/egtb/sbases) (by Ronald De Man)

### SyzygyPath

The path to the Syzygy endgame tablebases.this defines an absolute path on your computer to the
tablebase files, also on multiple paths separated with a semicolon (;) character (Windows), the colon
(:) character (OS X and Windows) character.
The folder(s) containing the Syzygy EGTB files. If multiple folders are used, separate them by the ;
(semicolon) character.

### SygyzyProbeDepth

_Integer, Default: 1, Min: 1, Max: 100_
The probing tablebases depth (always the root position).
If you don't have a SSD HD,you have to set it to maximize the depth and kn/s in infinite analysis
and during a time equals to the double of that corresponding to half RAM size.
Choice a test position with a few pieces on the board (from 7 to 12). For example:

- Fen: _8/5r2/R7/8/1p5k/p3P3/4K3/8 w -- 0 1_ Solution : Ra4 (=)
- Fen: _1R6/7k/1P5p/5p2/3K2p1/1r3P1P/8 b - - 1 1_ Solution: 1...h5 !! (=)


### SygyzyProbeLimit

_Integer, Default: 6, Min: 0, Max: 6_
How many pieces need to be on the board before ShashChess begins probing (even at the root).
Current default, obviously, is for 6-man.

## Advanced Chess Analyzer

Advanced analysis options, highly recommended for CC play

### Deep Analysis Mode

_Default: 0, Min: 0, Max: 8_
By default, no MultiPV. Using MultiPV, higher depths and longer time to reach them.
So, fewer tactical shots missed, but loss of some ELO, increasingly until 8, corresponding to
multiPV = 256.
Recommended values: from 1 to 4 ( > 4 too wide search width)

### Variety

_Integer, Default: 0, Min: 0, Max: 40_
To play different lines from default (0), if not from book (see below).
Higher variety -> more probable loss of ELO


### MCTS
_Boolean, Default: False_

Experimental, MonteCarloTreeSearch, if activated, the engine's behaviour is similar to AlphaZero concepts.
For now, two ideas are implemented, integrated on Shashin's theory:

- [https://github.com/Stefano80/Stockfish/compare/0365b08...ad6b324](playout by Stefano Cardanobile) for quiescent positions
- [https://github.com/Kellykinyama12/Stockfish] (montecarlo by Kelly Kinyama) only when true. This creates three files for machine learning purposes:
	-experience.bin when no more than 40 moves are played, there are non more than 6 pieces on the chessboard and at a not low depth in analysis
	-pawngame.bin when there are no more than 2 pieces and the game's phase is not the ending
	-openings.bin, in the form <positionKey>.bin (>=1) at the initial stage of game with memorized the move played, the depth and the score.
In this mode, the engine is not less strong than Stockfish in a match play without learning, but a lot better in analysis mode and to solve hard positions.
With learning, the engine became stronger and stronger.
The default mode, conversely, is stronger than Stockfish in a match play, but not as good as MCTS for the rest and can't improve its play because of no learning enabled.

## Book management

Polyglot and Cerebellum opening books, to instant play

### Book file

File name for Polyglot/Cerebellum opening book (usually with ".bin" extension).

### Best Book Move (checkbox)

When disabled (the default value), ShashChess will randomly pick a move taking into account the
weights of the various available moves, based on the probability.
If enabled, ShashCHess will always use the best book move (the move with the highest weight)
found in the opening book.


### Book depth

If 0, full opening book moves depth. If not, depth to the settled value.

## Shashin section

_Default: no option settled_
The engine will determine dynamically the position's type starting from a "Capablanca/default
positions".
If one or more (mixed algorithms/positions types at the boundaries) of the three following options
are settled, it will force the initial position/algorithm understanding

### Tal

Attack position/algorithm

### Capablanca

Strategical algorithm (for quiescent positions)

### Petrosian

Defense position/algorithm (the "reversed colors" Tal)

## Acknowledgments

- ICCF GM Matjaž Pirš (https://www.iccf.com/player?id=480232), for his great experience and tests on positions analysis in different game's phases
- ICCF CCE and CCM Maurizio Platino (https://www.iccf.com/player?id=241094), the official tester
- The Stockfish team
- The Sugar team: 
    - Marco Zerbinati, for the optimized windows builds and for his forum http://mzchessforum.altervista.org) and 
    - Sergey Aleksandrovitch Kozlov for his very interesting patch and code
- Alexei Chernakoff for his pretious suggestions about the android version and its contribution to it
- Dariusz Domagala for the Mac version
- The BrainFish, McBrain, CorChess, CiChess and MateFinder authors for their very interesting derivative
- Obviously, the chess theorician Alexander Shashin, whithout whom I wouldn't had the idea of this engine

Sorry If I forgot someone.
