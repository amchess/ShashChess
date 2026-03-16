@echo off
REM x64 builds begin (Standard & GoldDigger Edition - No Net Embedded)
SET "ORIGINAL_PATH=%PATH%"
SET PATH=C:\tools\msys64\mingw64\bin;C:\tools\msys64\usr\bin;%PATH%

Title "x86-64-vnni"
mingw32-make clean
mingw32-make profile-build ARCH=x86-64-vnni COMP=mingw CXX=x86_64-w64-mingw32-g++ ENV_CXXFLAGS="-DNNUE_EMBEDDING_OFF" -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe "ShashChess41.1-x86-64-vnni.exe"
mingw32-make clean
mingw32-make profile-build ARCH=x86-64-vnni COMP=mingw CXX=x86_64-w64-mingw32-g++ ENV_CXXFLAGS="-DGOLD_DIGGER -DNNUE_EMBEDDING_OFF" -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe "ShashChess41.1-x86-64-vnni-GoldDigger.exe"

Title "x86-64-avx512"
mingw32-make clean
mingw32-make profile-build ARCH=x86-64-avx512 COMP=mingw CXX=x86_64-w64-mingw32-g++ ENV_CXXFLAGS="-DNNUE_EMBEDDING_OFF" -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe "ShashChess41.1-x86-64-avx512.exe"
mingw32-make clean
mingw32-make profile-build ARCH=x86-64-avx512 COMP=mingw CXX=x86_64-w64-mingw32-g++ ENV_CXXFLAGS="-DGOLD_DIGGER -DNNUE_EMBEDDING_OFF" -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe "ShashChess41.1-x86-64-avx512-GoldDigger.exe"

Title "x86-64-bmi2"
mingw32-make clean
mingw32-make profile-build ARCH=x86-64-bmi2 COMP=mingw CXX=x86_64-w64-mingw32-g++ ENV_CXXFLAGS="-DNNUE_EMBEDDING_OFF" -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe "ShashChess41.1-x86-64-bmi2.exe"
mingw32-make clean
mingw32-make profile-build ARCH=x86-64-bmi2 COMP=mingw CXX=x86_64-w64-mingw32-g++ ENV_CXXFLAGS="-DGOLD_DIGGER -DNNUE_EMBEDDING_OFF" -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe "ShashChess41.1-x86-64-bmi2-GoldDigger.exe"

Title "x86-64-avx2"
mingw32-make clean
mingw32-make profile-build ARCH=x86-64-avx2 COMP=mingw CXX=x86_64-w64-mingw32-g++ ENV_CXXFLAGS="-DNNUE_EMBEDDING_OFF" -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe "ShashChess41.1-x86-64-avx2.exe"
mingw32-make clean
mingw32-make profile-build ARCH=x86-64-avx2 COMP=mingw CXX=x86_64-w64-mingw32-g++ ENV_CXXFLAGS="-DGOLD_DIGGER -DNNUE_EMBEDDING_OFF" -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe "ShashChess41.1-x86-64-avx2-GoldDigger.exe"

Title "x86-64-sse41-popcnt"
mingw32-make clean
mingw32-make profile-build ARCH=x86-64-sse41-popcnt COMP=mingw CXX=x86_64-w64-mingw32-g++ ENV_CXXFLAGS="-DNNUE_EMBEDDING_OFF" -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe "ShashChess41.1-x86-64-sse41-popcnt.exe"
mingw32-make clean
mingw32-make profile-build ARCH=x86-64-sse41-popcnt COMP=mingw CXX=x86_64-w64-mingw32-g++ ENV_CXXFLAGS="-DGOLD_DIGGER -DNNUE_EMBEDDING_OFF" -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe "ShashChess41.1-x86-64-sse41-popcnt-GoldDigger.exe"

Title "x86-64-ssse3"
mingw32-make clean
mingw32-make profile-build ARCH=x86-64-ssse3 COMP=mingw CXX=x86_64-w64-mingw32-g++ ENV_CXXFLAGS="-DNNUE_EMBEDDING_OFF" -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe "ShashChess41.1-x86-64-ssse3.exe"
mingw32-make clean
mingw32-make profile-build ARCH=x86-64-ssse3 COMP=mingw CXX=x86_64-w64-mingw32-g++ ENV_CXXFLAGS="-DGOLD_DIGGER -DNNUE_EMBEDDING_OFF" -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe "ShashChess41.1-x86-64-ssse3-GoldDigger.exe"

Title "x86-64-sse3-popcnt"
mingw32-make clean
mingw32-make profile-build ARCH=x86-64-sse3-popcnt COMP=mingw CXX=x86_64-w64-mingw32-g++ ENV_CXXFLAGS="-DNNUE_EMBEDDING_OFF" -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe "ShashChess41.1-x86-64-sse3-popcnt.exe"
mingw32-make clean
mingw32-make profile-build ARCH=x86-64-sse3-popcnt COMP=mingw CXX=x86_64-w64-mingw32-g++ ENV_CXXFLAGS="-DGOLD_DIGGER -DNNUE_EMBEDDING_OFF" -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe "ShashChess41.1-x86-64-sse3-popcnt-GoldDigger.exe"

Title "x86-64"
mingw32-make clean
mingw32-make profile-build ARCH=x86-64 COMP=mingw CXX=x86_64-w64-mingw32-g++ ENV_CXXFLAGS="-DNNUE_EMBEDDING_OFF" -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe "ShashChess41.1-x86-64.exe"
mingw32-make clean
mingw32-make profile-build ARCH=x86-64 COMP=mingw CXX=x86_64-w64-mingw32-g++ ENV_CXXFLAGS="-DGOLD_DIGGER -DNNUE_EMBEDDING_OFF" -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe "ShashChess41.1-x86-64-GoldDigger.exe"

Title "general-64"
mingw32-make clean
mingw32-make profile-build ARCH=general-64 COMP=mingw CXX=x86_64-w64-mingw32-g++ ENV_CXXFLAGS="-DNNUE_EMBEDDING_OFF" -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe "ShashChess41.1-general-64.exe"
mingw32-make clean
mingw32-make profile-build ARCH=general-64 COMP=mingw CXX=x86_64-w64-mingw32-g++ ENV_CXXFLAGS="-DGOLD_DIGGER -DNNUE_EMBEDDING_OFF" -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe "ShashChess41.1-general-64-GoldDigger.exe"

mingw32-make clean
SET "PATH=%ORIGINAL_PATH%"
REM x64 builds end
pause