#ifndef CHESSDBCONTRIBUTOR_H
#define CHESSDBCONTRIBUTOR_H
#ifdef USE_LIVEBOOK

    #include "../position.h"

    #define CURL_STATICLIB
extern "C" {
    #include <curl/curl.h>
}
    #undef min
    #undef max

namespace ShashChess::Livebook {
class ChessDBContributor {
   public:
    ChessDBContributor();
    ~ChessDBContributor() = default;

    void contribute(const Position& position, Move move);

   protected:
    CURL*       curl = nullptr;  // cURL handle
    std::string readBuffer;      // Buffer to store the response data

    CURLcode do_request(const std::string& uri);
};
};


#endif
#endif  //CHESSDBCONTRIBUTOR_H
