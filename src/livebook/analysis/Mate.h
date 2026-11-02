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

#ifndef MATE_H
#define MATE_H
#ifdef USE_LIVEBOOK
    #include <cstdint>


class Mate {
   public:
    explicit Mate(int32_t mate);
    ~Mate() = default;

    [[nodiscard]] Mate* opponent() const;

    [[nodiscard]] int32_t get_mate() const;

    [[nodiscard]] Mate* flip() const;

    bool operator==(const Mate& other) const;

    bool operator!=(const Mate& other) const;

    bool operator<(const Mate& other) const;

    bool operator>(const Mate& other) const;

    bool operator<=(const Mate& other) const;

    bool operator>=(const Mate& other) const;

   protected:
    int32_t mate;
};

#endif
#endif  //MATE_H