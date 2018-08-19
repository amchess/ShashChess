/*
  ShashChess, a UCI chess playing engine derived from Stockfish
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2015 Marco Costalba, Joona Kiiski, Tord Romstad
  Copyright (C) 2015-2018 Marco Costalba, Joona Kiiski, Gary Linscott, Tord Romstad

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

#include <algorithm>
#include <cassert>
#include <ostream>
//Hash		
#include <iostream>
//end_Hash
#include <thread>

#include "misc.h"
#include "search.h"
#include "thread.h"
#include "tt.h"
#include "uci.h"
#include "syzygy/tbprobe.h"
#include "polybook.h"

using std::string;

UCI::OptionsMap Options; // Global object

namespace UCI {

/// 'On change' actions, triggered by an option's value change
void on_clear_hash(const Option&) { Search::clear(); }
void on_hash_size(const Option& o) { TT.resize(o); }
void on_large_pages(const Option& o) { TT.resize(o); }  // warning is ok, will be removed
void on_logger(const Option& o) { start_logger(o); }
void on_threads(const Option& o) { Threads.set(o); }
void on_tb_path(const Option& o) { Tablebases::init(o); }
//Hash	
void on_HashFile(const Option& o) { TT.set_hash_file_name(o); }
void SaveHashToFile(const Option&) { TT.save(); }
void LoadHashFromFile(const Option&) { TT.load(); }
void LoadEpdToHash(const Option&) { TT.load_epd_to_hash(); }
//end_Hash

void on_book_file(const Option& o) { polybook.init(o); }
void on_best_book_move(const Option& o) { polybook.set_best_book_move(o); }
void on_book_depth(const Option& o) { polybook.set_book_depth(o); }

/// Our case insensitive less() function as required by UCI protocol
bool CaseInsensitiveLess::operator() (const string& s1, const string& s2) const {

  return std::lexicographical_compare(s1.begin(), s1.end(), s2.begin(), s2.end(),
         [](char c1, char c2) { return tolower(c1) < tolower(c2); });
}


/// init() initializes the UCI options to their hard-coded default values

void init(OptionsMap& o) {

  // at most 2^32 clusters.
  constexpr int MaxHashMB = Is64Bit ? 131072 : 2048;

  unsigned n = std::thread::hardware_concurrency();
  if (!n) n = 1;
  
  o["Debug Log File"]        << Option("", on_logger);
  o["Threads"]               << Option(n, unsigned(1), unsigned(512), on_threads);
  o["Large Pages"]           << Option(false, on_large_pages);
  o["Hash"]                  << Option(16, 1, MaxHashMB, on_hash_size);
  o["Clear_Hash"]            << Option(on_clear_hash);
  o["NeverClearHash"]           << Option(false);
  o["Ponder"]                << Option(false);
  o["MultiPV"]               << Option(1, 1, 500);
  o["UCI_Chess960"]          << Option(false);
  //handicap mode
  o["UCI_LimitStrength"]     << Option(false);
  o["UCI_Elo"]               << Option(2800, 1500, 2800);
  //Hash save capability 
  o["HashFile"]                 << Option("hash.hsh", on_HashFile);
  o["SaveHashToFile"]           << Option(SaveHashToFile);
  o["LoadHashFromFile"]         << Option(LoadHashFromFile);
  o["LoadEpdToHash"]            << Option(LoadEpdToHash);
  o["SyzygyPath"]            << Option("<empty>", on_tb_path);
  o["SyzygyProbeDepth"]      << Option(1, 1, 100);
  o["SyzygyProbeLimit"]      << Option(7, 0, 7);
  o["Deep Analysis Mode"]     << Option(0, 0,  8);
  o["Clean Search"]            << Option(false);
  o["Variety"]               << Option (0, 0, 40);

  //Polyglot Book management
  /*
      - Book file: default = <empty>, Path+Filename to the BrainFish book, for example d:\Chess\Cerebellum_Light_Poly.bin
      - BestBookMove: default = true, if false the move is selected according to the weights in the Polyglot book
        Brain Fish can of course handle also moves or openings which are never played by the book, for example 1. ..e6.
        You can play such openings with using a Standard opening book for your GUI which for example plays only 1. ..e6 as black and then stops.
        Another option is to edit the Poyglot Book.
      - BookDepth: default 255, maximum number of moves played out of the book in one row.

   */
  o["Book enabled"]          << Option(false);
  o["Book file"]              << Option("<empty>", on_book_file);
  o["Best book move"]          << Option(true, on_best_book_move);
  o["Book depth"]             << Option(255, 1, 255, on_book_depth);

  o["Tal"]                   << Option(false);
  o["Capablanca"]            << Option(false);
  o["Petrosian"]             << Option(false);

}


/// operator<<() is used to print all the options default values in chronological
/// insertion order (the idx field) and in the format defined by the UCI protocol.

std::ostream& operator<<(std::ostream& os, const OptionsMap& om) {

  for (size_t idx = 0; idx < om.size(); ++idx)
      for (const auto& it : om)
          if (it.second.idx == idx)
          {
              const Option& o = it.second;
              os << "\noption name " << it.first << " type " << o.type;

              if (o.type == "string" || o.type == "check" || o.type == "combo")
                  os << " default " << o.defaultValue;

              if (o.type == "spin")
                  os << " default " << int(stof(o.defaultValue))
                     << " min "     << o.min
                     << " max "     << o.max;

              break;
          }

  return os;
}


/// Option class constructors and conversion operators
Option::Option(const char* v, const char* cur, OnChange f) : type("combo"), min(0), max(0), on_change(f)
{
	defaultValue = v; currentValue = cur;
}

Option::Option(const char* v, OnChange f) : type("string"), min(0), max(0), on_change(f)
{ defaultValue = currentValue = v; }

Option::Option(bool v, OnChange f) : type("check"), min(0), max(0), on_change(f)
{ defaultValue = currentValue = (v ? "true" : "false"); }

Option::Option(OnChange f) : type("button"), min(0), max(0), on_change(f)
{}

Option::operator std::string() const {
  assert(type == "string");
  return currentValue;
}

bool Option::operator==(const char* s) const {
  assert(type == "combo");
  return    !CaseInsensitiveLess()(currentValue, s)
         && !CaseInsensitiveLess()(s, currentValue);
}


/// operator<<() inits options and assigns idx in the correct printing order

void Option::operator<<(const Option& o) {

  static size_t insert_order = 0;

  *this = o;
  idx = insert_order++;
}


/// operator=() updates currentValue and triggers on_change() action. It's up to
/// the GUI to check for option's limits, but we could receive the new value from
/// the user by console window, so let's check the bounds anyway.

Option& Option::operator=(const string& v) {

  assert(!type.empty());

  if (   (type != "button" && v.empty())
      || (type == "check" && v != "true" && v != "false")
      || (type == "spin" && (stof(v) < min || stof(v) > max)))
      return *this;

  if (type != "button")
      currentValue = v;

  if (on_change)
      on_change(*this);

  return *this;
}

} // namespace UCI
