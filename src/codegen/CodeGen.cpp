#include "CodeGen.hpp"

#include "ASMInstruction.hpp"
#include "CodeGenUtil.hpp"
#include "Function.hpp"
#include "Register.hpp"

void CodeGen::allocate() {
  // 备份 $ra $fp
  unsigned offset = PROLOGUE_OFFSET_BASE;

  // 为每个参数分配栈空间
  for (auto &arg : context.func->get_args()) {
    auto size = arg.get_type()->get_size();
    offset = ALIGN(offset + size, size);
    context.offset_map[&arg] = -static_cast<int>(offset);
  }

  // 为指令结果分配栈空间
  for (auto &bb : context.func->get_basic_blocks()) {
    for (auto &instr : bb.get_instructions()) {
      // 每个非 void 的定值都分配栈空间
      if (not instr.is_void()) {
        auto size = instr.get_type()->get_size();
        offset = ALIGN(offset + size, size);
        context.offset_map[&instr] = -static_cast<int>(offset);

        if (instr.is_phi()) {
          offset = ALIGN(offset + size, size);
          context.phi_temp_offset_map[static_cast<PhiInst *>(&instr)] =
              -static_cast<int>(offset);
        }
      }
      // alloca 的副作用：分配额外空间
      if (instr.is_alloca()) {
        auto *alloca_inst = static_cast<AllocaInst *>(&instr);
        auto alloc_size = alloca_inst->get_alloca_type()->get_size();
        offset += alloc_size;
      }
    }
  }

  // 分配栈空间，需要是 16 的整数倍
  context.frame_size = ALIGN(offset, PROLOGUE_ALIGN);
}

void CodeGen::load_int32(int32_t val, const Reg &reg) {
  if (IS_IMM_12(val)) {
    append_inst(ADDI WORD, {reg.print(), "$zero", std::to_string(val)});
  } else {
    load_large_int32(val, reg);
  }
}

void CodeGen::load_large_int32(int32_t val, const Reg &reg) {
  int32_t high_20 = val >> 12; // si20
  uint32_t low_12 = val & LOW_12_MASK;
  append_inst(LU12I_W, {reg.print(), std::to_string(high_20)});
  append_inst(ORI, {reg.print(), reg.print(), std::to_string(low_12)});
}

void CodeGen::load_large_int64(int64_t val, const Reg &reg) {
  auto low_32 = static_cast<int32_t>(val & LOW_32_MASK);
  load_large_int32(low_32, reg);

  auto high_32 = static_cast<int32_t>(val >> 32);
  int32_t high_32_low_20 = (high_32 << 12) >> 12; // si20
  int32_t high_32_high_12 = high_32 >> 20;        // si12
  append_inst(LU32I_D, {reg.print(), std::to_string(high_32_low_20)});
  append_inst(LU52I_D,
              {reg.print(), reg.print(), std::to_string(high_32_high_12)});
}

void CodeGen::load_to_greg(Value *val, const Reg &reg) {
  assert(val->get_type()->is_integer_type() ||
         val->get_type()->is_pointer_type());

  if (dynamic_cast<UndefValue *>(val)) {
    load_int32(0, reg);
  } else if (auto *constant = dynamic_cast<ConstantInt *>(val)) {
    load_int32(constant->get_value(), reg);
  } else if (auto *global = dynamic_cast<GlobalVariable *>(val)) {
    append_inst(LOAD_ADDR, {reg.print(), global->get_name()});
  } else {
    load_from_stack_to_greg(val, reg);
  }
}

void CodeGen::load_float_imm(float val, const FReg &reg) {
  int32_t bytes = *reinterpret_cast<int32_t *>(&val);
  load_large_int32(bytes, Reg::t(8));
  append_inst(GR2FR WORD, {reg.print(), Reg::t(8).print()});
}

void CodeGen::load_to_freg(Value *val, const FReg &freg) {
  assert(val->get_type()->is_float_type());
  if (dynamic_cast<UndefValue *>(val)) {
    load_float_imm(0.0F, freg);
  } else if (auto *constant = dynamic_cast<ConstantFP *>(val)) {
    float val = constant->get_value();
    load_float_imm(val, freg);
  } else {
    load_from_stack_to_freg(context.offset_map.at(val), freg);
  }
}

void CodeGen::load_from_stack_to_greg(Value *val, const Reg &reg) {
  load_from_stack_to_greg(val->get_type(), context.offset_map.at(val), reg);
}

