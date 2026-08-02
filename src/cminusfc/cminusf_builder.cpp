#include "cminusf_builder.hpp"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/ErrorHandling.h"

#define CONST_FP(num) ConstantFP::get((float)num, module.get())
#define CONST_INT(num) ConstantInt::get(num, module.get())

static std::string block_name(const std::string &prefix, uint64_t id) {
  return (llvm::Twine(prefix) + llvm::Twine(id)).str();
}

// types
Type *VOID_T;
Type *INT1_T;
Type *INT32_T;
Type *INT32PTR_T;
Type *FLOAT_T;
Type *FLOATPTR_T;

/*
 * use CMinusfBuilder::Scope to construct scopes
 * scope.enter: enter a new scope
 * scope.exit: exit current scope
 * scope.push: add a new binding to current scope
 * scope.find: find and return the value bound to the name
 */

Value *CminusfBuilder::visit(ASTProgram &node) {
  VOID_T = module->get_void_type();
  INT1_T = module->get_int1_type();
  INT32_T = module->get_int32_type();
  INT32PTR_T = module->get_int32_ptr_type();
  FLOAT_T = module->get_float_type();
  FLOATPTR_T = module->get_float_ptr_type();

  Value *ret_val = nullptr;
  for (auto &decl : node.declarations) {
    ret_val = decl->accept(*this);
  }
  return ret_val;
}

Value *CminusfBuilder::visit(ASTNum &node) {
  if (node.type == TYPE_INT) {
    return CONST_INT(node.i_val);
  }
  if (node.type == TYPE_FLOAT) {
    return CONST_FP(node.f_val);
  }
  llvm_unreachable("Invalid ASTNum type");
}

Value *CminusfBuilder::visit(ASTVarDeclaration &node) {
  Type *elem_type = (node.type == TYPE_INT) ? INT32_T : FLOAT_T;
  Type *var_type = elem_type;
  if (node.num) {
    var_type = module->get_array_type(elem_type, node.num->i_val);
  }

  if (scope.in_global()) {
    auto *initializer = ConstantZero::get(elem_type, module.get());
    auto *global = GlobalVariable::create(node.id, module.get(), var_type,
                                          false, initializer);
    scope.push(node.id, global);
    return global;
  }

  auto *alloca = builder->create_alloca(var_type);
  scope.push(node.id, alloca);
  return alloca;
}

Value *CminusfBuilder::visit(ASTFunDeclaration &node) {
  FunctionType *fun_type;
  Type *ret_type;
  std::vector<Type *> param_types;
  if (node.type == TYPE_INT)
    ret_type = INT32_T;
  else if (node.type == TYPE_FLOAT)
    ret_type = FLOAT_T;
  else
    ret_type = VOID_T;

  for (auto &param : node.params) {
    Type *param_type = nullptr;
    if (param->type == TYPE_INT) {
      param_type = param->isarray ? INT32PTR_T : INT32_T;
    } else if (param->type == TYPE_FLOAT) {
      param_type = param->isarray ? FLOATPTR_T : FLOAT_T;
    } else {
      llvm_unreachable("Invalid parameter type");
    }
    param_types.push_back(param_type);
  }

  fun_type = FunctionType::get(ret_type, param_types);
  auto func = Function::create(fun_type, node.id, module.get());
  scope.push(node.id, func);
  context.func = func;
  auto funBB = BasicBlock::create(module.get(), "entry", func);
  builder->set_insert_point(funBB);
  scope.enter();
  std::vector<Value *> args;
  for (auto &arg : func->get_args()) {
    args.push_back(&arg);
  }

  for (const auto &[param, arg] : llvm::zip(node.params, args)) {
    auto *arg_alloca = builder->create_alloca(arg->get_type());
    builder->create_store(arg, arg_alloca);
    scope.push(param->id, arg_alloca);
  }

  node.compound_stmt->accept(*this);
  if (not builder->get_insert_block()->is_terminated()) {
    if (context.func->get_return_type()->is_void_type())
      builder->create_void_ret();
    else if (context.func->get_return_type()->is_float_type())
      builder->create_ret(CONST_FP(0.));
    else
      builder->create_ret(CONST_INT(0));
  }
  scope.exit();
  return nullptr;
}

