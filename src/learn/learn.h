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

#include <string>
#include <unordered_map>
#include <vector>
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
    bool         isSuspended;
    LearningMode learningMode;
    // [SC:learning] The root position of the search that found the game decided (see pause())
    ShashChess::Key pausedKey;

    std::unordered_multimap<ShashChess::Key, LearningMove*> HT;
    std::vector<void*>                                      mainDataBuffers;
    std::vector<void*>                                      newMovesDataBuffers;
    bool                                                    load(const std::string& filename);
    bool insert_or_update(PersistedLearningMove* plm, bool qLearning);

   public:
    LearningData();
    ~LearningData();

    // [SC:learning] Learning stops when the game is decided, but only for that game: the
    // experience is saved and nothing more is learned while the engine is given positions
    // that continue it. A 'position' command that does not continue it resumes learning
    // (Engine::set_position), as 'ucinewgame' does. Before, only 'ucinewgame' resumed it, and
    // the console and the GUIs in analysis mode never send it: after a single decided position
    // (a mate, more than 5 pawns, fewer than 5 pieces, move > 75) nothing more was learned.
    void               pause(ShashChess::Key decidedKey);
    void               resume();
    [[nodiscard]] bool is_paused() const { return isPaused; };
    // True if 'key' is the position where the paused game was found decided: a 'position'
    // command whose history contains it continues that game
    [[nodiscard]] bool is_decided_position(ShashChess::Key key) const {
        return isPaused && key == pausedKey;
    }

    // [SC:learning] bench and speedtest: learning works as if it were Off (nothing learned,
    // paused, saved or probed), and then everything is as before
    void set_suspended(bool s) { isSuspended = s; }

    void quick_reset_exp();
    void set_learning_mode(ShashChess::OptionsMap& options, const std::string& lm);
    [[nodiscard]] LearningMode learning_mode() const;
    [[nodiscard]] bool         is_enabled() const {
        return learningMode != LearningMode::Off && !isSuspended;
    }

    void               set_readonly(bool ro) { isReadOnly = ro; }
    [[nodiscard]] bool is_readonly() const { return isReadOnly; }

    void clear();
    void init(ShashChess::OptionsMap& o);
    // [SC:learning] True if the experience file is complete on disk
    bool persist(const ShashChess::OptionsMap& o);

    void add_new_learning(ShashChess::Key key, const LearningMove& lm);

    int probeByMaxDepthAndScore(ShashChess::Key key, const LearningMove*& learningMove);
    const LearningMove*        probe_move(ShashChess::Key key, ShashChess::Move move);
    std::vector<LearningMove*> probe(ShashChess::Key key);
    static void                sortLearningMoves(std::vector<LearningMove*>& learningMoves);
    static void                show_exp(const ShashChess::Position& pos);
    // [SC:learning] GUI-readable variant ("info string" lines) for the "Show Experience" button
    static void show_exp_info(const ShashChess::Position& pos, bool experienceBook);
};

extern LearningData LD;

#endif  // #ifndef LEARN_H_INCLUDED
