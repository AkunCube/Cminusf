#include <cmath>
#include <limits>

#include "common/ConstFolder.hpp"

Constant *ConstFolder::try_fold(Instruction *instr) {
  if (!instr) {
    return nullptr;
  }

  const unsigned num = instr->get_num_operand();
  if (num == 2) {
    Constant *value1 = dynamic_cast<Constant *>(instr->get_operand(0));
    Constant *value2 = dynamic_cast<Constant *>(instr->get_operand(1));
    if (value1 == nullptr || value2 == nullptr) {
      return nullptr;
    }
    return compute(instr, value1, value2);
  }
  if (num == 1) {
    Constant *value1 = dynamic_cast<Constant *>(instr->get_operand(0));
    if (value1 == nullptr) {
      return nullptr;
    }
    return compute(instr, value1);
  }
  return nullptr;
}

Constant *ConstFolder::compute(Instruction *instr, Constant *value1,
                               Constant *value2) {
  const Instruction::OpID op = instr->get_instr_type();
  if (instr->is_add() || instr->is_sub() || instr->is_mul() ||
      instr->is_div() || instr->is_cmp()) {
    ConstantInt *int1 = dynamic_cast<ConstantInt *>(value1);
    ConstantInt *int2 = dynamic_cast<ConstantInt *>(value2);
    if (int1 == nullptr || int2 == nullptr) {
      return nullptr;
    }

    const int c1 = int1->get_value();
    const int c2 = int2->get_value();
    switch (op) {
      case Instruction::add:
        return ConstantInt::get(c1 + c2, module_);
      case Instruction::sub:
        return ConstantInt::get(c1 - c2, module_);
      case Instruction::mul:
        return ConstantInt::get(c1 * c2, module_);
      case Instruction::sdiv:
        return c2 == 0 ? nullptr : ConstantInt::get(c1 / c2, module_);
      case Instruction::eq:
        return ConstantInt::get(c1 == c2, module_);
      case Instruction::ne:
        return ConstantInt::get(c1 != c2, module_);
      case Instruction::gt:
        return ConstantInt::get(c1 > c2, module_);
      case Instruction::ge:
        return ConstantInt::get(c1 >= c2, module_);
      case Instruction::lt:
        return ConstantInt::get(c1 < c2, module_);
      case Instruction::le:
        return ConstantInt::get(c1 <= c2, module_);
      default:
        return nullptr;
    }
  }

  if (instr->is_fadd() || instr->is_fsub() || instr->is_fmul() ||
      instr->is_fdiv() || instr->is_fcmp()) {
    ConstantFP *fp1 = dynamic_cast<ConstantFP *>(value1);
    ConstantFP *fp2 = dynamic_cast<ConstantFP *>(value2);
    if (fp1 == nullptr || fp2 == nullptr) {
      return nullptr;
    }

    const float c1 = fp1->get_value();
    const float c2 = fp2->get_value();
    switch (op) {
      case Instruction::fadd:
        return ConstantFP::get(c1 + c2, module_);
      case Instruction::fsub:
        return ConstantFP::get(c1 - c2, module_);
      case Instruction::fmul:
        return ConstantFP::get(c1 * c2, module_);
      case Instruction::fdiv:
        if (std::isnan(c1) || std::isnan(c2)) {
          return nullptr;
        }
        return ConstantFP::get(c1 / c2, module_);
      case Instruction::feq:
        return ConstantInt::get(c1 == c2, module_);
      case Instruction::fne:
        return ConstantInt::get(c1 != c2, module_);
      case Instruction::fgt:
        return ConstantInt::get(c1 > c2, module_);
      case Instruction::fge:
        return ConstantInt::get(c1 >= c2, module_);
      case Instruction::flt:
        return ConstantInt::get(c1 < c2, module_);
      case Instruction::fle:
        return ConstantInt::get(c1 <= c2, module_);
      default:
        return nullptr;
    }
  }

  return nullptr;
}

Constant *ConstFolder::compute(Instruction *instr, Constant *value1) {
  const Instruction::OpID op = instr->get_instr_type();
  switch (op) {
    case Instruction::sitofp: {
      ConstantInt *int_value = dynamic_cast<ConstantInt *>(value1);
      if (int_value == nullptr) {
        return nullptr;
      }
      return ConstantFP::get(static_cast<float>(int_value->get_value()),
                             module_);
    }
    case Instruction::fptosi: {
      ConstantFP *fp_value = dynamic_cast<ConstantFP *>(value1);
      if (fp_value == nullptr) {
        return nullptr;
      }
      const double value = static_cast<double>(fp_value->get_value());
      if (value > std::numeric_limits<int>::max() ||
          value < std::numeric_limits<int>::min() || std::isnan(value)) {
        return nullptr;
      }
      return ConstantInt::get(static_cast<int>(value), module_);
    }
    case Instruction::zext: {
      ConstantInt *int_value = dynamic_cast<ConstantInt *>(value1);
      if (int_value == nullptr) {
        return nullptr;
      }
      return ConstantInt::get(int_value->get_value(), module_);
    }
    default:
      return nullptr;
  }
}
