/*
  ShashChess, a UCI chess playing engine derived from Stockfish
  Copyright (C) 2004-2021 The Stockfish developers (see AUTHORS file)

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
#include <sstream>

#include "evaluate.h"
#include "misc.h"
#include "search.h"
#include "thread.h"
#include "tt.h"
#include "uci.h"
#include "syzygy/tbprobe.h"

using std::string;

namespace Stockfish {

UCI::OptionsMap Options; // Global object

namespace UCI {

/// 'On change' actions, triggered by an option's value change
void on_clear_hash(const Option&) { Search::clear(); }
void on_hash_size(const Option& o) { TT.resize(size_t(o)); }
void on_logger(const Option& o) { start_logger(o); }
void on_threads(const Option& o) { Threads.set(size_t(o)); }
void on_full_threads(const Option& o) { Threads.setFull(o); } //full threads patch
void on_persisted_learning(const Option& o) { if (!(o == "Off")) initLearning();}//Kelly learning
void on_tb_path(const Option& o) { Tablebases::init(o); }
void on_use_NNUE(const Option& ) { Eval::NNUE::init(); }
void on_eval_file(const Option& ) { Eval::NNUE::init(); }
//livebook begin
void on_livebook_url(const Option& o) { Search::setLiveBookURL(o); }
void on_livebook_timeout(const Option& o) { Search::setLiveBookTimeout(o); }
void on_live_book_retry(const Option& o) { Search::set_livebook_retry(o); }
void on_livebook_depth(const Option& o) { Search::set_livebook_depth(o); }
//livebook end

/// Our case insensitive less() function as required by UCI protocol
bool CaseInsensitiveLess::operator() (const string& s1, const string& s2) const {

  return std::lexicographical_compare(s1.begin(), s1.end(), s2.begin(), s2.end(),
         [](char c1, char c2) { return tolower(c1) < tolower(c2); });
}


/// UCI::init() initializes the UCI options to their hard-coded default values

void init(OptionsMap& o) {

  constexpr int MaxHashMB = Is64Bit ? 33554432 : 2048;

  o["Debug Log File"]              << Option("", on_logger);
  o["Threads"]               	   << Option(1, 1, 512, on_threads);
  o["Hash"]                  	   << Option(16, 1, MaxHashMB, on_hash_size);
  o["Clear Hash"]            	   << Option(on_clear_hash);
  o["Ponder"]                	   << Option(false);
  o["MultiPV"]               	   << Option(1, 1, 500);
  o["Move Overhead"]         	   << Option(10, 0, 5000);
  o["Slow Mover"]            	   << Option(100, 10, 1000);
  o["UCI_Chess960"]          	   << Option(false);
  o["UCI_AnalyseMode"]       	   << Option(false);
  //handicap mode
  o["UCI_LimitStrength"]     	   << Option(false);
  o["UCI_Elo"]               << Option(2850, 1350, 2850); //from ShashChess
  o["UCI_ShowWDL"]           << Option(false);
  o["SyzygyPath"]            	   << Option("<empty>", on_tb_path);
  o["SyzygyProbeDepth"]            << Option(1, 1, 100);
  o["SyzygyProbeLimit"]            << Option(7, 0, 7);
  o["Use NNUE"]              << Option(true, on_use_NNUE);
  // The default must follow the format nn-[SHA256 first 12 digits].nnue
  // for the build process (profile-build and fishtest) to work.
  o["EvalFile"]              << Option(EvalFileDefaultName, on_eval_file);
  //livebook begin
  o["Live Book"]             << Option(false);
  o["Live Book URL"]         << Option("http://www.chessdb.cn/cdb.php", on_livebook_url);
  o["Live Book Timeout"]     << Option(5000, 0, 10000, on_livebook_timeout);
  o["Live Book Retry"]       << Option(3, 1, 100, on_live_book_retry);
  o["Live Book Diversity"]   << Option(false);
  o["Live Book Contribute"]  << Option(false);
  o["Live Book Depth"]       << Option(100, 1, 100, on_livebook_depth);
  //livebook end
  o["Full depth threads"]    << Option(0, 0, 512, on_full_threads); //if this is used, must be after #Threads is set.
  o["Opening variety"]       << Option (0, 0, 40);
  o["Persisted learning"]    << Option("Off var Off var Standard var Self", "Off", on_persisted_learning);
  o["Read only learning"]    << Option(false);
  o["MCTS"]                  << Option("Off var Off var Single var Multi", "Off");
  o["Multi Strategy"]        << Option(20, 0, 100);
  o["Multi MinVisits"]       << Option(5, 0, 1000);  
  o["Concurrent Experience"] << Option (false);
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

Option::Option(const char* v, OnChange f) : type("string"), min(0), max(0), on_change(f)
{ defaultValue = currentValue = v; }

Option::Option(bool v, OnChange f) : type("check"), min(0), max(0), on_change(f)
{ defaultValue = currentValue = (v ? "true" : "false"); }

Option::Option(OnChange f) : type("button"), min(0), max(0), on_change(f)
{}

Option::Option(double v, int minv, int maxv, OnChange f) : type("spin"), min(minv), max(maxv), on_change(f)
{ defaultValue = currentValue = std::to_string(v); }

Option::Option(const char* v, const char* cur, OnChange f) : type("combo"), min(0), max(0), on_change(f)
{ defaultValue = v; currentValue = cur; }

Option::operator double() const {
  assert(type == "check" || type == "spin");
  return (type == "spin" ? stof(currentValue) : currentValue == "true");
}

Option::operator std::string() const {
  assert(type == "string");
  return currentValue;
}

bool Option::operator==(const char* s) const {
  assert(type == "combo");
  return   !CaseInsensitiveLess()(currentValue, s)
        && !CaseInsensitiveLess()(s, currentValue);
}


/// operator<<() inits options and assigns idx in the correct printing order

void Option::operator<<(const Option& o) {

  static size_t insert_order = 0;

  *this = o;
  idx = insert_order++;
}


/// operator=() updates currentValue and triggers on_change() action. It's up to
/// the GUI to check for option's limits, but we could receive the new value
/// from the user by console window, so let's check the bounds anyway.

Option& Option::operator=(const string& v) {

  assert(!type.empty());

  if (   (type != "button" && v.empty())
      || (type == "check" && v != "true" && v != "false")
      || (type == "spin" && (stof(v) < min || stof(v) > max)))
      return *this;

  if (type == "combo")
  {
      OptionsMap comboMap; // To have case insensitive compare
      string token;
      std::istringstream ss(defaultValue);
      while (ss >> token)
          comboMap[token] << Option();
      if (!comboMap.count(v) || v == "var")
          return *this;
  }

  if (type != "button")
      currentValue = v;

  if (on_change)
      on_change(*this);

  return *this;
}

} // namespace UCI

} // namespace Stockfish
