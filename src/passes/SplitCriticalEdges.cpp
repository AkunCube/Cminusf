#include <cassert>
#include <utility>

#include "BasicBlock.hpp"
#include "Function.hpp"
#include "IRBuilder.hpp"
#include "Instruction.hpp"
#include "passes.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"

using namespace llvm;

static constexpr StringLiteral kCriticalEdgeBlockPrefix = "critical_edge_";

namespace {

/// Splits critical CFG edges by inserting an intermediate basic block.
class SplitCriticalEdges : public Pass {
  using Pass::Pass;
  using Edge =
      std::pair<BasicBlock *, BasicBlock *>; // (predecessor, successor)

public:
  void run(PassManager &pm) override;

private:
  /// Collect critical edges before mutating predecessor/successor lists.
  SmallVector<Edge> collectCriticalEdges(Function *func) const;

  /// Insert a new block on pred -> succ and return the inserted block.
  BasicBlock *splitEdge(BasicBlock *pred, BasicBlock *succ,
                        unsigned split_block_id);

  /// Rewrite phi incoming blocks in succ from old_pred to new_pred.
  void rewritePhiPredecessors(BasicBlock *succ, BasicBlock *old_pred,
                              BasicBlock *new_pred);
};

void SplitCriticalEdges::run(PassManager &pm) {
  (void)pm;
  for (Function &func : m_->get_functions()) {
    if (func.is_declaration()) {
      continue;
    }
    SmallVector<Edge> critical_edges = collectCriticalEdges(&func);
    if (critical_edges.empty()) {
      continue;
    }

    for (const auto &[idx, edge] : llvm::enumerate(critical_edges)) {
      auto [pred, succ] = edge;
      BasicBlock *middle = splitEdge(pred, succ, idx);
      rewritePhiPredecessors(succ, pred, middle);
    }
  }
}

SmallVector<SplitCriticalEdges::Edge>
SplitCriticalEdges::collectCriticalEdges(Function *func) const {
  SmallVector<Edge> critical_edges;
  for (BasicBlock &succ : func->get_basic_blocks()) {
    DenseSet<BasicBlock *> unique_preds;
    unique_preds.insert(succ.get_pre_basic_blocks().begin(),
                        succ.get_pre_basic_blocks().end());
    if (unique_preds.size() < 2) {
      continue;
    }

    DenseSet<BasicBlock *> visited_preds;
    for (BasicBlock *pred : succ.get_pre_basic_blocks()) {
      if (!visited_preds.insert(pred).second) {
        continue;
      }
      DenseSet<BasicBlock *> unique_succs;
      unique_succs.insert(pred->get_succ_basic_blocks().begin(),
                          pred->get_succ_basic_blocks().end());
      if (unique_succs.size() > 1) {
        critical_edges.push_back({pred, &succ});
      }
    }
  }
  return critical_edges;
}

BasicBlock *SplitCriticalEdges::splitEdge(BasicBlock *pred, BasicBlock *succ,
                                          unsigned split_block_id) {
  assert(pred && succ && "cannot split an edge with a null endpoint");
  assert(pred->get_parent() == succ->get_parent() &&
         "cannot split an edge across functions");

  auto *terminator = dynamic_cast<BranchInst *>(pred->get_terminator());
  assert(terminator && terminator->is_cond_br() &&
         "a critical edge must leave a conditional branch");

  auto *middle = BasicBlock::create(
      m_, (Twine(kCriticalEdgeBlockPrefix) + Twine(split_block_id)).str(),
      pred->get_parent());

  if (terminator->get_operand(1) == succ) {
    terminator->set_operand(1, middle);
  } else {
    assert(terminator->get_operand(2) == succ &&
           "successor is not a target of predecessor's terminator");
    terminator->set_operand(2, middle);
  }

  // set_operand() only updates use lists, so keep the explicit CFG lists in
  // sync with the rewritten branch.
  pred->remove_succ_basic_block(succ);
  succ->remove_pre_basic_block(pred);
  pred->add_succ_basic_block(middle);
  middle->add_pre_basic_block(pred);

  IRBuilder builder(middle, m_);
  builder.create_br(succ);
  return middle;
}

void SplitCriticalEdges::rewritePhiPredecessors(BasicBlock *succ,
                                                BasicBlock *old_pred,
                                                BasicBlock *new_pred) {
  for (Instruction &inst : succ->get_instructions()) {
    if (!inst.is_phi()) {
      break;
    }
    assert(inst.get_num_operand() % 2 == 0 && "malformed phi instruction");
    for (unsigned i = 1; i < inst.get_num_operand(); i += 2) {
      if (inst.get_operand(i) == old_pred) {
        inst.set_operand(i, new_pred);
      }
    }
  }
}

} // namespace

std::unique_ptr<Pass> createSplitCriticalEdges(Module *m) {
  return std::make_unique<SplitCriticalEdges>(m);
}
