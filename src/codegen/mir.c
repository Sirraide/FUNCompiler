#include <codegen/mir.h>
#include <stdlib.h>
#include <codegen/x86_64/arch_x86_64.h>

typedef MachineOperand Op;

#define MI CREATE_MIR_INSTRUCTION(ctx, ir, ir->parent_block->function)
#define MI_NO_VREG CREATE_MIR_INSTRUCTION_VREG(ctx, ir, ir->parent_block->function, VREG_INVALID)

/// Insert a MIR instruction into a block.
void insert_mi(IRBlock *block, MInst *mi) {
  vector_push(block->machine_instructions, mi);
}

/// Convert an IR binary instruction to a MIR instruction.
static int binary_instruction_kind(enum IRType t) {
  switch (t) {
    default: UNREACHABLE();
#define F(type, ...) case CAT(IR_, type): return CAT(M_, type);
  ALL_BINARY_INSTRUCTION_TYPES(F)
#undef F
  }
}

static VReg ir_to_mir(CodegenContext *ctx, IRInstruction *ir);

/// Create a MIR instruction for an IR instruction.
static VReg ir_to_mir_impl(CodegenContext *ctx, IRInstruction *ir, bool increase_refcount) {
  ASSERT(ir->parent_block->function->mi_counter >= VREG_MIN, "Counter must be set to at least VREG_MIN");
  ASSERT(!ir->result, "Cannot lower precoloured IR instruction");
  if (ir->mi) {
    if (increase_refcount) ir->mi->refcount++;
    return ir->mi->vreg;
  }

  STATIC_ASSERT(IR_COUNT == 34, "Handle all IR instructions");
  switch (ir->kind) {
    case IR_ALLOCA:
    case IR_COUNT:
    case IR_PARAMETER:
    case IR_LIT_INTEGER:
    case IR_LIT_STRING:
    default:
      UNREACHABLE();

    case IR_PHI: return ir->phi.vreg;

    case IR_IMMEDIATE: {
      MI;
      mi->kind = M_IMM;
      mi->operands[0] = (Op){.kind = M_OP_IMM, .value = ir->imm};
    } break;

    case IR_CALL: {
      MI_NO_VREG;
      mi->kind = M_CALL;
      Op callee = {0};

      /// This only has a result if the callee returns non-void.
      if (!type_is_void(ir->type)) mi->vreg = (u32) ir->parent_block->function->mi_counter++;

      /// Indirect call.
      if (ir->call.is_indirect) {
        callee.kind = M_OP_REG;
        callee.value = ir_to_mir(ctx, ir->call.callee_instruction);
      }

      /// Direct call.
      else {
        callee.kind = M_OP_FUNC;
        callee.function = ir->call.callee_function;
      }

      /// If there are more than two arguments, we need to bundle them.
      if (ir->call.arguments.size > 2) {
        vector_push(mi->bundle, callee);
        foreach_ptr (IRInstruction *, arg, ir->call.arguments) {
          Op mi_arg = {0};
          mi_arg.kind = M_OP_REG;
          mi_arg.value = ir_to_mir(ctx, arg);
          vector_push(mi->bundle, mi_arg);
        }
      }

      /// Otherwise, we can just use the operands array.
      else {
        mi->operands[0] = callee;
        if (ir->call.arguments.size) mi->operands[1] = (Op){.kind = M_OP_REG, .value = ir_to_mir(ctx, ir->call.arguments.data[0])};
        if (ir->call.arguments.size > 1) mi->operands[2] = (Op){.kind = M_OP_REG, .value = ir_to_mir(ctx, ir->call.arguments.data[1])};
      }
    } break;

    case IR_LOAD: {
      MI;
      mi->kind = M_LOAD;
      mi->operands[0] = (Op){.kind = M_OP_REG, .value = ir_to_mir(ctx, ir->operand)};
    } break;

    case IR_RETURN: {
      MI_NO_VREG;
      mi->kind = M_RETURN;
      if (ir->operand) mi->operands[0] = (Op){.kind = M_OP_REG, .value = ir_to_mir(ctx, ir->operand)};
    } break;

    case IR_BRANCH: {
      MI_NO_VREG;
      mi->kind = M_BRANCH;
      mi->operands[0] = (Op){.kind = M_OP_BLOCK, .block = ir->destination_block};
    } break;

    case IR_BRANCH_CONDITIONAL: {
      MI_NO_VREG;
      mi->kind = M_BRANCH;
      mi->operands[0] = (Op){.kind = M_OP_REG, .value = ir_to_mir(ctx, ir->cond_br.condition)};
      mi->operands[1] = (Op){.kind = M_OP_BLOCK, .block = ir->cond_br.then};
      mi->operands[2] = (Op){.kind = M_OP_BLOCK, .block = ir->cond_br.else_};
    } break;

    case IR_COPY: {
      MInst *mi = calloc(1, sizeof *mi);
      insert_mi(ir->parent_block, mi);
      ir->mi = mi;

      /// If this copy is used by a PHI, reuse its vreg.
      IRInstruction **phi;
      vector_find_if(ir->users, phi, i, ir->users.data[i]->kind == IR_PHI);
      mi->vreg = phi ? (*phi)->phi.vreg : (u32) ir->parent_block->function->mi_counter++;

      /// Set the operand.
      mi->kind = M_COPY;
      mi->operands[0] = (Op){.kind = M_OP_REG, .value = ir_to_mir(ctx, ir->operand)};
    } break;

    ALL_BINARY_INSTRUCTION_CASES() {
      MI;
      mi->kind = binary_instruction_kind(ir->kind);
      mi->operands[0] = (Op){.kind = M_OP_REG, .value = ir_to_mir(ctx, ir->lhs)};
      mi->operands[1] = (Op){.kind = M_OP_REG, .value = ir_to_mir(ctx, ir->rhs)};
    } break;

    case IR_STATIC_REF: {
      MI;
      mi->kind = M_COPY;
      mi->operands[0] = (Op){.kind = M_OP_STATIC_REF, .static_ref = ir->static_ref};
    } break;

    case IR_FUNC_REF: {
      MI;
      mi->kind = M_COPY;
      mi->operands[0] = (Op){.kind = M_OP_FUNC, .function = ir->function_ref};
    } break;

    case IR_STORE: {
      MI_NO_VREG;
      mi->kind = M_STORE;
      mi->operands[0] = (Op){.kind = M_OP_REG, .value = ir_to_mir(ctx, ir->store.addr)};
      mi->operands[1] = (Op){.kind = M_OP_REG, .value = ir_to_mir(ctx, ir->store.value)};
    } break;

    case IR_NOT: {
      MI;
      mi->kind = M_NOT;
      mi->operands[0] = (Op){.kind = M_OP_REG, .value = ir_to_mir(ctx, ir->operand)};
    } break;

    case IR_REGISTER: return ir->result;
    case IR_UNREACHABLE: return VREG_INVALID;
  }

  return ir->mi->vreg;
}

