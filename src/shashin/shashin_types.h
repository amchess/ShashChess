#ifndef SHASHIN_TYPES_H
#define SHASHIN_TYPES_H

#include <cstdint>

namespace ShashChess {

enum class ShashinPosition : int16_t {
    HIGH_PETROSIAN = 0x0001,
    MIDDLE_PETROSIAN = 0x0002,
    LOW_PETROSIAN = 0x0004,
    CAPABLANCA = 0x0008,
    LOW_TAL = 0x0010,
    MIDDLE_TAL = 0x0020,
    HIGH_TAL = 0x0040,
    MIDDLE_HIGH_PETROSIAN = MIDDLE_PETROSIAN | HIGH_PETROSIAN, // 0x0003
    MIDDLE_LOW_PETROSIAN = MIDDLE_PETROSIAN | LOW_PETROSIAN,   // 0x0006
    MIDDLE_HIGH_TAL = MIDDLE_TAL | HIGH_TAL,                   // 0x0060
    MIDDLE_LOW_TAL = MIDDLE_TAL | LOW_TAL,                     // 0x0030
    CAPABLANCA_PETROSIAN = CAPABLANCA | LOW_PETROSIAN,         // 0x000C
    CAPABLANCA_TAL = CAPABLANCA | LOW_TAL,                     // 0x0018
    TAL_CAPABLANCA_PETROSIAN = LOW_TAL | MIDDLE_TAL | HIGH_TAL |
    CAPABLANCA | LOW_PETROSIAN | MIDDLE_PETROSIAN | HIGH_PETROSIAN // 0x007F
};

enum WinProbabilityRange {
    HIGH_PETROSIAN_MAX = 5,            // High Petrosian: [0, 10]
    MIDDLE_HIGH_PETROSIAN_MAX = 10,     // Middle-High Petrosian: [11, 15]
    MIDDLE_PETROSIAN_MAX = 15,          // Middle Petrosian: [16, 20]
    MIDDLE_LOW_PETROSIAN_MAX = 20,      // Middle-Low Petrosian: [21, 24]
    LOW_PETROSIAN_MAX = 24,             // Low Petrosian: [25, 33]
    CAPABLANCA_PETROSIAN_MAX = 49,      // Chaos: Capablanca-Petrosian: [34, 49]
    CAPABLANCA_MAX = 50,                // Capablanca: [50, 50]
    CAPABLANCA_TAL_MAX = 75,            // Chaos: Capablanca-Tal: [51, 66]
    LOW_TAL_MAX = 79,                   // Low Tal: [67, 75]
    MIDDLE_LOW_TAL_MAX = 84,            // Middle-Low Tal: [76, 79]
    MIDDLE_TAL_MAX = 89,                // Middle Tal: [80, 84]
    MIDDLE_HIGH_TAL_MAX = 94,           // Middle-High Tal: [85, 89]
    HIGH_TAL_MAX = 100                  // High Tal: [90, 100]
};

struct ShashinConfig {
    bool highTal = false;
    bool middleTal = false;
    bool lowTal = false;
    bool capablanca = false;
    bool highPetrosian = false;
    bool middlePetrosian = false;
    bool lowPetrosian = false;
};

struct RootShashinState {
    int currentDepth = 0;
    ShashinPosition currentRange = ShashinPosition::CAPABLANCA;
    size_t legalMoveCount = 0;
    bool highMaterial = false;
    bool kingDanger = false;
    int allPiecesCount = 0;
};

} // namespace ShashChess

#endif // SHASHIN_TYPES_H
