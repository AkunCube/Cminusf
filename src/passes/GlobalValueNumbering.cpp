#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

#include "BasicBlock.hpp"
#include "Constant.hpp"
#include "Dominators.hpp"
#include "FuncInfo.hpp"
#include "Function.hpp"
#include "GlobalVariable.hpp"
#include "Instruction.hpp"
#include "Module.hpp"
#include "PassManager.hpp"
#include "Value.hpp"
#include "common/ConstFolder.hpp"
#include "passes.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseMapInfo.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/SmallVector.h"

using namespace llvm;
using nlohmann::json;

using ValueNumber = uint32_t;
using LeaderBinding = std::pair<ValueNumber, Value *>;

static bool is_commutative(const Instruction *instr) {
  assert(instr != nullptr && "Instruction pointer cannot be null");
  switch (instr->get_instr_type()) {
    case Instruction::add:
    case Instruction::mul:
    case Instruction::fadd:
    case Instruction::fmul:
    case Instruction::eq:
    case Instruction::ne:
    case Instruction::feq:
    case Instruction::fne:
      return true;
    default:
      return false;
  }
}

static bool same_phi(PhiInst *a, PhiInst *b) {
  const unsigned num = a->get_num_operand();
  if (num != b->get_num_operand()) {
    return false;
  }
  for (unsigned i = 0; i < num; i += 2) {
    if (a->get_operand(i) != b->get_operand(i)) {
      return false;
    }
    if (a->get_operand(i + 1) != b->get_operand(i + 1)) {
      return false;
    }
  }
  return true;
}

static std::string value_name(Value *v) {
  GlobalVariable *gv = dynamic_cast<GlobalVariable *>(v);
  if (gv != nullptr) {
    return "@" + v->get_name();
  }
  Function *func = dynamic_cast<Function *>(v);
  if (func != nullptr) {
    return "@" + v->get_name();
  }
  Constant *c = dynamic_cast<Constant *>(v);
  if (c != nullptr) {
    return c->print();
  }
  return "%" + v->get_name();
}

static json dump_leader_block(
    const SmallVectorImpl<LeaderBinding> &leaders,
    const DenseMap<ValueNumber, SmallVector<std::string>> &members_by_vn) {
  json block = json::array();
  for (auto &binding : leaders) {
    json members = json::array();
    auto it = members_by_vn.find(binding.first);
    if (it != members_by_vn.end()) {
      members.push_back(value_name(binding.second));
      for (const std::string &name : it->second) {
        if (name != value_name(binding.second)) {
          members.push_back(name);
        }
      }
    } else {
      members.push_back(value_name(binding.second));
    }
    block.push_back(members);
  }
  return block;
}

namespace {

struct Expression {
  Instruction::OpID opcode;
  SmallVector<ValueNumber> var_args;
  Type *type = nullptr;
  bool commutative = false;

  explicit Expression(Instruction::OpID op) : opcode(op), var_args{} {}
  explicit Expression() : opcode(Instruction::OpID(0)) {}

  bool operator==(const Expression &other) const {
    if (opcode != other.opcode || type != other.type) {
      return false;
    }

    return var_args == other.var_args;
  }

  friend hash_code hash_value(const Expression &value) {
    return hash_combine(value.opcode, value.type,
                        hash_combine_range(value.var_args));
  }
};

} // namespace

namespace llvm {
template <> struct DenseMapInfo<Expression> {
  static unsigned getHashValue(const Expression &expr) {
    return static_cast<unsigned>(hash_value(expr));
  }

  static bool isEqual(const Expression &lhs, const Expression &rhs) {
    return lhs == rhs;
  }

  static Expression getEmptyKey() {
    return Expression(static_cast<Instruction::OpID>(-1));
  }

  static Expression getTombstoneKey() {
    return Expression(static_cast<Instruction::OpID>(-2));
  }
};
} // namespace llvm

