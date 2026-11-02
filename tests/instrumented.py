import argparse
import re
import sys
import subprocess
import pathlib
import os
import fnmatch

from testing import (
    EPD,
    ShashChess as Engine,
    MiniTestFramework,
    OrderedClassMembers,
    Valgrind,
    Syzygy,
)

PATH = pathlib.Path(__file__).parent.resolve()
CWD = os.getcwd()


def get_prefix(expect_failure=False):
    if args.valgrind:
        return Valgrind.get_valgrind_command(expect_failure)
    if args.valgrind_thread:
        return Valgrind.get_valgrind_thread_command()

    return []


def get_threads():
    if args.valgrind_thread or args.sanitizer_thread:
        return 4
    return 1


def get_path():
    return os.path.abspath(os.path.join(CWD, args.shashchess_path))


def postfix_check(output):
    if args.sanitizer_undefined:
        for idx, line in enumerate(output):
            if "runtime error:" in line:
                # print next possible 50 lines
                for i in range(50):
                    debug_idx = idx + i
                    if debug_idx < len(output):
                        print(output[debug_idx])
                return False

    if args.sanitizer_thread:
        for idx, line in enumerate(output):
            if "WARNING: ThreadSanitizer:" in line:
                # print next possible 50 lines
                for i in range(50):
                    debug_idx = idx + i
                    if debug_idx < len(output):
                        print(output[debug_idx])
                return False

    if args.valgrind or args.valgrind_thread:
        for line in output:
            match = re.search(r"ERROR SUMMARY:\s*(\d+) errors", line)
            if match and int(match.group(1)) > 0:
                return False

    return True


def ShashChess(*args, expect_failure=False, **kwargs):
    return Engine(get_prefix(expect_failure), get_path(), *args, expect_failure=expect_failure, **kwargs)


class TestCLI(metaclass=OrderedClassMembers):

    def beforeAll(self):
        pass

    def afterAll(self):
        pass

    def beforeEach(self):
        self.shashchess = None

    def afterEach(self):
        assert postfix_check(self.shashchess.get_output()) == True
        self.shashchess.clear_output()

    def test_eval(self):
        self.shashchess = ShashChess("eval".split(" "), True)
        assert self.shashchess.process.returncode == 0

    def test_go_nodes_1000(self):
        self.shashchess = ShashChess("go nodes 1000".split(" "), True)
        assert self.shashchess.process.returncode == 0

    def test_go_depth_10(self):
        self.shashchess = ShashChess("go depth 10".split(" "), True)
        assert self.shashchess.process.returncode == 0

    def test_go_perft_4(self):
        self.shashchess = ShashChess("go perft 4".split(" "), True)
        assert self.shashchess.process.returncode == 0

    def test_go_movetime_1000(self):
        self.shashchess = ShashChess("go movetime 1000".split(" "), True)
        assert self.shashchess.process.returncode == 0

    def test_go_wtime_8000_btime_8000_winc_500_binc_500(self):
        self.shashchess = ShashChess(
            "go wtime 8000 btime 8000 winc 500 binc 500".split(" "),
            True,
        )
        assert self.shashchess.process.returncode == 0

    def test_go_wtime_1000_btime_1000_winc_0_binc_0(self):
        self.shashchess = ShashChess(
            "go wtime 1000 btime 1000 winc 0 binc 0".split(" "),
            True,
        )
        assert self.shashchess.process.returncode == 0

    def test_go_wtime_1000_btime_1000_winc_0_binc_0_movestogo_5(self):
        self.shashchess = ShashChess(
            "go wtime 1000 btime 1000 winc 0 binc 0 movestogo 5".split(" "),
            True,
        )
        assert self.shashchess.process.returncode == 0

    def test_go_movetime_200(self):
        self.shashchess = ShashChess("go movetime 200".split(" "), True)
        assert self.shashchess.process.returncode == 0

    def test_go_nodes_20000_searchmoves_e2e4_d2d4(self):
        self.shashchess = ShashChess(
            "go nodes 20000 searchmoves e2e4 d2d4".split(" "), True
        )
        assert self.shashchess.process.returncode == 0

    def test_bench_128_threads_8_default_depth(self):
        self.shashchess = ShashChess(
            f"bench 128 {get_threads()} 8 default depth".split(" "),
            True,
        )
        assert self.shashchess.process.returncode == 0

    def test_bench_128_threads_3_bench_tmp_epd_depth(self):
        self.shashchess = ShashChess(
            f"bench 128 {get_threads()} 3 {os.path.join(PATH, 'bench_tmp.epd')} depth".split(
                " "
            ),
            True,
        )
        assert self.shashchess.process.returncode == 0

    def test_d(self):
        self.shashchess = ShashChess("d".split(" "), True)
        assert self.shashchess.process.returncode == 0

    def test_compiler(self):
        self.shashchess = ShashChess("compiler".split(" "), True)
        assert self.shashchess.process.returncode == 0

    def test_license(self):
        self.shashchess = ShashChess("license".split(" "), True)
        assert self.shashchess.process.returncode == 0

    def test_uci(self):
        self.shashchess = ShashChess("uci".split(" "), True)
        assert self.shashchess.process.returncode == 0

    def test_export_net_verify_nnue(self):
        current_path = os.path.abspath(os.getcwd())
        self.shashchess = ShashChess(
            f"export_net {os.path.join(current_path, 'verify.nnue')}".split(" "), True
        )
        assert self.shashchess.process.returncode == 0

    # verify the generated net equals the base net

    def test_network_equals_base(self):
        self.shashchess = ShashChess(
            ["uci"],
            True,
        )

        output = self.shashchess.process.stdout

        # find line
        for line in output.split("\n"):
            if "option name EvalFile type string default" in line:
                network = line.split(" ")[-1]
                break

        # find network file in src dir
        network = os.path.join(PATH.parent.resolve(), "src", network)

        if not os.path.exists(network):
            print(
                f"Network file {network} not found, please download the network file over the make command."
            )
            assert False

        diff = subprocess.run(["diff", network, f"verify.nnue"])

        assert diff.returncode == 0


