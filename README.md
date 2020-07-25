## Overview

BrainLearn is a free, powerful UCI chess engine derived from BrainFish ([https://zipproth.de/Brainfish/download/](https://zipproth.de/Brainfish/download/)). It is not a complete chess program and requires a UCI-compatible GUI (e.g. XBoard with PolyGlot, Scid, Cute Chess, eboard, Arena, Sigma Chess, Shredder, Chess Partner or Fritz) in order to be used comfortably. Read the documentation for your GUI of choice for information about how to use Stockfish with it.

## Files

This distribution of BrainLearn consists of the following files:

- Readme.md, the file you are currently reading.
- Copying.txt, a text file containing the GNU General Public License version 3.
- src, a subdirectory containing the full source code, including a Makefile that can be used to compile BrainLearn on Unix-like systems.

## UCI parameters

BrainLearn hash the same options as BrainFish, but it implements a persisted learning algorithm, managing a file named experience.bin.

It is a collection of one or more positions stored with the following format (similar to in memory Stockfish Transposition Table):

- _best move_
- _board signature (hash key)_
- _best move depth_
- _best move score_
- _best move performance_ , a new parameter you can calculate with any learning application supporting this specification. An example is the private one, kernel of SaaS part of [ChessProbe](http://www.chessprobe.com) AI portal. The idea is to calculate it based on pattern recognition concept. In the portal, you can also exploit the reports of another NLG (virtual trainer) application and buy the products in the digishop based on all this. This open-source part has the performance default. So, it doesn't use it. Clearly, even if already strong, this private learning algorithm is a lot stronger as demostrate here: [Graphical result](https://github.com/amchess/BrainLearn/tree/master/tests/6-5.jpg)

This file is loaded in an hashtable at the engine load and updated each time the engine receive quit or stop uci command.
When BrainLearn starts a new game or when we have max 8 pieces on the chessboard, the learning is activated and the hash table updated each time the engine has a best score
at a depth >= 4 PLIES, according to Stockfish aspiration window.

At the engine loading, there is an automatic merge to experience.bin files, if we put the other ones, based on the following convention:

&lt;fileType&gt;&lt;qualityIndex&gt;.bin

where

- _fileType=&quot;experience&quot;/&quot;bin&quot;_
- _qualityIndex_ , an integer, incrementally from 0 on based on the file&#39;s quality assigned by the user (0 best quality and so on)

N.B.

Because of disk access, to be effective, the learning must be made at no bullet time controls (less than 5 minutes/game).

#### Contempt
The default value is 0 and keep it for analysis purpose. For game playing, you can use the default stockfish value 24

#### Dynamic contempt

_Boolean, Default: True_ For match play, activate it and the engine uses the dynamic contempt. For analysis purpose; keep it at its default, to completely avoid, with contempt settled to 0, the well known [rollercoaster effect](http://talkchess.com/forum3/viewtopic.php?t=69129) and align so the engine's score to the gui's informator symbols

### Read only learning

_Boolean, Default: False_ 
If activated, the learning file is only read.

### Self Q-learning

_Boolean, Default: False_ 
If activated, the learning algorithm is the [Q-learning](https://youtu.be/qhRNvCVVJaA?list=PLZbbT5o_s2xoWNVdDudn51XM8lOuZ_Njv), optimized for self play. Some GUIs don't write the experience file in some game's modes because the uci protocol is differently implemented

### Live Book section (thanks to Eman's author Khalid Omar for windows builds)

#### Live Book (checkbox)

_Boolean, Default: False_ If activated, the engine uses the livebook as primary choice.

#### Live Book URL
The default is the online [chessdb](https://www.chessdb.cn/queryc_en/), a wonderful project by noobpwnftw (thanks to him!)
 
[https://github.com/noobpwnftw/chessdb](https://github.com/noobpwnftw/chessdb)

[http://talkchess.com/forum3/viewtopic.php?f=2&t=71764&hilit=chessdb](http://talkchess.com/forum3/viewtopic.php?f=2&t=71764&hilit=chessdb)

The private application can also learn from this live db.

#### Live Book Timeout

_Default 5000, min 0, max 10000_ Only for bullet games, use a lower value, for example, 1500.

#### Live Book Retry

_Default 3, min 1, max 100_ Max times the engine tries to contribute (if the corresponding option is activated: see below) to the live book. If 0, the engine doesn't use the livebook.

#### Live Book Diversity

_Boolean, Default: False_ If activated, the engine varies its play, reducing conversely its strength because already the live chessdb is very large.

#### Live Book Contribute

_Boolean, Default: False_ If activated, the engine sends a move, not in live chessdb, in its queue to be analysed. In this manner, we have a kind of learning cloud.

#### Live Book Depth

_Default 100, min 1, max 100_ Depth of live book moves.

### Opening variety

_Integer, Default: 0, Min: 0, Max: 40_
To play different opening lines from default (0), if not from book (see below).
Higher variety -> more probable loss of ELO

## pgn_to_bl_converter

Converting pgn to brainlearn format is really simple.

Requirements
1. Download cuteChess gui
2. Download brainlearn
3. Download stockfish or equivalent
3. Download the pgn files you want to convert

In Cute Chess, set a tournament according to the photo in this link:
[CuteChess Settings](https://github.com/amchess/BrainLearn/tree/master/doc/pgn_to_bl.PNG)

- Add the 2 engines (One should be brainlearn)

-start a tournament with brainlearn and any other engine. It will convert all the games in the pgn file and save them to game.bin

Note: We recommend you use games from high quality play.

<h1 align="center">ShashChess NNUE</h1>

## Overview
Stockfish NNUE is a port of a shogi neural network named NNUE (efficiently updateable neural network backwards) to Stockfish 11. To learn more about the Stockfish chess engine, look [here](stockfish.md) for an overview and [here](https://github.com/official-stockfish/Stockfish) for the official repository.

## Training Guide
### Generating Training Data
Use the "no-nnue.nnue-gen-sfen-from-original-eval" binary. The given example is generation in its simplest form. There are more commands. 
```
uci
setoption name Threads value x
setoption name Hash value y
setoption name SyzygyPath value path
isready
gensfen depth a loop b use_draw_in_training_data_generation 1 eval_limit 32000
```
Specify how many threads and how much memory you would like to use with the x and y values. The option SyzygyPath is not necessary, but if you would like to use it, you must first have Syzygy endgame tablebases on your computer, which you can find [here](http://oics.olympuschess.com/tracker/index.php). You will need to have a torrent client to download these tablebases, as that is probably the fastest way to obtain them. The path is the path to the folder containing those tablebases. It does not have to be surrounded in quotes.

This will save a file named "generated_kifu.bin" in the same folder as the binary. Once generation is done, rename the file to something like "1billiondepth12.bin" to remember the depth and quantity of the positions and move it to a folder named "trainingdata" in the same directory as the binaries.
#### Generation Parameters
- Depth is the searched depth per move, or how far the engine looks forward. This value is an integer.
- Loop is the amount of positions generated. This value is also an integer
### Generating Validation Data
The process is the same as the generation of training data, except for the fact that you need to set loop to 1 million, because you don't need a lot of validation data. The depth should be the same as before or slightly higher than the depth of the training data. After generation rename the validation data file to val.bin and drop it in a folder named "validationdata" in the same directory to make it easier. 
### Training a Completely New Network
Use the "avx2.halfkp_256x2-32-32.nnue-learn.2020-07-11" binary. Create an empty folder named "evalsave" in the same directory as the binaries.
```
uci
setoption name SkipLoadingEval value true
setoption name Threads value x
isready
learn targetdir trainingdata loop 100 batchsize 1000000 use_draw_in_training 1 use_draw_in_validation 1 eta 1 lambda 1 eval_limit 32000 nn_batch_size 1000 newbob_decay 0.5 eval_save_interval 250000000 loss_output_interval 1000000 mirror_percentage 50 validation_set_file_name validationdata\val.bin
```
Nets get saved in the "evalsave" folder. 

#### Training Parameters
- eta is the learning rate
- lambda is the amount of weight it puts to eval of learning data vs win/draw/loss results. 1 puts all weight on eval, lambda 0 puts all weight on WDL results.

### Reinforcement Learning
If you would like to do some reinforcement learning on your original network, you must first generate training data using the learn binaries. Make sure that your previously trained network is in the eval folder. Use the commands specified above. Make sure `SkipLoadingEval` is set to false so that the data generated is using the neural net's eval by typing the command `uci setoption name SkipLoadingEval value false` before typing the `isready` command. You should aim to generate less positions than the first run, around 1/10 of the number of positions generated in the first run. The depth should be higher as well. You should also do the same for validation data, with the depth being higher than the last run.

After you have generated the training data, you must move it into your training data folder and delete the older data so that the binary does not accidentally train on the same data again. Do the same for the validation data and name it to val-1.bin to make it less confusing. Make sure the evalsave folder is empty. Then, using the same binary, type in the training commands shown above. Do __NOT__ set `SkipLoadingEval` to true, it must be false or you will get a completely new network, instead of a network trained with reinforcement learning. You should also set eval_save_interval to a number that is lower than the amount of positions in your training data, perhaps also 1/10 of the original value. The validation file should be set to the new validation data, not the old data.

After training is finished, your new net should be located in the "final" folder under the "evalsave" directory. You should test this new network against the older network to see if there are any improvements.

## Using Your Trained Net
If you want to use your generated net, copy the net located in the "final" folder under the "evalsave" directory and move it into a new folder named "eval" under the directory with the binaries. You can then use the halfkp_256x2 binaries pertaining to your CPU with a standard chess GUI, such as Cutechess. Refer to the [releases page](https://github.com/nodchip/Stockfish/releases) to find out which binary is best for your CPU.

If the engine does not load any net file, or shows "Error! *** not found or wrong format", please try to sepcify the net with the full file path with the "EvalFile" option by typing the command `setoption name EvalFile value path` where path is the full file path.

## Resources
- [Stockfish NNUE Wiki](https://www.qhapaq.org/shogi/shogiwiki/stockfish-nnue/)
- [Training instructions](https://twitter.com/mktakizawa/status/1273042640280252416) from the creator of the Elmo shogi engine
- [Original Talkchess thread](http://talkchess.com/forum3/viewtopic.php?t=74059) discussing Stockfish NNUE
- [Guide to Stockfish NNUE](http://yaneuraou.yaneu.com/2020/06/19/stockfish-nnue-the-complete-guide/) 
- [Unofficial Stockfish Discord](https://discord.gg/nv8gDtt)

A more updated list can be found in the #sf-nnue-resources channel in the Discord.


## Nets
- [On discord] https://discord.com/channels/435943710472011776/733547343319924808 

## Different profiles:
- profile-build -> Stockfish
- profile-nnue -> Stockfish nnue with pgo build
- nnue -> Stockfish nnue
- nnue-learn -> training a completely new net
- nnue-gen-sfen-from-original-eval -> training data generation

Network structure:

-nnue_architecture.h

	// include a header that defines the input features and network structure
	//#include "architectures/k-p_256x2-32-32.h"
	//#include "architectures/k-p-cr_256x2-32-32.h"
	//#include "architectures/k-p-cr-ep_256x2-32-32.h"
	#include "architectures/halfkp_256x2-32-32.h"
	//#include "architectures/halfkp-cr-ep_256x2-32-32.h"
	//#include "architectures/halfkp_384x2-32-32.h"

	256->20MB (5 different based on the input to the NN): short time controls
	384->30MB: long time controls
	
	Some others architectures, perhaps better in the future:
	there are even more architecture possible and you can find some in ttak repositity:  https://github.com/tttak/Stockfish
	The uncommented 256MB net is compatible with the current 20MB nets.

	For MinGW build on windows, use 7.3.0 and experimental/filesystem (not simple filesystem)
	Add CBlas as Curl:lib and includes.


## Terms of use

ShashChess is free, and distributed under the **GNU General Public License version 3** (GPL v3). Essentially, this means that you are free to do almost exactly what you want with the program, including distributing it among your friends, making it available for download from your web site, selling it (either by itself or as part of some bigger software package), or using it as the starting point for a software project of your own.

The only real limitation is that whenever you distribute BrainLearn in some way, you must always include the full source code, or a pointer to where the source code can be found. If you make any changes to the source code, these changes must also be made available under the GPL.

For full details, read the copy of the GPL v3 found in the file named _Copying.txt_.

