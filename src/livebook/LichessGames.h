#ifndef LICHESS_GAMES_H
#define LICHESS_GAMES_H
#ifdef USE_LIVEBOOK
    #include "LichessOpening.h"


namespace ShashChess::Livebook {
class LichessGames final: public LichessOpening {
   public:
    LichessGames();
};
}

#endif  //LICHESS_GAMES_H
#endif
