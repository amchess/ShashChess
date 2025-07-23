#include "moveconfig.h"

namespace ShashChess {
namespace MoveConfig {

// Definizione delle variabili thread-local
thread_local bool useMoveShashinLogic = false;
thread_local bool isStrategical       = false;
thread_local bool isAggressive        = false;
thread_local bool isFortress          = false;

}  // namespace MoveConfig
}  // namespace ShashChess