void CodeGen::load_from_stack_to_greg(Type *type, int offset, const Reg &reg) {
  auto offset_str = std::to_string(offset);
  if (IS_IMM_12(offset)) {
    if (type->is_int1_type()) {
      append_inst(LOAD BYTE, {reg.print(), "$fp", offset_str});
    } else if (type->is_int32_type()) {
      append_inst(LOAD WORD, {reg.print(), "$fp", offset_str});
    } else {
      assert(type->is_pointer_type());
      append_inst(LOAD DOUBLE, {reg.print(), "$fp", offset_str});
    }
    return;
  }

  auto addr = Reg::t(8);
  load_large_int64(offset, addr);
  append_inst(ADD DOUBLE, {addr.print(), "$fp", addr.print()});
  if (type->is_int1_type()) {
    append_inst(LOAD BYTE, {reg.print(), addr.print(), "0"});
  } else if (type->is_int32_type()) {
    append_inst(LOAD WORD, {reg.print(), addr.print(), "0"});
  } else {
    assert(type->is_pointer_type());
    append_inst(LOAD DOUBLE, {reg.print(), addr.print(), "0"});
  }
}

void CodeGen::load_from_stack_to_freg(int offset, const FReg &reg) {
  if (IS_IMM_12(offset)) {
    append_inst(FLOAD SINGLE, {reg.print(), "$fp", std::to_string(offset)});
    return;
  }

  auto addr = Reg::t(8);
  load_large_int64(offset, addr);
  append_inst(ADD DOUBLE, {addr.print(), "$fp", addr.print()});
  append_inst(FLOAD SINGLE, {reg.print(), addr.print(), "0"});
}

void CodeGen::store_from_greg(Value *val, const Reg &reg) {
  store_to_stack_from_greg(val->get_type(), context.offset_map.at(val), reg);
}

void CodeGen::store_from_freg(Value *val, const FReg &r) {
  store_to_stack_from_freg(context.offset_map.at(val), r);
}

void CodeGen::store_to_stack_from_greg(Type *type, int offset, const Reg &reg) {
  auto offset_str = std::to_string(offset);
  if (IS_IMM_12(offset)) {
    if (type->is_int1_type()) {
      append_inst(STORE BYTE, {reg.print(), "$fp", offset_str});
    } else if (type->is_int32_type()) {
      append_inst(STORE WORD, {reg.print(), "$fp", offset_str});
    } else {
      assert(type->is_pointer_type());
      append_inst(STORE DOUBLE, {reg.print(), "$fp", offset_str});
    }
    return;
  }

  auto addr = Reg::t(8);
  load_large_int64(offset, addr);
  append_inst(ADD DOUBLE, {addr.print(), "$fp", addr.print()});
  if (type->is_int1_type()) {
    append_inst(STORE BYTE, {reg.print(), addr.print(), "0"});
  } else if (type->is_int32_type()) {
    append_inst(STORE WORD, {reg.print(), addr.print(), "0"});
  } else {
    assert(type->is_pointer_type());
    append_inst(STORE DOUBLE, {reg.print(), addr.print(), "0"});
  }
}

void CodeGen::store_to_stack_from_freg(int offset, const FReg &reg) {
  if (IS_IMM_12(offset)) {
    append_inst(FSTORE SINGLE, {reg.print(), "$fp", std::to_string(offset)});
    return;
  }

  auto addr = Reg::t(8);
  load_large_int64(offset, addr);
  append_inst(ADD DOUBLE, {addr.print(), "$fp", addr.print()});
  append_inst(FSTORE SINGLE, {reg.print(), addr.print(), "0"});
}

llvm::SmallVector<CodeGen::PhiCopy>
CodeGen::collect_phi_copies(BasicBlock *pred, BasicBlock *succ) {
  llvm::SmallVector<PhiCopy> copies;
  for (Instruction &inst : succ->get_instructions()) {
    if (!inst.is_phi()) {
      break;
    }

    auto *phi = static_cast<PhiInst *>(&inst);
    assert(phi->get_num_operand() % 2 == 0 && "Malformed phi operands");
    bool found_incoming = false;
    for (unsigned i = 0; i < phi->get_num_operand(); i += 2) {
      if (phi->get_operand(i + 1) != pred) {
        continue;
      }
      assert(!found_incoming && "Duplicate phi incoming block");
      Value *source = phi->get_operand(i);
      assert(source->get_type() == phi->get_type() &&
             "Phi incoming value has the wrong type");
      copies.emplace_back(source, phi);
      found_incoming = true;
    }
    assert(found_incoming && "Missing phi value for CFG predecessor");
  }
  return copies;
}