class TestInteractive(metaclass=OrderedClassMembers):
    def beforeAll(self):
        self.shashchess = ShashChess()

    def afterAll(self):
        self.shashchess.quit()
        assert self.shashchess.close() == 0

    def afterEach(self):
        assert postfix_check(self.shashchess.get_output()) == True
        self.shashchess.clear_output()

    def test_startup_output(self):
        self.shashchess.starts_with("ShashChess")

    def test_uci_command(self):
        self.shashchess.send_command("uci")
        self.shashchess.equals("uciok")

    def test_set_threads_option(self):
        self.shashchess.send_command(f"setoption name Threads value {get_threads()}")

    def test_ucinewgame_and_startpos_nodes_1000(self):
        self.shashchess.send_command("ucinewgame")
        self.shashchess.send_command("position startpos")
        self.shashchess.send_command("go nodes 1000")
        self.shashchess.starts_with("bestmove")

    def test_ucinewgame_and_startpos_moves(self):
        self.shashchess.send_command("ucinewgame")
        self.shashchess.send_command("position startpos moves e2e4 e7e6")
        self.shashchess.send_command("go nodes 1000")
        self.shashchess.starts_with("bestmove")

    def test_fen_position_1(self):
        self.shashchess.send_command("ucinewgame")
        self.shashchess.send_command("position fen 5rk1/1K4p1/8/8/3B4/8/8/8 b - - 0 1")
        self.shashchess.send_command("go nodes 1000")
        self.shashchess.starts_with("bestmove")

    def test_fen_position_2_flip(self):
        self.shashchess.send_command("ucinewgame")
        self.shashchess.send_command("position fen 5rk1/1K4p1/8/8/3B4/8/8/8 b - - 0 1")
        self.shashchess.send_command("flip")
        self.shashchess.send_command("go nodes 1000")
        self.shashchess.starts_with("bestmove")

    def test_depth_5_with_callback(self):
        self.shashchess.send_command("ucinewgame")
        self.shashchess.send_command("position startpos")
        self.shashchess.send_command("go depth 5")

        def callback(output):
            regex = r"info depth \d+ seldepth \d+ multipv \d+ score cp -?\d+ nodes \d+ nps \d+ hashfull \d+ tbhits \d+ time \d+ pv"
            if output.startswith("info depth") and not re.match(regex, output):
                assert False
            if output.startswith("bestmove"):
                return True
            return False

        self.shashchess.check_output(callback)

    def test_ucinewgame_and_go_depth_9(self):
        self.shashchess.send_command("ucinewgame")
        self.shashchess.send_command("setoption name UCI_ShowWDL value true")
        self.shashchess.send_command("position startpos")
        self.shashchess.send_command("go depth 9")

        depth = 1

        def callback(output):
            nonlocal depth

            regex = rf"info depth {depth} seldepth \d+ multipv \d+ score cp -?\d+ wdl \d+ \d+ \d+ nodes \d+ nps \d+ hashfull \d+ tbhits \d+ time \d+ pv"

            if output.startswith("info depth"):
                if not re.match(regex, output):
                    assert False
                depth += 1

            if output.startswith("bestmove"):
                assert depth == 10
                return True

            return False

        self.shashchess.check_output(callback)

    def test_go_depth_3_with_mismatched_clock(self):
        self.shashchess.send_command("ucinewgame")
        self.shashchess.send_command("position startpos")
        self.shashchess.send_command("go depth 3 btime 1000")

        max_depth = 0

        def callback(output):
            nonlocal max_depth
            if output.startswith("info depth"):
                match = re.search(r"info depth (\d+)", output)
                if match:
                    max_depth = max(max_depth, int(match.group(1)))

            if output.startswith("bestmove"):
                assert max_depth == 3
                return True

            return False

        self.shashchess.check_output(callback)

    def test_clear_hash(self):
        self.shashchess.send_command("setoption name Clear Hash")

    def test_fen_position_mate_1(self):
        self.shashchess.send_command("ucinewgame")
        self.shashchess.send_command(
            "position fen 5K2/8/2qk4/2nPp3/3r4/6B1/B7/3R4 w - e6"
        )
        self.shashchess.send_command("go depth 18")

        self.shashchess.expect("* score mate 1 * pv d5e6")
        self.shashchess.equals("bestmove d5e6")

    def test_fen_position_mate_minus_1(self):
        self.shashchess.send_command("ucinewgame")
        self.shashchess.send_command(
            "position fen 2brrb2/8/p7/Q7/1p1kpPp1/1P1pN1K1/3P4/8 b - -"
        )
        self.shashchess.send_command("go depth 18")
        self.shashchess.expect("* score mate -1 *")
        self.shashchess.starts_with("bestmove")

    def test_fen_position_fixed_node(self):
        self.shashchess.send_command("ucinewgame")
        self.shashchess.send_command(
            "position fen 5K2/8/2P1P1Pk/6pP/3p2P1/1P6/3P4/8 w - - 0 1"
        )
        self.shashchess.send_command("go nodes 500000")
        self.shashchess.starts_with("bestmove")

    def test_fen_position_with_mate_go_depth(self):
        self.shashchess.send_command("ucinewgame")
        self.shashchess.send_command(
            "position fen 8/5R2/2K1P3/4k3/8/b1PPpp1B/5p2/8 w - -"
        )
        self.shashchess.send_command("go depth 18 searchmoves c6d7")
        self.shashchess.expect("* score mate 2 * pv c6d7 * f7f5")

        self.shashchess.starts_with("bestmove")

    def test_fen_position_with_mate_go_mate(self):
        self.shashchess.send_command("ucinewgame")
        self.shashchess.send_command(
            "position fen 8/5R2/2K1P3/4k3/8/b1PPpp1B/5p2/8 w - -"
        )
        self.shashchess.send_command("go mate 2 searchmoves c6d7")
        self.shashchess.expect("* score mate 2 * pv c6d7 *")

        self.shashchess.starts_with("bestmove")

    def test_fen_position_with_mate_go_nodes(self):
        self.shashchess.send_command("ucinewgame")
        self.shashchess.send_command(
            "position fen 8/5R2/2K1P3/4k3/8/b1PPpp1B/5p2/8 w - -"
        )
        self.shashchess.send_command("go nodes 500000 searchmoves c6d7")
        self.shashchess.expect("* score mate 2 * pv c6d7 * f7f5")

        self.shashchess.starts_with("bestmove")

    def test_fen_position_depth_27(self):
        self.shashchess.send_command("ucinewgame")
        self.shashchess.send_command(
            "position fen 1NR2B2/5p2/5p2/1p1kpp2/1P2rp2/2P1pB2/2P1P1K1/8 b - -"
        )
        self.shashchess.send_command("go depth 27")
        self.shashchess.contains("score mate -2")

        self.shashchess.starts_with("bestmove")

    def test_fen_position_with_mate_go_depth_and_promotion(self):
        self.shashchess.send_command("ucinewgame")
        self.shashchess.send_command(
            "position fen 8/5R2/2K1P3/4k3/8/b1PPpp1B/5p2/8 w - - moves c6d7 f2f1q"
        )
        self.shashchess.send_command("go depth 18")
        self.shashchess.expect("* score mate 1 * pv f7f5")
        self.shashchess.starts_with("bestmove f7f5")

    def test_fen_position_with_mate_go_depth_and_searchmoves(self):
        self.shashchess.send_command("ucinewgame")
        self.shashchess.send_command(
            "position fen 8/5R2/2K1P3/4k3/8/b1PPpp1B/5p2/8 w - -"
        )
        self.shashchess.send_command("go depth 18 searchmoves c6d7")
        self.shashchess.expect("* score mate 2 * pv c6d7 * f7f5")

        self.shashchess.starts_with("bestmove c6d7")

    def test_fen_position_with_moves_with_mate_go_depth_and_searchmoves(self):
        self.shashchess.send_command("ucinewgame")
        self.shashchess.send_command(
            "position fen 8/5R2/2K1P3/4k3/8/b1PPpp1B/5p2/8 w - - moves c6d7"
        )
        self.shashchess.send_command("go depth 18 searchmoves e3e2")
        self.shashchess.expect("* score mate -1 * pv e3e2 f7f5")
        self.shashchess.starts_with("bestmove e3e2")

    def test_verify_nnue_network(self):
        current_path = os.path.abspath(os.getcwd())
        ShashChess(
            f"export_net {os.path.join(current_path, 'verify.nnue')}".split(" "), True
        )

        self.shashchess.send_command("setoption name EvalFile value verify.nnue")
        self.shashchess.send_command("position startpos")
        self.shashchess.send_command("go depth 5")
        self.shashchess.starts_with("bestmove")

    def test_multipv_setting(self):
        self.shashchess.send_command("setoption name MultiPV value 4")
        self.shashchess.send_command("position startpos")
        self.shashchess.send_command("go depth 5")
        self.shashchess.starts_with("bestmove")

    def test_fen_position_with_skill_level(self):
        self.shashchess.send_command("setoption name Skill Level value 10")
        self.shashchess.send_command("position startpos")
        self.shashchess.send_command("go depth 5")
        self.shashchess.starts_with("bestmove")

        self.shashchess.send_command("setoption name Skill Level value 20")


