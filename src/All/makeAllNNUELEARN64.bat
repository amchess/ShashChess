@echo off
set PATH=C:\MinGW\msys\1.0\bin;%PATH%
REM x64 builds begin
ren C:\MinGW\mingw64-730-pse mingw64
set PATH=C:\MinGW\mingw64\bin;%PATH%

Title "x86-64-avx512"
make clean
mingw32-make nnue-learn ARCH=x86-64-avx512 COMP=mingw CXX=x86_64-w64-mingw32-g++ -j %Number_Of_Processors%
strip stockfish.exe
ren stockfish.exe "ShashChessNNUELEARN1.0-x86-64-avx512.exe"

Title "x86-64-bmi2"
make clean
mingw32-make nnue-learn ARCH=x86-64-bmi2 COMP=mingw CXX=x86_64-w64-mingw32-g++ -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe "ShashChessNNUELEARN1.0-x86-64-bmi2.exe"

Title "x86-64-avx2"
make clean
mingw32-make nnue-learn ARCH=x86-64-avx2 COMP=mingw CXX=x86_64-w64-mingw32-g++ -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe "ShashChessNNUELEARN1.0-x86-64-avx2.exe"

Title "x86-64-sse42"
make clean
mingw32-make build ARCH=x86-64-sse42 COMP=mingw
strip shashchess.exe
ren shashchess.exe "ShashChessNNUELEARN1.0-x86-64-sse42.exe"

Title "x86-64-sse41"
make clean
mingw32-make nnue-learn ARCH=x86-64-sse41 COMP=mingw CXX=x86_64-w64-mingw32-g++ -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe "ShashChessNNUELEARN1.0-x86-64-sse41.exe"
ren C:\MinGW\mingw64 mingw64-730-pse

Title "x86-64-ssse3"
make clean
mingw32-make nnue-learn ARCH=x86-64-ssse3 COMP=mingw CXX=x86_64-w64-mingw32-g++ -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe "ShashChessNNUELEARN1.0-x86-64-ssse3.exe"
ren C:\MinGW\mingw64 mingw64-730-pse

Title "x86-64-sse3-popcnt"
make clean
mingw32-make nnue-learn ARCH=x86-64-sse3-popcnt COMP=mingw CXX=x86_64-w64-mingw32-g++ -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe "ShashChessNNUELEARN1.0-x86-64-sse3-popcnt.exe"
ren C:\MinGW\mingw64 mingw64-730-pse

Title "x86-64-sse3"
make clean
mingw32-make nnue-learn ARCH=x86-64-sse3 COMP=mingw CXX=x86_64-w64-mingw32-g++ -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe "ShashChessNNUELEARN1.0-x86-64-sse3.exe"
ren C:\MinGW\mingw64 mingw64-730-pse

Title "x86-64"
make clean
mingw32-make nnue-learn ARCH=x86-64 COMP=mingw CXX=x86_64-w64-mingw32-g++ -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe "ShashChessNNUELEARN1.0-x86-64.exe"
ren C:\MinGW\mingw64 mingw64-730-pse

Title "ppc-64"
make clean
mingw32-make nnue-learn ARCH=ppc-64 COMP=mingw CXX=x86_64-w64-mingw32-g++ -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe "ShashChessNNUELEARN1.0-ppc-64.exe"
ren C:\MinGW\mingw64 mingw64-730-pse

Title "general-64"
make clean
mingw32-make nnue-learn ARCH=general-64 COMP=mingw CXX=x86_64-w64-mingw32-g++ -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe "ShashChessNNUELEARN1.0-general-64.exe"
ren C:\MinGW\mingw64 mingw64-730-pse

set PATH=%PATH:C:\MinGW\mingw64\bin;=%
REM x64 builds end
pause
