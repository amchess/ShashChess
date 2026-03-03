#!/bin/bash

# Rendi lo script resiliente agli errori
set -e

# Numero di core disponibili per velocizzare la compilazione
CORES=$(nproc)

echo "Inizio compilazione ShashChess GoldDigger Edition su Linux (Native)..."

# Pulizia preventiva
make clean

# Compilazione con flag GOLD_DIGGER forzato tramite ENV_CXXFLAGS e senza rete incorporata
make profile-build ARCH=native COMP=gcc ENV_CXXFLAGS="-DGOLD_DIGGER -DNNUE_EMBEDDING_OFF" -j $CORES

# Pulizia dei simboli per ridurre la dimensione del file
strip shashchess

# Rinominiamo l'eseguibile per distinguerlo (uniformato a ShashChess41)
mv shashchess ShashChess41-native-GoldDigger

# Pulizia dei file oggetto intermedi
make clean

echo "Compilazione completata: ShashChess41-native-GoldDigger"