class TestSyzygy(metaclass=OrderedClassMembers):
    def beforeAll(self):
        self.shashchess = ShashChess()

    def afterAll(self):
        self.shashchess.quit()
        assert self.shashchess.close() == 0

    def afterEach(self):
        assert postfix_check(self.shashchess.get_output()) == True
        self.shashchess.clear_output()

    def test_syzygy_setup(self):
        self.shashchess.starts_with("ShashChess")
        self.shashchess.send_command("uci")
        self.shashchess.send_command(
            f"setoption name SyzygyPath value {os.path.join(PATH, 'syzygy')}"
        )
        self.shashchess.expect(
            "info string Found 35 WDL and 35 DTZ tablebase files (up to 4-man)."
        )

    def test_syzygy_bench(self):
        self.shashchess.send_command("bench 128 1 8 default depth")
        self.shashchess.expect("Nodes searched  :*")

    def test_syzygy_position(self):
        self.shashchess.send_command("ucinewgame")
        self.shashchess.send_command("position fen 4k3/PP6/8/8/8/8/8/4K3 w - - 0 1")
        self.shashchess.send_command("go depth 5")

        def check_output(output):
            if "score cp 20000" in output or "score mate" in output:
                return True

        self.shashchess.check_output(check_output)
        self.shashchess.expect("bestmove *")

    def test_syzygy_position_2(self):
        self.shashchess.send_command("ucinewgame")
        self.shashchess.send_command("position fen 8/1P6/2B5/8/4K3/8/6k1/8 w - - 0 1")
        self.shashchess.send_command("go depth 5")

        def check_output(output):
            if "score cp 20000" in output or "score mate" in output:
                return True

        self.shashchess.check_output(check_output)
        self.shashchess.expect("bestmove *")

    def test_syzygy_position_3(self):
        self.shashchess.send_command("ucinewgame")
        self.shashchess.send_command("position fen 8/1P6/2B5/8/4K3/8/6k1/8 b - - 0 1")
        self.shashchess.send_command("go depth 5")

        def check_output(output):
            if "score cp -20000" in output or "score mate -" in output:
                return True

        self.shashchess.check_output(check_output)
        self.shashchess.expect("bestmove *")

    def test_syzygy_position_4(self):
        self.shashchess.send_command("ucinewgame")
        self.shashchess.send_command("position fen 8/8/7B/3B3P/7k/8/5K2/3r4 w - - 0 1")
        self.shashchess.send_command("go depth 1")
        self.shashchess.expect("bestmove *")

