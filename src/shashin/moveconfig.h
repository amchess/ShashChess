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

#ifndef MOVE_CONFIG_H
#define MOVE_CONFIG_H

namespace ShashChess {
namespace MoveConfig {

// Dichiarazione come variabili thread-local
// [SC 43-dev-C] One flag per side: [0] side to move at the root, [1] its opponent
extern thread_local bool useMoveShashinLogic[2];
extern thread_local bool isStrategical[2];
extern thread_local bool isAggressive[2];
extern thread_local bool isFortress;
// [SC] Tal, Petrosian or king danger: enables the "tactical phase" ordering bonus
extern thread_local bool isTactical[2];
// [SC 43-dev-C] Colour (0 white, 1 black) of the side to move at the root
extern thread_local int rootSide;

}  // namespace MoveConfig
}  // namespace ShashChess

#endif  // MOVE_CONFIG_H
