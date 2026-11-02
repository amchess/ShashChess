#include "search.h"
...
#include <cstdlib>
//from shashchess
#include <iostream>
...
#include "wdl/win_probability.h"  //shashin theory
...
namespace ShashChess {
using namespace ShashChess::Shashin;  //shashin
...
using namespace Search;
namespace {
...
}  // namespace
...
Search::Worker::Worker(SharedState&                    sharedState,
                       std::unique_ptr<ISearchManager> sm,
                       size_t                          threadId,
                       NumaReplicatedAccessToken       token) :
	...    
	refreshTable(networks[token]),
    shashinManager(std::make_unique<ShashinManager>()),
    shConfig(sharedState.shashinConfig) {
    clear();
}
//from shashin begin
Search::Worker::~Worker() = default;
ShashinManager& Worker::getShashinManager() { return *shashinManager; }
//fom Shashin end
...
void Search::Worker::start_searching() {

    accumulatorStack.reset();

    // Non-main threads go directly to iterative_deepening()
    if (!is_mainthread())
    {
        iterative_deepening();
        return;
    }
	...
    else
    {
        // It inizializes the rootPosition static state
        shashinManager->setStaticState(rootPos);
    }
	...
}
...
void Search::Worker::iterative_deepening() {
    auto&          localShashinManager = *this->shashinManager;
	...
    ss->pv = pv;
    localShashinManager.initDynamicRootShashinState(
      networks[numaAccessToken], accumulatorStack, refreshTable, rootPos, ss, VALUE_ZERO, shConfig);
	...
				...
                if (mainThread && multiPV == 1 && (bestValue <= alpha || bestValue >= beta)
                    && nodes > 10000000)
                {                                                              //no warning
                    main_manager()->pv(*this, threads, tt, rootDepth, false);  //shashin
                }  //no warning
				...
            }
			...
            if (mainThread
                && (threads.stop || pvIdx + 1 == multiPV || nodes > 10000000)
				...                
				&& !(threads.abortedSearch && is_loss(rootMoves[0].uciScore)))
            {                                                             //no warning
                main_manager()->pv(*this, threads, tt, rootDepth, true);  //shashin
            }  //no warning
			...
        }
	...
}
...
// Main search function for both PV and non-PV nodes
template<NodeType nodeType>
Value Search::Worker::search(
  Position& pos, Stack* ss, Value alpha, Value beta, Depth depth, bool cutNode) {
	...

    auto&                   localShashinManager = *this->shashinManager;
    const RootShashinState& rootShashinState    = localShashinManager.getState();  //for ShashChess
    // From Shashin fortress begin

    bool isFortressNode = localShashinManager.isFortress(rootPos);
    if (depth > 24 && !PvNode && isFortressNode)
    {
        depth = std::max(depth - 1, Depth(10));

        int progress = pos.rule50_count();
        if (progress < 29)
        {
            return ss->staticEval;
        }
        if (pos.count<ALL_PIECES>() > 12)
        {
            return ss->staticEval;
        }
        int window = std::min(15, 50 - progress - 10);
        if (progress >= (50 - window))
        {
            double weight = (progress - (50 - window)) / double(window);
            weight        = std::clamp(weight, 0.0, 1.0);

            bool  useNNUE  = (nodeType != NonPV) && (depth > 25);
            Value baseEval = useNNUE ? evaluate(pos) : ss->staticEval;

            Value drawInf = static_cast<Value>((1.0 - weight) * baseEval + weight * VALUE_DRAW);
            constexpr Value DrawBoundary = VALUE_DRAW - 384;
            if (drawInf >= DrawBoundary)
                return std::clamp(drawInf, DrawBoundary, VALUE_DRAW);
        }

        if (depth < 15)
        {
            static constexpr int Bonus[16] = {330, 308, 286, 264, 242, 220, 198, 176,
                                              154, 132, 110, 88,  66,  44,  22,  0};
            ss->staticEval += Bonus[depth];
        }
    }
    // From Shashin fortress end

	...    
    //from Shashin-Crystal begin
    bool gameCycle, capture, ttCapture;
    //from shashin crystal end
	...
    bool         ourMove = !(ss->ply & 1);  // from Shashin Crystal
    SearchedList capturesSearched;
	...
    maxValue           = VALUE_INFINITE;
    // from Crystal-shashin begin
    gameCycle       = false;
    bool kingDanger = !ourMove && king_danger(pos, us);

    ss->secondaryLine = false;
    ss->mainLine      = false;
    // from Crystal-shashin end
	...
    //from crystal shashin begin
    const auto&     dynamicBase        = rootShashinState.dynamicBase;
    const auto&     dynamicDerived     = rootShashinState.dynamicDerived;
    const auto&     staticState        = rootShashinState.staticState;
    bool            isStrategical      = dynamicDerived.isStrategical;
    bool            isAggressive       = dynamicDerived.isAggressive;
    bool            isTacticalReactive = dynamicDerived.isTacticalReactive;
    bool            quietKingSafe = !staticState.isSacrificial && (!staticState.stmKingExposed);
    ShashinPosition style         = dynamicBase.currentRange;
    bool            isCapablanca  = (style == ShashinPosition::CAPABLANCA);
    bool            isTal         = localShashinManager.isTal();
    bool            isPetrosian   = localShashinManager.isPetrosian();
    //from crystal shashin end
    if (!rootNode)
    {
        // from Crystal-Shashin begin
        bool allowCycleDetection =
          (isStrategical && localShashinManager.isComplexPosition())
          || (localShashinManager.isTacticalDefensive() && localShashinManager.isLowActivity());

        if (allowCycleDetection && !ss->excludedMove)
        {
            if (pos.upcoming_repetition(ss->ply))
            {
                gameCycle = true;
            }
        }
        // from Crystal-Shashin end
        // Step 2. Check for aborted search and immediate draw
		...        
		// Step 3. Mate distance pruning. Even if we mate at the next move our score
		...  
    }

	...

    // Step 4. Transposition table lookup
	...

    // Step 5. Tablebases probe
	...
    //from shashin-crystal begin
    Depth originalDepth = (Depth) 0;
    //from shashin-crystal end
    // Step 6. Static evaluation of the position
	...
    //from Shashin begin
    int  nullThreshold;
    bool commonNullMoveConditions;
    //from Shashin end
	...

    // Step 7. Razoring
	...
    // Step 8. Futility pruning: child node
    // The depth condition is important for mate finding.
    {
        // Adjust futility margin based on Shashin style
        Value styleFactor = 100;  // Default = neutral
        if (isAggressive)
        {
            styleFactor = 80;  // Reduce pruning in aggressive positions
        }
        else if (isStrategical)
        {
            styleFactor = 120;  // Increase pruning in strategical positions
        }
        else if (kingDanger)
        {
            styleFactor = 70;  // Reduce pruning when king is in danger
        }

        auto futility_margin = [&](Depth d) {
            Value baseMargin     = 90 - 20 * (cutNode && !ss->ttHit);
            Value adjustedMargin = (baseMargin * styleFactor) / 100;

            return adjustedMargin * d - improving * adjustedMargin * 2
                 - opponentWorsening * adjustedMargin / 3 + (ss - 1)->statScore / 356
                 + std::abs(correctionValue) / 171290
                 - 50 * staticState.legalMoveCount / 40;  // Account for mobility
        };

        bool allowPruning = !staticState.kingDanger || (dynamicDerived.isStrategical && depth < 8);

        if (allowPruning && !ss->ttPv && depth < 14 && eval - futility_margin(depth) >= beta
            && eval >= beta && (!ttData.move || ttCapture) && !is_loss(beta) && !is_win(eval))
        {
            // Less aggressive reduction in critical positions
            Value reduction = (dynamicDerived.isAggressive) ? (eval - beta) / 4 : (eval - beta) / 3;
            return beta + reduction;
        }
    }
	//end from shashin
		
    // Step 9. Null move search with verification search
    //crystal-shashin logic begin
    //common null move conditions begin
    nullThreshold            = beta - 19 * depth + 403;
    commonNullMoveConditions = cutNode && !disableNMAndPC && ss->staticEval >= nullThreshold
                            && !excludedMove && pos.non_pawn_material(us)
                            && eval >= beta;
    //common null move conditions end
    if (commonNullMoveConditions)
    {
        // Compute conditions once
        bool positionSafe = !kingDanger && quietKingSafe;

        //only from Stockfish begin
        bool allowNullMoveOnlyFromStockfish = (ss->ply >= nmpMinPly) && !is_loss(beta);

        //only from Stockfish end
        // only from shashin crystal begin
        bool allowNullMoveOnlyFromCrystal =
          !nmpGuard && (gameCycle ? (depth < 8 && positionSafe) : true)
          && (eval >= ss->staticEval + 20) && positionSafe
          && (rootDepth < 11 || ourMove || (MoveList<LEGAL>(pos).size() > 8));
        // Master switch from ShashinManager
        bool useNullMoveByShashinForStockfish =
          localShashinManager.useNullMoveByShashinForStockfish();
        bool nullMoveFromCrystal =
          !useNullMoveByShashinForStockfish && allowNullMoveOnlyFromCrystal;
        bool nullMoveFromStockfish =
          useNullMoveByShashinForStockfish && allowNullMoveOnlyFromStockfish;

        // Combined decision logic
        if (nullMoveFromStockfish || nullMoveFromCrystal)
        {
            if (nullMoveFromStockfish)
            {
                assert((ss - 1)->currentMove != Move::null());
            }
            assert(eval - beta >= 0);
            nmpSide = ourMove;  // Null move signal for Shashin
            // Null move dynamic reduction based on depth
            Depth R = 7 + depth / 3;

            // Crystal-specific adjustment
            if (nullMoveFromCrystal)
            {
                if (!ourMove && (ss - 1)->secondaryLine)
                    R = std::min(R, 8);
                if (localShashinManager.isComplexPosition() && depth > 10)
                {
                    R = std::max(R - 1, 3);
                }
            }
            ss->currentMove                   = Move::null();
            ss->continuationHistory           = &continuationHistory[0][0][NO_PIECE][0];
            ss->continuationCorrectionHistory = &continuationCorrectionHistory[NO_PIECE][0];


            do_null_move(pos, st);
            nmpGuard        = true;
            Value nullValue = -search<NonPV>(pos, ss + 1, -beta, -beta + 1, depth - R, false);
            nmpGuard        = false;
            undo_null_move(pos);

            if (nullValue >= beta && !is_win(nullValue))
            {
                if (nmpMinPly || depth < 16)
                    return nullValue;
                assert(!nmpMinPly);  // Recursive verification is not allowed

                // Do verification search at high depths, with null move pruning disabled
                // until ply exceeds nmpMinPly.
                nmpMinPly = ss->ply + 3 * (depth - R) / 4;
                Value v   = search<NonPV>(pos, ss, beta - 1, beta, depth - R, false);
                nmpMinPly = 0;

                if (v >= beta)
                    return nullValue;
            }
        }
        //only from shashin crystal end
    }

    improving |= ss->staticEval >= beta + 94 * !PvNode;

    // Step 10. Internal iterative reductions
    //by Shashin begin
    // At sufficient depth, reduce depth for PV/Cut nodes without a TTMove.
    // (*Scaler) Especially if they make IIR less aggressive.
    originalDepth = depth;
    if (!allNode && depth >= 6 && !ttData.move)
    {
        depth--;
    }
    if (quietKingSafe && !localShashinManager.avoidStep10() && depth >= 6)
    {
        const bool ttReduction = cutNode && ttData.move && ttData.bound == BOUND_UPPER;

        if (ttReduction)
        {
            const int reduction = isStrategical ? 2 : (isAggressive || isTacticalReactive) ? 0 : 1;

            // Applica la riduzione aggiuntiva solo se giustificata
            depth = std::max(originalDepth - reduction, 1);
        }
    }
    //by Shashin end
    // Step 11. ProbCut
    // If we have a good enough capture (or queen promotion) and a reduced search
    // returns a value much above beta, we can (almost) safely prune the previous move.
    probCutBeta = beta + 215 - 60 * improving;
    if (depth >= 3 && !disableNMAndPC  //learning
        && !is_decisive(beta)
        // If value from transposition table is lower than probCutBeta, don't attempt
        // probCut there
        && !(is_valid(ttData.value) && ttData.value < probCutBeta))
    {
        // Shashin adjustment: skip ProbCut in fortress positions or quiet strategical positions
        if (!isFortressNode && !(isStrategical && !kingDanger))
        {
            assert(probCutBeta < VALUE_INFINITE && probCutBeta > beta);

            MovePicker mp(pos, ttData.move, probCutBeta - ss->staticEval, &captureHistory);
            Depth      probCutDepth = std::max(depth - 5, 0);
            // Shashin adjustment: reduce depth more in Capablanca positions
            if (isCapablanca && depth > 8)
                probCutDepth = std::max(depth - 7, 0);
            while ((move = mp.next_move()) != Move::none())
            {
                assert(move.is_ok());

                if (move == excludedMove || !pos.legal(move))
                    continue;

                assert(pos.capture_stage(move));

                movedPiece = pos.moved_piece(move);

                do_move(pos, move, st, ss);


                // Perform a preliminary qsearch to verify that the move holds
                value = -qsearch<NonPV>(pos, ss + 1, -probCutBeta, -probCutBeta + 1);

                // If the qsearch held, perform the regular search
                if (value >= probCutBeta && probCutDepth > 0)
                    value = -search<NonPV>(pos, ss + 1, -probCutBeta, -probCutBeta + 1,
                                           probCutDepth, !cutNode);

                undo_move(pos, move);

                if (value >= probCutBeta)
                {
                    // Save ProbCut data into transposition table
                    ttWriter.write(posKey, value_to_tt(value, ss->ply), ss->ttPv, BOUND_LOWER,
                                   probCutDepth + 1, move, unadjustedStaticEval, tt.generation());

                    if (!is_decisive(value))
                        return value - (probCutBeta - beta);
                }
            }
        }
    }

moves_loop:  // When in check, search starts here

    // Step 12. A small Probcut idea
    //from Shashin Crystal begin
    probCutBeta              = beta + 417;
    const bool commonProbcut = (ttData.bound & BOUND_LOWER) && ttData.depth >= depth - 4
                            && ttData.value >= probCutBeta && !is_decisive(beta)
                            && is_valid(ttData.value) && !is_decisive(ttData.value);

    if (commonProbcut)
    {
        // 1. ProbCut Crystal: only for complex tactical positions
        if (localShashinManager.allowCrystalProbCut() && depth >= 8)
        {
            const bool crystalConditions = !PvNode && ttCapture && !gameCycle;

            if (crystalConditions)
            {
                // More aggressive reduction in high depth
                const Depth probCutDepth = depth - (depth >= 12 ? 5 : 4);
                if (ttData.depth >= probCutDepth)
                    return probCutBeta;
            }
        }

        // 2. ProbCut Stockfish: standard conditions for all other positions
        return probCutBeta;
    }
    //from Shashin Crystal end
	...
    // Step 13. Loop through all pseudo-legal moves until no moves remain
    // or a beta cutoff occurs.
    while ((move = mp.next_move()) != Move::none())
    {
		...
        // Step 14. Pruning at shallow depth.
        // Depth conditions are important for mate finding.
        if (!rootNode && pos.non_pawn_material(us) && !is_loss(bestValue))
        {
            const bool allowPruning = !kingDanger || (isStrategical && depth < 8);
            if (allowPruning)
            {
                // Skip quiet moves if movecount exceeds our FutilityMoveCount threshold
                if (moveCount >= (3 + depth * depth) / (2 - improving))
                    mp.skip_quiet_moves();

                // Reduced depth of the next LMR search
                int lmrDepth = newDepth - r / 1024;

                if (capture || givesCheck)
                {
                    Piece capturedPiece = pos.piece_on(move.to_sq());
                    int captHist = captureHistory[movedPiece][move.to_sq()][type_of(capturedPiece)];

                    // Futility pruning for captures
                    if (!givesCheck && lmrDepth < 7 && !ss->inCheck)
                    {
                        Value futilityValue = ss->staticEval + 225 + 220 * lmrDepth
                                            + 275 * (move.to_sq() == prevSq)
                                            + PieceValue[capturedPiece] + 131 * captHist / 1024;
                        if (futilityValue <= alpha)
                            continue;
                    }

                    // SEE based pruning for captures and checks
                    int margin = std::clamp(158 * depth + captHist / 31, 0, 283 * depth);
                    if (!pos.see_ge(move, -margin))
                    {
                        bool mayStalemateTrap =
                          depth > 2 && alpha < 0
                          && pos.non_pawn_material(us) == PieceValue[movedPiece]
                          && PieceValue[movedPiece] >= RookValue
                          // it can't be stalemate if we moved a piece adjacent to the king
                          && !(attacks_bb<KING>(pos.square<KING>(us)) & move.from_sq())
                          && !mp.can_move_king_or_pawn();

                        // avoid pruning sacrifices of our last piece for stalemate
                        if (!mayStalemateTrap)
                            continue;
                    }
                }
                else
                {
                    int history = (*contHist[0])[movedPiece][move.to_sq()]
                                + (*contHist[1])[movedPiece][move.to_sq()]
                                + pawnHistory[pawn_structure_index(pos)][movedPiece][move.to_sq()];

                    // Continuation history based pruning
                    if (history < -4361 * depth)
                        continue;

                    history += 71 * mainHistory[us][move.from_to()] / 32;

                    lmrDepth += history / 3233;

                    Value baseFutility  = (bestMove ? 46 : 230);
                    Value futilityValue = ss->staticEval + baseFutility + 131 * lmrDepth
                                        + 91 * (ss->staticEval > alpha);

                    // Futility pruning: parent node
                    // (*Scaler): Generally, more frequent futility pruning
                    // scales well with respect to time and threads
                    if (!ss->inCheck && lmrDepth < 11 && futilityValue <= alpha)
                    {
                        if (bestValue <= futilityValue && !is_decisive(bestValue)
                            && !is_win(futilityValue))
                            bestValue = futilityValue;
                        continue;
                    }

                    lmrDepth = std::max(lmrDepth, 0);

                    // Prune moves with negative SEE
                    if (!pos.see_ge(move, -26 * lmrDepth * lmrDepth))
                        continue;
                }
            }
        }

        // Step 15. Extensions
		...
        if (!rootNode && move == ttData.move && !excludedMove
            && depth >= 6 - (completedDepth > 26) + ss->ttPv && is_valid(ttData.value)
            && !is_decisive(ttData.value) && (ttData.bound & BOUND_LOWER)
            && ttData.depth >= depth - 3)
        {
            // Shashin crystal adjustments begin
            const bool isPv          = PvNode;
            const bool isTtPv        = ss->ttPv;
            const int  marginBase    = 56 + 79 * (isTtPv && !isPv);
            Value      singularBeta  = ttData.value - marginBase * depth / 58;
            Depth      singularDepth = newDepth / 2;

            // Precise position style identification
            bool isPotentiallyPetrosian = (depth >= 6 || staticState.stmKingExposed);
            bool isPotentiallyTal       = (depth >= 6 || staticState.opponentKingExposed);
            // Shashin crystal adjustments end

            ss->excludedMove = move;
            value = search<NonPV>(pos, ss, singularBeta - 1, singularBeta, singularDepth, cutNode);
            ss->excludedMove = Move::none();

            if (value < singularBeta)
            {
                int corrValAdj   = std::abs(correctionValue) / 249096;
                int doubleMargin = 4 + 205 * PvNode - 223 * !ttCapture - corrValAdj
                                 - 959 * ttMoveHistory / 131072 - (ss->ply > rootDepth) * 45;
                int tripleMargin = 80 + 276 * PvNode - 249 * !ttCapture + 86 * ss->ttPv - corrValAdj
                                 - (ss->ply * 2 > rootDepth * 3) * 53;
                // Dynamic adjustment based on Shashin intensity
                if (isTal && isPotentiallyTal)
                {
                    // Tal: higher intensity = more reduction
                    double talIntensity = ShashinManager::getTalIntensity(style);
                    doubleMargin -= static_cast<int>(55 * talIntensity);
                    tripleMargin -= static_cast<int>(75 * talIntensity);
                }
                else if (isPetrosian && isPotentiallyPetrosian)
                {
                    // Petrosian: higher intensity = less reduction
                    double petrosianIntensity = ShashinManager::getPetrosianIntensity(style);
                    doubleMargin -= static_cast<int>(55 * (1.0 - petrosianIntensity));
                    tripleMargin -= static_cast<int>(75 * (1.0 - petrosianIntensity));
                }

                if (isCapablanca)
                {
                    // Pure positional strategy
                    doubleMargin += 45;
                    tripleMargin += 65;
                }
                // Hybrid styles (should not occur but maintained as fallback)
                else
                {
                    // Hybrid positions with dynamic weighting
                    bool isAttackingHybrid =
                      (style == ShashinPosition::CAPABLANCA_TAL
                       || (style == ShashinPosition::TAL_CAPABLANCA_PETROSIAN));
                    bool isDefensiveHybrid =
                      (style == ShashinPosition::CAPABLANCA_PETROSIAN
                       || (style == ShashinPosition::TAL_CAPABLANCA_PETROSIAN));
                    if (isAttackingHybrid)
                    {
                        // Hybrid with tactical emphasis
                        doubleMargin -= isPotentiallyTal ? 32 : 18;
                        tripleMargin -= isPotentiallyTal ? 48 : 28;
                    }
                    else if (isDefensiveHybrid)
                    {
                        // Hybrid with strategic emphasis
                        doubleMargin += isPotentiallyPetrosian ? 15 : 38;
                        tripleMargin += isPotentiallyPetrosian ? 22 : 56;
                    }
                }
                extension =
                  1 + (value < singularBeta - doubleMargin) + (value < singularBeta - tripleMargin);

                depth++;
            }

            // Multi-cut pruning
			...            
			else if (value >= beta && !is_decisive(value))
            {
                return value;
            }
            // Negative extensions
			...            
			else if (ttData.value >= beta)
            {
                extension = -3;
            }
            else if (cutNode)
            {
                extension = -2;
            }
        }
        // Shashin-Crystal end
        // Step 16. Make the move
        do_move(pos, move, st, givesCheck);
        // Add extension to new depth
        newDepth += extension;
		...
        // Decrease reduction for PvNodes (*Scaler)
        if (ss->ttPv)
            r -= 2510 + PvNode * 963 + (ttData.value > alpha) * 916
               + (ttData.depth >= depth) * (943 + cutNode * 1180);
        // Shashin style based adjustements
        // Shashin logic here
        bool isShashinTactical = localShashinManager.isTal() || isTacticalReactive;
        // Tactical style: less reduction to preserve depth
        if (isShashinTactical && depth > 8)
        {
            r -= 512;  // Reduction by 0.5 plies
        }
        // Strategical style: more reduction in quiet phases
        else if (isStrategical && !ss->inCheck && depth > 6)
        {
            r += 256;  // Added reduction by 0.25 plies
        }
        // These reduction adjustments have no proven non-linear scaling
		...
        r -= ss->statScore * 826 / 8192;
        // Step 17. Late moves reduction / extension (LMR)
        //from shashin crystal begin
        if (depth >= 2 && moveCount > std::max(sibs, static_cast<int>(1)) && doLMRStep)  //learning
        {
            bool isCrystal       = localShashinManager.useStep17CrystalLogic();
            bool applyCrystalLMR = false;
            bool allowExt        = false;
            bool kingDangerThem  = false;
            bool lmrCapture      = false;
            if (isCrystal)
            {
                kingDangerThem  = ourMove && king_danger(pos, ~us);
                lmrCapture      = cutNode && (ss - 1)->moveCount > 1;
                allowExt        = (ss->ply + depth < MAX_PLY - 2) && (ss->ply < 2 * rootDepth);
                bool allowLMR   = depth > 1 && !gameCycle;
                allowLMR        = allowLMR && (!kingDangerThem || ss->ply > 6);
                allowLMR        = allowLMR && (!PvNode || ss->ply > 1);
                applyCrystalLMR = allowLMR && (!capture || lmrCapture) && moveCount > 3;
            }
            // In general we want to cap the LMR depth search at newDepth, but when
			...
            if (isCrystal && localShashinManager.isStrategical() && !capture && moveCount > 6)
                d = std::max(1, d - 1);

            ss->reduction = newDepth - d;
			...            
			if (value > alpha)
            {
                // Adjust full-depth search based on LMR results - if the result was
                // good enough search deeper, if it was bad enough search shallower.
                const bool doDeeperSearch    = value > (bestValue + 43 + 2 * newDepth);
                const bool doShallowerSearch = value < bestValue + 9;

                if (applyCrystalLMR && doDeeperSearch && !isStrategical)
                {

                    Depth extDepth = newDepth + 1;

                    if (localShashinManager.isTactical() && value > alpha + 150)
                        extDepth += allowExt ? 1 : 0;

                    value = -search<NonPV>(pos, ss + 1, -(alpha + 1), -alpha, extDepth, !cutNode);
                }
                else
                {
                    newDepth += doDeeperSearch - doShallowerSearch;

                    if (newDepth > d)
                        value =
                          -search<NonPV>(pos, ss + 1, -(alpha + 1), -alpha, newDepth, !cutNode);
                }

                // Differianted bonus history by Shashin
                int bonus = applyCrystalLMR ? 1712 : 1412;
                update_continuation_histories(ss, movedPiece, move.to_sq(), bonus);
            }
        }
        //from shashin crystal end

        // Step 18. Full-depth search when LMR is skipped
				...

        // Step 19. Undo move
		...
        // Step 20. Check for a new best move
		...        
		if (threads.stop.load(std::memory_order_relaxed))
            return VALUE_ZERO;

        if (rootNode)
        {
			...
        }
		...

        if (value + inc > bestValue)
        {
            bestValue = value;

            if (value + inc > alpha)
            {
                if (value >= beta)
                {
                    // (* Scaler) Especially if they make cutoffCnt increment more often.
                    ss->cutoffCnt += (extension < 2) || PvNode;
                    assert(value >= beta);  // Fail high
                    break;
                }

                //from crystal-shashin begin
                // Reduce other moves if we have found at least one score improvement
                if (depth > 2 && depth < 16 && !is_decisive(value))
                {
                    const int baseReduction = 2;
                    int reduction = baseReduction;
                    
                    // Specific tactical conditions for style (Shashin-correct)
                    const bool criticalTactical = (staticState.opponentKingExposed && isTal)
                                               || (staticState.stmKingExposed && isPetrosian);

                    // Hybrid Shashin Strategy
                    if (criticalTactical)
                    {
                        // Depth preserved in critical positions for style
                        reduction = 1;
                    }
                    else if (isStrategical && !staticState.stmKingExposed
                             && !staticState.opponentKingExposed)
                    {
                        // Positional scenario: more aggressive reduction
                        reduction = (depth > 8) ? 3 : 2;

                        // Extra caution in congested positions
                        if (staticState.allPiecesCount > 20)
                        {
                            reduction = std::min(reduction, 2);
                        }
                    }
                    else if (gameCycle)
                    {
                        // Cycle management: balancing safety and progress
                        reduction = (pos.rule50_count() > 20) ? 3 : 1;
                    }

                    // Adaptive safety clamp
                    reduction = std::clamp(reduction, 1, depth - 1);

                    depth -= reduction;
                }
                //from crystal-shashin end

                assert(depth > 0);
                alpha = value;  // Update alpha! Always alpha < beta
            }
        }
		...
    }

    // Step 21. Check for mate and stalemate
	...
    return bestValue;
}