static VReg ir_to_mir(CodegenContext *ctx, IRInstruction *ir) {
  return ir_to_mir_impl(ctx, ir, true);
}

void codegen_ir_to_mir(CodegenContext *ctx) {
  FOREACH_INSTRUCTION(ctx)
    ir_to_mir_impl(ctx, instruction, false);
}

static void print_vreg(VReg vreg) {
  if (vreg >= VREG_MIN) print("%34%%v%u", (unsigned) (vreg - VREG_MIN));
  else print("%31%%r%d", (int) vreg);
}

void mir_print_instruction(CodegenContext *ctx, MInst *mi) {
  print("    ");
  /// Print the vreg if there is one.
  if (mi->vreg) {
    print_vreg(mi->vreg);
    print("%31(%35%Z%31) = ", mi->refcount);
  }

  /// Print the instruction name if we know it.
  print("%33");
  switch (mi->kind) {
    case M_IMM: print("M_IMM"); break;
    case M_COPY: print("M_COPY"); break;
    case M_CALL: print("M_CALL"); break;
    case M_LOAD: print("M_LOAD"); break;
    case M_STORE: print("M_STORE"); break;
    case M_RETURN: print("M_RETURN"); break;
    case M_BRANCH: print("M_BRANCH"); break;
    case M_NOT: print("M_NOT"); break;
#   define F(type, ...) case CAT(M_, type): print("M_" #type); break;
    ALL_BINARY_INSTRUCTION_TYPES(F)
#   undef F

    default:
      if (ctx->format == CG_FMT_x86_64_GAS && x86_64_print_mir_instruction(ctx, mi)) break;
      print("MI%31(%33%d%31)", mi->kind);
      break;
  }

  /// Print the operands.
  bool first = true;
  FOREACH_MIR_OP (mi) {
    if (first) {
      print(" ");
      first = false;
    } else print("%31, ");

    switch (op->kind) {
      default: UNREACHABLE();
      case M_OP_REG: print("%36reg "); print_vreg((VReg) op->value); break;
      case M_OP_IMM: print("%36imm %35%D", (i64) op->value); break;
      case M_OP_BLOCK: print("%36block %33bb%Z", op->block->id); break;
      case M_OP_STATIC_REF: print("%36name %38%S", op->static_ref->name); break;
      case M_OP_FUNC: print("%36name %32%S", op->function->name); break;
      case M_OP_POISON: print("%36poison"); break;
    }
  }
  print("\n");
}

void mir_print(CodegenContext *ctx) {
  ir_set_ids(ctx);

  if (debug_ir) print("\n%31========== MIR ==========\n");

  bool first_func = true;
  foreach_ptr (IRFunction *, f, ctx->functions) {
    if (first_func) first_func = false;
    else print("\n");

    print("%31defun %32%S%31 {\n", f->name);
    print("    %31.stacksize %35%Z\n", f->locals_total_size);
    list_foreach (IRBlock *, b, f->blocks) {
      print("%33bb%Z%31:\n", b->id);
      foreach_ptr (MInst *, mi, b->machine_instructions) {
        mir_print_instruction(ctx, mi);
      }
    }
    print("%31}%m\n");
  }
}