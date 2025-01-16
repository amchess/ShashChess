make profile-build ARCH=native COMP=gcc -j$(nproc)
strip stockfish
mv 'stockfish' 'StockfishDev-native'
make clean