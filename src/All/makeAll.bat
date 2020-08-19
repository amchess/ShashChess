@echo off
set PATH=C:\MinGW\msys\1.0\bin;%PATH%
REM x64 builds begin
ren C:\MinGW\mingw64-730-pse mingw64
set PATH=C:\MinGW\mingw64\bin;%PATH%

Title "x86-64-vnni"
make clean
mingw32-make profile-build ARCH=x86-64-vnni COMP=mingw CXX=x86_64-w64-mingw32-g++ -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe "ShashChess13-x86-64-vnni.exe"

Title "x86-64-avx512"
make clean
mingw32-make profile-build ARCH=x86-64-avx512 COMP=mingw CXX=x86_64-w64-mingw32-g++ -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe "ShashChess13-x86-64-avx512.exe"

Title "x86-64-bmi2"
make clean
mingw32-make profile-build ARCH=x86-64-bmi2 COMP=mingw CXX=x86_64-w64-mingw32-g++ -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe "ShashChess13-x86-64-bmi2.exe"

Title "x86-64-avx2"
make clean
mingw32-make profile-build ARCH=x86-64-avx2 COMP=mingw CXX=x86_64-w64-mingw32-g++ -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe "ShashChess13-x86-64-avx2.exe"

Title "x86-64-modern"
make clean
mingw32-make profile-build ARCH=x86-64-modern COMP=mingw CXX=x86_64-w64-mingw32-g++ -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe "ShashChess13-x86-64-modern.exe"

Title "x86-64-sse41-popcnt"
make clean
mingw32-make profile-build ARCH=x86-64-sse41-popcnt COMP=mingw CXX=x86_64-w64-mingw32-g++ -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe "ShashChess13-x86-64-sse41-popcnt.exe"

Title "x86-64-ssse3"
make clean
mingw32-make profile-build ARCH=x86-64-ssse3 COMP=mingw CXX=x86_64-w64-mingw32-g++ -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe "ShashChess13-x86-64-ssse3.exe"

Title "x86-64-sse3-popcnt"
make clean
mingw32-make profile-build ARCH=x86-64-sse3-popcnt COMP=mingw CXX=x86_64-w64-mingw32-g++ -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe "ShashChess13-x86-64-sse3-popcnt.exe"

Title "x86-64"
make clean
mingw32-make profile-build ARCH=x86-64 COMP=mingw CXX=x86_64-w64-mingw32-g++ -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe "ShashChess13-x86-64.exe"

Title "general-64"
make clean
mingw32-make profile-build ARCH=general-64 COMP=mingw CXX=x86_64-w64-mingw32-g++ -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe "ShashChess13-general-64.exe"
ren C:\MinGW\mingw64 mingw64-730-pse

set PATH=%PATH:C:\MinGW\mingw64\bin;=%
REM x64 builds end

REM x32 builds begin
ren C:\MinGW\mingw32-730-pd mingw32
set PATH=C:\MinGW\mingw32\bin;%PATH%

Title "x86-32"
make clean
mingw32-make -f MakeFile profile-build ARCH=x86-32 COMP=mingw
strip shashchess.exe
ren shashchess.exe "ShashChess13-x86-32.exe"

Title "x86-32-old"
make clean
mingw32-make -f MakeFile profile-build ARCH=x86-32-old COMP=mingw
strip shashchess.exe
ren shashchess.exe "ShashChess13-x86-32-old.exe"

Title "general-32"
make clean
mingw32-make -f MakeFile profile-build ARCH=general-32 COMP=mingw
strip shashchess.exe
ren shashchess.exe "ShashChess13-general-32.exe"

make clean
ren C:\MinGW\mingw32 mingw32-730-pd
REM x32 builds end

set PATH=%PATH:C:\MinGW\mingw32\bin;C:\MinGW\msys\1.0\bin;=%
pause
