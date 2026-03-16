@echo off
REM Impostazione dei percorsi per i tool di compilazione
SET PATH=C:\tools\msys64\mingw64\bin;C:\tools\msys64\usr\bin;%PATH%
Title "ShashChess x86-64-bmi2 GoldDigger Build"

mingw32-make clean
mingw32-make profile-build ARCH=x86-64-bmi2 COMP=mingw ENV_CXXFLAGS="-DGOLD_DIGGER -DNNUE_EMBEDDING_OFF" -j %Number_Of_Processors%

if not exist shashchess.exe (
    echo Errore: Compilazione fallita!
    pause
    exit /b
)

strip shashchess.exe
ren shashchess.exe "ShashChess41.1-x86-64-bmi2-GoldDigger.exe"
mingw32-make clean

echo.
echo Compilazione completata con successo!
echo Eseguibile creato: ShashChess41.1-x86-64-bmi2-GoldDigger.exe
pause