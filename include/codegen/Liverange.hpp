#ifndef CODEGEN_LIVERANGE_HPP
#define CODEGEN_LIVERANGE_HPP

#include <string>

#include "Function.hpp"
#include "Instruction.hpp"
#include "Module.hpp"
#include "Value.hpp"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"

#define UNINITIAL -1

namespace LRA {

/// A live range [start, end] on the function's position line.
struct Interval {
  Interval(int start = UNINITIAL, int end = UNINITIAL)
      : start(start), end(end) {}
  int start; ///< first live position
  int end;   ///< last live position

  /// SmallSet's linear search for small capacities requires operator==.
  bool operator==(const Interval &other) const {
    return start == other.start && end == other.end;
  }
};

using LiveSet = llvm::DenseSet<Value *>;
using PhiToCopy = std::pair<Value *, Value *>; ///< (dst, src)
using PhiMap = llvm::DenseMap<BasicBlock *, llvm::SmallVector<PhiToCopy>>;
using LiveInterval = std::pair<Interval, Value *>;

/// Order live values by the start of their live interval.
struct LiveIntervalCmp {
  bool operator()(LiveInterval const &lhs, LiveInterval const &rhs) const {
    bool is_lhs_arg = dynamic_cast<Argument *>(lhs.second);
    bool is_rhs_arg = dynamic_cast<Argument *>(rhs.second);
    if (is_lhs_arg && !is_rhs_arg)
      return true;
    if (!is_lhs_arg && is_rhs_arg)
      return false;
    if (lhs.first.start != rhs.first.start)
      return lhs.first.start < rhs.first.start;
    else
      return lhs.second < rhs.second;
  }
};

/// SmallSet stores small capacities in insertion order; N=0 forces the
/// std::set path so that iteration follows LiveIntervalCmp's ordering.
using LiveIntervalSet = llvm::SmallSet<LiveInterval, 0, LiveIntervalCmp>;

/// Backward liveness analysis and live-interval construction for one function.
class LiveRangeAnalyzer {
  friend class CodeGenRegister;

private:
  Module *module;
  llvm::DenseMap<Value *, Interval> interval_map;
  llvm::SmallVector<BasicBlock *> bb_dfs_order;
  llvm::DenseMap<int, LiveSet> in_set, out_set;
  /// Copy statements inserted at the end of each basic block.
  const PhiMap &phi_map;
  LiveIntervalSet live_intervals;
  llvm::DenseMap<Instruction *, int>
      instr_id;                           ///< id of each numbered instruction
  llvm::DenseMap<PhiToCopy, int> copy_id; ///< id of each copy statement

  void get_dfs_order(Function *);
  void make_id(Function *);
  void make_interval(Function *);

  LiveSet join_for(BasicBlock *bb);
  void union_live_sets(LiveSet &dest, LiveSet &src) {
    for (Value *v : src)
      dest.insert(v);
  }
  LiveSet transfer_function(Instruction *instr, const LiveSet &cur_out);
  /// Apply the parallel copy statements before bb's terminator and return the
  /// live set at the entry of the copy block.
  LiveSet process_copy_stmts(BasicBlock *bb, const LiveSet &live_out);

public:
  LiveRangeAnalyzer(Module *module, PhiMap &phi_map)
      : module(module), phi_map(phi_map) {}
  LiveRangeAnalyzer() = delete;

  void run(Function *);
  void clear();
  static std::string print_live_set(const LiveSet &ls) {
    std::string s = "[ ";
    for (auto k : ls)
      s += k->get_name() + " ";
    s += "]";
    return s;
  }
  static std::string print_interval(const Interval &interval) {
    return "<" + std::to_string(interval.start) + ", " +
           std::to_string(interval.end) + ">";
  }
  const LiveIntervalSet &get_live_intervals() { return live_intervals; }
  const decltype(interval_map) &get_interval_map() const {
    return interval_map;
  }
  const decltype(in_set) &get_in_set() const { return in_set; }
  const decltype(out_set) &get_out_set() const { return out_set; }
};
} // namespace LRA
#endif
