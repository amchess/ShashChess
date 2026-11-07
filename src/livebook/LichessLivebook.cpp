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

    #include <memory>

    #include "../movegen.h"
    #include "../uci.h"

using namespace ShashChess::Livebook;

namespace ShashChess::Livebook {
namespace {

// The opening explorer encodes castling as "king takes rook" (e1h1, e8a8, ...) also in
// standard chess, while UCIEngine::to_move() only accepts e1g1 there.
Move to_engine_move(const Position& position, const std::string& uci_move) {
    if (const Move move = UCIEngine::to_move(position, uci_move))
    {
        return move;
    }

    const std::string lower = UCIEngine::to_lower(uci_move);

    for (const auto& move : MoveList<LEGAL>(position))
    {
        if (move.type_of() == CASTLING && UCIEngine::move(move, true) == lower)
        {
            return move;
        }
    }

    return Move::none();
}

// Newline delimited JSON: every line is a newer and complete snapshot. Return the last
// valid one (the last line may be truncated if the stream has been cut by the timeout).
std::string last_json_line(const std::string& buffer) {
    size_t end = buffer.size();

    while (end > 0)
    {
        const size_t newline = buffer.rfind('\n', end - 1);
        const size_t begin   = newline == std::string::npos ? 0 : newline + 1;
        std::string  line    = buffer.substr(begin, end - begin);

        if (line.find_first_not_of(" \t\r\n") != std::string::npos && nlohmann::json::accept(line))
        {
            return line;
        }

        if (newline == std::string::npos)
        {
            break;
        }

        end = newline;
    }

    return "";
}

}
}

std::vector<std::pair<std::string, Analysis>> LichessLivebook::lookup(const Position& position) {
    const std::string full_uri = format_url(position);
    auto              ret      = std::vector<std::pair<std::string, Analysis>>();

    if (!fetch(full_uri, request_headers(), is_nd_json(), auth_hint()))
    {
        return ret;
    }

    const std::string payload = is_nd_json() ? last_json_line(readBuffer) : readBuffer;

    if (payload.empty())
    {
        return ret;
    }

    try
    {
        const auto data = nlohmann::json::parse(payload);

        if (!data.contains("moves") || !data["moves"].is_array())
        {
            report(full_uri, "unexpected answer, \"moves\" not found");
            return ret;
        }

        for (const auto& move : data["moves"])
        {
            const std::string uci_move = parse_uci(move);

            if (uci_move.empty())
            {
                report(full_uri, "unexpected answer, \"uci\" not found");
                continue;
            }

            const Move engine_move = to_engine_move(position, uci_move);

            if (!engine_move)
            {
                continue;
            }

            const std::unique_ptr<Analysis> analysis(parse_analysis(move, position));

            if (analysis == nullptr)
            {
                continue;
            }

            // Store the move in the engine notation (e1g1, not e1h1, in standard chess)
            ret.emplace_back(UCIEngine::move(engine_move, position.is_chess960()), *analysis);
        }
    } catch (const nlohmann::json::exception& e)
    {
        report(full_uri, std::string("unexpected answer, ") + e.what());
        ret.clear();
    }

    return ret;
}

#endif
