#include <cassert>

#include "Function.hpp"
#include "Instruction.hpp"
#include "Regalloc.hpp"

using namespace RA;

bool RegAllocator::no_reg_alloca(Value *v) {
  auto instr = dynamic_cast<Instruction *>(v);
  auto arg = dynamic_cast<Argument *>(v);
  if (instr) {
    if (instr->is_void() || instr->is_alloca() ||
        instr->get_use_list().empty()) {
      return true;
    }

    if (instr->is_cmp() || instr->is_fcmp()) {
      if (instr->get_use_list().size() != 1) {
        return false;
      }
      Value *user = instr->get_use_list().begin()->val_;
      return dynamic_cast<BranchInst *>(user);
    }
    return false;
  }

  if (arg) { // Only the first ARG_MAX_R arguments get registers.
    return arg->get_arg_no() >= ARG_MAX_R;
  }
  assert(false && "only instruction and argument's LiveInterval exits");
}

void RegAllocator::reset(Function *func) {
  reg_map.clear();
  active.clear();
  used.reset();
}

void RegAllocator::reserve_for_arg(const LRA::LiveIntervalSet &liveints) {
  // Only the first ARG_MAX_R arguments are assigned registers.
  // arg_no is 0-based and maps to reg arg_no + 1 ($a0..$a7 / $fa0..$fa7),
  // which matches the calling convention.
  // ra_int / ra_float handle integer and float values separately, so only
  // arguments of the current class are allocated here.
  for (const auto &liveint : liveints) {
    auto *arg = dynamic_cast<Argument *>(liveint.second);
    if (!arg) {
      continue;
    }
    if (is_float && !arg->get_type()->is_float_type()) {
      continue;
    }
    if (!is_float && !arg->get_type()->is_integer_type()) {
      continue;
    }

    unsigned arg_no = arg->get_arg_no();
    if (arg_no >= ARG_MAX_R) {
      // Arguments after the first ARG_MAX_R are passed on the stack.
      continue;
    }

    int reg = arg_no + 1; // arg0 -> reg 1 ($a0/$fa0), ..., arg7 -> reg 8
    assert(!used[reg] && "argument register already taken");
    used[reg] = true;
    reg_map[arg] = reg;
    // Keep the interval in active so expire_old_intervals can release the
    // register once the argument's live range ends.
    active.insert(liveint);
  }
}

// input set is sorted by increasing start point
void RegAllocator::linear_scan(const LRA::LiveIntervalSet &liveints,
                               Function *func) {
  reset(func);
  reserve_for_arg(liveints);
  for (const auto &liveint : liveints) {
    Value *val = liveint.second;
    if (dynamic_cast<Argument *>(val)) {
      continue;
    }
    if (is_float && !val->get_type()->is_float_type()) {
      continue;
    }
    if (!is_float && !val->get_type()->is_integer_type()) {
      continue;
    }
    if (no_reg_alloca(val)) {
      continue;
    }

    expire_old_intervals(liveint);
    if (active.size() == num_regs) {
      // Registers are exhausted; spill may evict an active interval or,
      // if the new interval ends latest, leave it memory-resident instead.
      spill_at_interval(liveint);
    } else {
      // Index 0 is never used, so find_next_unset(0) scans registers
      // 1..num_regs for the first free one.
      int reg = used.find_next_unset(0);
      assert(reg != -1);
      used[reg] = true;
      reg_map[liveint.second] = reg;
      active.insert(liveint);
    }
  }
}

void RegAllocator::expire_old_intervals(LRA::LiveInterval liveint) {
  for (const LRA::LiveInterval &interval : llvm::make_early_inc_range(active)) {
    if (interval.first.end < liveint.first.start) {
      int reg = reg_map[interval.second];
      used[reg] = false;
      active.erase(interval);
    }
  }
}

void RegAllocator::spill_at_interval(LRA::LiveInterval liveint) {
  // Pick the non-argument interval with the furthest end as the victim;
  // registers 1..ARG_MAX_R are reserved for function arguments.
  auto it = std::prev(active.end());
  while (dynamic_cast<Argument *>(it->second)) {
    if (it == active.begin())
      break; // active holds only arguments; fall back to spilling one.
    --it;
  }

  // Spill whichever interval ends furthest: its next use is farthest away,
  // so it is the least valuable register occupant. If the new interval ends
  // later than the victim, spill the new interval instead (leave it in
  // memory) and keep the victim's register untouched.
  if (liveint.first.end > it->first.end) {
    return;
  }

  Value *old_val = it->second;
  auto reg_it = reg_map.find(old_val);
  assert(reg_it != reg_map.end() &&
         "No register mapping found for the spilled value");

  int reg = reg_it->second;
  reg_map.erase(old_val);
  reg_map[liveint.second] = reg;

  active.erase(it);
  active.insert(liveint);
}
