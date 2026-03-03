#!/bin/bash

# x86-64-vnni
make clean
make profile-build ARCH=x86-64-vnni COMP=gcc ENV_CXXFLAGS="-DNNUE_EMBEDDING_OFF" -j$(nproc)
strip shashchess
mv 'shashchess' 'ShashChess41-x86-64-vnni'
make clean
make profile-build ARCH=x86-64-vnni COMP=gcc ENV_CXXFLAGS="-DGOLD_DIGGER -DNNUE_EMBEDDING_OFF" -j$(nproc)
strip shashchess
mv 'shashchess' 'ShashChess41-x86-64-vnni-GoldDigger'
make clean

# x86-64-avx512
make profile-build ARCH=x86-64-avx512 COMP=gcc ENV_CXXFLAGS="-DNNUE_EMBEDDING_OFF" -j$(nproc)
strip shashchess
mv 'shashchess' 'ShashChess41-x86-64-avx512'
make clean
make profile-build ARCH=x86-64-avx512 COMP=gcc ENV_CXXFLAGS="-DGOLD_DIGGER -DNNUE_EMBEDDING_OFF" -j$(nproc)
strip shashchess
mv 'shashchess' 'ShashChess41-x86-64-avx512-GoldDigger'
make clean

# x86-64-bmi2
make profile-build ARCH=x86-64-bmi2 COMP=gcc ENV_CXXFLAGS="-DNNUE_EMBEDDING_OFF" -j$(nproc)
strip shashchess
mv 'shashchess' 'ShashChess41-x86-64-bmi2'
make clean
make profile-build ARCH=x86-64-bmi2 COMP=gcc ENV_CXXFLAGS="-DGOLD_DIGGER -DNNUE_EMBEDDING_OFF" -j$(nproc)
strip shashchess
mv 'shashchess' 'ShashChess41-x86-64-bmi2-GoldDigger'
make clean

# x86-64-avx2
make profile-build ARCH=x86-64-avx2 COMP=gcc ENV_CXXFLAGS="-DNNUE_EMBEDDING_OFF" -j$(nproc)
strip shashchess
mv 'shashchess' 'ShashChess41-x86-64-avx2'
make clean
make profile-build ARCH=x86-64-avx2 COMP=gcc ENV_CXXFLAGS="-DGOLD_DIGGER -DNNUE_EMBEDDING_OFF" -j$(nproc)
strip shashchess
mv 'shashchess' 'ShashChess41-x86-64-avx2-GoldDigger'
make clean

# x86-64-modern
make profile-build ARCH=x86-64-modern COMP=gcc ENV_CXXFLAGS="-DNNUE_EMBEDDING_OFF" -j$(nproc)
strip shashchess
mv 'shashchess' 'ShashChess41-x86-64-modern'
make clean
make profile-build ARCH=x86-64-modern COMP=gcc ENV_CXXFLAGS="-DGOLD_DIGGER -DNNUE_EMBEDDING_OFF" -j$(nproc)
strip shashchess
mv 'shashchess' 'ShashChess41-x86-64-modern-GoldDigger'
make clean

# x86-64-sse41-popcnt
make profile-build ARCH=x86-64-sse41-popcnt COMP=gcc ENV_CXXFLAGS="-DNNUE_EMBEDDING_OFF" -j$(nproc)
strip shashchess
mv 'shashchess' 'ShashChess41-x86-64-sse41-popcnt'
make clean
make profile-build ARCH=x86-64-sse41-popcnt COMP=gcc ENV_CXXFLAGS="-DGOLD_DIGGER -DNNUE_EMBEDDING_OFF" -j$(nproc)
strip shashchess
mv 'shashchess' 'ShashChess41-x86-64-sse41-popcnt-GoldDigger'
make clean

