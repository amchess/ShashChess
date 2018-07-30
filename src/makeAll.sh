make profile-build ARCH=x86-64 COMP=gcc COMPCC=gcc-7.3.0
strip shashchess
mv 'shashchess' 'ShashChess Pro 1.1.1-x86-64'
make clean

make profile-build ARCH=x86-64-modern COMP=gcc COMPCC=gcc-7.3.0
strip shashchess
mv 'shashchess' 'ShashChess Pro 1.1.1-x86-64-modern'
make clean

make profile-build ARCH=x86-64-bmi2 COMP=gcc COMPCC=gcc-7.3.0
strip shashchess
mv 'shashchess' 'ShashChess Pro 1.1.1-x86-64-bmi2'
make clean

make profile-build ARCH=x86-32 COMP=gcc COMPCC=gcc-7.3.0
strip shashchess
mv 'shashchess' 'ShashChess Pro 1.1.1-x86-32'
make clean

make profile-build ARCH=x86-32-old COMP=gcc COMPCC=gcc-7.3.0
strip shashchess
mv 'shashchess' 'ShashChess Pro 1.1.1-x86-32-old'
make clean

make profile-build ARCH=ppc-64 COMP=gcc COMPCC=gcc-7.3.0
strip shashchess
mv 'shashchess' 'ShashChess Pro 1.1.1-ppc-64'
make clean

make profile-build ARCH=ppc-32 COMP=gcc COMPCC=gcc-7.3.0
strip shashchess
mv 'shashchess' 'ShashChess Pro 1.1.1-ppc-32'
make clean

make profile-build ARCH=armv7 COMP=gcc COMPCC=gcc-7.3.0
strip shashchess
mv 'shashchess' 'ShashChess Pro 1.1.1-armv7'
make clean

make profile-build ARCH=general-64 COMP=gcc COMPCC=gcc-7.3.0
strip shashchess
mv 'shashchess' 'ShashChess Pro 1.1.1-general-64'
make clean

make profile-build ARCH=general-32 COMP=gcc COMPCC=gcc-7.3.0
strip shashchess
mv 'shashchess' 'ShashChess Pro 1.1.1-general-32'
make clean
