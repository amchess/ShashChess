cmake_minimum_required(VERSION 3.17)
project(ShashChess)

# Determine operating system
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(TARGET_WINDOWS yes)
    set(EXE shashchess.exe)
else()
    set(EXE shashchess)
endif()

# Set default installation directory
set(PREFIX /usr/local)
set(BINDIR ${PREFIX}/bin)

# Source and header files
set(SRCS
        benchmark.cpp bitboard.cpp evaluate.cpp main.cpp 
        misc.cpp movegen.cpp movepick.cpp position.cpp
        search.cpp thread.cpp timeman.cpp tt.cpp uci.cpp ucioption.cpp tune.cpp syzygy/tbprobe.cpp
        learn/learn.cpp mcts/montecarlo.cpp
        book/file_mapping.cpp book/book.cpp book/book_manager.cpp book/polyglot/polyglot.cpp book/ctg/ctg.cpp
        nnue/nnue_misc.cpp nnue/features/half_ka_v2_hm.cpp nnue/network.cpp engine.cpp score.cpp memory.cpp
        win_probability.cpp
        livebook/BaseLivebook.cpp livebook/LichessOpening.cpp livebook/LichessEndgame.cpp livebook/ChessDb.cpp
        livebook/analysis/Cp.cpp livebook/analysis/Analysis.cpp livebook/analysis/Wdl.cpp livebook/analysis/Mate.cpp
        livebook/LichessLivebook.cpp livebook/LichessMaster.cpp livebook/LichessPlayer.cpp livebook/LichessUsers.cpp
        livebook/LichessGames.cpp livebook/Proxy.cpp livebook/ChessDBContributor.cpp
)

set(HEADERS
        benchmark.h bitboard.h evaluate.h misc.h movegen.h movepick.h
        nnue/nnue_misc.h nnue/features/half_ka_v2_hm.h nnue/layers/affine_transform.h
        nnue/layers/affine_transform_sparse_input.h nnue/layers/clipped_relu.h nnue/layers/simd.h
        nnue/layers/sqr_clipped_relu.h nnue/nnue_accumulator.h nnue/nnue_architecture.h
        nnue/nnue_common.h nnue/nnue_feature_transformer.h position.h
        search.h syzygy/tbprobe.h thread.h thread_win32_osx.h timeman.h
        tt.h tune.h types.h uci.h ucioption.h perft.h nnue/network.h engine.h score.h numa.h memory.h
        book/file_mapping.h book/book.h book/book_manager.h book/polyglot/polyglot.h book/ctg/ctg.h learn/learn.h mcts/montecarlo.h
        win_probability.h
        livebook/BaseLivebook.h livebook/LichessOpening.h livebook/LichessEndgame.h livebook/ChessDb.h
        livebook/analysis/Cp.h livebook/analysis/Analysis.h livebook/analysis/Wdl.h livebook/analysis/Mate.h
        livebook/LichessLivebook.h livebook/LichessMaster.h livebook/LichessPlayer.h livebook/LichessUsers.h livebook/Proxy.h
        livebook/LichessGames.h  livebook/ChessDBContributor.h
        livebook/json/json.hpp main.cpp
)

# Set default values for options
option(DEBUG "Enable debugging" OFF)
option(OPTIMIZE "Enable optimizations" ON)
option(SANITIZE "Enable sanitizers" OFF)
option(USE_PREFETCH "Enable prefetch" OFF)
option(USE_POPCNT "Enable popcnt instruction" OFF)
option(USE_SSE "Enable SSE instruction set" OFF)
option(USE_SSE2 "Enable SSE2 instruction set" OFF)
option(USE_SSSE3 "Enable SSSE3 instruction set" OFF)
option(USE_SSE41 "Enable SSE4.1 instruction set" OFF)
option(USE_AVX2 "Enable AVX2 instruction set" OFF)
option(USE_AVXVNNI "Enable AVXVNNI instruction set" OFF)
option(USE_AVX512 "Enable AVX512 instruction set" OFF)
option(USE_VNNI256 "Enable VNNI256 instruction set" OFF)
option(USE_VNNI512 "Enable VNNI512 instruction set" OFF)
option(USE_NEON "Enable NEON instruction set" OFF)
option(USE_DOTPROD "Enable NEON Dot Product instruction set" OFF)
option(BITS "Set 32/64 bit build" 64)

# Set the compiler flags
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wcast-qual -fexceptions -std=c++17")

if(DEBUG)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g")
else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DNDEBUG")
endif()

if(OPTIMIZE)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -funroll-loops")
endif()

if(SANITIZE)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=${SANITIZE}")
    set(CMAKE_LINKER_FLAGS "${CMAKE_LINKER_FLAGS} -fsanitize=${SANITIZE}")
endif()

if(USE_PREFETCH)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DUSE_PREFETCH")
else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DNO_PREFETCH")
endif()

if(USE_POPCNT)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DUSE_POPCNT")
endif()

if(USE_SSE)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -msse")
endif()

if(USE_SSE2)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -msse2")
endif()

if(USE_SSSE3)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mssse3")
endif()

if(USE_SSE41)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -msse4.1")
endif()

if(USE_AVX2)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mavx2 -mbmi")
endif()

if(USE_AVXVNNI)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mavxvnni")
endif()

if(USE_AVX512)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mavx512f -mavx512bw")
endif()

if(USE_VNNI256)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mavx512f -mavx512bw -mavx512vnni -mavx512dq -mavx512vl -mprefer-vector-width=256")
endif()

if(USE_VNNI512)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mavx512f -mavx512bw -mavx512vnni -mavx512dq -mavx512vl -mprefer-vector-width=512")
endif()

if(USE_NEON)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DUSE_NEON")
endif()

if(USE_DOTPROD)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=armv8.2-a+dotprod -DUSE_NEON_DOTPROD")
endif()

# Set 32/64 bit flags
if(BITS EQUAL 32)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -m32")
else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -m64 -DIS_64BIT")
endif()

# Set the include directories
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# Add executable
add_executable(${EXE} ${SRCS})

# Link libraries (if any)
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_link_libraries(${EXE} bcrypt advapi32 crypt32 gdi32 ws2_32)
else()
    target_link_libraries(${EXE} curl)
endif()

# Installation
install(TARGETS ${EXE} DESTINATION ${BINDIR})

# Download nnue networks
set(NNUE_NET "nn-b1a57edbea57.nnue")
add_custom_command(
        OUTPUT ${NNUE_NET}
        COMMAND ${CMAKE_COMMAND} -E echo "Downloading ${NNUE_NET}..."
        COMMAND ${CMAKE_COMMAND} -E download
        "https://tests.stockfishchess.org/api/nn/${NNUE_NET}"
        "${CMAKE_BINARY_DIR}/${NNUE_NET}"
        COMMAND ${CMAKE_COMMAND} -E echo "Download completed: ${NNUE_NET}"
        VERBATIM
)

add_custom_target(net ALL
        DEPENDS ${NNUE_NET}
)

add_custom_target(clean-net
        COMMAND ${CMAKE_COMMAND} -E remove ${NNUE_NET}
)

add_compile_definitions(USE_LIVEBOOK)

# Clean target
# add_custom_target(clean
#        COMMAND ${CMAKE_COMMAND} -E remove -f ${EXE} *.o *.gcda *.gcno *.profdata *.profraw
# )