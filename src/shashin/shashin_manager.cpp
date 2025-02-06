#include "shashin_manager.h"
#include "../search.h"

namespace Alexander {

ShashinManager::ShashinManager() {}

void ShashinManager::setDepth(int depth) { state.currentDepth = depth; }

int ShashinManager::getDepth() const { return state.currentDepth; }

RootShashinState ShashinManager::getState() const {

    return state;  // Restituisce una copia sicura dello stato
}

Value ShashinManager::static_value(Position& rootPos, Search::Stack* ss) {
    MoveList<LEGAL> legalMoves(rootPos);  // Crea la lista delle mosse legali

    if (ss->ply >= MAX_PLY || rootPos.is_draw(ss->ply)
        || (legalMoves.size() == 0 && !rootPos.checkers()))
        return VALUE_DRAW;

    if (legalMoves.size() == 0)
        return VALUE_MATE;

    return Eval::evaluate(rootPos);
}

void ShashinManager::computeRootState(const Position& rootPos, RootShashinState& rootShashinState) {
    rootShashinState.legalMoveCount = MoveList<LEGAL>(rootPos).size();
    rootShashinState.highMaterial =
      rootPos.non_pawn_material(WHITE) + rootPos.non_pawn_material(BLACK) > 2400;
    rootShashinState.kingDanger         = rootPos.king_danger(WHITE) || rootPos.king_danger(BLACK);
    rootShashinState.allPiecesCount     = rootPos.count<ALL_PIECES>();
    rootShashinState.pawnsNearPromotion = isPawnNearPromotion(rootPos);
}
bool ShashinManager::isPawnNearPromotion(const Position& rootPos) {
    // Consideriamo i pedoni avanzati (vicini alla promozione)
    int advancedPawns = 0;
    for (Square s = SQ_A2; s <= SQ_H7; ++s)
    {
        Piece piece = rootPos.piece_on(s);
        if (type_of(piece) == PAWN)
        {
            if (color_of(piece) == WHITE && rank_of(s) >= RANK_5)
            {  // Pedoni avanzati dei bianchi
                advancedPawns++;
            }
            else if (color_of(piece) == BLACK && rank_of(s) <= RANK_4)
            {  // Pedoni avanzati dei neri
                advancedPawns++;
            }
        }
    }
    return advancedPawns > 0;
}

//dovrebbe essere giusto questo metodo, ma controlla begin
void ShashinManager::initShashinValues(Position&            rootPos,
                                       Search::Stack*       ss,
                                       const ShashinConfig& config) {
    state.currentDepth           = 0;
    Value           staticValue  = static_value(rootPos, ss);
    ShashinPosition initialRange = getInitialShashinRange(rootPos, staticValue, config);

    // Handle Chaos positions aggressively at depth = 0
    if (initialRange == ShashinPosition::CAPABLANCA_TAL)
    {
        state.currentRange = ShashinPosition::LOW_TAL;
    }
    else if (initialRange == ShashinPosition::CAPABLANCA_PETROSIAN)
    {
        state.currentRange = ShashinPosition::CAPABLANCA;
    }
    else if (initialRange == ShashinPosition::TAL_CAPABLANCA_PETROSIAN)
    {
        state.currentRange = ShashinPosition::LOW_TAL;
    }
    else
    {
        state.currentRange = initialRange;
    }

    // Compute derived state values
    computeRootState(rootPos, state);

    // Update MoveGenConfig
    MoveGenConfig::useMoveGenCrystalLogic = useMoveGenAndStep17CrystalLogic();
}
//dovrebbe essere giusto questo metodo, ma controlla end
//dovrebbe essere giusto questo metodo, ma controlla begin
void ShashinManager::updateShashinValues(Value score, const Position& rootPos, int depth) {
    if ((depth > state.currentDepth) || (depth == 0))
    {
        ShashinPosition range = static_cast<ShashinPosition>(getShashinRange(score, rootPos));

        if (range == ShashinPosition::CAPABLANCA_TAL)
        {
            state.currentRange =
              (depth <= 10) ? ShashinPosition::LOW_TAL : ShashinPosition::CAPABLANCA;
        }
        else if (range == ShashinPosition::CAPABLANCA_PETROSIAN)
        {
            state.currentRange =
              (depth <= 10) ? ShashinPosition::CAPABLANCA : ShashinPosition::LOW_PETROSIAN;
        }
        else if (range == ShashinPosition::TAL_CAPABLANCA_PETROSIAN)
        {
            if (depth <= 10)
                state.currentRange = ShashinPosition::LOW_TAL;
            else if (depth <= 20)
                state.currentRange = ShashinPosition::CAPABLANCA;
            else
                state.currentRange = ShashinPosition::LOW_PETROSIAN;
        }
        else
        {
            state.currentRange = range;
        }

        state.currentDepth = depth;

        // Compute derived state values
        computeRootState(rootPos, state);

        MoveGenConfig::useMoveGenCrystalLogic = useMoveGenAndStep17CrystalLogic();
    }
}

// Static lookup table for Shashin ranges
static const std::array<ShashinPosition, 101> initializeLookupTable() {
    // Create the lookup table using a lambda
    std::array<ShashinPosition, 101> lookup{};
    auto                             assignRange = [&](int start, int end, ShashinPosition pos) {
        for (int i = start; i <= end; ++i)
        {
            lookup[i] = pos;
        }
    };

    // Populate ranges
    assignRange(0, HIGH_PETROSIAN_MAX, ShashinPosition::HIGH_PETROSIAN);
    assignRange(HIGH_PETROSIAN_MAX + 1, MIDDLE_HIGH_PETROSIAN_MAX,
                ShashinPosition::MIDDLE_HIGH_PETROSIAN);
    assignRange(MIDDLE_HIGH_PETROSIAN_MAX + 1, MIDDLE_PETROSIAN_MAX,
                ShashinPosition::MIDDLE_PETROSIAN);
    assignRange(MIDDLE_PETROSIAN_MAX + 1, MIDDLE_LOW_PETROSIAN_MAX,
                ShashinPosition::MIDDLE_LOW_PETROSIAN);
    assignRange(MIDDLE_LOW_PETROSIAN_MAX + 1, LOW_PETROSIAN_MAX, ShashinPosition::LOW_PETROSIAN);
    assignRange(LOW_PETROSIAN_MAX + 1, CAPABLANCA_PETROSIAN_MAX,
                ShashinPosition::CAPABLANCA_PETROSIAN);

    // Special case: Chaos for Win Probability = 50
    lookup[CAPABLANCA_MAX] = ShashinPosition::TAL_CAPABLANCA_PETROSIAN;

    assignRange(CAPABLANCA_MAX + 1, CAPABLANCA_TAL_MAX, ShashinPosition::CAPABLANCA_TAL);
    assignRange(CAPABLANCA_TAL_MAX + 1, LOW_TAL_MAX, ShashinPosition::LOW_TAL);
    assignRange(LOW_TAL_MAX + 1, MIDDLE_LOW_TAL_MAX, ShashinPosition::MIDDLE_LOW_TAL);
    assignRange(MIDDLE_LOW_TAL_MAX + 1, MIDDLE_TAL_MAX, ShashinPosition::MIDDLE_TAL);
    assignRange(MIDDLE_TAL_MAX + 1, MIDDLE_HIGH_TAL_MAX, ShashinPosition::MIDDLE_HIGH_TAL);
    assignRange(MIDDLE_HIGH_TAL_MAX + 1, HIGH_TAL_MAX, ShashinPosition::HIGH_TAL);

    return lookup;
}

// Static lookup table (constexpr in C++17)
static const auto lookup = initializeLookupTable();

// Determine Shashin Range
ShashinPosition ShashinManager::getShashinRange(Value value, const Position& rootPos) {
    // Obtain the WDL representation based on the evaluation and position
    WDLModel::WDL wdl = WDLModel::get_wdl(value, rootPos);

    // Calculate Win Probability directly
    int winProbability = wdl.win + (wdl.draw / 2);

    // Special case for Win Probability = 50
    if (winProbability == CAPABLANCA_MAX)
    {
        // Check WDL to differentiate between Capablanca and Chaos: Capablanca-Petrosian-Tal
        if (wdl.win == 0 && wdl.loss == 0)
        {
            return ShashinPosition::CAPABLANCA;  // Purely equal position
        }
        else
        {
            return ShashinPosition::TAL_CAPABLANCA_PETROSIAN;  // Chaos
        }
    }

    // Use the lookup table if the value is within bounds
    if (winProbability >= 0 && winProbability <= 100)
    {
        return lookup[winProbability];
    }

    // Fallback for unhandled cases
    return ShashinPosition::TAL_CAPABLANCA_PETROSIAN;
}

ShashinPosition ShashinManager::getInitialShashinRange(Position&            rootPos,
                                                       Value                staticValue,
                                                       const ShashinConfig& config) {
    // Se tutte le configurazioni sono disabilitate, usa la valutazione standard
    if (!config.highTal && !config.middleTal && !config.lowTal && !config.capablanca
        && !config.highPetrosian && !config.middlePetrosian && !config.lowPetrosian)
    {
        return getShashinRange(staticValue, rootPos);
    }

    // Petrosian logic
    if (config.highPetrosian)
    {
        if (config.middlePetrosian)
            return ShashinPosition::MIDDLE_HIGH_PETROSIAN;
        return ShashinPosition::HIGH_PETROSIAN;
    }
    if (config.middlePetrosian)
    {
        if (config.lowPetrosian)
            return ShashinPosition::MIDDLE_LOW_PETROSIAN;
        return ShashinPosition::MIDDLE_PETROSIAN;
    }
    if (config.lowPetrosian)
    {
        if (config.capablanca)
            return ShashinPosition::CAPABLANCA_PETROSIAN;
        return ShashinPosition::LOW_PETROSIAN;
    }

    // Capablanca logic
    if (config.capablanca)
    {
        return ShashinPosition::CAPABLANCA;
    }

    // Tal logic
    if (config.highTal)
    {
        if (config.middleTal)
            return ShashinPosition::MIDDLE_HIGH_TAL;
        return ShashinPosition::HIGH_TAL;
    }
    if (config.middleTal)
    {
        if (config.lowTal)
            return ShashinPosition::MIDDLE_LOW_TAL;
        return ShashinPosition::MIDDLE_TAL;
    }
    if (config.lowTal)
    {
        if (config.capablanca)
            return ShashinPosition::CAPABLANCA_TAL;
        return ShashinPosition::LOW_TAL;
    }

    // Default static value
    return getShashinRange(staticValue, rootPos);
}
}  // namespace Alexander
