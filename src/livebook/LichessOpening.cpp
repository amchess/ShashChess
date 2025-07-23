#ifdef USE_LIVEBOOK
    #include "LichessOpening.h"
    #include "json/json.hpp"

using namespace ShashChess::Livebook;

LichessOpening::LichessOpening(std::string endpoint_) :
    endpoint(std::move(endpoint_)) {}

std::string LichessOpening::parse_uci(const nlohmann::json& move_) {
    if (!move_.contains("uci") || !move_["uci"].is_string())
    {
        return "";
    }

    return move_["uci"].get<std::string>();
}

Analysis* LichessOpening::parse_analysis(const nlohmann::json& move_) {
    const auto wins = move_.contains("white") ? move_["white"].get<uint64_t>() : 0;

    const auto draws = move_.contains("draws") ? move_["draws"].get<uint64_t>() : 0;

    const auto losses = move_.contains("black") ? move_["black"].get<uint64_t>() : 0;

    const auto wdl      = new Wdl(wins, draws, losses);
    const auto analysis = new Analysis(wdl);

    return analysis;
}

std::string LichessOpening::format_url(const Position& position_) {
    std::string fen_encoded = position_.fen();
    std::replace(fen_encoded.begin(), fen_encoded.end(), ' ', '_');
    const std::string full_uri = this->endpoint + "fen=" + fen_encoded;

    return full_uri;
}
#endif
