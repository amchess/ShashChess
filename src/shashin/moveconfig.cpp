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

#include "moveconfig.h"

namespace ShashChess {
namespace MoveConfig {

// Definizione delle variabili thread-local
thread_local bool useMoveShashinLogic = false;
thread_local bool isStrategical       = false;
thread_local bool isAggressive        = false;
thread_local bool isFortress          = false;
thread_local bool isTactical          = false;

}  // namespace MoveConfig
}  // namespace ShashChess