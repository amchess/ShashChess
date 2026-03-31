#!/bin/bash
make clean
make profile-build ARCH=native COMP=gcc ENV_CXXFLAGS="-DNNUE_EMBEDDING_OFF" -j$(nproc)
strip shashchess
mv 'shashchess' 'ShashChess41.2-native'
make clean