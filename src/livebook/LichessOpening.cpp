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
    #include "LichessOpening.h"

    #include <algorithm>
    #include <cmath>
    #include <cstdint>
    #include <limits>

    #include "json/json.hpp"
    #include "../shashin/shashin_types.h"

using namespace ShashChess::Livebook;

namespace {

// Shashin zone of the results of the games: win probability = success rate
ShashChess::ShashinPosition shashin_range(const Wdl& wdl_) {
    const auto win  = static_cast<int>(std::lround(100 * wdl_.get_success_probability()));
    const auto draw = static_cast<int>(std::lround(100 * wdl_.get_draw_probability()));

    return ShashChess::shashin_range_by_probability(win, draw);
}

uint32_t games_count(const nlohmann::json& move_, const char* key_) {
    if (!move_.contains(key_) || !move_[key_].is_number_integer())
    {
        return 0;
    }

    const auto games = move_[key_].get<int64_t>();

    return static_cast<uint32_t>(
      std::clamp<int64_t>(games, 0, std::numeric_limits<uint32_t>::max()));
}

}

LichessOpening::LichessOpening(std::string endpoint_, std::string token_) :
    endpoint(std::move(endpoint_)),
    token(std::move(token_)) {}

std::string LichessOpening::parse_uci(const nlohmann::json& move_) {
    if (!move_.contains("uci") || !move_["uci"].is_string())
    {
        return "";
    }

    return move_["uci"].get<std::string>();
}

Analysis* LichessOpening::parse_analysis(const nlohmann::json& move_, const Position& position_) {
    const auto white = games_count(move_, "white");
    const auto draws = games_count(move_, "draws");
    const auto black = games_count(move_, "black");

    // The explorer counts the results from White's point of view
    const auto wdl = position_.side_to_move() == WHITE ? new Wdl(white, draws, black)
                                                       : new Wdl(black, draws, white);

    return new Analysis(wdl);
}

std::string LichessOpening::format_url(const Position& position_) {
    std::string fen_encoded = position_.fen();
    std::replace(fen_encoded.begin(), fen_encoded.end(), ' ', '_');
    const std::string full_uri = this->endpoint + "fen=" + fen_encoded;

    return full_uri;
}

std::vector<std::string> LichessOpening::request_headers() const {
    if (token.empty())
    {
        return {};
    }

    return {"Authorization: Bearer " + token};
}

std::string LichessOpening::auth_hint() const {
    if (token.empty())
    {
        return ". The opening explorer requires a Lichess API token: create one (no scope is "
               "needed) at https://lichess.org/account/oauth/token and set it in the option "
               "'LiveBook Lichess Token'";
    }

    return ". The token in 'LiveBook Lichess Token' is invalid, expired or revoked";
}

bool LichessOpening::is_equivalent(const Analysis& best_,
                                   const Analysis& other_,
                                   const Moves&    moves_) const {
    if (!best_.has_wdl() || !other_.has_wdl())
    {
        return false;
    }

    uint64_t games = 0;

    for (const auto& move : moves_)
    {
        if (move.second.has_wdl())
        {
            games += move.second.get_wdl()->get_games();
        }
    }

    // As the BIN books: no move played in less than 0.5% of the games
    if (other_.get_wdl()->get_games() * 200 < games)
    {
        return false;
    }

    return shashin_range(*best_.get_wdl()) == shashin_range(*other_.get_wdl());
}
#endif
