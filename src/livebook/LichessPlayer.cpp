#ifdef USE_LIVEBOOK
    #include "LichessPlayer.h"

using namespace ShashChess::Livebook;

LichessPlayer::LichessPlayer(const std::string& player, std::string color) :
    LichessOpening("https://explorer.lichess.ovh/player?player=" + player + "&"),
    player(player),
    color(std::move(color)) {}

std::string LichessPlayer::format_url(const Position& position) {
    auto fen = position.fen();
    std::replace(fen.begin(), fen.end(), ' ', '_');

    return endpoint + "color="
         + (color == "both" ? position.side_to_move() == WHITE ? "white" : "black" : color)
         + "&fen=" + fen;
}
#endif
