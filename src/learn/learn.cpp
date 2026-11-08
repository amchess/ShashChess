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

#include <iostream>
#include <fstream>
#include <sstream>
#include "../misc.h"
#include "learn.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include "../movegen.h"
#include "../score.h"
#include "../uci.h"

using namespace std;
using namespace ShashChess;

LearningData LD;

namespace {
// [SC:learning] Case insensitive, as the check of the combo values (Option::operator=): before,
// "setoption name Persisted learning value standard" was accepted and then taken for Self
bool is_mode(const string& lm, const char* mode) {
    return !CaseInsensitiveLess()(lm, mode) && !CaseInsensitiveLess()(mode, lm);
}

LearningMode identify_learning_mode(const string& lm) {
    if (is_mode(lm, "Standard"))
        return LearningMode::Standard;

    if (is_mode(lm, "Self"))
        return LearningMode::Self;

    return LearningMode::Off;
}
}

bool LearningData::load(const string& filename) {
    ifstream in(filename, ios::in | ios::binary);

    //Quick exit if file is not present
    if (!in.is_open())
        return false;

    //Get experience file size
    in.seekg(0, ios::end);
    const size_t fileSize = in.tellg();

    //File size should be a multiple of 'PersistedLearningMove'
    if (fileSize % sizeof(PersistedLearningMove))
    {
        cerr << "info string The file <" << filename << "> with size <" << fileSize
             << "> is not a valid experience file" << endl;
        return false;
    }

    //An empty file is a valid experience file without moves (persist() writes it when no move
    //was searched): nothing to read. The loop below would otherwise read past the buffer
    if (fileSize == 0)
        return true;

    //Allocate buffer to read the entire file
    void* fileData = malloc(fileSize);
    if (!fileData)
    {
        cerr << "info string Failed to allocate <" << fileSize
             << "> bytes to read experience file <" << filename << ">" << endl;
        return false;
    }

    //Read the entire file
    in.seekg(0, ios::beg);  //Move read pointer to the beginning of the file
    in.read(static_cast<char*>(fileData), static_cast<std::streamsize>(fileSize));
    if (!in)
    {
        free(fileData);

        cerr << "info string Failed to read <" << fileSize << "> bytes from experience file <"
             << filename << ">" << endl;
        return false;
    }

    //Close the input data file
    in.close();

    //Save pointer to fileData to be freed later
    mainDataBuffers.push_back(fileData);

    //Loop the moves from this file
    const bool qLearning             = learningMode == LearningMode::Self;
    auto*      persistedLearningMove = static_cast<PersistedLearningMove*>(fileData);
    do
    {
        insert_or_update(persistedLearningMove, qLearning);
        ++persistedLearningMove;
    } while (reinterpret_cast<size_t>(persistedLearningMove)
             < reinterpret_cast<size_t>(fileData) + fileSize);

    return true;
}

inline bool should_update(const LearningMove existing_move, const LearningMove learning_move) {
    if (learning_move.depth > existing_move.depth)
    {
        return true;
    }

    if (learning_move.depth < existing_move.depth)
    {
        return false;
    }

    if (learning_move.score != existing_move.score)
    {
        return true;
    }

    return learning_move.performance != existing_move.performance;
}

