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

#include "../uci.h"
#include "polyglot/polyglot.h"
#include "ctg/ctg.h"
#include "book_manager.h"

namespace ShashChess {
BookManager::BookManager() {
    for (int i = 0; i < NumberOfBooks; ++i)
        books[i] = nullptr;
}

BookManager::~BookManager() {
    for (int i = 0; i < NumberOfBooks; ++i)
        delete books[i];
}

void BookManager::init(const OptionsMap& options) {
    for (size_t i = 0; i < NumberOfBooks; ++i)
        init(i, options);
}

void BookManager::init(int index, const OptionsMap& options) {
    assert(index < NumberOfBooks);

    //Close previous book if any
    delete books[index];
    books[index] = nullptr;

    std::string filename =
      std::string(options[Util::format_string("CTG/BIN Book %d File", index + 1)]);

    //Load new book
    if (Util::is_empty_filename(filename))
        return;

    //Create book object for the given book type
    std::string fn   = Util::map_path(filename);
    Book::Book* book = Book::Book::create_book(fn);
    if (book == nullptr)
    {
        sync_cout << "info string Unknown book type: " << filename << sync_endl;
        return;
    }

    //Open/Initialize the book
    if (!book->open(fn))
    {
        delete book;
        return;
    }

    books[index] = book;
}

Move BookManager::probe(const Position& pos, const OptionsMap& options) const {
    int  moveNumber = 1 + pos.game_ply() / 2;
    Move bookMove   = Move::none();

    for (size_t i = 0; i < NumberOfBooks; ++i)
    {
        if (books[i] != nullptr
            && int(options[Util::format_string("Book %d Depth", i + 1)]) >= moveNumber)
        {
            bookMove = books[i]->probe(
              pos, size_t(int(options[Util::format_string("Book %d Width", i + 1)])),
              bool(options[Util::format_string("(CTG) Book %d Only Green", i + 1)]));
            if (bookMove != Move::none())
                break;
        }
    }

    return bookMove;
}

void BookManager::show_moves(const Position& pos, const OptionsMap& options) const {
    std::cout << pos << std::endl << std::endl;

    for (size_t i = 0; i < NumberOfBooks; ++i)
    {
        if (books[i] == nullptr)
        {
            std::cout << "Book " << i + 1 << ": No book loaded" << std::endl;
        }
        else
        {
            std::cout << "Book " << i + 1 << " (" << books[i]->type() << "): "
                      << std::string(options[Util::format_string("CTG/BIN Book %d File", i + 1)])
                      << std::endl;
            books[i]->show_moves(pos);
        }
    }
}
}