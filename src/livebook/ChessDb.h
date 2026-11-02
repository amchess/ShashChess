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

#ifndef CHESS_DB_H
#define CHESS_DB_H
#ifdef USE_LIVEBOOK

    #include "BaseLivebook.h"

namespace ShashChess::Livebook {

enum class Action {
    QUERY_ALL,
    QUERY_BEST,
    QUERY,
    QUERY_SEARCH,
};

class ChessDb: public BaseLivebook {
   public:
    ChessDb();
    explicit ChessDb(std::string endpoint_);
    explicit ChessDb(Action action_);
    explicit ChessDb(std::string endpoint_, Action action_);
    ~ChessDb() override = default;

    std::vector<std::pair<std::string, Analysis>> lookup(const Position& position_) override;

    void set_action(Action action_);
    void set_min_rank(int min_rank_);

   protected:
    Action      action;
    std::string endpoint;
    int         min_rank = 2;

    std::pair<std::string, Analysis>
    parse_move(const Position& position_, std::string& item_, bool check_rank_) const;

    std::vector<std::pair<std::string, Analysis>> parse_query_all(const Position& position_);
    std::vector<std::pair<std::string, Analysis>> parse_query_best(const Position& position_);

    [[nodiscard]] std::string format_uri(const Position& position_) const;
};
}


#endif
#endif  //CHESS_DB_H
