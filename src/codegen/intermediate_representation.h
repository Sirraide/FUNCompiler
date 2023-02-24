#ifndef INTERMEDIATE_REPRESENTATION_H
#define INTERMEDIATE_REPRESENTATION_H

#include <codegen.h>
#include <codegen/codegen_forward.h>
#include <inttypes.h>
#include <stdbool.h>
#include <vector.h>

#define INSTRUCTION(name, given_type)                       \
  IRInstruction *(name) = calloc(1, sizeof(IRInstruction)); \
  (name)->kind = (given_type);                              \
  (name)->type = t_void

#define FOREACH_INSTRUCTION_N(context, function, block, instruction) \
  foreach_ptr (IRFunction *, function, context->functions)           \
  list_foreach (IRBlock *, block, function->blocks)                   \
      list_foreach (IRInstruction *, instruction, block->instructions)

#define FOREACH_INSTRUCTION_IN_FUNCTION_N(function, block, instruction) \
  list_foreach (IRBlock *, block, function->blocks)                      \
      list_foreach (IRInstruction *, instruction, block->instructions)

#define FOREACH_INSTRUCTION(context) FOREACH_INSTRUCTION_N(context, function, block, instruction)
#define FOREACH_INSTRUCTION_IN_FUNCTION(function) FOREACH_INSTRUCTION_IN_FUNCTION_N(function, block, instruction)

/// All instructions that take two arguments.
#define ALL_BINARY_INSTRUCTION_TYPES(F) \
  F(ADD, add)                           \
  F(SUB, sub)                           \
  F(MUL, mul)                           \
  F(DIV, div)                           \
  F(MOD, mod)                           \
                                        \
  F(SHL, shl)                           \
  F(SAR, sar)                           \
  F(SHR, shr)                           \
  F(AND, and)                           \
  F(OR, or)                             \
                                        \
  F(LT, lt)                             \
  F(LE, le)                             \
  F(GT, gt)                             \
  F(GE, ge)                             \
  F(EQ, eq)                             \
  F(NE, ne)

/// Some of these are also used in the parser, and until C implements
/// inheriting from enums (i.e. never), this is the best we can do.
#define ALL_IR_INSTRUCTION_TYPES(F)                              \
  F(IMMEDIATE)                                                   \
  F(CALL)                                                        \
  F(LOAD)                                                        \
                                                                 \
  F(RETURN)                                                      \
  F(BRANCH)                                                      \
  F(BRANCH_CONDITIONAL)                                          \
  F(UNREACHABLE)                                                 \
                                                                 \
  F(PHI)                                                         \
  F(COPY)                                                        \
                                                                 \
  ALL_BINARY_INSTRUCTION_TYPES(F)                                \
                                                                 \
  F(STATIC_REF)                                                  \
  F(FUNC_REF)                                                    \
                                                                 \
  /** Store data at an address. **/                              \
  F(STORE)                                                       \
                                                                 \
  F(NOT)                                                         \
                                                                 \
  F(PARAMETER)                                                   \
                                                                 \
  /**                                                            \
   * A lot of backends have these instructions, but the IR isn't \
   * generated with them in it.                                  \
   */                                                            \
  F(REGISTER)                                                    \
  F(ALLOCA)                                                      \
  /**                                                            \
   * Literal types (not generated, but used for data transfer    \
   * between frontend and backend)                               \
   */                                                            \
  F(LIT_INTEGER)                                                 \
  F(LIT_STRING)

#define BINARY_INSTRUCTION_CASE_HELPER(enumerator, name) case IR_##enumerator:
#define ALL_BINARY_INSTRUCTION_CASES() ALL_BINARY_INSTRUCTION_TYPES(BINARY_INSTRUCTION_CASE_HELPER)

#define DEFINE_IR_INSTRUCTION_TYPE(type, ...) CAT(IR_, type),
typedef enum IRType {
  ALL_IR_INSTRUCTION_TYPES(DEFINE_IR_INSTRUCTION_TYPE)
  IR_COUNT
} IRType;
#undef DEFINE_IR_INSTRUCTION_TYPE

