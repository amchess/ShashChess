#include "shashin_manager.h"
#include "../nnue/network.h"
#include "../nnue/nnue_accumulator.h"
#include "../search.h"
#include <algorithm>

namespace ShashChess {
using namespace ShashChess::Shashin;
ShashinManager::ShashinManager(const ShashinParams& params_) :
    params(params_) {}


__attribute__((hot)) bool ShashinManager::useMoveGenCrystalLogic() const {
    const auto& s = state.staticState;
    return (state.dynamicBase.currentRange >= ShashinPosition::MIDDLE_LOW_TAL
            && state.dynamicBase.currentRange <= ShashinPosition::MIDDLE_TAL && s.kingDanger);
}
__attribute__((hot)) bool ShashinManager::useStep17CrystalLogic() const {
    const auto& s = state.staticState;  // Local access

    // Strategical with high material and many moves
    if (__builtin_expect((state.dynamicBase.currentRange == ShashinPosition::CAPABLANCA)
                           && !s.kingDanger && s.highMaterial
                           && s.legalMoveCount >= params.highMobilityMoves,
                         0))
    {
        return true;
    }

    // Tal logic: attacking with low complexity and king danger
    if (__builtin_expect((state.dynamicBase.currentRange >= ShashinPosition::MIDDLE_LOW_TAL
                          && state.dynamicBase.currentRange <= ShashinPosition::MIDDLE_TAL)
                           && s.legalMoveCount < params.highMobilityMoves && s.kingDanger,
                         1))
    {
        return true;
    }

    return false;
}

bool ShashinManager::useCrystalFutility() const {
    const auto& st = state.staticState;
    const auto& dd = state.dynamicDerived;
    // Applichiamo Crystal solo in posizioni tattiche “pulite”:
    return dd.isTacticalReactive && !st.stmKingExposed && !st.isSacrificial
        && st.legalMoveCount <= params.advancedDepthLimit;
}

bool ShashinManager::allowCrystalProbCut() const {
    const auto& s = state.staticState;
    const auto& d = state.dynamicDerived;

    return (d.isHighTal || d.isTacticalReactive) && s.legalMoveCount < params.highMobilityMoves;
}

bool ShashinManager::allowStockfishProbCut() const {
    const auto& s     = state.staticState;
    int         depth = state.dynamicBase.currentDepth;
    return (s.legalMoveCount < 40 && depth <= 6)
        || (s.legalMoveCount < 60 && depth <= params.midDepthLimit);
}

bool ShashinManager::isTacticalReactive() const {
    return anyOf(state.dynamicBase.currentRange, ShashinPosition::LOW_PETROSIAN);
}

bool ShashinManager::isTacticalDefensive() const {
    return anyOf(state.dynamicBase.currentRange, ShashinPosition::MIDDLE_LOW_PETROSIAN,
                 ShashinPosition::MIDDLE_PETROSIAN);
}

bool ShashinManager::isShashinStyle(std::initializer_list<ShashinPosition> positions) const {
    return std::any_of(positions.begin(), positions.end(), [this](ShashinPosition pos) {
        return state.dynamicBase.currentRange == pos;
    });
}

bool ShashinManager::isTillCategory(ShashinPosition lowerBound, ShashinPosition upperBound) const {
    return !anyOf(state.dynamicBase.currentRange, lowerBound, upperBound);
}


bool ShashinManager::useNullMoveByShashinForStockfish() const {
    if (state.dynamicDerived.isStrategical)
        return true;

    const auto& staticState    = state.staticState;
    const auto& dynamicDerived = state.dynamicDerived;
    bool        avoidNullMove  = isInRange(ShashinPosition::HIGH_TAL)
                      || (dynamicDerived.isAggressive && staticState.highMaterial);

    return !avoidNullMove;
}

bool ShashinManager::isSimpleIntermediate() const {
    const auto& staticState = state.staticState;
    return isTillCategory(ShashinPosition::MIDDLE_PETROSIAN, ShashinPosition::MIDDLE_TAL)
        && staticState.legalMoveCount <= params.midMobilityMoves && !staticState.kingDanger
        && !isHighPieceDensity() && state.dynamicBase.currentDepth < params.midDepthLimit;
}

bool ShashinManager::isMCTSApplicableByValue() const {
    return isInRange(ShashinPosition::HIGH_PETROSIAN)
        || isInRange(ShashinPosition::MIDDLE_HIGH_PETROSIAN)
        || isInRange(ShashinPosition::MIDDLE_PETROSIAN);
}

bool ShashinManager::isMCTSExplorationApplicable() const {
    size_t legalMoveCount = state.staticState.legalMoveCount;
    return ((isInRange(ShashinPosition::MIDDLE_HIGH_PETROSIAN)
             || isInRange(ShashinPosition::HIGH_TAL))
            && legalMoveCount >= params.highMobilityMoves)
        || isInRange(ShashinPosition::CAPABLANCA) || isPetrosian();
}

bool ShashinManager::isComplexPosition() const {
    const auto& staticState = state.staticState;
    return (staticState.legalMoveCount >= 25 && staticState.highMaterial) || staticState.kingDanger
        || staticState.pawnsNearPromotion;
}
ShashinPosition ShashinManager::getResilientShashinRange(ShashinPosition range, int depth) {
    int rootDepth = state.dynamicBase.rootDepth;
    if (rootDepth <= 0)
        return range;

    double             depthRatio  = static_cast<double>(depth) / rootDepth;
    const StaticState& staticState = state.staticState;
    bool               complex     = isComplexPosition();

    switch (range)
    {
    case ShashinPosition::CAPABLANCA_TAL :
        // +2% per complessità + re esposto
        if (complex && staticState.opponentKingExposed)
            return (depthRatio <= 0.62) ? ShashinPosition::LOW_TAL : ShashinPosition::CAPABLANCA;

        return (depthRatio <= 0.60) ? ShashinPosition::LOW_TAL : ShashinPosition::CAPABLANCA;

    case ShashinPosition::CAPABLANCA_PETROSIAN :
        // +2% per complessità + re avversario esposto
        if (complex && staticState.stmKingExposed)
            return (depthRatio <= 0.62) ? ShashinPosition::CAPABLANCA
                                        : ShashinPosition::LOW_PETROSIAN;

        return (depthRatio <= 0.60) ? ShashinPosition::CAPABLANCA : ShashinPosition::LOW_PETROSIAN;

    case ShashinPosition::TAL_CAPABLANCA_PETROSIAN :
        // Manteniamo le soglie originali per l'ibrido triplo
        if (depthRatio <= 0.40)
            return ShashinPosition::LOW_TAL;
        if (depthRatio <= 0.80)
            return ShashinPosition::CAPABLANCA;
        return ShashinPosition::LOW_PETROSIAN;

    default :
        return range;
    }
}
bool ShashinManager::isHighPieceDensityCapablancaPosition() const {
    return isInRange(ShashinPosition::CAPABLANCA) && isHighPieceDensity();
}

bool ShashinManager::isTacticalReduction() const {
    const auto& staticState = state.staticState;
    return isAggressive() && (staticState.highMaterial || !isHighPieceDensity());
}

bool ShashinManager::avoidStep10() const {
    const auto& staticState    = state.staticState;
    const auto& dynamicDerived = state.dynamicDerived;
    if (staticState.kingDanger || staticState.isSacrificial || staticState.stmKingExposed)
        return true;
    if (dynamicDerived.isStrategical)
    {
        return (staticState.legalMoveCount < 5) && (staticState.allPiecesCount < 8);
    }
    else if (dynamicDerived.isAggressive)
    {
        return staticState.stmKingSafetyScore < 40;
    }
    return false;
}

bool ShashinManager::isTalTacticalHighMiddle() const {
    return isInRange(ShashinPosition::HIGH_TAL) || isInRange(ShashinPosition::MIDDLE_HIGH_TAL);
}

bool ShashinManager::isTacticalInitiative() const {
    return anyOf(state.dynamicBase.currentRange, ShashinPosition::LOW_TAL);
}

bool ShashinManager::isTal() const {
    return isShashinStyle(
      {ShashinPosition::LOW_TAL, ShashinPosition::MIDDLE_TAL, ShashinPosition::HIGH_TAL});
}

bool ShashinManager::isCapablanca() const { return inRange<ShashinPosition::CAPABLANCA>(); }

void ShashinManager::initDynamicBaseState(Value                currentValue,
                                          Position&            rootPos,
                                          const ShashinConfig& config,
                                          int                  rootDepth) {
    state.dynamicBase.currentDepth = 0;
    state.dynamicBase.rootDepth    = rootDepth;  // Memorizza la profondità massima
    ShashinPosition currentRange   = getInitialShashinRange(rootPos, currentValue, config);
    state.dynamicBase.currentRange = getResilientShashinRange(currentRange, 0);
}

void ShashinManager::setDynamicBaseState(Value           score,
                                         const Position& rootPos,
                                         int             depth,
                                         int             rootDepth) {
    ShashinPosition range          = getShashinRange(score, rootPos);
    state.dynamicBase.currentRange = getResilientShashinRange(range, depth);
    state.dynamicBase.currentDepth = depth;
    state.dynamicBase.rootDepth    = rootDepth;
}

void ShashinManager::initDynamicRootShashinState(const Eval::NNUE::Networks&   networks,
                                                 Eval::NNUE::AccumulatorStack& accumulators,
                                                 Eval::NNUE::AccumulatorCaches refreshTable,
                                                 Position&                     rootPos,
                                                 Search::Stack*                ss,
                                                 int                           optimism,
                                                 const ShashinConfig&          config,
                                                 Depth                         rootDepth) {
    const auto& alignedNetworks = networks;
    initDynamicBaseState(
      static_value(alignedNetworks, accumulators, refreshTable, rootPos, ss, optimism), rootPos,
      config, rootDepth);
    setDynamicDerivedState();
    MoveConfig::useMoveShashinLogic = state.dynamicDerived.useMoveGenCrystalLogic;
    MoveConfig::isStrategical       = isStrategical();
    MoveConfig::isAggressive        = state.dynamicDerived.isAggressive;
}

void ShashinManager::updateRootShashinState(Value           score,
                                            const Position& rootPos,
                                            int             depth,
                                            int             rootDepth) {
    if (depth <= state.dynamicBase.currentDepth && depth != 0)
        return;

    ShashinPosition newRange = getShashinRange(score, rootPos);
    if (newRange == state.dynamicBase.currentRange && depth == state.dynamicBase.currentDepth)
        return;

    setDynamicBaseState(score, rootPos, depth, rootDepth);
    setDynamicDerivedState();

    // Direct comparison without temporaries
    const bool newUseLogic    = useMoveGenCrystalLogic();
    const bool newStrategical = state.dynamicDerived.isStrategical;
    const bool newAggressive  = state.dynamicDerived.isAggressive;

    if (newUseLogic != MoveConfig::useMoveShashinLogic
        || newStrategical != MoveConfig::isStrategical || newAggressive != MoveConfig::isAggressive)
    {
        MoveConfig::useMoveShashinLogic = newUseLogic;
        MoveConfig::isStrategical       = newStrategical;
        MoveConfig::isAggressive        = newAggressive;
    }
}

void ShashinManager::setDynamicDerivedState() {
    auto& derived                  = state.dynamicDerived;
    derived.isStrategical          = isStrategical();
    derived.isAggressive           = isAggressive();
    derived.isTacticalReactive     = isTacticalReactive();
    derived.isHighTal              = isInRange(ShashinPosition::HIGH_TAL);
    derived.useMoveGenCrystalLogic = useMoveGenCrystalLogic();
}

bool ShashinManager::isFortress(const Position& pos) {
    const uint64_t newHash = computePositionHash(pos);
    if (positionCache.posHash == newHash)
        return positionCache.fortress;

    auto cacheFalse = [&]() {
        positionCache.posHash  = newHash;
        positionCache.fortress = false;
        return false;
    };

    const Color us     = pos.side_to_move();
    const Color them   = ~us;
    const int   MinPly = params.minPlyFortress;

    // Condizioni più restrittive
    if (pos.rule50_count() < MinPly + 1 || pos.count<PAWN>(us) < 4
        || pos.non_pawn_material(us) < PieceValue[ROOK])
    {
        return cacheFalse();
    }

    const Square ourKing   = pos.square<KING>(us);
    const Square theirKing = pos.square<KING>(them);

    // Re più distanti
    if (distance(ourKing, theirKing) <= 4)
        return cacheFalse();

    // Materiale attaccante più limitato
    if (pos.non_pawn_material(them) > 2 * PieceValue[KNIGHT] || pos.count<QUEEN>(them) > 0)
    {
        return cacheFalse();
    }

    // Struttura pedonale migliore
    Bitboard ourPawns = pos.pieces(us, PAWN);
    if (popcount(ourPawns & (shift<NORTH>(ourPawns) | shift<SOUTH>(ourPawns))) < 3)
        return cacheFalse();

    // Mobilità nemica più ristretta
    int theirMobility = 0;
    for (PieceType pt : {KNIGHT, BISHOP, ROOK})
    {
        Bitboard pieces = pos.pieces(them, pt);
        while (pieces)
        {
            Square   s       = pop_lsb(pieces);
            Bitboard attacks = attacks_bb(pt, s, pos.pieces());
            theirMobility += popcount(attacks & ~pos.pieces(us, KING));
        }
    }
    if (theirMobility > 8)
        return cacheFalse();

    // Almeno 3 mosse sicure
    if (count_safe_waiting_moves(pos) < 3)
        return cacheFalse();

    positionCache.posHash  = newHash;
    positionCache.fortress = true;
    return true;
}


Value ShashinManager::static_value(const Eval::NNUE::Networks&   networks,
                                   Eval::NNUE::AccumulatorStack& accumulators,
                                   Eval::NNUE::AccumulatorCaches refreshTable,
                                   Position&                     rootPos,
                                   Search::Stack*                ss,
                                   Value                         optimism) {
    // 1. Gestione casi terminali e posizioni quiescenti
    if (ss->ply >= MAX_PLY || rootPos.is_draw(ss->ply))
        return VALUE_DRAW;

    MoveList<LEGAL> legalMoves(rootPos);
    if (legalMoves.size() == 0)
        return rootPos.checkers() ? VALUE_MATE : VALUE_DRAW;

    // 2. Valutazione NNUE diretta se non sotto scacco
    if (!rootPos.checkers())
        return Eval::evaluate(networks, rootPos, accumulators, refreshTable, optimism);

    // 3. Ricorsione su tutte le mosse legali (evasioni obbligatorie)
    Value bestValue = -VALUE_INFINITE;
    for (const auto& move : legalMoves)
    {
        StateInfo  st;
        DirtyPiece dp = rootPos.do_move(move, st, rootPos.gives_check(move), nullptr);
        accumulators.push(dp);

        Value val = -static_value(networks, accumulators, refreshTable, rootPos, ss + 1, -optimism);

        rootPos.undo_move(move);
        accumulators.pop();

        if (val > bestValue)
        {
            bestValue = val;
            if (bestValue == VALUE_MATE)
                break;  // Early exit per scacco matto
        }
    }

    return bestValue;
}

void ShashinManager::setStaticState(const Position& rootPos) {
    StaticState& staticState = state.staticState;
    invalidateCaches();  // Invalidate caches when position changes

    staticState.legalMoveCount      = MoveList<LEGAL>(rootPos).size();
    staticState.isSacrificial       = isSacrificialPosition(rootPos);
    staticState.stmKingExposed      = isStmKingExposed();
    staticState.opponentKingExposed = isOpponentKingExposed();
    int nonPawnMaterial      = rootPos.non_pawn_material(WHITE) + rootPos.non_pawn_material(BLACK);
    staticState.highMaterial = (nonPawnMaterial > 2400);

    staticState.kingDanger         = king_danger(rootPos, WHITE) || king_danger(rootPos, BLACK);
    staticState.stmKingDanger      = king_danger(rootPos, rootPos.side_to_move());
    staticState.pawnsNearPromotion = isPawnNearPromotion(rootPos);
    staticState.allPiecesCount     = rootPos.count<ALL_PIECES>();
    staticState.stmKingSafetyScore = king_safety_score(rootPos, rootPos.side_to_move());
    staticState.opponentKingSafetyScore = king_safety_score(rootPos, ~rootPos.side_to_move());

    MoveConfig::isFortress = isFortress(rootPos);
}

bool ShashinManager::isSacrificialPosition(const Position& rootPos) {
    const uint64_t newHash = computePositionHash(rootPos);
    if (positionCache.posHash == newHash)
    {
        return positionCache.sacrificial;
    }
    positionCache.posHash     = newHash;
    positionCache.sacrificial = false;
    MoveList<LEGAL> legalMoves(rootPos);
    for (const auto& move : legalMoves)
    {
        if (is_sacrifice(rootPos, move))
        {
            positionCache.sacrificial = true;
            break;  // Interrompi al primo sacrificio trovato
        }
    }

    return positionCache.sacrificial;
}

bool ShashinManager::isPawnNearPromotion(const Position& rootPos) {
    const uint64_t newHash = computePositionHash(rootPos);
    if (positionCache.posHash == newHash)
    {
        return positionCache.pawnNearPromo;
    }
    positionCache.posHash = newHash;
    positionCache.pawnNearPromo =
      ((rootPos.pieces(WHITE, PAWN) & (Rank5BB | Rank6BB | Rank7BB))
       || (rootPos.pieces(BLACK, PAWN) & (Rank2BB | Rank3BB | Rank4BB)));
    return positionCache.pawnNearPromo;
}

constexpr ShashinPosition getPositionForValue(int i) {
    if (i <= HIGH_PETROSIAN_MAX)
        return ShashinPosition::HIGH_PETROSIAN;
    else if (i <= MIDDLE_HIGH_PETROSIAN_MAX)
        return ShashinPosition::MIDDLE_HIGH_PETROSIAN;
    else if (i <= MIDDLE_PETROSIAN_MAX)
        return ShashinPosition::MIDDLE_PETROSIAN;
    else if (i <= MIDDLE_LOW_PETROSIAN_MAX)
        return ShashinPosition::MIDDLE_LOW_PETROSIAN;
    else if (i <= LOW_PETROSIAN_MAX)
        return ShashinPosition::LOW_PETROSIAN;
    else if (i <= CAPABLANCA_PETROSIAN_MAX)
        return ShashinPosition::CAPABLANCA_PETROSIAN;
    else if (i == CAPABLANCA_MAX)
        return ShashinPosition::CAPABLANCA;
    else if (i <= CAPABLANCA_TAL_MAX)
        return ShashinPosition::CAPABLANCA_TAL;
    else if (i <= LOW_TAL_MAX)
        return ShashinPosition::LOW_TAL;
    else if (i <= MIDDLE_LOW_TAL_MAX)
        return ShashinPosition::MIDDLE_LOW_TAL;
    else if (i <= MIDDLE_TAL_MAX)
        return ShashinPosition::MIDDLE_TAL;
    else if (i <= MIDDLE_HIGH_TAL_MAX)
        return ShashinPosition::MIDDLE_HIGH_TAL;
    else
        return ShashinPosition::HIGH_TAL;
}

template<std::size_t... Is>
constexpr std::array<ShashinPosition, sizeof...(Is)> makeLookupTable(std::index_sequence<Is...>) {
    return {getPositionForValue(Is)...};
}

constexpr auto lookup = makeLookupTable(std::make_index_sequence<101>{});

ShashinPosition ShashinManager::getShashinRange(Value value, const Position& rootPos) {
    WDLModel::WDL wdl            = WDLModel::get_wdl(value, rootPos);
    int           winProbability = wdl.win + (wdl.draw / 2);
    if (winProbability == CAPABLANCA_MAX)
    {
        return (wdl.draw == 100) ? ShashinPosition::CAPABLANCA
                                 : ShashinPosition::TAL_CAPABLANCA_PETROSIAN;
    }

    // Use the lookup table for valid indices
    if (winProbability >= 0 && winProbability <= 100)
    {
        return lookup[winProbability];
    }

    return ShashinPosition::TAL_CAPABLANCA_PETROSIAN;
}

ShashinPosition ShashinManager::getInitialShashinRange(Position&            rootPos,
                                                       Value                staticValue,
                                                       const ShashinConfig& config) {
    // 1. Config disablede → Dynamic evaluaton (Total Chaos included)
    if (!config.highTal && !config.middleTal && !config.lowTal && !config.capablanca
        && !config.highPetrosian && !config.middlePetrosian && !config.lowPetrosian)
    {
        return getShashinRange(staticValue, rootPos);
    }

    // 2. Special cases: fortess → Capablanca
    if (isFortress(rootPos))
    {
        return ShashinPosition::CAPABLANCA;
    }

    // 3. Hybrid combinations (max priority)
    // -----------------------------------------
    // 3a. Tal-Capablanca (every level Tal + Capablanca)
    if (config.capablanca && (config.highTal || config.middleTal || config.lowTal))
    {
        return ShashinPosition::CAPABLANCA_TAL;
    }

    // 3b. Petrosian-Capablanca (every level Petrosian + Capablanca)
    if (config.capablanca
        && (config.highPetrosian || config.middlePetrosian || config.lowPetrosian))
    {
        return ShashinPosition::CAPABLANCA_PETROSIAN;
    }

    // 4. TAL (High → Middle → Low)
    // -------------------------------------------
    if (config.highTal)
    {
        return config.middleTal ? ShashinPosition::MIDDLE_HIGH_TAL : ShashinPosition::HIGH_TAL;
    }
    if (config.middleTal)
    {
        return config.lowTal ? ShashinPosition::MIDDLE_LOW_TAL : ShashinPosition::MIDDLE_TAL;
    }
    if (config.lowTal)
    {
        return ShashinPosition::LOW_TAL;
    }

    // 5. PETROSIAN (High → Middle → Low)
    // ------------------------------------------------
    if (config.highPetrosian)
    {
        return config.middlePetrosian ? ShashinPosition::MIDDLE_HIGH_PETROSIAN
                                      : ShashinPosition::HIGH_PETROSIAN;
    }
    if (config.middlePetrosian)
    {
        return config.lowPetrosian ? ShashinPosition::MIDDLE_LOW_PETROSIAN
                                   : ShashinPosition::MIDDLE_PETROSIAN;
    }
    if (config.lowPetrosian)
    {
        return ShashinPosition::LOW_PETROSIAN;
    }

    // 6. Capablanca (last option)
    if (config.capablanca)
    {
        return ShashinPosition::CAPABLANCA;
    }

    // 7. Default: Total Chaos (unmanaged combinations)
    return ShashinPosition::TAL_CAPABLANCA_PETROSIAN;
}

}  // namespace ShashChess