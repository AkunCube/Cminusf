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
  FunctionType *mainFuncType = FunctionType::get(i32Type, {});
  Function *mainFunc = Function::create(mainFuncType, "main", module);

  auto intConst = [&](int num) -> ConstantInt * {
    return ConstantInt::get(num, module);
  };

  BasicBlock *entryBB = BasicBlock::create(module, "entry", mainFunc);
  BasicBlock *loopCondBB = BasicBlock::create(module, "loop_cond", mainFunc);
  BasicBlock *loopBodyBB = BasicBlock::create(module, "loop_body", mainFunc);
  BasicBlock *exitBB = BasicBlock::create(module, "exit", mainFunc);

  builder.set_insert_point(entryBB);
  AllocaInst *retAlloca = builder.create_alloca(i32Type);
  AllocaInst *sumAlloca = builder.create_alloca(i32Type);
  AllocaInst *iAlloca = builder.create_alloca(i32Type);

  builder.create_store(intConst(0), retAlloca);
  builder.create_store(intConst(10), sumAlloca);
  builder.create_store(intConst(0), iAlloca);
  builder.create_br(loopCondBB);

  builder.set_insert_point(loopCondBB);
  LoadInst *iLoad = builder.create_load(iAlloca);
  ICmpInst *cmpResult = builder.create_icmp_lt(iLoad, intConst(10));
  builder.create_cond_br(cmpResult, loopBodyBB, exitBB);

  builder.set_insert_point(loopBodyBB);
  LoadInst *iLoad2 = builder.create_load(iAlloca);
  IBinaryInst *iAdd1 = builder.create_iadd(iLoad2, intConst(1));
  builder.create_store(iAdd1, iAlloca);
  LoadInst *sumLoad = builder.create_load(sumAlloca);
  LoadInst *iLoad3 = builder.create_load(iAlloca);
  IBinaryInst *sumAdd = builder.create_iadd(sumLoad, iLoad3);
  builder.create_store(sumAdd, sumAlloca);
  builder.create_br(loopCondBB);

  builder.set_insert_point(exitBB);
  LoadInst *result = builder.create_load(sumAlloca);
  builder.create_ret(result);
}

int main() {
  auto module = std::make_unique<Module>();
  makeFunction(module.get());
  std::cout << module->print();
  return 0;
}