typedef struct IRPhiArgument {
  /// The value of the argument itself.
  IRInstruction *value;
  /// Stores the predecessor to the Phi node in the direction of the
  /// argument assignment.
  ///    [a]
  ///  [t] [o]
  ///    \ [b]
  ///    [j]
  /// For example, if arg->value->block == o, then arg->block == b.
  IRBlock *block;
} IRPhiArgument;

typedef struct IRCall {
  Vector(IRInstruction *) arguments;
  // TODO: Make this a named union!
  union {
    IRInstruction *callee_instruction;
    IRFunction *callee_function;
  };
  bool is_indirect : 1;
  bool tail_call : 1;
} IRCall;

typedef struct IRBranchConditional {
  IRInstruction *condition;
  IRBlock *then;
  IRBlock *else_;
} IRBranchConditional;

typedef struct IRStackAllocation {
  usz size;
  usz offset;
} IRStackAllocation;

void mark_used(IRInstruction *usee, IRInstruction *user);

typedef struct IRInstruction {
  enum IRType kind;
  Register result;

  Type *type;

  /// TODO: do we really need both of these?
  u32 id;
  u32 index;

  /// List of instructions using this instruction.
  InstructionVector users;

  list_node(struct IRInstruction);

  IRBlock *parent_block;

  /// The MachineInstruction corresponding to this IRInstruction.
  MInst* mi;

  union {
    IRBlock *destination_block;
    IRInstruction *operand;
    u64 imm;
    IRCall call;
    struct {
      Vector(IRPhiArgument*) args;  ///< For unfortunate reasons, these *have* to be on the heap.
      RegisterDescriptor vreg;
    } phi;
    IRBranchConditional cond_br;
    struct {
      IRInstruction *addr;
      IRInstruction *value;
    } store;
    struct {
      IRInstruction *lhs;
      IRInstruction *rhs;
    };
    IRStaticVariable* static_ref;
    IRFunction *function_ref;
    IRStackAllocation alloca;
    string str;
  };
} IRInstruction;

/// A block is a list of instructions that have control flow enter at
/// the beginning and leave at the end.
typedef struct IRBlock {
  string name;

  List(IRInstruction) instructions;

  /// A pointer to the function the block is attached to, or NULL if
  /// detached.
  IRFunction *function;

  list_node(struct IRBlock);

  // Unique ID (among blocks)
  size_t id;
  // For the backend.
  bool done;
} IRBlock;

typedef struct IRFunction {
  string name;

  List(IRBlock) blocks;

  Vector(IRInstruction*) parameters;

  /// Pointer to the context that owns this function.
  CodegenContext *context;

  /// The type of the function.
  Type *type;

  // Unique ID (among functions)
  size_t id;

  // Used by certain backends.
  size_t locals_total_size;

  size_t registers_in_use;

  bool attr_consteval : 1;
  bool attr_forceinline : 1;
  bool attr_global : 1;
  bool attr_leaf : 1;
  bool attr_noreturn : 1;
  bool attr_pure : 1;
  bool is_extern : 1;
} IRFunction;

struct IR {
  Vector(IRFunction*) functions;
};

void ir_set_func_ids(IRFunction *f);
void ir_set_ids(CodegenContext *context);

bool ir_is_branch(IRInstruction*);

/// Check whether a block is closed.
bool ir_is_closed(IRBlock *block);

void ir_femit_instruction(FILE *file, IRInstruction *instruction);
void ir_femit_block(FILE *file, IRBlock *block);
void ir_femit_function(FILE *file, IRFunction *function);
void ir_femit(FILE *file, CodegenContext *context);

void ir_add_function_call_argument
(CodegenContext *context,
 IRInstruction *call,
 IRInstruction *argument);

IRBlock *ir_block_create();

void ir_block_attach_to_function(IRFunction *function, IRBlock *new_block);
void ir_block_attach(CodegenContext *context, IRBlock *new_block);

IRFunction *ir_function(CodegenContext *context, span name, Type *function_type);
IRInstruction *ir_funcref(CodegenContext *context, IRFunction *function);

void ir_force_insert_into_block(IRBlock *block, IRInstruction *new_instruction);
void ir_insert_into_block(IRBlock *block, IRInstruction *new_instruction);

void ir_insert(CodegenContext *context, IRInstruction *new_instruction);

void insert_instruction_before(IRInstruction *i, IRInstruction *before);
void insert_instruction_after(IRInstruction *i, IRInstruction *after);

