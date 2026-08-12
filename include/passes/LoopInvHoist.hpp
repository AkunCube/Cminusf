#ifndef PASSES_LOOPINVHOIST_HPP
#define PASSES_LOOPINVHOIST_HPP

#include "Instruction.hpp"
#include "LoopSearch.hpp"
#include "Module.hpp"
#include "PassManager.hpp"
#include "Value.hpp"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"

/// Hoists loop-invariant instructions out of loops (loop-invariant code
/// motion, LICM).
class LoopInvHoist : public Pass {
public:
  LoopInvHoist(Module *m) : Pass(m) {}

  void run() override;

private:
  using LoopTree =
      llvm::DenseMap<pass::BBset_t *, llvm::DenseSet<pass::BBset_t *>>;

  void hoist_invariants(pass::BBset_t *loop, LoopTree &loop_tree,
                        LoopInfo &loop_searcher, pass::BBset_t &vis);
  bool is_loop_invariant(Value *value, pass::BBset_t *loop);
  bool is_movable(Instruction *instr);

  llvm::DenseMap<Value *, bool> info_;
};

#endif
