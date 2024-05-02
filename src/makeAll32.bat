@echo off
REM x32 builds end
SET PATH=C:\tools\msys64\mingw32\bin;C:\tools\msys64\usr\bin;%PATH%
Title "x86-32"
make clean
make -f MakeFile profile-build ARCH=x86-32 COMP=mingw
strip shashchess.exe
ren shashchess.exe "ShashChess35.3-x86-32.exe"

Title "x86-32-old"
make clean
make -f MakeFile profile-build ARCH=x86-32-old COMP=mingw
strip shashchess.exe
ren shashchess.exe "ShashChess35.3-x86-32-old.exe"

Title "general-32"
make clean
make -f MakeFile profile-build ARCH=general-32 COMP=mingw
strip shashchess.exe
ren shashchess.exe "ShashChess35.3-general-32.exe"

make clean
REM x32 builds end

pause