IRInstruction *ir_parameter(CodegenContext *context, size_t index);
void ir_add_parameter_to_function(IRFunction *, Type *);

IRInstruction *ir_phi(CodegenContext *context, Type *type);
void ir_phi_add_argument(IRInstruction *phi, IRPhiArgument *argument);
void ir_phi_remove_argument(IRInstruction *phi, IRBlock *block);
void ir_phi_argument
(IRInstruction *phi,
 IRBlock *phi_predecessor,
 IRInstruction *argument);

/// NOTE: Does not insert call instruction.
IRInstruction *ir_direct_call
(CodegenContext *context,
 IRFunction *callee);

/// NOTE: Does not insert call instruction.
IRInstruction *ir_indirect_call
(CodegenContext *context,
 IRInstruction *function);

IRInstruction *ir_immediate
(CodegenContext *context,
 Type *type,
 u64 immediate);

IRInstruction *ir_load
(CodegenContext *context,
 IRInstruction *address);

IRInstruction *ir_store
(CodegenContext *context,
 IRInstruction *data,
 IRInstruction *address);

IRInstruction *ir_branch_conditional
(CodegenContext *context,
 IRInstruction *condition,
 IRBlock *then_block,
 IRBlock *otherwise_block);

IRInstruction *ir_branch
(CodegenContext *context,
 IRBlock *destination);

IRInstruction *ir_branch_into_block
(IRBlock *destination,
 IRBlock *block);

IRInstruction *ir_return
(CodegenContext *context,
 IRInstruction *return_value);

IRInstruction *ir_copy_unused
(CodegenContext *context,
 IRInstruction *source);

IRInstruction *ir_copy
(CodegenContext *context,
 IRInstruction *source);

IRInstruction *ir_not
(CodegenContext *context,
 IRInstruction *source);

#define DECLARE_BINARY_INSTRUCTION(_, name) \
  IRInstruction *ir_##name(CodegenContext *context, IRInstruction *lhs, IRInstruction *rhs);
ALL_BINARY_INSTRUCTION_TYPES(DECLARE_BINARY_INSTRUCTION)
#undef DECLARE_BINARY_INSTRUCTION

/// Create a variable with static storage duration.
///
/// \param context The codegen context.
/// \param decl The expression that declares the variable (may be NULL).
/// \param type The type of the variable.
/// \param name The name of the variable.
/// \return An IR_STATIC_REF to the variable.
IRInstruction *ir_create_static
(CodegenContext *context,
 Node *decl,
 Type *type,
 span name);

/// Create a reference to a variable with static storage duration.
IRInstruction *ir_static_reference(CodegenContext *context, IRStaticVariable* var);

IRInstruction *ir_stack_allocate
(CodegenContext *context,
 Type *type);

/// Check if an instruction returns a value.
bool ir_is_value(IRInstruction *instruction);

/// Print the defun signature of a function.
void ir_print_defun(FILE *file, IRFunction *function);

/// Replace all uses of instruction with replacement.
void ir_replace_uses(IRInstruction *instruction, IRInstruction *replacement);

/// Remove this instruction from the users lists of its children.
void ir_unmark_usees(IRInstruction *instruction);

/// Remove an instruction from the AST and free it.
/// Used by the optimiser.
void ir_remove(IRInstruction* instruction);
void ir_remove_use(IRInstruction *usee, IRInstruction *user);
void ir_remove_and_free_block(IRBlock *block);

/// Free memory used by an instruction. This is unsafe as
/// it doesn’t check whether the instruction is used by other
/// instructions, so use this only when freeing the entire IR.
void ir_free_instruction_data(IRInstruction *instruction);

/// Mark a block as ending w/ `unreachable` and remove it
/// from PHIs.
void ir_mark_unreachable(IRBlock *block);

/// Iterate over each child of an instruction.
///
/// \param instruction The instruction to iterate over.
/// \param callback A callback that is called with the instruction and the child.
/// \param data User data that is passed to the callback.
void ir_for_each_child(
    IRInstruction *inst,
    void callback(IRInstruction *user, IRInstruction **child, void *data),
    void *data
);

#endif /* INTERMEDIATE_REPRESENTATION_H */
