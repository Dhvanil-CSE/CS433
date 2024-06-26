/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2024 The Stockfish developers (see AUTHORS file)

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
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
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <memory>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

#include "benchmark.h"
#include "evaluate.h"
#include "movegen.h"
#include "nnue/network.h"
#include "nnue/nnue_common.h"
#include "perft.h"
#include "position.h"
#include "search.h"
#include "syzygy/tbprobe.h"
#include "types.h"
#include "ucioption.h"

namespace Stockfish {

constexpr auto StartFEN  = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
constexpr int  MaxHashMB = Is64Bit ? 33554432 : 2048;


namespace NN = Eval::NNUE;


UCI::UCI(int argc, char** argv) :
    networks(NN::Networks(
      NN::NetworkBig({EvalFileDefaultNameBig, "None", ""}, NN::EmbeddedNNUEType::BIG),
      NN::NetworkSmall({EvalFileDefaultNameSmall, "None", ""}, NN::EmbeddedNNUEType::SMALL))),
    cli(argc, argv) {

    options["Debug Log File"] << Option("", [](const Option& o) { start_logger(o); });

    options["Threads"] << Option(1, 1, 1024, [this](const Option&) {
        threads.set({options, threads, tt, networks});
    });

    options["Hash"] << Option(16, 1, MaxHashMB, [this](const Option& o) {
        threads.main_thread()->wait_for_search_finished();
        tt.resize(o, options["Threads"]);
    });

    options["Clear Hash"] << Option([this](const Option&) { search_clear(); });
    options["Ponder"] << Option(false);
    options["MultiPV"] << Option(1, 1, MAX_MOVES);
    options["Skill Level"] << Option(20, 0, 20);
    options["Move Overhead"] << Option(10, 0, 5000);
    options["nodestime"] << Option(0, 0, 10000);
    options["UCI_Chess960"] << Option(false);
    options["UCI_LimitStrength"] << Option(false);
    options["UCI_Elo"] << Option(1320, 1320, 3190);
    options["UCI_ShowWDL"] << Option(false);
    options["SyzygyPath"] << Option("<empty>", [](const Option& o) { Tablebases::init(o); });
    options["SyzygyProbeDepth"] << Option(1, 1, 100);
    options["Syzygy50MoveRule"] << Option(true);
    options["SyzygyProbeLimit"] << Option(7, 0, 7);
    options["EvalFile"] << Option(EvalFileDefaultNameBig, [this](const Option& o) {
        networks.big.load(cli.binaryDirectory, o);
    });
    options["EvalFileSmall"] << Option(EvalFileDefaultNameSmall, [this](const Option& o) {
        networks.small.load(cli.binaryDirectory, o);
    });

    networks.big.load(cli.binaryDirectory, options["EvalFile"]);
    networks.small.load(cli.binaryDirectory, options["EvalFileSmall"]);

    threads.set({options, threads, tt, networks});

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

        if (token == "CS433")
            cs433_project(pos, states);

        if (token == "quit" || token == "stop")
            threads.stop = true;

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
            search_clear();
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
        else if (token == "compiler")
            sync_cout << compiler_info() << sync_endl;
        else if (token == "export_net")
        {
            std::pair<std::optional<std::string>, std::string> files[2];

            if (is >> std::skipws >> files[0].second)
                files[0].first = files[0].second;

            if (is >> std::skipws >> files[1].second)
                files[1].first = files[1].second;

            networks.big.save(files[0].first);
            networks.small.save(files[1].first);
        }
        else if (token == "--help" || token == "help" || token == "--license" || token == "license")
            sync_cout
              << "\nStockfish is a powerful chess engine for playing and analyzing."
                 "\nIt is released as free software licensed under the GNU GPLv3 License."
                 "\nStockfish is normally used with a graphical user interface (GUI) and implements"
                 "\nthe Universal Chess Interface (UCI) protocol to communicate with a GUI, an API, etc."
                 "\nFor any further information, visit https://github.com/official-stockfish/Stockfish#readme"
                 "\nor read the corresponding README.md and Copying.txt files distributed along with this program.\n"
              << sync_endl;
        else if (!token.empty() && token[0] != '#')
            sync_cout << "Unknown command: '" << cmd << "'. Type help for more information."
                      << sync_endl;

    } while (token != "quit" && cli.argc == 1);  // The command-line arguments are one-shot
}

