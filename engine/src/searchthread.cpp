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
 * @file: searchthread.cpp
 * Created on 23.05.2019
 * @author: queensgambit
 */

#include "searchthread.h"
#ifdef TENSORRT
#include "NvInfer.h"
#include <cuda_runtime_api.h>
#include "common.h"
#endif

#include <stdlib.h>
#include <climits>
#include "util/blazeutil.h"
#include "evalinfo.h"


size_t SearchThread::get_max_depth() const
{
    return depthMax;
}

SearchThread::SearchThread(NeuralNetAPI *netBatch, const SearchSettings* searchSettings, MapWithMutex* mapWithMutex):
    NeuralNetAPIUser(netBatch),
    rootNode(nullptr), rootState(nullptr), newState(nullptr),  // will be be set via setter methods
    newNodes(make_unique<FixedVector<Node*>>(searchSettings->batchSize)),
    newNodeSideToMove(make_unique<FixedVector<SideToMove>>(searchSettings->batchSize)),
    transpositionValues(make_unique<FixedVector<float>>(searchSettings->batchSize*2)),
    isRunning(true), mapWithMutex(mapWithMutex), searchSettings(searchSettings),
    tbHits(0), depthSum(0), depthMax(0), visitsPreSearch(0),
    terminalNodeCache(searchSettings->batchSize*2),
    reachedTablebases(false),
    currentMinimaxSearchNode(nullptr)
{
    switch (searchSettings->searchPlayerMode) {
    case MODE_SINGLE_PLAYER:
        terminalNodeCache = 1;  // TODO: Check if this is really needed
    case MODE_TWO_PLAYER: ;
    }
    searchLimits = nullptr;  // will be set by set_search_limits() every time before go()
    trajectoryBuffer.reserve(DEPTH_INIT);
    actionsBuffer.reserve(DEPTH_INIT);
}

void SearchThread::set_root_node(Node *value)
{   
    rootNode = value;
    visitsPreSearch = rootNode->get_visits();
}

void SearchThread::set_search_limits(SearchLimits *s)
{
    searchLimits = s;
}

bool SearchThread::is_running() const
{
    return isRunning;
}

void SearchThread::set_is_running(bool value)
{
    isRunning = value;
}

void SearchThread::set_reached_tablebases(bool value)
{
    reachedTablebases = value;
}

Node* SearchThread::add_new_node_to_tree(StateObj* newState, Node* parentNode, ChildIdx childIdx, NodeBackup& nodeBackup)
{
    bool transposition;
    Node* newNode = parentNode->add_new_node_to_tree(mapWithMutex, newState, childIdx, searchSettings, transposition);
    if (newNode->is_terminal()) {
        nodeBackup = NODE_TERMINAL;
        return newNode;
    }
    if (transposition) {
        //const float qValue =  parentNode->get_child_node(childIdx)->get_value();
        float qValue; 
        qValue = parentNode->get_child_node(childIdx)->get_value();
        transpositionValues->add_element(qValue);
        nodeBackup = NODE_TRANSPOSITION;
        return newNode;
    }
    nodeBackup = NODE_NEW_NODE;
    return newNode;
}

void SearchThread::stop()
{
    isRunning = false;
}

Node *SearchThread::get_root_node() const
{
    return rootNode;
}

SearchLimits *SearchThread::get_search_limits() const
{
    return searchLimits;
}

void random_playout(Node* currentNode, ChildIdx& childIdx)
{
    if (currentNode->is_fully_expanded()) {
        const size_t idx = rand() % currentNode->get_number_child_nodes();
        if (currentNode->get_child_node(idx) == nullptr || !currentNode->get_child_node(idx)->is_playout_node()) {
            childIdx = idx;
            return;
        }
        if (currentNode->get_child_node(idx)->get_node_type() == UNSOLVED) {
            childIdx = idx;
            return;
        }
        childIdx = uint16_t(-1);
    }
    else {
        childIdx = min(size_t(currentNode->get_no_visit_idx()), currentNode->get_number_child_nodes()-1);
        currentNode->increment_no_visit_idx();
        return;
    }
}

