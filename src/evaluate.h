/*
  ShashChess, a UCI chess playing engine derived from Stockfish
  Copyright (C) 2004-2025 The ShashChess developers (see AUTHORS file)

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

#ifndef EVALUATE_H_INCLUDED
#define EVALUATE_H_INCLUDED

#include <string>

#include "types.h"

namespace ShashChess {

class Position;

namespace Eval {

// The default net name MUST follow the format nn-[SHA256 first 12 digits].nnue
// for the build process (profile-build and fishtest) to work. Do not change the
// name of the macro or the location where this macro is defined, as it is used
// in the Makefile/Fishtest.
#define EvalFileDefaultNameBig "nn-1c0000000000.nnue"
#define EvalFileDefaultNameSmall "nn-37f18f62d772.nnue"

namespace NNUE {
struct Networks;
struct AccumulatorCaches;
class AccumulatorStack;
}

std::string trace(Position& pos, const Eval::NNUE::Networks& networks);

int   simple_eval(const Position& pos);
bool  use_smallnet(const Position& pos);
Value evaluate(const NNUE::Networks&          networks,
               const Position&                pos,
               Eval::NNUE::AccumulatorStack&  accumulators,
               Eval::NNUE::AccumulatorCaches& caches,
               int                            optimism);
}  // namespace Eval

}  // namespace ShashChess

#endif  // #ifndef EVALUATE_H_INCLUDED
