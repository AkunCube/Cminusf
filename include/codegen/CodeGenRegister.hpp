#ifndef CODEGEN_CODEGENREGISTER_HPP
#define CODEGEN_CODEGENREGISTER_HPP

#include "ASMInstruction.hpp"
#include "BasicBlock.hpp"
#include "Constant.hpp"
#include "Function.hpp"
#include "IRprinter.hpp"
#include "Instruction.hpp"
#include "Liverange.hpp"
#include "Module.hpp"
#include "Regalloc.hpp"
#include "Register.hpp"
#include "Value.hpp"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

// #define STACK_ALIGN(x) (((x / 16) + (x % 16 ? 1 : 0)) * 16)
#define STACK_ALIGN(x) ALIGN(x, 16)
#define CONST_0 ConstantInt::get(0, module)
#define FP "$fp"
#define SP "$sp"
#define RA_reg "$ra"
// #a = 8, #t = 9, reserve $t0, $t1 for temporary
#define R_USABLE (17 - 2)
// #fa = 8, #ft=16, reserve $ft0, $ft1 for temporary
#define FR_USABLE (24 - 2)
#define ARG_R 8

class CodeGenRegister {
public:
  explicit CodeGenRegister(Module *module)
      : module(module), live_range_analyzer(module, phi_map),
        ra_int(R_USABLE, false), ra_float(FR_USABLE, true) {}

  std::string print() const;

  void run();

  template <class... Args> void append_inst(Args... arg) {
    output.emplace_back(arg...);
  }

  void append_inst(const char *inst, llvm::ArrayRef<std::string> args,
                   ASMInstruction::InstType ty = ASMInstruction::Instruction) {
    auto content = std::string(inst) + " ";
    for (const auto &arg : args) {
      content += arg + ", ";
    }
    content.pop_back();
    content.pop_back();
    output.emplace_back(content, ty);
  }

private:
  // TODO: 添加你需要的函数
  void get_phi_map();
  __attribute__((warn_unused_result)) std::string
  value_to_reg(Value *, int i = 0, std::string = "");
  void copy_stmt();
  void pass_arguments(CallInst *);
  void make_sure_in_range(std::string instr_ir, std::string reg1,
                          std::string reg2, int imm, std::string tinstr,
                          int tid = 0, int bits = 12, bool u = false) {
    /* this function will tranfser
     * `addi.d $a0, $fp, imm` to `add.d $a0, $fp, tmp_ireg` if imm
     * overfloats for `addi.d`. During the time we move `imm` to `tmp_ireg`,
     * another tmp ireg will be used, they can be same, but we must specify
     * it.
     */
    auto treg = tmp_reg_name(tid, false);
    assert(treg != reg2 && "it's possible to write tid before reg2's use");
    auto [l, h] = imm_range(bits, u);
    if (l <= imm and imm <= h) {
      append_inst(instr_ir.c_str(), {reg1, reg2, std::to_string(imm)});
    } else {
      assert(value_to_reg(ConstantInt::get(imm, module), tid, treg) == treg);
      append_inst(tinstr.c_str(), {reg1, reg2, treg});
    }
  }

  // 进行copy操作
  bool gen_copy(Value *lhs, std::string rhs_reg);
  void gen_copy(std::string lhs_reg, std::string rhs_reg, bool is_float);
  // 处理指针情况
  void ptr_content_to_reg(Value *, std::string);
  std::pair<std::string, bool> get_reg_name(Value *, int = 0) const;
  std::string bool_to_branch(Instruction *);

  // 向寄存器中装载数据
  void load_to_greg(Value *, const Reg &);
  void load_to_freg(Value *, const FReg &);
  void load_from_stack_to_greg(Value *, const Reg &);

  // 向寄存器中加载立即数
  void load_large_int32(int32_t, const Reg &);
  void load_large_int64(int64_t, const Reg &);
  void load_float_imm(float, const FReg &);

  // 将寄存器中的数据保存回栈上
  void store_from_greg(Value *, const Reg &);
  void store_from_freg(Value *, const FReg &);

  void allocate();
  void gen_prologue();
  void gen_ret();
  void gen_br();
  void gen_binary();
  void gen_alloca();
  void gen_load();
  void gen_store();
  void gen_icmp();
  void gen_fcmp();
  void gen_zext();
  void gen_call();
  void gen_gep();
  void gen_sitofp();
  void gen_fptosi();
  void gen_epilogue();

