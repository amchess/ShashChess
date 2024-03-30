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

#ifndef UCI_H_INCLUDED
#define UCI_H_INCLUDED

#include <iostream>
#include <string>
#include <unordered_map>

#include "evaluate.h"
#include "misc.h"
#include "position.h"
#include "thread.h"
#include "tt.h"
#include "book/book_manager.h"  //book management
#include "ucioption.h"

namespace ShashChess {

namespace Eval::NNUE {
enum NetSize : int;
}

class Move;
enum Square : int;
using Value = int;

class UCI {
   public:
    UCI(int argc, char** argv);

    void loop();

    static int         to_cp(Value v);
    static std::string value(Value v);
    static std::string square(Square s);
    static std::string move(Move m, bool chess960);
    static std::string wdl(Value v, int ply);
    static uint8_t     getWinProbability(int v, int ply);
    static Move        to_move(const Position& pos, std::string& str);

    const std::string& workingDirectory() const { return cli.workingDirectory; }

    OptionsMap options;

    std::unordered_map<Eval::NNUE::NetSize, Eval::EvalFile> evalFiles;

   private:
    TranspositionTable tt;
    ThreadPool         threads;
    BookManager        bookMan;  //book management
    CommandLine        cli;

    void go(Position& pos, std::istringstream& is, StateListPtr& states);
    void bench(Position& pos, std::istream& args, StateListPtr& states);
    void position(Position& pos, std::istringstream& is, StateListPtr& states);
    void trace_eval(Position& pos);
    void search_clear();
    void setoption(std::istringstream& is);
};

//begin no uci option, but constant
enum {
    NODES_TIME = 0,
};
//end no uci option, but constant
}  // namespace ShashChess

#endif  // #ifndef UCI_H_INCLUDED
