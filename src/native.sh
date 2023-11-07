@echo off
REM make -j profile-build
make profile-build ARCH=native COMP=gcc
ren shashchess.exe ShashChess34.2-native.exe
make clean
pause
