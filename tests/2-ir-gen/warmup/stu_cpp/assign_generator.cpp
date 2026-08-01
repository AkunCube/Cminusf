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
  ArrayType *arrayType = ArrayType::get(i32Type, 10);
  FunctionType *mainFuncType = FunctionType::get(i32Type, {});

  Function *mainFunc = Function::create(mainFuncType, "main", module);
  BasicBlock *bb = BasicBlock::create(module, "entry", mainFunc);
  builder.set_insert_point(bb);

  AllocaInst *retAlloca = builder.create_alloca(i32Type);
  AllocaInst *arrayAlloca = builder.create_alloca(arrayType);

  auto intConst = [&](int num) -> ConstantInt * {
    return ConstantInt::get(num, module);
  };

  (void)builder.create_store(intConst(0), retAlloca);
  GetElementPtrInst *firstElemPtr =
      builder.create_gep(arrayAlloca, {intConst(0), intConst(0)});
  (void)builder.create_store(intConst(10), firstElemPtr);
  LoadInst *firstElem = builder.create_load(firstElemPtr);
  IBinaryInst *doubledValue = builder.create_imul(firstElem, intConst(2));
  GetElementPtrInst *secondElemPtr =
      builder.create_gep(arrayAlloca, {intConst(0), intConst(1)});
  (void)builder.create_store(doubledValue, secondElemPtr);
  LoadInst *secondElem = builder.create_load(secondElemPtr);
  builder.create_ret(secondElem);
}

int main() {
  auto module = std::make_unique<Module>();
  makeFunction(module.get());
  std::cout << module->print();
  return 0;
}
