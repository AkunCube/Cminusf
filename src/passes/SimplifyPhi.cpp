#include <cassert>
#include <list>
#include <queue>

#include "BasicBlock.hpp"
#include "Function.hpp"
#include "Instruction.hpp"
#include "User.hpp"
#include "passes.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"

using namespace llvm;

namespace {

/// Removes phi instructions whose result is determined by a single value.
class SimplifyPhi : public Pass {
  using Pass::Pass;

public:
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    // Phi simplification rewrites values but does not add or remove CFG edges.
    AU.setPreservesCFG();
  }

  void run(PassManager &pm) override;

private:
  /// Repeatedly simplify trivial phis in one function until no more change.
  void simplifyFunction(Function *func);

  /// Return the value replacing phi, or nullptr when phi is not trivial.
  Value *findReplacement(PhiInst *phi) const;
};

void SimplifyPhi::run(PassManager &pm) {
  (void)pm;
  for (Function &func : m_->get_functions()) {
    if (func.is_declaration()) {
      continue;
    }
    simplifyFunction(&func);
  }
}

void SimplifyPhi::simplifyFunction(Function *func) {
  std::queue<PhiInst *> worklist;
  for (BasicBlock &block : func->get_basic_blocks()) {
    for (Instruction &inst : block.get_instructions()) {
      if (auto *phi = dynamic_cast<PhiInst *>(&inst)) {
        unsigned int num_operands = phi->get_num_operand();
        assert(num_operands > 0 && !(num_operands & 1));
        worklist.push(phi);
      } else {
        break;
      }
    }
  }

  auto addToWorklist = [&](const std::list<Use> &uses) {
    for (auto &use : uses) {
      if (auto *phi = dynamic_cast<PhiInst *>(use.val_)) {
        worklist.push(phi);
      }
    }
  };

  DenseSet<PhiInst *> phis_to_erase;
  while (!worklist.empty()) {
    PhiInst *cur_phi = worklist.front();
    worklist.pop();
    if (phis_to_erase.contains(cur_phi)) {
      continue;
    }
    Value *replacement = findReplacement(cur_phi);
    if (replacement && replacement != cur_phi) {
      addToWorklist(cur_phi->get_use_list());
      cur_phi->replace_all_use_with(replacement);
      phis_to_erase.insert(cur_phi);
    }
  }

  for (BasicBlock &block : func->get_basic_blocks()) {
    for (Instruction &inst :
         llvm::make_early_inc_range(block.get_instructions())) {
      auto *phi = dynamic_cast<PhiInst *>(&inst);
      if (!phi || !phis_to_erase.contains(phi)) {
        continue;
      }
      block.erase_instr(phi);
    }
  }
}

Value *SimplifyPhi::findReplacement(PhiInst *phi) const {
  Value *replacement = nullptr;
  for (unsigned i = 0; i < phi->get_num_operand(); i += 2) {
    Value *incoming = phi->get_operand(i);
    if (incoming == phi) {
      continue;
    }
    if (!replacement) {
      replacement = incoming;
    } else if (incoming != replacement) {
      return nullptr;
    }
  }
  // An all-self-referential phi has no concrete replacement value.
  return replacement;
}

} // namespace

std::unique_ptr<Pass> createSimplifyPhi(Module *m) {
  return std::make_unique<SimplifyPhi>(m);
}
