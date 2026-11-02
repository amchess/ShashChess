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
    #include "LichessEndgame.h"
    #include "json/json.hpp"

using namespace ShashChess::Livebook;

std::string LichessEndgame::parse_uci(const nlohmann::json& move) {
    if (!move.contains("uci") || !move["uci"].is_string())
    {
        return "";
    }

    return move["uci"].get<std::string>();
}

inline Analysis* LichessEndgame::parse_analysis(const nlohmann::json& move) {
    auto category = move["category"].get<std::string>();

    if (category == "unknown")
    {
        return nullptr;
    }

    const auto win = category == "win";

    if (const auto loss = category == "loss"; !win && !loss)
    {
        return new Analysis(new Wdl(0, 1, 0));
    }

    const auto mate      = move["dtm"].get<uint64_t>();
    const auto mate_eval = new Mate(static_cast<int32_t>(mate));

    if (win)
    {
        return new Analysis(mate_eval);
    }

    return new Analysis(mate_eval->opponent());
}

std::string LichessEndgame::format_url(const Position& position) {
    std::string fen_encoded = position.fen();
    std::replace(fen_encoded.begin(), fen_encoded.end(), ' ', '_');  // replace all ' ' to '_'
    const std::string full_uri = "https://tablebase.lichess.ovh/standard?fen=" + fen_encoded;

    return full_uri;
}
#endif
