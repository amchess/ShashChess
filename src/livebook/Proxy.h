#ifndef PROXY_H
#define PROXY_H
#ifdef USE_LIVEBOOK

    #include "ChessDb.h"

namespace ShashChess::Livebook {
class Proxy final: public ChessDb {
   public:
    explicit Proxy(const std::string& endpoint);
    ~Proxy() override = default;
};
}  // namespace ShashChess::Livebook;


#endif
#endif  //PROXY_H
