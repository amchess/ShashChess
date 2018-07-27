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

#ifndef UCI_H_INCLUDED
#define UCI_H_INCLUDED

#include <map>
#include <string>

#include "types.h"

class Position;

namespace UCI {

class Option;

/// Custom comparator because UCI options should be case insensitive
struct CaseInsensitiveLess {
  bool operator() (const std::string&, const std::string&) const;
};

/// Our options container is actually a std::map
typedef std::map<std::string, Option, CaseInsensitiveLess> OptionsMap;

/// Option class implements an option as defined by UCI protocol
class Option {

  typedef void (*OnChange)(const Option&);

public:
  Option(OnChange = nullptr);
  Option(bool v, OnChange = nullptr);
  Option(const char* v, OnChange = nullptr);
  //from Sugar
  template<class T> Option(T v, T minv, T maxv, OnChange f = nullptr) : type("spin"), min(minv), max(maxv), on_change(f)
  { defaultValue = currentValue = std::to_string(v); } //end from Sugar
  Option(const char* v, const char* cur, OnChange = nullptr);

  Option& operator=(const std::string&);
  void operator<<(const Option&);
  operator int() const; //from Sugar
  operator std::string() const;
  bool operator==(const char*) const;

private:
  friend std::ostream& operator<<(std::ostream&, const OptionsMap&);

  std::string defaultValue, currentValue, type;
  int min, max;
  size_t idx;
  OnChange on_change;
};

void init(OptionsMap&);
void loop(int argc, char* argv[]);
std::string value(Value v);
std::string value(Value v,Position& pos); //Shashin
std::string square(Square s);
std::string move(Move m, bool chess960);
std::string pv(Position& pos, Depth depth, Value alpha, Value beta); //Shashin
Move to_move(const Position& pos, std::string& str);

} // namespace UCI
extern bool pawnsPiecesSpace, passedPawns,initiativeToCalculate,eloLevel; //Shashin
extern UCI::OptionsMap Options;
//from Shashin
enum {
	NODES_TIME = 0,
	SLOW_MOVER = 84,
	MIN_THINK_TIME = 20,
	MOVE_OVERHEAD = 30,
	SYZ_50_MOVE = 1
};
//end from Shashin
#endif // #ifndef UCI_H_INCLUDED
