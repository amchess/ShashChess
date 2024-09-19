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
