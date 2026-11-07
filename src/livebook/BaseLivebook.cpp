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
    #include "BaseLivebook.h"
    #include <algorithm>
    #include <cstdint>
    #include <iostream>
    #include <random>
    #include "../misc.h"
using namespace ShashChess::Livebook;

namespace {

// Identify the client: some services (e.g. Lichess) reject or throttle anonymous agents
constexpr const char* USER_AGENT = "ShashChess-LiveBook (+https://github.com/amchess/ShashChess)";

// How long a live book stays silent after a network error / server error / rate limit
constexpr std::chrono::seconds RETRY_DELAY{30};
constexpr std::chrono::seconds RATE_LIMIT_DELAY{60};  // Lichess: "wait one minute"

// Never print the query string (it contains the position)
std::string endpoint_of(const std::string& uri) { return uri.substr(0, uri.find('?')); }

}

static size_t curl_write(void* contents, const size_t size, const size_t nmemb, void* userp) {
    const size_t new_size = size * nmemb;
    const auto   buffer   = static_cast<std::string*>(userp);

    buffer->append(static_cast<char*>(contents), new_size);

    return new_size;
}

void BaseLivebook::set_timeout(const long timeout_ms_) { timeoutMs = std::max(timeout_ms_, 1L); }

void BaseLivebook::set_time_budget(const long budget_ms_) { budgetMs = std::max(budget_ms_, 0L); }

long BaseLivebook::get_timeout() {
    const long timeout = timeoutMs;
    const long budget  = budgetMs;

    return budget > 0 ? std::min(timeout, budget) : timeout;
}

std::vector<std::string> BaseLivebook::choices(Moves moves_, const size_t width_) const {
    std::vector<std::string> result;

    if (moves_.empty() || width_ == 0)
    {
        return result;
    }

    // Best first. Stable: on ties the order of the server is kept
    std::stable_sort(moves_.begin(), moves_.end(),
                     [](const auto& a, const auto& b) { return a.second > b.second; });

    const Analysis& best = moves_.front().second;

    for (const auto& [uci, analysis] : moves_)
    {
        if (result.size() >= width_)
        {
            break;
        }

        if (result.empty() || is_equivalent(best, analysis, moves_))
        {
            result.push_back(uci);
        }
    }

    return result;
}

std::string BaseLivebook::pick(const std::vector<std::string>& choices_) {
    if (choices_.empty())
    {
        return "";
    }

    // Only the main thread probes the live books
    static std::mt19937_64 randomEngine(uint64_t(std::random_device{}()) ^ uint64_t(now()));

    return choices_[std::uniform_int_distribution<size_t>(0, choices_.size() - 1)(randomEngine)];
}

bool BaseLivebook::is_equivalent([[maybe_unused]] const Analysis& best_,
                                 [[maybe_unused]] const Analysis& other_,
                                 [[maybe_unused]] const Moves&    moves_) const {
    return false;
}

void BaseLivebook::clean_buffer_from_terminator() {
    if (readBuffer.empty())
    {
        return;
    }

    if (const auto last_char = readBuffer.back();
        last_char == '\n' || last_char == '\r' || last_char == '\0')
    {
        readBuffer.pop_back();
    }
}

BaseLivebook::~BaseLivebook() {
    if (curl)
    {
        curl_easy_cleanup(curl);
    }
}

// Perform an HTTP request to the given URI and store the response in readBuffer
CURLcode BaseLivebook::do_request(const std::string&              uri,
                                  const std::vector<std::string>& headers_) {
    readBuffer.clear();
    httpStatus = 0;

    // The handle is kept between the requests: its connections (and DNS cache) are reused,
    // which saves the TCP and TLS handshakes on our clock. The options are set every time.
    if (curl)
    {
        curl_easy_reset(curl);
    }
    else if (!(curl = curl_easy_init()))
    {
        report(uri, "failed to initialize cURL");
        return CURLE_FAILED_INIT;
    }

    curl_slist* headers = nullptr;
    for (const auto& header : headers_)
    {
        headers = curl_slist_append(headers, header.c_str());
    }

    const long timeout = get_timeout();

    curl_easy_setopt(curl, CURLOPT_URL, uri.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    // Compressed answers if cURL supports them ("": all the encodings it knows)
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    // Hosts move (e.g. explorer.lichess.ovh -> explorer.lichess.org): follow the
    // redirects. cURL does not forward the Authorization header to another host.
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    // Never block the engine for longer than the configured timeout. NOSIGNAL is
    // required for the timeouts to be thread safe.
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, timeout);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout);

    const CURLcode res = curl_easy_perform(curl);

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpStatus);

    // The list must outlive the transfer only: the next request sets its own (after a reset)
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, nullptr);
    curl_slist_free_all(headers);

    return res;
}

bool BaseLivebook::fetch(const std::string&              uri,
                         const std::vector<std::string>& headers_,
                         const bool                      partial_ok_,
                         const std::string&              auth_hint_) {
    if (is_suspended())
    {
        return false;
    }

    if (const CURLcode res = do_request(uri, headers_); res != CURLE_OK)
    {
        // A streamed answer cut by our timeout: what arrived so far is still usable
        if (partial_ok_ && res == CURLE_OPERATION_TIMEDOUT && httpStatus == 200)
        {
            return true;
        }

        report(uri, std::string("request failed: ") + curl_easy_strerror(res) + ". Paused for "
                      + std::to_string(RETRY_DELAY.count()) + "s");
        suspend_for(RETRY_DELAY);
        return false;
    }

    if (httpStatus >= 200 && httpStatus < 300)
    {
        return true;
    }

    const std::string status = "HTTP " + std::to_string(httpStatus);

    if (httpStatus == 401 || httpStatus == 403)
    {
        report(uri, "refused the request (" + status + ")" + auth_hint_
                      + ". Disabled until the LiveBook options are changed");
        suspend_until_reconfigured();
    }
    else if (httpStatus == 429)
    {
        report(uri, "rate limit reached (" + status + "). Paused for "
                      + std::to_string(RATE_LIMIT_DELAY.count()) + "s");
        suspend_for(RATE_LIMIT_DELAY);
    }
    else if (httpStatus >= 500)
    {
        report(uri, "server error (" + status + "). Paused for "
                      + std::to_string(RETRY_DELAY.count()) + "s");
        suspend_for(RETRY_DELAY);
    }
    else
    {
        report(uri, "unexpected answer (" + status + ")");
    }

    return false;
}

void BaseLivebook::report(const std::string& uri, const std::string& message) {
    sync_cout << "info string LiveBook " << endpoint_of(uri) << ": " << message << sync_endl;
}

bool BaseLivebook::is_suspended() const {
    return std::chrono::steady_clock::now() < suspendedUntil;
}

void BaseLivebook::suspend_for(const std::chrono::seconds duration_) {
    suspendedUntil = std::chrono::steady_clock::now() + duration_;
}

void BaseLivebook::suspend_until_reconfigured() {
    suspendedUntil = std::chrono::steady_clock::time_point::max();
}
#endif
