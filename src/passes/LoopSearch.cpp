#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <vector>

#include "BasicBlock.hpp"
#include "Function.hpp"
#include "LoopSearch.hpp"
#include "common.hpp"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SetVector.h"

using namespace pass;

namespace pass {
struct CFGNode {
  CFGNodePtrSet succs;
  CFGNodePtrSet prevs;
  BasicBlock *bb;
  /// The index of the node in CFG.
  std::optional<int> index;
  /// The min index of the node in the strongly connected componets.
  std::optional<int> low_link;

  explicit CFGNode(BasicBlock *bb) : bb(bb) {}
};
} // namespace pass

CFGNodePtrSet LoopSearch::build_cfg(Function *func) {
  CFGNodePtrSet result;
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

  return result;
}

// Tarjan algorithm
llvm::DenseSet<CFGNodePtrSet *> LoopSearch::find_scc(CFGNodePtrSet &nodes) {
  llvm::DenseSet<CFGNodePtrSet *> result;
  for (CFGNodePtr node : nodes) {
    node->index.reset();
    node->low_link.reset();
  }
  int index_count = 0;
  // SetVector preserves DFS stack order and provides fast membership checks.
  llvm::SetVector<CFGNodePtr> stack;

  std::function<void(CFGNodePtr)> traverse = [&](CFGNodePtr node) {
    node->index = node->low_link = index_count;
    ++index_count;
    stack.insert(node);

    for (CFGNodePtr succ : node->succs) {
      if (!nodes.contains(succ)) {
        continue;
      }

      if (!succ->index.has_value()) {
        traverse(succ);
        node->low_link = std::min(*node->low_link, *succ->low_link);
      } else if (stack.contains(succ)) {
        // A back edge to an active node may lower this node's SCC root.
        node->low_link = std::min(*node->low_link, *succ->index);
      }
    }

    // A root node closes the SCC currently at the top of the DFS stack.
    if (*node->index == *node->low_link) {
      auto *group = new CFGNodePtrSet();
      while (!stack.empty()) {
        CFGNodePtr member = stack.pop_back_val();
        group->insert(member);
        if (member == node) {
          break;
        }
      }
      result.insert(group);
    }
  };

  for (CFGNodePtr node : nodes) {
    if (!node->index.has_value()) {
      traverse(node);
    }
  }

  return result;
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
    CFGNodePtrSet reserved;

    // step 1: build cfg
    CFGNodePtrSet nodes = build_cfg(&func);
    // dump graph
    dump_graph(nodes, func.get_name());
    // step 2: find strongly connected graph from external to internal
    llvm::DenseSet<CFGNodePtrSet *> sccs = find_scc(nodes);
    // step 3: find loop base node for each strongly connected graph
    // step 4: store result
    // step 5: map each node to loop base
    // step 6: remove loop base node for researching inner loop
    // TODO
    reserved.clear();
    for (CFGNodePtrSet *scc : sccs) {
      delete scc;
    }
    for (auto node : nodes) {
      delete node;
    }
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
