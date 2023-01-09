@echo off
SET PATH=C:\msys64\mingw64\bin;C:\msys64\usr\bin;%PATH%

Title "x86-64-bmi2"
make clean
mingw32-make profile-build ARCH=x86-64-bmi2 COMP=mingw CXX=x86_64-w64-mingw32-g++ -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe "ShashChess27.1-x86-64-bmi2.exe"
make clean
ren C:\MinGW\mingw64 mingw64-730-pse
pause
