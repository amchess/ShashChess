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

#ifndef LICHESS_OPENING_H
#define LICHESS_OPENING_H
#ifdef USE_LIVEBOOK

    #include "LichessLivebook.h"

namespace ShashChess::Livebook {
class LichessOpening: public LichessLivebook {
   public:
    // endpoint_ must end with '?' or '&': the position is appended as "fen=..."
    // token_ is the Lichess API token (required by the explorer since March 2026)
    LichessOpening(std::string endpoint_, std::string token_);
    ~LichessOpening() override = default;

   protected:
    std::string endpoint;
    std::string token;

    std::string parse_uci(const nlohmann::json& move_) override;
    Analysis*   parse_analysis(const nlohmann::json& move_, const Position& position_) override;
    std::string format_url(const Position& position_) override;

    [[nodiscard]] std::vector<std::string> request_headers() const override;
    [[nodiscard]] std::string              auth_hint() const override;

    // Statistics of games: a move is equivalent to the best one if its success rate (wins +
    // draws / 2, the win probability of the Shashin zones) is in the same Shashin zone and,
    // as for the BIN books, it has been played in at least 0.5% of the games
    [[nodiscard]] bool is_equivalent(const Analysis& best_,
                                     const Analysis& other_,
                                     const Moves&    moves_) const override;
};
}

#endif  //LICHESS_OPENING_H
#endif
