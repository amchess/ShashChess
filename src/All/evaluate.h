/*
  ShashChess, a UCI chess playing engine derived from Stockfish
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2015 Marco Costalba, Joona Kiiski, Tord Romstad
  Copyright (C) 2015-2019 Marco Costalba, Joona Kiiski, Gary Linscott, Tord Romstad

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

class Position;

namespace Eval {

  std::string trace(const Position& pos);
  Value evaluate(const Position& pos);

  extern bool useNNUE;
  extern std::string eval_file_loaded;
  void init_NNUE();
  void verify_NNUE();

  namespace NNUE {

    Value evaluate(const Position& pos);
    Value compute_eval(const Position& pos);
    void  update_eval(const Position& pos);
    bool  load_eval_file(const std::string& evalFile);

  } // namespace NNUE

} // namespace Eval

#endif // #ifndef EVALUATE_H_INCLUDED