namespace {

/// Apply the same canonicalization as `createExpr`: sort commutative operands
/// and normalize swapped comparison predicates, so that translated edge
/// expressions hash to the same value numbers as the original expressions.
static void finalize_expression(Expression &expr, Instruction *instr) {
  if (is_commutative(instr)) {
    assert(expr.var_args.size() == 2 && "");
    if (expr.var_args[0] > expr.var_args[1]) {
      std::swap(expr.var_args[0], expr.var_args[1]);
    }
    expr.commutative = true;
  }

  CmpInst *cmp_instr = dynamic_cast<CmpInst *>(instr);
  if (cmp_instr != nullptr) {
    if (expr.var_args[0] > expr.var_args[1]) {
      std::swap(expr.var_args[0], expr.var_args[1]);
      expr.opcode = cmp_instr->get_swapped_predicate();
      expr.commutative = true;
    }
  }
}

class ValueTable {
public:
  explicit ValueTable(Module *module, FuncInfo *func_info)
      : folder_(module), func_info_(func_info) {}

  ValueNumber lookupOrAdd(Value *V);
  Value *tryPhiCongruence(Instruction *instr, BasicBlock *bb,
                          DenseMap<ValueNumber, Value *> &leader_table);

  Constant *constantFor(ValueNumber num) const {
    auto it = const_numbering.find(num);
    if (it == const_numbering.end()) {
      return nullptr;
    }
    return it->second;
  }

  const DenseMap<Value *, ValueNumber> &getValueNumbering() const {
    return value_numbering;
  }

private:
  DenseMap<Value *, ValueNumber> value_numbering;
  DenseMap<Expression, ValueNumber> expression_numbering;
  DenseMap<ValueNumber, PhiInst *> numbering_phi;
  DenseMap<ValueNumber, Constant *> const_numbering;

  SmallVector<Expression> expressions;
  SmallVector<ValueNumber> expr_idx;
  ValueNumber next_expr_number = 0;
  ValueNumber next_value_number = 1;

  ConstFolder folder_;
  FuncInfo *func_info_;

