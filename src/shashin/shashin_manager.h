#ifndef SHASHIN_MANAGER_H
#define SHASHIN_MANAGER_H

#include "shashin_position.h"
#include "shashin_params.h"
#include "../types.h"
#include "../movegen.h"
#include "../movepick.h"
#include "../evaluate.h"
#include <array>
#include "../wdl/win_probability.h"
#include "shashin_types.h"
#include "shashin_helper.h"

namespace ShashChess {
namespace Eval {
namespace NNUE {
struct Networks;
struct AccumulatorCaches;
}
}

namespace Search {
class Worker;
struct Stack;
}

class ShashinManager {
   public:
    explicit ShashinManager(const ShashinParams& params_ = {});

    // Core interface
    ShashinPosition getResilientShashinRange(ShashinPosition range, int depth);
    void updateRootShashinState(Value score, const Position& rootPos, int depth, int rootDepth);
    ShashinPosition         getShashinRange(Value value, const Position& rootPos);
    const RootShashinState& getState() const { return state; }
    void                    setDepth(int depth) { state.dynamicBase.currentDepth = depth; }
    int                     getDepth() const { return state.dynamicBase.currentDepth; }

    ShashinPosition
    getInitialShashinRange(Position& rootPos, Value staticValue, const ShashinConfig& config);

    // Position analysis
    bool isFortress(const Position& pos);
    void setStaticState(const Position& rootPos);
    bool isSacrificialPosition(const Position& rootPos);
    bool isPawnNearPromotion(const Position& rootPos);

    // Evaluation
    Value static_value(const Eval::NNUE::Networks&   networks,
                       Eval::NNUE::AccumulatorStack& accumulators,
                       Eval::NNUE::AccumulatorCaches refreshTable,
                       Position&                     rootPos,
                       Search::Stack*                ss,
                       Value                         optimism);

    // Initialization
    void initDynamicRootShashinState(const Eval::NNUE::Networks&   networks,
                                     Eval::NNUE::AccumulatorStack& accumulators,
                                     Eval::NNUE::AccumulatorCaches refreshTable,
                                     Position&                     rootPos,
                                     Search::Stack*                ss,
                                     Value                         optimism,
                                     const ShashinConfig&          config,
                                     Depth                         rootDepth);
    void setDynamicBaseState(Value score, const Position& rootPos, int depth, int rootDepth);
    void setDynamicDerivedState();
    void initDynamicBaseState(Value                currentValue,
                              Position&            rootPos,
                              const ShashinConfig& config,
                              int                  rootDepth);

    // Feature flags
    bool useMoveGenCrystalLogic() const;
    bool useStep17CrystalLogic() const;
    bool isComplexPosition() const;
    bool isLowActivity() const { return state.staticState.legalMoveCount < 20; }
    bool isTacticalInitiative() const;
    bool isTacticalReactive() const;
    bool isTacticalDefensive() const;

    // Style checks
    bool isShashinStyle(std::initializer_list<ShashinPosition> positions) const;
    bool isTillCategory(ShashinPosition lowerBound, ShashinPosition upperBound) const;

    // Search optimizations
    bool allowCrystalProbCut() const;
    bool allowStockfishProbCut() const;
    bool useNullMoveByShashinForStockfish() const;
    bool isSimpleIntermediate() const;
    bool isMCTSApplicableByValue() const;
    bool isMCTSExplorationApplicable() const;
    bool isTal() const;
    bool isCapablanca() const;
    bool isHighPieceDensityCapablancaPosition() const;
    bool isTacticalReduction() const;
    bool avoidStep10() const;
    bool isTalTacticalHighMiddle() const;
    bool useCrystalFutility() const;
    bool isStmKingExposed() const { return state.staticState.stmKingSafetyScore < -30; }
    bool isOpponentKingExposed() const { return state.staticState.opponentKingSafetyScore < -30; }

    // Inline methods for performance
    template<ShashinPosition P>
    inline __attribute__((always_inline)) bool inRange() const {
        return state.dynamicBase.currentRange == P;
    }

    inline __attribute__((always_inline)) bool isStrategical() const {
        return inRange<ShashinPosition::CAPABLANCA>() && !state.staticState.kingDanger;
    }

    inline __attribute__((always_inline)) bool isPassive() const {
        return anyOf(state.dynamicBase.currentRange, ShashinPosition::MIDDLE_HIGH_PETROSIAN,
                     ShashinPosition::HIGH_PETROSIAN);
    }

    inline __attribute__((always_inline)) bool isTactical() const {
        return isTal() || isPetrosian() || state.staticState.kingDanger;
    }

    inline __attribute__((always_inline)) bool isPetrosian() const {
        return isShashinStyle({ShashinPosition::LOW_PETROSIAN, ShashinPosition::MIDDLE_PETROSIAN,
                               ShashinPosition::HIGH_PETROSIAN});
    }

    inline __attribute__((always_inline)) bool isHighPieceDensity() const {
        return state.staticState.allPiecesCount > 14;
    }

    inline __attribute__((always_inline)) bool isAggressive() const {
        return anyOf(state.dynamicBase.currentRange, ShashinPosition::MIDDLE_LOW_TAL,
                     ShashinPosition::MIDDLE_TAL);
    }

    inline __attribute__((always_inline)) bool isInRange(ShashinPosition position) const {
        return state.dynamicBase.currentRange == position;
    }
    // Helper functions for styles intensity (inline for performance
    static inline __attribute__((always_inline)) double getTalIntensity(ShashinPosition style) {
        switch (style)
        {
        case ShashinPosition::HIGH_TAL :
            return 1.00;  // 100% reduction
        case ShashinPosition::MIDDLE_HIGH_TAL :
            return 0.85;  // 85%
        case ShashinPosition::MIDDLE_TAL :
            return 0.73;  // 73% (40/55 ≈ 0.727, 55/75 ≈ 0.733)
        case ShashinPosition::MIDDLE_LOW_TAL :
            return 0.60;  // 60%
        case ShashinPosition::LOW_TAL :
            return 0.45;  // 45%
        default :
            return 0.0;
        }
    }

    static inline __attribute__((always_inline)) double
    getPetrosianIntensity(ShashinPosition style) {
        switch (style)
        {
        case ShashinPosition::HIGH_PETROSIAN :
            return 0.55;  // 55% (25/55 ≈ 0.45, 35/75 ≈ 0.47 → media 0.46 → 1-0.46=0.54)
        case ShashinPosition::MIDDLE_HIGH_PETROSIAN :
            return 0.50;  // 50%
        case ShashinPosition::MIDDLE_PETROSIAN :
            return 0.40;  // 40%
        case ShashinPosition::MIDDLE_LOW_PETROSIAN :
            return 0.30;  // 30%
        case ShashinPosition::LOW_PETROSIAN :
            return 0.20;  // 20%
        default :
            return 0.0;
        }
    }

   private:
    ShashinParams params;
    struct PositionCache {
        uint64_t posHash       = 0;
        bool     fortress      = false;
        bool     pawnNearPromo = false;
        bool     sacrificial   = false;
    };
    RootShashinState state;
    PositionCache    positionCache;
    // Helper to calculate the position hash
    uint64_t computePositionHash(const Position& pos) {
        return Shashin::shashin_position_hash(pos);
    }
    // Helper to invalidate caches when position changes
    void invalidateCaches() {
        positionCache.posHash = 0;  // It invalidates all at the position change
    }
};

}  // namespace ShashChess

#endif  // SHASHIN_MANAGER_H