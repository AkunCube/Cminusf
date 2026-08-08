
#include <queue>

#include "BasicBlock.hpp"
#include "Function.hpp"
#include "Instruction.hpp"
#include "UnreachableBlockElim.hpp"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

// Remove the incoming (value, predecessor) pairs of phi instructions in
// `succ` that reference `dead_block`.
static void remove_dead_predecessor_from_phis(BasicBlock *succ,
                                              BasicBlock *dead_block) {
  for (auto &inst : succ->get_instructions()) {
    if (!inst.is_phi()) {
      continue;
    }
    auto *phi = static_cast<PhiInst *>(&inst);
    for (unsigned i = 0; i < phi->get_num_operand(); i += 2) {
      if (phi->get_operand(i + 1) != dead_block) {
        continue;
      }
      phi->remove_operand(i + 1); // predecessor operand
      phi->remove_operand(i);     // corresponding value operand
      break;
    }
  }
}

void UnreachableBlockElim::run() {
  for (Function &func : m_->get_functions()) {
    if (func.is_declaration()) {
      continue;
    }

    llvm::DenseSet<BasicBlock *> reachable_blocks;
    std::queue<BasicBlock *> worklist;
    BasicBlock *entry_block = func.get_entry_block();
    worklist.push(entry_block);
    reachable_blocks.insert(entry_block);

    while (!worklist.empty()) {
      BasicBlock *block = worklist.front();
      worklist.pop();
      Instruction *terminator = block->get_terminator();
      if (auto *branch = dynamic_cast<BranchInst *>(terminator)) {
        llvm::SmallVector<BasicBlock *> successor_blocks;
        if (branch->is_cond_br()) {
          successor_blocks.push_back(
              static_cast<BasicBlock *>(branch->get_operand(1)));
          successor_blocks.push_back(
              static_cast<BasicBlock *>(branch->get_operand(2)));
        } else {
          successor_blocks.push_back(
              static_cast<BasicBlock *>(branch->get_operand(0)));
        }
        for (BasicBlock *succ : successor_blocks) {
          if (reachable_blocks.insert(succ).second) {
            worklist.push(succ);
          }
        }
      }
    }

    for (BasicBlock &block :
         llvm::make_early_inc_range(func.get_basic_blocks())) {
      if (!reachable_blocks.contains(&block)) {
        // Drop the phi incoming pairs referencing the dead block from each
        // successor.
        for (BasicBlock *succ : block.get_succ_basic_blocks()) {
          remove_dead_predecessor_from_phis(succ, &block);
        }
        func.remove(&block);
      }
    }
  }
}
