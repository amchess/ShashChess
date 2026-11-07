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

    #include <atomic>
    #include <chrono>
    #include <cstddef>
    #include <map>
    #include <string>
    #include <utility>
    #include <vector>
    #include "analysis/Analysis.h"

    #include "../position.h"

    #define CURL_STATICLIB
extern "C" {
    #include <curl/curl.h>
}
    #undef min
    #undef max

namespace ShashChess::Livebook {

// The moves of a live book (UCI notation) with their analysis, from the point of view of the
// side to move
using Moves = std::vector<std::pair<std::string, Analysis>>;

class BaseLivebook {
   public:
    BaseLivebook()                               = default;
    BaseLivebook(const BaseLivebook&)            = delete;
    BaseLivebook& operator=(const BaseLivebook&) = delete;

    // Virtual destructor for proper cleanup of derived classes
    virtual ~BaseLivebook();

    // Pure virtual function to lookup analysis for a given UCI position
    virtual std::vector<std::pair<std::string, Analysis>> lookup(const Position& position) = 0;

    // The moves among which the book move is chosen at random (option "LiveBook Width"): the
    // best move first, then the moves equivalent to it (see is_equivalent), at most width_.
    // The moves are sorted with Analysis::operator>; on ties the order of the server is kept
    // (e.g. the online tablebases send the moves best first).
    [[nodiscard]] std::vector<std::string> choices(Moves moves_, size_t width_) const;

    // One of the choices, with the same probability (as the CTG/BIN books do with their width)
    static std::string pick(const std::vector<std::string>& choices_);

    // Maximum duration of an HTTP request in milliseconds. The lookup is done
    // while our clock is running, so a request must never be allowed to hang.
    static void set_timeout(long timeout_ms_);
    // Further limit set by the search from the time for the current move (0 = no limit)
    static void set_time_budget(long budget_ms_);
    // The duration allowed to a request: the timeout, within the time budget
    static long get_timeout();

   protected:
    std::string readBuffer;      // Buffer to store the response data
    long        httpStatus = 0;  // HTTP status of the last request (0 = no response)

    void clean_buffer_from_terminator();

    // True if other_ can be played instead of best_, the best move of moves_. By default no
    // move is equivalent to the best one: the best move is always played.
    [[nodiscard]] virtual bool
    is_equivalent(const Analysis& best_, const Analysis& other_, const Moves& moves_) const;

    // Function to perform an HTTP request
    CURLcode do_request(const std::string& uri, const std::vector<std::string>& headers_ = {});

    // Perform the request and validate the answer. Returns true only if a usable
    // body was received. On failure an "info string" is printed and, depending on
    // the error, the live book is paused for a while (rate limit, server down) or
    // until it is reconfigured (credentials refused). If partial_ok_ is set, a
    // streamed answer cut by the timeout is accepted (see LichessPlayer).
    bool fetch(const std::string&              uri,
               const std::vector<std::string>& headers_    = {},
               bool                            partial_ok_ = false,
               const std::string&              auth_hint_  = "");

    static void report(const std::string& uri, const std::string& message);

   private:
    static inline std::atomic<long> timeoutMs{5000};
    static inline std::atomic<long> budgetMs{0};

    // Kept between the requests: the connection to the server is reused (no new TLS handshake)
    CURL* curl = nullptr;

    std::chrono::steady_clock::time_point suspendedUntil{};

    [[nodiscard]] bool is_suspended() const;
    void               suspend_for(std::chrono::seconds duration_);
    void               suspend_until_reconfigured();
};
}

#endif
#endif  //BASE_LIVEBOOK_H
