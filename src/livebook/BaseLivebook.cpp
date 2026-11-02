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
    #include <iostream>
using namespace ShashChess::Livebook;

static size_t curl_write(void* contents, const size_t size, const size_t nmemb, void* userp) {
    const size_t new_size = size * nmemb;
    const auto   buffer   = static_cast<std::string*>(userp);

    buffer->append(static_cast<char*>(contents), new_size);

    return new_size;
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

// Perform an HTTP request to the given URI and store the response in readBuffer
CURLcode BaseLivebook::do_request(const std::string& uri) {
    readBuffer.clear();

    CURL* curl = curl_easy_init();
    if (!curl)
    {
        std::cerr << "Failed to initialize cURL" << std::endl;
        return CURLE_FAILED_INIT;
    }

    curl_easy_setopt(curl, CURLOPT_URL, uri.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    const CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK)
    {
        std::cerr << "curl_easy_perform() failed: " << curl_easy_strerror(res) << std::endl;
    }

    curl_easy_cleanup(curl);

    return res;
}
#endif
