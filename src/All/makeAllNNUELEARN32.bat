@echo off
set PATH=C:\MinGW\msys\1.0\bin;%PATH%
REM x32 builds begin
ren C:\MinGW\mingw32-730-pd mingw32
set PATH=C:\MinGW\mingw32\bin;%PATH%

Title "x86-32"
make clean
mingw32-make -f MakeFile nnue-learn ARCH=x86-32 COMP=mingw
strip shashchess.exe
ren shashchess.exe "ShashChessNNUELEARN1.0-x86-32.exe"

Title "x86-32-old"
make clean
mingw32-make -f MakeFile nnue-learn ARCH=x86-32-old COMP=mingw
strip shashchess.exe
ren shashchess.exe "ShashChessNNUELEARN1.0-x86-32-old.exe"

Title "general-32"
make clean
mingw32-make -f MakeFile nnue-learn ARCH=general-32 COMP=mingw
strip shashchess.exe
ren shashchess.exe "ShashChessNNUELEARN1.0-general-32.exe"

make clean
ren C:\MinGW\mingw32 mingw32-730-pd
REM x32 builds end

set PATH=%PATH:C:\MinGW\mingw32\bin;C:\MinGW\msys\1.0\bin;=%
pause
