#ifndef SHASHIN_TYPES_H
#define SHASHIN_TYPES_H

#include <cstddef>
#include <cstdint>
#include "../types.h"

namespace ShashChess {

constexpr Depth MIN_DEPTH_FOR_QS_CUTOFF = 6;
constexpr Depth ADJUSTED_QS_DEPTH       = DEPTH_QS + 2;

// REFACTORING: Enum sequenziale ordinato logicamente (Spettro Petrosian -> Capablanca -> Tal)
// Questo permette di usare operatori >= e <= per i range.
enum class ShashinPosition : uint8_t {
    HIGH_PETROSIAN           = 0,
    MIDDLE_HIGH_PETROSIAN    = 1,
    MIDDLE_PETROSIAN         = 2,
    MIDDLE_LOW_PETROSIAN     = 3,
    LOW_PETROSIAN            = 4,
    
    CAPABLANCA_PETROSIAN     = 5,
    CAPABLANCA               = 6,
    CAPABLANCA_TAL           = 7,
    
    LOW_TAL                  = 8,
    MIDDLE_LOW_TAL           = 9,
    MIDDLE_TAL               = 10,
    MIDDLE_HIGH_TAL          = 11,
    HIGH_TAL                 = 12,
    
    TAL_CAPABLANCA_PETROSIAN = 13 // Total Chaos / Undefined
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

struct RootShashinStateBase {
    DynamicBaseState    dynamicBase;
    DynamicDerivedState dynamicDerived;
    StaticState         staticState;
};

constexpr size_t BASE_SIZE = sizeof(RootShashinStateBase);
constexpr size_t TARGET_SIZE = 64;
constexpr size_t PADDING_SIZE = (BASE_SIZE < TARGET_SIZE) ? (TARGET_SIZE - BASE_SIZE) : 0;

// Ensure proper alignment for performance
struct RootShashinState {
    DynamicBaseState    dynamicBase;
    DynamicDerivedState dynamicDerived;
    StaticState         staticState;
    uint8_t             padding[PADDING_SIZE];
    
    RootShashinState() :
        dynamicBase(),
        dynamicDerived(),
        staticState()
    {
        for (size_t i = 0; i < sizeof(padding); ++i) {
            padding[i] = 0;
        }
    }
    
    void clear() {
        dynamicBase = DynamicBaseState();
        dynamicDerived = DynamicDerivedState();
        staticState = StaticState();
        for (size_t i = 0; i < sizeof(padding); ++i) {
            padding[i] = 0;
        }
    }
};

static_assert(sizeof(RootShashinState) == TARGET_SIZE, "RootShashinState must be exactly 64 bytes");

}  // namespace ShashChess

#endif  // SHASHIN_TYPES_H