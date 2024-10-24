make profile-build ARCH=native COMP=gcc -j$(nproc)
strip shashchess
mv shashchess ShashChess37-native
make clean
