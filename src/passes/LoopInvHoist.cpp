#include "LoopInvHoist.hpp"
#include "LoopSearch.hpp"
#include "logging.hpp"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"

using pass::BBset_t;
using namespace llvm;

/// A instruction can be moved <= no side effects (memory stores included)
/// PHIs are excluded because we don't want to modify them.
static bool is_movable(Instruction *instr) {
  return instr->isBinary() || instr->is_si2fp() || instr->is_fp2si() ||
         instr->is_zext() || instr->is_cmp() || instr->is_fcmp() ||
         instr->is_gep();
}

/// Returns false if instr involves any value that is assigned inside loop.
static bool is_loop_invariant(Value *value, BBset_t *loop,
                              DenseMap<Value *, bool> &invariant_cache) {
  if (auto it = invariant_cache.find(value); it != invariant_cache.end()) {
    return it->second;
  }

  bool invariant = [&]() -> bool {
    Instruction *inst = dynamic_cast<Instruction *>(value);
    if (!inst) {
      return true;
    }

    // If the instruction is not inside the loop, it's trivially invariant.
    if (!loop->contains(inst->get_parent())) {
      return true;
    }
    if (!is_movable(inst)) {
      return false;
    }

    // An instruction is invariant iff all its operands are invariant.
    return all_of(inst->get_operands(), [&](Value *operand) {
      return is_loop_invariant(operand, loop, invariant_cache);
    });
  }();

  invariant_cache[value] = invariant;
  return invariant;
}

void LoopInvHoist::run() {
  LoopInfo loop_searcher(m_, false);
  loop_searcher.run();

  LOG(INFO) << "====== Loop invariant motion started ======";

  LoopTree loop_tree;
  for (BBset_t *loop : loop_searcher) {
    BBset_t *parent = loop_searcher.get_parent(loop);
    if (parent) {
      loop_tree[parent].insert(loop);
    }
  }

  LOG(INFO) << "====== Loop invariant motion ended ======";
}

// Optimize from leaf nodes on the loop tree up to the root nodes.
void LoopInvHoist::hoist_invariants(BBset_t *loop, LoopTree &loop_tree,
                                    LoopInfo &loop_searcher, BBset_t &vis) {
  for (auto subloop : loop_tree[loop]) {
    hoist_invariants(subloop, loop_tree, loop_searcher, vis);
  }

  if (!loop) {
    return;
  }

  auto base = loop_searcher.get_base(loop);
  std::vector<Instruction *> loop_invs;
  // TODO: find loop invariants, insert them into loop_invs

  if (!loop_invs.empty()) {
    // Insert to the block just before the base block.
    BasicBlock *dest = nullptr;
    for (auto prec : base->get_pre_basic_blocks()) {
      if (!loop->count(prec)) {
        dest = prec;
        break;
      }
    }
    if (dest) {
      // TODO: insert loop_invs to dest
    } else {
      LOG(ERROR) << "This loop doesn't have an entry block?!";
    }
  }

  // Mark this loop body as analyzed
  for (auto bb : *loop) {
    vis.insert(bb);
  }
}
