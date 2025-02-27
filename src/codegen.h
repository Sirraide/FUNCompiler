#ifndef CODEGEN_H
#define CODEGEN_H

#include <codegen/codegen_forward.h>
#include <codegen/generic_object.h>

#include <ast.h>
#include <error.h>
#include <parser.h>
#include <stdio.h>
#include <vector.h>

extern bool debug_ir;
extern bool print_ir2;
extern bool codegen_only;
extern bool annotate_code;
extern bool print_dot_cfg;
extern bool print_dot_dj;

typedef Vector(IRInstruction *) InstructionVector;

CodegenContext *codegen_context_create
(Module *ast,
 CodegenArchitecture,
 CodegenTarget,
 CodegenCallingConvention call_convention,
 FILE* code);

void codegen_context_free(CodegenContext *context);

typedef struct IRStaticVariable {
  string name;
  Type *type;
  Node *decl;
  InstructionVector references;
  /// When non-null, points to the IRInstruction of the initialised value.
  /// This *must* be one of:
  /// - IR_LIT_INTEGER
  /// - IR_LIT_STRING
  IRInstruction *init;

  SymbolLinkage linkage;

  /// Used by the optimiser.
  bool referenced;
} IRStaticVariable;

struct CodegenContext {
  /// The IR
  Vector(IRFunction *) functions;
  Vector(IRStaticVariable *) static_vars;
  IRInstructionVector free_instructions;
  IRBlockVector free_blocks;

  /// Code emission (targets)
  FILE *code;
  GenericObjectFile *object;

  /// (Used to construct IR)
  Module *ast;
  IRFunction *function;
  IRFunction *entry;
  IRBlock *insert_point;

  /// Options
  CodegenArchitecture arch;
  // TODO: Allow for multiple targets.
  CodegenTarget target;
  CodegenCallingConvention call_convention;

  /// Poison value.
  IRInstruction *poison;

  /// Whether there was an error during codegen.
  /// TODO: Move target information up to Sema. Keep this here regardless tho.
  bool has_err;

  /// FFI type sizes in BITS (!)
  struct {
    u8 cchar_size;
    u8 cshort_size;
    u8 cint_size;
    u8 clong_size;
    u8 cllong_size;
    u8 pointer_size;
    u8 integer_size; /// Intercept `integer` type.
  } ffi;
};

// TODO/FIXME: Make this a parameter affectable by command line arguments.
extern char codegen_verbose;

NODISCARD bool codegen
(CodegenLanguage,
 CodegenArchitecture arch,
 CodegenTarget target,
 CodegenCallingConvention,
 const char *infile,
 const char *outfile, Module *ast,
 string ir);

/// Mangle a function name.
void mangle_function_name(IRFunction *function);

#endif /* CODEGEN_H */
