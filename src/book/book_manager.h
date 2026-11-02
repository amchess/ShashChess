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

#ifndef BOOKMANAGER_H_INCLUDED
#define BOOKMANAGER_H_INCLUDED

namespace ShashChess {
namespace Book {
class Book;
}

class BookManager {
   public:
    static constexpr int NumberOfBooks = 2;

   private:
    Book::Book* books[NumberOfBooks];

   public:
    BookManager();
    virtual ~BookManager();

    BookManager(const BookManager&)            = delete;
    BookManager& operator=(const BookManager&) = delete;

    void init(const OptionsMap& options);
    void init(int index, const OptionsMap& options);
    Move probe(const Position& pos, const OptionsMap& options) const;
    void show_moves(const Position& pos, const OptionsMap& options) const;
};
}

#endif