Value *CminusfBuilder::visit(ASTParam &node) { return nullptr; }

Value *CminusfBuilder::visit(ASTCompoundStmt &node) {
  scope.enter();
  for (auto &decl : node.local_declarations) {
    decl->accept(*this);
  }

  for (auto &stmt : node.statement_list) {
    stmt->accept(*this);
    if (builder->get_insert_block()->is_terminated())
      break;
  }
  scope.exit();
  return nullptr;
}

Value *CminusfBuilder::visit(ASTExpressionStmt &node) {
  node.expression->accept(*this);
  return nullptr;
}

Value *CminusfBuilder::visit(ASTSelectionStmt &node) {
  Value *cond = prepare_condition(node.expression->accept(*this));

  uint64_t id = context.block_counter++;
  auto *then_block =
      BasicBlock::create(module.get(), block_name("then", id), context.func);
  auto *else_block =
      BasicBlock::create(module.get(), block_name("else", id), context.func);
  auto *end_block =
      BasicBlock::create(module.get(), block_name("end", id), context.func);

  builder->create_cond_br(cond, then_block, else_block);

  builder->set_insert_point(then_block);
  node.if_statement->accept(*this);
  if (!builder->get_insert_block()->is_terminated()) {
    builder->create_br(end_block);
  }

  builder->set_insert_point(else_block);
  if (node.else_statement) {
    node.else_statement->accept(*this);
  }
  if (!builder->get_insert_block()->is_terminated()) {
    builder->create_br(end_block);
  }

  builder->set_insert_point(end_block);
  return nullptr;
}

Value *CminusfBuilder::visit(ASTIterationStmt &node) {
  uint64_t id = context.block_counter++;
  auto *entry_block = BasicBlock::create(
      module.get(), block_name("while_entry", id), context.func);
  auto *body_block = BasicBlock::create(
      module.get(), block_name("while_body", id), context.func);
  auto *end_block = BasicBlock::create(
      module.get(), block_name("while_end", id), context.func);

  builder->create_br(entry_block);

  builder->set_insert_point(entry_block);
  Value *cond = prepare_condition(node.expression->accept(*this));
  builder->create_cond_br(cond, body_block, end_block);

  builder->set_insert_point(body_block);
  node.statement->accept(*this);
  if (!builder->get_insert_block()->is_terminated()) {
    builder->create_br(entry_block);
  }

  builder->set_insert_point(end_block);
  return nullptr;
}

Value *CminusfBuilder::visit(ASTReturnStmt &node) {
  if (node.expression == nullptr) {
    builder->create_void_ret();
    return nullptr;
  }

  Value *ret_val = load_if_scalar(node.expression->accept(*this));
  Type *ret_type = context.func->get_return_type();
  ret_val = cast_value_to_type(ret_val, ret_type);
  builder->create_ret(ret_val);
  return nullptr;
}

Value *CminusfBuilder::visit(ASTVar &node) {
  Value *var = scope.find(node.id);
  assert(var->get_type()->is_pointer_type() && "var must be pointer type");

  if (!node.expression) {
    return var;
  }

  Type *element_type = var->get_type()->get_pointer_element_type();
  assert((element_type->is_array_type() || element_type->is_pointer_type()) &&
         "array subscript applied to non-array or non-pointer variable");

  Value *idx = cast_value_to_type(
      load_if_scalar(node.expression->accept(*this)), INT32_T);
  Value *zero = CONST_INT(0);
  Value *cmp = builder->create_icmp_lt(idx, zero);

  // Check if index is negative, call neg_idx_except if true.
  uint64_t id = context.block_counter++;
  auto *neg_block = BasicBlock::create(
      module.get(), block_name("neg_idx_abort", id), context.func);
  auto *cont_block = BasicBlock::create(
      module.get(), block_name("neg_idx_cont", id), context.func);
  builder->create_cond_br(cmp, neg_block, cont_block);

  builder->set_insert_point(neg_block);
  auto *except_func = static_cast<Function *>(scope.find("neg_idx_except"));
  builder->create_call(except_func, {});
  builder->create_br(cont_block);

  builder->set_insert_point(cont_block);
  if (element_type->is_pointer_type()) {
    var = builder->create_load(var);
    return builder->create_gep(var, {idx});
  }
  return builder->create_gep(var, {ConstantInt::get(0, module.get()), idx});
}