Node* SearchThread::get_starting_node(Node* currentNode, NodeDescription& description, ChildIdx& childIdx)
{
    size_t depth = get_random_depth();
    for (uint curDepth = 0; curDepth < depth; ++curDepth) {
        currentNode->lock();
        childIdx = get_best_action_index(currentNode, true, searchSettings);
        Node* nextNode = currentNode->get_child_node(childIdx);
        if (nextNode == nullptr || !nextNode->is_playout_node() || nextNode->get_visits() < searchSettings->epsilonGreedyCounter || nextNode->get_node_type() != UNSOLVED) {
            currentNode->unlock();
            break;
        }
        currentNode->unlock();
        actionsBuffer.emplace_back(currentNode->get_action(childIdx));
        currentNode = nextNode;
        ++description.depth;
    }
    return currentNode;
}

Node* SearchThread::get_new_child_to_evaluate(NodeDescription& description)
{
    description.depth = 0;
    Node* currentNode = rootNode;
    Node* nextNode;

    ChildIdx childIdx = uint16_t(-1);
    if (searchSettings->epsilonGreedyCounter && rootNode->is_playout_node() && rand() % searchSettings->epsilonGreedyCounter == 0) {
        currentNode = get_starting_node(currentNode, description, childIdx);
        currentNode->lock();
        random_playout(currentNode, childIdx);
        currentNode->unlock();
    }
    else if (searchSettings->epsilonChecksCounter && rootNode->is_playout_node() && rand() % searchSettings->epsilonChecksCounter == 0) {
        currentNode = get_starting_node(currentNode, description, childIdx);
        currentNode->lock();
        childIdx = select_enhanced_move(currentNode);
        if (childIdx == uint16_t(-1)) {
            random_playout(currentNode, childIdx);
        }
        currentNode->unlock();
    }
    deque<Action> pTempLine;
    while (true) {
        currentNode->lock();
        if (childIdx == uint16_t(-1)) {
            uint32_t numberVisits = 0;
            if (currentNode->is_playout_node()) {
                numberVisits = currentNode->get_visits();
            }
            if (searchSettings->mctsIpM) {
                if (numberVisits >= searchSettings->switchingAtVisits && pTempLine.empty() && !currentNode->isMinimaxCalled()) {
                    unique_ptr<StateObj> evalState = unique_ptr<StateObj>(rootState->clone());
                    assert(actionsBuffer.size() == description.depth - 1);
                    for (Action action : actionsBuffer) {
                        evalState->do_action(action);
                    }
                    float minimaxValue = -2.0;
                    childIdx = minimax_select_child_node(evalState.get(), currentNode, searchSettings->minimaxDepth, pTempLine, minimaxValue);
                    currentNode->setIsMinimaxCalled(true);
                    nextNode = currentNode->get_child_node(childIdx);
                    if (minimaxValue > -2.0 && nextNode != nullptr) {
                        nextNode->update_qValue_after_minimax_search(currentNode, childIdx, minimaxValue, searchSettings);
                    }
                    //pLine.pop_front();
                    //currentMinimaxSearchNode = currentNode->get_child_node(childIdx);
                }
                else {
                    if (!pTempLine.empty()) {
                        childIdx = currentNode->select_child_node(searchSettings, pTempLine[0]);
                        pTempLine.pop_front();
                        //currentMinimaxSearchNode = currentNode->get_child_node(childIdx);
                    }
                    else {
                        childIdx = currentNode->select_child_node(searchSettings);
                    }
                }
            }
            else {
                childIdx = currentNode->select_child_node(searchSettings);
            }
            
        }
        currentNode->apply_virtual_loss_to_child(childIdx, searchSettings);
        trajectoryBuffer.emplace_back(NodeAndIdx(currentNode, childIdx));
        nextNode = currentNode->get_child_node(childIdx);
        description.depth++;             
        if (nextNode == nullptr) {
#ifdef MCTS_STORE_STATES
            StateObj* newState = currentNode->get_state()->clone();
#else
            newState = unique_ptr<StateObj>(rootState->clone());
            assert(actionsBuffer.size() == description.depth - 1);
            for (Action action : actionsBuffer) {
                newState->do_action(action);
            }
#endif      
            newState->do_action(currentNode->get_action(childIdx));
            
            currentNode->increment_no_visit_idx();
#ifdef MCTS_STORE_STATES
            nextNode = add_new_node_to_tree(newState, currentNode, childIdx, description.type);
#else
            nextNode = add_new_node_to_tree(newState.get(), currentNode, childIdx, description.type);
#endif
            currentNode->unlock();

            if (description.type == NODE_NEW_NODE) {
#ifdef SEARCH_UCT
                Node* nextNode = currentNode->get_child_node(childIdx);
                nextNode->set_value(newState->random_rollout());
                nextNode->enable_has_nn_results();
                if (searchSettings->useTranspositionTable && !nextNode->is_terminal()) {
                    mapWithMutex->mtx.lock();
                    mapWithMutex->hashTable.insert({ nextNode->hash_key(), nextNode });
                    mapWithMutex->mtx.unlock();
                }
#else           
                if (searchSettings->mctsIc) {
                    unique_ptr<StateObj> evalState = unique_ptr<StateObj>(newState->clone());
                    LINE line;
                    line.cmove = 0;
                    ChildIdx childIdx = 0;
                    //pvs(evalState.get(), searchSettings->minimaxDepth, -INT_MAX, INT_MAX, searchSettings, childIdx, &line, 0, this);
                    pvs_nn(evalState.get(), searchSettings->minimaxDepth, -2.0, 2.0, searchSettings, childIdx, &line, 0, this);
                    for (int i = 0; i < line.cmove; ++i) {
                        evalState->do_action(line.argmove[i]);
                    }
                    evalState->get_state_planes(true, inputPlanes + newNodes->size() * net->get_nb_input_values_total(), net->get_version());
                    newNodeSideToMove->add_element(evalState->side_to_move());
                }
                else {
                    // fill a new board in the input_planes vector
                    // we shift the index by nbNNInputValues each time
                    newState->get_state_planes(true, inputPlanes + newNodes->size() * net->get_nb_input_values_total(), net->get_version());
                    // save a reference newly created list in the temporary list for node creation
                    // it will later be updated with the evaluation of the NN
                    newNodeSideToMove->add_element(newState->side_to_move());
                }
                
#endif
            }
            return nextNode;
        }
        if (nextNode->is_terminal()) {
            description.type = NODE_TERMINAL;
            currentNode->unlock();
            return nextNode;
        }
        if (!nextNode->has_nn_results()) {
            description.type = NODE_COLLISION;
            currentNode->unlock();
            return nextNode;
        }
        if (nextNode->is_transposition()) {
            nextNode->lock();
            const uint_fast32_t transposVisits = currentNode->get_real_visits(childIdx, searchSettings);
            const double transposQValue = -currentNode->get_q_sum(childIdx, searchSettings->virtualLoss) / transposVisits;
            if (nextNode->is_transposition_return(transposQValue)) {
                const float qValue = get_transposition_q_value(transposVisits, transposQValue, nextNode->get_value());
                nextNode->unlock();
                description.type = NODE_TRANSPOSITION;
                transpositionValues->add_element(qValue);
                currentNode->unlock();
                return nextNode;
            }
            nextNode->unlock();
        }
        currentNode->unlock();
#ifndef MCTS_STORE_STATES
        actionsBuffer.emplace_back(currentNode->get_action(childIdx));
#endif
        currentNode = nextNode;
        childIdx = uint16_t(-1);
    }
}

