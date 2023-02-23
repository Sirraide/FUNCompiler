#ifndef INTERCEPT_LEXER_H
#define INTERCEPT_LEXER_H

#include <ast.h>
#include <ctype.h>
#include <error.h>
#include <codegen/codegen_forward.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
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

/// Seek to a source location.
void seek_location(
  span source,
  loc location,
  u32 *line,
  u32 *line_start,
  u32 *line_end
);

/// Check if a character may start an identifier.
static inline bool isstart(char c) {
  return isalpha(c) || c == '_' || c == '$';
}

/// Check if a character may be part of an identifier.
static inline bool iscontinue(char c) {
  return isstart(c) || isdigit(c);
}

/// Start lexing a token.
///
/// \return Whether we should continue or not.
bool lexer_start_token(Lexer *l);

/// Finish lexing a token.
void lexer_fin_token(Lexer *l);

/// Skip a line.
void lexer_skip_line(Lexer *l);

/// Lex the next character.
///
/// longjmp()s to l->error_buffer on error.
void next_char(Lexer *l);

/// Lex a number.
///
/// longjmp()s to l->error_buffer on error.
void next_number(Lexer *l);

/// Lex a string.
///
/// longjmp()s to l->error_buffer on error.
void next_string(Lexer *l);
#endif // INTERCEPT_LEXER_H
