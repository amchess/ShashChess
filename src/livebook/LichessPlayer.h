#ifndef LICHESS_PLAYER_H
#define LICHESS_PLAYER_H

#ifdef USE_LIVEBOOK

    #include "LichessOpening.h"


namespace ShashChess::Livebook {
class LichessPlayer final: public LichessOpening {
   public:
    LichessPlayer(const std::string& player_, std::string color_);
    ~LichessPlayer() override = default;

   protected:
    std::string player;
    std::string color;

    std::string format_url(const Position& position_) override;
};
}

#endif
#endif  //LICHESS_PLAYER_H