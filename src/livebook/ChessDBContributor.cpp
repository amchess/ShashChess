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
