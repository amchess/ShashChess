#ifndef WIN_PROBABILITY_H
#define WIN_PROBABILITY_H
#include <cstdint>
#include "uci.h"

namespace ShashChess::WDLModel {

void    init();
uint8_t get_win_probability_by_material(const Value value, const int materialClamp);
uint8_t get_win_probability(Value value, const Position& pos);
uint8_t get_win_probability(Value value, int plies);

}
#endif  //WIN_PROBABILITY_H
