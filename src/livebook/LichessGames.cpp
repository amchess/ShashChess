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

#ifdef USE_LIVEBOOK
    #include "LichessGames.h"

    #include <algorithm>
    #include <iterator>

using namespace ShashChess::Livebook;

namespace {

struct RatingBand {
    int         maxElo;  // Upper bound of the band, inclusive
    const char* name;
    const char* ratings;  // Lichess rating groups (each one goes up to the next one)
    const char* players;  // The same groups, readable
};

// The bands of Alexander (handicap mode): Beginner up to 1999, Intermediate 2000-2199,
// Advanced 2200-2399, Expert 2400 and more. The rating groups of the Lichess explorer are 0,
// 1000, 1200, 1400, 1600, 1800, 2000, 2200 (2200-2499) and 2500 (2500 and more).
constexpr RatingBand RATING_BANDS[] = {{1999, "Beginner", "1200,1400,1600,1800", "1200-1999"},
                                       {2199, "Intermediate", "2000", "2000-2199"},
                                       {2399, "Advanced", "2200", "2200-2499"},
                                       {9999, "Expert", "2500", "2500+"}};

const RatingBand& rating_band(const int elo) {
    for (const auto& band : RATING_BANDS)
    {
        if (elo <= band.maxElo)
        {
            return band;
        }
    }

    return RATING_BANDS[std::size(RATING_BANDS) - 1];
}

}

// topGames=0, recentGames=0: only the statistics are needed, not the game references
LichessGames::LichessGames(std::string token_) :
    LichessOpening(std::string(LICHESS_EXPLORER_URL) + "/lichess?topGames=0&recentGames=0&",
                   std::move(token_)) {}

void LichessGames::set_engine_elo(const int elo_) { engineElo = elo_; }

std::string LichessGames::format_url(const Position& position_) {
    const int   elo  = engineElo;
    const auto& band = rating_band(elo);

    std::string fen = position_.fen();
    std::replace(fen.begin(), fen.end(), ' ', '_');

    const std::string full_uri = endpoint + "ratings=" + band.ratings + "&fen=" + fen;

    if (lastBand != band.name)
    {
        lastBand = band.name;
        report(full_uri, "engine Elo " + std::to_string(elo) + " (" + band.name
                           + "): games of the players rated " + band.players);
    }

    return full_uri;
}

#endif
