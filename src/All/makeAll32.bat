REM x32 builds end
@echo off
SET PATH=C:\msys64\mingw32\bin;C:\msys64\usr\bin;%PATH%
Title "x86-32"
make clean
mingw32-make -f MakeFile profile-build ARCH=x86-32 COMP=mingw
strip shashchess.exe
ren shashchess.exe "ShashChess27.1-x86-32.exe"

Title "x86-32-old"
make clean
mingw32-make -f MakeFile profile-build ARCH=x86-32-old COMP=mingw
strip shashchess.exe
ren shashchess.exe "ShashChess27.1-x86-32-old.exe"

Title "general-32"
make clean
mingw32-make -f MakeFile profile-build ARCH=general-32 COMP=mingw
strip shashchess.exe
ren shashchess.exe "ShashChess27.1-general-32.exe"

make clean
REM x32 builds end

pause
