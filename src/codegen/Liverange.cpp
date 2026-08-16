#include <functional>

#include "BasicBlock.hpp"
#include "Function.hpp"
#include "Instruction.hpp"
#include "Liverange.hpp"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SetOperations.h"

using namespace LRA;
using namespace llvm;

/// True for values that may occupy a register: instruction results and
/// function arguments.
static inline bool is_register_value(Value *v) {
  return dynamic_cast<Instruction *>(v) || dynamic_cast<Argument *>(v);
}

/// Reset all per-function state before analyzing the next function.
void LiveRangeAnalyzer::clear() {
  bb_dfs_order.clear();
  in_set.clear();
  out_set.clear();
  instr_id.clear();
  copy_id.clear();
  interval_map.clear();
  live_intervals.clear();
}

/// Union the live-in sets of bb's successors into its live-out set.
LiveSet LiveRangeAnalyzer::join_for(BasicBlock *bb) {
  LiveSet out;
  for (auto succ : bb->get_succ_basic_blocks()) {
    auto &irs = succ->get_instructions();
    auto it = irs.begin();
    while (it != irs.end() and it->is_phi())
      ++it;
    assert(it != irs.end() && "need to find first_ir from copy-stmt");

    Instruction &anchor = *it;
    auto id_it = instr_id.find(&anchor);
    assert(id_it != instr_id.end() && "anchor not numbered");

    // The block live-in is the in_set of the first numbered entity.
    // Usually this is the first non-phi instruction; if the block only
    // contains copy-stmts and a terminator, it is the first copy-stmt,
    // whose id is smaller than the terminator's.
    int entry_id = id_it->second;
    if (anchor.isTerminator()) {
      if (auto cit = phi_map.find(succ);
          cit != phi_map.end() && !cit->second.empty())
        entry_id = copy_id[cit->second.front()];
    }

    if (auto in_it = in_set.find(entry_id); in_it != in_set.end()) {
      union_live_sets(out, in_it->second);
    }
  }
  return out;
}

/// Number every non-phi instruction and copy statement in DFS order; id 0 is
/// reserved for function arguments.
void LiveRangeAnalyzer::make_id(Function *func) {
  int next_id = 1; // `0` for function args.
  for (auto bb : bb_dfs_order) {
    for (auto &instr : bb->get_instructions()) {
      if (instr.is_phi())
        continue; // phis are lowered into copy statements, so not numbered.

      // Copy statements must be numbered before the br/ret instruction.
      if (instr.isTerminator()) {
        if (auto it = phi_map.find(bb); it != phi_map.end()) {
          for (auto &copy : it->second) {
            copy_id[copy] = next_id++;
          }
        }
      }

      instr_id[&instr] = next_id++;
    }
  }
}

/// Collect the basic blocks in DFS pre-order from the entry block.
void LiveRangeAnalyzer::get_dfs_order(Function *func) {
  DenseSet<BasicBlock *> visited;
  std::function<void(BasicBlock *)> traverse = [&](BasicBlock *bb) {
    if (!visited.insert(bb).second) {
      return;
    }
    this->bb_dfs_order.push_back(bb);
    for (BasicBlock *succ : bb->get_succ_basic_blocks()) {
      traverse(succ);
    }
  };

  traverse(func->get_entry_block());
}

/// Backward transfer: in = use + (out - def). Only register-allocatable
/// operands (instruction results and arguments) count as uses.
LiveSet LiveRangeAnalyzer::transfer_function(Instruction *instr,
                                             const LiveSet &cur_out) {
  LiveSet in = cur_out;
  if (!instr->is_void()) {
    in.erase(instr);
  }

  for (Value *op : instr->get_operands()) {
    if (is_register_value(op)) {
      in.insert(op);
    }
  }
  return in;
}

