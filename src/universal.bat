@echo off
setlocal enableextensions enabledelayedexpansion

REM ==========================================================================
REM  universal.bat  -  ShashChess 42  -  binario UNIVERSAL x86-64 (MinGW/MSYS2)
REM  Da lanciare DENTRO la cartella "src" del sorgente.
REM
REM  ARCH=x86-64-universal compila 8 slice
REM  (x86-64, sse41-popcnt, avx2, bmi2, avxvnni, avx512, vnni512, avx512icl)
REM  e le linka in un unico .exe che sceglie a runtime quella giusta.
REM
REM  RETE ESTERNA: richiede Makefile e universal\nnue_embed.cpp patchati.
REM  L'exe non contiene la rete: il file nn-*.nnue va tenuto accanto all'exe
REM  oppure indicato con l'opzione UCI EvalFile.
REM ==========================================================================

REM --- toolchain: identica al batch nativo ----------------------------------
SET PATH=C:\tools\msys64\mingw64\bin;C:\tools\msys64\usr\bin;%PATH%

REM --- Intel SDE: serve SOLO per il PGO -------------------------------------
SET "SDE_DIR=C:\tools\sde"

REM --- controllo: sorgenti patchati per la rete esterna ---------------------
findstr /c:"NNUE_STUB_OBJ" Makefile >nul 2>&1
if errorlevel 1 (
    echo *** Makefile non patchato: manca il supporto a NNUE_EMBEDDING_OFF=yes ***
    goto :fallita
)
findstr /c:"defined(NNUE_EMBEDDING_OFF)" universal\nnue_embed.cpp >nul 2>&1
if errorlevel 1 (
    echo *** universal\nnue_embed.cpp non patchato ***
    goto :fallita
)

set "TARGET=build"
set "RUN_PREFIX="
if exist "%SDE_DIR%\sde.exe" (
    set "SDE_FWD=%SDE_DIR:\=/%"
    set "RUN_PREFIX=!SDE_FWD!/sde.exe -future --"
    set "TARGET=profile-build"
    echo [universal] Intel SDE trovato: build CON PGO ^(profile-build^)
) else (
    echo [universal] Intel SDE assente in "%SDE_DIR%": build SENZA PGO ^(build^)
)

mingw32-make clean

REM NNUE_EMBEDDING_OFF=yes: le slice e il loro bench PGO usano ancora la rete
REM incorporata in temp_builds, solo il link finale usa il segnaposto da 1 byte.
REM Non passare CXXFLAGS da riga di comando: sostituisce -O3, -m... e -flto.
mingw32-make %TARGET% ARCH=x86-64-universal COMP=mingw NNUE_EMBEDDING_OFF=yes -j %NUMBER_OF_PROCESSORS%
if errorlevel 1 goto :fallita
if not exist shashchess.exe goto :fallita

strip shashchess.exe
REM move /y invece di ren: ren fallisce se l'exe esiste gia' e il clean finale
REM cancellerebbe quello nuovo, lasciando il binario della build precedente.
move /y shashchess.exe ShashChess42-universal.exe >nul
if errorlevel 1 goto :fallita

REM --- nome della rete di default, letto da evaluate.h ----------------------
set "NETFILE="
for /f "tokens=3" %%N in ('findstr /c:"define EvalFileDefaultName " evaluate.h') do set "NETFILE=%%~N"
if not defined NETFILE (
    echo *** EvalFileDefaultName non trovato in evaluate.h ***
    goto :fallita
)
if not exist "%NETFILE%" (
    echo *** Rete %NETFILE% assente in src: scaricala con "mingw32-make net" ***
    goto :fallita
)

REM --- verifiche in una cartella temporanea, senza toccare src ---------------
set "CHECKDIR=%TEMP%\shashchess_netcheck"
if exist "%CHECKDIR%" rmdir /s /q "%CHECKDIR%"
mkdir "%CHECKDIR%"
copy /y ShashChess42-universal.exe "%CHECKDIR%\ShashChess42-universal.exe" >nul

REM verifica 1: senza rete accanto all'exe il motore deve fermarsi
pushd "%CHECKDIR%"
ShashChess42-universal.exe bench 16 1 1 >nul 2>&1
set "RC=%errorlevel%"
popd
if "%RC%"=="0" (
    rmdir /s /q "%CHECKDIR%"
    echo *** ATTENZIONE: il bench gira anche senza %NETFILE%: la rete e' ancora incorporata ***
    goto :fallita
)
echo [universal] OK: senza %NETFILE% il motore si ferma, la rete non e' incorporata

REM verifica 2: con la rete accanto all'exe il bench deve funzionare
copy /y "%NETFILE%" "%CHECKDIR%\%NETFILE%" >nul
pushd "%CHECKDIR%"
ShashChess42-universal.exe bench 16 1 1 >nul 2>&1
set "RC=%errorlevel%"
popd
rmdir /s /q "%CHECKDIR%"
if not "%RC%"=="0" (
    echo *** ERRORE: il motore non carica %NETFILE% messo accanto all'exe ***
    goto :fallita
)
echo [universal] OK: rete caricata da file esterno

REM --- verifica: stampa la slice selezionata sulla CPU corrente -------------
ShashChess42-universal.exe compiler

mingw32-make clean
echo.
echo [universal] OK: ShashChess42-universal.exe
echo [universal] Da distribuire insieme: ShashChess42-universal.exe + %NETFILE%
pause
exit /b 0

:fallita
echo.
echo *** BUILD UNIVERSAL FALLITA ***
pause
exit /b 1
