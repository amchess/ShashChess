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

// The tablebase gives the category of each move from the point of view of the side to
// move *after* the move (the opponent): "loss" is the best category for us. The moves
// are already sorted best first, so a score per category is enough: within the same
// category the first move wins the ties (see Search::Worker::start_searching).
// No color flip: the score is already from the point of view of the side to move.
Analysis* LichessEndgame::parse_analysis(const nlohmann::json&            move,
                                         [[maybe_unused]] const Position& position) {
    static const std::map<std::string, int32_t> CATEGORY_SCORE = {
      {"loss", 40000},         // We win
      {"syzygy-loss", 30000},  // We win (but DTZ is rounded)
      {"maybe-loss", 20000},   // We win (50-move rule outcome unknown)
      {"blessed-loss", 1},     // We would win without the 50-move rule
      {"draw", 0},             //
      {"cursed-win", -1},      // We would lose without the 50-move rule
      {"maybe-win", -20000},   // We lose (50-move rule outcome unknown)
      {"syzygy-win", -30000},  // We lose (but DTZ is rounded)
      {"win", -40000}};        // We lose

    if (!move.contains("category") || !move["category"].is_string())
    {
        return nullptr;
    }

    const auto score = CATEGORY_SCORE.find(move["category"].get<std::string>());

    // "unknown": the position is not in the tablebases, let the engine search
    if (score == CATEGORY_SCORE.end())
    {
        return nullptr;
    }

    return new Analysis(new Cp(score->second));
}

std::string LichessEndgame::format_url(const Position& position) {
    std::string fen_encoded = position.fen();
    std::replace(fen_encoded.begin(), fen_encoded.end(), ' ', '_');  // replace all ' ' to '_'
    const std::string full_uri =
      std::string(LICHESS_TABLEBASE_URL) + "/standard?fen=" + fen_encoded;

    return full_uri;
}
#endif
