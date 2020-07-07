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

#ifndef TT_H_INCLUDED
#define TT_H_INCLUDED

#include "misc.h"
#include "types.h"
#include <unordered_map> //from Kelly

/// TTEntry struct is the 10 bytes transposition table entry, defined as below:
///
/// key        16 bit
/// move       16 bit
/// value      16 bit
/// eval value 16 bit
/// generation  5 bit
/// pv node     1 bit
/// bound type  2 bit
/// depth       8 bit

struct TTEntry {

  Move  move()  const { return (Move )move16; }
  Value value() const { return (Value)value16; }
  Value eval()  const { return (Value)eval16; }
  Depth depth() const { return (Depth)depth8 + DEPTH_OFFSET; }
  bool is_pv()  const { return (bool)(genBound8 & 0x4); }
  Bound bound() const { return (Bound)(genBound8 & 0x3); }
  void save(Key k, Value v, bool pv, Bound b, Depth d, Move m, Value ev);

private:
  friend class TranspositionTable;

  uint16_t key16;
  uint16_t move16;
  int16_t  value16;
  int16_t  eval16;
  uint8_t  genBound8;
  uint8_t  depth8;
};


/// A TranspositionTable is an array of Cluster, of size clusterCount. Each
/// cluster consists of ClusterSize number of TTEntry. Each non-empty TTEntry
/// contains information on exactly one position. The size of a Cluster should
/// divide the size of a cache line for best performance, as the cacheline is
/// prefetched when possible.

class TranspositionTable {

  static constexpr int ClusterSize = 3;

  struct Cluster {
    TTEntry entry[ClusterSize];
    char padding[2]; // Pad to 32 bytes
  };

  static_assert(sizeof(Cluster) == 32, "Unexpected Cluster size");

public:
 ~TranspositionTable() { aligned_ttmem_free(mem); }
  void new_search() { generation8 += 8; } // Lower 3 bits are used by PV flag and Bound
  TTEntry* probe(const Key key, bool& found) const;
  int hashfull() const;
  void resize(size_t mbSize);
  void clear();

  TTEntry* first_entry(const Key key) const {
    return &table[mul_hi64(key, clusterCount)].entry[0];
  }

private:
  friend struct TTEntry;

  size_t clusterCount;
  Cluster* table;
  void* mem;
  uint8_t generation8; // Size must be not bigger than TTEntry::genBound8
};

//from Kelly begin
//Very important to have HashTableType values use flag friendly values (i.e., powers of 2)
//since we use bitwise oprations on them
enum class HashTableType : uint8_t  { global = 0x01, experience = 0x02 };

extern bool pauseExperience;

//Operator overloading for HashTableType enum
//Those operators are used elsewhere in the code and therefore need to be defined
//More operators can be added if needed
constexpr HashTableType operator |(HashTableType h1, HashTableType h2) { return HashTableType((uint8_t)h1 | (uint8_t)h2); }
constexpr HashTableType operator &(HashTableType h1, HashTableType h2) { return HashTableType((uint8_t)h1 & (uint8_t)h2); }

struct LearningFileEntry
{
	Key hashKey = 0;
	Depth depth = 0;
	Value score = VALUE_NONE;
	Move move = MOVE_NONE;
	int performance = 100;
};
struct MoveInfo
{
	Move move = MOVE_NONE;
	Depth depth = 0;
	Value score = VALUE_NONE;
	int performance = 100;
};


struct NodeInfo
{
	Key hashKey;
	MoveInfo latestMoveInfo;	
	MoveInfo siblingMoveInfo[MOVE_INFOS_SIZE];
	int siblings = 0;
};


// The Monte-Carlo tree is stored implicitly in one big hash table
typedef std::unordered_multimap<Key, NodeInfo> LearningHashTable;
void setLearningStructures ();
void writeLearningFile(HashTableType hashTableType);

void insertIntoOrUpdateLearningTable(LearningFileEntry& tempExpEntry,LearningHashTable& learningHT);

typedef NodeInfo* Node;
Node getNodeFromHT(Key key,HashTableType hashTableType);

Value makeExpValue(LearningFileEntry fileExpEntry,HashTableType hashTableType);

extern LearningHashTable globalLearningHT,experienceHT;
//from Kelly end

extern TranspositionTable TT;

#endif // #ifndef TT_H_INCLUDED