void CodeGen::emit_parallel_phi_copies(BasicBlock *pred, BasicBlock *succ) {
  auto copies = collect_phi_copies(pred, succ);

  // Snapshot every source before overwriting any phi destination.
  for (auto [source, phi] : copies) {
    int temp_offset = context.phi_temp_offset_map.at(phi);
    if (phi->get_type()->is_float_type()) {
      load_to_freg(source, FReg::ft(0));
      store_to_stack_from_freg(temp_offset, FReg::ft(0));
    } else {
      load_to_greg(source, Reg::t(0));
      store_to_stack_from_greg(phi->get_type(), temp_offset, Reg::t(0));
    }
  }

  for (auto [source, phi] : copies) {
    (void)source;
    int temp_offset = context.phi_temp_offset_map.at(phi);
    int destination_offset = context.offset_map.at(phi);
    if (phi->get_type()->is_float_type()) {
      load_from_stack_to_freg(temp_offset, FReg::ft(0));
      store_to_stack_from_freg(destination_offset, FReg::ft(0));
    } else {
      load_from_stack_to_greg(phi->get_type(), temp_offset, Reg::t(0));
      store_to_stack_from_greg(phi->get_type(), destination_offset, Reg::t(0));
    }
  }
}

void CodeGen::gen_prologue() {
  // 寄存器备份及栈帧设置
  append_inst("st.d $ra, $sp, -8");
  append_inst("st.d $fp, $sp, -16");
  append_inst("addi.d $fp, $sp, 0");
  if (IS_IMM_12(-static_cast<int>(context.frame_size))) {
    append_inst("addi.d $sp, $sp, " +
                std::to_string(-static_cast<int>(context.frame_size)));
  } else {
    load_large_int64(context.frame_size, Reg::t(0));
    append_inst("sub.d $sp, $sp, $t0");
  }

  // 将函数参数转移到栈帧上
  int garg_cnt = 0;
  int farg_cnt = 0;
  for (auto &arg : context.func->get_args()) {
    if (arg.get_type()->is_float_type()) {
      store_from_freg(&arg, FReg::fa(farg_cnt++));
    } else { // int or pointer
      store_from_greg(&arg, Reg::a(garg_cnt++));
    }
  }
}

void CodeGen::gen_epilogue() {
  append_inst(exit_label_name(context.func), ASMInstruction::Label);
  append_inst("addi.d $sp, $fp, 0");
  append_inst("ld.d $ra, $sp, -8");
  append_inst("ld.d $fp, $sp, -16");
  append_inst("jr $ra");
}

void CodeGen::gen_ret() {
  if (context.inst->get_num_operand() == 0) {
    load_int32(0, Reg::a(0));
    append_inst("b " + exit_label_name(context.func));
    return;
  }

  Value *ret_val = context.inst->get_operand(0);
  if (ret_val->get_type()->is_float_type()) {
    load_to_freg(ret_val, FReg::fa(0));
  } else {
    load_to_greg(ret_val, Reg::a(0));
  }
  append_inst("b " + exit_label_name(context.func));
}

void CodeGen::gen_br() {
  auto *branch = static_cast<BranchInst *>(context.inst);
  auto *pred = branch->get_parent();
  if (!branch->is_cond_br()) {
    auto *succ = static_cast<BasicBlock *>(branch->get_operand(0));
    emit_parallel_phi_copies(pred, succ);
    append_inst("b " + label_name(succ));
    return;
  }

  load_to_greg(branch->get_operand(0), Reg::t(0));
  auto *true_bb = static_cast<BasicBlock *>(branch->get_operand(1));
  auto *false_bb = static_cast<BasicBlock *>(branch->get_operand(2));
  std::string true_edge_label = "." + context.func->get_name() + "_phi_edge_" +
                                std::to_string(context.next_phi_edge_id++);

  append_inst("bnez $t0, " + true_edge_label);
  emit_parallel_phi_copies(pred, false_bb);
  append_inst("b " + label_name(false_bb));

  append_inst(true_edge_label, ASMInstruction::Label);
  emit_parallel_phi_copies(pred, true_bb);
  append_inst("b " + label_name(true_bb));
}