Square *AvailablePosn (Stockfish::Position &pos) {
    Square *availableSq = new Square [32];
    int counter = 0;
    for (int i=0; i<64; i++) {
        if (pos.empty((Square)i)) {
            availableSq[counter++] = (Square)i;
        }
    }
    return availableSq;
} 

Value getVal (Stockfish::Position &pos, const Eval::NNUE::Networks& networks) {
    Value v = networks.big.evaluate(pos,false);
    v = UCI::to_cp(v,pos);
    return v;
}

std::pair<Move,Value> makeOneMove (Stockfish::Position &pos, Stockfish::StateListPtr &states, Square fromSq,const Eval::NNUE::Networks& networks) { 
    
    Square *t = AvailablePosn(pos);
    Value checkVal = 0;
    Move bestMove;
    for (int i=0; i<32; i++) {
        Move m(fromSq,t[i]);
        // std::cout<<"moving from "<<fromSq<<" to "<<t[i]<<"\n";
        // Move u(t[i],fromSq);
        StateInfo *newSt = new StateInfo;
        pos.move433(m,*newSt);
        // StateListPtr states(new std::deque<StateInfo>(1));


        // sync_cout << "\n" << Eval::trace(pos, networks) << sync_endl;        
        // std::cout<<"move completed\n";
        Value newEval = getVal(pos,networks);
        // std::cout<<"move completed345\n";
        if (newEval > checkVal) {
            checkVal = newEval;
            bestMove = m;
        }
        pos.undo433(m);
        delete newSt;
        // StateInfo *newState = new StateInfo;
        // pos.move433(u,*newState);
        // delete newState;
    }
    delete t;
    std::pair<Move,Value> returner = std::make_pair(bestMove,checkVal);
    // std::cout<<"returning\n";
    return returner;
}

std::pair <Move,Value> makeFinalMove (Stockfish::Position &pos, Stockfish::StateListPtr &states, Square fromSq[],const Eval::NNUE::Networks& networks) {
    Move bestMove;
    // std::cout<<"entered fourth move\n";
    Value bestVal = 0;
    for (int i=0; i<7; i++) {
        if (fromSq[i] == -1) {
            continue;
        }
        else {
            // std::cout<<"ha yes\n";
            std::pair<Move,Value> thisSq = makeOneMove(pos,states,fromSq[i],networks);
            // std::cout<<"return successful\n";
            // std::cout<<"best val so far is "<<thisSq.second<<"\n";
            if (thisSq.second > bestVal) {
                
                bestVal = thisSq.second;
                bestMove = thisSq.first;
            }
        }
    }
    return std::make_pair(bestMove,bestVal);
}

