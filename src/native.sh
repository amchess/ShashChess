make profile-build ARCH=native COMP=gcc -j$(nproc)
strip shashchess
mv 'shashchess' 'ShashChessDev-native'
make clean