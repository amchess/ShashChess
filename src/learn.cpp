#include <iostream>
#include <fstream>
#include <sstream>
#include "misc.h"
#include "learn.h"
#include "uci.h"

using namespace Stockfish;

LearningData LD;

namespace {
LearningMode identify_learning_mode(const std::string& lm) {
    if (lm == "Off")
        return LearningMode::Off;

    if (lm == "Standard")
        return LearningMode::Standard;

    return LearningMode::Self;
}
}

bool LearningData::load(const std::string& filename) {
    std::ifstream in(filename, std::ios::in | std::ios::binary);

    //Quick exit if file is not present
    if (!in.is_open())
        return false;

    //Get experience file size
    in.seekg(0, std::ios::end);
    size_t fileSize = in.tellg();

    //File size should be a multiple of 'PersistedLearningMove'
    if (fileSize % sizeof(PersistedLearningMove))
    {
        std::cerr << "info string The file <" << filename << "> with size <" << fileSize
                  << "> is not a valid experience file" << std::endl;
        return false;
    }

    //Allocate buffer to read the entire file
    void* fileData = malloc(fileSize);
    if (!fileData)
    {
        std::cerr << "info string Failed to allocate <" << fileSize
                  << "> bytes to read experience file <" << filename << ">" << std::endl;
        return false;
    }

    //Read the entire file
    in.seekg(0, std::ios::beg);  //Move read pointer to the beginning of the file
    in.read((char*) fileData, fileSize);
    if (!in)
    {
        free(fileData);

        std::cerr << "info string Failed to read <" << fileSize << "> bytes from experience file <"
                  << filename << ">" << std::endl;
        return false;
    }

    //Close the input data file
    in.close();

    //Save pointer to fileData to be freed later
    mainDataBuffers.push_back(fileData);

    //Loop the moves from this file
    bool                   qLearning             = (learningMode == LearningMode::Self);
    PersistedLearningMove* persistedLearningMove = (PersistedLearningMove*) fileData;
    do
    {
        insert_or_update(persistedLearningMove, qLearning);
        ++persistedLearningMove;
    } while ((size_t) persistedLearningMove < (size_t) fileData + fileSize);

    return true;
}

void LearningData::insert_or_update(PersistedLearningMove* plm, bool qLearning) {
    // We search in the range of all the hash table entries with plm's key
    auto range = HT.equal_range(plm->key);

    //If the plm's key belongs to a position that did not exist before in the 'LHT'
    //then, we insert this new key and LearningMove and return
    if (range.first == range.second)
    {
        //Insert new key and learningMove
        HT.insert({plm->key, &plm->learningMove});

        //Flag for persisting
        needPersisting = true;

        //Nothing else to do
        return;
    }
    //The plm's key belongs to a position already existing in the 'LHT'
    //Check if this move already exists for this position
    auto itr = std::find_if(range.first, range.second, [&plm](const auto& p) {
        return p.second->move == plm->learningMove.move;
    });

    //If the move does not exist then insert it
    LearningMove* bestNewMoveCandidate = nullptr;
    if (itr == range.second)
    {
        HT.insert({plm->key, &plm->learningMove});
        bestNewMoveCandidate = &plm->learningMove;

        //Flag for persisting
        needPersisting = true;
    }
    else  //If the move exists, check if it better than the move we already have
    {
        LearningMove* existingMove = itr->second;
        if (existingMove->depth < plm->learningMove.depth
            || (existingMove->depth == plm->learningMove.depth
                && existingMove->score < plm->learningMove.score))
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
    bool newBestMove = false;
    if (bestNewMoveCandidate != nullptr)
    {
        LearningMove* currentBestMove = range.first->second;
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

void LearningData::init() {
    clear();

    learningMode = identify_learning_mode(Options["Persisted learning"]);
    if (learningMode == LearningMode::Off)
        return;

    load(Utility::map_path("experience.exp"));

    std::vector<std::string> slaveFiles;

    //Just in case, check and load for "experience_new.exp" which will be present if
    //previous saving operation failed (engine crashed or terminated)
    std::string slaveFile = Utility::map_path("experience_new.exp");
    if (load("experience_new.exp"))
        slaveFiles.push_back(slaveFile);

    //Load slave experience files (if any)
    int i = 0;
    while (true)
    {
        slaveFile   = Utility::map_path("experience" + std::to_string(i) + ".exp");
        bool loaded = load(slaveFile);
        if (!loaded)
            break;

        slaveFiles.push_back(slaveFile);
        ++i;
    }

    //We need to write all consolidated experience to disk
    if (slaveFiles.size())
        persist();

    //Remove slave files
    for (std::string fn : slaveFiles)
        remove(fn.c_str());

    //Clear the 'needPersisting' flag
    needPersisting = false;
}

void LearningData::set_learning_mode(const std::string& lm) {
    LearningMode newLearningMode = identify_learning_mode(lm);
    if (newLearningMode == learningMode)
        return;

    init();
}

LearningMode LearningData::learning_mode() const { return learningMode; }

void LearningData::persist() {
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

        This approach is failproof so that the old file is only removed when the new file is sufccessfully saved!
        If, for whatever odd reason, the engine is able to execute step (1) and (2) and fails to execute step (3)
        i.e., we end up with experience0.exp then it is not a problem since the file will be loaded anyway the next
        time the engine starts!
    */

    std::string experienceFilename;
    std::string tempExperienceFilename;

    if ((bool) Options["Concurrent Experience"])
    {
        static std::string uniqueStr;

        if (uniqueStr.empty())
        {
            PRNG prng(now());

            std::stringstream ss;
            ss << std::hex << prng.rand<uint64_t>();

            uniqueStr = ss.str();
        }

        experienceFilename     = Utility::map_path("experience-" + uniqueStr + ".exp");
        tempExperienceFilename = Utility::map_path("experience_new-" + uniqueStr + ".exp");
    }
    else
    {
        experienceFilename     = Utility::map_path("experience.exp");
        tempExperienceFilename = Utility::map_path("experience_new.exp");
    }

    std::ofstream outputFile(tempExperienceFilename, std::ofstream::trunc | std::ofstream::binary);
    PersistedLearningMove persistedLearningMove;
    for (auto& kvp : HT)
    {
        persistedLearningMove.key          = kvp.first;
        persistedLearningMove.learningMove = *kvp.second;
        if (persistedLearningMove.learningMove.depth != 0)
        {
            outputFile.write((char*) &persistedLearningMove, sizeof(persistedLearningMove));
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
    PersistedLearningMove* newPlm = (PersistedLearningMove*) malloc(sizeof(PersistedLearningMove));
    if (!newPlm)
    {
        std::cerr << "info string Failed to allocate <" << sizeof(PersistedLearningMove)
                  << "> bytes for new learning entry" << std::endl;
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

int LearningData::probe(Key key, const LearningMove*& learningMove) {
    auto range = HT.equal_range(key);

    if (range.first == range.second)
    {
        learningMove = nullptr;
        return 0;
    }

    learningMove = range.first->second;
    return std::distance(range.first, range.second);
}

const LearningMove* LearningData::probe_move(Key key, Move move) {
    auto range = HT.equal_range(key);

    if (range.first == range.second)
        return nullptr;

    auto itr = std::find_if(range.first, range.second,
                            [&move](const auto& p) { return p.second->move == move; });

    if (itr == range.second)
        return nullptr;

    return itr->second;
}
