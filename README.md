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

### Never Clear Hash (checkbox)

This option prevents the Hash Memory from being cleared between successive games or positions
belonging to different games. Check this option also if you want to Load the Hash from disk file,
otherwise your loaded Hash could be cleared by a subsequent ucinewgame or Clear Hash
command.

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

## Handicap mode

A very refined handicap mode based on the four famous sovietic chess school levels:

- _beginner: elo < 2000_
- _intermediate: 2000 <= elo < 2200_
- _advanced: 2200 <= elo < 2400_
- _expert: elo > 2400_
Every school correspond to a different evaluation function, more and more refined.

### UCI_LimitStrength

Activate the strength limit specified in the UCI_Elo parameter.

### UCI_Elo

_Default 2800, min 1500, max 2800_
UCI-protocol compliant version of Strength parameter.
Internally the UCI_Elo value will be converted to a Strength value according to the table given
above.
The UCI_Elo feature is controlled by the chess GUI, and usually doesn't appear in the configuration
window.

## Hash save capability

The goal is to use an hash saving capability to allow the user to recover a previous analysis session and continue it. 
The code is from Daniel Josè.
The saved hash file will be of the same size of the hash memory, so if you defined 4 GB of hash, such will be the file size. Saving and loading such big files can take some time.
You can set the NeverClearHash option to avoid that the hash could be cleared by a Clear Hash or ucinewgame command.

### HashFile

The full file name with path information. If you don't set the path, it will be saved in the current folder. It defaults to hash.hsh.


### SaveHashtoFile

To save the hash, stop the analysis and press this button in the uci options screen of the GUI.

### Load Hash from File

To load the hash file, load the game you are interested in, load the engine withouth starting it, and press the LoadHashfromFile button in the uci options screen of the GUI. Now you can start the analysis.

### LoadEpdToHash

It loads EPD on offer

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

### Clean Search

If on, it always resets search state to its initial value

### Variety

_Integer, Default: 0, Min: 0, Max: 40_
To play different lines from default (0), if not from book (see below).
Higher variety -> more probable loss of ELO

## Book management

Polyglot and Cerebellum opening books, to instant play

### Book enabled

If enabled, ShashChess will try to use the Polyglot/Cerebellum opening book specified in the "Book
File" parameter.

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

- Maurizio Platino, a CCE and CCM ICCF player (https://www.iccf.com/player?id=241094), the official tester
- The Stockfish team
- The Sugar team: 
    - Marco Zerbinati, for the optimized windows builds and for his forum http://mzchessforum.altervista.org) and 
    - Sergey Aleksandrovitch Kozlov for his very interesting patch and code
- The mzforum user hagtorp for his pretious suggestions about the android version
- The BrainFish, McBrain, CorChess, CiChess and MateFinder authors for their very interesting derivative
- Obviously, the chess theorician Alexander Shashin, whithout whom I wouldn't had the idea of this engine

Sorry If I forgot someone.