  std::pair<ValueNumber, bool> assignExpNewValueNum(Expression &exp);
  Expression createExpr(Instruction *instr);
  Expression createPhiExpr(PhiInst *phi);
  ValueNumber phiTranslate(const BasicBlock *pred, const BasicBlock *phi_block,
                           ValueNumber num);
  Constant *getConstant(Value *v);
  Constant *tryFoldConst(Instruction *instr);
};

std::pair<ValueNumber, bool> ValueTable::assignExpNewValueNum(Expression &exp) {
  ValueNumber &e = expression_numbering[exp];
  bool create_new_val_num = !e;
  if (create_new_val_num) {
    expressions.push_back(exp);
    if (expr_idx.size() < next_value_number + 1) {
      expr_idx.resize(next_value_number * 2);
    }
    e = next_value_number;
    expr_idx[next_value_number++] = next_expr_number++;
  }
  return {e, create_new_val_num};
}

Constant *ValueTable::getConstant(Value *v) {
  Constant *c = dynamic_cast<Constant *>(v);
  if (c != nullptr) {
    return c;
  }

  auto it = value_numbering.find(v);
  if (it == value_numbering.end()) {
    return nullptr;
  }

  auto cit = const_numbering.find(it->second);
  if (cit == const_numbering.end()) {
    return nullptr;
  }
  return cit->second;
}

Constant *ValueTable::tryFoldConst(Instruction *instr) {
  const unsigned num = instr->get_num_operand();
  if (num == 2) {
    Constant *c0 = getConstant(instr->get_operand(0));
    Constant *c1 = getConstant(instr->get_operand(1));
    if (c0 != nullptr && c1 != nullptr) {
      return folder_.compute(instr, c0, c1);
    }
    return nullptr;
  }
  if (num == 1) {
    Constant *c0 = getConstant(instr->get_operand(0));
    if (c0 != nullptr) {
      return folder_.compute(instr, c0);
    }
    return nullptr;
  }
  return nullptr;
}

ValueNumber ValueTable::lookupOrAdd(Value *v) {
  auto it = value_numbering.find(v);
  if (it != value_numbering.end()) {
    return it->second;
  }

  Instruction *instr = dynamic_cast<Instruction *>(v);
  if (!instr) {
    ValueNumber num = next_value_number;
    value_numbering[v] = num;
    Constant *c = dynamic_cast<Constant *>(v);
    if (c != nullptr) {
      const_numbering[num] = c;
    }
    return next_value_number++;
  }

  // Fold first so that a folded instruction shares the value number of its
  // constant; later instructions can then fold through it in one traversal.
  Constant *folded = tryFoldConst(instr);
  if (folded != nullptr) {
    ValueNumber num = lookupOrAdd(folded);
    value_numbering[instr] = num;
    return num;
  }

  Expression expr;
  switch (instr->get_instr_type()) {
    // TODO: implement structural numbering for call (pure functions) and
    // getelementptr; give them fresh value numbers for now.
    case Instruction::call: {
      Function *callee = dynamic_cast<Function *>(instr->get_operand(0));
      if (callee != nullptr && func_info_ != nullptr &&
          func_info_->is_pure_function(callee)) {
        // Pure calls with identical arguments always produce the same value.
        Expression expr = createExpr(instr);
        ValueNumber number = assignExpNewValueNum(expr).first;
        value_numbering[instr] = number;
        return number;
      }
      value_numbering[instr] = next_value_number;
      return next_value_number++;
    }
    case Instruction::add:
    case Instruction::sub:
    case Instruction::mul:
    case Instruction::sdiv:
    case Instruction::fadd:
    case Instruction::fsub:
    case Instruction::fmul:
    case Instruction::fdiv:
    case Instruction::ge:
    case Instruction::gt:
    case Instruction::le:
    case Instruction::lt:
    case Instruction::eq:
    case Instruction::ne:
    case Instruction::fge:
    case Instruction::fgt:
    case Instruction::fle:
    case Instruction::flt:
    case Instruction::feq:
    case Instruction::fne:
    case Instruction::zext:
    case Instruction::fptosi:
    case Instruction::sitofp:
      expr = createExpr(instr);
      break;
    case Instruction::phi: {
      // Give the phi a fresh number first so self-referencing phis terminate.
      PhiInst *phi = static_cast<PhiInst *>(v);
      ValueNumber self = next_value_number;
      value_numbering[instr] = self;
      numbering_phi[self] = phi;
      ++next_value_number;

      // phi(x, x, ...) is just a copy of x: share its value number.
      ValueNumber first_incoming = 0;
      bool all_same_incoming = true;
      for (unsigned i = 0; i < phi->get_num_operand(); i += 2) {
        ValueNumber incoming = lookupOrAdd(phi->get_operand(i));
        if (first_incoming == 0) {
          first_incoming = incoming;
        } else if (incoming != first_incoming) {
          all_same_incoming = false;
        }
      }
      if (all_same_incoming && phi->get_num_operand() > 0) {
        value_numbering[instr] = first_incoming;
        numbering_phi.erase(self);
        return first_incoming;
      }

      // Register the incoming-value pattern: congruent phis share a number,
      // and phi-translated expressions can look the pattern up later.
      Expression phi_expr = createPhiExpr(phi);
      ValueNumber &existing = expression_numbering[phi_expr];
      if (existing == 0) {
        existing = self;
        return self;
      }
      value_numbering[instr] = existing;
      numbering_phi.erase(self);
      return existing;
    }
    default:
      value_numbering[instr] = next_value_number;
      return next_value_number++;
  }

  // createExpr may have just numbered (and folded) operands that were not
  // numbered yet, e.g. when a phi expression forces numbering of a backedge
  // chain. Retry folding now that every operand has a value number.
  Constant *late_folded = tryFoldConst(instr);
  if (late_folded != nullptr) {
    ValueNumber num = lookupOrAdd(late_folded);
    value_numbering[instr] = num;
    return num;
  }

  std::pair<ValueNumber, bool> ret = assignExpNewValueNum(expr);
  ValueNumber number = ret.first;
  value_numbering[instr] = number;
  return number;
}

Expression ValueTable::createExpr(Instruction *instr) {
  Expression expr;
  expr.opcode = instr->get_instr_type();
  expr.type = instr->get_type();

  for (Value *operand : instr->get_operands()) {
    expr.var_args.push_back(lookupOrAdd(operand));
  }

  finalize_expression(expr, instr);

  return expr;
}

Expression ValueTable::createPhiExpr(PhiInst *phi) {
  Expression expr;
  expr.opcode = Instruction::phi;
  expr.type = phi->get_type();
  for (unsigned i = 0; i < phi->get_num_operand(); i += 2) {
    expr.var_args.push_back(lookupOrAdd(phi->get_operand(i)));
    expr.var_args.push_back(lookupOrAdd(phi->get_operand(i + 1)));
  }
  return expr;
}

/// Translate a value number across the edge `pred -> phi_block`: if `num` is
/// a phi defined in `phi_block`, return the value number of its incoming
/// value for `pred`; otherwise the value is the same on every edge.
ValueNumber ValueTable::phiTranslate(const BasicBlock *pred,
                                     const BasicBlock *phi_block,
                                     ValueNumber num) {
  auto phi_it = numbering_phi.find(num);
  if (phi_it == numbering_phi.end()) {
    return num;
  }
  PhiInst *phi = phi_it->second;
  if (phi->get_parent() != phi_block) {
    return num;
  }
  for (unsigned i = 0; i < phi->get_num_operand(); i += 2) {
    if (phi->get_operand(i + 1) == pred) {
      return lookupOrAdd(phi->get_operand(i));
    }
  }
  return num;
}

/// Phi congruence: `op(phi(a,b), phi(c,d))` equals `phi(op(a,c), op(b,d))`.
/// If every predecessor edge evaluates the instruction to the same value,
/// reuse that value; otherwise look up whether a phi in this block already
/// has the per-edge results as its incoming values, e.g.
/// `%d = add %a, %b` with `%a`/`%b` phis may equal `%c = phi(%x, %y)`.
Value *
ValueTable::tryPhiCongruence(Instruction *instr, BasicBlock *bb,
                             DenseMap<ValueNumber, Value *> &leader_table) {
  if (instr->is_phi() || instr->get_num_operand() == 0) {
    return nullptr;
  }
  if (!instr->isBinary() && !instr->is_cmp() && !instr->is_fcmp() &&
      !instr->is_si2fp() && !instr->is_fp2si() && !instr->is_zext()) {
    return nullptr;
  }

  const std::list<BasicBlock *> &preds = bb->get_pre_basic_blocks();
  if (preds.size() < 2) {
    return nullptr;
  }

  // Evaluate the instruction along each incoming edge of the block.
  bool any_translated = false;
  SmallVector<ValueNumber> edge_vns;
  for (BasicBlock *pred : preds) {
    Expression edge_expr;
    edge_expr.opcode = instr->get_instr_type();
    edge_expr.type = instr->get_type();

    SmallVector<Constant *> edge_consts;
    bool edge_translated = false;
    for (Value *operand : instr->get_operands()) {
      ValueNumber operand_num = lookupOrAdd(operand);
      ValueNumber translated_num = phiTranslate(pred, bb, operand_num);
      edge_expr.var_args.push_back(translated_num);
      auto const_it = const_numbering.find(translated_num);
      if (const_it != const_numbering.end()) {
        edge_consts.push_back(const_it->second);
      } else {
        edge_consts.push_back(nullptr);
      }
      if (translated_num != operand_num) {
        edge_translated = true;
      }
    }
    finalize_expression(edge_expr, instr);

    // Fold edge expressions with constant operands, so that e.g.
    // add(phi(66, 110), phi(88, 132)) collapses to phi(154, 242).
    ValueNumber edge_vn = 0;
    if (edge_consts.size() == 2 && edge_consts[0] != nullptr &&
        edge_consts[1] != nullptr) {
      Constant *folded = folder_.compute(instr, edge_consts[0], edge_consts[1]);
      if (folded != nullptr) {
        edge_vn = lookupOrAdd(folded);
      }
    } else if (edge_consts.size() == 1 && edge_consts[0] != nullptr) {
      Constant *folded = folder_.compute(instr, edge_consts[0]);
      if (folded != nullptr) {
        edge_vn = lookupOrAdd(folded);
      }
    }
    if (edge_vn == 0) {
      edge_vn = assignExpNewValueNum(edge_expr).first;
    }
    edge_vns.push_back(edge_vn);
    if (edge_translated) {
      any_translated = true;
    }
  }
  if (!any_translated) {
    return nullptr;
  }

  // Every edge produces the same value: reuse its leader/constant.
  bool all_same = true;
  for (ValueNumber edge_vn : edge_vns) {
    if (edge_vn != edge_vns[0]) {
      all_same = false;
      break;
    }
  }
  if (all_same) {
    auto leader_it = leader_table.find(edge_vns[0]);
    if (leader_it != leader_table.end()) {
      return leader_it->second;
    }
    return constantFor(edge_vns[0]);
  }

  // The instruction is congruent to a phi whose incoming values are the
  // per-edge results, e.g. `%c = phi(%x, %y)` for `add(phi(a,b), phi(c,d))`.
  Expression phi_expr;
  phi_expr.opcode = Instruction::phi;
  phi_expr.type = instr->get_type();
  size_t i = 0;
  for (BasicBlock *pred : preds) {
    phi_expr.var_args.push_back(edge_vns[i++]);
    phi_expr.var_args.push_back(lookupOrAdd(pred));
  }

  auto expr_it = expression_numbering.find(phi_expr);
  if (expr_it == expression_numbering.end() || expr_it->second == 0) {
    return nullptr;
  }
  auto leader_it = leader_table.find(expr_it->second);
  if (leader_it == leader_table.end()) {
    return nullptr;
  }
  return leader_it->second;
}

} // namespace

