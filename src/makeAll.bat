@echo off
ren C:\MinGW\mingw64-730-pse mingw64
ren C:\MinGW\mingw32-730-pd mingw32
make clean
mingw32-make profile-build ARCH=x86-64 COMP=mingw CXX=x86_64-w64-mingw32-g++ -j14
strip shashchess.exe
ren shashchess.exe "ShashChess Pro 1.1.1-x86-64.exe"
make clean
mingw32-make profile-build ARCH=x86-64-modern COMP=mingw CXX=x86_64-w64-mingw32-g++ -j14
strip shashchess.exe
ren shashchess.exe "ShashChess Pro 1.1.1-x86-64-modern.exe"
make clean
mingw32-make profile-build ARCH=x86-64-bmi2 COMP=mingw CXX=x86_64-w64-mingw32-g++ -j14
strip shashchess.exe
ren shashchess.exe "ShashChess Pro 1.1.1-x86-64-bmi2.exe"
mingw32-make build ARCH=ppc-64 COMP=mingw
strip shashchess.exe
ren shashchess.exe "ShashChess Pro 1.1.1-ppc-64.exe"
make clean
mingw32-make profile-build ARCH=general-64 COMP=mingw CXX=x86_64-w64-mingw32-g++ -j14
strip shashchess.exe
ren shashchess.exe "ShashChess Pro 1.1.1-general-64.exe"
make clean
mingw32-make build ARCH=ppc-32 COMP=mingw
strip shashchess.exe
ren shashchess.exe "ShashChess Pro 1.1.1-ppc-32.exe"
make clean
mingw32-make profile-build ARCH=armv7 COMP=mingw CXX=x86_64-w64-mingw32-g++ -j14
strip shashchess.exe
ren shashchess.exe "ShashChess Pro 1.1.1-armv7.exe"
make clean
mingw32-make -f MakeFile profile-build ARCH=general-32 COMP=mingw
strip shashchess.exe
ren shashchess.exe "ShashChess Pro 1.1.1-general-32.exe"
make clean
mingw32-make -f MakeFile profile-build ARCH=x86-32 COMP=mingw
strip shashchess.exe
ren shashchess.exe "ShashChess Pro 1.1.1-x86-32.exe"
make clean
mingw32-make -f MakeFile profile-build ARCH=x86-32-old COMP=mingw
strip shashchess.exe
ren shashchess.exe "ShashChess Pro 1.1.1-x86-32-old.exe"
make clean
ren C:\MinGW\mingw64 mingw64-730-pse 
ren C:\MinGW\mingw32 mingw32-730-pd
make clean
pause