ChildIdx SearchThread::minimax_select_child_node(StateObj* state, Node* node, uint8_t depth, deque<Action> pTempLine, float& minimaxValue) {
    if (!node->is_sorted()) {
        node->prepare_node_for_visits();
    }
    if (node->has_forced_win()) {
        return node->get_checkmate_idx();
    }
    assert(sum(node->get_child_number_visits()) == node->get_visits());
    node->fully_expand_node();
    ChildIdx childIdx = 0;
    LINE line;
    line.cmove = 0;

    if (searchSettings->evaluationType == EVAL_NN) {
        // save first input plane values for later
        vector<float> firstInputPlanes(net->get_nb_input_values_total());
        std::copy(inputPlanes, inputPlanes+net->get_nb_input_values_total(), firstInputPlanes.begin());
        //pvs(state, depth, -INT_MAX, INT_MAX, searchSettings, childIdx, &line, 0, this);
        pvs_nn(state, depth, -2.0, 2.0, searchSettings, childIdx, &line, 0, this);
        // revert change
        std::copy(firstInputPlanes.begin(), firstInputPlanes.begin()+net->get_nb_input_values_total(), inputPlanes);
    }
    else {
        pvs_sf(state, depth, -INT_MAX, INT_MAX, searchSettings, childIdx, &line, 0, this);
    }
    Node* currNode = node->get_child_node(childIdx);
    if (currNode == nullptr) {
        return childIdx;
    }
    /*for (int i = 1; i < line.cmove; i++) {
        pTempLine.emplace_back(line.argmove[i]);
    }*/
    for (int i = 1; i < line.cmove; i++) {
        currNode->lock();
        ChildIdx idx = currNode->get_action_index(line.argmove[i]);
        if (idx == -1) {
            currNode->unlock();
            return childIdx;
        }
        Node* newCurrNode = currNode->get_child_node(idx);
        currNode->unlock();
        currNode = newCurrNode;
        if (currNode == nullptr) {
            return childIdx;
        }
    }
    if (currNode != node) {
        if (line.cmove % 2 == 0) {
            minimaxValue = currNode->get_value();
        }
        else {
            minimaxValue = -currNode->get_value();
        }
        
    }
    return childIdx;
}

