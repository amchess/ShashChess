#ifndef LICHESS_MASTER_H
#define LICHESS_MASTER_H
#ifdef USE_LIVEBOOK
    #include "LichessOpening.h"


namespace ShashChess::Livebook {
class LichessMaster final: public LichessOpening {
   public:
    LichessMaster();
};
}


#endif  //LICHESS_MASTER_H
#endif
