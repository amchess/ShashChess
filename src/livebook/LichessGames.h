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

#ifndef LICHESS_GAMES_H
#define LICHESS_GAMES_H
#ifdef USE_LIVEBOOK
    #include <atomic>

    #include "LichessOpening.h"


namespace ShashChess::Livebook {
class LichessGames final: public LichessOpening {
   public:
    explicit LichessGames(std::string token_);

    // Strength of the engine: UCI_Elo when UCI_LimitStrength is enabled, otherwise the
    // maximum (set by the search before probing the live books)
    static void set_engine_elo(int elo_);

   protected:
    // Only the games of the Lichess players in the rating band of the engine
    std::string format_url(const Position& position_) override;

   private:
    static inline std::atomic<int> engineElo{3190};

    std::string lastBand;  // To report the band only when it changes
};
}

#endif  //LICHESS_GAMES_H
#endif
