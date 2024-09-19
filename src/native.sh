make profile-build ARCH=native COMP=gcc -j$(nproc)
strip shashchess
mv shashchess ShashChess36-native
make clean