void SearchThread::set_root_state(StateObj* value)
{
    rootState = value;
}

size_t SearchThread::get_tb_hits() const
{
    return tbHits;
}

void SearchThread::reset_stats()
{
    tbHits = 0;
    depthMax = 0;
    depthSum = 0;
}

void fill_nn_results(size_t batchIdx, bool isPolicyMap, const float* valueOutputs, const float* probOutputs, const float* auxiliaryOutputs, Node *node, size_t& tbHits, bool mirrorPolicy, const SearchSettings* searchSettings, bool isRootNodeTB)
{
    node->set_probabilities_for_moves(get_policy_data_batch(batchIdx, probOutputs, isPolicyMap), mirrorPolicy);
    node_post_process_policy(node, searchSettings->nodePolicyTemperature, searchSettings); 
    node_assign_value(node, valueOutputs, tbHits, batchIdx, isRootNodeTB);
    // initiate vValue
    if (searchSettings->backupOperator == BACKUP_POWER_MEAN ||
        searchSettings->backupOperator == BACKUP_POWER_MEAN_MEAN ||
        searchSettings->backupOperator == BACKUP_POWER_MEAN_MAX) {
        node->init_vValue(searchSettings);
    }
#ifdef MCTS_STORE_STATES
    node->set_auxiliary_outputs(get_auxiliary_data_batch(batchIdx, auxiliaryOutputs));
#endif
    node->enable_has_nn_results();
}

