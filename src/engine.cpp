/*
  ShashChess, a UCI chess playing engine derived from Stockfish
  Copyright (C) 2004-2025 The ShashChess developers (see AUTHORS file)

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

#include "engine.h"

#include <algorithm>
#include <cassert>
#include <deque>
#include <iosfwd>
#include <memory>
#include <ostream>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

#include "evaluate.h"
#include "misc.h"
#include "nnue/network.h"
#include "nnue/nnue_common.h"
#include "numa.h"
#include "perft.h"
#include "position.h"
#include "search.h"
#include "syzygy/tbprobe.h"
#include "types.h"
#include "uci.h"
#include "ucioption.h"
#include "wdl/win_probability.h"
#include "learn/learn.h"      //learning
#include "book/book.h"        //book management
#include "mcts/montecarlo.h"  //mcts
namespace ShashChess {

namespace NN = Eval::NNUE;

constexpr auto StartFEN   = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
constexpr int  MaxHashMB  = Is64Bit ? 33554432 : 2048;
int            MaxThreads = std::max(1024, 4 * int(get_hardware_concurrency()));

Engine::Engine(std::optional<std::string> path) :
    binaryDirectory(CommandLine::get_binary_directory(
      path.value_or(""), CommandLine::get_working_directory())),  //learning
    numaContext(NumaConfig::from_system()),
    states(new std::deque<StateInfo>(1)),
    threads(),
    networks(
      numaContext,
      NN::Networks(
        NN::NetworkBig({EvalFileDefaultNameBig, "None", ""}, NN::EmbeddedNNUEType::BIG),
        NN::NetworkSmall({EvalFileDefaultNameSmall, "None", ""}, NN::EmbeddedNNUEType::SMALL))) {
    pos.set(StartFEN, false, &states->back());


    options.add(  //
      "Debug Log File", Option("", [](const Option& o) {
          start_logger(o);
          return std::nullopt;
      }));

    options.add(  //
      "NumaPolicy", Option("auto", [this](const Option& o) {
          set_numa_config_from_option(o);
          return numa_config_information_as_string() + "\n"
               + thread_allocation_information_as_string();
      }));

    options.add(  //
      "Threads", Option(1, 1, MaxThreads, [this](const Option&) {
          resize_threads();
          return thread_allocation_information_as_string();
      }));

    options.add(  //
      "Hash", Option(16, 1, MaxHashMB, [this](const Option& o) {
          set_tt_size(o);
          return std::nullopt;
      }));

    options.add(  //
      "Clear Hash", Option([this](const Option&) {
          search_clear();
          return std::nullopt;
      }));

    options.add("Ponder", Option(false));

    options.add("MultiPV", Option(1, 1, MAX_MOVES));

    options.add("Move Overhead", Option(10, 0, 5000));

    options.add("Minimum Thinking Time", Option(100, 0, 5000));

    options.add("Slow Mover", Option(100, 10, 1000));

    options.add("nodestime", Option(0, 0, 10000));

    options.add("UCI_Chess960", Option(false));

    options.add("UCI_ShowWDL", Option(true));

    // Book management
    for (int i = 0; i < BookManager::NumberOfBooks; ++i)
    {
        options.add(Util::format_string("CTG/BIN Book %d File", i + 1),
                    Option(EMPTY, [this, i](const Option&) {
                        init_bookMan(i);
                        return std::nullopt;
                    }));

        options.add(Util::format_string("Book %d Width", i + 1), Option(1, 1, 20));

        options.add(Util::format_string("Book %d Depth", i + 1), Option(255, 1, 255));

        options.add(Util::format_string("(CTG) Book %d Only Green", i + 1), Option(true));
    }

    options.add(  //
      "SyzygyPath", Option("", [](const Option& o) {
          Tablebases::init(o);
          return std::nullopt;
      }));

    options.add("SyzygyProbeDepth", Option(1, 1, 100));

    options.add("Syzygy50MoveRule", Option(true));

    options.add("SyzygyProbeLimit", Option(7, 0, 7));

    options.add(  //
      "EvalFile", Option(EvalFileDefaultNameBig, [this](const Option& o) {
          load_big_network(o);
          return std::nullopt;
      }));

    options.add(  //
      "EvalFileSmall", Option(EvalFileDefaultNameSmall, [this](const Option& o) {
          load_small_network(o);
          return std::nullopt;
      }));

    options.add(  //
      "Full depth threads", Option(0, 0, 1024, [this](const Option& o) {
          resize_full(o);
          return thread_binding_information_as_string();
      }));

    // From learning
    options.add("Persisted learning", Option("Off var Off var Standard var Self", "Off",
                                             [this](const Option& o) -> std::optional<std::string> {
                                                 if (!(o == "Off"))
                                                 {
                                                     LD.set_learning_mode(get_options(), o);
                                                 }
                                                 return std::optional<std::string>{};
                                             }));

    options.add("Read only learning", Option(false, [](const Option& o) {
                    LD.set_readonly(o);
                    return std::nullopt;
                }));

    options.add("Experience Book", Option(false, [this](const Option&) {
                    LD.init(get_options());
                    return std::nullopt;
                }));

    options.add("Experience Book Max Moves", Option(100, 1, 100));

    options.add("Experience Book Min Depth", Option(4, 1, 255));

    // From MCTS
    options.add("MCTS by Shashin", Option(false));

    options.add("MCTSThreads", Option(1, 1, 512));

    options.add("MCTS Multi Strategy", Option(20, 0, 100));

    options.add("MCTS Multi MinVisits", Option(5, 0, 1000));

    options.add("MCTS Explore", Option(false));

    // LiveBook options
#ifdef USE_LIVEBOOK
    options.add("LiveBook Proxy Url", Option("", [](const Option& o) -> std::optional<std::string> {
                    Search::set_proxy_url(o);
                    return std::optional<std::string>{};
                }));

    options.add("LiveBook Lichess Games", Option(false, [](const Option& o) {
                    Search::set_use_lichess_games(o);
                    return std::nullopt;
                }));

    options.add("LiveBook Lichess Masters", Option(false, [](const Option& o) {
                    Search::set_use_lichess_masters(o);
                    return std::nullopt;
                }));

    options.add("LiveBook Lichess Player",
                Option("", [](const Option& o) -> std::optional<std::string> {
                    Search::set_lichess_player(o);
                    return std::optional<std::string>{};
                }));

    options.add("LiveBook Lichess Player Color",
                Option("White var Both var White var Black", "White",
                       [](const Option& o) -> std::optional<std::string> {
                           std::string str = o;
                           std::transform(str.begin(), str.end(), str.begin(),
                                          [](const unsigned char c) { return std::tolower(c); });
                           Search::set_lichess_player_color(str);
                           return std::optional<std::string>{};
                       }));

    options.add("LiveBook ChessDB", Option(false, [](const Option& o) {
                    Search::set_use_chess_db(o);
                    return std::nullopt;
                }));

    options.add("LiveBook Depth", Option(255, 1, 255, [](const Option& o) {
                    Search::set_livebook_depth(o);
                    return std::nullopt;
                }));

    options.add("ChessDB Tablebase", Option(false, [](const Option& o) {
                    Search::set_use_chess_db_tablebase(o);
                    return std::nullopt;
                }));

    options.add("Lichess Tablebase", Option(false, [](const Option& o) {
                    Search::set_use_lichess_tablebase(o);
                    return std::nullopt;
                }));

    options.add("ChessDB Contribute", Option(false, [](const Option& o) {
                    Search::set_chess_db_contribute(o);
                    return std::nullopt;
                }));
#endif

    options.add("Variety", Option("Off var Off var Standard var Psychological", "Off",
                                  [](const Option& o) -> std::optional<std::string> {
                                      Search::set_variety(o);
                                      return std::optional<std::string>{};
                                  }));

    options.add("Concurrent Experience", Option(false));

    // Shashin personalities begin
    options.add("High Tal",
                Option(false, [this](const Option&) noexcept -> std::optional<std::string> {
                    resize_threads();
                    return std::nullopt;
                }));
    options.add("Middle Tal",
                Option(false, [this](const Option&) noexcept -> std::optional<std::string> {
                    resize_threads();
                    return std::nullopt;
                }));
    options.add("Low Tal",
                Option(false, [this](const Option&) noexcept -> std::optional<std::string> {
                    resize_threads();
                    return std::nullopt;
                }));
    options.add("Capablanca",
                Option(false, [this](const Option&) noexcept -> std::optional<std::string> {
                    resize_threads();
                    return std::nullopt;
                }));
    options.add("Low Petrosian",
                Option(false, [this](const Option&) noexcept -> std::optional<std::string> {
                    resize_threads();
                    return std::nullopt;
                }));
    options.add("Middle Petrosian",
                Option(false, [this](const Option&) noexcept -> std::optional<std::string> {
                    resize_threads();
                    return std::nullopt;
                }));
    options.add("High Petrosian",
                Option(false, [this](const Option&) noexcept -> std::optional<std::string> {
                    resize_threads();
                    return std::nullopt;
                }));
    // Shashin personalities end
    load_networks();
    resize_threads();
}

std::uint64_t Engine::perft(const std::string& fen, Depth depth, bool isChess960) {
    verify_networks();

    return Benchmark::perft(fen, depth, isChess960);
}

void Engine::go(Search::LimitsType& limits) {
    assert(limits.perft == 0);
    verify_networks();

    threads.start_thinking(options, pos, states, limits);
}
void Engine::stop() { threads.stop = true; }

void Engine::search_clear() {
    wait_for_search_finished();

    MCTS.clear();  //mcts
    tt.clear(threads);
    threads.clear();

    // @TODO wont work with multiple instances
    Tablebases::init(options["SyzygyPath"]);  // Free mapped files
}

void Engine::set_on_update_no_moves(std::function<void(const Engine::InfoShort&)>&& f) {
    updateContext.onUpdateNoMoves = std::move(f);
}

void Engine::set_on_update_full(std::function<void(const Engine::InfoFull&)>&& f) {
    updateContext.onUpdateFull = std::move(f);
}

void Engine::set_on_iter(std::function<void(const Engine::InfoIter&)>&& f) {
    updateContext.onIter = std::move(f);
}

void Engine::set_on_bestmove(std::function<void(std::string_view, std::string_view)>&& f) {
    updateContext.onBestmove = std::move(f);
}

void Engine::set_on_verify_networks(std::function<void(std::string_view)>&& f) {
    onVerifyNetworks = std::move(f);
}

void Engine::wait_for_search_finished() { threads.main_thread()->wait_for_search_finished(); }

void Engine::set_position(const std::string& fen, const std::vector<std::string>& moves) {
    // Drop the old state and create a new one
    states = StateListPtr(new std::deque<StateInfo>(1));
    pos.set(fen, options["UCI_Chess960"], &states->back());

    for (const auto& move : moves)
    {
        auto m = UCIEngine::to_move(pos, move);

        if (m == Move::none())
            break;
        //learning begin
        if (LD.is_enabled() && LD.learning_mode() != LearningMode::Self && !LD.is_paused())
        {
            PersistedLearningMove persistedLearningMove;

            persistedLearningMove.key                      = pos.key();
            persistedLearningMove.learningMove.depth       = 0;
            persistedLearningMove.learningMove.move        = m;
            persistedLearningMove.learningMove.score       = VALUE_NONE;
            persistedLearningMove.learningMove.performance = WDLModel::get_win_probability(0, pos);
            LD.add_new_learning(persistedLearningMove.key, persistedLearningMove.learningMove);
        }
        //learning end
        states->emplace_back();
        pos.do_move(m, states->back());
    }
}

// modifiers

void Engine::set_numa_config_from_option(const std::string& o) {
    if (o == "auto" || o == "system")
    {
        numaContext.set_numa_config(NumaConfig::from_system());
    }
    else if (o == "hardware")
    {
        // Don't respect affinity set in the system.
        numaContext.set_numa_config(NumaConfig::from_system(false));
    }
    else if (o == "none")
    {
        numaContext.set_numa_config(NumaConfig{});
    }
    else
    {
        numaContext.set_numa_config(NumaConfig::from_string(o));
    }

    // Force reallocation of threads in case affinities need to change.
    resize_threads();
    threads.ensure_network_replicated();
}

void Engine::resize_threads() {
    threads.wait_for_search_finished();
    //from shashin begin
    ShashinConfig shCfg;
    shCfg.highTal         = options["High Tal"];
    shCfg.middleTal       = options["Middle Tal"];
    shCfg.lowTal          = options["Low Tal"];
    shCfg.capablanca      = options["Capablanca"];
    shCfg.highPetrosian   = options["High Petrosian"];
    shCfg.middlePetrosian = options["Middle Petrosian"];
    shCfg.lowPetrosian    = options["Low Petrosian"];
    threads.set(numaContext.get_numa_config(), {bookMan, options, threads, tt, networks, shCfg},
                updateContext);  //book management
    //from shashin end
    // Reallocate the hash with the new threadpool size
    set_tt_size(options["Hash"]);
    threads.ensure_network_replicated();
}
void Engine::init_bookMan(int bookIndex) { bookMan.init(bookIndex, options); }  //book management
void Engine::resize_full(size_t requested) { threads.setFull(requested); }      //full threads patch

void Engine::set_tt_size(size_t mb) {
    wait_for_search_finished();
    tt.resize(mb, threads);
}

void Engine::set_ponderhit(bool b) { threads.main_manager()->ponder = b; }

// network related

void Engine::verify_networks() const {
    networks->big.verify(options["EvalFile"], onVerifyNetworks);
    networks->small.verify(options["EvalFileSmall"], onVerifyNetworks);
}

void Engine::load_networks() {
    networks.modify_and_replicate([this](NN::Networks& networks_) {
        networks_.big.load(binaryDirectory, options["EvalFile"]);
        networks_.small.load(binaryDirectory, options["EvalFileSmall"]);
    });
    threads.clear();
    threads.ensure_network_replicated();
}

void Engine::load_big_network(const std::string& file) {
    networks.modify_and_replicate(
      [this, &file](NN::Networks& networks_) { networks_.big.load(binaryDirectory, file); });
    threads.clear();
    threads.ensure_network_replicated();
}

void Engine::load_small_network(const std::string& file) {
    networks.modify_and_replicate(
      [this, &file](NN::Networks& networks_) { networks_.small.load(binaryDirectory, file); });
    threads.clear();
    threads.ensure_network_replicated();
}

void Engine::save_network(const std::pair<std::optional<std::string>, std::string> files[2]) {
    networks.modify_and_replicate([&files](NN::Networks& networks_) {
        networks_.big.save(files[0].first);
        networks_.small.save(files[1].first);
    });
}

// utility functions

void Engine::trace_eval() const {
    StateListPtr trace_states(new std::deque<StateInfo>(1));
    Position     p;
    p.set(pos.fen(), options["UCI_Chess960"], &trace_states->back());

    verify_networks();

    sync_cout << "\n" << Eval::trace(p, *networks) << sync_endl;
}

const OptionsMap& Engine::get_options() const { return options; }
OptionsMap&       Engine::get_options() { return options; }

std::string Engine::fen() const { return pos.fen(); }

void Engine::flip() { pos.flip(); }
void Engine::show_moves_bookMan(const Position& position) {
    bookMan.show_moves(position, options);
}  //book management
std::string Engine::visualize() const {
    std::stringstream ss;
    ss << pos;
    return ss.str();
}

int Engine::get_hashfull(int maxAge) const { return tt.hashfull(maxAge); }

std::vector<std::pair<size_t, size_t>> Engine::get_bound_thread_count_by_numa_node() const {
    auto                                   counts = threads.get_bound_thread_count_by_numa_node();
    const NumaConfig&                      cfg    = numaContext.get_numa_config();
    std::vector<std::pair<size_t, size_t>> ratios;
    NumaIndex                              n = 0;
    for (; n < counts.size(); ++n)
        ratios.emplace_back(counts[n], cfg.num_cpus_in_numa_node(n));
    if (!counts.empty())
        for (; n < cfg.num_numa_nodes(); ++n)
            ratios.emplace_back(0, cfg.num_cpus_in_numa_node(n));
    return ratios;
}

std::string Engine::get_numa_config_as_string() const {
    return numaContext.get_numa_config().to_string();
}

std::string Engine::numa_config_information_as_string() const {
    auto cfgStr = get_numa_config_as_string();
    return "Available processors: " + cfgStr;
}

std::string Engine::thread_binding_information_as_string() const {
    auto              boundThreadsByNode = get_bound_thread_count_by_numa_node();
    std::stringstream ss;
    if (boundThreadsByNode.empty())
        return ss.str();

    bool isFirst = true;

    for (auto&& [current, total] : boundThreadsByNode)
    {
        if (!isFirst)
            ss << ":";
        ss << current << "/" << total;
        isFirst = false;
    }

    return ss.str();
}

std::string Engine::thread_allocation_information_as_string() const {
    std::stringstream ss;

    size_t threadsSize = threads.size();
    ss << "Using " << threadsSize << (threadsSize > 1 ? " threads" : " thread");

    auto boundThreadsByNodeStr = thread_binding_information_as_string();
    if (boundThreadsByNodeStr.empty())
        return ss.str();

    ss << " with NUMA node thread binding: ";
    ss << boundThreadsByNodeStr;

    return ss.str();
}

}
