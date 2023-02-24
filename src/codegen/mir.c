#include <codegen/mir.h>
#include <stdlib.h>

typedef MachineOperand Op;

#define MI \
  MInst *mi = calloc(1, sizeof *mi); \
  insert_mi(ir->parent_block, mi);    \
  mi->vreg = (u32) ctx->counter++

/// Insert a MIR instruction into a block.
static void insert_mi(IRBlock *block, MInst *mi) {
  TODO();
}

/// Convert an IR binary instruction to a MIR instruction.
static enum MIRType binary_instruction_kind(enum IRType t) {
  switch (t) {
    default: UNREACHABLE();
#define F(type, ...) case CAT(IR_, type): return CAT(M_, type);
  ALL_BINARY_INSTRUCTION_TYPES(F)
#undef F
  }
}

/// Create a MIR instruction for an IR instruction.
VReg ir_to_mir(CodegenContext *ctx, IRInstruction *ir) {
  ASSERT(ctx->counter, "Counter must be set to a value greater than VREG_MIN");
  ASSERT(!ir->result, "Cannot lower precoloured IR instruction");
  if (ir->mi) return ir->mi->vreg;

  /// Todo before this:
  ///     - lower phis (replace w/ copies and set vreg).
  ///     - lower and replace parameters.
  ///     - lower and replace allocas.
  ///     - lower returns (as in, branch to ret block).

  STATIC_ASSERT(IR_COUNT == 34, "Handle all IR instructions");
  switch (ir->kind) {
    case IR_ALLOCA:
    case IR_COUNT:
    case IR_PARAMETER:
    case IR_PHI:
    case IR_LIT_INTEGER:
    case IR_LIT_STRING:
    default:
      UNREACHABLE();

    /// Immediate value.
    case IR_IMMEDIATE: {
      MI;
      mi->kind = M_IMM;
      mi->operands[0] = (Op){.kind = M_OP_IMM, .value = ir->imm};
      mi->vreg = (u32) ctx->counter++;
    } break;

    /// Function call.
    case IR_CALL: {
      MI;
      mi->kind = M_CALL;
      Op callee = {0};

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
    } break;

    case IR_LOAD: {
      MI;
      mi->kind = M_LOAD;
      mi->operands[0] = (Op){.kind = M_OP_REG, .value = ir_to_mir(ctx, ir->operand)};
    } break;

    case IR_RETURN: {
      MI;
      mi->kind = M_RETURN;
      if (ir->operand) mi->operands[0] = (Op){.kind = M_OP_REG, .value = ir_to_mir(ctx, ir->operand)};
    } break;

    case IR_BRANCH: {
      MI;
      mi->kind = M_BRANCH;
      mi->operands[0] = (Op){.kind = M_OP_BLOCK, .block = ir->destination_block};
    } break;

    case IR_BRANCH_CONDITIONAL: {
      MI;
      mi->kind = M_BRANCH;
      mi->operands[0] = (Op){.kind = M_OP_REG, .value = ir_to_mir(ctx, ir->cond_br.condition)};
      mi->operands[1] = (Op){.kind = M_OP_BLOCK, .block = ir->cond_br.then};
      mi->operands[2] = (Op){.kind = M_OP_BLOCK, .block = ir->cond_br.else_};
    } break;

    case IR_UNREACHABLE: return VREG_INVALID;
    case IR_REGISTER: return ir->result;

    case IR_COPY: {
      MI;
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
      MI;
      mi->kind = M_STORE;
      mi->operands[0] = (Op){.kind = M_OP_REG, .value = ir_to_mir(ctx, ir->store.addr)};
      mi->operands[1] = (Op){.kind = M_OP_REG, .value = ir_to_mir(ctx, ir->store.value)};
    } break;

    case IR_NOT: {
      MI;
      mi->kind = M_NOT;
      mi->operands[0] = (Op){.kind = M_OP_REG, .value = ir_to_mir(ctx, ir->operand)};
    } break;
  }

  return ir->mi->vreg;
}