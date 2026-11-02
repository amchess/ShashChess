#!/usr/bin/env bash
set -e

LOGFILE="$HOME/match_tournament.log"
exec > >(tee -a "$LOGFILE") 2>&1
echo "===== Avvio torneo: $(date) ====="

CLI="$HOME/cutechess-extract/squashfs-root/usr/bin/cutechess-cli"

time "$CLI" \
  -engine name=ShashChess \
          cmd=/home/claudio-intel/git/ShashChessDev/src/ShashChessDev-native \
          proto=uci \
          option.Threads=4 \
          option.Hash=2048 \
          option.SyzygyPath=/home/claudio-intel/syzygy \
  -engine name=Stockfish \
          cmd=/home/claudio-intel/git/Stockfish/src/StockfishDev-native \
          proto=uci \
          option.Threads=4 \
          option.Hash=2048 \
          option.SyzygyPath=/home/claudio-intel/syzygy \
  -variant standard \
  -each tc=0/25:00+10 \
  -openings file=/home/claudio-intel/git/ShashChessDev/tests/TestSuiteCenterType/Test2025.pgn \
            format=pgn order=sequential plies=60 \
  -draw movenumber=40 movecount=8 score=5 \
  -resign movecount=5 score=600 \
  -maxmoves 300 \
  -tb /home/claudio-intel/syzygy \
  -tournament round-robin \
  -rounds 150 \
  -games 2 \
  -repeat \
  -wait 0 \
  -concurrency 90 \
  -pgnout /home/claudio-intel/MatchConcurrent94.pgn fi \
  -recover \
  -outcomeinterval 1 \
  -ratinginterval 10

echo "===== Fine torneo: $(date) ====="
