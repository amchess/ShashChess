#!/usr/bin/env bash
# ==========================================================================
#  universal.sh  -  ShashChess 42  -  binario UNIVERSAL x86-64 (Linux / GCC)
#  Da lanciare DENTRO la cartella "src" del sorgente.
#
#  ARCH=x86-64-universal compila 8 slice
#  e le linka in un unico eseguibile che sceglie a runtime quella giusta.
#
#  RETE ESTERNA: richiede Makefile e universal/nnue_embed.cpp patchati.
#  L'eseguibile non contiene la rete: il file nn-*.nnue va tenuto accanto
#  all'eseguibile oppure indicato con l'opzione UCI EvalFile.
# ==========================================================================

set -e

fallita() {
    echo ""
    echo "*** BUILD UNIVERSAL FALLITA: $1 ***"
    exit 1
}

# --- controllo: sorgenti patchati per la rete esterna ---------------------
grep -q "NNUE_STUB_OBJ" Makefile \
    || fallita "Makefile non patchato, manca il supporto a NNUE_EMBEDDING_OFF=yes"
grep -q "defined(NNUE_EMBEDDING_OFF)" universal/nnue_embed.cpp \
    || fallita "universal/nnue_embed.cpp non patchato"

SDE_DIR="${SDE_DIR:-/opt/sde}"
NPROC=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

TARGET="build"
RUN_PREFIX=""

if [ -x "$SDE_DIR/sde" ]; then
    SDE_BIN="$SDE_DIR/sde"
elif command -v sde >/dev/null 2>&1; then
    SDE_BIN=$(command -v sde)
fi

if [ -n "$SDE_BIN" ]; then
    RUN_PREFIX="$SDE_BIN -future --"
    TARGET="profile-build"
    echo "[universal] Intel SDE trovato ($SDE_BIN): build CON PGO (profile-build)"
else
    echo "[universal] Intel SDE non trovato: build SENZA PGO (build)"
fi

make clean

echo "[universal] Avvio compilazione ARCH=x86-64-universal con -j$NPROC..."

# NNUE_EMBEDDING_OFF=yes: le slice e il loro bench PGO usano ancora la rete
# incorporata in temp_builds, solo il link finale usa il segnaposto da 1 byte.
# Non passare CXXFLAGS da riga di comando: sostituisce -O3, -m... e -flto.
if [ -n "$RUN_PREFIX" ]; then
    make "$TARGET" ARCH=x86-64-universal COMP=gcc NNUE_EMBEDDING_OFF=yes RUN_PREFIX="$RUN_PREFIX" -j"$NPROC"
else
    make "$TARGET" ARCH=x86-64-universal COMP=gcc NNUE_EMBEDDING_OFF=yes -j"$NPROC"
fi

[ -f shashchess ] || fallita "eseguibile non generato"

strip shashchess
mv shashchess ShashChess42-universal

# --- nome della rete di default, letto da evaluate.h ----------------------
NETFILE=$(grep -E '^[[:space:]]*#[[:space:]]*define[[:space:]]+EvalFileDefaultName[[:space:]]' evaluate.h \
    | grep -oE 'nn-[0-9a-f]+\.nnue' | head -n 1)
[ -n "$NETFILE" ] || fallita "EvalFileDefaultName non trovato in evaluate.h"
[ -f "$NETFILE" ] || fallita "rete $NETFILE assente in src, scaricala con: make net"

# --- verifiche in una cartella temporanea, senza toccare src ---------------
CHECKDIR=$(mktemp -d)
cp ShashChess42-universal "$CHECKDIR/"

# verifica 1: senza rete accanto all'eseguibile il motore deve fermarsi
if (cd "$CHECKDIR" && ./ShashChess42-universal bench 16 1 1 >/dev/null 2>&1); then
    rm -rf "$CHECKDIR"
    fallita "il bench gira anche senza $NETFILE, la rete e' ancora incorporata"
fi
echo "[universal] OK: senza $NETFILE il motore si ferma, la rete non e' incorporata"

# verifica 2: con la rete accanto all'eseguibile il bench deve funzionare
cp "$NETFILE" "$CHECKDIR/"
if ! (cd "$CHECKDIR" && ./ShashChess42-universal bench 16 1 1 >/dev/null 2>&1); then
    rm -rf "$CHECKDIR"
    fallita "il motore non carica $NETFILE messo accanto all'eseguibile"
fi
rm -rf "$CHECKDIR"
echo "[universal] OK: rete caricata da file esterno"

# --- Verifica: stampa la slice selezionata sulla CPU corrente -------------
echo ""
echo "[universal] Test di esecuzione (compiler info):"
./ShashChess42-universal compiler

make clean

echo ""
echo "[universal] OK: ShashChess42-universal"
echo "[universal] Da distribuire insieme: ShashChess42-universal + $NETFILE"
exit 0
