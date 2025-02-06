#ifndef WIN_PROBABILITY_H
#define WIN_PROBABILITY_H
#include <cstdint>
#include "../position.h"
#include "../types.h"

namespace Alexander {
namespace WDLModel {
const uint8_t MAX_WIN_PROBABILITY = 100;
struct WDL {       // Structure to represent Win-Draw-Loss in centipawns
    uint8_t win;   // Win percentage (0-100)
    uint8_t draw;  // Draw percentage (0-100)
    uint8_t loss;  // Loss percentage (0-100)
};

void    init();  //inizializes the wdls array
WDL     get_wdl_by_material(const Value value,
                            const int   materialClamp);     // Returns the wdl, given the material
WDL     get_wdl(const Value value, const Position& pos);  // Returns the WDL based on the position
uint8_t get_win_probability_by_material(
  const Value value, const int materialClamp);  // Returns the win probability using the WDL
uint8_t get_win_probability(const Value     value,
                            const Position& pos);  // Returns the win probability given the position
uint8_t
get_win_probability(const Value value,
                    const int plies);  // Returns the win probability by using the half moves number
std::string
    wdl(Value           v,
        const Position& pos);  // Generates a WDL string (in thousandths for compatibility elsewhere)
WDL get_precomputed_wdl(int valueClamp,
                        int materialClamp);  // Returns the WDL precomputed by value and material
struct WinRateParams {
    double a;
    double b;
};
WinRateParams win_rate_params(const Position& pos);
WinRateParams win_rate_params(int materialClamp);
int           win_rate_model(Value           v,
                             const Position& pos);  //usato in  std::string wdl(Value v, const Position& pos);
}
}
#endif  //WIN_PROBABILITY_H
