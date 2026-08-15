#include "BasicBlock.hpp"
#include "Constant.hpp"
#include "FuncInfo.hpp"
#include "Function.hpp"
#include "IRprinter.hpp"
#include "Instruction.hpp"
#include "Module.hpp"
#include "PassManager.hpp"
#include "Value.hpp"
#include "common/ConstFolder.hpp"
#include "logging.hpp"
#include "passes.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

namespace GVNExpression {

/**
 * for constructor of class derived from `Expression`, we make it public
 * because `std::make_shared` needs the constructor to be publicly available,
 * but you should call the static factory method `create` instead the
 * constructor itself to get the desired data
 */
class Expression {
public:
  // TODO: you need to extend expression types according to testcases
  enum gvn_expr_t {
    e_bin,
  };
  explicit Expression(gvn_expr_t t) : expr_type(t) {}
  virtual ~Expression() = default;
  virtual std::string print() = 0;
  [[nodiscard]] gvn_expr_t get_expr_type() const { return expr_type; }

private:
  gvn_expr_t expr_type;
};

bool operator==(const std::shared_ptr<Expression> &lhs,
                const std::shared_ptr<Expression> &rhs);
bool operator==(const GVNExpression::Expression &lhs,
                const GVNExpression::Expression &rhs);

// arithmetic expression
class BinaryExpression : public Expression {
public:
  static std::shared_ptr<BinaryExpression>
  create(Instruction::OpID op, std::shared_ptr<Expression> lhs,
         std::shared_ptr<Expression> rhs) {
    return std::make_shared<BinaryExpression>(op, lhs, rhs);
  }
  std::string print() override {
    return "(" + print_instr_op_name(op_) + " " + lhs_->print() + " " +
           rhs_->print() + ")";
  }

  bool equiv(const BinaryExpression *other) const {
    return op_ == other->op_ and *lhs_ == *other->lhs_ and
           *rhs_ == *other->rhs_;
  }

  BinaryExpression(Instruction::OpID op, std::shared_ptr<Expression> lhs,
                   std::shared_ptr<Expression> rhs)
      : Expression(e_bin), op_(op), lhs_(std::move(lhs)), rhs_(std::move(rhs)) {
  }

  bool both_phi() {
    // TODO: determine whether both operands are phi functions
    return false;
  }

  std::shared_ptr<Expression> get_lhs() { return lhs_; }

  std::shared_ptr<Expression> get_rhs() { return rhs_; }

  Instruction::OpID get_op() { return op_; }

private:
  Instruction::OpID op_;
  std::shared_ptr<Expression> lhs_, rhs_;
};

// TODO: add other expression subclasses here
} // namespace GVNExpression

/**
 * Congruence class in each partitions
 * Note: for constant propagation, you might need to add other fields
 */
struct CongruenceClass {
  size_t index_;
  // representative of the congruence class, used to replace all the members
  // (except itself) when analysis is done
  Value *leader_{};
  // value expression in congruence class
  // Note: type of value_expr_ and value_phi_ is shared_ptr<Expression>, which
  // correspond to the return type of valueExpr and valuePhiFunc function, if
  // you want to design your own value expression, you need to modify the type
  // of value_expr_, value_phi_ and the return type of valueExpr and
  // valuePhiFunc
  std::shared_ptr<GVNExpression::Expression> value_expr_;
  // value φ-function is an annotation of the congruence class
  std::shared_ptr<GVNExpression::Expression> value_phi_;
  // equivalent variables in one congruence class
  std::set<Value *> members_;

  explicit CongruenceClass(size_t index) : index_(index) {}

  bool operator<(const CongruenceClass &other) const {
    return this->index_ < other.index_;
  }
  bool operator==(const CongruenceClass &other) const;
};

} // namespace

namespace std {
template <>
// overload std::less for std::shared_ptr<CongruenceClass>, i.e. how to sort the
// congruence classes
struct less<std::shared_ptr<CongruenceClass>> {
  bool operator()(const std::shared_ptr<CongruenceClass> &a,
                  const std::shared_ptr<CongruenceClass> &b) const {
    // nullptrs should never appear in partitions, so we just dereference it
    return *a < *b;
  }
};
} // namespace std