Value *CminusfBuilder::visit(ASTAssignExpression &node) {
  Value *ptr = node.var->accept(*this);
  Value *val = load_if_scalar(node.expression->accept(*this));
  Type *ptr_type = ptr->get_type()->get_pointer_element_type();
  val = cast_value_to_type(val, ptr_type);
  builder->create_store(val, ptr);
  return val;
}

Value *CminusfBuilder::visit(ASTSimpleExpression &node) {
  Value *l_val = node.additive_expression_l->accept(*this);

  if (!node.additive_expression_r) {
    return l_val;
  }

  Value *r_val = node.additive_expression_r->accept(*this);
  promote_operands(l_val, r_val);

  if (l_val->get_type()->is_float_type()) {
    switch (node.op) {
      case OP_LE:
        return builder->create_fcmp_le(l_val, r_val);
      case OP_LT:
        return builder->create_fcmp_lt(l_val, r_val);
      case OP_GT:
        return builder->create_fcmp_gt(l_val, r_val);
      case OP_GE:
        return builder->create_fcmp_ge(l_val, r_val);
      case OP_EQ:
        return builder->create_fcmp_eq(l_val, r_val);
      case OP_NEQ:
        return builder->create_fcmp_ne(l_val, r_val);
    }
  }

  if (l_val->get_type()->is_int32_type()) {
    switch (node.op) {
      case OP_LE:
        return builder->create_icmp_le(l_val, r_val);
      case OP_LT:
        return builder->create_icmp_lt(l_val, r_val);
      case OP_GT:
        return builder->create_icmp_gt(l_val, r_val);
      case OP_GE:
        return builder->create_icmp_ge(l_val, r_val);
      case OP_EQ:
        return builder->create_icmp_eq(l_val, r_val);
      case OP_NEQ:
        return builder->create_icmp_ne(l_val, r_val);
    }
  }

  llvm_unreachable("Invalid relational operator");
}

Value *CminusfBuilder::visit(ASTAdditiveExpression &node) {
  if (!node.additive_expression) {
    return node.term->accept(*this);
  }

  Value *l_val = node.additive_expression->accept(*this);
  Value *r_val = node.term->accept(*this);
  promote_operands(l_val, r_val);

  if (l_val->get_type()->is_float_type()) {
    switch (node.op) {
      case OP_PLUS:
        return builder->create_fadd(l_val, r_val);
      case OP_MINUS:
        return builder->create_fsub(l_val, r_val);
    }
  }

  if (l_val->get_type()->is_int32_type()) {
    switch (node.op) {
      case OP_PLUS:
        return builder->create_iadd(l_val, r_val);
      case OP_MINUS:
        return builder->create_isub(l_val, r_val);
    }
  }

  llvm_unreachable("Invalid additive operator");
}

Value *CminusfBuilder::visit(ASTTerm &node) {
  if (!node.term) {
    return node.factor->accept(*this);
  }

  Value *l_val = node.term->accept(*this);
  Value *r_val = node.factor->accept(*this);
  promote_operands(l_val, r_val);

  if (l_val->get_type()->is_float_type()) {
    switch (node.op) {
      case OP_MUL:
        return builder->create_fmul(l_val, r_val);
      case OP_DIV:
        return builder->create_fdiv(l_val, r_val);
    }
  }

  if (l_val->get_type()->is_int32_type()) {
    switch (node.op) {
      case OP_MUL:
        return builder->create_imul(l_val, r_val);
      case OP_DIV:
        return builder->create_isdiv(l_val, r_val);
    }
  }

  llvm_unreachable("Invalid multiplicative operator");
}