namespace {

class GlobalValueNumbering : public Pass {
public:
  GlobalValueNumbering(Module *m, bool dump_json)
      : Pass(m), dump_json(dump_json), dominators_(nullptr),
        func_info_(nullptr) {}
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    // GVN only replaces uses, never the CFG.
    AU.setPreservesCFG();
  }
  void run(PassManager &pm) override;

private:
  void runOnFunction(Function &func, json &dump);
  void analyzeGVN(BasicBlock *bb, ValueTable &vn,
                  DenseMap<ValueNumber, Value *> &leader_table,
                  SmallVector<LeaderBinding> &history,
                  DenseMap<Instruction *, Value *> &replace_map);
  void eliminateDuplicatePhis(BasicBlock *bb,
                              DenseMap<Instruction *, Value *> &replace_map);
  json dumpFunctionJson(Function &func, ValueTable &vn);

  bool dump_json;
  DenseMap<BasicBlock *, SmallVector<LeaderBinding>> pout_leaders_;
  Dominators *dominators_;
  FuncInfo *func_info_;
};

void GlobalValueNumbering::run(PassManager &pm) {
  dominators_ = &pm.getAnalysis<Dominators>();
  func_info_ = &pm.getAnalysis<FuncInfo>();

  m_->set_print_name();
  json dump = json::array();
  for (Function &func : m_->get_functions()) {
    if (func.is_declaration()) {
      continue;
    }
    runOnFunction(func, dump);
  }
  if (dump_json) {
    std::ofstream out("gvn.json");
    out << dump.dump(2) << std::endl;
  }
}

