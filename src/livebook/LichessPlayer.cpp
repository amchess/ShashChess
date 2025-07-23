#ifdef USE_LIVEBOOK
    #include "LichessPlayer.h"

using namespace ShashChess::Livebook;

LichessPlayer::LichessPlayer(const std::string& player_, std::string color_) :
    LichessOpening("https://explorer.lichess.ovh/player?player=" + player_ + "&"),
    player(player_),
    color(std::move(color_)) {}

std::string LichessPlayer::format_url(const Position& position_) {
    auto fen = position_.fen();
    std::replace(fen.begin(), fen.end(), ' ', '_');

    return endpoint + "color="
         + (color == "both" ? position_.side_to_move() == WHITE ? "white" : "black" : color)
         + "&fen=" + fen;
}
#endif
