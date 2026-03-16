@echo off
REM x64 builds begin
SET PATH=C:\tools\msys64\mingw64\bin;C:\tools\msys64\usr\bin;%PATH%
mingw32-make clean
mingw32-make profile-build ARCH=native COMP=mingw ENV_CXXFLAGS="-DNNUE_EMBEDDING_OFF" -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe ShashChess41.1-native.exe
mingw32-make clean
pause