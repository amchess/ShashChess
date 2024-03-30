/*
  ShashChess, a UCI chess playing engine derived from Stockfish
  Copyright (C) 2004-2024 Andrea Manzo, K.Kiniama and ShashChess developers (see AUTHORS file)

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

#include <iostream>
#include <unordered_map>

#include "bitboard.h"
#include "evaluate.h"
#include "misc.h"
#include "position.h"
#include "tune.h"
#include "types.h"
#include "uci.h"
#include "learn/learn.h"  //learning
using namespace ShashChess;

int main(int argc, char* argv[]) {

    std::cout << engine_info() << std::endl;
    UCI uci(argc, argv);   //Khalid
    LD.init(uci.options);  //Kelly
    Bitboards::init();
    Position::init();
    Tune::init(uci.options);

    uci.evalFiles = Eval::NNUE::load_networks(uci.workingDirectory(), uci.options, uci.evalFiles);

    uci.loop();

    return 0;
}
