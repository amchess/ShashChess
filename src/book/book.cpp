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

#include "../misc.h"
#include "../uci.h"
#include "polyglot/polyglot.h"
#include "ctg/ctg.h"
#include "book.h"

namespace ShashChess {
namespace Book {
/*static*/ Book* Book::create_book(const std::string& filename) {
    size_t extIndex = filename.find_last_of('.');
    if (extIndex == std::string::npos)
        return nullptr;

    std::string ext = filename.substr(extIndex + 1);

    if (ext == "ctg" || ext == "cto" || ext == "ctb")
        return new CTG::CtgBook();
    else if (ext == "bin")
        return new Polyglot::PolyglotBook();
    else
        return nullptr;
}
}
}