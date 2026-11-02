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
    #include "ChessDBContributor.h"

    #include "../uci.h"

using namespace ShashChess::Livebook;


ChessDBContributor::ChessDBContributor() = default;

void ChessDBContributor::contribute(const Position& position, const Move move) {
    const auto        escaped_fen_str = curl_easy_escape(curl, position.fen().c_str(), 0);
    const std::string escaped_fen(escaped_fen_str);
    curl_free(escaped_fen_str);

    const auto url = "https://www.chessdb.cn/cdb.php?action=store&board=" + escaped_fen
                   + "&move=move:" + UCIEngine::move(move, position.is_chess960());

    if (const auto ret = do_request(url); ret != CURLE_OK)
    {
        std::cerr << "Failed to contribute to ChessDB: " << curl_easy_strerror(ret) << std::endl;
    }
}

static size_t curl_write(void* contents, const size_t size, const size_t nmemb, void* userp) {
    const size_t new_size = size * nmemb;
    const auto   buffer   = static_cast<std::string*>(userp);

    buffer->append(static_cast<char*>(contents), new_size);

    return new_size;
}

// Perform an HTTP request to the given URI and store the response in readBuffer
CURLcode ChessDBContributor::do_request(const std::string& uri) {
    readBuffer.clear();

    CURL* local_curl = curl_easy_init();
    if (!local_curl)
    {
        std::cerr << "Failed to initialize cURL" << std::endl;
        return CURLE_FAILED_INIT;
    }

    curl_easy_setopt(local_curl, CURLOPT_URL, uri.c_str());
    curl_easy_setopt(local_curl, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(local_curl, CURLOPT_WRITEDATA, &readBuffer);
    curl_easy_setopt(local_curl, CURLOPT_SSL_VERIFYHOST, 2L);

    const CURLcode res = curl_easy_perform(local_curl);
    if (res != CURLE_OK)
    {
        std::cerr << "curl_easy_perform() failed: " << curl_easy_strerror(res) << std::endl;
    }

    curl_easy_cleanup(local_curl);

    return res;
}
#endif
