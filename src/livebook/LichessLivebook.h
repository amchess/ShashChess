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

#ifndef JSON_LIVEBOOK_H
#define JSON_LIVEBOOK_H
#ifdef USE_LIVEBOOK

    #include "json/json.hpp"
    #include "BaseLivebook.h"

namespace ShashChess::Livebook {
class LichessLivebook: public BaseLivebook {
   public:
    ~LichessLivebook() override = default;

    std::vector<std::pair<std::string, Analysis>> lookup(const Position& position) override;

   protected:
    virtual std::string format_url(const Position& position)       = 0;
    virtual std::string parse_uci(const nlohmann::json& move)      = 0;
    virtual Analysis*   parse_analysis(const nlohmann::json& move) = 0;
};
}

#endif  //JSON_LIVEBOOK_H
#endif