void CodeGen::gen_binary() {
  // 分别将左右操作数加载到 $t0 $t1
  load_to_greg(context.inst->get_operand(0), Reg::t(0));
  load_to_greg(context.inst->get_operand(1), Reg::t(1));
  // 根据指令类型生成汇编
  switch (context.inst->get_instr_type()) {
    case Instruction::add:
      output.emplace_back("add.w $t2, $t0, $t1");
      break;
    case Instruction::sub:
      output.emplace_back("sub.w $t2, $t0, $t1");
      break;
    case Instruction::mul:
      output.emplace_back("mul.w $t2, $t0, $t1");
      break;
    case Instruction::sdiv:
      output.emplace_back("div.w $t2, $t0, $t1");
      break;
    default:
      assert(false);
  }
  // 将结果填入栈帧中
  store_from_greg(context.inst, Reg::t(2));
}

void CodeGen::gen_float_binary() {
  load_to_freg(context.inst->get_operand(0), FReg::ft(0));
  load_to_freg(context.inst->get_operand(1), FReg::ft(1));

  switch (context.inst->get_instr_type()) {
    case Instruction::fadd:
      output.emplace_back("fadd.s $ft2, $ft0, $ft1");
      break;
    case Instruction::fsub:
      output.emplace_back("fsub.s $ft2, $ft0, $ft1");
      break;
    case Instruction::fmul:
      output.emplace_back("fmul.s $ft2, $ft0, $ft1");
      break;
    case Instruction::fdiv:
      output.emplace_back("fdiv.s $ft2, $ft0, $ft1");
      break;
    default:
      assert(false);
  }

  store_from_freg(context.inst, FReg::ft(2));
}

void CodeGen::gen_alloca() {
  auto *alloca_inst = static_cast<AllocaInst *>(context.inst);
  unsigned int alloc_size = alloca_inst->get_alloca_type()->get_size();
  int offset = context.offset_map.at(alloca_inst);
  offset -= alloc_size;
  std::string offset_str = std::to_string(offset);
  if (IS_IMM_12(offset)) {
    append_inst("addi.d $t0, $fp, " + offset_str);
  } else {
    load_large_int64(offset, Reg::t(0));
    append_inst("add.d $t0, $fp, $t0");
  }
  store_from_greg(alloca_inst, Reg::t(0));
}

void CodeGen::gen_load() {
  auto *ptr = context.inst->get_operand(0);
  auto *type = context.inst->get_type();
  load_to_greg(ptr, Reg::t(0));

  if (type->is_float_type()) {
    append_inst("fld.s $ft0, $t0, 0");
    store_from_freg(context.inst, FReg::ft(0));
  } else {
    if (type->is_int1_type()) {
      append_inst("ld.b $t1, $t0, 0");
    } else if (type->is_int32_type()) {
      append_inst("ld.w $t1, $t0, 0");
    } else if (type->is_pointer_type()) {
      append_inst("ld.d $t1, $t0, 0");
    } else {
      assert(false && "Unsupported type in gen_load");
    }
    store_from_greg(context.inst, Reg::t(1));
  }
}

void CodeGen::gen_store() {
  Value *val = context.inst->get_operand(0);
  Value *ptr = context.inst->get_operand(1);
  Type *valType = val->get_type();

  load_to_greg(ptr, Reg::t(0));

  if (valType->is_float_type()) {
    load_to_freg(val, FReg::ft(0));
    append_inst("fst.s $ft0, $t0, 0");
  } else {
    load_to_greg(val, Reg::t(1));
    if (valType->is_int1_type()) {
      append_inst("st.b $t1, $t0, 0");
    } else if (valType->is_int32_type()) {
      append_inst("st.w $t1, $t0, 0");
    } else if (valType->is_pointer_type()) {
      append_inst("st.d $t1, $t0, 0");
    } else {
      assert(false && "Unsupported type in gen_load");
    }
  }
}