# x86-64-ssse3
make profile-build ARCH=x86-64-ssse3 COMP=gcc ENV_CXXFLAGS="-DNNUE_EMBEDDING_OFF" -j$(nproc)
strip shashchess
mv 'shashchess' 'ShashChess41-x86-64-ssse3'
make clean
make profile-build ARCH=x86-64-ssse3 COMP=gcc ENV_CXXFLAGS="-DGOLD_DIGGER -DNNUE_EMBEDDING_OFF" -j$(nproc)
strip shashchess
mv 'shashchess' 'ShashChess41-x86-64-ssse3-GoldDigger'
make clean

# x86-64-sse3-popcnt
make profile-build ARCH=x86-64-sse3-popcnt COMP=gcc ENV_CXXFLAGS="-DNNUE_EMBEDDING_OFF" -j$(nproc)
strip shashchess
mv 'shashchess' 'ShashChess41-x86-64-sse3-popcnt'
make clean
make profile-build ARCH=x86-64-sse3-popcnt COMP=gcc ENV_CXXFLAGS="-DGOLD_DIGGER -DNNUE_EMBEDDING_OFF" -j$(nproc)
strip shashchess
mv 'shashchess' 'ShashChess41-x86-64-sse3-popcnt-GoldDigger'
make clean

# x86-64
make profile-build ARCH=x86-64 COMP=gcc ENV_CXXFLAGS="-DNNUE_EMBEDDING_OFF" -j$(nproc)
strip shashchess
mv 'shashchess' 'ShashChess41-x86-64'
make clean
make profile-build ARCH=x86-64 COMP=gcc ENV_CXXFLAGS="-DGOLD_DIGGER -DNNUE_EMBEDDING_OFF" -j$(nproc)
strip shashchess
mv 'shashchess' 'ShashChess41-x86-64-GoldDigger'
make clean

# general-64
make profile-build ARCH=general-64 COMP=gcc ENV_CXXFLAGS="-DNNUE_EMBEDDING_OFF" -j$(nproc)
strip shashchess
mv 'shashchess' 'ShashChess41-general-64'
make clean
make profile-build ARCH=general-64 COMP=gcc ENV_CXXFLAGS="-DGOLD_DIGGER -DNNUE_EMBEDDING_OFF" -j$(nproc)
strip shashchess
mv 'shashchess' 'ShashChess41-general-64-GoldDigger'
make clean

# x86-32
make profile-build ARCH=x86-32 COMPCC=gcc ENV_CXXFLAGS="-DNNUE_EMBEDDING_OFF" -j$(nproc)
strip shashchess
mv 'shashchess' 'ShashChess41-x86-32'
make clean
make profile-build ARCH=x86-32 COMPCC=gcc ENV_CXXFLAGS="-DGOLD_DIGGER -DNNUE_EMBEDDING_OFF" -j$(nproc)
strip shashchess
mv 'shashchess' 'ShashChess41-x86-32-GoldDigger'
make clean

# x86-32-old
make profile-build ARCH=x86-32-old COMPCC=gcc ENV_CXXFLAGS="-DNNUE_EMBEDDING_OFF" -j$(nproc)
strip shashchess
mv 'shashchess' 'ShashChess41-x86-32-old'
make clean
make profile-build ARCH=x86-32-old COMPCC=gcc ENV_CXXFLAGS="-DGOLD_DIGGER -DNNUE_EMBEDDING_OFF" -j$(nproc)
strip shashchess
mv 'shashchess' 'ShashChess41-x86-32-old-GoldDigger'
make clean

# general-32
make profile-build ARCH=general-32 COMPCC=gcc ENV_CXXFLAGS="-DNNUE_EMBEDDING_OFF" -j$(nproc)
strip shashchess
mv 'shashchess' 'ShashChess41-general-32'
make clean
make profile-build ARCH=general-32 COMPCC=gcc ENV_CXXFLAGS="-DGOLD_DIGGER -DNNUE_EMBEDDING_OFF" -j$(nproc)
strip shashchess
mv 'shashchess' 'ShashChess41-general-32-GoldDigger'
make clean