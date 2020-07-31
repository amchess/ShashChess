make nnue-gen-sfen-from-original-eval ARCH=x86-64-avx512 COMP=gcc
strip shashchess
mv 'shashchess' 'ShashChessNNUEGEN 1.0-x86-64-avx512'
make clean

make nnue-gen-sfen-from-original-eval ARCH=x86-64-bmi2 COMP=gcc
strip shashchess
mv 'shashchess' 'ShashChessNNUEGEN 1.0-x86-64-bmi2'
make clean

make nnue-gen-sfen-from-original-eval ARCH=x86-64-avx2 COMP=gcc
strip shashchess
mv 'shashchess' 'ShashChessNNUEGEN 1.0-x86-64-avx2'
make clean

make nnue-gen-sfen-from-original-eval ARCH=x86-64-sse42 COMP=gcc
strip shashchess
mv 'shashchess' 'ShashChessNNUEGEN 1.0-x86-64-sse42'
make clean

make nnue-gen-sfen-from-original-eval ARCH=x86-64-sse41 COMP=gcc
strip shashchess
mv 'shashchess' 'ShashChessNNUEGEN 1.0-x86-64-sse41'
make clean

make nnue-gen-sfen-from-original-eval ARCH=x86-64-ssse3 COMP=gcc
strip shashchess
mv 'shashchess' 'ShashChessNNUEGEN 1.0-x86-64-ssse3'
make clean

make nnue-gen-sfen-from-original-eval ARCH=x86-64-sse3-popcnt COMP=gcc
strip shashchess
mv 'shashchess' 'ShashChessNNUEGEN 1.0-x86-64-sse3-popcnt'
make clean

make nnue-gen-sfen-from-original-eval ARCH=x86-64-sse3 COMP=gcc
strip shashchess
mv 'shashchess' 'ShashChessNNUEGEN 1.0-x86-64-sse3'
make clean

make nnue-gen-sfen-from-original-eval ARCH=x86-64 COMP=gcc
strip shashchess
mv 'shashchess' 'ShashChessNNUEGEN 1.0-x86-64'
make clean

make nnue-gen-sfen-from-original-eval ARCH=ppc-64 COMP=gcc
strip shashchess
mv 'shashchess' 'ShashChessNNUEGEN 1.0-ppc-64'
make clean

make nnue-gen-sfen-from-original-eval ARCH=general-64 COMP=gcc
strip shashchess
mv 'shashchess' 'ShashChessNNUEGEN 1.0-general-64'
make clean
