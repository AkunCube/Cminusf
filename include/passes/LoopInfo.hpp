#ifndef PASSES_LOOPINFO_HPP
#define PASSES_LOOPINFO_HPP

#include <string>

#include "Function.hpp"
#include "PassManager.hpp"
#include "common.hpp"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"

/// Finds all loops in every function of the module using Tarjan's strongly
/// connected components, and records each block's innermost loop entry.
class LoopInfo : public Analysis {
public:
  explicit LoopInfo(Module *m, bool dump = false)
      : Analysis(m), en_dump_graph(dump) {}
  ~LoopInfo() override = default;
  bool isCFGOnly() const override { return true; }

  void run() override;

  auto begin() { return loop_set.begin(); }
  auto end() { return loop_set.end(); }

  BasicBlock *get_base(pass::BBset_t *loop) { return loop2base[loop]; }

  /// Get the innermost loop which contains `bb`.
  pass::BBset_t *get_innermost(BasicBlock *bb) {
    if (bb2base.find(bb) == bb2base.end()) {
      return nullptr;
    }
    return base2loop[bb2base[bb]];
  }

  /// Get the parent loop of `loop`.
  pass::BBset_t *get_parent(pass::BBset_t *loop);

  /// Get all loops in a function.
  llvm::DenseSet<pass::BBset_t *> get_loops(Function *f);

private:
  pass::CFGNodePtrSet build_cfg(Function *func);
  llvm::DenseSet<pass::CFGNodePtrSet *> find_scc(pass::CFGNodePtrSet &nodes);
  void dump_graph(pass::CFGNodePtrSet &nodes, std::string title);

  bool en_dump_graph;
  // Loops found.
  llvm::DenseSet<pass::BBset_t *> loop_set;
  // Loops found in a function.
  llvm::DenseMap<Function *, llvm::DenseSet<pass::BBset_t *>> func2loop;
  // { entry bb of loop : loop }
  llvm::DenseMap<BasicBlock *, pass::BBset_t *> base2loop;
  // { loop : entry bb of loop }
  llvm::DenseMap<pass::BBset_t *, BasicBlock *> loop2base;
  // { bb : entry bb of loop } Defaults to the lowest-level loop.
  llvm::DenseMap<BasicBlock *, BasicBlock *> bb2base;
};

#endif