/// Apply the parallel copy statements that sit right before bb's terminator
/// and return the live set at the entry of the copy block:
///   live_in = (live_out - D) u S
/// where D is the set of all destinations and S the set of all sources.
LiveSet LiveRangeAnalyzer::process_copy_stmts(BasicBlock *bb,
                                              const LiveSet &live_out) {
  auto it = phi_map.find(bb);
  if (it == phi_map.end()) {
    return live_out;
  }
  ArrayRef<PhiToCopy> copies = it->second;
  LiveSet live_in = live_out;
  for (const auto &[dst, _] : copies) {
    live_in.erase(dst);
  }
  for (const auto &[_, src] : copies) {
    // Constants are materialized at the copy site, so they are not tracked
    // by liveness.
    if (is_register_value(src)) {
      live_in.insert(src);
    }
  }

  // With parallel-copy semantics, every copy shares the same block live-in
  // and live-out.
  for (const auto &copy : copies) {
    int cid = copy_id[copy];
    out_set[cid] = live_out;
    in_set[cid] = live_in;
  }
  return live_in;
}

/// Compute per-instruction liveness and derive every value's live interval.
void LiveRangeAnalyzer::run(Function *func) {
  clear();
  get_dfs_order(func);
  make_id(func);

  // Liveness is a backward data-flow problem: iterate over the blocks in
  // reverse DFS order until no in-set changes anymore.
  bool changed = true;
  while (changed) {
    changed = false;

    for (auto rit_bb = bb_dfs_order.rbegin(); rit_bb != bb_dfs_order.rend();
         ++rit_bb) {
      BasicBlock *bb = *rit_bb;
      // True while the current reverse iteration is still at the terminator;
      // used to assert that a phi node is never the block terminator.
      bool last_ir = true;
      LiveSet cur_out;
      Instruction *terminator = bb->get_terminator();
      cur_out = join_for(bb);
      out_set[instr_id[terminator]] = cur_out;

      for (auto rit_ir = bb->get_instructions().rbegin();
           rit_ir != bb->get_instructions().rend(); ++rit_ir) {
        Instruction *instr = &(*rit_ir);
        if (instr->is_phi()) {
          // Phi nodes are lowered into copy statements, so they carry no
          // liveness of their own.
          assert(!last_ir && "If phi is the last ir, then data "
                             "flow fails due to ignorance of phi");
          continue;
        }

        // Backward transfer: out is the live set propagated from the next
        // instruction, in is produced by the per-instruction transfer.
        out_set[instr_id[instr]] = cur_out;
        LiveSet new_in = transfer_function(instr, cur_out);
        changed |= llvm::set_union(in_set[instr_id[instr]], new_in);
        cur_out = new_in;

        // Copy statements are executed right before the terminator, so the
        // backward traversal reaches them just after the terminator.
        if (instr->isTerminator()) {
          cur_out = process_copy_stmts(bb, cur_out);
        }
        last_ir = false;
      }
    }
  }

  // Function arguments are live at position 0, which is reserved for them.
  assert(in_set.find(0) == in_set.end() and out_set.find(0) == out_set.end() &&
         "no instr_id will be mapped to 0");
  in_set[0] = out_set[0] = {};
  for (auto &arg : func->get_args())
    in_set[0].insert(&arg);
  make_interval(func);
}

/// Derive each value's live interval from its live positions: the function
/// entry occupies position 0, and entity id k spans in(k) = 2k-1 and
/// out(k) = 2k.
void LiveRangeAnalyzer::make_interval(Function *) {
  auto in_pos = [](int id) { return 2 * id - 1; };
  auto out_pos = [](int id) { return 2 * id; };

  auto touch = [this](Value *v, int pos) {
    auto &iv = interval_map[v];
    if (iv.start == UNINITIAL || pos < iv.start)
      iv.start = pos;
    if (iv.end == UNINITIAL || pos > iv.end)
      iv.end = pos;
  };

  for (auto &[id, live_in] : in_set) {
    int pos = (id == 0) ? 0 : in_pos(id);
    for (Value *v : live_in) {
      touch(v, pos);
    }
  }

  for (auto &[id, live_out] : out_set) {
    int pos = (id == 0) ? 0 : out_pos(id);
    for (Value *v : live_out) {
      touch(v, pos);
    }
  }

  // Collect the intervals in a set ordered by start position.
  for (auto &[op, interval] : interval_map)
    live_intervals.insert({interval, op});
}
