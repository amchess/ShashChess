#ifndef SHASHIN_TYPES_H
#define SHASHIN_TYPES_H

#include <cstddef>
#include <cstdint>
#include "../types.h"

namespace ShashChess {

constexpr Depth MIN_DEPTH_FOR_QS_CUTOFF = 6;
constexpr Depth ADJUSTED_QS_DEPTH       = DEPTH_QS + 2;

// Use uint16_t for better bitmask operations
enum class ShashinPosition : uint16_t {
    HIGH_PETROSIAN           = 0x0001,
    MIDDLE_PETROSIAN         = 0x0002,
    LOW_PETROSIAN            = 0x0004,
    MIDDLE_HIGH_PETROSIAN    = 0x0008,
    MIDDLE_LOW_PETROSIAN     = 0x0010,
    CAPABLANCA               = 0x0020,
    LOW_TAL                  = 0x0040,
    MIDDLE_TAL               = 0x0080,
    HIGH_TAL                 = 0x0100,
    MIDDLE_HIGH_TAL          = 0x0200,
    MIDDLE_LOW_TAL           = 0x0400,
    CAPABLANCA_PETROSIAN     = 0x0800,
    CAPABLANCA_TAL           = 0x1000,
    TAL_CAPABLANCA_PETROSIAN = 0x2000
};

// Win probability ranges - made constexpr
constexpr int   HIGH_PETROSIAN_MAX        = 5;
constexpr int   MIDDLE_HIGH_PETROSIAN_MAX = 10;
constexpr int   MIDDLE_PETROSIAN_MAX      = 15;
constexpr int   MIDDLE_LOW_PETROSIAN_MAX  = 20;
constexpr int   LOW_PETROSIAN_MAX         = 24;
constexpr int   CAPABLANCA_PETROSIAN_MAX  = 49;
constexpr int   CAPABLANCA_MAX            = 50;
constexpr int   CAPABLANCA_TAL_MAX        = 75;
constexpr int   LOW_TAL_MAX               = 79;
constexpr int   MIDDLE_LOW_TAL_MAX        = 84;
constexpr int   MIDDLE_TAL_MAX            = 89;
constexpr int   MIDDLE_HIGH_TAL_MAX       = 94;
constexpr int   HIGH_TAL_MAX              = 100;
constexpr Value PawnConversionFactor      = 356;
constexpr Value VALUE_TB_WIN              = 51 * PawnConversionFactor;
constexpr Value VALUE_MAX_EVAL            = VALUE_TB_WIN - 8 * PawnConversionFactor;
// Packed config to save space
struct ShashinConfig {
    uint8_t highTal: 1;
    uint8_t middleTal: 1;
    uint8_t lowTal: 1;
    uint8_t capablanca: 1;
    uint8_t highPetrosian: 1;
    uint8_t middlePetrosian: 1;
    uint8_t lowPetrosian: 1;
    uint8_t reserved: 1;

    ShashinConfig() :
        highTal(0),
        middleTal(0),
        lowTal(0),
        capablanca(0),
        highPetrosian(0),
        middlePetrosian(0),
        lowPetrosian(0),
        reserved(0) {}
};

// Packed structs for better cache utilization
struct DynamicBaseState {
    int             currentDepth = 0;
    int             rootDepth    = 0;
    ShashinPosition currentRange = ShashinPosition::CAPABLANCA;
};

struct DynamicDerivedState {
    bool isAggressive;
    bool isStrategical;
    bool isTacticalReactive;
    bool isHighTal;
    bool useMoveGenCrystalLogic;

    DynamicDerivedState() :
        isAggressive(false),
        isStrategical(false),
        isTacticalReactive(false),
        isHighTal(false),
        useMoveGenCrystalLogic(false) {}
};

struct StaticState {
    uint8_t legalMoveCount;
    bool    isSacrificial;
    bool    stmKingExposed;
    bool    opponentKingExposed;
    bool    highMaterial;
    bool    kingDanger;
    bool    stmKingDanger;
    bool    pawnsNearPromotion;
    uint8_t allPiecesCount;
    int16_t stmKingSafetyScore;
    int16_t opponentKingSafetyScore;

    StaticState() :
        legalMoveCount(0),
        isSacrificial(false),
        stmKingExposed(false),
        opponentKingExposed(false),
        highMaterial(false),
        kingDanger(false),
        stmKingDanger(false),
        pawnsNearPromotion(false),
        allPiecesCount(0),
        stmKingSafetyScore(0),
        opponentKingSafetyScore(0) {}
};

// Ensure proper alignment for performance
struct alignas(64) RootShashinState {
    DynamicBaseState    dynamicBase;     // 8 bytes
    DynamicDerivedState dynamicDerived;  // 4 bytes
    StaticState         staticState;     // 12 bytes
    uint8_t             padding[40];     // Padding calcolato

    RootShashinState() :
        dynamicBase(),
        dynamicDerived(),
        staticState() {
        std::memset(padding, 0, sizeof(padding));
    }
};
}  // namespace ShashChess

#endif  // SHASHIN_TYPES_H