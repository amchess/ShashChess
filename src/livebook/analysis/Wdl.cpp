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

#ifdef USE_LIVEBOOK
    #include "Wdl.h"

Wdl::Wdl(const uint32_t wins_, const uint32_t draws_, const uint32_t losses_) :
    wins(wins_),
    draws(draws_),
    losses(losses_) {}

Wdl* Wdl::opponent() const { return new Wdl{losses, draws, wins}; }

uint32_t Wdl::get_wins() const { return wins; }

uint32_t Wdl::get_draws() const { return draws; }

uint32_t Wdl::get_losses() const { return losses; }

double Wdl::get_win_probability() const {
    uint32_t sum = get_sum();

    if (sum == 0)
    {
        return 0.f;
    }

    return static_cast<double>(wins) / sum;
}

double Wdl::get_draw_probability() const {
    const uint32_t sum = get_sum();

    if (sum == 0)
    {
        return 0.f;
    }

    return static_cast<double>(draws) / sum;
}

double Wdl::get_loss_probability() const {
    const uint32_t sum = get_sum();

    if (sum == 0)
    {
        return 0.f;
    }

    return static_cast<double>(losses) / sum;
}

double Wdl::get_success_probability() const {
    const uint32_t sum = get_sum();

    if (sum == 0)
    {
        return 0.f;
    }

    return (static_cast<double>(wins) + 0.5 * static_cast<double>(draws)) / sum;
}

Wdl* Wdl::flip() const { return new Wdl(losses, draws, wins); }

uint32_t Wdl::get_sum() const { return wins + draws + losses; }

bool Wdl::operator==(const Wdl& other) const {
    return wins == other.wins && draws == other.draws && losses == other.losses;
}

bool Wdl::operator!=(const Wdl& other) const { return !(*this == other); }

bool Wdl::operator<(const Wdl& other) const {
    if (wins < other.wins)
    {
        return true;
    }

    if (wins > other.wins)
    {
        return false;
    }

    if (draws < other.draws)
    {
        return true;
    }

    if (draws > other.draws)
    {
        return false;
    }

    return losses < other.losses;
}

bool Wdl::operator>(const Wdl& other) const { return !(*this < other || *this == other); }

bool Wdl::operator<=(const Wdl& other) const { return *this < other || *this == other; }

bool Wdl::operator>=(const Wdl& other) const { return *this > other || *this == other; }
#endif
