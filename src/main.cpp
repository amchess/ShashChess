/*
  Alexander, a UCI chess playing engine derived from Stockfish
  Copyright (C) 2004-2025 Andrea Manzo, F. Ferraguti, K.Kiniama and Stockfish developers (see AUTHORS file)

  Alexander is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Alexander is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <iostream>
//for classical begin
#include <unordered_map>
#include "evaluate.h"
#include "endgame.h"
#include "psqt.h"
//for classical end
#include "bitboard.h"
#include "misc.h"
#include "position.h"
#include "uci.h"
#include "tune.h"
#include "wdl/win_probability.h"
#include "learn/learn.h"  //learning
using namespace Alexander;

int main(int argc, char* argv[]) {

    std::cout << engine_info() << std::endl;

    WDLModel::init();

    Bitboards::init();
    Position::init();

    UCIEngine uci(argc, argv);
    LD.init(uci.engine_options());  //Kelly
    Tune::init(uci.engine_options());
    //for classical begin
    Eval::initHandicapMode(uci.engine_options());
    Bitbases::init();
    Eval::loadAvatar(uci.engine_options()["Avatar File"]);  //handicap mode
    PSQT::init();
    Bitbases::init();
    Endgames::init();
    //for classical end
    uci.loop();

    return 0;
}
