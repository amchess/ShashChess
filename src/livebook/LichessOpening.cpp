#ifdef USE_LIVEBOOK
    #include "LichessOpening.h"
    #include "json/json.hpp"

using namespace ShashChess::Livebook;

LichessOpening::LichessOpening(std::string endpoint) :
    endpoint(std::move(endpoint)) {}

std::string LichessOpening::parse_uci(const nlohmann::json& move) {
    if (!move.contains("uci") || !move["uci"].is_string())
    {
        return "";
    }

    return move["uci"].get<std::string>();
}

Analysis* LichessOpening::parse_analysis(const nlohmann::json& move) {
    const auto wins = move.contains("white") ? move["white"].get<uint64_t>() : 0;

    const auto draws = move.contains("draws") ? move["draws"].get<uint64_t>() : 0;

    const auto losses = move.contains("black") ? move["black"].get<uint64_t>() : 0;

    const auto wdl      = new Wdl(wins, draws, losses);
    const auto analysis = new Analysis(wdl);

    return analysis;
}

std::string LichessOpening::format_url(const Position& position) {
    std::string fen_encoded = position.fen();
    std::replace(fen_encoded.begin(), fen_encoded.end(), ' ', '_');
    const std::string full_uri = this->endpoint + "fen=" + fen_encoded;

    return full_uri;
}
#endif