void SearchThread::set_nn_results_to_child_nodes()
{
    size_t batchIdx = 0;
    for (auto node: *newNodes) {
        if (!node->is_terminal()) {
            fill_nn_results(batchIdx, net->is_policy_map(), valueOutputs, probOutputs, auxiliaryOutputs, node,
                            tbHits, rootState->mirror_policy(newNodeSideToMove->get_element(batchIdx)),
                            searchSettings, rootNode->is_tablebase());
        }
        ++batchIdx;
    }
}

void SearchThread::backup_value_outputs()
{
    backup_values(*newNodes, newTrajectories);
    newNodeSideToMove->reset_idx();
    backup_values(transpositionValues.get(), transpositionTrajectories);
}

void SearchThread::backup_collisions() {
    for (size_t idx = 0; idx < collisionTrajectories.size(); ++idx) {
        backup_collision(searchSettings, collisionTrajectories[idx]);
    }
    collisionTrajectories.clear();
}

bool SearchThread::nodes_limits_ok()
{
    return (searchLimits->nodes == 0 || (rootNode->get_node_count() < searchLimits->nodes)) &&
            (searchLimits->simulations == 0 || (rootNode->get_visits() < searchLimits->simulations)) &&
            (searchLimits->nodesLimit == 0 || (rootNode->get_node_count() < searchLimits->nodesLimit));
}

bool SearchThread::is_root_node_unsolved()
{
#ifdef MCTS_TB_SUPPORT
    return is_unsolved_or_tablebase(rootNode->get_node_type());
#else
    return rootNode->get_node_type() == UNSOLVED;
#endif
}

size_t SearchThread::get_avg_depth()
{
    return size_t(double(depthSum) / (rootNode->get_visits() - visitsPreSearch) + 0.5);
}

void SearchThread::create_mini_batch()
{
    // select nodes to add to the mini-batch
    NodeDescription description;
    size_t numTerminalNodes = 0;

    while (!newNodes->is_full() &&
           collisionTrajectories.size() != searchSettings->batchSize &&
           !transpositionValues->is_full() &&
           numTerminalNodes < terminalNodeCache) {

        trajectoryBuffer.clear();
        actionsBuffer.clear();
        Node* newNode = get_new_child_to_evaluate(description);
        depthSum += description.depth;
        depthMax = max(depthMax, description.depth);
        if(description.type == NODE_TERMINAL) {
            ++numTerminalNodes;
            backup_value<true>(newNode->get_value(), searchSettings, trajectoryBuffer, searchSettings->mctsSolver);
        }
        else if (description.type == NODE_COLLISION) {
            // store a pointer to the collision node in order to revert the virtual loss of the forward propagation
            collisionTrajectories.emplace_back(trajectoryBuffer);
        }
        else if (description.type == NODE_TRANSPOSITION) {
            transpositionTrajectories.emplace_back(trajectoryBuffer);
        }
        else {  // NODE_NEW_NODE
            newNodes->add_element(newNode);
            newTrajectories.emplace_back(trajectoryBuffer);
        }
    }
}

void SearchThread::thread_iteration()
{
    create_mini_batch();
#ifndef SEARCH_UCT
    if (newNodes->size() != 0) {
        if (true) {
            net->predict(inputPlanes, valueOutputs, probOutputs, auxiliaryOutputs);
        }
        set_nn_results_to_child_nodes();
    }
#endif
    backup_value_outputs();
    backup_collisions();
}

void run_search_thread(SearchThread *t)
{
    t->set_is_running(true);
    t->reset_stats();
    while(t->is_running() && t->nodes_limits_ok() && t->is_root_node_unsolved()) {
        t->thread_iteration();
    }
    t->set_is_running(false);
}

void SearchThread::backup_values(FixedVector<Node*>& nodes, vector<Trajectory>& trajectories) {
    for (size_t idx = 0; idx < nodes.size(); ++idx) {
        Node* node = nodes.get_element(idx);
        float value = node->get_value();
#ifdef MCTS_TB_SUPPORT
        const bool solveForTerminal = searchSettings->mctsSolver && node->is_tablebase();
        backup_value<false>(value, searchSettings, trajectories[idx], solveForTerminal);
#else   
        backup_value<false>(value, searchSettings, trajectories[idx], false);
#endif
    }
    nodes.reset_idx();
    trajectories.clear();
}

