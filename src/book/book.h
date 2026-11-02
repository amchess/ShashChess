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

#ifndef BOOK_H_INCLUDED
#define BOOK_H_INCLUDED
#include "../movegen.h"
namespace ShashChess {
class BookManager;

namespace Book {
namespace {
static const union {
    uint32_t i;
    char     c[4];
} Le                          = {0x01020304};
static const bool IsBigEndian = (Le.c[0] == 1);
}

class BookUtil {
   public:
    template<typename IntType>
    static IntType read_big_endian(const unsigned char* buffer, size_t& offset, size_t bufferLen) {
        IntType          result;
        constexpr size_t typeSize = sizeof(IntType);

        if (offset + typeSize > bufferLen)
        {
            assert(false);
            return IntType();
        }

        //Read the integer type and convert (if needed)
        memcpy(&result, buffer + offset, typeSize);

        if (!IsBigEndian)
        {
            unsigned char                              u[typeSize];
            typename std::make_unsigned<IntType>::type v = 0;

            memcpy(&u, &result, typeSize);
            for (size_t i = 0; i < typeSize; ++i)
                v = (v << 8) | u[i];

            memcpy(&result, &v, typeSize);
        }

        offset += typeSize;
        return result;
    }

    template<typename IntType>
    static IntType read_big_endian(const unsigned char* buffer, size_t bufferLen) {
        size_t offset = 0;
        return read_big_endian<IntType>(buffer, offset, bufferLen);
    }
};

class Book {
    friend class ShashChess::BookManager;

   private:
    static Book* create_book(const std::string& filename);

   public:
    Book() {}
    virtual ~Book() {}

    Book(const Book&)            = delete;
    Book& operator=(const Book&) = delete;

    virtual std::string type() const = 0;

    virtual bool open(const std::string& filename) = 0;
    virtual void close()                           = 0;

    virtual Move probe(const Position& pos, size_t width, bool onlyGreen) const = 0;
    virtual void show_moves(const Position& pos) const                          = 0;
};
}
}

#endif  // #ifndef BOOK_H_INCLUDED
