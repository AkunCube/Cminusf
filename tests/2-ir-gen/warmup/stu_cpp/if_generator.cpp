#include <iostream>
#include <memory>

#include "BasicBlock.hpp"
#include "Constant.hpp"
#include "Function.hpp"
#include "IRBuilder.hpp"
#include "Instruction.hpp"
#include "Module.hpp"
#include "Type.hpp"

void makeFunction(Module *module) {
  IRBuilder builder(nullptr, module);

  Type *i32Type = module->get_int32_type();
  Type *floatType = module->get_float_type();

  FunctionType *mainFuncType = FunctionType::get(i32Type, {});
  Function *mainFunc = Function::create(mainFuncType, "main", module);

  BasicBlock *entryBB = BasicBlock::create(module, "entry", mainFunc);
  builder.set_insert_point(entryBB);

  auto intConst = [&](int num) -> ConstantInt * {
    return ConstantInt::get(num, module);
  };

  auto floatConst = [&](float num) -> ConstantFP * {
    return ConstantFP::get(num, module);
  };

  AllocaInst *retAlloca = builder.create_alloca(i32Type);
  AllocaInst *floatAlloca = builder.create_alloca(floatType);
  builder.create_store(intConst(0), retAlloca);
  builder.create_store(floatConst(5.555), floatAlloca);

  LoadInst *floatLoad = builder.create_load(floatAlloca);
  FCmpInst *cmpResult = builder.create_fcmp_gt(floatLoad, floatConst(1.0));

  BasicBlock *thenBB = BasicBlock::create(module, "then", mainFunc);
  BasicBlock *elseBB = BasicBlock::create(module, "else", mainFunc);
  BasicBlock *mergeBB = BasicBlock::create(module, "merge", mainFunc);

  builder.create_cond_br(cmpResult, thenBB, elseBB);

  builder.set_insert_point(thenBB);
  builder.create_store(intConst(233), retAlloca);
  builder.create_br(mergeBB);

  builder.set_insert_point(elseBB);
  builder.create_store(intConst(0), retAlloca);
  builder.create_br(mergeBB);

  builder.set_insert_point(mergeBB);
  LoadInst *result = builder.create_load(retAlloca);
  builder.create_ret(result);
}

int main() {
  auto module = std::make_unique<Module>();
  makeFunction(module.get());
  std::cout << module->print();
  return 0;
}
