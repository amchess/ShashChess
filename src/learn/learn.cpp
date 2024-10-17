#include <iostream>
#include <fstream>
#include <sstream>
#include "../misc.h"
#include "learn.h"
#include <algorithm>
#include <cstdint>
#include "../uci.h"

#include "../win_probability.h"

using namespace std;
using namespace ShashChess;

LearningData LD;

namespace {
LearningMode identify_learning_mode(const string& lm) {
    if (lm == "Off")
        return LearningMode::Off;

    if (lm == "Standard")
        return LearningMode::Standard;

    return LearningMode::Self;
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

void LearningData::insert_or_update(PersistedLearningMove* plm, bool qLearning) {
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
        return;
    }

    //The plm key belongs to a position already existing in the LHT
    //Check if this move already exists for this position
    const auto itr = find_if(
      first, second, [&plm](const auto& p) { return p.second->move == plm->learningMove.move; });

    //If the move does not exist then insert it
    LearningMove* bestNewMoveCandidate = nullptr;
    if (itr == second)
    {
        HT.insert({plm->key, &plm->learningMove});
        bestNewMoveCandidate = &plm->learningMove;

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
}

LearningData::LearningData() :
    isPaused(false),
    isReadOnly(false),
    needPersisting(false),
    learningMode(LearningMode::Off) {}

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
}

void LearningData::init(ShashChess::OptionsMap& o) {
    OptionsMap& options = o;
    clear();

    learningMode = identify_learning_mode(options["Persisted learning"]);
    if ((learningMode == LearningMode::Off) && !((bool) options["Experience Book"]))
        return;

    load(Util::map_path("experience.exp"));

    vector<string> slaveFiles;

    //Just in case, check and load for "experience_new.exp" which will be present if
    //previous saving operation failed (engine crashed or terminated)
    string slaveFile = Util::map_path("experience_new.exp");
    if (load("experience_new.exp"))
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
    if (!slaveFiles.empty())
    {
        persist(options);
    }

    //Remove slave files
    for (const string& fn : slaveFiles)
    {
        remove(fn.c_str());
    }

    // Clear the 'needPersisting' flag
    needPersisting = false;
}

void LearningData::quick_reset_exp() {
    std::cout << "Loading experience file: experience.exp" << std::endl;

    std::ifstream file("experience.exp", std::ifstream::binary | std::ifstream::ate);
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

    if (const auto check = load("experience.exp"); !check)
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

void LearningData::persist(const ShashChess::OptionsMap& o) {
    const OptionsMap& options = o;
    //Quick exit if we have nothing to persist
    if (HT.empty() || !needPersisting)
        return;

    if (isReadOnly)
    {
        //We should not be here if we are running in ReadOnly mode
        assert(false);
        return;
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

    ofstream              outputFile(tempExperienceFilename, ofstream::trunc | ofstream::binary);
    PersistedLearningMove persistedLearningMove;
    for (auto& kvp : HT)
    {
        persistedLearningMove.key          = kvp.first;
        persistedLearningMove.learningMove = *kvp.second;
        if (persistedLearningMove.learningMove.depth != 0)
        {
            outputFile.write(reinterpret_cast<char*>(&persistedLearningMove),
                             sizeof(persistedLearningMove));
        }
    }
    outputFile.close();

    remove(experienceFilename.c_str());
    rename(tempExperienceFilename.c_str(), experienceFilename.c_str());

    //Prevent persisting again without modifications
    needPersisting = false;
}

void LearningData::pause() { isPaused = true; }

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

    //Add to HT
    insert_or_update(newPlm, learningMode == LearningMode::Self);
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
