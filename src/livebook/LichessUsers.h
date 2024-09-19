#ifndef LICHESS_USERS_H
#define LICHESS_USERS_H
#ifdef USE_LIVEBOOK
    #include "LichessOpening.h"

namespace ShashChess::Livebook {
class LichessUsers final: public LichessOpening {
   public:
    LichessUsers();
};
};


#endif  //LICHESS_USERS_H
#endif
