#!/bin/bash
make clean
make profile-build ARCH=native COMP=gcc ENV_CXXFLAGS="-DNNUE_EMBEDDING_OFF" -j$(nproc)
strip shashchess
mv 'shashchess' 'ShashChess41.1-native'
make clean