std::pair <Move,std::pair<Move,Value>> makeSemiFinalMove (Stockfish::Position &pos, Stockfish::StateListPtr &states, Square fromSq[],const Eval::NNUE::Networks& networks) {
    Move bestMove[2];
    // std::cout<<"Entered third move\n";
    Value bestVal = 0;
    for (int i=0; i<7; i++) {
        if (fromSq[i] == -1) {
            continue;
        }
        else {
            Square *choices = new Square[7];
            for (int j=0; j<7; j++) {
                choices[j] = fromSq[j];
                if (j == i) {
                    choices[j] = (Square)-1;
                }
            }

            // we have chosen which semi final piece to move :)
            // iterate through all possible available moves for this particular piece
            // then for this piece chose the move that gives best eval
            // finally choose the semi final piece having best eval
            Square *available = AvailablePosn(pos);

            Value bestEvalForPiece = 0;
            Move FollowUp;
            Move bestMoveForPiece;
            for (int j=0; j<32; j++) {

                // make move
                Move forward(fromSq[i],available[j]);
                // Move backward(available[j],fromSq[i]);

                StateInfo *newSt = new StateInfo;
                // StateInfo *restore = new StateInfo;

                pos.move433(forward,*newSt);

                // now, call final move..
                std::pair<Move,Value> bestFinale = makeFinalMove(pos,states,choices,networks);
                // std::cout<<"reached here\n";
                if (bestFinale.second > bestEvalForPiece) {
                    bestEvalForPiece = bestFinale.second;
                    FollowUp = bestFinale.first;
                    bestMoveForPiece = forward;
                }

                // pos.move433(backward,*newSt);
                pos.undo433(forward);

                delete newSt;
                // delete restore;

            }
            if (bestEvalForPiece > bestVal) {
                // std::cout<<"best eval so far is "<<bestEvalForPiece<<"\n";
                bestVal = bestEvalForPiece;
                bestMove[0] = bestMoveForPiece;
                bestMove[1] = FollowUp;
            }
        }
    }
    std::pair<Move,Value> second = std::make_pair(bestMove[1],bestVal);
    return std::make_pair(bestMove[0],second);
}
std::pair <Move,std::pair<Move,std::pair<Move,Value>>> makeQuarterFinalMove (Stockfish::Position &pos, Stockfish::StateListPtr &states, Square fromSq[],const Eval::NNUE::Networks& networks) {
    Move bestMove[3];
    // std::cout<<"entered second move\n";
    Value bestVal = 0;
    for (int i=0; i<7; i++) {
        if (fromSq[i] == -1) {
            continue;
        }
        else {
            Square *choices = new Square[7];
            for (int j=0; j<7; j++) {
                choices[j] = fromSq[j];
                if (j == i) {
                    choices[j] = (Square)-1;
                }
            }

            // we have chosen which Quarter final piece to move :)
            // iterate through all possible available moves for this particular piece
            // then for this piece chose the move that gives best eval
            // finally choose the quarter final piece having best eval
            Square *available = AvailablePosn(pos);

            Value bestEvalForPiece = 0;
            Move semiFinal;
            Move Final;
            Move quarterFinal;

            for (int j=0; j<32; j++) {

                // make move
                Move forward(fromSq[i],available[j]);
                // Move backward(available[j],fromSq[i]);

                StateInfo *newSt = new StateInfo;
                // StateInfo *restore = new StateInfo;

                pos.move433(forward,*newSt);

                // now, call final move..
                std::pair<Move,std::pair<Move,Value>> bestSemiFinale = makeSemiFinalMove(pos,states,choices,networks);
                if (bestSemiFinale.second.second > bestEvalForPiece) {
                    bestEvalForPiece = bestSemiFinale.second.second;
                    semiFinal = bestSemiFinale.first;
                    Final = bestSemiFinale.second.first;
                    quarterFinal = forward;
                }

                // pos.move433(backward,*newSt);
                pos.undo433(forward);

                delete newSt;
                // delete restore;

            }
            if (bestEvalForPiece > bestVal) {
                // std::cout<<"best eval so far is "<<bestEvalForPiece<<"\n";
                bestVal = bestEvalForPiece;
                bestMove[0] = quarterFinal;
                bestMove[1] = semiFinal;
                bestMove[2] = Final;
            }
        }
    }
    std::pair<Move,Value> secondKaSecond = std::make_pair(bestMove[2],bestVal);
    std::pair<Move,std::pair<Move,Value>> second = std::make_pair(bestMove[1],secondKaSecond);
    return std::make_pair(bestMove[0],second);
}

