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

#ifndef BASE_LIVEBOOK_H
#define BASE_LIVEBOOK_H
#ifdef USE_LIVEBOOK

    #include <map>
    #include <string>
    #include "analysis/Analysis.h"

    #include "../position.h"

    #define CURL_STATICLIB
extern "C" {
    #include <curl/curl.h>
}
    #undef min
    #undef max

namespace ShashChess::Livebook {
class BaseLivebook {
   public:
    // Virtual destructor for proper cleanup of derived classes
    virtual ~BaseLivebook() = default;

    // Pure virtual function to lookup analysis for a given UCI position
    virtual std::vector<std::pair<std::string, Analysis>> lookup(const Position& position) = 0;

   protected:
    // CURL* curl = nullptr; // cURL handle
    std::string readBuffer;  // Buffer to store the response data

    void clean_buffer_from_terminator();

    // Function to perform an HTTP request
    CURLcode do_request(const std::string& uri);
};
}

#endif
#endif  //BASE_LIVEBOOK_H
