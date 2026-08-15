#include <deque>

#include "BasicBlock.hpp"
#include "Constant.hpp"
#include "Function.hpp"
#include "GlobalVariable.hpp"
#include "IRBuilder.hpp"
#include "Instruction.hpp"
#include "common/ConstFolder.hpp"
#include "passes.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"

using namespace llvm;

// Rewrite conditional branches with constant conditions into unconditional
// branches, returning the blocks that become unreachable as a result.
static DenseSet<BasicBlock *> simplify_constant_branches(Function &function) {
  DenseSet<BasicBlock *> dead_blocks;
  for (BasicBlock &block : function.get_basic_blocks()) {
    auto *terminator = dynamic_cast<BranchInst *>(block.get_terminator());
    if (!terminator || !terminator->is_cond_br()) {
      continue;
    }
    auto *const_cond = dynamic_cast<ConstantInt *>(terminator->get_operand(0));
    if (!const_cond) {
      continue;
    }

    BasicBlock *target_bb = nullptr;
    BasicBlock *dead_bb = nullptr;
    if (const_cond->get_value()) {
      target_bb = static_cast<BasicBlock *>(terminator->get_operand(1));
      dead_bb = static_cast<BasicBlock *>(terminator->get_operand(2));
    } else {
      target_bb = static_cast<BasicBlock *>(terminator->get_operand(2));
      dead_bb = static_cast<BasicBlock *>(terminator->get_operand(1));
    }

    // `remove_instr` only unlinks the terminator without running its
    // destructor, so clean up the CFG edges of the old conditional branch
    // explicitly while its operands are still intact.
    for (BasicBlock *succ : {target_bb, dead_bb}) {
      succ->remove_pre_basic_block(&block);
      block.remove_succ_basic_block(succ);
    }
    terminator->remove_all_operands();
    block.remove_instr(terminator);

    IRBuilder builder(&block, function.get_parent());
    builder.create_br(target_bb);
    dead_blocks.insert(dead_bb);
  }
  return dead_blocks;
}

namespace {

class ConstPropagation : public Pass {
public:
  using GlobalConstantMap = llvm::DenseMap<GlobalVariable *, Constant *>;
  ConstPropagation(Module *m) : Pass(m), folder(m) {}
  void run(PassManager &pm) override;

private:
  void run_on_function(Function &function);
  void run_on_basic_block(BasicBlock &block);
  void simplify_control_flow(Function &);
  Constant *try_propagate_global_constant(Instruction *, GlobalConstantMap &);

  ConstFolder folder;
};

void ConstPropagation::run(PassManager &pm) {
  for (Function &function : m_->get_functions()) {
    if (function.is_declaration()) {
      continue;
    }
    run_on_function(function);
  }
}

void ConstPropagation::run_on_function(Function &function) {
  for (BasicBlock &block : function.get_basic_blocks()) {
    run_on_basic_block(block);
  }
  simplify_control_flow(function);
}

void ConstPropagation::run_on_basic_block(BasicBlock &block) {
  GlobalConstantMap known_constants;
  for (Instruction &instr :
       llvm::make_early_inc_range(block.get_instructions())) {
    Constant *replacement =
        try_propagate_global_constant(&instr, known_constants);
    if (!replacement) {
      replacement = folder.try_fold(&instr);
    }
    if (!replacement) {
      continue;
    }
    instr.replace_all_use_with(replacement);
    block.remove_instr(&instr);
  }
}

void ConstPropagation::simplify_control_flow(Function &function) {
  DenseSet<BasicBlock *> dead_blocks = simplify_constant_branches(function);
  if (dead_blocks.empty()) {
    return;
  }

  std::deque<BasicBlock *> worklist(dead_blocks.begin(), dead_blocks.end());
  DenseSet<BasicBlock *> removed_blocks;
  while (!worklist.empty()) {
    BasicBlock *cur = worklist.front();
    worklist.pop_front();

    if (cur == function.get_entry_block() ||
        !cur->get_pre_basic_blocks().empty()) {
      continue;
    }

    if (!removed_blocks.insert(cur).second) {
      continue;
    }

    for (BasicBlock *succ : cur->get_succ_basic_blocks()) {
      for (Instruction &instr :
           llvm::make_early_inc_range(succ->get_instructions())) {
        if (!instr.is_phi()) {
          break;
        }

        // Drop the incoming value coming from the removed predecessor.
        for (unsigned i = 0; i < instr.get_num_operand(); i += 2) {
          if (instr.get_operand(i + 1) == cur) {
            instr.remove_operand(i);
            instr.remove_operand(i);
            break;
          }
        }

        // A phi with a single remaining incoming value can be folded away.
        if (instr.get_num_operand() == 2) {
          instr.replace_all_use_with(instr.get_operand(0));
          succ->remove_instr(&instr);
        }
      }
      worklist.push_back(succ);
    }

    function.remove(cur);
  }
}

Constant *ConstPropagation::try_propagate_global_constant(
    Instruction *instr, GlobalConstantMap &known_constants) {
  if (dynamic_cast<CallInst *>(instr)) {
    // A call may write to any global, so invalidate the known constants.
    known_constants.clear();
    return nullptr;
  }

  if (auto *load = dynamic_cast<LoadInst *>(instr)) {
    auto *global_var = dynamic_cast<GlobalVariable *>(load->get_lval());
    if (!global_var) {
      return nullptr;
    }
    if (auto it = known_constants.find(global_var);
        it != known_constants.end()) {
      return it->getSecond();
    }
    return nullptr;
  }

  if (auto *store = dynamic_cast<StoreInst *>(instr)) {
    auto *global_var = dynamic_cast<GlobalVariable *>(store->get_lval());
    if (!global_var) {
      return nullptr;
    }
    if (auto const_var = dynamic_cast<Constant *>(store->get_rval())) {
      known_constants[global_var] = const_var;
    } else {
      // A non-constant store overwrites the global; drop any cached value.
      known_constants.erase(global_var);
    }
  }
  return nullptr;
}

} // namespace

std::unique_ptr<Pass> createConstPropagation(Module *m) {
  return std::make_unique<ConstPropagation>(m);
}
