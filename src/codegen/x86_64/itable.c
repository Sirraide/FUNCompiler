#include <codegen/isel.h>
#include <codegen/x86_64/itable.h>

#ifndef _MSC_VER
#  define ENTRY(X) [X] =
#else
#  define ENTRY(X)
#endif

#define arrsize(X) (sizeof(X) / sizeof(0[X]))

ISelPattern ir_immediate_patterns[1] = {
  /// Fallback pattern. Just emit the immediate as is.
  {
    .length = 1,
    .result = 0,
    .instructions_commutative = 1,
    .link = {0},
    .entries = &(ISelPatternEntry) {
      .mir_type = I_MOV,
      .constraints_count = 1,
      .operands_commutative = false,
      .min_operands = 1,
      .max_operands = 1,
      .constraints = &(ISelOperandConstraint){
        .kind = ISEL_OP_KIND_IMMEDIATE,
        .imm = {
          .min = INT64_MIN,
          .max = INT64_MAX,
          .allowed_values_count = 0,
          .values = NULL,
        },
      },
    },
  },
};


//
//
//
//

ISelTable x86_64_isel_table = {
  /// Perhaps at first unintuitively, this is a very
  /// important pattern list, as it is responsible for
  /// inlining immediates.
  ENTRY(IR_IMMEDIATE) {
    .count = arrsize(ir_immediate_patterns),
    .single = 0,
    .double_ = 0,
    .triple = 0,
    .entries = ir_immediate_patterns,
  },

  ENTRY(IR_CALL) {0},
  ENTRY(IR_LOAD) {0},
  ENTRY(IR_RETURN) {0},
  ENTRY(IR_BRANCH) {0},
  ENTRY(IR_BRANCH_CONDITIONAL) {0},
  ENTRY(IR_UNREACHABLE) {0},
  ENTRY(IR_PHI) {0},
  ENTRY(IR_COPY) {0},

  ENTRY(IR_ADD) {0},
  ENTRY(IR_SUB) {0},
  ENTRY(IR_MUL) {0},
  ENTRY(IR_DIV) {0},
  ENTRY(IR_MOD) {0},
  ENTRY(IR_SHL) {0},
  ENTRY(IR_SAR) {0},
  ENTRY(IR_SHR) {0},
  ENTRY(IR_AND) {0},
  ENTRY(IR_OR) {0},
  ENTRY(IR_LT) {0},
  ENTRY(IR_LE) {0},
  ENTRY(IR_GT) {0},
  ENTRY(IR_GE) {0},
  ENTRY(IR_EQ) {0},
  ENTRY(IR_NE) {0},

  ENTRY(IR_STATIC_REF) {0},
  ENTRY(IR_FUNC_REF) {0},
  ENTRY(IR_STORE) {0},
  ENTRY(IR_NOT) {0},
  ENTRY(IR_PARAMETER) {0},
  ENTRY(IR_REGISTER) {0},
  ENTRY(IR_ALLOCA) {0},
  ENTRY(IR_LIT_INTEGER) {0},
  ENTRY(IR_LIT_STRING) {0},
};