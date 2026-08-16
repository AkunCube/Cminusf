#ifndef CODEGEN_LIVERANGE_HPP
#define CODEGEN_LIVERANGE_HPP

#include <regex>
#include <string>

#include "Function.hpp"
#include "Module.hpp"
#include "Value.hpp"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"

#define UNINITIAL -1

namespace LRA {

struct Interval {
  Interval(int start = UNINITIAL, int end = UNINITIAL)
      : start(start), end(end) {}
  int start; // 活跃区间起始点
  int end;   // 活跃区间结束点

  // SmallSet 的小容量线性查找需要元素支持 operator==
  bool operator==(const Interval &other) const {
    return start == other.start && end == other.end;
  }
};

using LiveSet = llvm::DenseSet<Value *>;
using PhiMap = llvm::DenseMap<BasicBlock *,
                              llvm::SmallVector<std::pair<Value *, Value *>>>;
using LiveInterval = std::pair<Interval, Value *>;

// 对活跃变量进行排序，此处采用按活跃区间的起始点进行排序
struct LiveIntervalCmp {
  bool operator()(LiveInterval const &lhs, LiveInterval const &rhs) const {
    std::regex pattern_arg("arg\\d+");
    bool is_lhs_arg = std::regex_match(lhs.second->get_name(), pattern_arg);
    bool is_rhs_arg = std::regex_match(rhs.second->get_name(), pattern_arg);
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

// SmallSet 在小容量时按插入序存储；N=0 强制走 std::set 路径，
// 从而保持按 LiveIntervalCmp 排序的迭代语义。
using LiveIntervalSet = llvm::SmallSet<LiveInterval, 0, LiveIntervalCmp>;

class LiveRangeAnalyzer {
  friend class CodeGenRegister;

private:
  Module *module;
  llvm::DenseMap<Value *, Interval> interval_map;
  llvm::SmallVector<BasicBlock *> bb_dfs_order;
  llvm::DenseMap<int, LiveSet> in_set, out_set;
  // phi_map: 标识在bb中的copy-statement
  const PhiMap &phi_map;
  LiveIntervalSet live_intervals;
  // TODO:添加你需要的变量

  void get_dfs_order(Function *);
  void make_id(Function *);
  void make_interval(Function *);

  LiveSet join_for(BasicBlock *bb);
  void union_live_sets(LiveSet &dest, LiveSet &src) {
    for (Value *v : src)
      dest.insert(v);
  }
  LiveSet transfer_function(Instruction *);

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
// TODO: 这只是一个样例，你可以自行对框架进行修改以符合你自己的心意
// TODO: 对框架不满可尽情修改
