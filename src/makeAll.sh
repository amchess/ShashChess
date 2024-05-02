make profile-build ARCH=x86-64-vnni COMP=gcc
strip shashchess
mv 'shashchess' 'ShashChess35.3-x86-64-vnni'
make clean

make profile-build ARCH=x86-64-avx512 COMP=gcc
strip shashchess
mv 'shashchess' 'ShashChess35.3-x86-64-avx512'
make clean

make profile-build ARCH=x86-64-bmi2 COMP=gcc
strip shashchess
mv 'shashchess' 'ShashChess35.3-x86-64-bmi2'
make clean

make profile-build ARCH=x86-64-avx2 COMP=gcc
strip shashchess
mv 'shashchess' 'ShashChess35.3-x86-64-avx2'
make clean

make profile-build ARCH=x86-64-modern COMP=gcc
strip shashchess
mv 'shashchess' 'ShashChess35.3-x86-64-modern'
make clean

make profile-build ARCH=x86-64-sse41-popcnt COMP=gcc
strip shashchess
mv 'shashchess' 'ShashChess35.3-x86-64-sse41-popcnt'
make clean

make profile-build ARCH=x86-64-ssse3 COMP=gcc
strip shashchess
mv 'shashchess' 'ShashChess35.3-x86-64-ssse3'
make clean

make profile-build ARCH=x86-64-sse3-popcnt COMP=gcc
strip shashchess
mv 'shashchess' 'ShashChess35.3-x86-64-sse3-popcnt'
make clean

make profile-build ARCH=x86-64 COMP=gcc
strip shashchess
mv 'shashchess' 'ShashChess35.3-x86-64'
make clean

make profile-build ARCH=general-64 COMP=gcc
strip shashchess
mv 'shashchess' 'ShashChess35.3-general-64'
make clean

make profile-build ARCH=x86-32 COMPCC=gcc
strip shashchess
mv 'shashchess' 'ShashChess35.3-x86-32'
make clean

make profile-build ARCH=x86-32-old COMPCC=gcc
strip shashchess
mv 'shashchess' 'ShashChess35.3-x86-32-old'
make clean

make profile-build ARCH=general-32 COMPCC=gcc
strip shashchess
mv 'shashchess' 'ShashChess35.3-general-32'
make clean

