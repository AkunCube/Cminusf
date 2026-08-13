#ifndef PASSES_LOOPINVHOIST_HPP
#define PASSES_LOOPINVHOIST_HPP

#include "PassManager.hpp"

/// Hoists loop-invariant instructions out of loops (loop-invariant code
/// motion, LICM).
class LoopInvHoist : public Pass {
public:
  LoopInvHoist(Module *m) : Pass(m) {}

  void run() override;
};

#endif