// Returns true if the hash table keeps a pointer to plm->learningMove, false if plm was only
// used to update a move already in the table (and then the caller may free plm)
bool LearningData::insert_or_update(PersistedLearningMove* plm, bool qLearning) {
    // We search in the range of all the hash table entries with plm key
    const auto [first, second] = HT.equal_range(plm->key);

    //If the plm key belongs to a position that did not exist before in the 'LHT'
    //then, we insert this new key and LearningMove and return
    if (first == second)
    {
        //Insert new key and learningMove
        HT.insert({plm->key, &plm->learningMove});

        //Flag for persisting
        needPersisting = true;

        //Nothing else to do
        return true;
    }

    //The plm key belongs to a position already existing in the LHT
    //Check if this move already exists for this position
    const auto itr = find_if(
      first, second, [&plm](const auto& p) { return p.second->move == plm->learningMove.move; });

    //If the move does not exist then insert it
    LearningMove* bestNewMoveCandidate = nullptr;
    bool          inserted             = false;
    if (itr == second)
    {
        HT.insert({plm->key, &plm->learningMove});
        bestNewMoveCandidate = &plm->learningMove;
        inserted             = true;

        //Flag for persisting
        needPersisting = true;
    }
    else  //If the move exists, check if it better than the move we already have
    {
        LearningMove* existingMove = itr->second;
        if (should_update(*existingMove, plm->learningMove))
        {
            //Replace the existing move
            *existingMove = plm->learningMove;

            //Since an existing move was replaced, check the best move again
            bestNewMoveCandidate = existingMove;

            //Flag for persisting
            needPersisting = true;
        }
    }

    //Do we have a candidate for new best move?
    if (bestNewMoveCandidate != nullptr)
    {
        bool          newBestMove     = false;
        LearningMove* currentBestMove = first->second;
        if (bestNewMoveCandidate != currentBestMove)
        {
            if (qLearning)
            {
                if (bestNewMoveCandidate->score > currentBestMove->score)
                {
                    newBestMove = true;
                }
            }
            else
            {
                if ((currentBestMove->depth < bestNewMoveCandidate->depth)
                    || (currentBestMove->depth == bestNewMoveCandidate->depth
                        && currentBestMove->score <= bestNewMoveCandidate->score))
                {
                    newBestMove = true;
                }
            }
        }

        if (newBestMove)
        {
            //Boring and slow, but I can't think of an alternative at the moment
            //This is not thread safe, but it is fine since this code will never be called from multiple threads
            static LearningMove lm;

            lm                    = *bestNewMoveCandidate;
            *bestNewMoveCandidate = *currentBestMove;
            *currentBestMove      = lm;

            //Flag for persisting
            needPersisting = true;
        }
    }

    return inserted;
}

LearningData::LearningData() :
    isPaused(false),
    isReadOnly(false),
    needPersisting(false),
    isSuspended(false),
    learningMode(LearningMode::Off),
    pausedKey(0) {}

LearningData::~LearningData() { clear(); }

void LearningData::clear() {
    //Clear hash table
    HT.clear();

    //Release internal data buffers
    for (void* p : mainDataBuffers)
        free(p);

    //Clear internal data buffers
    mainDataBuffers.clear();

    //Release internal new moves data buffers
    for (void* p : newMovesDataBuffers)
        free(p);

    //Clear internal new moves data buffers
    newMovesDataBuffers.clear();

    //Nothing left to persist
    needPersisting = false;
}

void LearningData::init(ShashChess::OptionsMap& o) {
    OptionsMap& options = o;

    //init() is also called when Persisted learning or Experience Book change: save what was
    //learned since the last save, otherwise clear() would discard it
    if (needPersisting && !isReadOnly)
        persist(options);

    clear();

    // [SC:learning] A new learning mode (or the experience reloaded) starts afresh: before, a
    // pause stayed even switching Persisted learning Off and on again
    resume();

    learningMode = identify_learning_mode(options["Persisted learning"]);
    if ((learningMode == LearningMode::Off) && !((bool) options["Experience Book"]))
        return;

    load(Util::map_path("experience.exp"));

    vector<string> slaveFiles;

    //Just in case, check and load for "experience_new.exp" which will be present if
    //previous saving operation failed (engine crashed or terminated)
    //The same path that persist() writes: the engine folder, not the working directory
    string slaveFile = Util::map_path("experience_new.exp");
    if (load(slaveFile))
        slaveFiles.push_back(slaveFile);

    //Load slave experience files (if any)

    int i = 0;
    while (true)
    {
        slaveFile = Util::map_path("experience" + to_string(i) + ".exp");
        if (const bool loaded = load(slaveFile); !loaded)
            break;

        slaveFiles.push_back(slaveFile);
        ++i;
    }

    //We need to write all consolidated experience to disk
    //[SC:learning] and only then remove the slave files: before, they were removed also when
    //the save failed (for instance the rename, on Windows when an antivirus holds the new
    //file): experience_new.exp, then the only copy of the experience, was deleted. And not in
    //read-only mode, where nothing is written (before, there the slave files were just deleted)
    if (!slaveFiles.empty() && !isReadOnly && persist(options))
    {
        //Remove slave files
        for (const string& fn : slaveFiles)
            remove(fn.c_str());
    }

    // Clear the 'needPersisting' flag
    needPersisting = false;
}

