#ifndef INTERCEPT_ISel_H
#define INTERCEPT_ISel_H

#include <codegen/codegen_forward.h>
#include <codegen/intermediate_representation.h>

typedef usz ISelIOName, ISelIName, ISelOName, ISelRegister;

enum ISelFilterOperandKind {
  ISEL_FILTER_OPERAND_INAME, ///< Reference to previous instruction.
  ISEL_FILTER_OPERAND_ONAME, ///< Reference to operand of instructions.
  ISEL_FILTER_OPERAND_REST,  ///< Any remaining operands.
};

enum ISelFilterOperandType {
  ISEL_FILTER_OPERAND_TYPE_ANY,
  ISEL_FILTER_OPERAND_TYPE_REG,
  ISEL_FILTER_OPERAND_TYPE_IMM,
  ISEL_FILTER_OPERAND_TYPE_NAME,
  ISEL_FILTER_OPERAND_TYPE_BLOCK,
};

enum ISelConstraintKind {
  ISEL_CONSTRAINT_ANY,
  ISEL_CONSTRAINT_EQ,
  ISEL_CONSTRAINT_NE,
  ISEL_CONSTRAINT_LT,
  ISEL_CONSTRAINT_LE,
  ISEL_CONSTRAINT_GT,
  ISEL_CONSTRAINT_GE,
};

enum ISelParameterKind {
  ISEL_PARAMETER_REGISTER,
  ISEL_PARAMETER_IMMEDIATE,
  ISEL_PARAMETER_INAME,
  ISEL_PARAMETER_ONAME,

  /// Only used for emit operands.
  ISEL_PARAMETER_RESULT,
};

enum ISelResultKind {
  ISEL_RESULT_NONE,
  ISEL_RESULT_ALLOC,
  ISEL_RESULT_REGISTER,
  ISEL_RESULT_ONAME,
  ISEL_RESULT_ANY
};

typedef struct ISelConstraintParameter {
  enum ISelParameterKind kind;
  usz value;
} ISelConstraintParameter;

typedef struct ISelFilterOperand {
  enum ISelFilterOperandKind kind;
  enum ISelFilterOperandType type;
  enum ISelConstraintKind constraint;
  ISelIOName name; ///< iname or oname.
  Vector(ISelConstraintParameter) constraint_parameters;
} ISelFilterOperand;

typedef struct ISelFilter {
  ISelIName iname;
  usz instruction;
  Vector(ISelFilterOperand) operands;
  bool commutative;
} ISelFilter;

typedef struct ISelEmitOperand {
  enum ISelParameterKind kind;
  usz value;
} ISelEmitOperand;

typedef struct ISelEmit {
  usz instruction;
  Vector(ISelEmitOperand) operands;
} ISelEmit;

typedef Vector(ISelFilter) ISelFilters;

typedef Vector(ISelEmit) ISelEmits;

typedef struct ISelPattern {
  usz icount;

  enum ISelResultKind result_kind;
  ISelRegister result;

  ISelFilters filters;
  ISelEmits emits;
  Vector(ISelRegister) clobbers;
} ISelPattern;

typedef struct ISelTable {
  Vector(ISelPattern) patterns;
  Vector(string) instruction_names;
  Vector(string) register_names;
} ISelTable;

/// Instruction selector entry point.
///
/// This is a table-based instruction selector that attempts
/// to match sequences of instructions and emits register
/// transfers.
///
/// A register transfer is a machine instruction that has
/// input registers, output registers, as well as a list
/// of clobbers.
///
/// \param context The codegen context.
/// \param table The instruction selection table to use.
void isel(CodegenContext *context, ISelTable table);

/// Free an instruction table.
void isel_table_free(ISelTable *table);

/// Parse an instruction table.
///
/// \param filename The name of the file to parse.
/// \param data The file contents.
/// \return The parsed table, or NULL if there was an error.
NODISCARD ISelTable *isel_table_parse(span filename, span data);

#endif // INTERCEPT_ISel_H
