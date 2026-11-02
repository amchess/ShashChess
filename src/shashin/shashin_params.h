/*
  ShashChess, a UCI chess playing engine derived from Stockfish
  Copyright (C) 2004-2026 The Stockfish developers (see AUTHORS file)
  Copyright (C) 2018-2026 The ShashChess developers (see AUTHORS file)

  ShashChess is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  ShashChess is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

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