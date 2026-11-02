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

#ifndef LEARN_H_INCLUDED
#define LEARN_H_INCLUDED

#include <unordered_map>
#include "../types.h"
#include "../ucioption.h"
#include "../position.h"

enum class LearningMode {
    Off      = 1,
    Standard = 2,
    Self     = 3,
};

struct LearningMove {
    ShashChess::Depth depth       = 0;
    ShashChess::Value score       = ShashChess::VALUE_NONE;
    ShashChess::Move  move        = ShashChess::Move::none();
    int               performance = 100;
};

struct PersistedLearningMove {
    ShashChess::Key key{};
    LearningMove    learningMove;
};

struct QLearningMove {
    PersistedLearningMove persistedLearningMove;
    int                   materialClamp;
};

class LearningData {
    bool         isPaused;
    bool         isReadOnly;
    bool         needPersisting;
    LearningMode learningMode;

    std::unordered_multimap<ShashChess::Key, LearningMove*> HT;
    std::vector<void*>                                      mainDataBuffers;
    std::vector<void*>                                      newMovesDataBuffers;
    bool                                                    load(const std::string& filename);
    void insert_or_update(PersistedLearningMove* plm, bool qLearning);

   public:
    LearningData();
    ~LearningData();

    void               pause();
    void               resume();
    [[nodiscard]] bool is_paused() const { return isPaused; };

    void quick_reset_exp();
    void set_learning_mode(ShashChess::OptionsMap& options, const std::string& lm);
    [[nodiscard]] LearningMode learning_mode() const;
    [[nodiscard]] bool         is_enabled() const { return learningMode != LearningMode::Off; }

    void               set_readonly(bool ro) { isReadOnly = ro; }
    [[nodiscard]] bool is_readonly() const { return isReadOnly; }

    void clear();
    void init(ShashChess::OptionsMap& o);
    void persist(const ShashChess::OptionsMap& o);

    void add_new_learning(ShashChess::Key key, const LearningMove& lm);

    int probeByMaxDepthAndScore(ShashChess::Key key, const LearningMove*& learningMove);
    const LearningMove*        probe_move(ShashChess::Key key, ShashChess::Move move);
    std::vector<LearningMove*> probe(ShashChess::Key key);
    static void                sortLearningMoves(std::vector<LearningMove*>& learningMoves);
    static void                show_exp(const ShashChess::Position& pos);
};

extern LearningData LD;

#endif  // #ifndef LEARN_H_INCLUDED
