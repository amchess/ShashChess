/*
  ShashChess, a UCI chess playing engine derived from Stockfish
  Copyright (C) 2004-2025 Andrea Manzo, F. Ferraguti, K.Kiniama and ShashChess developers (see AUTHORS file)

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
#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
//ShashChess specific begin
#include "types.h"
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
//ShashChess specific end

#define stringify2(x) #x
#define stringify(x) stringify2(x)

namespace ShashChess {

std::string engine_version_info();
std::string engine_info(bool to_uci = false);
std::string compiler_info();

// Preloads the given address in L1/L2 cache. This is a non-blocking
// function that doesn't stall the CPU waiting for data to be loaded from memory,
// which can be quite slow.
void prefetch(const void* addr);

void start_logger(const std::string& fname);

size_t str_to_size_t(const std::string& s);

#if defined(__linux__)

struct PipeDeleter {
    void operator()(FILE* file) const {
        if (file != nullptr)
        {
            pclose(file);
        }
    }
};

#endif

// Reads the file as bytes.
// Returns std::nullopt if the file does not exist.
std::optional<std::string> read_file_to_string(const std::string& path);

void dbg_hit_on(bool cond, int slot = 0);
void dbg_mean_of(int64_t value, int slot = 0);
void dbg_stdev_of(int64_t value, int slot = 0);
void dbg_extremes_of(int64_t value, int slot = 0);
void dbg_correl_of(int64_t value1, int64_t value2, int slot = 0);
void dbg_print();
void dbg_clear();

using TimePoint = std::chrono::milliseconds::rep;  // A value in milliseconds
static_assert(sizeof(TimePoint) == sizeof(int64_t), "TimePoint should be 64 bits");
inline TimePoint now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

inline std::vector<std::string_view> split(std::string_view s, std::string_view delimiter) {
    std::vector<std::string_view> res;

    if (s.empty())
        return res;

    size_t begin = 0;
    for (;;)
    {
        const size_t end = s.find(delimiter, begin);
        if (end == std::string::npos)
            break;

        res.emplace_back(s.substr(begin, end - begin));
        begin = end + delimiter.size();
    }

    res.emplace_back(s.substr(begin));

    return res;
}

void remove_whitespace(std::string& s);
bool is_whitespace(std::string_view s);

enum SyncCout {
    IO_LOCK,
    IO_UNLOCK
};
std::ostream& operator<<(std::ostream&, SyncCout);

#define sync_cout std::cout << IO_LOCK
#define sync_endl std::endl << IO_UNLOCK

void sync_cout_start();
void sync_cout_end();

// True if and only if the binary is compiled on a little-endian machine
static inline const std::uint16_t Le             = 1;
static inline const bool          IsLittleEndian = *reinterpret_cast<const char*>(&Le) == 1;


template<typename T, std::size_t MaxSize>
class ValueList {

   public:
    std::size_t size() const { return size_; }
    void        push_back(const T& value) { values_[size_++] = value; }
    const T*    begin() const { return values_; }
    const T*    end() const { return values_ + size_; }
    const T&    operator[](int index) const { return values_[index]; }

   private:
    T           values_[MaxSize];
    std::size_t size_ = 0;
};


template<typename T, std::size_t Size, std::size_t... Sizes>
class MultiArray;

namespace Detail {

template<typename T, std::size_t Size, std::size_t... Sizes>
struct MultiArrayHelper {
    using ChildType = MultiArray<T, Sizes...>;
};

template<typename T, std::size_t Size>
struct MultiArrayHelper<T, Size> {
    using ChildType = T;
};

template<typename To, typename From>
constexpr bool is_strictly_assignable_v =
  std::is_assignable_v<To&, From> && (std::is_same_v<To, From> || !std::is_convertible_v<From, To>);

}

// MultiArray is a generic N-dimensional array.
// The template parameters (Size and Sizes) encode the dimensions of the array.
template<typename T, std::size_t Size, std::size_t... Sizes>
class MultiArray {
    using ChildType = typename Detail::MultiArrayHelper<T, Size, Sizes...>::ChildType;
    using ArrayType = std::array<ChildType, Size>;
    ArrayType data_;

   public:
    using value_type             = typename ArrayType::value_type;
    using size_type              = typename ArrayType::size_type;
    using difference_type        = typename ArrayType::difference_type;
    using reference              = typename ArrayType::reference;
    using const_reference        = typename ArrayType::const_reference;
    using pointer                = typename ArrayType::pointer;
    using const_pointer          = typename ArrayType::const_pointer;
    using iterator               = typename ArrayType::iterator;
    using const_iterator         = typename ArrayType::const_iterator;
    using reverse_iterator       = typename ArrayType::reverse_iterator;
    using const_reverse_iterator = typename ArrayType::const_reverse_iterator;

    constexpr auto&       at(size_type index) noexcept { return data_.at(index); }
    constexpr const auto& at(size_type index) const noexcept { return data_.at(index); }

    constexpr auto&       operator[](size_type index) noexcept { return data_[index]; }
    constexpr const auto& operator[](size_type index) const noexcept { return data_[index]; }

    constexpr auto&       front() noexcept { return data_.front(); }
    constexpr const auto& front() const noexcept { return data_.front(); }
    constexpr auto&       back() noexcept { return data_.back(); }
    constexpr const auto& back() const noexcept { return data_.back(); }

    auto*       data() { return data_.data(); }
    const auto* data() const { return data_.data(); }

    constexpr auto begin() noexcept { return data_.begin(); }
    constexpr auto end() noexcept { return data_.end(); }
    constexpr auto begin() const noexcept { return data_.begin(); }
    constexpr auto end() const noexcept { return data_.end(); }
    constexpr auto cbegin() const noexcept { return data_.cbegin(); }
    constexpr auto cend() const noexcept { return data_.cend(); }

    constexpr auto rbegin() noexcept { return data_.rbegin(); }
    constexpr auto rend() noexcept { return data_.rend(); }
    constexpr auto rbegin() const noexcept { return data_.rbegin(); }
    constexpr auto rend() const noexcept { return data_.rend(); }
    constexpr auto crbegin() const noexcept { return data_.crbegin(); }
    constexpr auto crend() const noexcept { return data_.crend(); }

    constexpr bool      empty() const noexcept { return data_.empty(); }
    constexpr size_type size() const noexcept { return data_.size(); }
    constexpr size_type max_size() const noexcept { return data_.max_size(); }

    template<typename U>
    void fill(const U& v) {
        static_assert(Detail::is_strictly_assignable_v<T, U>,
                      "Cannot assign fill value to entry type");
        for (auto& ele : data_)
        {
            if constexpr (sizeof...(Sizes) == 0)
                ele = v;
            else
                ele.fill(v);
        }
    }

    constexpr void swap(MultiArray<T, Size, Sizes...>& other) noexcept { data_.swap(other.data_); }
};


// xorshift64star Pseudo-Random Number Generator
// This class is based on original code written and dedicated
// to the public domain by Sebastiano Vigna (2014).
// It has the following characteristics:
//
//  -  Outputs 64-bit numbers
//  -  Passes Dieharder and SmallCrush test batteries
//  -  Does not require warm-up, no zeroland to escape
//  -  Internal state is a single 64-bit integer
//  -  Period is 2^64 - 1
//  -  Speed: 1.60 ns/call (Core i7 @3.40GHz)
//
// For further analysis see
//   <http://vigna.di.unimi.it/ftp/papers/xorshift.pdf>

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

    // Special generator used to fast init magic numbers.
    // Output values only have 1/8th of their bits set on average.
    template<typename T>
    T sparse_rand() {
        return T(rand64() & rand64() & rand64());
    }
};

inline uint64_t mul_hi64(uint64_t a, uint64_t b) {
#if defined(__GNUC__) && defined(IS_64BIT)
    __extension__ using uint128 = unsigned __int128;
    return (uint128(a) * uint128(b)) >> 64;
#else
    uint64_t aL = uint32_t(a), aH = a >> 32;
    uint64_t bL = uint32_t(b), bH = b >> 32;
    uint64_t c1 = (aL * bL) >> 32;
    uint64_t c2 = aH * bL + c1;
    uint64_t c3 = aL * bH + uint32_t(c2);
    return aH * bH + (c2 >> 32) + (c3 >> 32);
#endif
}


struct CommandLine {
   public:
    CommandLine(int, char**);  //from learning

    int    argc;
    char** argv;
    //from learning begin
    std::string        binaryDirectory;   // path of the executable directory
    std::string        workingDirectory;  // path of the working directory
    static std::string get_working_directory();
    static std::string get_binary_directory(std::string argv0, std::string workingDirectory);
    //from learning end
};

namespace Utility {

template<typename T, typename Predicate>
void move_to_front(std::vector<T>& vec, Predicate pred) {
    auto it = std::find_if(vec.begin(), vec.end(), pred);

    if (it != vec.end())
    {
        std::rotate(vec.begin(), it, it + 1);
    }
}
}

#if defined(__GNUC__) && !defined(__clang__)
    #if __GNUC__ >= 13
        #define sf_assume(cond) __attribute__((assume(cond)))
    #else
        #define sf_assume(cond) \
            do \
            { \
                if (!(cond)) \
                    __builtin_unreachable(); \
            } while (0)
    #endif
#else
    // do nothing for other compilers
    #define sf_assume(cond)
#endif

//begin from learning
#define EMPTY "<empty>"

class Util {
   private:
#if defined(_WIN32) || defined(_WIN64)
    static constexpr char DirectorySeparator        = '\\';
    static constexpr char ReverseDirectorySeparator = '/';
#else
    static constexpr char DirectorySeparator        = '/';
    static constexpr char ReverseDirectorySeparator = '\\';
#endif
    static CommandLine* cli;

   public:
    static void        init(CommandLine* _cli);
    static std::string unquote(const std::string& s);
    static bool        is_empty_filename(const std::string& f);
    static std::string fix_path(const std::string& p);
    static std::string combine_path(const std::string& p1, const std::string& p2);
    static std::string map_path(const std::string& p);

    static size_t get_file_size(const std::string& f);
    static bool   is_same_file(const std::string& f1, const std::string& f2);

    static std::string format_bytes(uint64_t bytes, int decimals);

    static std::string format_string(const char* const fmt, ...);
};
//end from learning

}  // namespace ShashChess

#endif  // #ifndef MISC_H_INCLUDED
