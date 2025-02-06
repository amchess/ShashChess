#ifndef SHASHIN_MANAGER_H
#define SHASHIN_MANAGER_H
#include "../position.h"
#include "../types.h"
#include "../movegen.h"
#include "../evaluate.h"
#include <unordered_map>
#include <array>
#include "../wdl/win_probability.h"
#include "shashin_types.h"
#include "shashin_helper.h"
namespace Alexander {
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
    explicit ShashinManager();
    // Manage state
    void             updateShashinValues(Value score, const Position& rootPos, int depth);
    ShashinPosition  getShashinRange(Value value, const Position& rootPos);
    RootShashinState getState() const;
    void             setDepth(int depth);
    int              getDepth() const;

    ShashinPosition
    getInitialShashinRange(Position& rootPos, Value staticValue, const ShashinConfig& config);
    // static methods
    static void  computeRootState(const Position& rootPos, RootShashinState& rootShashinState);
    static bool  isPawnNearPromotion(const Position& rootPos);
    static Value static_value(Position& rootPos, Search::Stack* ss);
    void initShashinValues(Position& rootPos, Search::Stack* ss, const ShashinConfig& config);
    //inline methods
    inline bool useMoveGenAndStep17CrystalLogic() const {

        if (isStrategical() && state.highMaterial && state.legalMoveCount >= 30)
        {
            return true;
        }
        // Tal logic: attacking with low complexity and king danger
        if (isTacticalAttacking() && state.legalMoveCount < 30 && state.kingDanger)
        {
            return true;
        }
        // Default: no Crystal logic
        return false;
    }
    inline bool isComplexPosition() const {
        size_t legalMoveCount     = state.legalMoveCount;
        bool   highMaterial       = state.highMaterial;
        bool   kingDanger         = state.kingDanger;
        bool   pawnsNearPromotion = state.pawnsNearPromotion;
        return isComplexPosition(legalMoveCount, highMaterial, kingDanger, pawnsNearPromotion);
    }
    inline static bool isComplexPosition(size_t legalMoveCount,
                                         bool   highMaterial,
                                         bool   kingDanger,
                                         bool   pawnsNearPromotion) {
        return (legalMoveCount >= 25 && highMaterial) || kingDanger || pawnsNearPromotion;
    }
    inline bool useEarlyPruning() const {
        // No pruning beyond depth 8
        if (state.currentDepth > 6)
            return false;
        // Pruning allowed in simple strategic positions at shallow depths
        if (isStrategical() && !isComplexPosition() && state.legalMoveCount < 20
            && state.currentDepth <= 6)
            return true;
        // Pruning allowed in simple tactical positions with low activity at very shallow depths
        if (isTactical() && state.legalMoveCount < 10 && state.currentDepth <= 4)
            return true;
        return false;  // Default: no pruning
    }
    inline bool isLowActivity() const { return state.legalMoveCount < 20; }
    // Attack degrees
    inline bool isTacticalInitiative() const {
        return isInRanges(state.currentRange, ShashinPosition::LOW_TAL);
    }
    inline bool isTacticalAttacking() const {

        return isInRanges(state.currentRange, ShashinPosition::MIDDLE_LOW_TAL,
                          ShashinPosition::MIDDLE_TAL);
    }
    inline bool isTacticalAggressive() const {
        return isInRanges(state.currentRange, ShashinPosition::MIDDLE_HIGH_TAL,
                          ShashinPosition::HIGH_TAL);
    }
    // Defense degrees
    inline bool isTacticalReactive() const {

        return isInRanges(state.currentRange, ShashinPosition::LOW_PETROSIAN);
    }
    inline bool isTacticalDefensive() const {

        return isInRanges(state.currentRange, ShashinPosition::MIDDLE_LOW_PETROSIAN,
                          ShashinPosition::MIDDLE_PETROSIAN);
    }
    inline bool isTacticalPassive() const {
        return isInRanges(state.currentRange, ShashinPosition::MIDDLE_HIGH_PETROSIAN,
                          ShashinPosition::HIGH_PETROSIAN);
    }
    // Utility methods
    inline bool isInRange(ShashinPosition position) const { return state.currentRange == position; }
    // Check for all Petrosian ranges
    inline bool isPetrosian() const {
        return isTacticalReactive() || isTacticalDefensive() || isTacticalPassive();
    }
    inline bool isTal() const {
        return isTacticalInitiative() || isTacticalAttacking() || isTacticalAggressive();
    }
    inline bool isHighPetrosian() const { return isInRange(ShashinPosition::HIGH_PETROSIAN); }
    inline bool isMiddleHighPetrosian() const {
        return isInRange(ShashinPosition::MIDDLE_HIGH_PETROSIAN);
    }
    inline bool isMiddlePetrosian() const { return isInRange(ShashinPosition::MIDDLE_PETROSIAN); }
    inline bool isMiddleLowPetrosian() const {
        return isInRange(ShashinPosition::MIDDLE_LOW_PETROSIAN);
    }
    inline bool isLowPetrosian() const { return isInRange(ShashinPosition::LOW_PETROSIAN); }
    inline bool isCapablancaPetrosian() const {
        return isInRange(ShashinPosition::CAPABLANCA_PETROSIAN);
    }
    inline bool isCapablanca() const { return isInRange(ShashinPosition::CAPABLANCA); }
    inline bool isCapablancaTal() const { return isInRange(ShashinPosition::CAPABLANCA_TAL); }
    inline bool isLowTal() const { return isInRange(ShashinPosition::LOW_TAL); }
    inline bool isMiddleLowTal() const { return isInRange(ShashinPosition::MIDDLE_LOW_TAL); }