void CodeGen::gen_icmp() {
  load_to_greg(context.inst->get_operand(0), Reg::t(0));
  load_to_greg(context.inst->get_operand(1), Reg::t(1));

  switch (context.inst->get_instr_type()) {
    case Instruction::eq:
      append_inst("xor $t0, $t0, $t1");
      append_inst("sltui $t0, $t0, 1");
      break;
    case Instruction::ne:
      append_inst("xor $t0, $t0, $t1");
      append_inst("sltu $t0, $zero, $t0");
      break;
    case Instruction::lt:
      append_inst("slt $t0, $t0, $t1");
      break;
    case Instruction::gt:
      append_inst("slt $t0, $t1, $t0");
      break;
    case Instruction::le:
      append_inst("slt $t0, $t1, $t0");
      append_inst("sltui $t0, $t0, 1");
      break;
    case Instruction::ge:
      append_inst("slt $t0, $t0, $t1");
      append_inst("sltui $t0, $t0, 1");
      break;
    default:
      assert(false);
  }

  store_from_greg(context.inst, Reg::t(0));
}

void CodeGen::gen_fcmp() {
  load_to_freg(context.inst->get_operand(0), FReg::ft(0));
  load_to_freg(context.inst->get_operand(1), FReg::ft(1));

  switch (context.inst->get_instr_type()) {
    case Instruction::feq:
      append_inst("fcmp.seq.s $fcc0, $ft0, $ft1");
      break;
    case Instruction::fne:
      append_inst("fcmp.sne.s $fcc0, $ft0, $ft1");
      break;
    case Instruction::flt:
      append_inst("fcmp.slt.s $fcc0, $ft0, $ft1");
      break;
    case Instruction::fgt:
      append_inst("fcmp.slt.s $fcc0, $ft1, $ft0");
      break;
    case Instruction::fle:
      append_inst("fcmp.sle.s $fcc0, $ft0, $ft1");
      break;
    case Instruction::fge:
      append_inst("fcmp.sle.s $fcc0, $ft1, $ft0");
      break;
    default:
      assert(false);
  }

  append_inst("movcf2gr $t0, $fcc0");
  store_from_greg(context.inst, Reg::t(0));
}

void CodeGen::gen_zext() {
  load_to_greg(context.inst->get_operand(0), Reg::t(0));
  append_inst("bstrpick.w $t0, $t0, 0, 0");
  store_from_greg(context.inst, Reg::t(0));
}

void CodeGen::gen_call() {
  auto *call = static_cast<CallInst *>(context.inst);
  auto *callee = static_cast<Function *>(call->get_operand(0));

  unsigned garg_cnt = 0;
  unsigned farg_cnt = 0;
  for (unsigned i = 1; i < call->get_num_operand(); ++i) {
    auto *arg = call->get_operand(i);
    if (arg->get_type()->is_float_type()) {
      assert(farg_cnt < 8 && "More than 8 floating-point call arguments");
      load_to_freg(arg, FReg::fa(farg_cnt++));
    } else {
      assert(garg_cnt < 8 && "More than 8 general-purpose call arguments");
      load_to_greg(arg, Reg::a(garg_cnt++));
    }
  }

  append_inst("bl " + callee->get_name());

  if (call->get_type()->is_float_type()) {
    store_from_freg(call, FReg::fa(0));
  } else if (not call->get_type()->is_void_type()) {
    store_from_greg(call, Reg::a(0));
  }
}

void CodeGen::gen_gep() {
  Instruction *gep = context.inst;
  Value *ptr = gep->get_operand(0);
  load_to_greg(ptr, Reg::t(0));
  assert(ptr->get_type()->is_pointer_type());

  Type *indexed_type = ptr->get_type()->get_pointer_element_type();
  for (unsigned i = 1; i < gep->get_num_operand(); ++i) {
    Value *index = gep->get_operand(i);
    if (i > 1) {
      assert(indexed_type->is_array_type());
      indexed_type = indexed_type->get_array_element_type();
    }
    load_to_greg(index, Reg::t(1));
    load_int32(indexed_type->get_size(), Reg::t(2));
    append_inst("mul.d $t1, $t1, $t2");
    append_inst("add.d $t0, $t0, $t1");
  }
  store_from_greg(gep, Reg::t(0));
}

void CodeGen::gen_sitofp() {
  load_to_greg(context.inst->get_operand(0), Reg::t(0));
  append_inst("movgr2fr.w $ft0, $t0");
  append_inst("ffint.s.w $ft1, $ft0");
  store_from_freg(context.inst, FReg::ft(1));
}

void CodeGen::gen_fptosi() {
  load_to_freg(context.inst->get_operand(0), FReg::ft(0));
  append_inst("ftintrz.w.s $ft1, $ft0");
  append_inst("movfr2gr.s $t0, $ft1");
  store_from_greg(context.inst, Reg::t(0));
}

