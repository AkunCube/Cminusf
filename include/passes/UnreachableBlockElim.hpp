#ifndef PASSES_UNREACHABLEBLOCKELIM_HPP
#define PASSES_UNREACHABLEBLOCKELIM_HPP

#include "PassManager.hpp"

/// Eliminates basic blocks unreachable from the function entry.
/// Keeps only blocks reachable via CFG edges from the entry block.
class UnreachableBlockElim : public Pass {
  using Pass::Pass;

public:
  void run() override;
};

#endif
