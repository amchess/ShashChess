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

#ifndef WDL_H
#define WDL_H
#ifdef USE_LIVEBOOK
    #include <cstdint>

class Wdl {
   public:
    Wdl(uint32_t wins_, uint32_t draws_, uint32_t losses_);

    ~Wdl() = default;

    [[nodiscard]] Wdl* opponent() const;

    [[nodiscard]] uint32_t get_wins() const;
    [[nodiscard]] uint32_t get_draws() const;
    [[nodiscard]] uint32_t get_losses() const;

    [[nodiscard]] double get_win_probability() const;
    [[nodiscard]] double get_draw_probability() const;
    [[nodiscard]] double get_loss_probability() const;

    [[nodiscard]] double get_success_probability() const;

    [[nodiscard]] Wdl* flip() const;

    bool operator==(const Wdl& other) const;

    bool operator!=(const Wdl& other) const;

    bool operator<(const Wdl& other) const;

    bool operator>(const Wdl& other) const;

    bool operator<=(const Wdl& other) const;

    bool operator>=(const Wdl& other) const;

   protected:
    uint32_t wins;
    uint32_t draws;
    uint32_t losses;

    [[nodiscard]] uint32_t get_sum() const;
};

#endif  //USE_LIVEBOOK
#endif  //WDL_H
