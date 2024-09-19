@echo off
REM x64 builds begin
SET PATH=C:\tools\msys64\mingw64\bin;C:\tools\msys64\usr\bin;%PATH%
make clean
mingw32-make -f MakeFileVSCode build COMP=mingw -j %Number_Of_Processors%
pause
