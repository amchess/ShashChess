/*
  ShashChess, a UCI chess playing engine derived from Stockfish
  Copyright (C) 2004-2026 The Stockfish developers (see AUTHORS file)
  Copyright (C) 2018-2026 The ShashChess developers (see AUTHORS file)

  ShashChess is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  ShashChess is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// Standalone NNUE embedding for universal binary builds

#include "../evaluate.h"

#ifdef UNIVERSAL_BINARY_MACOS_X86_SLICE

// In a macOS universal binary the network is embedded only in the arm64 slice,
// and the x86-64 slice mmaps it from the arm64 slice.

    #include <climits>
    #include <cstdint>
    #include <fcntl.h>
    #include <mach-o/dyld.h>
    #include <stdlib.h>
    #include <sys/mman.h>
    #include <unistd.h>

// Must be kept in sync with patch_x86_slice.sh
extern const volatile ShashChess::u64 gUniversalNNUEOffset = 0xCAFE0FF5E70FF5E7ULL;
extern const volatile ShashChess::u64 gUniversalNNUESize   = 0xCAFE512ECAFE512EULL;

static const unsigned char* map_embedded_nnue() {
    char            path[PATH_MAX];
    ShashChess::u32 len = sizeof(path);
    if (_NSGetExecutablePath(path, &len) != 0)
        return nullptr;

    char        resolved[PATH_MAX];
    const char* file = realpath(path, resolved) ? resolved : path;

    int fd = open(file, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return nullptr;

    // Align down to page size for mmap
    const ShashChess::u64 pageSize = ShashChess::u64(sysconf(_SC_PAGESIZE));
    const ShashChess::u64 base     = gUniversalNNUEOffset & ~(pageSize - 1);
    const ShashChess::u64 pad      = gUniversalNNUEOffset - base;

    void* p =
      mmap(nullptr, size_t(gUniversalNNUESize + pad), PROT_READ, MAP_PRIVATE, fd, off_t(base));
    close(fd);
    if (p == MAP_FAILED)
        return nullptr;

    return reinterpret_cast<const unsigned char*>(p) + pad;
}

extern const unsigned char* const gEmbeddedNNUEData = map_embedded_nnue();
extern const unsigned int         gEmbeddedNNUESize = static_cast<unsigned int>(gUniversalNNUESize);

#elif defined(NNUE_EMBEDDING_OFF)

// Universal build with an external network (NNUE_EMBEDDING_OFF=yes).
// The slices are built and PGO-benched against the full nnue_embed.o, but the
// final link uses this object (temp_builds/nnue_embed_off.o), so it MUST define
// the symbols the slices reference. The 1-byte placeholder makes the internal
// load fail, and the engine then loads the .nnue file (EvalFile).
// "extern" is required: in C++ a namespace-scope const without it has internal
// linkage, the symbol would not be exported and the final link would fail with
// "undefined reference to gEmbeddedNNUEData / gEmbeddedNNUESize".
extern const unsigned char gEmbeddedNNUEData[] = {0x0};
extern const unsigned int  gEmbeddedNNUESize   = 1;

#else

extern const unsigned char gEmbeddedNNUEData[] =
    #ifdef __has_embed
  {
        #embed EvalFileDefaultName
};
const unsigned int padding = 0;
    #else
        #include "network_dump.inc"
  ;
const unsigned int padding = 1;  // trailing NUL byte
    #endif
extern const unsigned int gEmbeddedNNUESize = sizeof(gEmbeddedNNUEData) - padding;

#endif