class TestEnPassantSanitization(metaclass=OrderedClassMembers):
    def beforeAll(self):
        self.shashchess = ShashChess()

    def afterAll(self):
        self.shashchess.quit()
        assert self.shashchess.close() == 0

    def afterEach(self):
        assert postfix_check(self.shashchess.get_output()) == True
        self.shashchess.clear_output()

    def test_position_1(self):
        self.shashchess.send_command("position fen rnbqkbnr/ppp1p1pp/5p2/3pP3/8/8/PPPP1PPP/RNBQKBNR w kq d6 0 3")
        self.shashchess.send_command("d")

        self.shashchess.expect_for_line_matching("Fen*", "*rnbqkbnr/ppp1p1pp/5p2/3pP3/8/8/PPPP1PPP/RNBQKBNR w kq d6 0 3*")

    def test_position_2(self):
        self.shashchess.send_command("position fen k7/8/8/1pP5/2K5/8/8/8 w - b6 0 1")
        self.shashchess.send_command("d")

        self.shashchess.expect_for_line_matching("Fen*", "*k7/8/8/1pP5/2K5/8/8/8 w - b6 0 1*")

    def test_position_3(self):
        self.shashchess.send_command("position fen k1r5/8/8/1pP5/2K5/8/8/8 w - b6 0 1")
        self.shashchess.send_command("d")

        self.shashchess.expect_for_line_matching("Fen*", "*k1r5/8/8/1pP5/2K5/8/8/8 w - - 0 1*")

    def test_position_4(self):
        self.shashchess.send_command("position fen k1r5/8/8/1pP5/8/2K5/8/8 w - b6 0 1")
        self.shashchess.send_command("d")

        self.shashchess.expect_for_line_matching("Fen*", "*k1r5/8/8/1pP5/8/2K5/8/8 w - - 0 1*")

    def test_position_5(self):
        self.shashchess.send_command("position fen k1r5/8/8/PpP5/8/2K5/8/8 w - b6 0 1")
        self.shashchess.send_command("d")

        self.shashchess.expect_for_line_matching("Fen*", "*k1r5/8/8/PpP5/8/2K5/8/8 w - b6 0 1*")

    def test_position_6(self):
        self.shashchess.send_command("position fen k1r5/8/8/PpP5/2K5/8/8/8 w - b6 0 1")
        self.shashchess.send_command("d")

        self.shashchess.expect_for_line_matching("Fen*", "*k1r5/8/8/PpP5/2K5/8/8/8 w - b6 0 1*")

    def test_position_7(self):
        self.shashchess.send_command("position fen k7/4b3/8/PpP5/1K6/8/8/8 w - b6 0 1")
        self.shashchess.send_command("d")

        self.shashchess.expect_for_line_matching("Fen*", "*k7/4b3/8/PpP5/1K6/8/8/8 w - b6 0 1*")

    def test_position_8(self):
        self.shashchess.send_command("position fen k7/b5b1/8/2PpP3/3K4/8/8/8 w - d6 0 1")
        self.shashchess.send_command("d")

        self.shashchess.expect_for_line_matching("Fen*", "*k7/b5b1/8/2PpP3/3K4/8/8/8 w - - 0 1*")

    def test_position_9(self):
        self.shashchess.send_command("position fen k7/8/8/r2pPK2/8/8/8/8 w - d6 0 1")
        self.shashchess.send_command("d")

        self.shashchess.expect_for_line_matching("Fen*", "*k7/8/8/r2pPK2/8/8/8/8 w - - 0 1*")

    def test_position_10(self):
        self.shashchess.send_command("position fen k7/8/8/r1PpPK2/8/8/8/8 w - d6 0 1")
        self.shashchess.send_command("d")

        self.shashchess.expect_for_line_matching("Fen*", "*k7/8/8/r1PpPK2/8/8/8/8 w - d6 0 1*")

    def test_position_11(self):
        self.shashchess.send_command("position fen kb6/8/8/3pP3/5K2/8/8/8 w - d6 0 1")
        self.shashchess.send_command("d")

        self.shashchess.expect_for_line_matching("Fen*", "*kb6/8/8/3pP3/5K2/8/8/8 w - d6 0 1*")

    def test_position_find_draw(self):
        self.shashchess.send_command("position fen q4kb1/3Q2nq/8/r3PpK1/2n5/7q/8/q7 w - f6 0 1 moves d7c8 f8f7 c8d7 f7f8 d7d8 f8f7")
        self.shashchess.send_command("go nodes 10000")

        def check_output(output):
            if fnmatch.fnmatch(output, "* score cp 0 * pv d8d7*"):
                return True

        self.shashchess.check_output(check_output)
        self.shashchess.expect("bestmove d8d7*")

