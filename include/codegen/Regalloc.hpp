#ifndef CODEGEN_REGALLOC_HPP
#define CODEGEN_REGALLOC_HPP

#include <set>
#include <string>

#include "Function.hpp"
#include "Liverange.hpp"
#include "Value.hpp"
#include "logging.hpp"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DenseMap.h"

namespace RA {

// 32 registers in total; the first 8 are reserved for function arguments.
constexpr uint MAXR = 32;
constexpr uint ARG_MAX_R = 8;

struct ActiveCmp {
  bool operator()(LRA::LiveInterval const &lhs,
                  LRA::LiveInterval const &rhs) const {
    bool is_lhs_arg = dynamic_cast<Argument *>(lhs.second);
    bool is_rhs_arg = dynamic_cast<Argument *>(rhs.second);
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
  llvm::BitVector used; // index range: 1 ~ num_regs
  llvm::DenseMap<Value *, int> reg_map;
  // Ordered by ActiveCmp so iteration follows end-ascending order
  // (arguments first).
  std::set<LRA::LiveInterval, ActiveCmp> active;

  void reset(Function * = nullptr);
  void reserve_for_arg(const LRA::LiveIntervalSet &);
  void expire_old_intervals(LRA::LiveInterval);
  void spill_at_interval(LRA::LiveInterval);

public:
  RegAllocator(uint num_regs, bool is_float)
      : is_float(is_float), num_regs(num_regs), used(num_regs + 1, false) {
    LOG_DEBUG << "RegAllocator initialize: R=" << num_regs << "\n";
    assert(num_regs <= MAXR);
  }
  RegAllocator() = delete;

  // Returns true if v does not need a register.
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
