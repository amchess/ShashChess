#ifdef USE_LIVEBOOK
    #include "Proxy.h"

using namespace ShashChess::Livebook;

    #define DEFAULT_ACTION Action::QUERY

Proxy::Proxy(const std::string& endpoint_) :
    ChessDb(endpoint_, DEFAULT_ACTION) {}

    #undef DEFAULT_ACTION
#endif