class TestInvalidFEN(metaclass=OrderedClassMembers):
    def beforeEach(self):
        self.shashchess = None

    def afterEach(self):
        assert postfix_check(self.shashchess.get_output()) == True
        self.shashchess.clear_output()

    def _expect_critical(self, fen):
        self.shashchess = ShashChess(f"position fen {fen}".split(" "), True, expect_failure=True)
        assert self.shashchess.process.returncode != 0
        assert "CRITICAL ERROR" in self.shashchess.process.stdout

    def test_no_kings(self):
        self._expect_critical("8/8/8/8/8/8/8/8 w - - 0 1")

    def test_invalid_piece(self):
        self._expect_critical("rnbqkbnr/pppXpppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1")

    def test_invalid_side_to_move(self):
        self._expect_critical("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR x KQkq - 0 1")

    def test_pawns_on_back_rank(self):
        self._expect_critical("pppppppp/8/8/8/8/8/8/4K2k w - - 0 1")

    def test_invalid_skip_count(self):
        self._expect_critical("9/8/8/8/8/8/8/8 w - - 0 1")


class TestInvalidOptions(metaclass=OrderedClassMembers):
    def beforeAll(self):
        self.shashchess = ShashChess()

    def afterAll(self):
        self.shashchess.quit()
        assert self.shashchess.close() == 0

    def afterEach(self):
        assert postfix_check(self.shashchess.get_output()) == True
        self.shashchess.clear_output()

    # Ignore bogus spin values
    def test_spin_non_numeric(self):
        self.shashchess.send_command("setoption name Threads value abc")
        self.shashchess.send_command("isready")
        self.shashchess.equals("readyok")

    def test_spin_out_of_range(self):
        self.shashchess.send_command("setoption name Threads value 999999999999")
        self.shashchess.send_command("isready")
        self.shashchess.equals("readyok")

    def test_spin_negative(self):
        self.shashchess.send_command("setoption name Threads value -5")
        self.shashchess.send_command("isready")
        self.shashchess.equals("readyok")

    # Warn on bogus NUMA configs
    def test_numa_garbage(self):
        self.shashchess.send_command("setoption name NumaPolicy value zzz")
        self.shashchess.expect("*NumaPolicy: invalid value 'zzz', keeping previous config.*")

    def test_numa_malformed_range(self):
        self.shashchess.send_command("setoption name NumaPolicy value 0-")
        self.shashchess.expect("*NumaPolicy: invalid value '0-', keeping previous config.*")

    def test_numa_overflow(self):
        self.shashchess.send_command(
            "setoption name NumaPolicy value 99999999999999999999999"
        )
        self.shashchess.expect("*NumaPolicy: invalid value*keeping previous config.*")
        self.shashchess.send_command("isready")
        self.shashchess.equals("readyok")


