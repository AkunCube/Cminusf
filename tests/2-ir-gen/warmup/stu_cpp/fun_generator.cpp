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

  std::vector<Type *> calleeArgTypes = {i32Type};
  FunctionType *calleeFuncType = FunctionType::get(i32Type, calleeArgTypes);
  Function *calleeFunc = Function::create(calleeFuncType, "callee", module);

  BasicBlock *calleeBB = BasicBlock::create(module, "entry", calleeFunc);
  builder.set_insert_point(calleeBB);

  // Store function args.
  std::vector<Value *> storedArgs;
  for (auto &arg : calleeFunc->get_args()) {
    AllocaInst *argAlloca = builder.create_alloca(arg.get_type());
    storedArgs.push_back(argAlloca);
    builder.create_store(&arg, argAlloca);
  }

  auto intConst = [&](int num) -> ConstantInt * {
    return ConstantInt::get(num, module);
  };

  LoadInst *argLoad = builder.create_load(storedArgs[0]);
  IBinaryInst *mulResult = builder.create_imul(intConst(2), argLoad);

  builder.create_ret(mulResult);

  FunctionType *mainFuncType = FunctionType::get(i32Type, {});
  Function *mainFunc = Function::create(mainFuncType, "main", module);
  BasicBlock *mainBB = BasicBlock::create(module, "entry", mainFunc);
  builder.set_insert_point(mainBB);

  AllocaInst *retAlloca = builder.create_alloca(i32Type);
  builder.create_store(intConst(0), retAlloca);
  CallInst *callResult = builder.create_call(calleeFunc, {intConst(110)});
  builder.create_ret(callResult);
}

int main() {
  auto module = std::make_unique<Module>();
  makeFunction(module.get());
  std::cout << module->print();
  return 0;
}