void LearningData::quick_reset_exp() {
    const std::string experienceFilename = Util::map_path("experience.exp");
    std::cout << "Loading experience file: " << experienceFilename << std::endl;

    std::ifstream file(experienceFilename, std::ifstream::binary | std::ifstream::ate);
    if (!file)
    {
        std::cerr << "Failed to load experience file" << std::endl;
        return;
    }

    const std::streamsize  file_size     = file.tellg();
    constexpr unsigned int entry_size    = 24;
    const unsigned int     total_entries = file_size / entry_size;

    file.close();

    std::cout << "Total entries in the file: " << total_entries << std::endl;

    if (const auto check = load(experienceFilename); !check)
    {
        std::cerr << "Failed to load experience file" << std::endl;
        return;
    }

    std::cout << "Successfully loaded experience file" << std::endl;

    int entry_count = 0;
    for (auto& [key, learning_move] : HT)
    {
        entry_count++;

        const auto old_performance = learning_move->performance;
        const auto new_performance =
          WDLModel::get_win_probability(learning_move->score, learning_move->depth);

        std::cout << "Updating entry " << entry_count << "/" << total_entries << " Key " << key
                  << "Value " << learning_move->score << "Depth " << learning_move->depth
                  << ": old performance=" << static_cast<int>(old_performance)
                  << ", new performance=" << static_cast<int>(new_performance) << std::endl;

        learning_move->performance = new_performance;
    }

    needPersisting = true;
    std::cout << "Finished updating performances. Total processed entries: " << entry_count
              << std::endl;
}


void LearningData::set_learning_mode(ShashChess::OptionsMap& options, const string& lm) {
    LearningMode newLearningMode = identify_learning_mode(lm);
    if (newLearningMode == learningMode)
        return;

    init(options);
}

LearningMode LearningData::learning_mode() const { return learningMode; }

bool LearningData::persist(const ShashChess::OptionsMap& o) {
    const OptionsMap& options = o;
    //Quick exit if we have nothing to persist
    if (HT.empty() || !needPersisting)
        return true;

    if (isReadOnly)
    {
        //We should not be here if we are running in ReadOnly mode
        assert(false);
        return false;
    }

    /*
        To avoid any problems when saving to experience file, we will actually do the following:
        1) Save new experience to "experience_new.exp"
        2) Remove "experience.exp"
        3) Rename "experience_new.exp" to "experience.exp"

        This approach is fail proof so that the old file is only removed when the new file is successfully saved!
        If, for whatever odd reason, the engine is able to execute step (1) and (2) and fails to execute step (3)
        i.e., we end up with experience0.exp then it is not a problem since the file will be loaded anyway the next
        time the engine starts!
    */

    string experienceFilename;
    string tempExperienceFilename;

    if (static_cast<bool>(options["Concurrent Experience"]))
    {
        static string uniqueStr;

        if (uniqueStr.empty())
        {
            PRNG prng(now());

            stringstream ss;
            ss << hex << prng.rand<uint64_t>();

            uniqueStr = ss.str();
        }

        experienceFilename     = Util::map_path("experience-" + uniqueStr + ".exp");
        tempExperienceFilename = Util::map_path("experience_new-" + uniqueStr + ".exp");
    }
    else
    {
        experienceFilename     = Util::map_path("experience.exp");
        tempExperienceFilename = Util::map_path("experience_new.exp");
    }

    ofstream outputFile(tempExperienceFilename, ofstream::trunc | ofstream::binary);
    if (!outputFile.is_open())
    {
        sync_cout << "info string Cannot create <" << tempExperienceFilename
                  << ">: the experience is not saved" << sync_endl;
        return false;
    }

    //Zeroed once and then filled field by field: the padding bytes written to the file are
    //always 0 (before, they were whatever was in memory)
    PersistedLearningMove persistedLearningMove;
    std::memset(static_cast<void*>(&persistedLearningMove), 0, sizeof(persistedLearningMove));
    for (auto& kvp : HT)
    {
        const LearningMove& learningMove = *kvp.second;
        if (learningMove.depth != 0)
        {
            persistedLearningMove.key                      = kvp.first;
            persistedLearningMove.learningMove.depth       = learningMove.depth;
            persistedLearningMove.learningMove.score       = learningMove.score;
            persistedLearningMove.learningMove.move        = learningMove.move;
            persistedLearningMove.learningMove.performance = learningMove.performance;
            outputFile.write(reinterpret_cast<char*>(&persistedLearningMove),
                             sizeof(persistedLearningMove));
        }
    }
    outputFile.close();

    //Replace the experience file only with a complete new one: if a write failed (disk full,
    //I/O error) the old file must stay where it is
    if (outputFile.fail())
    {
        remove(tempExperienceFilename.c_str());
        sync_cout << "info string Cannot write <" << tempExperienceFilename
                  << ">: the experience is not saved, <" << experienceFilename << "> is unchanged"
                  << sync_endl;
        return false;
    }

    remove(experienceFilename.c_str());
    const bool renamed = rename(tempExperienceFilename.c_str(), experienceFilename.c_str()) == 0;
    if (!renamed)
        sync_cout << "info string Cannot rename <" << tempExperienceFilename << "> to <"
                  << experienceFilename << ">: the experience is saved in <"
                  << tempExperienceFilename << ">" << sync_endl;

    //Prevent persisting again without modifications (if the rename failed, the next start
    //loads the temporary file as a slave file)
    needPersisting = false;

    return renamed;
}

