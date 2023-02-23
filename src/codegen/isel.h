#ifndef INTERCEPT_ISel_H
#define INTERCEPT_ISel_H

#include <codegen/codegen_forward.h>
#include <codegen/intermediate_representation.h>

enum ISelOperandConstraintKind {
  /// Match any operand.
  ISEL_OP_KIND_ANY,

  /// Match a register.
  ISEL_OP_KIND_REGISTER,

  /// Match an immediate.
  ISEL_OP_KIND_IMMEDIATE,
};

/// Constraint on an operand.
typedef struct {
  /// The kind of constraint.
  enum ISelOperandConstraintKind kind;

  /// The constraint.
  union {
    struct {
      /// Register class. Zero means any register class.
      u32 rclass;

      /// Register. Zero means any register.
      RegisterDescriptor rd;
    } reg;

    struct {
      /// Range of allowed values.
      i64 min;
      i64 max;

      /// Allowed values. If this is NULL, use the range instead.
      i64 allowed_values_count;
      i64 *values;
    } imm;
  };
} ISelOperandConstraint;

/// Pattern entry. This is what matches a single instruction.
typedef struct ISelPatternEntry {
  /// The Machine Instruction type produced by this pattern.
  u32 mir_type;

  /// Number of constraints.
  u16 constraints_count;

  /// Whether this pattern is commutative.
  bool operands_commutative;

  /// Number of operands to match.
  ///
  /// Note: If an instruction has more than 65535 operands,
  /// then there’s something wrong with the architecture
  /// in question candidly...
  u16 min_operands;
  u16 max_operands;

  /// Operand patterns.
  ISelOperandConstraint *constraints;
} ISelPatternEntry;

/// Instruction selection pattern. This matches
/// a sequence of instructions.
typedef struct ISelPattern {
  /// Number of instructions in the pattern.
  usz length;

  /// The result register of this pattern. 0 means any register.
  RegisterDescriptor result;

  /// Whether this pattern is commutative wrt instructions.
  bool instructions_commutative;

  /// Pattern to jump to if this pattern doesn’t match;
  /// NULL means reject. This is used so we don’t end
  /// up matching the same instruction over and over again.
  ///
  /// For instance, if a pattern requires 2 instructions,
  /// but only the first one matches, there might be another
  /// pattern that we know matches the first instruction,
  /// so we can jump to that pattern instead of just trying
  /// to match the next one in the list.
  ///
  /// The first element is the pattern to jump to if only
  /// the first instruction matches, the second element
  /// is the pattern to jump to if the first two instructions
  /// match.
  struct ISelPattern *link[2];

  /// Pattern entries.
  ISelPatternEntry *entries;
} ISelPattern;

/// Table for instruction selection.
typedef struct ISelList {
    /// Number of entries.
    u32 count;

    /// Index of the first pattern that matches 1 instruction.
    u32 single;

    /// Index of the first pattern that matches 2 instructions.
    u32 double_;

    /// Index of the first pattern that matches 3 instructions.
    u32 triple;

    /// Patterns, sorted by length in descending order.
    ISelPattern *entries;
} ISelList, ISelTable[IR_COUNT];

/// Instruction selection tables.
extern ISelTable x86_64_isel_table;

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


#endif // INTERCEPT_ISel_H