void GlobalValueNumbering::runOnFunction(Function &func, json &dump) {
  pout_leaders_.clear();

  ValueTable vn(m_, func_info_);
  DenseMap<ValueNumber, Value *> leader_table;
  DenseMap<Instruction *, Value *> replace_map;
  SmallVector<LeaderBinding> history;
  analyzeGVN(func.get_entry_block(), vn, leader_table, history, replace_map);

  for (auto &[inst, replace] : replace_map) {
    inst->replace_all_use_with(replace);
  }

  if (dump_json) {
    dump.push_back(dumpFunctionJson(func, vn));
  }
}

json GlobalValueNumbering::dumpFunctionJson(Function &func, ValueTable &vn) {
  json entry;
  entry["function"] = func.get_name();

  // members_by_vn: value number -> member names, constants excluded to match
  // the framework's partition member lists.
  DenseMap<ValueNumber, SmallVector<std::string>> members_by_vn;
  for (auto &pair : vn.getValueNumbering()) {
    std::string name = value_name(pair.first);
    if (dynamic_cast<Constant *>(pair.first) == nullptr) {
      members_by_vn[pair.second].push_back(name);
    }
  }
  for (auto &pair : members_by_vn) {
    std::sort(pair.second.begin(), pair.second.end());
  }

  json pout = json::object();
  for (auto &pair : pout_leaders_) {
    json block_json = dump_leader_block(pair.second, members_by_vn);
    pout[pair.first->get_name()] = block_json;
  }
  entry["pout"] = pout;

  return entry;
}

