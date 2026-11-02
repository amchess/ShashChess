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
    #include "LichessLivebook.h"

    #include "../uci.h"

using namespace ShashChess::Livebook;

std::vector<std::pair<std::string, Analysis>> LichessLivebook::lookup(const Position& position) {
    const std::string full_uri = format_url(position);
    auto              ret      = std::vector<std::pair<std::string, Analysis>>();

    if (const CURLcode res = do_request(full_uri); res != CURLE_OK)
    {
        return ret;
    }

    try
    {
        auto data = nlohmann::json::parse(readBuffer);

        if (!data.contains("moves") || !data["moves"].is_array())
        {
            std::cerr << "Error parsing JSON: \"moves\" not found" << std::endl
                      << readBuffer << std::endl;
            return ret;
        }

        auto moves = data["moves"];

        for (auto move : moves)
        {
            if (!move.contains("uci") || !move["uci"].is_string())
            {
                std::cerr << "Error parsing JSON: \"uci\" not found" << std::endl
                          << readBuffer << std::endl;
                continue;
            }

            auto uci_move = move["uci"].get<std::string>();

            if (const auto uci = UCIEngine::to_move(position, uci_move); !uci)
            {
                continue;
            }

            auto analysis = parse_analysis(move);

            if (analysis == nullptr)
            {
                continue;
            }

            if (position.side_to_move() == BLACK)
            {
                analysis = analysis->flip();
            }

            auto element = std::pair(uci_move, *analysis);
            ret.push_back(element);
        }
    } catch (const nlohmann::json::exception& e)
    {
        std::cerr << "Error parsing JSON: " << e.what() << std::endl << readBuffer << std::endl;
        return ret;
    }

    return ret;
}

#endif
