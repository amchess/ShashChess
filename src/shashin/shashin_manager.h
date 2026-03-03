#ifndef SHASHIN_MANAGER_H
#define SHASHIN_MANAGER_H

#include "shashin_position.h"
#include "shashin_params.h"
#include "../types.h"
// Rimosso movegen/movepick/evaluate per pulizia header, ma li teniamo se servono al compilatore
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
struct AccumulatorStack; // Forward dec necessaria
}
}

namespace Search {
class Worker;
struct Stack;
}

// --- FIX 1: DEFINIZIONE MASCHERE PER LA CACHE ---
// Serve per tracciare singolarmente cosa è stato calcolato
enum CacheMask : uint8_t {
    CACHE_SACRIFICIAL = 1,
    CACHE_PAWN_PROMO  = 2,
    CACHE_FORTRESS    = 4
};

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

    // Evaluation - Definita in search.cpp
    Value static_value(const Eval::NNUE::Networks&   networks,
                       Eval::NNUE::AccumulatorStack& accumulators,
                       Eval::NNUE::AccumulatorCaches& refreshTable,
                       Position&                     rootPos,
                       Search::Stack* ss,
                       Value                         optimism);

    // Initialization
    void initDynamicRootShashinState(const Eval::NNUE::Networks&   networks,
                                     Eval::NNUE::AccumulatorStack& accumulators,
                                     Eval::NNUE::AccumulatorCaches& refreshTable,
                                     Position&                     rootPos,
                                     Search::Stack* ss,
                                     Value                         optimism,
                                     const ShashinConfig&          config,
                                     Depth                         rootDepth);
    
    void setDynamicBaseState(Value score, const Position& rootPos, int depth, int rootDepth);
    void setDynamicDerivedState();
    void initDynamicBaseState(Value                        currentValue,
                              Position&                    rootPos,
                              const ShashinConfig& config,
                              int                          rootDepth);

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
    bool isCapablanca() const;
    bool isHighPieceDensityCapablancaPosition() const;
    bool isTacticalReduction() const;
    bool avoidStep10() const;
    bool isTalTacticalHighMiddle() const;
    bool useCrystalFutility() const;
    bool isStmKingExposed() const { return state.staticState.stmKingSafetyScore < -30; }
    bool isOpponentKingExposed() const { return state.staticState.opponentKingSafetyScore < -30; }

    // Inline methods for performance
    
    // REFACTORING: Aggiunto metodo non-template richiesto
    inline __attribute__((always_inline)) bool isInRange(ShashinPosition position) const {
        return state.dynamicBase.currentRange == position;
    }

    template<ShashinPosition P>
    inline __attribute__((always_inline)) bool inRange() const {
        return state.dynamicBase.currentRange == P;
    }

    inline __attribute__((always_inline)) bool isStrategical() const {
        // È strategico solo se è Capablanca E non ci sono pericoli immediati.
        return inRange<ShashinPosition::CAPABLANCA>() 
            && !state.staticState.kingDanger 
            && !state.staticState.pawnsNearPromotion;
    }

    inline __attribute__((always_inline)) bool isPassive() const {
        return anyOf(state.dynamicBase.currentRange, 
                     ShashinPosition::MIDDLE_HIGH_PETROSIAN,
                     ShashinPosition::HIGH_PETROSIAN);
    }

    inline __attribute__((always_inline)) bool isTactical() const {
        return isTal() || isPetrosian() || state.staticState.kingDanger;
    }

    inline __attribute__((always_inline)) bool isPetrosian() const {
        return ShashChess::isInRange(state.dynamicBase.currentRange, 
                                     ShashinPosition::LOW_PETROSIAN, 
                                     ShashinPosition::HIGH_PETROSIAN);
    }
    inline __attribute__((always_inline)) bool isTal() const {
        return ShashChess::isInRange(state.dynamicBase.currentRange, 
                                     ShashinPosition::LOW_TAL, 
                                     ShashinPosition::HIGH_TAL);
    }
    inline __attribute__((always_inline)) bool isHighPieceDensity() const {
        return state.staticState.allPiecesCount > 14;
    }

    inline __attribute__((always_inline)) bool isAggressive() const {
        return anyOf(state.dynamicBase.currentRange, 
                     ShashinPosition::MIDDLE_LOW_TAL,
                     ShashinPosition::MIDDLE_TAL);
    }

    // Helper functions for styles intensity
    static inline __attribute__((always_inline)) double getTalIntensity(ShashinPosition style) {
        switch (style)
        {
        case ShashinPosition::HIGH_TAL : return 1.00;
        case ShashinPosition::MIDDLE_HIGH_TAL : return 0.85;
        case ShashinPosition::MIDDLE_TAL : return 0.73;
        case ShashinPosition::MIDDLE_LOW_TAL : return 0.60;
        case ShashinPosition::LOW_TAL : return 0.45;
        default : return 0.0;
        }
    }

    static inline __attribute__((always_inline)) double
    getPetrosianIntensity(ShashinPosition style) {
        switch (style)
        {
        case ShashinPosition::HIGH_PETROSIAN : return 0.55;
        case ShashinPosition::MIDDLE_HIGH_PETROSIAN : return 0.50;
        case ShashinPosition::MIDDLE_PETROSIAN : return 0.40;
        case ShashinPosition::MIDDLE_LOW_PETROSIAN : return 0.30;
        case ShashinPosition::LOW_PETROSIAN : return 0.20;
        default : return 0.0;
        }
    }

   private:
    ShashinParams params;
    
    // --- FIX 1 (Parte Struct): Struttura aggiornata ---
    struct PositionCache {
        uint64_t posHash       = 0;
        uint8_t  computedMask  = 0; // Maschera per tracciare i singoli calcoli
        bool     fortress      = false;
        bool     pawnNearPromo = false;
        bool     sacrificial   = false;
    };
    
    RootShashinState state;
    PositionCache    positionCache;
    
    uint64_t computePositionHash(const Position& pos);
    
    // --- FIX 2: Reset completo ---
    void invalidateCaches() {
        positionCache.posHash = 0;
        positionCache.computedMask = 0;
        positionCache.fortress = false;
        positionCache.pawnNearPromo = false;
        positionCache.sacrificial = false;
    }
};

}  // namespace ShashChess

#endif  // SHASHIN_MANAGER_H