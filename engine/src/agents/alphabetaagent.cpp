/*
  CrazyAra, a deep learning chess variant engine
  Copyright (C) 2018       Johannes Czech, Moritz Willig, Alena Beyer
  Copyright (C) 2019-2020  Johannes Czech

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

/*
 * @file: alphabetaagent.cpp
 * Created on 18.08.2023
 * @author: queensgambit
 */

#include "alphabetaagent.h"
#include "searchthread.h"

AlphaBetaAgent::AlphaBetaAgent(NeuralNetAPI* net, PlaySettings* playSettings, const SearchSettings* searchSettings, bool verbose):
    Agent(net, playSettings, verbose),
    searchSettings(searchSettings)
{

}

void AlphaBetaAgent::evaluate_board_state()
{
    evalInfo->legalMoves = state->legal_actions();
    evalInfo->init_vectors_for_multi_pv(1UL);

    ChildIdx childIdx = 0;
    deque<Action> pLine;
    int depth = searchSettings->minimaxDepth;
    if (searchLimits->depth != 0) {
        depth = searchLimits->depth;
    }
    pLine.resize(depth);
    unique_ptr<StateObj> evalState = unique_ptr<StateObj>(state->clone());
    LINE line;
    line.cmove = 0;
    evalState->set(state->fen(), state->is_chess960(), StateConstants::variant_to_int(Options["UCI_Variant"]));
    evalInfo->centipawns[0] = pvs(evalState.get(), depth, - INT_MAX, INT_MAX, searchSettings, childIdx, &line, 0);
    //evalInfo->centipawns[0] = pvs(state, searchSettings->minimaxDepth, - INT_MAX, INT_MAX, searchSettings, childIdx, pLine, 0);
    evalInfo->movesToMate[0] = 0;
    info_string("childIdx:", childIdx);
    evalInfo->depth = depth;
    evalInfo->selDepth = line.cmove;
    evalInfo->tbHits = 0;
    evalInfo->nodes = 1;
    evalInfo->isChess960 = state->is_chess960();
    evalInfo->pv[0].clear();
    info_string("SEE PV-LINE");
    for (int i = 0; i < pLine.size(); ++i) {
        info_string(StateConstants::action_to_uci(line.argmove[i], false));
        evalInfo->pv[0].push_back(line.argmove[i]);
    }
    unlock_and_notify();

}

void AlphaBetaAgent::stop()
{
    // pass
}

void AlphaBetaAgent::apply_move_to_tree(Action move, bool ownMove)
{
    // pass
}

