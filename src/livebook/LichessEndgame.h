#ifndef LICHESS_ENDGAME_H
#define LICHESS_ENDGAME_H
#ifdef USE_LIVEBOOK
    #include "LichessLivebook.h"

namespace ShashChess::Livebook {
class LichessEndgame final: public LichessLivebook {
   public:
    std::string parse_uci(const nlohmann::json& move) override;
    Analysis*   parse_analysis(const nlohmann::json& move) override;
    std::string format_url(const Position& position) override;
};
}


#endif  //LICHESS_ENDGAME_H
#endif