std::pair<Move,std::pair <Move,std::pair<Move,std::pair<Move,Value>>>> makeFirstMove (Stockfish::Position &pos, Stockfish::StateListPtr &states, Square fromSq[],const Eval::NNUE::Networks& networks) {
    Move bestMove[4];
    // std::cout<<"entered first move\n";
    Value bestVal = 0;
    for (int i=0; i<7; i++) {
        if (fromSq[i] == -1) {
            continue;
        }
        else {
            Square *choices = new Square[7];
            for (int j=0; j<7; j++) {
                choices[j] = fromSq[j];
                if (j == i) {
                    choices[j] = (Square)-1;
                }
            }

            // we have chosen which first piece to move :)
            // iterate through all possible available moves for this particular piece
            // then for this piece chose the move that gives best eval
            // finally choose the first piece having best eval
            Square *available = AvailablePosn(pos);

            Value bestEvalForPiece = 0;
            Move first;
            Move semiFinal;
            Move Final;
            Move quarterFinal;

            for (int j=0; j<32; j++) {

                // make move
                Move forward(fromSq[i],available[j]);
                // Move backward(available[j],fromSq[i]);

                StateInfo *newSt = new StateInfo;
                // StateInfo *restore = new StateInfo;

                pos.move433(forward,*newSt);

                // now, call final move..
                std::pair<Move,std::pair<Move,std::pair<Move,Value>>> bestQuarterFinale = makeQuarterFinalMove(pos,states,choices,networks);
                if (bestQuarterFinale.second.second.second > bestEvalForPiece) {
                    bestEvalForPiece = bestQuarterFinale.second.second.second;
                    first = forward;
                    quarterFinal = bestQuarterFinale.first;
                    semiFinal = bestQuarterFinale.second.first;
                    Final = bestQuarterFinale.second.second.first;
                }

                // pos.move433(backward,*newSt);
                pos.undo433(forward);

                delete newSt;
                // delete restore;

            }
            if (bestEvalForPiece > bestVal) {
                bestVal = bestEvalForPiece;
                bestMove[0] = first;
                bestMove[1] = quarterFinal;
                bestMove[2] = semiFinal;
                bestMove[3] = Final;
            }
        }
    }
    std::pair<Move,Value> secondKaSecond = std::make_pair(bestMove[3],bestVal);
    std::pair<Move,std::pair<Move,Value>> second = std::make_pair(bestMove[2],secondKaSecond);
    std::pair<Move,std::pair<Move,std::pair<Move,Value>>> nivesh = std::make_pair(bestMove[1],second);
    return std::make_pair(bestMove[0],nivesh);
}

//write code here for CS433 project
void UCI::cs433_project(Stockfish::Position &pos, Stockfish::StateListPtr &states){

    /*
    I am making the following assumptions:
    1]We are only allowed to relocate white pieces (not pawns)
    2]We are only allowed to move pieces (not pawns) to unoccupied squares (i.e. we can't capture pieces or pawns of black)
    3]We are not allowed to move a piece twice 
    */
    Square fromSq [7] = {SQ_A1,SQ_B1,SQ_C1,SQ_D1,SQ_F1,SQ_G1,SQ_H1};
//    trace_eval(pos);
    std::cout<<"current evaluation is "<<0.01 * getVal(pos,networks)<<"\n";
    std::pair<Move,std::pair<Move,std::pair<Move,std::pair<Move,Value>>>> finalSet = makeFirstMove(pos,states,fromSq,networks);
    StateInfo *t = new StateInfo[8];
    pos.move433(finalSet.first,t[0]);
    pos.move433(finalSet.second.first,t[1]);
    pos.move433(finalSet.second.second.first,t[2]);
    pos.move433(finalSet.second.second.second.first,t[3]);
    std::cout<<"Now evaluation is "<<0.01*getVal(pos,networks)<<"\n";
    trace_eval(pos);
    //compute relevant board configuration where 4 pieces are relocated, by performing a state space search over the staring board configuration

    //call the neural network evaluation function and get the score for white

    //print out to sync_cout stream the FEN enconding of best board configuration with the score
    
}

Search::LimitsType UCI::parse_limits(const Position& pos, std::istream& is) {
    Search::LimitsType limits;
    std::string        token;

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
            limits.ponderMode = true;

    return limits;
}