namespace {

class GVN : public Pass {
public:
  using partitions = std::set<std::shared_ptr<CongruenceClass>>;
  static bool isTop(const partitions &p) { return (*p.begin())->index_ == 0; }
  GVN(Module *m, bool dump_json) : Pass(m), dump_json_(dump_json) {}
  // pass start
  void run(PassManager &pm) override;
  // init for pass metadata;
  void initPerFunction();

  // fill the following functions according to Pseudocode, **you might need to
  // add more arguments**
  void detectEquivalences(llvm::ilist<GlobalVariable> *global_list);
  partitions join(const partitions &P1, const partitions &P2, BasicBlock *lbb,
                  BasicBlock *rbb);
  std::shared_ptr<CongruenceClass>
  intersect(const std::shared_ptr<CongruenceClass> &,
            const std::shared_ptr<CongruenceClass> &, BasicBlock *lbb,
            BasicBlock *rbb);
  partitions transferFunction(Instruction *x, Value *e, const partitions &pin);
  std::shared_ptr<GVNExpression::Expression>
  valuePhiFunc(const std::shared_ptr<GVNExpression::Expression> &,
               const partitions &);
  std::shared_ptr<GVNExpression::Expression> valueExpr(const partitions &pout,
                                                       Value *v);

  static std::shared_ptr<GVNExpression::Expression>
  getVN(const partitions &pout, std::shared_ptr<GVNExpression::Expression> ve);

  // replace cc members with leader
  void replace_cc_members();

  // note: be careful when to use copy constructor or clone
  static partitions clone(const partitions &p);