void LearningData::pause(Key decidedKey) {
    isPaused  = true;
    pausedKey = decidedKey;
}

void LearningData::resume() { isPaused = false; }

void LearningData::add_new_learning(Key key, const LearningMove& lm) {
    //Allocate buffer to read the entire file
    auto* newPlm = static_cast<PersistedLearningMove*>(malloc(sizeof(PersistedLearningMove)));
    if (!newPlm)
    {
        cerr << "info string Failed to allocate <" << sizeof(PersistedLearningMove)
             << "> bytes for new learning entry" << endl;
        return;
    }

    //Save pointer to fileData to be freed later
    newMovesDataBuffers.push_back(newPlm);

    //Assign
    newPlm->key          = key;
    newPlm->learningMove = lm;

    //Add to HT. If the move was already there, it has only been updated with newPlm: free the
    //buffer at once, otherwise each 'position ... moves' of a game would allocate again all
    //its moves until the engine is closed
    if (!insert_or_update(newPlm, learningMode == LearningMode::Self))
    {
        newMovesDataBuffers.pop_back();
        free(newPlm);
    }
}

int LearningData::probeByMaxDepthAndScore(Key key, const LearningMove*& learningMove) {
    const LearningMove* maxDepthMove = nullptr;
    int                 maxDepth     = -1;
    int                 maxScore     = -1;

    // Iterate through the range of elements with the given key
    auto range = HT.equal_range(key);
    if (range.first == range.second)
    {
        learningMove = nullptr;
        return 0;
    }
    const auto siblings = distance(range.first, range.second);
    for (auto it = range.first; it != range.second; ++it)
    {
        LearningMove* move = it->second;

        // Check if the current move has a greater depth than the maximum depth found so far
        if (move->depth > maxDepth)
        {
            maxDepth     = move->depth;
            maxScore     = move->score;
            maxDepthMove = move;
        }
        // If the current move has the same depth as the maximum depth found so far,
        // check if it has a greater score
        else if (move->depth == maxDepth && move->score > maxScore)
        {
            maxScore     = move->score;
            maxDepthMove = move;
        }
    }

    // Return the reference to the LearningMove with the maximum depth and score (or nullptr if not found)
    learningMove = maxDepthMove;

    return static_cast<int>(siblings);
}

const LearningMove* LearningData::probe_move(Key key, Move move) {
    auto range = HT.equal_range(key);

    if (range.first == range.second)
        return nullptr;

    auto itr =
      find_if(range.first, range.second, [&move](const auto& p) { return p.second->move == move; });

    if (itr == range.second)
        return nullptr;

    return itr->second;
}


