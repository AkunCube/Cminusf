#pragma once

#include "ASMInstruction.hpp"
#include "Module.hpp"
#include "Register.hpp"
#include "llvm/ADT/SmallVector.h"

class CodeGen {
public:
  explicit CodeGen(Module *module) : m(module) {}

  std::string print() const;

  void run();

  template <class... Args> void append_inst(Args... arg) {
    output.emplace_back(arg...);
  }

  void append_inst(const char *inst, std::initializer_list<std::string> args,
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
  using PhiCopy = std::pair<Value *, PhiInst *>;

  void allocate();

  // Load immediate values.
  void load_int32(int32_t, const Reg &);
  void load_large_int32(int32_t, const Reg &);
  void load_large_int64(int64_t, const Reg &);
  void load_float_imm(float, const FReg &);

  // Load values into registers.
  void load_to_greg(Value *, const Reg &);
  void load_to_freg(Value *, const FReg &);

  // Load values from stack slots.
  void load_from_stack_to_greg(Value *, const Reg &);
  void load_from_stack_to_greg(Type *, int, const Reg &);
  void load_from_stack_to_freg(int, const FReg &);

  // Store register values into Value-owned stack slots.
  void store_from_greg(Value *, const Reg &);
  void store_from_freg(Value *, const FReg &);

  // Store register values into explicit stack slots.
  void store_to_stack_from_greg(Type *, int, const Reg &);
  void store_to_stack_from_freg(int, const FReg &);

  // Collect and emit parallel phi copies on CFG edges.
  llvm::SmallVector<PhiCopy> collect_phi_copies(BasicBlock *, BasicBlock *);
  void emit_parallel_phi_copies(BasicBlock *, BasicBlock *);

  void gen_prologue();
  void gen_ret();
  void gen_br();
  void gen_binary();
  void gen_float_binary();
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

  static std::string exit_label_name(Function *func) {
    return "." + func->get_name() + "_exit";
  }

  struct {
    /* Updated while traversing the IR. */
    Function *func{nullptr};    // Current function.
    Instruction *inst{nullptr}; // Current instruction.
    /* Initialized by allocate(). */
    unsigned frame_size{0};                        // Current frame size.
    std::unordered_map<Value *, int> offset_map{}; // Offset relative to fp.
    std::unordered_map<PhiInst *, int> phi_temp_offset_map{};
    unsigned next_phi_edge_id{0};

    void clear() {
      func = nullptr;
      inst = nullptr;
      frame_size = 0;
      offset_map.clear();
      phi_temp_offset_map.clear();
      next_phi_edge_id = 0;
    }

  } context;

  Module *m;
  std::list<ASMInstruction> output;
};
