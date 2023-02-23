#ifndef COMPILER_PARSER_H
#define COMPILER_PARSER_H

#include <ast.h>
#include <utils.h>

/// Parse a program.
///
/// \param source The source code to parse.
/// \param filename The name of the file that we’re parsing.
/// \return An AST representing the source code on success, NULL on failure.
NODISCARD AST *parse(span source, const char* filename);

/// Get the string representation of a token type.
NODISCARD const char *token_type_to_string(enum TokenType type);

#endif /* COMPILER_PARSER_H */
