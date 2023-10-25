/*
  ShashChess, a UCI chess playing engine derived from Stockfish
  Copyright (C) 2004-2023 The Stockfish developers (see AUTHORS file)

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

#ifndef MISC_H_INCLUDED
#define MISC_H_INCLUDED

#include <algorithm>
#include <cassert>
#include <chrono>
#include <functional>
#include <mutex>
#include <ostream>
#include <string>
#include <vector>
#include <cstdint>
#ifndef _MSC_VER
    #include <mm_malloc.h>
#endif
#include <iostream>
#ifndef _WIN32
    #include <fcntl.h>
    #include <unistd.h>
    #include <sys/mman.h>
    #include <sys/stat.h>
#else
    #define WIN32_LEAN_AND_MEAN
    #ifndef NOMINMAX
        #define NOMINMAX  // Disable macros min() and max()
    #endif
    #include <windows.h>
#endif

#include "types.h"

#define stringify2(x) #x
#define stringify(x) stringify2(x)

namespace Stockfish {

class Position;  //Needed by is_game_decided() Learner from Khalid

std::string engine_info(bool to_uci = false);
std::string compiler_info();
void        prefetch(void* addr);
void        start_logger(const std::string& fname);
void*       std_aligned_alloc(size_t alignment, size_t size);
void        std_aligned_free(void* ptr);
void*       aligned_large_pages_alloc(
        size_t size);                      // memory aligned by page size, min alignment: 4096 bytes
void aligned_large_pages_free(void* mem);  // nop if mem == nullptr

void dbg_hit_on(bool cond, int slot = 0);
void dbg_mean_of(int64_t value, int slot = 0);
void dbg_stdev_of(int64_t value, int slot = 0);
void dbg_correl_of(int64_t value1, int64_t value2, int slot = 0);
void dbg_print();

using TimePoint = std::chrono::milliseconds::rep;  // A value in milliseconds
static_assert(sizeof(TimePoint) == sizeof(int64_t), "TimePoint should be 64 bits");
inline TimePoint now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

template<class Entry, int Size>
struct HashTable {
    Entry* operator[](Key key) { return &table[(uint32_t) key & (Size - 1)]; }

   private:
    std::vector<Entry> table = std::vector<Entry>(Size);  // Allocate on the heap
};


enum SyncCout {
    IO_LOCK,
    IO_UNLOCK
};
std::ostream& operator<<(std::ostream&, SyncCout);

#define sync_cout std::cout << IO_LOCK
#define sync_endl std::endl << IO_UNLOCK


// align_ptr_up() : get the first aligned element of an array.
// ptr must point to an array of size at least `sizeof(T) * N + alignment` bytes,
// where N is the number of elements in the array.
template<uintptr_t Alignment, typename T>
T* align_ptr_up(T* ptr) {
    static_assert(alignof(T) < Alignment);

    const uintptr_t ptrint = reinterpret_cast<uintptr_t>(reinterpret_cast<char*>(ptr));
    return reinterpret_cast<T*>(
      reinterpret_cast<char*>((ptrint + (Alignment - 1)) / Alignment * Alignment));
}


// IsLittleEndian : true if and only if the binary is compiled on a little endian machine
static inline const union {
    uint32_t i;
    char     c[4];
} Le                                    = {0x01020304};
static inline const bool IsLittleEndian = (Le.c[0] == 4);


template<typename T, std::size_t MaxSize>
class ValueList {

   public:
    std::size_t size() const { return size_; }
    void        push_back(const T& value) { values_[size_++] = value; }
    const T*    begin() const { return values_; }
    const T*    end() const { return values_ + size_; }

   private:
    T           values_[MaxSize];
    std::size_t size_ = 0;
};

/// xorshift64star Pseudo-Random Number Generator
/// This class is based on original code written and dedicated
/// to the public domain by Sebastiano Vigna (2014).
/// It has the following characteristics:
///
///  -  Outputs 64-bit numbers
///  -  Passes Dieharder and SmallCrush test batteries
///  -  Does not require warm-up, no zeroland to escape
///  -  Internal state is a single 64-bit integer
///  -  Period is 2^64 - 1
///  -  Speed: 1.60 ns/call (Core i7 @3.40GHz)
///
/// For further analysis see
///   <http://vigna.di.unimi.it/ftp/papers/xorshift.pdf>

class PRNG {

    uint64_t s;

    uint64_t rand64() {

        s ^= s >> 12, s ^= s << 25, s ^= s >> 27;
        return s * 2685821657736338717LL;
    }

   public:
    PRNG(uint64_t seed) :
        s(seed) {
        assert(seed);
    }

    template<typename T>
    T rand() {
        return T(rand64());
    }

    /// Special generator used to fast init magic numbers.
    /// Output values only have 1/8th of their bits set on average.
    template<typename T>
    T sparse_rand() {
        return T(rand64() & rand64() & rand64());
    }
};

inline uint64_t mul_hi64(uint64_t a, uint64_t b) {
#if defined(__GNUC__) && defined(IS_64BIT)
    __extension__ using uint128 = unsigned __int128;
    return ((uint128) a * (uint128) b) >> 64;
#else
    uint64_t aL = (uint32_t) a, aH = a >> 32;
    uint64_t bL = (uint32_t) b, bH = b >> 32;
    uint64_t c1 = (aL * bL) >> 32;
    uint64_t c2 = aH * bL + c1;
    uint64_t c3 = aL * bH + (uint32_t) c2;
    return aH * bH + (c2 >> 32) + (c3 >> 32);
#endif
}

/// Under Windows it is not possible for a process to run on more than one
/// logical processor group. This usually means to be limited to use max 64
/// cores. To overcome this, some special platform specific API should be
/// called to set group affinity for each thread. Original code from Texel by
/// Peter Österlund.

namespace WinProcGroup {
void bindThisThread(size_t idx);
}

namespace CommandLine {
void init(int argc, char* argv[]);

extern std::string binaryDirectory;   // path of the executable directory
extern std::string workingDirectory;  // path of the working directory
}
//begin from khalid polyfish
#define EMPTY "<empty>"
namespace Utility {
#if defined(_WIN32) || defined(_WIN64)
constexpr char DirectorySeparator        = '\\';
constexpr char ReverseDirectorySeparator = '/';
#else
constexpr char DirectorySeparator        = '/';
constexpr char ReverseDirectorySeparator = '\\';
#endif
std::string unquote(const std::string& s);
bool        is_empty_filename(const std::string& f);
std::string fix_path(const std::string& p);
std::string combine_path(const std::string& p1, const std::string& p2);
std::string map_path(const std::string& p);
//from learner
void init(const char* arg0);
bool is_game_decided(const Position& pos, Value lastScore);
//from learner
size_t get_file_size(const std::string& f);
bool   is_same_file(const std::string& f1, const std::string& f2);

std::string format_bytes(uint64_t bytes, int decimals);

std::string format_string(const char* const fmt, ...);

class FileMapping {
   private:
    uint64_t mapping;
    void*    baseAddress;
    size_t   dataSize;

   public:
    FileMapping() :
        mapping(0),
        baseAddress(nullptr),
        dataSize(0) {}

    ~FileMapping() { unmap(); }

    bool map(const std::string& f, bool verbose) {
        unmap();

#ifdef _WIN32
        // Note FILE_FLAG_RANDOM_ACCESS is only a hint to Windows and as such may get ignored.
        HANDLE fd = CreateFile(f.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                               FILE_FLAG_RANDOM_ACCESS, nullptr);

        if (fd == INVALID_HANDLE_VALUE)
        {
            if (verbose)
                sync_cout << "info string CreateFile() failed for: " << f
                          << ". Error code: " << GetLastError() << sync_endl;

            return false;
        }

        //Read file size
        DWORD sizeHigh;
        DWORD sizeLow = GetFileSize(fd, &sizeHigh);
        if (sizeHigh == 0 && sizeLow == 0)
        {
            CloseHandle(fd);

            if (verbose)
                sync_cout << "info string File is empty: " << f << sync_endl;

            return false;
        }

        //Create mapping
        HANDLE mmap = CreateFileMapping(fd, nullptr, PAGE_READONLY, sizeHigh, sizeLow, nullptr);
        CloseHandle(fd);

        if (!mmap)
        {
            if (verbose)
                sync_cout << "info string CreateFileMapping() failed for: " << f
                          << ". Error code: " << GetLastError() << sync_endl;

            return false;
        }

        //Get data pointer
        void* viewBase = MapViewOfFile(mmap, FILE_MAP_READ, 0, 0, 0);
        if (!viewBase)
        {
            if (verbose)
                sync_cout << "info string MapViewOfFile() failed for: " << f
                          << ". Error code: " << GetLastError() << sync_endl;

            return false;
        }

        //Assign
        mapping     = (uint64_t) mmap;
        baseAddress = viewBase;
        dataSize    = ((size_t) sizeHigh << 32) | (size_t) sizeLow;
#else
        //Open the file
        struct stat statbuf;
        int         fd = ::open(f.c_str(), O_RDONLY);

        if (fd == -1)
        {
            if (verbose)
                sync_cout << "info string open() failed for: " << f << sync_endl;

            return false;
        }

        //Read file size
        fstat(fd, &statbuf);
        if (statbuf.st_size == 0)
        {
            ::close(fd);

            if (verbose)
                sync_cout << "info string File is empty: " << f << sync_endl;

            return false;
        }

        //Create mapping
        void* data = mmap(nullptr, statbuf.st_size, PROT_READ, MAP_SHARED, fd, 0);
        if (data == MAP_FAILED)
        {
            ::close(fd);

            if (verbose)
                sync_cout << "info string mmap() failed for: " << f << sync_endl;

            return false;
        }

    #if defined(MADV_RANDOM)
        madvise(data, statbuf.st_size, MADV_RANDOM);
    #endif
        ::close(fd);

        mapping     = statbuf.st_size;
        baseAddress = data;
        dataSize    = statbuf.st_size;
#endif
        return true;
    }

    void unmap() {
        assert((mapping == 0) == (baseAddress == nullptr)
               && (baseAddress == nullptr) == (dataSize == 0));

#ifdef _WIN32
        if (baseAddress)
            UnmapViewOfFile(baseAddress);

        if (mapping)
            CloseHandle((HANDLE) mapping);
#else
        if (baseAddress && mapping)
            munmap(baseAddress, mapping);
#endif
        baseAddress = nullptr;
        mapping     = 0;
        dataSize    = 0;
    }

    bool has_data() const {
        assert((mapping == 0) == (baseAddress == nullptr)
               && (baseAddress == nullptr) == (dataSize == 0));

        return (baseAddress != nullptr && dataSize != 0);
    }

    const unsigned char* data() const {
        assert(mapping != 0 && baseAddress != nullptr && dataSize != 0);
        return (const unsigned char*) baseAddress;
    }

    size_t data_size() const {
        assert(mapping != 0 && baseAddress != nullptr && dataSize != 0);
        return dataSize;
    }
};
}
//end from khalid polyfish

}  // namespace Stockfish

#endif  // #ifndef MISC_H_INCLUDED