Value *CminusfBuilder::visit(ASTCall &node) {
  Value *callee = scope.find(node.id);
  assert(callee->get_type()->is_function_type() &&
         "callee must be function type");

  auto *callee_type = static_cast<FunctionType *>(callee->get_type());
  assert(callee_type->get_num_of_args() == node.args.size() &&
         "argument count mismatch");

  std::vector<Value *> args;
  args.reserve(node.args.size());

  for (auto pair : llvm::enumerate(node.args)) {
    size_t i = pair.index();
    auto &arg = pair.value();
    Type *param_type = callee_type->get_param_type(i);
    Value *arg_val = load_or_decay(arg->accept(*this));
    args.push_back(cast_value_to_type(arg_val, param_type));
  }

  return builder->create_call(callee, args);
}

void CminusfBuilder::promote_operands(Value *&lhs, Value *&rhs) {
  lhs = load_if_scalar(lhs);
  rhs = load_if_scalar(rhs);
  Type *lhs_type = lhs->get_type();
  Type *rhs_type = rhs->get_type();

  if (lhs_type->is_float_type()) {
    rhs = cast_value_to_type(rhs, FLOAT_T);
    return;
  }

  if (rhs_type->is_float_type()) {
    lhs = cast_value_to_type(lhs, FLOAT_T);
    return;
  }

  if (lhs_type->is_integer_type() && rhs_type->is_integer_type()) {
    lhs = cast_value_to_type(lhs, INT32_T);
    rhs = cast_value_to_type(rhs, INT32_T);
    return;
  }

  llvm_unreachable("Invalid operand types in promote_operands");
}

Value *CminusfBuilder::cast_value_to_type(Value *val, Type *target_type) {
  Type *val_type = val->get_type();
  if (val_type == target_type) {
    return val;
  }

  if (target_type->is_float_type() && val_type->is_integer_type()) {
    return builder->create_sitofp(val, target_type);
  }

  if (target_type->is_integer_type() && val_type->is_float_type()) {
    return builder->create_fptosi(val, target_type);
  }

  if (target_type->is_integer_type() && val_type->is_integer_type()) {
    auto *src_int = static_cast<IntegerType *>(val_type);
    auto *dst_int = static_cast<IntegerType *>(target_type);
    if (src_int->get_num_bits() < dst_int->get_num_bits()) {
      return builder->create_zext(val, target_type);
    }
  }

  llvm_unreachable("Unsupported type conversion");
}

Value *CminusfBuilder::load_or_decay(Value *val) {
  if (!val->get_type()->is_pointer_type()) {
    return val;
  }

  Type *element_type = val->get_type()->get_pointer_element_type();

  // Array decays to pointer to first element.
  if (element_type->is_array_type()) {
    return builder->create_gep(val, {CONST_INT(0), CONST_INT(0)});
  }

  // Otherwise, load the value.
  return builder->create_load(val);
}

Value *CminusfBuilder::prepare_condition(Value *cond) {
  cond = load_if_scalar(cond);
  Type *condType = cond->get_type();
  if (condType->is_int1_type()) {
    return cond;
  }

  if (condType->is_int32_type()) {
    return builder->create_icmp_ne(cond, CONST_INT(0));
  }

  if (condType->is_float_type()) {
    return builder->create_fcmp_ne(cond, CONST_FP(0.0f));
  }

  llvm_unreachable("Invalid condition type");
}

Value *CminusfBuilder::load_if_scalar(Value *val) {
  if (!val->get_type()->is_pointer_type()) {
    return val;
  }

  Type *elem_type = val->get_type()->get_pointer_element_type();
  assert(!elem_type->is_array_type() &&
         "load_if_scalar: cannot handle array type");
  return builder->create_load(val);
}
