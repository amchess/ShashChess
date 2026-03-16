@echo off
REM x64 builds begin (GoldDigger Edition)
SET PATH=C:\tools\msys64\mingw64\bin;C:\tools\msys64\usr\bin;%PATH%

echo Inizio compilazione ShashChess GoldDigger Edition (Native)...
mingw32-make clean
mingw32-make profile-build ARCH=native COMP=mingw ENV_CXXFLAGS="-DGOLD_DIGGER -DNNUE_EMBEDDING_OFF" -j %Number_Of_Processors%

if not exist shashchess.exe (
    echo Errore: Compilazione fallita!
    pause
    exit /b
)

strip shashchess.exe
ren shashchess.exe ShashChess41.1-native-GoldDigger.exe
mingw32-make clean

echo.
echo Compilazione completata con successo!
pause