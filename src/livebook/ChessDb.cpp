#ifdef USE_LIVEBOOK
    #include "ChessDb.h"

    #include <algorithm>
    #include <cmath>
    #include <iostream>
    #include <sstream>
    #include "../uci.h"

using namespace ShashChess::Livebook;

    #define CHESS_DB_ENDPOINT "https://www.chessdb.cn/cdb.php"
    #define DEFAULT_ACTION Action::QUERY_ALL

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


std::pair<std::string, Analysis>
ChessDb::parse_move(const Position& position_, std::string& item_, bool check_rank_) const {
    std::string move_type = item_.substr(0, item_.find(':'));
    std::string move      = item_.substr(item_.find(':') + 1);

    std::stringstream stream(item_);
    std::string       token;
    std::string       key, value;

    int         rank = 0;
    std::string uci;
    auto        analysis = new Analysis();

    while (std::getline(stream, token, ','))
    {
        key = token.substr(0, token.find(':'));

        if (key == "rank")
        {
            value = token.substr(token.find(':') + 1);
            rank  = std::stoi(value);

            if (check_rank_ && rank < this->min_rank)
            {
                break;
            }
        }

        value = token.substr(token.find(':') + 1);

        if (key == "move")
        {
            if (auto uci_move = UCIEngine::to_move(position_, value); !uci_move)
            {
                uci = "";
                break;
            }

            uci = value;
        }
        else if (key == "score")
        {
            auto score_value = std::stoi(value);
            auto score       = new Cp(score_value);
            analysis->set_centi_pawns(score);
        }
        else if (key == "mate")
        {
            auto mate_value = std::stoi(value);
            auto mate       = new Mate(mate_value);
            analysis->set_mate(mate);
        }
        else if (key == "winrate")
        {
            auto     wdl_value = std::stof(value);
            auto     wins      = static_cast<uint32_t>(std::round(wdl_value * 10));
            uint32_t losses    = 1000 - wins;
            auto     wdl       = new Wdl(wins, 0, losses);
            analysis->set_wdl(wdl);
        }
    }

    auto element = std::pair(uci, *analysis);

    return element;
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

    if (const CURLcode res = do_request(full_uri); res != CURLE_OK)
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

    const std::string full_uri =
      this->endpoint + "?action=" + action_str + +"&board=" + fen_encoded;

    return full_uri;
}

    #undef DEFAULT_ACTION
    #undef CHESS_DB_ENDPOINT

#endif