  // create congruence class helper
  static std::shared_ptr<CongruenceClass>
  createCongruenceClass(size_t index = 0) {
    return std::make_shared<CongruenceClass>(index);
  }

private:
  bool dump_json_;
  std::uint64_t next_value_number_ = 1;
  Function *func_{};
  std::map<BasicBlock *, partitions> pin_, pout_;
  FuncInfo *func_info_;
  std::unique_ptr<ConstFolder> folder_;
};

bool operator==(const GVN::partitions &p1, const GVN::partitions &p2);

using namespace GVNExpression;
using std::string_literals::operator""s;
using std::shared_ptr;

namespace utils {
static std::string print_congruence_class(const CongruenceClass &cc) {
  std::stringstream ss;
  if (cc.index_ == 0) {
    ss << "top class\n";
    return ss.str();
  }
  ss << "\nindex: " << cc.index_ << "\nleader: " << cc.leader_->print()
     << "\nvalue phi: " << (cc.value_phi_ ? cc.value_phi_->print() : "nullptr"s)
     << "\nvalue expr: "
     << (cc.value_expr_ ? cc.value_expr_->print() : "nullptr"s)
     << "\nmembers: {";
  for (const auto &member : cc.members_) {
    ss << member->print() << "; ";
  }
  ss << "}\n";
  return ss.str();
}

static std::string dump_cc_json(const CongruenceClass &cc) {
  std::string json;
  json += "[";
  for (auto *member : cc.members_) {
    if (dynamic_cast<Constant *>(member) != nullptr) {
      json += member->print() + ", ";
    } else {
      json += "\"%" + member->get_name() + "\", ";
    }
  }
  json += "]";
  return json;
}

static std::string dump_partition_json(const GVN::partitions &p) {
  std::string json;
  json += "[";
  for (const auto &cc : p) {
    json += dump_cc_json(*cc) + ", ";
  }
  json += "]";
  return json;
}

static std::string
dump_bb2partition(const std::map<BasicBlock *, GVN::partitions> &map) {
  std::string json;
  json += "{";
  for (auto [bb, p] : map) {
    json += "\"" + bb->get_name() + "\": " + dump_partition_json(p) + ",";
  }
  json += "}";
  return json;
}

// logging utility for you
static void print_partitions(const GVN::partitions &p) {
  if (p.empty()) {
    LOG_DEBUG << "empty partitions\n";
    return;
  }
  std::string log;
  for (const auto &cc : p) {
    log += print_congruence_class(*cc);
  }
  LOG_DEBUG << log; // please don't use std::cout
}
} // namespace utils

GVN::partitions GVN::join(const partitions &P1, const partitions &P2,
                          BasicBlock *lbb, BasicBlock *rbb) {
  // TODO: do intersection pair-wise
}

std::shared_ptr<CongruenceClass>
GVN::intersect(const std::shared_ptr<CongruenceClass> &Ci,
               const std::shared_ptr<CongruenceClass> &Cj, BasicBlock *lbb,
               BasicBlock *rbb) {
  // TODO: do intersection
}

void GVN::detectEquivalences(llvm::ilist<GlobalVariable> *global_list) {
  auto top = std::set<shared_ptr<CongruenceClass>>();
  top.insert(createCongruenceClass(0));
  bool changed = false;
  // initialize pout with top
  for (auto &bb : func_->get_basic_blocks()) {
    pout_[&bb] = top;
  }
  // iterate until convergence
  BasicBlock *entry = func_->get_entry_block();
  pin_[entry] = {};

  // TODO: you might need to do something here

  do {
    changed = false;

    for (auto &bb : func_->get_basic_blocks()) {
      GVN::partitions p = {};
      // TODO: compute p (pin of bb) according to predecessors of bb

      // iterate through all instructions in the block
      for (auto &instr : bb.get_instructions()) {
        p = transferFunction(&instr, &instr, p);
      }

      // check changes in pout
      if (p != pout_[&bb]) {
        changed = true;
      }
      pout_[&bb] = std::move(p);
    }
  } while (changed);
}

shared_ptr<Expression> GVN::valueExpr(const partitions &pout, Value *v) {
  // TODO: do something here for const propagation and other cases

  auto instr = dynamic_cast<Instruction *>(v);
  if (instr == nullptr or instr->is_void()) {
    return nullptr;
  }

  // TODO: create value expression for instr according to its type
  // Hint: you might need to use valueExpr recursively
  // Although TA's implementation use Expression as value expression, you can
  // design your own
  return nullptr;
}

GVN::partitions GVN::transferFunction(Instruction *x, Value *e,
                                      const partitions &pin) {
  partitions pout = clone(pin);

  if (x->is_void()) {
    return pout;
  }

  // TODO: remove x from any cc which contains x

  auto ve = valueExpr(pout, dynamic_cast<Instruction *>(e));
  auto vpf = valuePhiFunc(ve, pin);

  for (const auto &Ci : pout) {
    // TODO: if ve or vpf is in Ci, insert x into Ci
  }

  auto cc = createCongruenceClass(next_value_number_++);
  // TODO: you might need to do something here for const propagation

  cc->members_ = {x};
  cc->value_expr_ = ve;
  cc->value_phi_ = vpf;

  pout.insert(cc);

  return pout;
}

shared_ptr<Expression> GVN::valuePhiFunc(const shared_ptr<Expression> &ve,
                                         const partitions &P) {
  auto binary_ve = std::dynamic_pointer_cast<BinaryExpression>(ve);
  if (binary_ve != nullptr and binary_ve->both_phi()) {
    // TODO: if ve is binary expression and both of its operands are phi
    // expression, return phi expression according to the algorithm
  }
  return nullptr;
}

shared_ptr<Expression> GVN::getVN(const partitions &pout,
                                  shared_ptr<Expression> ve) {
  // TODO
  return nullptr;
}

void GVN::initPerFunction() {
  next_value_number_ = 1;
  pin_.clear();
  pout_.clear();
}

void GVN::replace_cc_members() {
  for (auto &[_bb, part] : pout_) {
    auto *bb =
        _bb; // workaround: structured bindings can't be captured in C++17
    for (const auto &cc : part) {
      if (cc->index_ == 0) {
        continue;
      }
      // if you are planning to do constant propagation, leaders should be set
      // to constant at some point
      for (const auto &member : cc->members_) {
        bool member_is_phi = dynamic_cast<PhiInst *>(member) != nullptr;
        bool value_phi = cc->value_phi_ != nullptr;
        if (member != cc->leader_ and (value_phi or !member_is_phi)) {
          // only replace the members if users are in the same block as bb
          member->replace_use_with_if(cc->leader_, [bb](Use *use) {
            auto *user = use->val_;
            if (auto *instr = dynamic_cast<Instruction *>(user)) {
              auto *parent = instr->get_parent();
              auto &bb_pre = parent->get_pre_basic_blocks();
              if (instr->is_phi()) { // as copy stmt, the phi belongs to this
                                     // block
                return std::find(bb_pre.begin(), bb_pre.end(), bb) !=
                       bb_pre.end();
              }
              return parent == bb;
            }
            return false;
          });
        }
      }
    }
  }
}

// top-level function, done for you
void GVN::run(PassManager &pm) {
  m_->set_print_name();
  std::ofstream gvn_json;
  if (dump_json_) {
    gvn_json.open("gvn.json", std::ios::out);
    gvn_json << "[";
  }

  folder_ = std::make_unique<ConstFolder>(m_);
  func_info_ = &pm.getAnalysis<FuncInfo>();

  for (auto &f : m_->get_functions()) {
    if (f.get_basic_blocks().empty()) {
      continue;
    }
    func_ = &f;
    initPerFunction();
    LOG_INFO << "Processing " << f.get_name();
    detectEquivalences(&m_->get_global_variable());
    LOG_INFO << "===============pin=========================\n";
    for (auto &[bb, part] : pin_) {
      LOG_INFO << "\n===============bb: " << bb->get_name()
               << "=========================\npartitionIn: ";
      for (const auto &cc : part) {
        LOG_DEBUG << f.get_name();
        LOG_INFO << utils::print_congruence_class(*cc);
      }
    }
    LOG_INFO << "\n===============pout=========================\n";
    for (auto &[bb, part] : pout_) {
      LOG_INFO << "\n=====bb: " << bb->get_name() << "=====\npartitionOut: ";
      for (const auto &cc : part) {
        LOG_DEBUG << f.get_name();
        LOG_INFO << utils::print_congruence_class(*cc);
      }
    }
    if (dump_json_) {
      gvn_json << "{\n\"function\": ";
      gvn_json << "\"" << f.get_name() << "\", ";
      gvn_json << "\n\"pout\": " << utils::dump_bb2partition(pout_);
      gvn_json << "},";
    }
    replace_cc_members(); // don't delete instructions, just replace them
  }
  if (dump_json_) {
    gvn_json << "]";
  }
}

template <typename T>
static bool equiv_as(const Expression &lhs, const Expression &rhs) {
  // we use static_cast because we are very sure that both operands are actually
  // T, not other types.
  return static_cast<const T *>(&lhs)->equiv(static_cast<const T *>(&rhs));
}

bool GVNExpression::operator==(const Expression &lhs, const Expression &rhs) {
  if (lhs.get_expr_type() != rhs.get_expr_type()) {
    return false;
  }
  switch (lhs.get_expr_type()) {
    case Expression::e_bin:
      return equiv_as<BinaryExpression>(lhs, rhs);
    // TODO: add other cases here
    default:
      return false;
  }
}

bool GVNExpression::operator==(const shared_ptr<Expression> &lhs,
                               const shared_ptr<Expression> &rhs) {
  return lhs and rhs and *lhs == *rhs;
}

GVN::partitions GVN::clone(const partitions &p) {
  partitions data;
  for (const auto &cc : p) {
    data.insert(std::make_shared<CongruenceClass>(*cc));
  }
  return data;
}

bool operator==(const GVN::partitions &p1, const GVN::partitions &p2) {
  if (p1.size() != p2.size()) {
    return false;
  }
  for (auto it1 = p1.begin(), it2 = p2.begin(); it1 != p1.end(); ++it1, ++it2) {
    if (!(**it1 == **it2)) {
      return false;
    }
  }
  return true;
}

bool CongruenceClass::operator==(const CongruenceClass &other) const {
  // TODO: you might need to change this function to fit your implementation
  return std::tie(leader_, members_) == std::tie(other.leader_, other.members_);
}

} // namespace

std::unique_ptr<Pass> createGVN(Module *m, bool dump_json) {
  return std::make_unique<GVN>(m, dump_json);
}