void LearningData::sortLearningMoves(std::vector<LearningMove*>& learningMoves) {
    std::sort(learningMoves.begin(), learningMoves.end(),
              [](const LearningMove* a, const LearningMove* b) {
                  if (a->depth != b->depth)
                  {
                      return a->depth > b->depth;
                  }
                  const int winProbA = a->performance;
                  const int winProbB = b->performance;

                  if (winProbA != winProbB)
                  {
                      return winProbA > winProbB;
                  }
                  return a->score > b->score;
              });
}
vector<LearningMove*> LearningData::probe(ShashChess::Key key) {
    vector<LearningMove*> result;
    auto                  range = HT.equal_range(key);
    for (auto it = range.first; it != range.second; ++it)
    {
        result.push_back(it->second);
    }

    return result;
}
// [SC:learning] "Show Experience" button (proposed by ALF): the same data as show_exp(), but
// as "info string" lines, so that a GUI shows it (Arena, Shredder; the ChessBase GUIs only
// write them in the Debug Log File). Moves are listed in the order the Experience Book picks by:
// depth, then win probability, then score
void LearningData::show_exp_info(const Position& pos, bool experienceBook) {
    const string experienceFilename = Util::map_path("experience.exp");
    const bool   found              = ifstream(experienceFilename, ios::in | ios::binary).is_open();
    sync_cout << "info string Experience file: " << experienceFilename
              << (found ? "" : " (not found)") << ", " << LD.HT.size() << " entries in memory"
              << sync_endl;
    sync_cout << "info string Experience for the current position: " << pos.fen() << sync_endl;

    if (!LD.is_enabled())
    {
        if (!experienceBook)
        {
            sync_cout << "info string Persisted learning and Experience Book are Off: the "
                         "experience file is not loaded"
                      << sync_endl;
            return;
        }

        sync_cout << "info string Persisted learning is Off: the table is only used by the "
                     "Experience Book (nothing is learned or written)"
                  << sync_endl;
    }
    else if (LD.is_paused())
        sync_cout << "info string Learning is paused: the game was found decided and its "
                     "experience saved. It resumes with ucinewgame or a position that does "
                     "not continue that game"
                  << sync_endl;

    // A different position may have the same key: only the moves legal here
    vector<LearningMove*> learningMoves = LD.probe(pos.key());
    const MoveList<LEGAL> legalMoves(pos);
    learningMoves.erase(std::remove_if(learningMoves.begin(), learningMoves.end(),
                                       [&legalMoves](const LearningMove* lm) {
                                           return !legalMoves.contains(lm->move);
                                       }),
                        learningMoves.end());

    if (learningMoves.empty())
    {
        sync_cout << "info string   no experience data for this position" << sync_endl;
        return;
    }

    sortLearningMoves(learningMoves);
    for (const LearningMove* lm : learningMoves)
    {
        sync_cout << "info string   " << UCIEngine::move(lm->move, pos.is_chess960());

        // A move of the game in progress, stored without a search: it is never saved
        if (lm->depth <= 0)
            cout << " played, not searched";
        else
        {
            // The value is in internal units, as in the file; in brackets as in the analysis
            cout << " depth " << lm->depth << " value " << lm->score;
            if (lm->score > -VALUE_INFINITE && lm->score < VALUE_INFINITE)
                cout << " (" << UCIEngine::format_score(Score(lm->score, pos)) << ")";
            cout << " win probability " << lm->performance << "%";
        }
        cout << sync_endl;
    }
}

void LearningData::show_exp(const Position& pos) {
    sync_cout << pos << endl;
    cout << "Experience: ";
    vector<LearningMove*> learningMoves = LD.probe(pos.key());
    if (learningMoves.empty())
    {
        cout << "No experience data found for this position" << sync_endl;
        return;
    }

    sortLearningMoves(learningMoves);

    cout << endl;
    for (const auto& move : learningMoves)
    {
        const int winProb = move->performance;
        cout << "move: " << UCIEngine::move(move->move, pos.is_chess960())
             << " depth: " << move->depth << " value: " << move->score
             << " win probability: " << winProb << endl;
    }
    cout << sync_endl;
}