void SearchThread::backup_values(FixedVector<float>* values, vector<Trajectory>& trajectories) {
    for (size_t idx = 0; idx < values->size(); ++idx) {
        const float value = values->get_element(idx);
        backup_value<true>(value, searchSettings, trajectories[idx], false);
    }
    values->reset_idx();
    trajectories.clear();
}

ChildIdx SearchThread::select_enhanced_move(Node* currentNode) const {
    if (currentNode->is_playout_node() && !currentNode->was_inspected() && !currentNode->is_terminal()) {

        // iterate over the current state
        unique_ptr<StateObj> pos = unique_ptr<StateObj>(rootState->clone());
        for (Action action : actionsBuffer) {
            pos->do_action(action);
        }

        // make sure a check has been explored at least once
        for (size_t childIdx = currentNode->get_no_visit_idx(); childIdx < currentNode->get_number_child_nodes(); ++childIdx) {
            if (pos->gives_check(currentNode->get_action(childIdx))) {
                for (size_t idx = currentNode->get_no_visit_idx(); idx < childIdx+1; ++idx) {
                    currentNode->increment_no_visit_idx();
                }
                return childIdx;
            }
        }
        // a full loop has been done
        currentNode->set_as_inspected();
    }
    return uint16_t(-1);
}

void node_assign_value(Node *node, const float* valueOutputs, size_t& tbHits, size_t batchIdx, bool isRootNodeTB)
{
#ifdef MCTS_TB_SUPPORT
    if (node->is_tablebase()) {
        ++tbHits;
        // TODO: Improvement the value assignment for table bases
        if (node->get_value() != 0 && isRootNodeTB) {
            // use the average of the TB entry and NN eval for non-draws
            node->set_value((valueOutputs[batchIdx] + node->get_value()) * 0.5f);
        }
        return;
    }
#endif
    node->set_value(valueOutputs[batchIdx]);
}

void node_post_process_policy(Node *node, float temperature, const SearchSettings* searchSettings)
{
    node->enhance_moves(searchSettings);
    node->apply_temperature_to_prior_policy(temperature);
}

size_t get_random_depth()
{
    const int randInt = rand() % 100 + 1;
    return std::ceil(-std::log2(1 - randInt / 100.0) - 1);
}



int pvs(StateObj* state, uint8_t depth, int alpha, int beta, const SearchSettings* searchSettings, ChildIdx& idx, LINE* pLine, uint8_t pLineIdx, NeuralNetAPIUser* netUser)
{
    LINE line;
    line.cmove = 0;
    ChildIdx idxDummy;
    if (state->is_board_terminal()) {
        pLine->cmove = 0;
        float dummy;
        switch (state->is_terminal(0, dummy))
        {
        case TERMINAL_WIN:
            return VALUE_KNOWN_WIN;
        case TERMINAL_DRAW:
            return 0;
        case TERMINAL_LOSS:
            return -VALUE_KNOWN_WIN;
        default:
            return state->get_stockfish_value();
        }
    }
    if (depth == 0) {
        pLine->cmove = 0;
        if (searchSettings->evaluationType == EVAL_NN) {
            return value_to_centipawn(netUser->evaluate(state));
        }
        else {
            if (!state->is_board_ok()) {
                return -pvs(state, 1, -beta, -alpha, searchSettings, idxDummy, &line, pLineIdx + 1, netUser);
            }
            else {
                return state->get_stockfish_value();
            }
        }
    }
    ChildIdx childIdx = -1;
    for (Action action : state->legal_actions()) {
        childIdx += 1;
        string fen_after_do_action = state->fen();
        state->do_action(action);
        int value = -pvs(state, depth - 1, -beta, -alpha, searchSettings, idxDummy, &line, pLineIdx + 1, netUser);
        state->undo_action(action);
        if (abs(value) == VALUE_INFINITE || abs(value) == VALUE_NONE) {
            continue;
        }
        if (alpha < value) {
            alpha = value;
            idx = childIdx;
            pLine->argmove[0] = action;
            memcpy(pLine->argmove + 1, line.argmove, line.cmove * sizeof(Action));
            pLine->cmove = line.cmove + 1;
        }
        if (alpha >= beta)
            break;
    }
    return alpha;
}