void GlobalValueNumbering::eliminateDuplicatePhis(
    BasicBlock *bb, DenseMap<Instruction *, Value *> &replace_map) {
  SmallVector<PhiInst *> phis;
  for (Instruction &inst : bb->get_instructions()) {
    if (inst.is_phi()) {
      phis.push_back(static_cast<PhiInst *>(&inst));
    }
  }

  for (size_t i = 0; i < phis.size(); ++i) {
    PhiInst *phi = phis[i];
    if (replace_map.find(phi) != replace_map.end()) {
      continue;
    }
    for (size_t j = 0; j < i; ++j) {
      PhiInst *other = phis[j];
      if (replace_map.find(other) != replace_map.end()) {
        continue;
      }
      if (same_phi(phi, other)) {
        replace_map[phi] = other;
        break;
      }
    }
  }
}

void GlobalValueNumbering::analyzeGVN(
    BasicBlock *bb, ValueTable &vn,
    DenseMap<ValueNumber, Value *> &leader_table,
    SmallVector<LeaderBinding> &history,
    DenseMap<Instruction *, Value *> &replace_map) {
  size_t old_size = history.size();

  eliminateDuplicatePhis(bb, replace_map);

  for (Instruction &inst : bb->get_instructions()) {
    if (inst.is_void()) {
      continue;
    }
    if (replace_map.find(&inst) != replace_map.end()) {
      continue;
    }

    ValueNumber num = vn.lookupOrAdd(&inst);
    auto it = leader_table.find(num);
    if (it != leader_table.end()) {
      replace_map[&inst] = it->second;
      continue;
    }

    Constant *c = vn.constantFor(num);
    if (c != nullptr) {
      replace_map[&inst] = c;
      continue;
    }

    // Phi congruence: an expression over phis may equal an existing phi.
    Value *phi_repl = vn.tryPhiCongruence(&inst, bb, leader_table);
    if (phi_repl != nullptr) {
      replace_map[&inst] = phi_repl;
      continue;
    }

    leader_table[num] = &inst;
    history.emplace_back(num, &inst);
  }

  if (dump_json) {
    pout_leaders_[bb] =
        SmallVector<LeaderBinding>(history.begin(), history.end());
  }

  for (BasicBlock *succ : dominators_->get_dom_tree_succ_blocks(bb)) {
    analyzeGVN(succ, vn, leader_table, history, replace_map);
  }

  while (history.size() > old_size) {
    LeaderBinding bind = history.pop_back_val();
    leader_table.erase(bind.first);
  }
}

} // namespace

std::unique_ptr<Pass> createGlobalValueNumbering(Module *m, bool dump_json) {
  return std::make_unique<GlobalValueNumbering>(m, dump_json);
}
