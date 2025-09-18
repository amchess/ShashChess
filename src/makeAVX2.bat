@echo off
SET PATH=C:\tools\msys64\mingw64\bin;C:\tools\msys64\usr\bin;%PATH%

Title "x86-64-avx2"
make clean
mingw32-make profile-build ARCH=x86-64-avx2 COMP=mingw CXX=x86_64-w64-mingw32-g++ -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe "ShashChess39.1-x86-64-avx2.exe"
make clean
pause
