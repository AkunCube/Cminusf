#ifndef PASSES_COMMON_CONSTFOLDER_HPP
#define PASSES_COMMON_CONSTFOLDER_HPP

#include "Constant.hpp"
#include "Instruction.hpp"
#include "Module.hpp"

class ConstFolder {
public:
  explicit ConstFolder(Module *module) : module_(module) {}
  Constant *try_fold(Instruction *instr);

private:
  Module *module_;
};

#endif
