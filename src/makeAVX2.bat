@echo off
SET PATH=C:\tools\msys64\mingw64\bin;C:\tools\msys64\usr\bin;%PATH%
Title "x86-64-avx2"
mingw32-make clean
mingw32-make profile-build ARCH=x86-64-avx2 COMP=mingw CXX=x86_64-w64-mingw32-g++ ENV_CXXFLAGS="-DNNUE_EMBEDDING_OFF" -j %Number_Of_Processors%
strip shashchess.exe
ren shashchess.exe "ShashChess41.1-x86-64-avx2.exe"
mingw32-make clean
pause