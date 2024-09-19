#ifndef JSON_LIVEBOOK_H
#define JSON_LIVEBOOK_H
#ifdef USE_LIVEBOOK

    #include "json/json.hpp"
    #include "BaseLivebook.h"

namespace ShashChess::Livebook {
class LichessLivebook: public BaseLivebook {
   public:
    ~LichessLivebook() override = default;

    std::vector<std::pair<std::string, Analysis>> lookup(const Position& position) override;

   protected:
    virtual std::string format_url(const Position& position)       = 0;
    virtual std::string parse_uci(const nlohmann::json& move)      = 0;
    virtual Analysis*   parse_analysis(const nlohmann::json& move) = 0;
};
}

#endif  //JSON_LIVEBOOK_H
#endif
