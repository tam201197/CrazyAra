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
 * @file: alphabetaagent.h
 * Created on 18.08.2023
 * @author: queensgambit
 *
 * The AlphaBeta agent runs a simple mini-max search with alpha beta pruning.
 */


#ifndef ALPHABETAAGENT_H
#define ALPHABETAAGENT_H

#include "agent.h"

using namespace crazyara;


class AlphaBetaAgent : public Agent
{
private:
    const SearchSettings* searchSettings;

public:
    AlphaBetaAgent(NeuralNetAPI* net, PlaySettings* playSettings, const SearchSettings* searchSettings, bool verbose);

    void evaluate_board_state() override;

    void stop() override;

    void apply_move_to_tree(Action move, bool ownMove) override;
};

#endif // ALPHABETAAGENT_H