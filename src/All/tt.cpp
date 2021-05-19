/*
  ShashChess, a UCI chess playing engine derived from Stockfish
  Copyright (C) 2004-2021 The Stockfish developers (see AUTHORS file)

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

#include <cstring>   // For std::memset
#include <iostream>
#include <iomanip>
#include <thread>
#include <algorithm>
#include <fstream> //from kellykynyama
#include "bitboard.h"
#include "misc.h"
#include "thread.h"
#include "tt.h"
#include "uci.h"
#include <sstream>

namespace Stockfish {
//from Kelly Begin
#define SHOW_EXP_STATS  0
PersistedLearningUsage usePersistedLearning;
using namespace std;
LearningHashTable globalLearningHT;
//from Kelly end

TranspositionTable TT; // Our global transposition table

/// TTEntry::save() populates the TTEntry with a new node's data, possibly
/// overwriting an old position. Update is not atomic and can be racy.

void TTEntry::save(Key k, Value v, bool pv, Bound b, Depth d, Move m, Value ev) {

  // Preserve any existing move for the same position
  if (m || (uint16_t)k != key16)
      move16 = (uint16_t)m;

  // Overwrite less valuable entries (cheapest checks first)
  if (b == BOUND_EXACT
      || (uint16_t)k != key16
      || d - DEPTH_OFFSET > depth8 - 4)
  {
      assert(d > DEPTH_OFFSET);
      assert(d < 256 + DEPTH_OFFSET);

      key16     = (uint16_t)k;
      depth8    = (uint8_t)(d - DEPTH_OFFSET);
      genBound8 = (uint8_t)(TT.generation8 | uint8_t(pv) << 2 | b);
      value16   = (int16_t)v;
      eval16    = (int16_t)ev;
  }
}


/// TranspositionTable::resize() sets the size of the transposition table,
/// measured in megabytes. Transposition table consists of a power of 2 number
/// of clusters and each cluster consists of ClusterSize number of TTEntry.

void TranspositionTable::resize(size_t mbSize) {

  Threads.main()->wait_for_search_finished();

  aligned_large_pages_free(table);

  clusterCount = mbSize * 1024 * 1024 / sizeof(Cluster);

  table = static_cast<Cluster*>(aligned_large_pages_alloc(clusterCount * sizeof(Cluster)));
  if (!table)
  {
      std::cerr << "Failed to allocate " << mbSize
                << "MB for transposition table." << std::endl;
      exit(EXIT_FAILURE);
  }

  clear();
}


/// TranspositionTable::clear() initializes the entire transposition table to zero,
//  in a multi-threaded way.

void TranspositionTable::clear() {

  std::vector<std::thread> threads;

  for (size_t idx = 0; idx < Options["Threads"]; ++idx)
  {
      threads.emplace_back([this, idx]() {

          // Thread binding gives faster search on systems with a first-touch policy
          if (Options["Threads"] > 8)
              WinProcGroup::bindThisThread(idx);

          // Each thread will zero its part of the hash table
          const size_t stride = size_t(clusterCount / Options["Threads"]),
                       start  = size_t(stride * idx),
                       len    = idx != Options["Threads"] - 1 ?
                                stride : clusterCount - start;

          std::memset(&table[start], 0, len * sizeof(Cluster));
      });
  }

  for (std::thread& th : threads)
      th.join();
}


/// TranspositionTable::probe() looks up the current position in the transposition
/// table. It returns true and a pointer to the TTEntry if the position is found.
/// Otherwise, it returns false and a pointer to an empty or least valuable TTEntry
/// to be replaced later. The replace value of an entry is calculated as its depth
/// minus 8 times its relative age. TTEntry t1 is considered more valuable than
/// TTEntry t2 if its replace value is greater than that of t2.

TTEntry* TranspositionTable::probe(const Key key, bool& found) const {

  TTEntry* const tte = first_entry(key);
  const uint16_t key16 = (uint16_t)key;  // Use the low 16 bits as key inside the cluster

  for (int i = 0; i < ClusterSize; ++i)
      if (tte[i].key16 == key16 || !tte[i].depth8)
      {
          tte[i].genBound8 = uint8_t(generation8 | (tte[i].genBound8 & (GENERATION_DELTA - 1))); // Refresh

          return found = (bool)tte[i].depth8, &tte[i];
      }

  // Find an entry to be replaced according to the replacement strategy
  TTEntry* replace = tte;
  for (int i = 1; i < ClusterSize; ++i)
      // Due to our packed storage format for generation and its cyclic
      // nature we add GENERATION_CYCLE (256 is the modulus, plus what
      // is needed to keep the unrelated lowest n bits from affecting
      // the result) to calculate the entry age correctly even after
      // generation8 overflows into the next cycle.
      if (  replace->depth8 - ((GENERATION_CYCLE + generation8 - replace->genBound8) & GENERATION_MASK)
          >   tte[i].depth8 - ((GENERATION_CYCLE + generation8 -   tte[i].genBound8) & GENERATION_MASK))
          replace = &tte[i];

  return found = false, replace;
}


/// TranspositionTable::hashfull() returns an approximation of the hashtable
/// occupation during a search. The hash is x permill full, as per UCI protocol.

int TranspositionTable::hashfull() const {

  int cnt = 0;
  for (int i = 0; i < 1000; ++i)
      for (int j = 0; j < ClusterSize; ++j)
          cnt += table[i].entry[j].depth8 && (table[i].entry[j].genBound8 & GENERATION_MASK) == generation8;

  return cnt / ClusterSize;
}
//from Kelly begin
bool pauseExperience = false;

#if defined(SHOW_EXP_STATS) && SHOW_EXP_STATS == 1
static size_t fileEntries = 0;
static size_t countOfNodeInfo = 0;
static size_t countOfMoveInfo = 0;
static size_t duplicateMoves = 0;
static size_t sizeInMemory = sizeof(globalLearningHT);
#endif

bool loadExperienceFile(const string& filename, bool deleteAfterLoading)
{
    std::string fn = Utility::map_path(filename);
    ifstream inputLearningFile(fn, ios::in | ios::binary);

    //Quick exit if file is not present
    if (!inputLearningFile.is_open())
        return false;

    LearningFileEntry tempEntry;
    while (true)
    {
        //Invalidate the entry
        tempEntry.hashKey = (Key)0;

        //Read a new entry
        inputLearningFile.read((char*)&tempEntry, sizeof(tempEntry));

        //If we got a null hashKey it means we are done!
        if (!tempEntry.hashKey)
            break;

        insertIntoOrUpdateLearningTable(tempEntry);
    }

    //Close it
    inputLearningFile.close();

    //Delete it if requested
    if (deleteAfterLoading)
        remove(fn.c_str());

    return true;
}

bool loadSlaveLearningFilesIntoLearningTables()
{
    int i = 0;
    while (true)
    {
        if (!loadExperienceFile("experience" + std::to_string(i) + ".bin", true))
            break;

        i++;
    }

    return i > 0;
}

void initLearning()
{
    setUsePersistedLearning();
    loadExperienceFile("experience.bin", false);

    bool shouldRefresh = false;

    //Just in case, check and load for "experience_new.bin" which will be present if
    //previous saving operation failed (engine crashed or terminated)
    shouldRefresh |= loadExperienceFile("experience_new.bin", true);

    //Load slave experience files (if any)
    shouldRefresh |= loadSlaveLearningFilesIntoLearningTables();

    if (shouldRefresh)
    {
        //We need to write all consolidated experience to disk
        writeLearningFile();

        //Clear existing hash tables before we refresh them
        globalLearningHT.clear();

#if defined(SHOW_EXP_STATS) && SHOW_EXP_STATS == 1
        //Start fresh
        fileEntries = 0;
        countOfNodeInfo = 0;
        countOfMoveInfo = 0;
        duplicateMoves = 0;
        sizeInMemory = sizeof(globalLearningHT);
#endif

        //Refresh
        loadExperienceFile("experience.bin", false);
    }

#if defined(SHOW_EXP_STATS) && SHOW_EXP_STATS == 1
    //Calculate actual data in globalLearningHT
    size_t nodes = globalLearningHT.size();
    size_t moves = 0;
    for (const std::pair<Key, NodeInfo> &it : globalLearningHT)
        moves += it.second.siblingMoveInfo.size();

    cout << "File entries                                                         : " << fileEntries << endl;
    cout << "Number of NodeInfo structures                                        : " << countOfNodeInfo << " (Actual count in hash table: " << nodes << ")" << endl;
    cout << "Number of MoveInfo structures                                        : " << countOfMoveInfo << endl;
    cout << "Duplicate moves (Moves that exist twice in same file or slave files) : " << duplicateMoves << endl;
    cout << "Total moves                                                          : " << (duplicateMoves + countOfMoveInfo) << " (Actual count in hash table: " << moves << ")" << endl;
    cout << "Calculated memory consumption                                        : " << sizeInMemory << " B = "
                                                                                     << std::setprecision(2) << std::fixed << sizeInMemory / 1024.0 << " KB = "
                                                                                     << std::setprecision(2) << std::fixed << sizeInMemory / (1024.0 * 1024.0) << " MB = "
                                                                                     << std::setprecision(2) << std::fixed << sizeInMemory / (1024.0 * 1024.0 * 1000.0) << " GB" << endl;
#endif
}

void setUsePersistedLearning()
{
    if (Options["Persisted learning"] == "Off")
    {
        usePersistedLearning = PersistedLearningUsage::Off;
    }
    else if (Options["Persisted learning"] == "Standard")
    {
        usePersistedLearning = PersistedLearningUsage::Standard;
    }
    else //Classical
    {
        usePersistedLearning = PersistedLearningUsage::Self;
    }
}

void updateLatestMoveInfo(NodeInfo *node, int k)
{
    //update lateChild begin
    node->latestMoveInfo.move = node->siblingMoveInfo[k].move;
    node->latestMoveInfo.score = node->siblingMoveInfo[k].score;
    node->latestMoveInfo.depth = node->siblingMoveInfo[k].depth;
    node->latestMoveInfo.performance = node->siblingMoveInfo[k].performance;
    //update lateChild end
}

void setSiblingMoveInfo(NodeInfo *node, int k, LearningFileEntry& fileExpEntry)
{
    node->siblingMoveInfo[k].score = fileExpEntry.score;
    node->siblingMoveInfo[k].depth = fileExpEntry.depth;
    node->siblingMoveInfo[k].performance = fileExpEntry.performance;
}

NodeInfo getNewNodeInfo(LearningFileEntry& fileExpEntry)
{
    // Node was not found, so we have to create a new one
    NodeInfo newNodeInfo;
    newNodeInfo.hashKey = fileExpEntry.hashKey;
    newNodeInfo.latestMoveInfo.move = fileExpEntry.move;
    newNodeInfo.latestMoveInfo.score = fileExpEntry.score;
    newNodeInfo.latestMoveInfo.depth = fileExpEntry.depth;
    newNodeInfo.latestMoveInfo.performance = fileExpEntry.performance;

    newNodeInfo.siblingMoveInfo.push_back(newNodeInfo.latestMoveInfo);
    return newNodeInfo;
}

void insertIntoOrUpdateLearningTable(LearningFileEntry& fileExpEntry)
{
#if defined(SHOW_EXP_STATS) && SHOW_EXP_STATS == 1
    fileEntries++;
#endif

    // We search in the range of all the hash table entries with key fileExpEntry
    LearningHashTable::iterator it = globalLearningHT.find(fileExpEntry.hashKey);

    //Quick handling/exit if 'fileExpEntry' is for a new position
    if (it == globalLearningHT.end())
    {
#if defined(SHOW_EXP_STATS) && SHOW_EXP_STATS == 1
        countOfNodeInfo++;
        countOfMoveInfo++;
        sizeInMemory += sizeof(NodeInfo) + sizeof(MoveInfo) + sizeof(std::pair<Key, NodeInfo>);
#endif

        //Insert new node
        globalLearningHT.insert(std::make_pair(fileExpEntry.hashKey, getNewNodeInfo(fileExpEntry)));

        //Nothing else to do
        return;
    }

    NodeInfo *node = &(it->second);

    //Check if this move already exists
    std::vector<MoveInfo>::iterator existingMoveIterator = std::find_if(
        node->siblingMoveInfo.begin(),
        node->siblingMoveInfo.end(),
        [&fileExpEntry](const MoveInfo &mi) {return mi.move == fileExpEntry.move; });

    //If move does not exist then insert it
    size_t k; //This will be used as index
    if (existingMoveIterator == node->siblingMoveInfo.end())
    {
#if defined(SHOW_EXP_STATS) && SHOW_EXP_STATS == 1
        countOfMoveInfo++;
        sizeInMemory += sizeof(MoveInfo);
#endif
        
        node->siblingMoveInfo.push_back(MoveInfo()); //Insert a new MoveInfo structure

        //Update the newly inserted MoveInfo
        k = node->siblingMoveInfo.size() - 1;
        setSiblingMoveInfo(node, k, fileExpEntry);
        node->siblingMoveInfo[k].move = fileExpEntry.move;

        if (
            ((node->siblingMoveInfo[k].score > node->latestMoveInfo.score
                || node->latestMoveInfo.move == node->siblingMoveInfo[k].move)
                && (usePersistedLearning == PersistedLearningUsage::Self)
                )
            ||
            (
                ((node->latestMoveInfo.depth < node->siblingMoveInfo[k].depth)
                    ||
                    ((node->latestMoveInfo.depth == node->siblingMoveInfo[k].depth) && (node->latestMoveInfo.score <= node->siblingMoveInfo[k].score)))
                && (usePersistedLearning != PersistedLearningUsage::Self)
                )
            )
        {
            updateLatestMoveInfo(node, k);
        }

        //Nothing else to do
        return;
    }

    //Since we are here it means the move is duplicate (already exists)
    //This also means that we have the NodeInfo structure in the map, as well as the MoveInfo in the vector
#if defined(SHOW_EXP_STATS) && SHOW_EXP_STATS == 1
    duplicateMoves++;
    assert(existingMoveIterator->move == fileExpEntry.move); //Just to double check
#endif

    k = std::distance(node->siblingMoveInfo.begin(), existingMoveIterator);

    if (usePersistedLearning == PersistedLearningUsage::Self)
    {
        setSiblingMoveInfo(node, k, fileExpEntry);
        if (node->siblingMoveInfo[k].score > node->latestMoveInfo.score
            || node->latestMoveInfo.move == node->siblingMoveInfo[k].move)
        {
            updateLatestMoveInfo(node, k);
        }
    }
    else
    {
        if (
            ((((node->siblingMoveInfo[k].depth < fileExpEntry.depth))
                ||
                ((node->siblingMoveInfo[k].depth == fileExpEntry.depth) &&
                    ((node->siblingMoveInfo[k].score < fileExpEntry.score)))))
            )
        {
            setSiblingMoveInfo(node, k, fileExpEntry);
            if ((node->latestMoveInfo.depth < node->siblingMoveInfo[k].depth)
                ||
                ((node->latestMoveInfo.depth == node->siblingMoveInfo[k].depth) && (node->latestMoveInfo.score <= node->siblingMoveInfo[k].score)))
            {
                updateLatestMoveInfo(node, k);
            }
        }
    }
}

/// getNodeFromGlobalHT(Key key) probes the Monte-Carlo hash table to return the node with the given
/// position or a nullptr Node if it doesn't exist yet in the table.
NodeInfo *getNodeFromHT(Key key)
{
    LearningHashTable::iterator it = globalLearningHT.find(key);
    if (it == globalLearningHT.end())
        return nullptr;

    return &(it->second);
}
Value makeExpValue(LearningFileEntry fileExpEntry)
{
    LearningHashTable::iterator it = globalLearningHT.find(fileExpEntry.hashKey);

    //Quick handling/exit if 'fileExpEntry' is for a new position
    if (it != globalLearningHT.end())
    {
        //Key found. Now check if we have the move itself
        std::vector<MoveInfo>::iterator existingMoveIterator = std::find_if(
            it->second.siblingMoveInfo.begin(),
            it->second.siblingMoveInfo.end(),
            [&fileExpEntry](const MoveInfo& mi) {return mi.move == fileExpEntry.move; });

        //If found, then return the score from hash table
        if (existingMoveIterator != it->second.siblingMoveInfo.end())
            return existingMoveIterator->score;
    }

    //Resort to the 'fileExpEntry' score
    return fileExpEntry.score;
}

void writeLearningFile()
{
    /*
      To avoid any problems when saving to experience file, we will actually do the following:
      1) Save new experience to "experience_new.bin"
      2) Remove "experience.bin"
      3) Rename "experience_new.bin" to "experience.bin"

      This approach is failproof so that the old file is only removed when the new file is sufccessfully saved!
      If, for whatever odd reason, the engine is able to execute step (1) and (2) and fails to execute step (3)
      i.e., we end up with experience0.bin then it is not a problem since the file will be loaded anyway the next
      time the engine starts!
    */
    if (!globalLearningHT.empty())
    {
        std::string experienceFilename;
        std::string tempExperienceFilename;

        if ((bool)Options["Concurrent Experience"])
        {
            static std::string uniqueStr;

            if (uniqueStr.empty())
            {
                PRNG prng(now());

                std::stringstream ss;
                ss << hex << prng.rand<uint64_t>();

                uniqueStr = ss.str();
            }

            experienceFilename = Utility::map_path("experience-" + uniqueStr + ".bin");
            tempExperienceFilename = Utility::map_path("experience_new-" + uniqueStr + ".bin");
        }
        else
        {
            experienceFilename = Utility::map_path("experience.bin");
            tempExperienceFilename = Utility::map_path("experience_new.bin");
        }

        std::ofstream outputFile(tempExperienceFilename, std::ofstream::trunc | std::ofstream::binary);

        for (auto& it : globalLearningHT)
        {
            LearningFileEntry currentFileExpEntry;
            NodeInfo currentNodeInfo = it.second;
            int siblingMoveInfoSize = currentNodeInfo.siblingMoveInfo.size();
            for (int k = 0; k < siblingMoveInfoSize; k++)
            {
                MoveInfo currentLatestMoveInfo = currentNodeInfo.siblingMoveInfo[k];
                currentFileExpEntry.depth = currentLatestMoveInfo.depth;
                currentFileExpEntry.hashKey = it.first;
                currentFileExpEntry.move = currentLatestMoveInfo.move;
                currentFileExpEntry.score = currentLatestMoveInfo.score;
                currentFileExpEntry.performance = currentLatestMoveInfo.performance;
                outputFile.write((char*)&currentFileExpEntry, sizeof(currentFileExpEntry));
            }
        }
        outputFile.close();

        remove(experienceFilename.c_str());
        rename(tempExperienceFilename.c_str(), experienceFilename.c_str());
    }
}
//from Kelly End
} // namespace Stockfish
