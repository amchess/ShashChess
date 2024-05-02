/*
  ShashChess, a UCI chess playing engine derived from Stockfish
  Copyright (C) 2004-2024 Andrea Manzo, K.Kiniama and ShashChess developers (see AUTHORS file)

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

#include "uci.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <memory>
#include <optional>
#include <sstream>
#include <vector>
#include <cstdint>

#include "benchmark.h"
#include "evaluate.h"
#include "movegen.h"
#include "nnue/evaluate_nnue.h"
#include "nnue/nnue_architecture.h"
#include "position.h"
#include "search.h"
#include "syzygy/tbprobe.h"
#include "types.h"
#include "ucioption.h"
#include "perft.h"
//From ShashChess begin

#include "learn/learn.h"
#include "book/book.h"
#include "mcts/montecarlo.h"
//From ShashChess end
namespace ShashChess {

constexpr auto StartFEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
//from learning
constexpr int MaxHashMB = Is64Bit ? 33554432 : 2048;

UCI::UCI(int argc, char** argv) :
    cli(argc, argv) {

    evalFiles = {{Eval::NNUE::Big, {"EvalFile", EvalFileDefaultNameBig, "None", ""}},
                 {Eval::NNUE::Small, {"EvalFileSmall", EvalFileDefaultNameSmall, "None", ""}}};


    options["Debug Log File"] << Option("", [](const Option& o) { start_logger(o); });

    options["Threads"] << Option(1, 1, 1024, [this](const Option&) {
        threads.set({bookMan, evalFiles, options, threads, tt});
    });

    options["Hash"] << Option(16, 1, MaxHashMB, [this](const Option& o) {
        threads.main_thread()->wait_for_search_finished();
        tt.resize(o, options["Threads"]);
    });

    options["Clear Hash"] << Option([this](const Option&) { search_clear(); });
    options["Ponder"] << Option(false);
    options["MultiPV"] << Option(1, 1, MAX_MOVES);
    options["Move Overhead"] << Option(10, 0, 5000);
    options["Minimum Thinking Time"] << Option(100, 0, 5000);  //minimum thining time
    options["Slow Mover"] << Option(100, 10, 1000);            //slow mover
    options["nodestime"] << Option(0, 0, 10000);
    options["UCI_Chess960"] << Option(false);
    options["UCI_ShowWDL"] << Option(true);  //better Win Probability as the default
    //Book management begin
    for (int i = 0; i < BookManager::NumberOfBooks; ++i)
    {
        options[Util::format_string("CTG/BIN Book %d File", i + 1)]
          << Option(EMPTY, [this, i](const Option&) { bookMan.init(i, options); });
        options[Util::format_string("Book %d Width", i + 1)] << Option(1, 1, 20);
        options[Util::format_string("Book %d Depth", i + 1)] << Option(255, 1, 255);
        options[Util::format_string("(CTG) Book %d Only Green", i + 1)] << Option(true);
    }
    //Book management end
    options["SyzygyPath"] << Option("<empty>", [](const Option& o) { Tablebases::init(o); });
    options["SyzygyProbeDepth"] << Option(1, 1, 100);
    options["Syzygy50MoveRule"] << Option(true);
    options["SyzygyProbeLimit"] << Option(7, 0, 7);
    options["EvalFile"] << Option(EvalFileDefaultNameBig, [this](const Option&) {
        evalFiles = Eval::NNUE::load_networks(cli.binaryDirectory, options, evalFiles);
    });
    options["EvalFileSmall"] << Option(EvalFileDefaultNameSmall, [this](const Option&) {
        evalFiles = Eval::NNUE::load_networks(cli.binaryDirectory, options, evalFiles);
    });
    options["Full depth threads"] << Option(
      0, 0, 1024, [this](const Option& o) { threads.setFull(o); });  //full threads patch to check
    //From Kelly begin
    options["Persisted learning"] << Option("Off var Off var Standard var Self", "Off",
                                            [this](const Option& o) {
                                                if (!(o == "Off"))
                                                {
                                                    LD.set_learning_mode(options, o);
                                                }
                                            });
    options["Read only learning"] << Option(
      false, [this](const Option& o) { LD.set_readonly(o); });  //From Kelly begin
    //From Kelly end
    //From MCTS begin
    options["MCTS"] << Option(false);
    options["MCTSThreads"] << Option(1, 1, 512);
    options["MCTS Multi Strategy"] << Option(20, 0, 100);
    options["MCTS Multi MinVisits"] << Option(5, 0, 1000);
    //From MCTS end
    //livebook begin
#ifdef USE_LIVEBOOK
    options["Live Book"] << Option("Off var Off var NoEgtbs var Egtbs var Both", "Off",
                                            [this](const Option& o) {
                                                Search::set_livebook(o);
                                            });
    options["Live Book URL"] << Option("http://www.chessdb.cn/cdb.php",
                                       [this](const Option& o) { Search::setLiveBookURL(o); });    
    options["Live Book Timeout"] << Option(
      5000, 0, 10000, [this](const Option& o) { Search::setLiveBookTimeout(o); });
    options["Live Book Retry"] << Option(3, 1, 100);
    options["Live Book Diversity"] << Option(false);
    options["Live Book Contribute"] << Option(false);
    options["Live Book Depth"] << Option(
      255, 1, 255, [this](const Option& o) { Search::set_livebook_depth(o); });
#endif
    //livebook end
    options["Opening variety"] << Option(0, 0, 40);  //Opening discoverer
    options["Concurrent Experience"]
      << Option(false);  //for a same experience file on a same folder
    //Shashin personalities begin
    options["High Tal"] << Option(false);
    options["Middle Tal"] << Option(false);
    options["Low Tal"] << Option(false);
    options["Capablanca"] << Option(false);
    options["Low Petrosian"] << Option(false);
    options["Middle Petrosian"] << Option(false);
    options["High Petrosian"] << Option(false);
    //Shashin personalities end
    threads.set({bookMan, evalFiles, options, threads, tt});

    search_clear();  // After threads are up
}

void UCI::loop() {

    Position     pos;
    std::string  token, cmd;
    StateListPtr states(new std::deque<StateInfo>(1));

    pos.set(StartFEN, false, &states->back());

    for (int i = 1; i < cli.argc; ++i)
        cmd += std::string(cli.argv[i]) + " ";

    do
    {
        if (cli.argc == 1
            && !getline(std::cin, cmd))  // Wait for an input or an end-of-file (EOF) indication
            cmd = "quit";

        std::istringstream is(cmd);

        token.clear();  // Avoid a stale if getline() returns nothing or a blank line
        is >> std::skipws >> token;

        if (token == "quit" || token == "stop")
        {
            threads.stop = true;

            //Kelly begin
            if (token == "quit" && LD.is_enabled() && !LD.is_paused())
            {
                //Wait for the current search operation (if any) to stop
                //before proceeding to save experience data
                threads.main_thread()->wait_for_search_finished();

                //Perform Q-learning if enabled
                if (LD.learning_mode() == LearningMode::Self)
                {
                    putGameLineIntoLearningTable();
                }
                if (!LD.is_readonly())
                {
                    //Save to learning file
                    LD.persist(options);
                }
            }
            //Kelly end
        }
        // The GUI sends 'ponderhit' to tell that the user has played the expected move.
        // So, 'ponderhit' is sent if pondering was done on the same move that the user
        // has played. The search should continue, but should also switch from pondering
        // to the normal search.
        else if (token == "ponderhit")
            threads.main_manager()->ponder = false;  // Switch to the normal search

        else if (token == "uci")
            sync_cout << "id name " << engine_info(true) << "\n"
                      << options << "\nuciok" << sync_endl;

        else if (token == "setoption")
            setoption(is);
        else if (token == "go")
            go(pos, is, states);
        else if (token == "position")
            position(pos, is, states);
        else if (token == "ucinewgame")
        //Kelly and Khalid begin
        {
            if (LD.is_enabled())
            {
                //Perform Q-learning if enabled
                if (LD.learning_mode() == LearningMode::Self)
                {
                    putGameLineIntoLearningTable();
                }

                if (!LD.is_readonly())
                {
                    //Save to learning file
                    LD.persist(options);
                }
                setStartPoint();
            }
            search_clear();
        }
        //Kelly and Khalid end
        else if (token == "isready")
            sync_cout << "readyok" << sync_endl;

        // Add custom non-UCI commands, mainly for debugging purposes.
        // These commands must not be used during a search!
        else if (token == "flip")
            pos.flip();
        else if (token == "bench")
            bench(pos, is, states);
        else if (token == "d")
            sync_cout << pos << sync_endl;
        else if (token == "eval")
            trace_eval(pos);
        else if (token == "book")
            bookMan.show_moves(pos, options);
        else if (token == "compiler")
            sync_cout << compiler_info() << sync_endl;
        else if (token == "export_net")
        {
            std::optional<std::string> filename;
            std::string                f;
            if (is >> std::skipws >> f)
                filename = f;
            Eval::NNUE::save_eval(filename, Eval::NNUE::Big, evalFiles);
        }
        else if (token == "--help" || token == "help" || token == "--license" || token == "license")
            sync_cout
              << "\nShashChess is a powerful chess engine for playing and analyzing."
                 "\nIt is released as free software licensed under the GNU GPLv3 License."
                 "\nShashChess is normally used with a graphical user interface (GUI) and implements"
                 "\nthe Universal Chess Interface (UCI) protocol to communicate with a GUI, an API, etc."
                 "\nFor any further information, visit https://github.com/official-shashchess/ShashChess#readme"
                 "\nor read the corresponding README.md and Copying.txt files distributed along with this program.\n"
              << sync_endl;
        else if (!token.empty() && token[0] != '#')
            sync_cout << "Unknown command: '" << cmd << "'. Type help for more information."
                      << sync_endl;

    } while (token != "quit" && cli.argc == 1);  // The command-line arguments are one-shot
}
// handicapMode begin
inline int getHandicapDepth(int elo) {
    if (elo <= 1350)
    {
        return (int) (3 * elo / 1350 + 1);
    }
    if (elo <= 1999)
    {
        return (int) ((2 * elo - 104) / 649);
    }
    if (elo <= 2199)
    {
        return (int) ((2 * elo - 2607) / 199);
    }
    if (elo <= 2399)
    {
        return (int) ((2 * elo - 2410) / 199);
    }
    return (int) ((7 * elo - 10950) / 450);
}
// handicapMode end


void UCI::go(Position& pos, std::istringstream& is, StateListPtr& states) {

    Search::LimitsType limits;
    std::string        token;
    bool               ponderMode = false;

    limits.startTime = now();  // The search starts as early as possible
    while (is >> token)
        if (token == "searchmoves")  // Needs to be the last command on the line
            while (is >> token)
                limits.searchmoves.push_back(to_move(pos, token));

        else if (token == "wtime")
            is >> limits.time[WHITE];
        else if (token == "btime")
            is >> limits.time[BLACK];
        else if (token == "winc")
            is >> limits.inc[WHITE];
        else if (token == "binc")
            is >> limits.inc[BLACK];
        else if (token == "movestogo")
            is >> limits.movestogo;
        else if (token == "depth")
            is >> limits.depth;
        else if (token == "nodes")
            is >> limits.nodes;
        else if (token == "movetime")
            is >> limits.movetime;
        else if (token == "mate")
            is >> limits.mate;
        else if (token == "perft")
            is >> limits.perft;
        else if (token == "infinite")
            limits.infinite = 1;
        else if (token == "ponder")
            ponderMode = true;

    Eval::NNUE::verify(options, evalFiles);

    if (limits.perft)
    {
        perft(pos.fen(), limits.perft, options["UCI_Chess960"]);
        return;
    }

    threads.start_thinking(options, pos, states, limits, ponderMode);
}

void UCI::bench(Position& pos, std::istream& args, StateListPtr& states) {
    std::string token;
    uint64_t    num, nodes = 0, cnt = 1;

    std::vector<std::string> list = setup_bench(pos, args);

    num = count_if(list.begin(), list.end(),
                   [](const std::string& s) { return s.find("go ") == 0 || s.find("eval") == 0; });

    TimePoint elapsed = now();

    for (const auto& cmd : list)
    {
        std::istringstream is(cmd);
        is >> std::skipws >> token;

        if (token == "go" || token == "eval")
        {
            std::cerr << "\nPosition: " << cnt++ << '/' << num << " (" << pos.fen() << ")"
                      << std::endl;
            if (token == "go")
            {
                go(pos, is, states);
                threads.main_thread()->wait_for_search_finished();
                nodes += threads.nodes_searched();
            }
            else
                trace_eval(pos);
        }
        else if (token == "setoption")
            setoption(is);
        else if (token == "position")
            position(pos, is, states);
        else if (token == "ucinewgame")
        {
            //Kelly begin
            if (LD.is_enabled())
            {
                if (LD.learning_mode() == LearningMode::Self && !LD.is_paused())
                {
                    putGameLineIntoLearningTable();
                }
                setStartPoint();
            }
            //Kelly end
            search_clear();  // Search::clear() may take a while
            elapsed = now();
        }
    }

    elapsed = now() - elapsed + 1;  // Ensure positivity to avoid a 'divide by zero'

    dbg_print();

    std::cerr << "\n==========================="
              << "\nTotal time (ms) : " << elapsed << "\nNodes searched  : " << nodes
              << "\nNodes/second    : " << 1000 * nodes / elapsed << std::endl;
}

void UCI::trace_eval(Position& pos) {
    StateListPtr states(new std::deque<StateInfo>(1));
    Position     p;
    p.set(pos.fen(), options["UCI_Chess960"], &states->back());

    Eval::NNUE::verify(options, evalFiles);

    sync_cout << "\n" << Eval::trace(p) << sync_endl;
}

void UCI::search_clear() {
    threads.main_thread()->wait_for_search_finished();
    // livebook begin
#ifdef USE_LIVEBOOK
    ShashChess::Search::set_livebook_depth((int) options["Live Book Depth"]);
    ShashChess::Search::set_g_inBook((int)options["Live Book Retry"]);
#endif
    // livebook end
    tt.clear(options["Threads"]);
    MCTS.clear();  // mcts
    threads.clear();
    Tablebases::init(options["SyzygyPath"]);  // Free mapped file
    Search::initWinProbability();             //from ShashChess
}

void UCI::setoption(std::istringstream& is) {
    threads.main_thread()->wait_for_search_finished();
    options.setoption(is);
}

void UCI::position(Position& pos, std::istringstream& is, StateListPtr& states) {
    Move        m;
    std::string token, fen;

    is >> token;

    if (token == "startpos")
    {
        fen = StartFEN;
        is >> token;  // Consume the "moves" token, if any
    }
    else if (token == "fen")
        while (is >> token && token != "moves")
            fen += token + " ";
    else
        return;

    states = StateListPtr(new std::deque<StateInfo>(1));  // Drop the old state and create a new one
    pos.set(fen, options["UCI_Chess960"], &states->back());

    // Parse the move list, if any
    while (is >> token && (m = to_move(pos, token)) != Move::none())
    {
        //Kelly begin
        if (LD.is_enabled() && LD.learning_mode() != LearningMode::Self && !LD.is_paused())
        {
            PersistedLearningMove persistedLearningMove;

            persistedLearningMove.key                      = pos.key();
            persistedLearningMove.learningMove.depth       = 0;
            persistedLearningMove.learningMove.move        = m;
            persistedLearningMove.learningMove.score       = VALUE_NONE;
            persistedLearningMove.learningMove.performance = 100;

            LD.add_new_learning(persistedLearningMove.key, persistedLearningMove.learningMove);
        }
        //Kelly end

        states->emplace_back();
        pos.do_move(m, states->back());
    }
}

int UCI::to_cp(Value v) { return 100 * v / NormalizeToPawnValue; }

std::string UCI::value(Value v) {
    assert(-VALUE_INFINITE < v && v < VALUE_INFINITE);

    std::stringstream ss;

    if (std::abs(v) < VALUE_TB_WIN_IN_MAX_PLY)
        ss << "cp " << to_cp(v);
    else if (std::abs(v) <= VALUE_TB)
    {
        const int ply = VALUE_TB - std::abs(v);  // recompute ss->ply
        ss << "cp " << (v > 0 ? 20000 - ply : -20000 + ply);
    }
    else
        ss << "mate " << (v > 0 ? VALUE_MATE - v + 1 : -VALUE_MATE - v) / 2;

    return ss.str();
}
//for Shashin theory begin
uint8_t UCI::getWinProbability(int v, int ply) {
    double correctionFactor = (std::min(240, ply) / 64.0);
    double forExp1 =
      ((((0.38036525 * correctionFactor - 6.94334517) * correctionFactor + 23.17882135)
        * correctionFactor)
       + 307.36768407);
    double forExp2 =
      (((-2.29434733 * correctionFactor + 13.27689788) * correctionFactor - 14.26828904)
       * correctionFactor)
      + 63.45318330;
    double winrateToMove = double(
      0.5 + 1000 / (1 + std::exp((forExp1 - (std::clamp(double(v), -4000.0, 4000.0))) / forExp2)));
    double winrateOpponent = double(
      0.5 + 1000 / (1 + std::exp((forExp1 - (std::clamp(double(-v), -4000.0, 4000.0))) / forExp2)));
    double winrateDraw = 1000 - winrateToMove - winrateOpponent;
    return static_cast<uint8_t>(round((winrateToMove + winrateDraw / 2.0) / 10.0));
}
//for Shashin theory end

std::string UCI::square(Square s) {
    return std::string{char('a' + file_of(s)), char('1' + rank_of(s))};
}

std::string UCI::move(Move m, bool chess960) {
    if (m == Move::none())
        return "(none)";

    if (m == Move::null())
        return "0000";

    Square from = m.from_sq();
    Square to   = m.to_sq();

    if (m.type_of() == CASTLING && !chess960)
        to = make_square(to > from ? FILE_G : FILE_C, rank_of(from));

    std::string move = square(from) + square(to);

    if (m.type_of() == PROMOTION)
        move += " pnbrqk"[m.promotion_type()];

    return move;
}

namespace {
// The win rate model returns the probability of winning (in per mille units) given an
// eval and a game ply. It fits the LTC fishtest statistics rather accurately.
int win_rate_model(Value v, int ply) {

    // The fitted model only uses data for moves in [8, 120], and is anchored at move 32.
    double m = std::clamp(ply / 2 + 1, 8, 120) / 32.0;

    // The coefficients of a third-order polynomial fit is based on the fishtest data
    // for two parameters that need to transform eval to the argument of a logistic
    // function.
    constexpr double as[] = {-1.06249702, 7.42016937, 0.89425629, 348.60356174};
    constexpr double bs[] = {-5.33122190, 39.57831533, -90.84473771, 123.40620748};

    // Enforce that NormalizeToPawnValue corresponds to a 50% win rate at move 32.
    static_assert(NormalizeToPawnValue == int(0.5 + as[0] + as[1] + as[2] + as[3]));

    double a = (((as[0] * m + as[1]) * m + as[2]) * m) + as[3];
    double b = (((bs[0] * m + bs[1]) * m + bs[2]) * m) + bs[3];

    // Return the win rate in per mille units, rounded to the nearest integer.
    return int(0.5 + 1000 / (1 + std::exp((a - double(v)) / b)));
}
}

std::string UCI::wdl(Value v, int ply) {
    std::stringstream ss;

    int wdl_w = win_rate_model(v, ply);
    int wdl_l = win_rate_model(-v, ply);
    int wdl_d = 1000 - wdl_w - wdl_l;
    ss << " wdl " << wdl_w << " " << wdl_d << " " << wdl_l;

    return ss.str();
}

Move UCI::to_move(const Position& pos, std::string& str) {
    if (str.length() == 5)
        str[4] = char(tolower(str[4]));  // The promotion piece character must be lowercased

    for (const auto& m : MoveList<LEGAL>(pos))
        if (str == move(m, pos.is_chess960()))
            return m;

    return Move::none();
}

}  // namespace ShashChess
