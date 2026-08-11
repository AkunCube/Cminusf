#ifndef PASSES_COMMON_HPP
#define PASSES_COMMON_HPP

#include "BasicBlock.hpp"
#include "llvm/ADT/DenseSet.h"

/// Type aliases shared by the loop passes (LoopSearch, LoopInvHoist).
///
/// They live in their own namespace so that generic names such as `BBset_t`
/// do not pollute the global namespace. The namespace is spelled `pass`
/// (lowercase) because the global scope already contains the base class
/// `Pass`, which forbids declaring `namespace Pass` there.
namespace pass {

struct CFGNode;

using CFGNodePtr = CFGNode *;
using CFGNodePtrSet = llvm::DenseSet<CFGNode *>;
using BBset_t = llvm::DenseSet<BasicBlock *>;

} // namespace pass

#endif
