#ifndef LICHESS_OPENING_H
#define LICHESS_OPENING_H
#ifdef USE_LIVEBOOK

    #include "LichessLivebook.h"

namespace ShashChess::Livebook {
class LichessOpening: public LichessLivebook {
   public:
    explicit LichessOpening(std::string endpoint);
    ~LichessOpening() override = default;

   protected:
    std::string endpoint;

    std::string parse_uci(const nlohmann::json& move) override;
    Analysis*   parse_analysis(const nlohmann::json& move) override;
    std::string format_url(const Position& position) override;
};
}

#endif  //LICHESS_OPENING_H
#endif
