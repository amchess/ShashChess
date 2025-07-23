#ifndef SHASHIN_PARAMS_H
#define SHASHIN_PARAMS_H

namespace ShashChess {

struct ShashinParams {
    size_t highMobilityMoves = 30;
    size_t midMobilityMoves  = 20;
    size_t lowMobilityMoves  = 15;

    int highDepthLimit     = 12;
    int midDepthLimit      = 8;
    int lowDepthLimit      = 5;
    int advancedDepthLimit = 20;

    int minPlyFortress      = 28;
    int minDefenderPieces   = 2;
    int maxAttackerMaterial = 2500;
    int maxTotalPieces      = 14;

    int attackerPenalty                 = 12;
    int shieldBonus                     = 15;
    int centerBonus                     = 8;
    int avoidStep10PassiveMoveThreshold = 20;
};

}  // namespace ShashChess

#endif  // SHASHIN_PARAMS_H