void CodeGen::run() {
  // 确保每个函数中基本块的名字都被设置好
  // 想一想：为什么？
  m->set_print_name();

  /* 使用 GNU 伪指令为全局变量分配空间
   * 你可以使用 `la.local` 指令将标签 (全局变量) 的地址载入寄存器中, 比如
   * 要将 `a` 的地址载入 $t0, 只需要 `la.local $t0, a`
   */
  if (!m->get_global_variable().empty()) {
    append_inst("Global variables", ASMInstruction::Comment);
    /* 虽然下面两条伪指令可以简化为一条 `.bss` 伪指令, 但是我们还是选择使用
     * `.section` 将全局变量放到可执行文件的 BSS 段, 原因如下:
     * - 尽可能对齐交叉编译器 loongarch64-unknown-linux-gnu-gcc 的行为
     * - 支持更旧版本的 GNU 汇编器, 因为 `.bss` 伪指令是应该相对较新的指令,
     *   GNU 汇编器在 2023 年 2 月的 2.37 版本才将其引入
     */
    append_inst(".text", ASMInstruction::Atrribute);
    append_inst(".section", {".bss", "\"aw\"", "@nobits"},
                ASMInstruction::Atrribute);
    for (auto &global : m->get_global_variable()) {
      auto size = global.get_type()->get_pointer_element_type()->get_size();
      append_inst(".globl", {global.get_name()}, ASMInstruction::Atrribute);
      append_inst(".type", {global.get_name(), "@object"},
                  ASMInstruction::Atrribute);
      append_inst(".size", {global.get_name(), std::to_string(size)},
                  ASMInstruction::Atrribute);
      append_inst(global.get_name(), ASMInstruction::Label);
      append_inst(".space", {std::to_string(size)}, ASMInstruction::Atrribute);
    }
  }

  // 函数代码段
  output.emplace_back(".text", ASMInstruction::Atrribute);
  for (auto &func : m->get_functions()) {
    if (not func.is_declaration()) {
      // 更新 context
      context.clear();
      context.func = &func;

      // 函数信息
      append_inst(".globl", {func.get_name()}, ASMInstruction::Atrribute);
      append_inst(".type", {func.get_name(), "@function"},
                  ASMInstruction::Atrribute);
      append_inst(func.get_name(), ASMInstruction::Label);

      // 分配函数栈帧
      allocate();
      // 生成 prologue
      gen_prologue();

      for (auto &bb : func.get_basic_blocks()) {
        append_inst(label_name(&bb), ASMInstruction::Label);
        for (auto &instr : bb.get_instructions()) {
          // For debug
          append_inst(instr.print(), ASMInstruction::Comment);
          context.inst = &instr; // 更新 context
          switch (instr.get_instr_type()) {
            case Instruction::ret:
              gen_ret();
              break;
            case Instruction::br:
              gen_br();
              break;
            case Instruction::add:
            case Instruction::sub:
            case Instruction::mul:
            case Instruction::sdiv:
              gen_binary();
              break;
            case Instruction::fadd:
            case Instruction::fsub:
            case Instruction::fmul:
            case Instruction::fdiv:
              gen_float_binary();
              break;
            case Instruction::alloca:
              gen_alloca();
              break;
            case Instruction::load:
              gen_load();
              break;
            case Instruction::store:
              gen_store();
              break;
            case Instruction::ge:
            case Instruction::gt:
            case Instruction::le:
            case Instruction::lt:
            case Instruction::eq:
            case Instruction::ne:
              gen_icmp();
              break;
            case Instruction::fge:
            case Instruction::fgt:
            case Instruction::fle:
            case Instruction::flt:
            case Instruction::feq:
            case Instruction::fne:
              gen_fcmp();
              break;
            case Instruction::phi:
              break;
            case Instruction::call:
              gen_call();
              break;
            case Instruction::getelementptr:
              gen_gep();
              break;
            case Instruction::zext:
              gen_zext();
              break;
            case Instruction::fptosi:
              gen_fptosi();
              break;
            case Instruction::sitofp:
              gen_sitofp();
              break;
          }
        }
      }
      // 生成 epilogue
      gen_epilogue();
    }
  }
}

std::string CodeGen::print() const {
  std::string result;
  for (const auto &inst : output) {
    result += inst.format();
  }
  return result;
}
