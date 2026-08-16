#ifndef CODEGEN_REGALLOC_HPP
#define CODEGEN_REGALLOC_HPP

#include <regex>
#include <string>

#include "Function.hpp"
#include "Liverange.hpp"
#include "Value.hpp"
#include "logging.hpp"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallSet.h"

namespace RA {

// 一共32个寄存器，只使用其中8个
#define MAXR 32
#define ARG_MAX_R 8

struct ActiveCmp {
  bool operator()(LRA::LiveInterval const &lhs,
                  LRA::LiveInterval const &rhs) const {
    std::regex pattern_arg("arg\\d+");
    bool is_lhs_arg = std::regex_match(lhs.second->get_name(), pattern_arg);
    bool is_rhs_arg = std::regex_match(rhs.second->get_name(), pattern_arg);
    if (is_lhs_arg && !is_rhs_arg)
      return true;
    if (!is_lhs_arg && is_rhs_arg)
      return false;
    if (lhs.first.end != rhs.first.end)
      return lhs.first.end < rhs.first.end;
    else if (lhs.first.start != rhs.first.start)
      return lhs.first.start < rhs.first.start;
    else
      return lhs.second < rhs.second;
  }
};

class RegAllocator {
private:
  Function *cur_func;
  const bool is_float;
  const uint num_regs;
  bool used[MAXR + 1]; // index range: 1 ~ num_regs
  llvm::DenseMap<Value *, int> reg_map;
  // 同 LiveIntervalSet：N=0 保证按 ActiveCmp 排序的迭代语义。
  llvm::SmallSet<LRA::LiveInterval, 0, ActiveCmp> active;
  // TODO:添加你需要的变量

  void reset(Function * = nullptr);
  void reserve_for_arg(const LRA::LiveIntervalSet &);
  void expire_old_intervals(LRA::LiveInterval);
  void spill_at_interval(LRA::LiveInterval);

public:
  RegAllocator(uint num_regs, bool is_float)
      : is_float(is_float), num_regs(num_regs), used{false} {
    LOG_DEBUG << "RegAllocator initialize: R=" << num_regs << "\n";
    assert(num_regs <= MAXR);
  }
  RegAllocator() = delete;

  // 判断当前v是否需要分配寄存器，需要则返回false
  static bool no_reg_alloca(Value *v);
  void linear_scan(const LRA::LiveIntervalSet &, Function *);
  const llvm::DenseMap<Value *, int> &get_reg_map() const { return reg_map; }
  void print(std::string (*reg_name)(int)) {
    for (auto [op, reg] : reg_map)
      LOG_DEBUG << op->get_name() << " ~ " << reg_name(reg) << "\n";
  }
};
} // namespace RA
#endif
// TODO: 这只是一个样例，你可以自行对框架进行修改以符合你自己的心意
// TODO: 对框架不满可尽情修改
