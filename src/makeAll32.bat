@echo off
REM x32 builds end
SET "ORIGINAL_PATH=%PATH%"
SET PATH=C:\tools\msys64\mingw32\bin;C:\tools\msys64\usr\bin;%PATH%
Title "x86-32"
make -f MakeFile profile-build ARCH=x86-32 COMP=mingw
strip alexander.exe
ren alexander.exe "Alexander4.1-x86-32.exe"

Title "x86-32-old"
make clean
make -f MakeFile profile-build ARCH=x86-32-old COMP=mingw
strip alexander.exe
ren alexander.exe "Alexander4.1-x86-32-old.exe"

Title "general-32"
make clean
make -f MakeFile profile-build ARCH=general-32 COMP=mingw
strip alexander.exe
ren alexander.exe "Alexander4.1-general-32.exe"

make clean
SET "PATH=%ORIGINAL_PATH%"
REM x32 builds end

pause
