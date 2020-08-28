@echo off
set PATH=C:\MinGW\msys\1.0\bin;%PATH%
REM x64 builds begin
ren C:\MinGW\mingw64-730-pse mingw64
set PATH=C:\MinGW\mingw64\bin;%PATH%

Title "x86-64-bmi2"
make clean
mingw32-make profile-build ARCH=x86-64-bmi2 COMP=mingw CXX=x86_64-w64-mingw32-g++ -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe "ShashChess13.2-x86-64-bmi2.exe"
make clean

ren C:\MinGW\mingw64 mingw64-730-pse

set PATH=%PATH:C:\MinGW\mingw64\bin;C:\MinGW\msys\1.0\bin;=%
REM x64 builds end
pause
