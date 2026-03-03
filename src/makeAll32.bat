@echo off
REM x32 builds begin (Standard & GoldDigger Edition - No Net Embedded)
SET "ORIGINAL_PATH=%PATH%"
SET PATH=C:\tools\msys64\mingw32\bin;C:\tools\msys64\usr\bin;%PATH%

Title "x86-32"
mingw32-make clean
mingw32-make profile-build ARCH=x86-32 COMP=mingw ENV_CXXFLAGS="-DNNUE_EMBEDDING_OFF" -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe "ShashChess41-x86-32.exe"
mingw32-make clean
mingw32-make profile-build ARCH=x86-32 COMP=mingw ENV_CXXFLAGS="-DGOLD_DIGGER -DNNUE_EMBEDDING_OFF" -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe "ShashChess41-x86-32-GoldDigger.exe"

Title "x86-32-old"
mingw32-make clean
mingw32-make profile-build ARCH=x86-32-old COMP=mingw ENV_CXXFLAGS="-DNNUE_EMBEDDING_OFF" -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe "ShashChess41-x86-32-old.exe"
mingw32-make clean
mingw32-make profile-build ARCH=x86-32-old COMP=mingw ENV_CXXFLAGS="-DGOLD_DIGGER -DNNUE_EMBEDDING_OFF" -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe "ShashChess41-x86-32-old-GoldDigger.exe"

Title "general-32"
mingw32-make clean
mingw32-make profile-build ARCH=general-32 COMP=mingw ENV_CXXFLAGS="-DNNUE_EMBEDDING_OFF" -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe "ShashChess41-general-32.exe"
mingw32-make clean
mingw32-make profile-build ARCH=general-32 COMP=mingw ENV_CXXFLAGS="-DGOLD_DIGGER -DNNUE_EMBEDDING_OFF" -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe "ShashChess41-general-32-GoldDigger.exe"

mingw32-make clean
SET "PATH=%ORIGINAL_PATH%"
REM x32 builds end
pause