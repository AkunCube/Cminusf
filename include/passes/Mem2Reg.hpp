#pragma once

#include <memory>

#include "Dominators.hpp"
#include "Instruction.hpp"
#include "Value.hpp"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"

class Mem2Reg : public Pass {
private:
  using PhiAllocaPair = std::pair<PhiInst *, AllocaInst *>;
  Function *func_;
  std::unique_ptr<Dominators> dominators_;
  /// Allocations in the current function that can be safely promoted to SSA.
  llvm::DenseSet<AllocaInst *> promotable_allocas_;
  /// Promoted loads and stores to erase after renaming finishes.
  llvm::SmallVector<Instruction *> instructions_to_delete_;
  /// Maps each phi instruction to the alloca (memory variable) it belongs to.
  llvm::DenseMap<PhiInst *, AllocaInst *> phi_to_alloca_;
  /// Per-alloca stack holding the current reaching definitions.
  llvm::DenseMap<AllocaInst *, llvm::SmallVector<Value *>> var_stack_;
  /// Phis inserted at the entry of each block, grouped by their alloca.
  llvm::DenseMap<BasicBlock *, llvm::DenseSet<PhiAllocaPair>> block_phi_map_;

public:
  Mem2Reg(Module *m) : Pass(m) {}
  ~Mem2Reg() = default;

  void run() override;

private:
  void reset_function_state();
  void collect_promotable_allocas();
  void delete_promoted_memory_instructions();
  void generate_phi();
  void rename(BasicBlock *block);

  static inline bool is_global_variable(Value *l_val) {
    return dynamic_cast<GlobalVariable *>(l_val) != nullptr;
  }
  static inline bool is_gep_instr(Value *l_val) {
    return dynamic_cast<GetElementPtrInst *>(l_val) != nullptr;
  }

  static inline bool is_valid_ptr(Value *l_val) {
    return not is_global_variable(l_val) and not is_gep_instr(l_val);
  }
};