class TestBenchFile(metaclass=OrderedClassMembers):
    def beforeEach(self):
        self.shashchess = None

    def afterEach(self):
        assert postfix_check(self.shashchess.get_output()) == True
        self.shashchess.clear_output()

    def _bench(self, name, content, expect_failure=False):
        with open(name, "w") as f:
            f.write(content)
        self.shashchess = ShashChess(f"bench 16 1 4 {name} depth".split(" "), True, expect_failure=expect_failure)

    def test_valid_file(self):
        self._bench("good.epd", "4k3/8/4K3/8/8/8/8/8 w - - 0 1\n")
        assert self.shashchess.process.returncode == 0
        assert "Nodes searched" in self.shashchess.process.stderr

    def test_empty_file(self):
        self._bench("empty.epd", "")
        assert self.shashchess.process.returncode == 0

    def test_malformed_fen(self):
        self._bench("bad.epd", "not a valid fen\n", expect_failure=True)
        assert self.shashchess.process.returncode != 0
        assert "CRITICAL ERROR" in self.shashchess.process.stdout

    def test_missing_file(self):
        self.shashchess = ShashChess(
            "bench 16 1 4 does_not_exist.epd depth".split(" "), True, expect_failure=True
        )
        assert self.shashchess.process.returncode != 0