void UCI::go(Position& pos, std::istringstream& is, StateListPtr& states) {

    Search::LimitsType limits = parse_limits(pos, is);

    networks.big.verify(options["EvalFile"]);
    networks.small.verify(options["EvalFileSmall"]);

    if (limits.perft)
    {
        perft(pos.fen(), limits.perft, options["UCI_Chess960"]);
        return;
    }

    threads.start_thinking(options, pos, states, limits);
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

    networks.big.verify(options["EvalFile"]);
    networks.small.verify(options["EvalFileSmall"]);


    sync_cout << "\n" << Eval::trace(p, networks) << sync_endl;
}

void UCI::search_clear() {
    threads.main_thread()->wait_for_search_finished();

    tt.clear(options["Threads"]);
    threads.clear();
    Tablebases::init(options["SyzygyPath"]);  // Free mapped files
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
        states->emplace_back();
        pos.do_move(m, states->back());
    }
}

namespace {
std::pair<double, double> win_rate_params(const Position& pos) {

    int material = pos.count<PAWN>() + 3 * pos.count<KNIGHT>() + 3 * pos.count<BISHOP>()
                 + 5 * pos.count<ROOK>() + 9 * pos.count<QUEEN>();

    // The fitted model only uses data for material counts in [10, 78], and is anchored at count 58.
    double m = std::clamp(material, 10, 78) / 58.0;

    // Return a = p_a(material) and b = p_b(material), see github.com/official-stockfish/WDL_model
    constexpr double as[] = {-185.71965483, 504.85014385, -438.58295743, 474.04604627};
    constexpr double bs[] = {89.23542728, -137.02141296, 73.28669021, 47.53376190};

    double a = (((as[0] * m + as[1]) * m + as[2]) * m) + as[3];
    double b = (((bs[0] * m + bs[1]) * m + bs[2]) * m) + bs[3];

    return {a, b};
}

// The win rate model is 1 / (1 + exp((a - eval) / b)), where a = p_a(material) and b = p_b(material).
// It fits the LTC fishtest statistics rather accurately.
int win_rate_model(Value v, const Position& pos) {

    auto [a, b] = win_rate_params(pos);

    // Return the win rate in per mille units, rounded to the nearest integer.
    return int(0.5 + 1000 / (1 + std::exp((a - double(v)) / b)));
}
}

std::string UCI::to_score(Value v, const Position& pos) {
    assert(-VALUE_INFINITE < v && v < VALUE_INFINITE);

    std::stringstream ss;

    if (std::abs(v) < VALUE_TB_WIN_IN_MAX_PLY)
        ss << "cp " << to_cp(v, pos);
    else if (std::abs(v) <= VALUE_TB)
    {
        const int ply = VALUE_TB - std::abs(v);  // recompute ss->ply
        ss << "cp " << (v > 0 ? 20000 - ply : -20000 + ply);
    }
    else
        ss << "mate " << (v > 0 ? VALUE_MATE - v + 1 : -VALUE_MATE - v) / 2;

    return ss.str();
}

// Turns a Value to an integer centipawn number,
// without treatment of mate and similar special scores.
int UCI::to_cp(Value v, const Position& pos) {

    // In general, the score can be defined via the the WDL as
    // (log(1/L - 1) - log(1/W - 1)) / ((log(1/L - 1) + log(1/W - 1))
    // Based on our win_rate_model, this simply yields v / a.

    auto [a, b] = win_rate_params(pos);

    return std::round(100 * int(v) / a);
}

std::string UCI::wdl(Value v, const Position& pos) {
    std::stringstream ss;

    int wdl_w = win_rate_model(v, pos);
    int wdl_l = win_rate_model(-v, pos);
    int wdl_d = 1000 - wdl_w - wdl_l;
    ss << " wdl " << wdl_w << " " << wdl_d << " " << wdl_l;

    return ss.str();
}

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


Move UCI::to_move(const Position& pos, std::string& str) {
    if (str.length() == 5)
        str[4] = char(tolower(str[4]));  // The promotion piece character must be lowercased

    for (const auto& m : MoveList<LEGAL>(pos))
        if (str == move(m, pos.is_chess960()))
            return m;

    return Move::none();
}

}  // namespace Stockfish
