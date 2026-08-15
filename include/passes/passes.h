#pragma once

#include "PassManager.hpp"

#include <memory>

/// Transform passes are created through the factory functions declared here
/// and implemented in the corresponding .cpp files, so the class definitions
/// stay hidden from the pipeline.
///
/// Analyses (Dominators, FuncInfo, LoopInfo) are deliberately not listed
/// here: request them inside a pass via `pm.getAnalysis<...>()`.
std::unique_ptr<Pass> createUnreachableBlockElim(Module *m);
std::unique_ptr<Pass> createMem2Reg(Module *m);
std::unique_ptr<Pass> createDeadCode(Module *m);
std::unique_ptr<Pass> createConstPropagation(Module *m);
std::unique_ptr<Pass> createGVN(Module *m, bool dump_json = false);
std::unique_ptr<Pass> createLoopInvHoist(Module *m);