def parse_args():
    parser = argparse.ArgumentParser(description="Run ShashChess with testing options")
    parser.add_argument("--valgrind", action="store_true", help="Run valgrind testing")
    parser.add_argument(
        "--valgrind-thread", action="store_true", help="Run valgrind-thread testing"
    )
    parser.add_argument(
        "--sanitizer-undefined",
        action="store_true",
        help="Run sanitizer-undefined testing",
    )
    parser.add_argument(
        "--sanitizer-thread", action="store_true", help="Run sanitizer-thread testing"
    )

    parser.add_argument(
        "--none", action="store_true", help="Run without any testing options"
    )
    parser.add_argument("shashchess_path", type=str, help="Path to ShashChess binary")

    return parser.parse_args()


if __name__ == "__main__":
    args = parse_args()

    EPD.create_bench_epd()
    Syzygy.download_syzygy()

    framework = MiniTestFramework()

    # Each test suite will be run inside a temporary directory
    framework.run(
        [
            TestCLI,
            TestInteractive,
            TestSyzygy,
            TestEnPassantSanitization,
            TestInvalidFEN,
            TestInvalidOptions,
            TestBenchFile,
        ]
    )

    EPD.delete_bench_epd()

    if framework.has_failed():
        sys.exit(1)

    sys.exit(0)
