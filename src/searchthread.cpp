/*
  CrazyAra, a deep learning chess variant engine
  Copyright (C) 2018  Johannes Czech, Moritz Willig, Alena Beyer
  Copyright (C) 2019  Johannes Czech

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
#include "inputrepresentation.h"
#include "outputrepresentation.h"
#include "uci.h"
#include "misc.h"

SearchThread::SearchThread(NeuralNetAPI *netBatch, SearchSettings searchSettings, unordered_map<Key, Node *> *hashTable):
    netBatch(netBatch), searchSettings(searchSettings), isRunning(false), hashTable(hashTable)
{
    // allocate memory for all predictions and results
    inputPlanes = new float[searchSettings.batchSize * NB_VALUES_TOTAL];
    valueOutputs = new NDArray(Shape(searchSettings.batchSize, 1), Context::cpu());

    bool select_policy_from_plane = true;

    if (select_policy_from_plane) {
        probOutputs = new NDArray(Shape(searchSettings.batchSize, NB_LABELS_POLICY_MAP), Context::cpu());
    } else {
        probOutputs = new NDArray(Shape(searchSettings.batchSize, NB_LABELS), Context::cpu());
    }
    //    states = nullptr;
    //    states = StateListPtr(new std::deque<StateInfo>(1)); // Drop old and create a new one
}

void SearchThread::setRootNode(Node *value)
{
    rootNode = value;
}

void SearchThread::set_search_limits(SearchLimits *s)
{
    searchLimits = s;
}

bool SearchThread::getIsRunning() const
{
    return isRunning;
}

void SearchThread::setIsRunning(bool value)
{
    isRunning = value;
}

void SearchThread::stop()
{
    isRunning = false;
}

Node *SearchThread::getRootNode() const
{
    return rootNode;
}

SearchLimits *SearchThread::getSearchLimits() const
{
    return searchLimits;
}

inline Node* SearchThread::get_new_child_to_evaluate(unsigned int &childIdx, bool &isCollision, bool &isTerminal, size_t &depth)
{
    Node *currentNode = rootNode;
    Node *nextNode;

    depth = 0;
    while (true) {
        childIdx = currentNode->select_child_node();
        currentNode->apply_virtual_loss_to_child(childIdx);
        nextNode = currentNode->get_child_node(childIdx);
        depth++;
        if (nextNode == nullptr) {
            isCollision = false;
            isTerminal = false;
            return currentNode;
        }
        if (nextNode->isTerminal) {
            isCollision = false;
            isTerminal = true;
            return currentNode;
        }
        if (!nextNode->hasNNResults) {
            isCollision = true;
            isTerminal = false;
            return currentNode;
        }
        currentNode = nextNode;
    }

}

void SearchThread::set_NN_results_to_child_nodes()
{
    size_t batchIdx = 0;
    for (auto node: newNodes) {
        if (!node->isTerminal) {
            get_probs_of_move_list(batchIdx, probOutputs, node->legalMoves, node->pos->side_to_move(),
                                   !netBatch->getSelectPolicyFromPlane(), node->policyProbSmall, netBatch->getSelectPolicyFromPlane());
            node->mtx.lock();
            node->value = valueOutputs->At(batchIdx, 0);
            node->hasNNResults = true;
            node->enhance_checks();
            node->mtx.unlock();
        }
        //        node->parentNode->waitForNNResults[node->childIdxOfParent] = 0;
        //        node->parentNode->numberWaitingChildNodes--;
        ++batchIdx;
        hashTable->insert({node->pos->hash_key(), node});
    }
}

void SearchThread::backup_value_outputs()
{
    //    size_t batchIdx = 0;
    for (auto node: newNodes) {
        node->parentNode->backup_value(node->childIdxForParent, -node->value);
    }
    newNodes.clear();

    for (auto node: transpositionNodes) {
        node->parentNode->backup_value(node->childIdxForParent, -node->value);
    }
    transpositionNodes.clear();

    for (auto node: terminalNodes) {
        node->parentNode->backup_value(node->childIdxForParent, -node->value);
    }
    terminalNodes.clear();

}

void SearchThread::backup_collisions()
{
    for (auto node: collisionNodes) {
        node->parentNode->backup_collision(node->childIdxForParent);
    }
    collisionNodes.clear();
}

void SearchThread::create_new_node(Board* newPos, Node* parentNode, size_t childIdx, size_t numberNewNodes)
{
    Node *newNode = new Node(newPos, parentNode, childIdx, &searchSettings);

    // save a reference newly created list in the temporary list for node creation
    // it will later be updated with the evaluation of the NN
    newNodes.push_back(newNode);

    // connect the Node to the parent
    parentNode->mtx.lock();
    parentNode->childNodes[childIdx] = newNode;
    parentNode->mtx.unlock();

    // fill a new board in the input_planes vector
    // we shift the index by NB_VALUES_TOTAL each time
    board_to_planes(newPos, newPos->getStateInfo()->repetition, true, inputPlanes+numberNewNodes*NB_VALUES_TOTAL);
}

void SearchThread::copy_node(const unordered_map<Key,Node*>::const_iterator &it, Board* newPos, Node* parentNode, size_t childIdx)
{
    Node *newNode = new Node(*it->second);
    newNode->mtx.lock();
    //                 newNode->value = it->second->value;
    //                 newNode->policyProbSmall = it->second->policyProbSmall;
    newNode->pos = newPos;
    newNode->parentNode = parentNode;
    newNode->childIdxForParent = childIdx;
    newNode->hasNNResults = true;
    newNode->mtx.unlock();
    parentNode->mtx.lock();
    //                 sync_cout << "parentNode: childNodes" << parentNode->childNodes.size() << " " << childIdx << sync_endl;
    parentNode->childNodes[childIdx] = newNode;
    parentNode->mtx.unlock();
    //    assert(newNode->nbDirectChildNodes == it->second->nbDirectChildNodes);
    //                 sync_cout << parentNode->childNodes[childIdx]->value << sync_endl;
    //    parentNode->backup_value(childIdx, virtualLoss, -newNode->value);
    //    parentNode->backup_value(childIdx, virtualLoss, -parentNode->childNodes[childIdx]->value);
}

void SearchThread::create_mini_batch()
{
    // select nodes to add to the mini-batchel
    Node *parentNode;
    unsigned int childIdx;
    bool isCollision;
    bool isTerminal;

    size_t depth;
    size_t numberNewNodes = 0;
    size_t tranpositionEvents = 0;
    size_t terminalEvents = 0;

    while (newNodes.size() < searchSettings.batchSize and
           collisionNodes.size() < searchSettings.batchSize and
           tranpositionEvents < searchSettings.batchSize and
           terminalEvents < searchSettings.batchSize) {
        parentNode = get_new_child_to_evaluate(childIdx, isCollision, isTerminal, depth);

        if(isTerminal) {
            //            terminalNodes.push_back(parentNode->childNodes[childIdx]);
            parentNode->backup_value(childIdx, -parentNode->childNodes[childIdx]->value);
            ++terminalEvents;
        }
        else if (isCollision) {
            // store a pointer to the collision node in order to revert the virtual loss of the forward propagation
            collisionNodes.push_back(parentNode->childNodes[childIdx]);
        }
        else {
            StateInfo* newState = new StateInfo;
            Board* newPos= new Board(*parentNode->pos);
            newPos->do_move(parentNode->legalMoves[childIdx], *newState);

            auto it = hashTable->find(newPos->hash_key());
            if(false and it != hashTable->end() && it->second->hasNNResults &&
               it->second->pos->getStateInfo()->pliesFromNull == newState->pliesFromNull &&
               it->second->pos->getStateInfo()->rule50 == newState->rule50 &&
               newState->repetition == 0)
                {
                                    copy_node(it, newPos, parentNode, childIdx);
                    //                sync_cout << "transposition event!: " << newPos->fen() << sync_endl;

//                    if (newPos->fen() != it->second->pos->fen()) {
//                        sync_cout << "fen missmatch!" << sync_endl;
//                        sync_cout << newPos->fen() << " != " << it->second->pos->fen() << sync_endl;
//                        assert(newPos->fen() == it->second->pos->fen());
//                    }
//                    Node *newNode = new Node(newPos, parentNode, childIdx);
//                    newNode->mtx.lock();
//                    if (!newNode->isTerminal) {
//                        newNode->value = it->second->value;
//                        newNode->checkmateIdx = it->second->checkmateIdx;
//                    }
//                    newNode->hasNNResults = true;
//                    newNode->mtx.unlock();

//                    //                // connect the Node to the parent
//                    parentNode->mtx.lock();
//                    parentNode->childNodes[childIdx] = newNode;
//                    parentNode->mtx.unlock();
                    //                assert(it->second->nbDirectChildNodes == newNode->nbDirectChildNodes);
                    //                parentNode->backup_value(childIdx, virtualLoss, -newNode->value);
                    transpositionNodes.push_back(parentNode->childNodes[childIdx]);

                    ++tranpositionEvents;
            }
            else {
                create_new_node(newPos, parentNode, childIdx, numberNewNodes);
                ++numberNewNodes;
            }
        }
    }

}

void SearchThread::thread_iteration()
{
    create_mini_batch();
    if (newNodes.size() > 0) {
        netBatch->predict(inputPlanes, *valueOutputs, *probOutputs);
        set_NN_results_to_child_nodes();
    }
    backup_value_outputs();
    backup_collisions();
    rootNode->numberVisits = sum(rootNode->childNumberVisits);
}

void go(SearchThread *t)
{
    t->setIsRunning(true);
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

    //    sync_cout << "string info thread rootNode" << sync_endl;
    //    sync_cout << "string info >> fen " << t->getRootNode()->getPos().fen() << sync_endl;

    TimePoint elapsedTimeMS;

    do {
        t->thread_iteration();
        //        if (i % 100 == 0) {
        //            cout << "rootNode" << endl;
        //            cout << t->getRootNode() << endl;
        //        }
        TimePoint end = now();
        elapsedTimeMS = end - t->getSearchLimits()->startTime;
    } while(t->getIsRunning());
}
