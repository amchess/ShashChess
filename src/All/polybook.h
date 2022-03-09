/*
BrainFish, a UCI chess playing engine derived from Stockfish
Copyright (C) 2016-2017 Thomas Zipproth

BrainFish is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

BrainFish is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef POLYBOOK_H_INCLUDED
#define POLYBOOK_H_INCLUDED

#include "bitboard.h"
#include "position.h"
#include "string.h"

typedef struct {
    uint64_t key;
    uint16_t move;
    uint16_t weight;
    uint32_t learn;
} PolyHash;

class PolyBook
{
public:

    PolyBook();
    ~PolyBook();

    void init(const std::string& bookfile);
    Stockfish::Move probe(Stockfish::Position& pos, bool bestBookMove);

private:

    Stockfish::Key polyglot_key(const Stockfish::Position& pos);
    Stockfish::Move pg_move_to_sf_move(const Stockfish::Position & pos, unsigned short pg_move);

    int find_first_key(uint64_t key);
    int get_key_data();

    bool check_draw(Stockfish::Position& pos, Stockfish::Move m);

    int keycount;
    PolyHash *polyhash;
    bool enabled;

    int index_first;
    int index_best;
    int index_rand;
    int index_count;
    int index_weight_count;
};

extern PolyBook polybook[2];
#endif // #ifndef POLYBOOK_H_INCLUDED