    inline bool isMiddleTal() const { return isInRange(ShashinPosition::MIDDLE_TAL); }
    inline bool isMiddleHighTal() const { return isInRange(ShashinPosition::MIDDLE_HIGH_TAL); }
    inline bool isHighTal() const { return isInRange(ShashinPosition::HIGH_TAL); }
    //isTill begin
    // Check if the currentRange is up to (but not including) High zones
    inline bool isTillHigh() const {

        return !isInRanges(state.currentRange, ShashinPosition::HIGH_PETROSIAN,
                           ShashinPosition::HIGH_TAL);
    }

    // Check if the currentRange is up to (but not including) Middle-High zones
    inline bool isTillMiddleHigh() const {

        return isTillHigh()
            && !isInRanges(state.currentRange, ShashinPosition::MIDDLE_HIGH_PETROSIAN,
                           ShashinPosition::MIDDLE_HIGH_TAL);
    }
    // Check if the currentRange is up to (but not including) Middle zones
    inline bool isTillMiddle() const {

        return isTillMiddleHigh()
            && !isInRanges(state.currentRange, ShashinPosition::MIDDLE_PETROSIAN,
                           ShashinPosition::MIDDLE_TAL);
    }
    // Check if the currentRange is up to (but not including) Middle-Low zones
    inline bool isTillMiddleLow() const {

        return isTillMiddle()
            && !isInRanges(state.currentRange, ShashinPosition::MIDDLE_LOW_PETROSIAN,
                           ShashinPosition::MIDDLE_LOW_TAL);
    }
    // Check if the currentRange is up to (but not including) Low zones
    inline bool isTillLow() const {

        return isTillMiddleLow()
            && !isInRanges(state.currentRange, ShashinPosition::LOW_PETROSIAN,
                           ShashinPosition::LOW_TAL);
    }
    //isTill end
    // Strategic zones: Only Capablanca is purely strategic
    inline bool isStrategical() const {
        return ((state.currentRange == ShashinPosition::CAPABLANCA) && (!state.kingDanger));
    }
    // Tactical zones
    inline bool isTactical() const { return isTal() || isPetrosian() || state.kingDanger; }
    inline bool useCrystalFutilityPruningParent() const {
        return isTalTacticalHighMiddle() && isSimpleIntermediate() && !state.kingDanger
            && !isHighPieceDensity()
            && state.currentDepth < 12;  // Applica il pruning solo a profondità più basse
    }
    inline bool useCrystalFutilityPruningChild() const {
        return isTacticalAggressive() || (isTacticalAttacking() && state.legalMoveCount >= 20)
            || (isTacticalReactive() && (state.legalMoveCount < 20 || state.highMaterial))
            || (isStrategical() && state.legalMoveCount >= 30);
    }
    inline bool useCrystalNullMoveSearch() const {

        return isHighTal() || (isTacticalAggressive() && state.highMaterial)
            || (isTacticalPassive() && state.legalMoveCount < 15);
    }
    // Simple intermediate positions: within bounds and low move count
    inline bool isSimpleIntermediate() const {
        return isTillMiddle() && state.legalMoveCount <= 20 && !state.kingDanger
            && !isHighPieceDensity() && state.currentDepth < 8;
    }
    //for mcts begin
    inline bool isMCTSApplicableByValue() const {
        // MCTS is applicable in specific Petrosian zones
        return isHighPetrosian() || isMiddleHighPetrosian() || isMiddlePetrosian();
    }
    inline bool isMCTSExplorationApplicable() const {

        size_t legalMoveCount = state.legalMoveCount;
        return ((isMiddleHighTal() || isHighTal()) && legalMoveCount >= 30) || isCapablanca()
            || isPetrosian();
    }
    inline bool isHighPieceDensity() const {
        const int pieceCount = state.allPiecesCount;
        return pieceCount > 14;
    }
    inline bool isHighPieceDensityCapablancaPosition() const {
        return isCapablanca() && isHighPieceDensity();
    }
    //for mcts end
    // Determines whether to allow Crystal's ProbCut logic (~4 Elo improvement)
    // This logic prunes the search tree earlier when the position is non-critical.
    inline bool allowCrystalProbCut() const {

        return (isTalTacticalHighMiddle() && state.legalMoveCount < 30)
            || (isStrategical() && state.highMaterial);
    }
    inline bool isTacticalReduction() const {

        return (isTacticalAggressive() && state.highMaterial)
            || (isTacticalAttacking() && !isHighPieceDensity());
    }
    // Determines whether to skip Step 10: Internal Iterative Reductions (~9 Elo)
    // This method adjusts the logic based on Shashin's position classification
    // and the complexity of the current position.
    inline bool avoidStep10() const {

        return (isTacticalPassive() && state.legalMoveCount < 20)
            ||                     // Skip for passive zones with low activity
               isStrategical() ||  // Skip for strategic zones
               (isTacticalAggressive()
                && state.kingDanger);  // Skip for aggressive zones with king danger
    }
    inline bool isTalTacticalHighMiddle() const { return isHighTal() || isMiddleHighTal(); }

   private:
    RootShashinState state;
};
}  // namespace Alexander
#endif  // SHASHIN_MANAGER_H