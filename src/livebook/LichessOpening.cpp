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
    #include "json/json.hpp"

using namespace ShashChess::Livebook;

LichessOpening::LichessOpening(std::string endpoint_) :
    endpoint(std::move(endpoint_)) {}

std::string LichessOpening::parse_uci(const nlohmann::json& move_) {
    if (!move_.contains("uci") || !move_["uci"].is_string())
    {
        return "";
    }

    return move_["uci"].get<std::string>();
}

Analysis* LichessOpening::parse_analysis(const nlohmann::json& move_) {
    const auto wins = move_.contains("white") ? move_["white"].get<uint64_t>() : 0;

    const auto draws = move_.contains("draws") ? move_["draws"].get<uint64_t>() : 0;

    const auto losses = move_.contains("black") ? move_["black"].get<uint64_t>() : 0;

    const auto wdl      = new Wdl(wins, draws, losses);
    const auto analysis = new Analysis(wdl);

    return analysis;
}

std::string LichessOpening::format_url(const Position& position_) {
    std::string fen_encoded = position_.fen();
    std::replace(fen_encoded.begin(), fen_encoded.end(), ' ', '_');
    const std::string full_uri = this->endpoint + "fen=" + fen_encoded;

    return full_uri;
}
#endif
