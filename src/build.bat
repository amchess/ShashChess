@echo off
REM x64 builds begin
SET PATH=C:\tools\msys64\mingw64\bin;C:\tools\msys64\usr\bin;%PATH%
REM make -j profile-build
mingw32-make build COMP=mingw -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe ShashChessBuilt.exe
make clean
pause