float pvs_nn(StateObj* state, uint8_t depth, float alpha, float beta, const SearchSettings* searchSettings, ChildIdx& idx, LINE* pLine, uint8_t pLineIdx, NeuralNetAPIUser* netUser)
{
    LINE line;
    line.cmove = 0;
    uint8_t saveIndex = pLineIdx;
    if (state->is_board_terminal()) {
        float dummy;
        switch (state->is_terminal(0, dummy))
        {
        case TERMINAL_WIN:
            return WIN_VALUE;
        case TERMINAL_DRAW:
            return DRAW_VALUE;
        case TERMINAL_LOSS:
            return LOSS_VALUE;
        default:
            break;
        }
    }
    if (depth == 0) {
        pLine->cmove = 0;
        return netUser->evaluate(state);    
    }
    ChildIdx childIdx = -1;
    ChildIdx idxDummy;
    for (Action action : state->legal_actions()) {
        childIdx += 1;
        string fen_after_do_action = state->fen();
        state->do_action(action);
        float value = -pvs_nn(state, depth - 1, -beta, -alpha, searchSettings, idxDummy, &line, pLineIdx + 1, netUser);
        state->undo_action(action);
        if (alpha < value) {
            alpha = value;
            idx = childIdx;
            pLine->argmove[0] = action;
            memcpy(pLine->argmove + 1, line.argmove, line.cmove * sizeof(Action));
            pLine->cmove = line.cmove + 1;
        }
        if (alpha >= beta)
            break;
    }
    return alpha;
}

int pvs_sf(StateObj* state, uint8_t depth, int alpha, int beta, const SearchSettings* searchSettings, ChildIdx& idx, LINE* pLine, uint8_t pLineIdx, NeuralNetAPIUser* netUser)
{
    LINE line;
    line.cmove = 0;
    ChildIdx idxDummy;
    if (state->is_board_terminal()) {
        pLine->cmove = 0;
        float dummy;
        switch (state->is_terminal(0, dummy))
        {
        case TERMINAL_WIN:
            return VALUE_KNOWN_WIN;
        case TERMINAL_DRAW:
            return 0;
        case TERMINAL_LOSS:
            return -VALUE_KNOWN_WIN;
        default:
            return state->get_stockfish_value();
        }
    }
    if (depth == 0) {
        pLine->cmove = 0;
        if (!state->is_board_ok()) {
            return -pvs(state, 1, -beta, -alpha, searchSettings, idxDummy, &line, pLineIdx + 1, netUser);
        }
        else {
            return state->get_stockfish_value();
        }

    }
    ChildIdx childIdx = -1;
    for (Action action : state->legal_actions()) {
        childIdx += 1;
        string fen_after_do_action = state->fen();
        state->do_action(action);
        int value = -pvs_sf(state, depth - 1, -beta, -alpha, searchSettings, idxDummy, &line, pLineIdx + 1, netUser);
        state->undo_action(action);
        if (abs(value) == VALUE_INFINITE || abs(value) == VALUE_NONE) {
            continue;
        }
        if (alpha < value) {
            alpha = value;
            idx = childIdx;
            pLine->argmove[0] = action;
            memcpy(pLine->argmove + 1, line.argmove, line.cmove * sizeof(Action));
            pLine->cmove = line.cmove + 1;
        }
        if (alpha >= beta)
            break;
    }
    return alpha;
}