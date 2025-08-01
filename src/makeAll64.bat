@echo off
REM x64 builds begin
SET "ORIGINAL_PATH=%PATH%"
SET PATH=C:\tools\msys64\mingw64\bin;C:\tools\msys64\usr\bin;%PATH%

Title "x86-64-vnni"
make clean
mingw32-make profile-build ARCH=x86-64-vnni COMP=mingw CXX=x86_64-w64-mingw32-g++ -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe "ShashChess39-x86-64-vnni.exe"

Title "x86-64-avx512"
make clean
mingw32-make profile-build ARCH=x86-64-avx512 COMP=mingw CXX=x86_64-w64-mingw32-g++ -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe "ShashChess39-x86-64-avx512.exe"

Title "x86-64-bmi2"
make clean
mingw32-make profile-build ARCH=x86-64-bmi2 COMP=mingw CXX=x86_64-w64-mingw32-g++ -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe "ShashChess39-x86-64-bmi2.exe"

Title "x86-64-avx2"
make clean
mingw32-make profile-build ARCH=x86-64-avx2 COMP=mingw CXX=x86_64-w64-mingw32-g++ -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe "ShashChess39-x86-64-avx2.exe"

Title "x86-64-sse41-popcnt"
make clean
mingw32-make profile-build ARCH=x86-64-sse41-popcnt COMP=mingw CXX=x86_64-w64-mingw32-g++ -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe "ShashChess39-x86-64-sse41-popcnt.exe"

Title "x86-64-ssse3"
make clean
mingw32-make profile-build ARCH=x86-64-ssse3 COMP=mingw CXX=x86_64-w64-mingw32-g++ -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe "ShashChess39-x86-64-ssse3.exe"

Title "x86-64-sse3-popcnt"
make clean
mingw32-make profile-build ARCH=x86-64-sse3-popcnt COMP=mingw CXX=x86_64-w64-mingw32-g++ -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe "ShashChess39-x86-64-sse3-popcnt.exe"

Title "x86-64"
make clean
mingw32-make profile-build ARCH=x86-64 COMP=mingw CXX=x86_64-w64-mingw32-g++ -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe "ShashChess39-x86-64.exe"

Title "general-64"
make clean
mingw32-make profile-build ARCH=general-64 COMP=mingw CXX=x86_64-w64-mingw32-g++ -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe "ShashChess39-general-64.exe"
make clean
SET "PATH=%ORIGINAL_PATH%"
REM x64 builds end
pause

