#include <cmath>
#include <limits>

#include "common/ConstFolder.hpp"

Constant *ConstFolder::try_fold(Instruction *instr) {
  if (!instr) {
    return nullptr;
  }

  const auto op = instr->get_instr_type();
  if (instr->is_add() || instr->is_sub() || instr->is_mul() ||
      instr->is_div() || instr->is_cmp()) {
    auto *value1 = dynamic_cast<ConstantInt *>(instr->get_operand(0));
    auto *value2 = dynamic_cast<ConstantInt *>(instr->get_operand(1));
    if (!value1 || !value2) {
      return nullptr;
    }

    const int c1 = value1->get_value();
    const int c2 = value2->get_value();
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
    auto *value1 = dynamic_cast<ConstantFP *>(instr->get_operand(0));
    auto *value2 = dynamic_cast<ConstantFP *>(instr->get_operand(1));
    if (!value1 || !value2) {
      return nullptr;
    }

    const float c1 = value1->get_value();
    const float c2 = value2->get_value();
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

  switch (op) {
    case Instruction::sitofp: {
      auto *int_value = dynamic_cast<ConstantInt *>(instr->get_operand(0));
      if (!int_value) {
        return nullptr;
      }
      return ConstantFP::get(static_cast<float>(int_value->get_value()),
                             module_);
    }
    case Instruction::fptosi: {
      auto *fp_value = dynamic_cast<ConstantFP *>(instr->get_operand(0));
      if (!fp_value) {
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
      auto *int_value = dynamic_cast<ConstantInt *>(instr->get_operand(0));
      if (!int_value) {
        return nullptr;
      }
      return ConstantInt::get(int_value->get_value(), module_);
    }
    default:
      return nullptr;
  }
}
