# CLAUDE.md - AI Assistant Guide for ShashChess

## Table of Contents
1. [Project Overview](#project-overview)
2. [Repository Structure](#repository-structure)
3. [Build System](#build-system)
4. [Development Workflow](#development-workflow)
5. [Code Conventions](#code-conventions)
6. [Key Components](#key-components)
7. [Testing](#testing)
8. [Common Tasks](#common-tasks)
9. [Important Notes](#important-notes)

---

## Project Overview

**ShashChess** is a free, open-source UCI chess engine derived from Stockfish. It implements Alexander Shashin's chess theory to provide position-type-aware algorithms.

### Key Characteristics
- **Base:** Stockfish family engines
- **Language:** C++17
- **Lines of Code:** ~29,343 lines (48 .cpp files, 65 .h files)
- **License:** GNU General Public License v3
- **Platforms:** Linux, macOS, Windows (32/64-bit), ARM, PPC64
- **Main Developer:** ICCF IM Andrea Manzo

### Unique Features
1. **Shashin Theory Integration:** Recognizes position types (Tal/Capablanca/Petrosian) and adapts search strategy accordingly
2. **Hybrid Search:** Combines alpha-beta search with Monte Carlo Tree Search (MCTS) for specific position types
3. **Dual-Network NNUE:** Uses both "big" and "small" neural networks for different position complexities
4. **Self-Learning System:** Q-learning and experience-based reinforcement learning
5. **Online Opening Book:** Integrates with Lichess and ChessDB APIs

---

## Repository Structure

```
ShashChess/
├── src/                          # Main source code
│   ├── Core Engine:
│   │   ├── main.cpp             # Entry point
│   │   ├── engine.cpp/h         # Main engine controller
│   │   ├── uci.cpp/h            # UCI protocol implementation
│   │   ├── search.cpp/h         # Search algorithm (3,722 lines)
│   │   ├── position.cpp/h       # Chess position representation (2,146 lines)
│   │   ├── evaluate.cpp/h       # Evaluation function
│   │   ├── movegen.cpp/h        # Move generation
│   │   ├── movepick.cpp/h       # Move ordering
│   │   ├── thread.cpp/h         # Multi-threading
│   │   ├── timeman.cpp/h        # Time management
│   │   ├── tt.cpp/h             # Transposition table
│   │   └── benchmark.cpp/h      # Benchmarking
│   │
│   ├── nnue/                     # NNUE neural network evaluation
│   │   ├── network.cpp/h        # Network interface
│   │   ├── nnue_accumulator.*   # Incremental evaluation
│   │   ├── features/            # Feature extractors (HalfKA v2)
│   │   └── layers/              # Network layers (affine, ReLU)
│   │
│   ├── shashin/                  # Shashin position type recognition
│   │   ├── shashin_manager.*    # Main Shashin manager
│   │   ├── shashin_position.h   # Position type utilities
│   │   └── moveconfig.*         # Move configuration
│   │
│   ├── livebook/                 # Online opening books
│   │   ├── BaseLivebook.*       # Base class
│   │   ├── Lichess*.cpp/h       # Lichess integration
│   │   ├── ChessDb.*            # ChessDB integration
│   │   ├── analysis/            # Analysis components
│   │   └── json/                # JSON parsing (json.hpp)
│   │
│   ├── mcts/                     # Monte Carlo Tree Search
│   │   └── montecarlo.cpp/h     # MCTS implementation
│   │
│   ├── wdl/                      # Win/Draw/Loss probability
│   │   └── win_probability.*    # WDL model
│   │
│   ├── learn/                    # Self-learning system
│   │   └── learn.cpp/h          # Learning module
│   │
│   ├── book/                     # Opening book management
│   │   ├── book.*               # Book interface
│   │   ├── book_manager.*       # Manager
│   │   ├── polyglot/            # Polyglot format
│   │   └── ctg/                 # CTG format
│   │
│   ├── syzygy/                   # Endgame tablebases
│   │   └── tbprobe.cpp/h        # Syzygy probing
│   │
│   ├── Makefile                  # Primary build system
│   ├── CMakeLists.txt            # CMake build system
│   └── makeAll.sh/bat            # Build scripts
│
├── tests/                        # Test infrastructure
│   ├── perft.sh                 # Correctness testing
│   ├── testing.py               # Test framework
│   ├── Tests/                   # General test suites
│   ├── TestSuiteCenterType/     # Position-type tests
│   └── HardPositions/           # Difficult positions
│
├── scripts/                      # Utility scripts
│   ├── get_native_properties.sh # CPU detection
│   └── net.sh                   # NNUE network downloader
│
├── doc/                          # Documentation
├── .github/workflows/            # CI/CD pipelines
├── .clang-format                 # Code style configuration
├── README.md                     # User documentation
└── CONTRIBUTING.md               # Contribution guidelines
```

---

## Build System

### Primary Build Tool: GNU Make

**Location:** `src/Makefile`

### Building from Source

```bash
cd src

# Basic build (native architecture)
make build ARCH=native

# Specific architecture builds
make build ARCH=x86-64-avx512
make build ARCH=x86-64-bmi2
make build ARCH=armv8

# Debug build
make build ARCH=x86-64-modern debug=yes

# With sanitizers
make build ARCH=x86-64-modern sanitize="address undefined"

# Clean build
make clean
```

### Supported Architectures

| Architecture | Description |
|--------------|-------------|
| `x86-64-avx512icl` | Intel Ice Lake / AMD Zen 4 with all AVX-512 features |
| `x86-64-vnni512` | AVX-512 with VNNI instructions |
| `x86-64-avx512` | AVX-512 support |
| `x86-64-avxvnni` | AVX with VNNI instructions |
| `x86-64-bmi2` | BMI2 instructions |
| `x86-64-avx2` | AVX2 instructions (recommended for modern CPUs) |
| `x86-64-modern` | SSE4.1 + POPCNT (good default) |
| `x86-64` | Basic x86-64 |
| `armv8-dotprod` | ARM v8 with dot product |
| `armv8` | ARM v8 / Apple Silicon |
| `ppc-64-vsx` | PowerPC 64-bit with VSX |
| `loongarch64` | Loongson architecture |
| `native` | Auto-detect (default) |

### Key Build Flags

| Flag | Values | Description |
|------|--------|-------------|
| `ARCH` | See above | Target CPU architecture |
| `COMP` | gcc, clang, mingw, icx | Compiler choice |
| `debug` | yes/no | Enable debug symbols |
| `optimize` | yes/no | Enable optimizations (-O3) |
| `sanitize` | address, undefined, thread | Enable sanitizers |
| `bits` | 64/32 | 64 or 32-bit build |
| `prefetch` | yes/no | Enable prefetch instructions |

### Build Outputs

- **Linux/macOS:** `shashchess` executable
- **Windows:** `shashchess.exe` executable
- **Installation directory:** `/usr/local/bin` (via `make install`)

### Alternative Build Systems

- **CMake:** `cd src && cmake . && make`
- **Windows batch scripts:** `makeAll64.bat`, `makeBMI2.bat`, etc.
- **Linux script:** `./makeAll.sh`

---

## Development Workflow

### Setting Up Development Environment

1. **Clone the repository:**
   ```bash
   git clone https://github.com/amchess/ShashChess.git
   cd ShashChess
   ```

2. **Install dependencies:**
   - C++17 compiler (GCC 9+, Clang 10+, MSVC 2019+)
   - Make or CMake
   - clang-format 20 (for code formatting)

3. **Download NNUE network:**
   ```bash
   cd scripts
   ./net.sh
   ```

4. **Build:**
   ```bash
   cd src
   make build ARCH=native
   ```

5. **Test:**
   ```bash
   ./shashchess bench
   ```

### Contribution Workflow

1. **Create a branch** for your feature/fix
2. **Make changes** following code conventions
3. **Format code:**
   ```bash
   cd src
   make format  # Requires clang-format 20
   ```
4. **Test changes:**
   - Functional changes MUST be tested on fishtest
   - Non-functional changes (refactoring) don't require fishtest unless performance-impacting
5. **Update bench score** if needed
6. **Submit pull request** with:
   - Clear description
   - Link to fishtest results (if applicable)
   - New bench score (if changed)

### Git Configuration

```bash
# Ignore formatting commits in git blame
git config blame.ignoreRevsFile .git-blame-ignore-revs
```

---

## Code Conventions

### Code Style

**Enforced by:** `.clang-format` (clang-format 20)

**Key Style Rules:**
- **Base style:** WebKit
- **Column limit:** 100 characters
- **Indentation:** 4 spaces
- **Line endings:** LF (Unix style)
- **Aligned:** Consecutive assignments and declarations
- **Brace wrapping:** Custom (after control statements, before else)
- **No spaces after C-style casts**
- **Include sorting:** Disabled (manual control)

**Formatting:**
```bash
cd src
make format
```

### Naming Conventions

| Element | Convention | Example |
|---------|------------|---------|
| **Classes** | PascalCase | `TranspositionTable`, `Position` |
| **Functions** | snake_case | `search()`, `do_move()` |
| **Variables** | camelCase or snake_case | `rootDepth`, `move_count` |
| **Constants** | UPPER_SNAKE_CASE | `MAX_MOVES`, `VALUE_INFINITE` |
| **Namespaces** | PascalCase | `ShashChess::Eval::NNUE` |
| **Enums** | PascalCase members | `Color::WHITE`, `PieceType::KNIGHT` |

### Code Organization

**Namespace structure:**
```cpp
namespace ShashChess {
    namespace Bitboards { }
    namespace Eval {
        namespace NNUE { }
    }
    namespace Search { }
}
```

**Header guards:** Use `#ifndef HEADER_NAME_H_INCLUDED`

**Include order:**
1. Corresponding header (for .cpp files)
2. C++ standard library
3. Third-party libraries
4. Project headers

### Documentation

- Use `//` for single-line comments
- Use `/* */` for multi-line comments
- Document complex algorithms and non-obvious logic
- Include references to papers/techniques where applicable

---

## Key Components

### 1. Search Algorithm (search.cpp/h)

**Location:** `src/search.cpp` (3,722 lines)

**Key functions:**
- `search()` - Main search loop with iterative deepening
- `search_pv()` - PV node search
- `search_nonpv()` - Non-PV node search
- `qsearch()` - Quiescence search for tactical positions

**Techniques implemented:**
- Alpha-beta pruning with transposition tables
- Iterative deepening with aspiration windows
- Late Move Reductions (LMR)
- Null move pruning
- Futility pruning
- Razoring
- SEE (Static Exchange Evaluation)
- Killer moves and history heuristics
- Multi-PV support

**Shashin integration:** Position type affects search parameters

### 2. Position Representation (position.cpp/h)

**Location:** `src/position.cpp` (2,146 lines)

**Key responsibilities:**
- Board state management
- FEN parsing/generation
- Move making/unmaking
- Legal move checking
- Zobrist hashing
- Piece lists and bitboards

**State tracking:** `StateInfo` struct maintains reversible state

### 3. NNUE Evaluation (nnue/)

**Location:** `src/nnue/`

**Architecture:**
- **Input:** HalfKA v2 with HM features (half_ka_v2_hm.cpp/h)
- **Hidden layers:** Affine transforms with ReLU activation
- **Output:** Evaluation score

**Key features:**
- Incremental updates via accumulator
- SIMD optimizations (AVX2/AVX512/NEON)
- Dual-network support (big/small nets)
- Network file auto-download

### 4. Shashin Theory (shashin/)

**Location:** `src/shashin/`

**Position types:**
- **Tal:** Tactical, aggressive (favor tactics)
- **Capablanca:** Balanced (standard search)
- **Petrosian:** Strategic, defensive (use MCTS)
- **Mixed:** Combinations of above

**Integration points:**
- Search parameter adjustment
- MCTS activation
- Move selection strategy

### 5. UCI Protocol (uci.cpp/h)

**Location:** `src/uci.cpp`

**Commands handled:**
- `uci` - Initialize UCI mode
- `isready` - Engine ready check
- `position` - Set position
- `go` - Start search
- `stop` - Stop search
- `setoption` - Set UCI option
- `quit` - Exit engine

### 6. Multi-Threading (thread.cpp/h)

**Location:** `src/thread.cpp`

**Features:**
- Thread pool management
- Lazy SMP (parallel search)
- Shared transposition table
- Work distribution
- Synchronization primitives

### 7. Transposition Table (tt.cpp/h)

**Location:** `src/tt.cpp`

**Features:**
- Hash table for position caching
- Replacement scheme with aging
- Configurable size (1-131072 MB)
- Multi-threaded access with clustering

---

## Testing

### Test Framework

**Location:** `tests/`

**Components:**
1. **Python test framework:** `testing.py`
2. **Perft testing:** `perft.sh` (correctness validation)
3. **Instrumented testing:** `instrumented.py`

### Test Suites

| Directory | Purpose |
|-----------|---------|
| `tests/Tests/` | General test positions, experience files |
| `tests/TestSuiteCenterType/` | Position-type specific tests (Tal/Capablanca/Petrosian) |
| `tests/HardPositions/` | Difficult positions (2023-256 suite) |
| `tests/KB/` | Knowledge base (ECO classifications, pawn structures) |

### Running Tests

```bash
# Built-in benchmark
./shashchess bench

# Perft testing (correctness)
cd tests
./perft.sh

# Python test framework
python testing.py
```

### Fishtest Integration

- **Website:** https://tests.stockfishchess.org/tests
- **Purpose:** Distributed testing for functional changes
- **Required for:** All functional changes (search, evaluation)
- **Not required for:** Refactoring, documentation (unless performance impact)

### CI/CD Pipelines

**Location:** `.github/workflows/`

**Workflows:**
- `compilation.yml` - Multi-platform compilation
- `tests.yml` - Test suite execution
- `sanitizers.yml` - Memory safety checks (ASan, UBSan, TSan)
- `clang-format.yml` - Code style enforcement
- `codeql.yml` - Security analysis
- `arm_compilation.yml` - ARM builds
- `games.yml` - Chess game testing

---

## Common Tasks

### Adding a New UCI Option

1. **Define option in `ucioption.cpp`:**
   ```cpp
   o["MyOption"] << Option(defaultValue, minValue, maxValue);
   ```

2. **Access in code:**
   ```cpp
   int value = options["MyOption"];
   ```

3. **Update documentation** in README.md

### Adding a New Search Feature

1. **Implement in `search.cpp`**
2. **Test on fishtest** (required!)
3. **Include link to test results** in PR
4. **Update bench score** if changed
5. **Document in commit message**

### Modifying NNUE

1. **Changes in `src/nnue/`**
2. **Ensure network compatibility** (may need new network file)
3. **Test thoroughly** - evaluation changes are critical
4. **Fishtest required**

### Adding Shashin Position Type

1. **Define in `shashin/shashin_types.h`**
2. **Implement recognition in `shashin_manager.cpp`**
3. **Adjust search parameters in `search.cpp`**
4. **Create test suite in `tests/TestSuiteCenterType/`**

### Debugging

```bash
# Build with debug symbols
make clean
make build ARCH=x86-64-modern debug=yes

# Run with debugger
gdb ./shashchess
(gdb) run

# Memory debugging
make build ARCH=x86-64-modern sanitize="address undefined"
./shashchess
```

### Performance Profiling

```bash
# Build with profiling
make profile-build ARCH=x86-64-modern

# Run to generate profile data
./shashchess bench

# Build optimized version using profile
make profile-use ARCH=x86-64-modern
```

---

## Important Notes

### For AI Assistants

1. **Always format code** before committing:
   ```bash
   cd src && make format
   ```

2. **Functional changes require fishtest:**
   - Any change affecting search or evaluation MUST be tested
   - Include test link in PR
   - Update bench score

3. **Preserve code structure:**
   - Follow existing patterns
   - Maintain namespace organization
   - Keep file organization consistent

4. **Documentation:**
   - Update README.md for user-visible changes
   - Update CLAUDE.md for structural changes
   - Comment complex algorithms

5. **No new features without discussion:**
   - ShashChess development is focused on improvements, not new features
   - Discuss in GitHub discussions or Discord first

6. **Performance matters:**
   - This is a performance-critical codebase
   - Avoid unnecessary allocations
   - Profile before optimizing
   - Benchmark changes

### Key Files to Know

| File | When to Modify |
|------|----------------|
| `search.cpp` | Search algorithm changes |
| `evaluate.cpp` | Evaluation changes |
| `uci.cpp` | UCI protocol changes |
| `ucioption.cpp` | Adding/modifying UCI options |
| `Makefile` | Build system changes |
| `types.h` | Core type definitions |
| `position.cpp` | Board representation |
| `movegen.cpp` | Move generation |
| `nnue/network.cpp` | NNUE network loading |
| `shashin/shashin_manager.cpp` | Shashin position recognition |

### Feature Flags

```cpp
USE_LIVEBOOK      // Online book integration (Lichess/ChessDB)
USE_POPCNT        // POPCNT instruction
USE_PEXT          // PEXT instruction
USE_PREFETCH      // Memory prefetch
NDEBUG            // Disable assertions (release mode)
```

### Important Constants

```cpp
VALUE_INFINITE = 32001      // Infinite score
VALUE_NONE = 32002          // No score
MAX_MOVES = 256             // Maximum legal moves
MAX_PLY = 246               // Maximum search depth
```

### UCI Options Reference

**Essential options:**
- `Hash` (1-131072 MB) - Transposition table size
- `Threads` (1-512) - Number of search threads
- `MultiPV` (1-500) - Number of PV lines
- `SyzygyPath` - Endgame tablebase path
- `Full Depth Threads` - Brute force threads
- `MCTS by Shashin` - Enable MCTS for Petrosian
- `Persisted learning` - Enable self-learning
- `LiveBook options` - Online book configuration

### Performance Tips

1. **Profile-Guided Optimization (PGO):**
   ```bash
   make profile-build ARCH=x86-64-avx2
   ./shashchess bench
   make profile-use ARCH=x86-64-avx2
   ```

2. **Large pages (Linux):**
   - Enables transparent huge pages
   - 5-30% performance improvement
   - Automatic if available

3. **NUMA systems:**
   - Set threads equal to cores per socket
   - Enable NUMA replication in code

4. **CPU architecture:**
   - Use highest supported ARCH (avx512 > avx2 > sse41)
   - `native` auto-detects optimal

### External Resources

- **ShashChess repository:** https://github.com/amchess/ShashChess
- **Discord:** https://discord.gg/GWDRS3kU6R
- **Fishtest:** https://tests.stockfishchess.org/tests
- **Chess Programming Wiki:** https://www.chessprogramming.org
- **Stockfish (upstream):** https://github.com/official-stockfish/Stockfish

### License

GNU General Public License v3.0 - see `Copying.txt`

**Key requirement:** Any distribution must include full source code or pointer to source.

---

## Quick Reference Card

```bash
# Build
cd src && make build ARCH=native

# Format code
cd src && make format

# Run benchmark
./shashchess bench

# Run tests
cd tests && ./perft.sh

# Clean build
cd src && make clean

# Debug build
cd src && make build ARCH=x86-64-modern debug=yes

# View help
cd src && make help

# Install
cd src && sudo make install
```

---

**Last Updated:** 2025-11-21 (ShashChess 40)

**Note:** This document should be updated whenever significant structural or workflow changes are made to the repository.