// Quiescence search function, which is called by the main search function with
...
template<NodeType nodeType>
Value Search::Worker::qsearch(Position& pos, Stack* ss, Value alpha, Value beta) {

    static_assert(nodeType != Root);
    const RootShashinState& rootShashinState = shashinManager->getState();
    constexpr bool          PvNode           = nodeType == PV;

    assert(alpha >= -VALUE_INFINITE && alpha < beta && beta <= VALUE_INFINITE);
	...
    Key   posKey;
    Move  move, bestMove, expTTMove = Move::none();                               //learning
    Value bestValue, value, futilityBase, expTTValue = VALUE_NONE;                //learning
    bool  pvHit, givesCheck, capture, expTTHit = false, updatedLearning = false;  //learning
    int   moveCount;
	...
    // Step 3. Transposition table lookup
    posKey                         = pos.key();
	...    
	pvHit        = ttHit && ttData.is_pv;
    // Crystal-Shashin QSearch optimization begin
    const auto& staticState    = rootShashinState.staticState;
    const auto& dynamicDerived = rootShashinState.dynamicDerived;
    const bool  canUseTTValue  = !PvNode && is_valid(ttData.value)
                            && (ttData.bound & (ttData.value >= beta ? BOUND_LOWER : BOUND_UPPER));

    if (canUseTTValue)
    {
        if (ttData.depth >= DEPTH_QS)
            return ttData.value;
        if (staticState.legalMoveCount < 35 && std::abs(ttData.value) > 60)
        {
            const bool areQuietKings =
              !staticState.stmKingExposed && !staticState.opponentKingExposed;
            if (!dynamicDerived.isStrategical && areQuietKings)
            {
                const bool depthCondition =
                  ttData.depth >= ADJUSTED_QS_DEPTH && ttData.depth > MIN_DEPTH_FOR_QS_CUTOFF;
                const bool kingSafety =
                  (!staticState.stmKingExposed && !staticState.opponentKingExposed)
                  || ttData.depth > DEPTH_QS + 4;
                if (depthCondition && kingSafety)
                    return ttData.value;
            }
        }
    }
    // Crystal-Shashin QSearch optimization end

    // Step 4. Static evaluation of the position
	...
    // Step 5. Loop through all pseudo-legal moves until no moves remain or a beta
    // cutoff occurs.
    while ((move = mp.next_move()) != Move::none())
    {
		...
        // Step 6. Pruning
        if (!is_loss(bestValue))
        {
            // Futility pruning and moveCount pruning
            if (!givesCheck && move.to_sq() != prevSq && !is_loss(futilityBase)
                && move.type_of() != PROMOTION)
            {
                if (moveCount
                    > 2 + ((!(dynamicDerived.isHighTal)) ? 0 : PvNode))  //from Crystal-shashin
                    continue;
		...
        // Step 7. Make and search the move
		...
        // Step 8. Check for a new best move
		...    
	}
    // Step 9. Check for mate
	...
    return bestValue;
}

...
}  // namespace ShashChess