  static std::string label_name(BasicBlock *bb) {
    return "." + bb->get_parent()->get_name() + "_" + bb->get_name();
  }

  static std::string func_exit_label_name(Function *func) {
    return func->get_name() + "_exit";
  }

  static std::string fcmp_label_name(BasicBlock *bb, unsigned cnt) {
    return label_name(bb) + "_fcmp_" + std::to_string(cnt);
  }

  static std::string reg_name(uint i, bool is_float) {
    std::string name;
    if (is_float) {
      // assert(false && "not implemented!");
      if (1 <= i and i <= 8)
        name = "$fa" + std::to_string(i - 1);
      else if (9 <= i and i <= FR_USABLE)
        name = "$ft" + std::to_string(i - 9 + 2);
      else
        name = "WRONG_REG_" + std::to_string(i);
    } else {
      if (1 <= i and i <= 8)
        name = "$a" + std::to_string(i - 1);
      else if (9 <= i and i <= R_USABLE)
        name = "$t" + std::to_string(i - 9 + 2);
      else
        name = "WRONG_REG_" + std::to_string(i);
    }
    return name;
  }

  std::string tmp_reg_name(int i, bool is_float) const {
    assert(i == 0 or i == 1);
    return (is_float ? "$ft" : "$t") + std::to_string(i);
  }

  static std::pair<int, int> imm_range(int bit, bool u) {
    std::pair<int, int> res;
    if (u) {
      res.first = 0;
      res.second = (1 << bit) - 1;
    } else {
      bit--;
      res.first = -(1 << bit);
      res.second = (1 << bit) - 1;
    }

    return res;
  };

  static int type_len(Type *type) {
    if (type->is_float_type())
      return 4;
    else if (type->is_integer_type()) {
      if (static_cast<IntegerType *>(type)->get_num_bits() == 32)
        return 4;
      else
        return 1;
    } else if (type->is_pointer_type())
      return 8;
    else if (type->is_array_type()) {
      auto arr_tp = static_cast<ArrayType *>(type);
      int n = arr_tp->get_num_of_elements();
      return n * type_len(arr_tp->get_element_type());
    } else {
      assert(false && "unexpected case while computing type-length");
    }
  }

  static std::string suffix(Type *type) {
    int len = type_len(type);
    switch (len) {
      case 1:
        return ".b";
      case 2:
        return ".h";
      case 4:
        return type->is_float_type() ? ".s" : ".w";
      case 8:
        return ".d";
    }
    assert(false && "no such suffix");
  }

  bool no_stack_alloca(Instruction *instr) const {
    if (instr->is_void())
      return true;
    if (instr->is_fcmp() or instr->is_cmp() or instr->is_zext())
      return true;
    auto reg_map = (instr->get_type()->is_float_type() ? ra_float.get_reg_map()
                                                       : ra_int.get_reg_map());
    if (reg_map.find(instr) != reg_map.end())
      return true;

    return false;
  }

  std::string label_in_assem(BasicBlock *bb) const {
    return (context.func)->get_name() + bb->get_name().substr(5);
  }

  struct {
    /* 随着ir遍历设置 */
    Function *func{nullptr};    // 当前函数
    BasicBlock *bb{nullptr};    // 当前基本块
    Instruction *inst{nullptr}; // 当前指令
    /* 在allocate()中设置 */
    unsigned frame_size{0};                    // 当前函数的栈帧大小
    llvm::DenseMap<Value *, int> offset_map{}; // 指针相对 fp 的偏移
    unsigned fcmp_cnt{0}; // fcmp 的计数器, 用于创建 fcmp 需要的 label

    void clear() {
      func = nullptr;
      bb = nullptr;
      inst = nullptr;
      frame_size = 0;
      fcmp_cnt = 0;
      offset_map.clear();
    }

  } context;

  Module *module;
  llvm::SmallVector<ASMInstruction> output;

  LRA::LiveRangeAnalyzer live_range_analyzer;
  LRA::LiveIntervalSet live_intervals_int, live_intervals_float;
  RA::RegAllocator ra_int, ra_float;

  LRA::PhiMap phi_map;
  llvm::DenseMap<Constant *, std::string> ro_data;

  // TODO:添加你需要的变量
};
// TODO:本次实验为开放性实验，你可以自行设计实验框架并自行对提供的实验框架进行修改。本框架只作为参考，不要让它束缚住你的设计思路。
// TODO: 对框架不满可尽情修改
#endif
