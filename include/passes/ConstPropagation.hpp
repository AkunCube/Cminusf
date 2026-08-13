#ifndef PASSES_CONSTPROPAGATION_HPP
#define PASSES_CONSTPROPAGATION_HPP

#include "Constant.hpp"
#include "Instruction.hpp"
#include "Module.hpp"
#include "PassManager.hpp"
#include "common/ConstFolder.hpp"
#include "llvm/ADT/DenseMap.h"

class ConstPropagation : public Pass {
public:
  using GlobalConstantMap = llvm::DenseMap<GlobalVariable *, Constant *>;
  ConstPropagation(Module *m) : Pass(m), folder(m) {}
  void run();

private:
  void run_on_function(Function &function);
  void run_on_basic_block(BasicBlock &block);
  void simplify_control_flow(Function &);
  Constant *try_propagate_global_constant(Instruction *, GlobalConstantMap &);

  ConstFolder folder;
};

#endif
