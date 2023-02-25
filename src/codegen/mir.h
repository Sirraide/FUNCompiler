#ifndef INTERCEPT_MIR_H
#define INTERCEPT_MIR_H

#include <codegen/intermediate_representation.h>

typedef RegisterDescriptor VReg;
#define VREG_MIN ((VReg)1024)
#define VREG_INVALID ((VReg)0)
#define MIR_BACKEND_FIRST (M_INSTRUCTION_COUNT)
#define CREATE_MIR_INSTRUCTION(ctx, ir, function)   \
  MInst *mi = calloc(1, sizeof *mi); \
  insert_mi(ir->parent_block, mi);   \
  mi->vreg = (u32) function->mi_counter++

#define FOREACH_MIR_OP(mi)                             \
  for (                                                \
      MachineOperand                                   \
          *op =  mi->operands[0].kind == M_OP_BUNDLE   \
               ? mi->bundle.data                       \
               : mi->operands,                         \
          *_end_ = mi->operands[0].kind == M_OP_BUNDLE \
               ? mi->bundle.data + mi->bundle.size     \
               : mi->operands + 3;                     \
      op < _end_;                                      \
      op++                                             \
  ) if (op->kind == M_OP_NONE) break; else

enum MIRType {
  M_IMM,
  M_COPY,
  M_CALL,
  M_LOAD,
  M_STORE,
  M_RETURN,
  M_BRANCH,
  M_NOT,
#define F(type, ...) CAT(M_, type),
  ALL_BINARY_INSTRUCTION_TYPES(F)
#undef F
  M_INSTRUCTION_COUNT,
};

enum MIROperandType {
  M_OP_NONE = 0,   ///< Empty slot.
  M_OP_IMM,        ///< Immediate value.
  M_OP_REG,        ///< Register.
  M_OP_FUNC,       ///< Pointer to IR_FUNCTION. The type of this operand is `name`.
  M_OP_STATIC_REF, ///< Pointer to IR_STATIC_REF. The type of this operand is `name`.
  M_OP_BLOCK,      ///< Pointer to IR_BLOCK.
  M_OP_POISON,     ///< Poison value.

  /// This indicates that the three operands together
  /// form a Vector(MachineOperand), with the first
  /// operand being the data, the second the size and
  /// the third the capacity.
  M_OP_BUNDLE,
};

typedef struct MachineOperand {
  enum MIROperandType kind;
  union {
    usz value;
    IRFunction *function;
    IRBlock *block;
    IRStaticVariable *static_ref;
  };
} MachineOperand;

struct MInst {
  int kind;
  VReg vreg;

  /// The operands of this instruction.
  union {
    MachineOperand operands[3];

    /// If we have more than 3 operands,
    /// we need to bundle them.
    struct {
      enum MIROperandType kind1;
      MachineOperand *data;
      enum MIROperandType kind2;
      usz size;
      enum MIROperandType kind3;
      usz capacity;
    } bundle;
  };
};

/// Convert IR to MIR.
void codegen_ir_to_mir(CodegenContext *ctx);

/// Insert a MIR instruction into a block.
/// Should probably never be called manually.
void insert_mi(IRBlock *block, MInst *mi);

#endif // INTERCEPT_MIR_H
