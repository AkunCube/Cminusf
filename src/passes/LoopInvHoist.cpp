#include <cassert>
#include <optional>
#include <queue>

#include "Instruction.hpp"
#include "LoopInfo.hpp"
#include "LoopInvHoist.hpp"
#include "common.hpp"
#include "logging.hpp"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallVector.h"

using pass::BBset_t;
using LoopTree = llvm::DenseMap<BBset_t *, llvm::DenseSet<BBset_t *>>;
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
                              const BBset_t &processed_subloop_blocks,
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
    // Do not move users ahead of definitions left inside a nested loop.
    if (processed_subloop_blocks.contains(inst->get_parent())) {
      return false;
    }
    if (!is_movable(inst)) {
      return false;
    }

    // An instruction is invariant iff all its operands are invariant.
    return all_of(inst->get_operands(), [&](Value *operand) {
      return is_loop_invariant(operand, loop, processed_subloop_blocks,
                               invariant_cache);
    });
  }();

  invariant_cache[value] = invariant;
  return invariant;
}

/// Collects loop-invariant instructions from the loop body.
static SetVector<Instruction *>
collect_loop_invariant_instructions(BBset_t *loop,
                                    const BBset_t &processed_subloop_blocks) {
  assert(loop && "Loop block set cannot be null");

  DenseMap<Value *, bool> invariant_cache;
  SetVector<Instruction *> invariant_instructions;

  for (BasicBlock *block : *loop) {
    // Nested loops have already been handled by the bottom-up traversal.
    if (processed_subloop_blocks.contains(block)) {
      continue;
    }

    for (Instruction &inst : block->get_instructions()) {
      if (is_loop_invariant(&inst, loop, processed_subloop_blocks,
                            invariant_cache)) {
        invariant_instructions.insert(&inst);
      }
    }
  }

  return invariant_instructions;
}

/// Orders instructions so each in-set operand precedes its users.
/// Returns an error if the induced dependency graph contains a cycle.
static std::optional<SmallVector<Instruction *>>
compute_topological_order(SetVector<Instruction *> &instructions) {
  DenseMap<Instruction *, unsigned> remaining_deps;
  for (Instruction *inst : instructions) {
    remaining_deps.try_emplace(inst, 0);
  }

  // Count only dependencies whose definitions are also in the input set.
  for (Instruction *inst : instructions) {
    for (Value *operand : inst->get_operands()) {
      auto *operand_inst = dynamic_cast<Instruction *>(operand);
      if (!operand_inst || !instructions.contains(operand_inst)) {
        continue;
      }
      ++remaining_deps[inst];
    }
  }

  std::queue<Instruction *> worklist;
  for (Instruction *inst : instructions) {
    if (remaining_deps[inst] == 0) {
      worklist.push(inst);
    }
  }

  SmallVector<Instruction *> sorted_order;
  while (!worklist.empty()) {
    Instruction *current = worklist.front();
    worklist.pop();
    sorted_order.push_back(current);

    // Removing a node releases one dependency from each in-set user.
    for (const Use &use : current->get_use_list()) {
      auto *user = dynamic_cast<Instruction *>(use.val_);
      if (!user || !remaining_deps.contains(user)) {
        continue;
      }
      if (--remaining_deps[user] == 0) {
        worklist.push(user);
      }
    }
  }

  if (sorted_order.size() == instructions.size()) {
    return sorted_order;
  }

  return std::nullopt;
}

/// Hoists loop-invariant instructions from the loop body to the preheader.
/// Requires a single entry edge (preheader) to safely move instructions.
static void hoist_from_current_loop(BBset_t *loop, BasicBlock *header,
                                    const BBset_t &processed_subloop_blocks) {
  assert(loop && "Loop block set cannot be null");
  assert(header && "Loop header cannot be null");

  // Find the unique predecessor outside the loop (the loop's entry edge).
  SmallVector<BasicBlock *> outside_preds;
  for (BasicBlock *pred : header->get_pre_basic_blocks()) {
    if (!loop->contains(pred)) {
      outside_preds.push_back(pred);
    }
  }

  // Require a unique outside predecessor whose only successor is the header.
  if (outside_preds.size() != 1 ||
      outside_preds[0]->get_succ_basic_blocks().size() != 1) {
    return;
  }

  // Collect and topologically sort loop-invariant instructions.
  SetVector<Instruction *> inv_instrs =
      collect_loop_invariant_instructions(loop, processed_subloop_blocks);

  std::optional<SmallVector<Instruction *>> maybe_sorted =
      compute_topological_order(inv_instrs);

  if (!maybe_sorted || maybe_sorted->empty()) {
    return;
  }

  BasicBlock *preheader = outside_preds[0];
  Instruction *terminator = preheader->get_terminator();
  preheader->remove_instr(terminator);

  // Move instructions in dependency order before the terminator.
  for (Instruction *instr : maybe_sorted.value()) {
    BasicBlock *source = instr->get_parent();
    source->remove_instr(instr);
    instr->set_parent(preheader);
    preheader->add_instruction(instr);
  }

  preheader->add_instruction(terminator);
}

/// Optimize from leaf nodes on the loop tree up to the root nodes.
static void hoist_invariants(BBset_t *loop, LoopTree &loop_tree,
                             LoopInfo &loop_info,
                             BBset_t &processed_subloop_blocks) {
  if (!loop) {
    return;
  }

  for (auto subloop : loop_tree[loop]) {
    hoist_invariants(subloop, loop_tree, loop_info, processed_subloop_blocks);
  }

  BasicBlock *base = loop_info.get_base(loop);
  if (base) {
    hoist_from_current_loop(loop, base, processed_subloop_blocks);
  }

  // Parent loops should not rescan blocks handled by this traversal.
  for (auto bb : *loop) {
    processed_subloop_blocks.insert(bb);
  }
}

void LoopInvHoist::run() {
  LoopInfo loop_info(m_, false);
  loop_info.run();

  LOG(INFO) << "====== Loop invariant motion started ======";

  LoopTree loop_tree;
  SmallVector<BBset_t *> root_loops;
  for (BBset_t *loop : loop_info) {
    BBset_t *parent = loop_info.get_parent(loop);
    if (parent) {
      loop_tree[parent].insert(loop);
    } else {
      root_loops.push_back(loop);
    }
  }

  for (BBset_t *root : root_loops) {
    BBset_t processed_subloop_blocks;
    hoist_invariants(root, loop_tree, loop_info, processed_subloop_blocks);
  }

  LOG(INFO) << "====== Loop invariant motion ended ======";
}
