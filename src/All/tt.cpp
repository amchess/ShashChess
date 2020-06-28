/*
  ShashChess, a UCI chess playing engine derived from Stockfish
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2015 Marco Costalba, Joona Kiiski, Tord Romstad
  Copyright (C) 2015-2019 Marco Costalba, Joona Kiiski, Gary Linscott, Tord Romstad

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
#include <thread>
#include <fstream> //from kellykynyama mcts
#include "bitboard.h"
#include "misc.h"
#include "thread.h"
#include "tt.h"
#include "uci.h"

//from Kelly Begin
using namespace std;
LearningHashTable globalLearningHT,experienceHT;
//from Kelly end

TranspositionTable TT; // Our global transposition table

/// TTEntry::save() populates the TTEntry with a new node's data, possibly
/// overwriting an old position. Update is not atomic and can be racy.

void TTEntry::save(Key k, Value v, bool pv, Bound b, Depth d, Move m, Value ev) {

  // Preserve any existing move for the same position
  if (m || (uint16_t)k != key16)
      move16 = (uint16_t)m;

  // Overwrite less valuable entries
  if ((uint16_t)k != key16
      || d - DEPTH_OFFSET > depth8 - 4
      || b == BOUND_EXACT)
  {
      assert(d >= DEPTH_OFFSET);

      key16     = (uint16_t)k;
      value16   = (int16_t)v;
      eval16    = (int16_t)ev;
      genBound8 = (uint8_t)(TT.generation8 | uint8_t(pv) << 2 | b);
      depth8    = (uint8_t)(d - DEPTH_OFFSET);
  }
}


/// TranspositionTable::resize() sets the size of the transposition table,
/// measured in megabytes. Transposition table consists of a power of 2 number
/// of clusters and each cluster consists of ClusterSize number of TTEntry.

void TranspositionTable::resize(size_t mbSize) {

  Threads.main()->wait_for_search_finished();

  aligned_ttmem_free(mem);

  clusterCount = mbSize * 1024 * 1024 / sizeof(Cluster);
  table = static_cast<Cluster*>(aligned_ttmem_alloc(clusterCount * sizeof(Cluster), mem));
  if (!mem)
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
      if (!tte[i].key16 || tte[i].key16 == key16)
      {
          tte[i].genBound8 = uint8_t(generation8 | (tte[i].genBound8 & 0x7)); // Refresh

          return found = (bool)tte[i].key16, &tte[i];
      }

  // Find an entry to be replaced according to the replacement strategy
  TTEntry* replace = tte;
  for (int i = 1; i < ClusterSize; ++i)
      // Due to our packed storage format for generation and its cyclic
      // nature we add 263 (256 is the modulus plus 7 to keep the unrelated
      // lowest three bits from affecting the result) to calculate the entry
      // age correctly even after generation8 overflows into the next cycle.
      if (  replace->depth8 - ((263 + generation8 - replace->genBound8) & 0xF8)
          >   tte[i].depth8 - ((263 + generation8 -   tte[i].genBound8) & 0xF8))
          replace = &tte[i];

  return found = false, replace;
}


/// TranspositionTable::hashfull() returns an approximation of the hashtable
/// occupation during a search. The hash is x permill full, as per UCI protocol.

int TranspositionTable::hashfull() const {

  int cnt = 0;
  for (int i = 0; i < 1000; ++i)
      for (int j = 0; j < ClusterSize; ++j)
          cnt += (table[i].entry[j].genBound8 & 0xF8) == generation8;

  return cnt / ClusterSize;
}
//from Kelly begin
void setLearningStructures ()
{
  loadLearningFileIntoLearningTables (true);
  loadSlaveLearningFilesIntoLearningTables ();
  writeLearningFile (HashTableType::experience);
  experienceHT.clear ();
  globalLearningHT.clear ();
  loadLearningFileIntoLearningTables (false);
}

void loadLearningFileIntoLearningTables(bool toDeleteBinFile) {
  std::string fileName="experience";
  ifstream inputLearningFile("experience.bin", ios::in | ios::binary);
  int loading = 1;
  while (loading)
  {
    LearningFileEntry currentInputLearningFileEntry;
    currentInputLearningFileEntry.depth = 0;
    currentInputLearningFileEntry.hashKey = 0;
    currentInputLearningFileEntry.move = MOVE_NONE;
    currentInputLearningFileEntry.score = VALUE_NONE;
    currentInputLearningFileEntry.performance = 100;
    inputLearningFile.read((char*)&currentInputLearningFileEntry, sizeof(currentInputLearningFileEntry));
    if (currentInputLearningFileEntry.hashKey)
    {
      insertIntoOrUpdateLearningTable(currentInputLearningFileEntry,globalLearningHT);

      if(toDeleteBinFile)
      {
	 insertIntoOrUpdateLearningTable(currentInputLearningFileEntry,experienceHT);
      }
    }
    else
      loading = 0;
  }
  inputLearningFile.close();
  if(toDeleteBinFile)
  {
    char fileNameStr[fileName.size() + 1];
    strcpy(fileNameStr, fileName.c_str());
    remove(fileNameStr);
  }
}

void updateLatestMoveInfo (Node node, int k)
{
  //update lateChild begin
  node->latestMoveInfo.move = node->siblingMoveInfo[k].move;
  node->latestMoveInfo.score = node->siblingMoveInfo[k].score;
  node->latestMoveInfo.depth = node->siblingMoveInfo[k].depth;
  node->latestMoveInfo.performance = node->siblingMoveInfo[k].performance;
  //update lateChild end
}

void setSiblingMoveInfo (Node node, int k, LearningFileEntry& fileExpEntry)
{
  node->siblingMoveInfo[k].score = fileExpEntry.score;
  node->siblingMoveInfo[k].depth = fileExpEntry.depth;
  node->siblingMoveInfo[k].performance = fileExpEntry.performance;
}

NodeInfo getNewNodeInfo (LearningFileEntry& fileExpEntry)
{
  // Node was not found, so we have to create a new one
  NodeInfo newNodeInfo;
  newNodeInfo.hashKey = fileExpEntry.hashKey;
  newNodeInfo.latestMoveInfo.move = fileExpEntry.move;
  newNodeInfo.latestMoveInfo.score = fileExpEntry.score;
  newNodeInfo.latestMoveInfo.depth = fileExpEntry.depth;
  newNodeInfo.latestMoveInfo.performance = fileExpEntry.performance;
  newNodeInfo.siblingMoveInfo[0] = newNodeInfo.latestMoveInfo;
  newNodeInfo.siblings = 1;
  return newNodeInfo;
}

void insertIntoOrUpdateLearningTable(LearningFileEntry& fileExpEntry,LearningHashTable& learningHT)
{
    // We search in the range of all the hash table entries with key fileExpEntry
    auto range = learningHT.equal_range(fileExpEntry.hashKey);
    auto it1 = range.first;
    auto it2 = range.second;

    bool isNewNode = true;
    while (it1 != it2)
    {
      Node node = &(it1->second);
      if (node->hashKey == fileExpEntry.hashKey)
      {
	isNewNode = false;
	int maxSibs = node->siblings % MOVE_INFOS_SIZE;
	for(int k = 0; k <= maxSibs; k++)
	{
	  if(k == maxSibs) //new position for sibling from learning file
	  {
	    node->siblingMoveInfo[k].move = fileExpEntry.move;
	    setSiblingMoveInfo (node, k, fileExpEntry);
	    node->siblings++;
	    if(
	        ((node->siblingMoveInfo[k].score > node->latestMoveInfo.score
	  	  ||	node->latestMoveInfo.move == node->siblingMoveInfo[k].move)
	        && (Options["Persisted learning"]=="Self")
	        )
	       ||
	       (
	        ((node->latestMoveInfo.depth < node->siblingMoveInfo[k].depth)
	  		||
	  		((node->latestMoveInfo.depth == node->siblingMoveInfo[k].depth) && (node->latestMoveInfo.score <= node->siblingMoveInfo[k].score )))
	  	&& (Options["Persisted learning"]=="Standard")
	        )
	      )
	    {
		updateLatestMoveInfo (node, k);
	    }
	    break;
	  }
	  else
	  {
	    if(node->siblingMoveInfo[k].move == fileExpEntry.move)
	    {
                if((Options["Persisted learning"]=="Self"))
                {
		  setSiblingMoveInfo (node, k, fileExpEntry);
		  if(node->siblingMoveInfo[k].score > node->latestMoveInfo.score
			  ||	node->latestMoveInfo.move == node->siblingMoveInfo[k].move)
		  {
		      updateLatestMoveInfo (node, k);
		  }
		  break;
                }
                else
                {
		  if(
		      ((((node->siblingMoveInfo[k].depth < fileExpEntry.depth))
		      ||
		      ((node->siblingMoveInfo[k].depth == fileExpEntry.depth) &&
		       ((node->siblingMoveInfo[k].score < fileExpEntry.score )))))
		    )
		  {
		    setSiblingMoveInfo (node, k, fileExpEntry);
		    if((node->latestMoveInfo.depth < node->siblingMoveInfo[k].depth)
			||
			((node->latestMoveInfo.depth == node->siblingMoveInfo[k].depth) && (node->latestMoveInfo.score <= node->siblingMoveInfo[k].score )))
		    {
			updateLatestMoveInfo (node, k);
		    }
		   }
		   break;
                }
	    }
	  }
	}
        break;
      }
      it1++;
    }

    if (isNewNode)
    {
      NodeInfo newNodeInfo = getNewNodeInfo (fileExpEntry);
      learningHT.insert(make_pair(fileExpEntry.hashKey, newNodeInfo));
    }
}

/// getNodeFromGlobalHT(Key key) probes the Monte-Carlo hash table to return the node with the given
/// position or a nullptr Node if it doesn't exist yet in the table.
Node getNodeFromHT(Key key,HashTableType hashTableType)
{
  // We search in the range of all the hash table entries with key key.
  Node currentNode = nullptr;
  auto range=globalLearningHT.equal_range(key);
  if(hashTableType==HashTableType::experience)
    {
      range=experienceHT.equal_range(key);
    }
  auto it1 = range.first;
  auto it2 = range.second;
  while (it1 != it2)
  {
    currentNode = &(it1->second);
    if (currentNode->hashKey == key)
    {
	return currentNode;
    }
    it1++;
  }

  return currentNode;
}
Value makeExpValue(LearningFileEntry fileExpEntry,HashTableType hashTableType)
{
  // We search in the range of all the hash table entries with key key.
  Node currentNode = nullptr;
  auto range=globalLearningHT.equal_range(fileExpEntry.hashKey);
  if(hashTableType==HashTableType::experience)
    {
      range=experienceHT.equal_range(fileExpEntry.hashKey);
    }
  auto it1 = range.first;
  auto it2 = range.second;
  while (it1 != it2)
  {
    currentNode = &(it1->second);
    if (currentNode->hashKey == fileExpEntry.hashKey)
    {
		for(int k = 0; k <= currentNode->siblings; k++)
		{
			if(currentNode->siblingMoveInfo[k].move == fileExpEntry.move)	    
				return currentNode->siblingMoveInfo[k].score;
		}
    }
    it1++;
  }

  return fileExpEntry.score;
}

void writeLearningFile(HashTableType hashTableType)
{
  LearningHashTable currentLearningHT;
  currentLearningHT=experienceHT;
  if(hashTableType==HashTableType::global)
    {
      currentLearningHT=globalLearningHT;
    }
  if(!currentLearningHT.empty())
    {
      std::ofstream outputFile ("experience.bin", std::ofstream::trunc | std::ofstream::binary);
      for(auto& it:currentLearningHT)
      {
        LearningFileEntry currentFileExpEntry;
        NodeInfo currentNodeInfo=it.second;
        for(int k = 0; k < currentNodeInfo.siblings; k++)
	{
		MoveInfo currentLatestMoveInfo=currentNodeInfo.siblingMoveInfo[k];
		currentFileExpEntry.depth = currentLatestMoveInfo.depth;
		currentFileExpEntry.hashKey = it.first;
		currentFileExpEntry.move = currentLatestMoveInfo.move;
		currentFileExpEntry.score = currentLatestMoveInfo.score;
		currentFileExpEntry.performance = currentLatestMoveInfo.performance;
		outputFile.write((char*)&currentFileExpEntry, sizeof(currentFileExpEntry));
	}
      }
      outputFile.close();
    }
}

void loadSlaveLearningFilesIntoLearningTables()
{
    bool merging=true;
    int i=0;
    while (merging)
    {
      std::string index = std::to_string(i);
      std::string slaveFileName ="";
      slaveFileName="experience" + index + ".bin";
      ifstream slaveInputFile (slaveFileName, ios::in | ios::binary);
      if(!slaveInputFile.good())
      {
	merging=false;
	i++;
      }
      else
      {
	while(slaveInputFile.good())
	{
	  LearningFileEntry slaveFileExpEntry;
	  slaveFileExpEntry.depth = 0;
	  slaveFileExpEntry.hashKey = 0;
	  slaveFileExpEntry.move = MOVE_NONE;
	  slaveFileExpEntry.score = VALUE_NONE;
	  slaveFileExpEntry.performance = 100;

	  slaveInputFile.read((char*)&slaveFileExpEntry, sizeof(slaveFileExpEntry));
	  if (slaveFileExpEntry.hashKey)
	  {
	      insertIntoOrUpdateLearningTable(slaveFileExpEntry,experienceHT);
	  }
	  else
	  {
	    slaveInputFile.close();
	    char slaveStr[slaveFileName.size() + 1];
	    strcpy(slaveStr, slaveFileName.c_str());
	    remove(slaveStr);
	    i++;
	  }
	}
      }
    }
}
//from Kelly End
