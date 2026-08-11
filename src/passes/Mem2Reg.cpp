#include <cassert>
#include <deque>
#include <memory>

#include "BasicBlock.hpp"
#include "Constant.hpp"
#include "Instruction.hpp"
#include "Mem2Reg.hpp"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

using namespace llvm;

// Return the current reaching definition of an alloca, which is the top of
// its definition stack, or an undef placeholder if no definition reaches the
// current point.
static Value *
get_reaching_def(const DenseMap<AllocaInst *, SmallVector<Value *>> &var_stack,
                 AllocaInst *alloca, Module *module) {
  auto it = var_stack.find(alloca);
  if (it == var_stack.end() || it->second.empty()) {
    return UndefValue::get(alloca->get_alloca_type(), module);
  }
  return it->second.back();
}

void Mem2Reg::run() {
  // Build the dominator tree used by the phi insertion stage.
  dominators_ = std::make_unique<Dominators>(m_);
  dominators_->run();

  for (auto &func : m_->get_functions()) {
    if (func.is_declaration()) {
      continue;
    }
    func_ = &func;
    reset_function_state();
    if (func_->get_basic_blocks().size() >= 1) {
      collect_promotable_allocas();
      generate_phi();
      rename(func_->get_entry_block());
      delete_promoted_memory_instructions();
    }
  }
}

void Mem2Reg::reset_function_state() {
  promotable_allocas_.clear();
  instructions_to_delete_.clear();
  phi_to_alloca_.clear();
  var_stack_.clear();
  block_phi_map_.clear();
}

void Mem2Reg::collect_promotable_allocas() {
  for (BasicBlock &block : func_->get_basic_blocks()) {
    for (Instruction &inst : block.get_instructions()) {
      auto *alloca = dynamic_cast<AllocaInst *>(&inst);
      if (!alloca) {
        continue;
      }

      bool has_invalid_use =
          llvm::any_of(alloca->get_use_list(), [alloca](const Use &use) {
            if (auto *load = dynamic_cast<LoadInst *>(use.val_)) {
              return load->get_lval() != alloca;
            }
            if (auto *store = dynamic_cast<StoreInst *>(use.val_)) {
              // Storing the alloca address itself makes the address escape.
              return store->get_lval() != alloca;
            }
            return true;
          });

      if (!has_invalid_use) {
        promotable_allocas_.insert(alloca);
      }
    }
  }
}

void Mem2Reg::delete_promoted_memory_instructions() {
  // First detach all operands so the alloca use lists are up to date.
  for (Instruction *inst : instructions_to_delete_) {
    inst->remove_all_operands();
  }
  for (Instruction *inst : instructions_to_delete_) {
    inst->get_parent()->erase_instr(inst);
  }

  for (AllocaInst *alloca : llvm::make_early_inc_range(promotable_allocas_)) {
    if (!alloca->get_use_list().empty()) {
      continue;
    }

    promotable_allocas_.erase(alloca);
    alloca->remove_all_operands();
    alloca->get_parent()->erase_instr(alloca);
  }
}

void Mem2Reg::generate_phi() {
  // Collect the defining blocks (stores) of each memory variable (alloca).
  DenseMap<AllocaInst *, DenseSet<BasicBlock *>> alloca_def_blocks;
  for (BasicBlock &block : func_->get_basic_blocks()) {
    for (Instruction &inst : block.get_instructions()) {
      auto *store = dynamic_cast<StoreInst *>(&inst);
      if (!store) {
        continue;
      }
      auto *alloca = dynamic_cast<AllocaInst *>(store->get_lval());
      if (!alloca || !promotable_allocas_.contains(alloca)) {
        continue;
      }
      alloca_def_blocks[alloca].insert(&block);
    }
  }

  // Place phis at the iterated dominance frontier of the defining blocks.
  for (auto &[alloca, def_blocks] : alloca_def_blocks) {
    std::deque<BasicBlock *> worklist(def_blocks.begin(), def_blocks.end());
    DenseSet<BasicBlock *> phi_blocks;
    while (!worklist.empty()) {
      BasicBlock *def_block = worklist.front();
      worklist.pop_front();
      for (BasicBlock *frontier :
           dominators_->get_dominance_frontier(def_block)) {
        if (!phi_blocks.insert(frontier).second) {
          continue;
        }

        // Insert an empty phi at the entry of the frontier block. create_phi()
        // only sets the parent, so add the instruction to the block explicitly.
        auto *phi = PhiInst::create_phi(alloca->get_alloca_type(), frontier);
        frontier->add_instr_begin(phi);
        block_phi_map_[frontier].insert({phi, alloca});
        phi_to_alloca_[phi] = alloca;

        // The inserted phi is also a definition of the variable, so keep
        // iterating its dominance frontier if the block is not already a def.
        if (!def_blocks.contains(frontier)) {
          worklist.push_back(frontier);
        }
      }
    }
  }
}

void Mem2Reg::rename(BasicBlock *block) {
  // Push the definitions (stores and phis) of this block onto the variable
  // stacks, replace loads with the current reaching definition, fill the
  // successor phis, then recurse along the dominator tree.
  DenseMap<AllocaInst *, unsigned> pushed_counts;
  for (Instruction &inst : block->get_instructions()) {
    if (auto *phi = dynamic_cast<PhiInst *>(&inst)) {
      auto it = phi_to_alloca_.find(phi);
      assert(it != phi_to_alloca_.end() && "phi not created by generate_phi");
      AllocaInst *alloca = it->second;
      var_stack_[alloca].push_back(phi);
      ++pushed_counts[alloca];
    } else if (auto *store = dynamic_cast<StoreInst *>(&inst)) {
      auto *alloca = dynamic_cast<AllocaInst *>(store->get_lval());
      if (!alloca || !promotable_allocas_.contains(alloca)) {
        continue;
      }
      instructions_to_delete_.push_back(store);
      var_stack_[alloca].push_back(store->get_rval());
      ++pushed_counts[alloca];
    } else if (auto *load = dynamic_cast<LoadInst *>(&inst)) {
      auto *alloca = dynamic_cast<AllocaInst *>(load->get_lval());
      if (!alloca || !promotable_allocas_.contains(alloca)) {
        continue;
      }
      // Replace the load with the current reaching definition of the variable.
      load->replace_all_use_with(
          get_reaching_def(var_stack_, alloca, func_->get_parent()));
      instructions_to_delete_.push_back(load);
    }
  }

  // Fill the incoming values of the phis in the CFG successors.
  for (BasicBlock *successor : block->get_succ_basic_blocks()) {
    for (auto &[phi, alloca] : block_phi_map_[successor]) {
      // Fill the incoming value with the current reaching definition of the
      // variable on this edge.
      phi->add_phi_pair_operand(
          get_reaching_def(var_stack_, alloca, func_->get_parent()), block);
    }
  }

  // Recursively rename the dominator tree children.
  for (BasicBlock *dom_tree_child :
       dominators_->get_dom_tree_succ_blocks(block)) {
    rename(dom_tree_child);
  }

  // Restore the stacks to the state before entering this block.
  for (auto &[alloca, count] : pushed_counts) {
    var_stack_[alloca].pop_back_n(count);
  }
}
