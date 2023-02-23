#ifndef COMPILER_PARSER_H
#define COMPILER_PARSER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <error.h>
#include <codegen/codegen_forward.h>
#include <ast.h>
#include <setjmp.h>

typedef struct Token {
  enum TokenType type;
  loc source_location;
  string_buffer text;
  u64 integer;
} Token;

typedef struct Lexer {
  /// The source code that we’re parsing.
  span source;

  /// The name of the file that we’re parsing.
  const char *filename;

  /// The last character read.
  char lastc;

  /// Lexer state.
  const char *curr;
  const char *end;

  /// The current token.
  Token tok;

  /// For error handling.
  jmp_buf error_buffer;
} Lexer;


/// Parse a program.
///
/// \param source The source code to parse.
/// \param filename The name of the file that we’re parsing.
/// \return An AST representing the source code on success, NULL on failure.
NODISCARD AST *parse(span source, const char* filename);

/// Get the string representation of a token type.
NODISCARD const char *token_type_to_string(enum TokenType type);

/// Seek to a source location.
void seek_location(
  span source,
  loc location,
  u32 *line,
  u32 *line_start,
  u32 *line_end
);

#endif /* COMPILER_PARSER_H */
