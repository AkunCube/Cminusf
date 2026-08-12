#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <vector>

#include "BasicBlock.hpp"
#include "Function.hpp"
#include "LoopSearch.hpp"
#include "common.hpp"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"

using namespace pass;

namespace pass {
struct CFGNode {
  CFGNodePtrSet succs;
  CFGNodePtrSet prevs;
  BasicBlock *bb;
  /// The index of the node in CFG.
  int index = -1;
  /// The min index of the node in the strongly connected componets.
  int lowlink = 0;
  bool onStack = false;

  explicit CFGNode(BasicBlock *bb) : bb(bb) {}
};
} // namespace pass

void LoopSearch::build_cfg(Function *func, CFGNodePtrSet &result) {
  // Clean up existing CFG.
  for (CFGNode *node : result) {
    delete node;
  }
  result.clear();

  // Map from BasicBlock to its corresponding CFGNode.
  llvm::DenseMap<BasicBlock *, CFGNodePtr> block_to_node_map;

  // Create CFGNode for each BasicBlock.
  for (BasicBlock &bb : func->get_basic_blocks()) {
    CFGNodePtr node_ptr = new CFGNode(&bb);
    result.insert(node_ptr);
    block_to_node_map[&bb] = node_ptr;
  }

  // Second pass: build predecessor and successor edges.
  for (BasicBlock &bb : func->get_basic_blocks()) {
    auto it = block_to_node_map.find(&bb);
    assert(it != block_to_node_map.end() && "BasicBlock not found in map");
    CFGNodePtr node_ptr = it->second;

    for (BasicBlock *pred : bb.get_pre_basic_blocks()) {
      auto pred_it = block_to_node_map.find(pred);
      assert(pred_it != block_to_node_map.end() &&
             "Predecessor not found in map");
      node_ptr->prevs.insert(pred_it->second);
    }

    for (BasicBlock *succ : bb.get_succ_basic_blocks()) {
      auto succ_it = block_to_node_map.find(succ);
      assert(succ_it != block_to_node_map.end() &&
             "Successor not found in map");
      node_ptr->succs.insert(succ_it->second);
    }
  }
}

// Tarjan algorithm
bool LoopSearch::find_scc(CFGNodePtrSet &nodes,
                          llvm::DenseSet<CFGNodePtrSet *> &result) {
  index_count = 0;
  stack.clear();
  for (auto n : nodes) {
    if (n->index == -1) {
      traverse(n, result);
    }
  }
  return result.size() != 0;
}

void LoopSearch::traverse(CFGNodePtr n,
                          llvm::DenseSet<CFGNodePtrSet *> &result) {
  n->index = index_count++;
  n->lowlink = n->index;
  stack.push_back(n);
  n->onStack = true;

  for (auto su : n->succs) {
    // has not visited su
    if (su->index == -1) {
      traverse(su, result);
      n->lowlink = std::min(su->lowlink, n->lowlink);
    }
    // has visited su
    else if (su->onStack) {
      n->lowlink = std::min(su->index, n->lowlink);
    }
  }

  if (n->index == n->lowlink) {
    // TODO: pop out the nodes in the same strongly connected component from
    // stack
  }
}

CFGNodePtr LoopSearch::find_base(CFGNodePtrSet *set, CFGNodePtrSet &reserved) {
  CFGNodePtr base = nullptr;
  // TODO: find the loop base node

  return base;
}

void LoopSearch::run() {
  for (Function &func : m_->get_functions()) {
    if (func.is_declaration()) {
      continue;
    }
    CFGNodePtrSet nodes;
    CFGNodePtrSet reserved;
    llvm::DenseSet<CFGNodePtrSet *> sccs;

    // step 1: build cfg
    build_cfg(&func, nodes);
    // dump graph
    dump_graph(nodes, func.get_name());
    // step 2: find strongly connected graph from external to internal
    // step 3: find loop base node for each strongly connected graph
    // step 4: store result
    // step 5: map each node to loop base
    // step 6: remove loop base node for researching inner loop
    // TODO
    reserved.clear();
    for (auto node : nodes) {
      delete node;
    }
    nodes.clear();
  }
}

void LoopSearch::dump_graph(CFGNodePtrSet &nodes, std::string title) {
  if (dump) {
    std::vector<std::string> edge_set;
    for (auto node : nodes) {
      if (node->bb->get_name() == "") {
        return;
      }
      if (base2loop.find(node->bb) != base2loop.end()) {
        for (auto succ : node->succs) {
          if (nodes.find(succ) != nodes.end()) {
            edge_set.insert(edge_set.begin(), '\t' + node->bb->get_name() +
                                                  "->" + succ->bb->get_name() +
                                                  ';' + '\n');
          }
        }
        edge_set.insert(edge_set.begin(), '\t' + node->bb->get_name() +
                                              " [color=red]" + ';' + '\n');
      } else {
        for (auto succ : node->succs) {
          if (nodes.find(succ) != nodes.end()) {
            edge_set.push_back('\t' + node->bb->get_name() + "->" +
                               succ->bb->get_name() + ';' + '\n');
          }
        }
      }
    }
    std::string digragh = "digraph G {\n";
    for (auto edge : edge_set) {
      digragh += edge;
    }
    digragh += '}';
    std::ofstream file_output;
    file_output.open(title + ".dot", std::ios::out);

    file_output << digragh;
    file_output.close();
    std::string dot_cmd =
        "dot -Tpng " + title + ".dot" + " -o " + title + ".png";
    std::system(dot_cmd.c_str());
  }
}

BBset_t *LoopSearch::get_parent(BBset_t *loop) {
  auto base = loop2base[loop];
  for (auto prev : base->get_pre_basic_blocks()) {
    if (loop->find(prev) != loop->end()) {
      continue;
    }
    auto loop = get_innermost(prev);
    if (loop == nullptr || loop->find(base) == loop->end()) {
      return nullptr;
    } else {
      return loop;
    }
  }
  return nullptr;
}

llvm::DenseSet<BBset_t *> LoopSearch::get_loops(Function *f) {
  return func2loop.count(f) ? func2loop[f] : llvm::DenseSet<BBset_t *>();
}
