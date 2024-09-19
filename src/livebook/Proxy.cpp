#ifdef USE_LIVEBOOK
    #include "Proxy.h"

using namespace ShashChess::Livebook;

    #define DEFAULT_ACTION Action::QUERY

Proxy::Proxy(const std::string& endpoint) :
    ChessDb(endpoint, DEFAULT_ACTION) {}

    #undef DEFAULT_ACTION
#endif
