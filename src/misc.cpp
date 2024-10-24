/*
  ShashChess, a UCI chess playing engine derived from Stockfish
  Copyright (C) 2004-2024 Andrea Manzo, F. Ferraguti, K.Kiniama and ShashChess developers (see AUTHORS file)

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

#include "misc.h"

#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string_view>
#include <system_error>
//from ShashChess begin
#include <algorithm>
#include <stdarg.h>
//from ShashChess end

#include "types.h"

namespace ShashChess {

namespace {

// Version number or dev.
constexpr std::string_view version = "37";

// Our fancy logging facility. The trick here is to replace cin.rdbuf() and
// cout.rdbuf() with two Tie objects that tie cin and cout to a file stream. We
// can toggle the logging of std::cout and std:cin at runtime whilst preserving
// usual I/O functionality, all without changing a single line of code!
// Idea from http://groups.google.com/group/comp.lang.c++/msg/1d941c0f26ea0d81

struct Tie: public std::streambuf {  // MSVC requires split streambuf for cin and cout

    Tie(std::streambuf* b, std::streambuf* l) :
        buf(b),
        logBuf(l) {}

    int sync() override { return logBuf->pubsync(), buf->pubsync(); }
    int overflow(int c) override { return log(buf->sputc(char(c)), "<< "); }
    int underflow() override { return buf->sgetc(); }
    int uflow() override { return log(buf->sbumpc(), ">> "); }

    std::streambuf *buf, *logBuf;

    int log(int c, const char* prefix) {

        static int last = '\n';  // Single log file

        if (last == '\n')
            logBuf->sputn(prefix, 3);

        return last = logBuf->sputc(char(c));
    }
};

class Logger {

    Logger() :
        in(std::cin.rdbuf(), file.rdbuf()),
        out(std::cout.rdbuf(), file.rdbuf()) {}
    ~Logger() { start(""); }

    std::ofstream file;
    Tie           in, out;

   public:
    static void start(const std::string& fname) {

        static Logger l;

        if (l.file.is_open())
        {
            std::cout.rdbuf(l.out.buf);
            std::cin.rdbuf(l.in.buf);
            l.file.close();
        }

        if (!fname.empty())
        {
            l.file.open(fname, std::ifstream::out);

            if (!l.file.is_open())
            {
                std::cerr << "Unable to open debug log file " << fname << std::endl;
                exit(EXIT_FAILURE);
            }

            std::cin.rdbuf(&l.in);
            std::cout.rdbuf(&l.out);
        }
    }
};

}  // namespace


// Returns the full name of the current Stockfish version.
//
// For local dev compiles we try to append the commit SHA and
// commit date from git. If that fails only the local compilation
// date is set and "nogit" is specified:
//      Stockfish dev-YYYYMMDD-SHA
//      or
//      Stockfish dev-YYYYMMDD-nogit
//
// For releases (non-dev builds) we only include the version number:
//      ShashChess version
std::string engine_version_info() {
    std::stringstream ss;
    ss << "ShashChess " << version << std::setfill('0');

    if constexpr (version == "dev")
    {
        ss << "-";
#ifdef GIT_DATE
        ss << stringify(GIT_DATE);
#else
        constexpr std::string_view months("Jan Feb Mar Apr May Jun Jul Aug Sep Oct Nov Dec");
        std::string                month, day, year;
        std::stringstream          date(__DATE__);  // From compiler, format is "Sep 21 2008"

        date >> month >> day >> year;
        ss << year << std::setw(2) << std::setfill('0') << (1 + months.find(month) / 4)
           << std::setw(2) << std::setfill('0') << day;
#endif

        ss << "-";

#ifdef GIT_SHA
        ss << stringify(GIT_SHA);
#else
        ss << "nogit";
#endif
    }

    return ss.str();
}

std::string engine_info(bool to_uci) {
    return engine_version_info() + (to_uci ? "\nid author " : " by ")
         + "A. Manzo, F. Ferraguti, K. Kiniama and Stockfish developers (see AUTHORS file)";
}


// Returns a string trying to describe the compiler we use
std::string compiler_info() {

#define make_version_string(major, minor, patch) \
    stringify(major) "." stringify(minor) "." stringify(patch)

    // Predefined macros hell:
    //
    // __GNUC__                Compiler is GCC, Clang or ICX
    // __clang__               Compiler is Clang or ICX
    // __INTEL_LLVM_COMPILER   Compiler is ICX
    // _MSC_VER                Compiler is MSVC
    // _WIN32                  Building on Windows (any)
    // _WIN64                  Building on Windows 64 bit

    std::string compiler = "\nCompiled by                : ";

#if defined(__INTEL_LLVM_COMPILER)
    compiler += "ICX ";
    compiler += stringify(__INTEL_LLVM_COMPILER);
#elif defined(__clang__)
    compiler += "clang++ ";
    compiler += make_version_string(__clang_major__, __clang_minor__, __clang_patchlevel__);
#elif _MSC_VER
    compiler += "MSVC ";
    compiler += "(version ";
    compiler += stringify(_MSC_FULL_VER) "." stringify(_MSC_BUILD);
    compiler += ")";
#elif defined(__e2k__) && defined(__LCC__)
    #define dot_ver2(n) \
        compiler += char('.'); \
        compiler += char('0' + (n) / 10); \
        compiler += char('0' + (n) % 10);

    compiler += "MCST LCC ";
    compiler += "(version ";
    compiler += std::to_string(__LCC__ / 100);
    dot_ver2(__LCC__ % 100) dot_ver2(__LCC_MINOR__) compiler += ")";
#elif __GNUC__
    compiler += "g++ (GNUC) ";
    compiler += make_version_string(__GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#else
    compiler += "Unknown compiler ";
    compiler += "(unknown version)";
#endif

#if defined(__APPLE__)
    compiler += " on Apple";
#elif defined(__CYGWIN__)
    compiler += " on Cygwin";
#elif defined(__MINGW64__)
    compiler += " on MinGW64";
#elif defined(__MINGW32__)
    compiler += " on MinGW32";
#elif defined(__ANDROID__)
    compiler += " on Android";
#elif defined(__linux__)
    compiler += " on Linux";
#elif defined(_WIN64)
    compiler += " on Microsoft Windows 64-bit";
#elif defined(_WIN32)
    compiler += " on Microsoft Windows 32-bit";
#else
    compiler += " on unknown system";
#endif

    compiler += "\nCompilation architecture   : ";
#if defined(ARCH)
    compiler += stringify(ARCH);
#else
    compiler += "(undefined architecture)";
#endif

    compiler += "\nCompilation settings       : ";
    compiler += (Is64Bit ? "64bit" : "32bit");
#if defined(USE_VNNI)
    compiler += " VNNI";
#endif
#if defined(USE_AVX512)
    compiler += " AVX512";
#endif
    compiler += (HasPext ? " BMI2" : "");
#if defined(USE_AVX2)
    compiler += " AVX2";
#endif
#if defined(USE_SSE41)
    compiler += " SSE41";
#endif
#if defined(USE_SSSE3)
    compiler += " SSSE3";
#endif
#if defined(USE_SSE2)
    compiler += " SSE2";
#endif
    compiler += (HasPopCnt ? " POPCNT" : "");
#if defined(USE_NEON_DOTPROD)
    compiler += " NEON_DOTPROD";
#elif defined(USE_NEON)
    compiler += " NEON";
#endif

#if !defined(NDEBUG)
    compiler += " DEBUG";
#endif

    compiler += "\nCompiler __VERSION__ macro : ";
#ifdef __VERSION__
    compiler += __VERSION__;
#else
    compiler += "(undefined macro)";
#endif

    compiler += "\n";

    return compiler;
}


// Debug functions used mainly to collect run-time statistics
constexpr int MaxDebugSlots = 32;

namespace {

template<size_t N>
struct DebugInfo {
    std::atomic<int64_t> data[N] = {0};

    constexpr std::atomic<int64_t>& operator[](int index) { return data[index]; }
};

struct DebugExtremes: public DebugInfo<3> {
    DebugExtremes() {
        data[1] = std::numeric_limits<int64_t>::min();
        data[2] = std::numeric_limits<int64_t>::max();
    }
};

DebugInfo<2>  hit[MaxDebugSlots];
DebugInfo<2>  mean[MaxDebugSlots];
DebugInfo<3>  stdev[MaxDebugSlots];
DebugInfo<6>  correl[MaxDebugSlots];
DebugExtremes extremes[MaxDebugSlots];

}  // namespace

void dbg_hit_on(bool cond, int slot) {

    ++hit[slot][0];
    if (cond)
        ++hit[slot][1];
}

void dbg_mean_of(int64_t value, int slot) {

    ++mean[slot][0];
    mean[slot][1] += value;
}

void dbg_stdev_of(int64_t value, int slot) {

    ++stdev[slot][0];
    stdev[slot][1] += value;
    stdev[slot][2] += value * value;
}

void dbg_extremes_of(int64_t value, int slot) {
    ++extremes[slot][0];

    int64_t current_max = extremes[slot][1].load();
    while (current_max < value && !extremes[slot][1].compare_exchange_weak(current_max, value))
    {}

    int64_t current_min = extremes[slot][2].load();
    while (current_min > value && !extremes[slot][2].compare_exchange_weak(current_min, value))
    {}
}

void dbg_correl_of(int64_t value1, int64_t value2, int slot) {

    ++correl[slot][0];
    correl[slot][1] += value1;
    correl[slot][2] += value1 * value1;
    correl[slot][3] += value2;
    correl[slot][4] += value2 * value2;
    correl[slot][5] += value1 * value2;
}

void dbg_print() {

    int64_t n;
    auto    E   = [&n](int64_t x) { return double(x) / n; };
    auto    sqr = [](double x) { return x * x; };

    for (int i = 0; i < MaxDebugSlots; ++i)
        if ((n = hit[i][0]))
            std::cerr << "Hit #" << i << ": Total " << n << " Hits " << hit[i][1]
                      << " Hit Rate (%) " << 100.0 * E(hit[i][1]) << std::endl;

    for (int i = 0; i < MaxDebugSlots; ++i)
        if ((n = mean[i][0]))
        {
            std::cerr << "Mean #" << i << ": Total " << n << " Mean " << E(mean[i][1]) << std::endl;
        }

    for (int i = 0; i < MaxDebugSlots; ++i)
        if ((n = stdev[i][0]))
        {
            double r = sqrt(E(stdev[i][2]) - sqr(E(stdev[i][1])));
            std::cerr << "Stdev #" << i << ": Total " << n << " Stdev " << r << std::endl;
        }

    for (int i = 0; i < MaxDebugSlots; ++i)
        if ((n = extremes[i][0]))
        {
            std::cerr << "Extremity #" << i << ": Total " << n << " Min " << extremes[i][2]
                      << " Max " << extremes[i][1] << std::endl;
        }

    for (int i = 0; i < MaxDebugSlots; ++i)
        if ((n = correl[i][0]))
        {
            double r = (E(correl[i][5]) - E(correl[i][1]) * E(correl[i][3]))
                     / (sqrt(E(correl[i][2]) - sqr(E(correl[i][1])))
                        * sqrt(E(correl[i][4]) - sqr(E(correl[i][3]))));
            std::cerr << "Correl. #" << i << ": Total " << n << " Coefficient " << r << std::endl;
        }
}


// Used to serialize access to std::cout
// to avoid multiple threads writing at the same time.
std::ostream& operator<<(std::ostream& os, SyncCout sc) {

    static std::mutex m;

    if (sc == IO_LOCK)
        m.lock();

    if (sc == IO_UNLOCK)
        m.unlock();

    return os;
}

void sync_cout_start() { std::cout << IO_LOCK; }
void sync_cout_end() { std::cout << IO_UNLOCK; }

// Trampoline helper to avoid moving Logger to misc.h
void start_logger(const std::string& fname) { Logger::start(fname); }


#ifdef NO_PREFETCH

void prefetch(const void*) {}

#else

void prefetch(const void* addr) {

    #if defined(_MSC_VER)
    _mm_prefetch((char const*) addr, _MM_HINT_T0);
    #else
    __builtin_prefetch(addr);
    #endif
}

#endif

#ifdef _WIN32
    #include <direct.h>
    #define GETCWD _getcwd
#else
    #include <unistd.h>
    #define GETCWD getcwd
#endif

size_t str_to_size_t(const std::string& s) {
    unsigned long long value = std::stoull(s);
    if (value > std::numeric_limits<size_t>::max())
        std::exit(EXIT_FAILURE);
    return static_cast<size_t>(value);
}

std::optional<std::string> read_file_to_string(const std::string& path) {
    std::ifstream f(path, std::ios_base::binary);
    if (!f)
        return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

void remove_whitespace(std::string& s) {
    s.erase(std::remove_if(s.begin(), s.end(), [](char c) { return std::isspace(c); }), s.end());
}

bool is_whitespace(std::string_view s) {
    return std::all_of(s.begin(), s.end(), [](char c) { return std::isspace(c); });
}

//from Khalid begin
CommandLine::CommandLine(int _argc, char** _argv) :
    argc(_argc),
    argv(_argv) {
    workingDirectory = CommandLine::get_working_directory();
    binaryDirectory  = CommandLine::get_binary_directory(argv[0], workingDirectory);
    Util::init(this);
    //from Khalid end
}

std::string CommandLine::get_binary_directory(std::string argv0, std::string workingDirectory) {
    std::string pathSeparator;
#ifdef _WIN32
    pathSeparator = "\\";
    #ifdef _MSC_VER
    // Under windows argv[0] may not have the extension. Also _get_pgmptr() had
    // issues in some Windows 10 versions, so check returned values carefully.
    char* pgmptr = nullptr;
    if (!_get_pgmptr(&pgmptr) && pgmptr != nullptr && *pgmptr)
        argv0 = pgmptr;
    #endif
#else
    pathSeparator = "/";
#endif
    //from Khalid end
    // Extract the binary directory path from argv0
    auto   binaryDirectory = argv0;
    size_t pos             = binaryDirectory.find_last_of("\\/");
    if (pos == std::string::npos)
        binaryDirectory = "." + pathSeparator;
    else
        binaryDirectory.resize(pos + 1);

    // Pattern replacement: "./" at the start of path is replaced by the working directory
    //from Khalid begin
    if (binaryDirectory.find("." + pathSeparator) == 0)
    {
        binaryDirectory.replace(0, 1, workingDirectory);
    }
    binaryDirectory = Util::fix_path(binaryDirectory);
    //from Khalid end
    return binaryDirectory;
}

std::string CommandLine::get_working_directory() {
    std::string workingDirectory = "";
    char        buff[40000];
    char*       cwd = GETCWD(buff, 40000);
    //from Khalid begin
    if (cwd)
    {
        workingDirectory = cwd;
    }
    workingDirectory = Util::fix_path(workingDirectory);  //khalid
    //from Khalid end
    return workingDirectory;
}

//Book management and learning begin
CommandLine* Util::cli = nullptr;
void         Util::init(CommandLine* _cli) { cli = _cli; }
std::string  Util::unquote(const std::string& s) {
    std::string s1 = s;

    if (s1.size() > 2)
    {
        if ((s1.front() == '\"' && s1.back() == '\"') || (s1.front() == '\'' && s1.back() == '\''))
        {
            s1 = s1.substr(1, s1.size() - 2);
        }
    }

    return s1;
}

bool Util::is_empty_filename(const std::string& fn) {
    if (fn.empty())
        return true;

    static std::string Empty = EMPTY;
    return std::equal(fn.begin(), fn.end(), Empty.begin(), Empty.end(),
                      [](char a, char b) { return tolower(a) == tolower(b); });
}

std::string Util::fix_path(const std::string& p) {
    if (is_empty_filename(p))
        return p;

    std::string p1 = unquote(p);
    std::replace(p1.begin(), p1.end(), ReverseDirectorySeparator, DirectorySeparator);

    return p1;
}

std::string Util::combine_path(const std::string& p1, const std::string& p2) {
    //We don't expect the first part of the path to be empty!
    assert(is_empty_filename(p1) == false);

    if (is_empty_filename(p2))
        return p2;

    std::string p;
    if (p1.back() == DirectorySeparator || p1.back() == ReverseDirectorySeparator)
        p = p1 + p2;
    else
        p = p1 + DirectorySeparator + p2;

    return fix_path(p);
}

std::string Util::map_path(const std::string& p) {
    if (is_empty_filename(p))
        return p;

    std::string p2 = fix_path(p);

    //Make sure we can map this path
    if (p2.find(DirectorySeparator) == std::string::npos)
        p2 = combine_path(cli->binaryDirectory, p);

    return p2;
}

size_t Util::get_file_size(const std::string& f) {
    if (is_empty_filename(f))
        return (size_t) -1;

    std::ifstream in(map_path(f), std::ifstream::ate | std::ifstream::binary);
    if (!in.is_open())
        return (size_t) -1;

    return (size_t) in.tellg();
}

bool Util::is_same_file(const std::string& f1, const std::string& f2) {
    return map_path(f1) == map_path(f2);
}

std::string Util::format_bytes(uint64_t bytes, int decimals) {
    static const uint64_t KB = 1024;
    static const uint64_t MB = KB * 1024;
    static const uint64_t GB = MB * 1024;
    static const uint64_t TB = GB * 1024;

    std::stringstream ss;

    if (bytes < KB)
        ss << bytes << " B";
    else if (bytes < MB)
        ss << std::fixed << std::setprecision(decimals) << (double(bytes) / KB) << "KB";
    else if (bytes < GB)
        ss << std::fixed << std::setprecision(decimals) << (double(bytes) / MB) << "MB";
    else if (bytes < TB)
        ss << std::fixed << std::setprecision(decimals) << (double(bytes) / GB) << "GB";
    else
        ss << std::fixed << std::setprecision(decimals) << (double(bytes) / TB) << "TB";

    return ss.str();
}

//Code is an `edited` version of: https://stackoverflow.com/a/49812018
std::string Util::format_string(const char* const fmt, ...) {
    //Initialize use of the variable arguments
    va_list vaArgs;
    va_start(vaArgs, fmt);

    //Acquire the required string size
    va_start(vaArgs, fmt);
    int len = vsnprintf(nullptr, 0, fmt, vaArgs);
    va_end(vaArgs);


    //Allocate enough buffer and format
    std::vector<char> v(len + 1);

    va_start(vaArgs, fmt);
    vsnprintf(v.data(), v.size(), fmt, vaArgs);
    va_end(vaArgs);

    return std::string(v.data(), len);
}
//Book management and learning end


}  // namespace ShashChess
