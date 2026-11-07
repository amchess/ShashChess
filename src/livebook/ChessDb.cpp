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
    #include "ChessDb.h"

    #include <algorithm>
    #include <cerrno>
    #include <climits>
    #include <cmath>
    #include <cstdlib>
    #include <iostream>
    #include <sstream>
    #include "../uci.h"

using namespace ShashChess::Livebook;

    #define CHESS_DB_ENDPOINT "https://www.chessdb.cn/cdb.php"
    #define DEFAULT_ACTION Action::QUERY_ALL

namespace {

// The values sent by ChessDB may be unknown ("score:??"): parse them without exceptions
bool to_int(const std::string& value_, int& result_) {
    char* end = nullptr;
    errno     = 0;

    const long value = std::strtol(value_.c_str(), &end, 10);

    if (end == value_.c_str() || errno == ERANGE || value < INT_MIN || value > INT_MAX)
    {
        return false;
    }

    result_ = static_cast<int>(value);
    return true;
}

bool to_double(const std::string& value_, double& result_) {
    char* end = nullptr;

    const double value = std::strtod(value_.c_str(), &end);

    if (end == value_.c_str() || !std::isfinite(value))
    {
        return false;
    }

    result_ = value;
    return true;
}

}

ChessDb::ChessDb() :
    action(DEFAULT_ACTION),
    endpoint(CHESS_DB_ENDPOINT) {}

ChessDb::ChessDb(std::string endpoint_) :
    action(DEFAULT_ACTION),
    endpoint(std::move(endpoint_)) {}

ChessDb::ChessDb(const Action action_) :
    action(action_),
    endpoint(CHESS_DB_ENDPOINT) {}

ChessDb::ChessDb(std::string endpoint_, const Action action_) :
    action(action_),
    endpoint(std::move(endpoint_)) {}


// An item is "key:value,key:value,...", e.g. "move:e2e4,score:35,rank:2,note:! (12-05),
// winrate:52.65" for queryall, "move:e2e4" or "egtb:e2e4" (tablebase move) for querybest
// and query. An empty move is returned if the item has to be discarded.
std::pair<std::string, Analysis>
ChessDb::parse_move(const Position& position_, std::string& item_, bool check_rank_) const {
    const std::pair<std::string, Analysis> discarded{"", Analysis()};

    std::stringstream stream(item_);
    std::string       token;

    std::string uci;
    Analysis    analysis;

    while (std::getline(stream, token, ','))
    {
        const size_t      colon = token.find(':');
        const std::string key   = token.substr(0, colon);
        const std::string value = colon == std::string::npos ? "" : token.substr(colon + 1);

        if (key == "move" || key == "egtb")
        {
            if (!UCIEngine::to_move(position_, value))
            {
                return discarded;
            }

            uci = value;
        }
        else if (key == "rank")
        {
            // "move" comes before "rank": discard the move
            if (int rank = 0; check_rank_ && (!to_int(value, rank) || rank < this->min_rank))
            {
                return discarded;
            }
        }
        else if (key == "score")
        {
            // Unknown score ("??"): the move cannot be compared with the others
            int score = 0;

            if (!to_int(value, score))
            {
                return discarded;
            }

            analysis.set_centi_pawns(new Cp(score));
        }
        else if (key == "mate")
        {
            if (int mate = 0; to_int(value, mate))
            {
                analysis.set_mate(new Mate(mate));
            }
        }
        else if (key == "winrate")
        {
            // Percentage with two decimals: per mille
            if (double winrate = 0; to_double(value, winrate))
            {
                const auto wins =
                  static_cast<uint32_t>(std::lround(std::clamp(winrate, 0.0, 100.0) * 10));
                analysis.set_wdl(new Wdl(wins, 0, 1000 - wins));
            }
        }
    }

    return {uci, analysis};
}

std::vector<std::pair<std::string, Analysis>> ChessDb::parse_query_all(const Position& position_) {
    std::vector<std::pair<std::string, Analysis>> moves = {};

    this->clean_buffer_from_terminator();

    if (this->readBuffer == "invalid board" || this->readBuffer == "nobestmove")
    {
        return moves;
    }

    std::stringstream ss(this->readBuffer);
    std::string       item;

    while (std::getline(ss, item, '|'))
    {
        auto element = parse_move(position_, item, true);

        if (element.first.empty())
        {
            continue;
        }

        moves.push_back(element);
    }

    return moves;
}

std::vector<std::pair<std::string, Analysis>> ChessDb::parse_query_best(const Position& position_) {
    std::vector<std::pair<std::string, Analysis>> moves = {};

    this->clean_buffer_from_terminator();

    if (this->readBuffer == "invalid board" || this->readBuffer == "nobestmove")
    {
        return moves;
    }

    std::stringstream ss(this->readBuffer);
    std::string       item;

    while (std::getline(ss, item, '|'))
    {
        auto element = parse_move(position_, item, false);

        if (element.first.empty())
        {
            continue;
        }

        moves.push_back(element);
    }

    return moves;
}

std::vector<std::pair<std::string, Analysis>> ChessDb::lookup(const Position& position_) {
    const std::string full_uri = format_uri(position_);

    auto ret = std::vector<std::pair<std::string, Analysis>>();

    if (!fetch(full_uri))
    {
        return ret;
    }

    std::vector<std::pair<std::string, Analysis>> output;

    switch (this->action)
    {
    case Action::QUERY_ALL :
        output = parse_query_all(position_);
        break;

    case Action::QUERY_BEST :
    case Action::QUERY :
    case Action::QUERY_SEARCH :
        output = parse_query_best(position_);
        break;
    default :
        break;
    }

    return output;
}

void ChessDb::set_action(const Action new_action_) { this->action = new_action_; }

void ChessDb::set_min_rank(const int new_min_rank_) {
    if (new_min_rank_ < 0)
    {
        return;
    }

    if (new_min_rank_ > 2)
    {
        return;
    }

    this->min_rank = new_min_rank_;
}

std::string ChessDb::format_uri(const Position& position_) const {
    auto fen_encoded = position_.fen();

    std::replace(fen_encoded.begin(), fen_encoded.end(), ' ',
                 '_');  // replace all ' ' to '_'

    std::string action_str;
    switch (this->action)
    {
    case Action::QUERY_ALL :
        action_str = "queryall";
        break;

    case Action::QUERY_BEST :
        action_str = "querybest";
        break;

    case Action::QUERY :
        action_str = "query";
        break;

    case Action::QUERY_SEARCH :
        action_str = "querysearch";
        break;

    default :
        action_str = "queryall";
    }

    const std::string full_uri = this->endpoint + "?action=" + action_str + "&board=" + fen_encoded;

    return full_uri;
}

bool ChessDb::is_equivalent([[maybe_unused]] const Analysis& best_,
                            [[maybe_unused]] const Analysis& other_,
                            [[maybe_unused]] const Moves&    moves_) const {
    return true;
}

    #undef DEFAULT_ACTION
    #undef CHESS_DB_ENDPOINT

#endif
