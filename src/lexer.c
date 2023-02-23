#include <ctype.h>
#include <errno.h>
#include <lexer.h>

/// ===========================================================================
///  Error handling.
/// ===========================================================================
#define ISSUE_DIAGNOSTIC(sev, loc, parser, ...)                                        \
  do {                                                                                 \
    issue_diagnostic((sev), (parser)->filename, (parser)->source, (loc), __VA_ARGS__); \
    longjmp((parser)->error_buffer, 1);                                                  \
  } while (0)
#define ERR_AT(p, loc, ...) ISSUE_DIAGNOSTIC(DIAG_ERR, loc, (p), __VA_ARGS__)
#define ERR(p, ...)         ERR_AT((p), (p)->tok.source_location, __VA_ARGS__)

bool lexer_start_token(Lexer *l) {
  /// Keep returning EOF once EOF has been reached.
  if (!l->lastc) {
    l->tok.type = TK_EOF;
    return false;
  }

  /// Set the token to invalid in case there is an error.
  l->tok.type = TK_INVALID;

  /// Skip whitespace.
  while (isspace(l->lastc)) next_char(l);

  /// Start of the token.
  l->tok.source_location.start = (u32) (l->curr - l->source.data - 1);
  return true;
}

void lexer_fin_token(Lexer *l) {
  /// Set the end of the token.
  l->tok.source_location.end = (u32) (l->curr - l->source.data - 1);
}

void lexer_skip_line(Lexer *l) {
  while (l->lastc && l->lastc != '\n') next_char(l);
}

/// Lex the next character.
void next_char(Lexer *p) {
  /// Keep returning EOF once EOF has been reached.
  if (p->curr >= p->end) {
    p->lastc = 0;
    return;
  }

  /// Read the next character.
  p->lastc = *p->curr++;
  if (p->lastc == 0) ERR(p, "Lexer can not handle null bytes");

  /// Collapse CRLF and LFCR to a single newline,
  /// but keep CRCR and LFLF as two newlines.
  if (p->lastc == '\r' || p->lastc == '\n') {
      /// Two newlines in a row.
      if (p->curr != p->end && (*p->curr == '\r' || *p->curr == '\n')) {
          bool same = p->lastc == *p->curr;
          p->lastc = '\n';

          /// CRCR or LFLF
          if (same) return;

          /// CRLF or LFCR
          p->curr++;
      }

      /// Either CR or LF followed by something else.
      p->lastc = '\n';
  }
}

/// Parse a number.
static void parse_number(Lexer *p, int base) {
  /// Zero-terminate the string or else `strtoull()` might try
  /// to convert data left over from the previous token.
  string_buf_zterm(&p->tok.text);

  /// Convert the number.
  char *end;
  errno = 0;
  p->tok.integer = (u64) strtoull(p->tok.text.data, &end, base);
  if (errno == ERANGE) ERR(p, "Integer literal too large");
  if (end != p->tok.text.data + p->tok.text.size) ERR(p, "Invalid integer literal");
}

/// Lex a number.
void next_number(Lexer *p) {
  /// Record the start of the number.
  vector_clear(p->tok.text);
  p->tok.integer = 0;
  p->tok.type = TK_NUMBER;

  /// At least one leading zero.
  if (p->lastc == '0') {
    /// Discard the zero.
    next_char(p);

    /// Another zero is an error.
    if (p->lastc == '0') ERR(p, "Leading zeroes are not allowed in decimal literals. Use 0o/0O for octal literals.");

#define DO_PARSE_NUMBER(name, chars, condition, base)                       \
  /** Read all chars that are part of the literal. **/                      \
  if (p->lastc == chars[0] || p->lastc == chars[1]) {                       \
    /** Yeet the prefix. **/                                                \
    next_char(p);                                                           \
                                                                            \
    /** Lex the digits. **/                                                 \
    while (condition) {                                                     \
      vector_push(p->tok.text, p->lastc);                                   \
      next_char(p);                                                         \
    }                                                                       \
                                                                            \
    /** We need at least one digit. **/                                     \
    p->tok.source_location.end = (u32) ((p->curr - 1) - p->source.data);    \
    if (p->tok.text.size == 0) ERR(p, "Expected at least one " name " digit"); \
                                                                            \
    /** Actually parse the number. **/                                      \
    return parse_number(p, base);                                           \
  }

    DO_PARSE_NUMBER("binary", "bB", p->lastc == '0' || p->lastc == '1', 2)
    DO_PARSE_NUMBER("octal", "oO", isdigit(p->lastc) && p->lastc < '8', 8)
    DO_PARSE_NUMBER("hexadecimal", "xX", isxdigit(p->lastc), 16)

#undef DO_PARSE_NUMBER

    /// If the next character is a space or delimiter, then this is a literal 0.
    if (isspace(p->lastc) || !isalpha(p->lastc)) return;

    /// Anything else is an error.
    ERR(p, "Invalid integer literal");
  }

  /// Any other digit means we have a decimal number.
  if (isdigit(p->lastc)) {
    do {
      vector_push(p->tok.text, p->lastc);
      next_char(p);
    } while (isdigit(p->lastc));
    return parse_number(p, 10);
  }

  /// Anything else is an error.
  ERR(p, "Invalid integer literal");
}

void next_string(Lexer *p) {
  /// Yeet the delimiter and clear the string.
  char delim = p->lastc;
  vector_clear(p->tok.text);
  next_char(p);

  /// Single-quoted strings are not escaped.
  if (delim == '\'') {
    while (p->lastc != delim) {
      if (p->lastc == 0) ERR(p, "Unterminated string literal");
      vector_push(p->tok.text, p->lastc);
      next_char(p);
    }
  }

  /// Double-quoted strings are escaped.
  else {
    ASSERT(delim == '"');
    while (p->lastc != delim) {
      if (p->lastc == 0) ERR(p, "Unterminated string literal");

      /// Handle escape sequences.
      if (p->lastc == '\\') {
        next_char(p);
        switch (p->lastc) {
          case 'n': vector_push(p->tok.text, '\n'); break;
          case 'r': vector_push(p->tok.text, '\r'); break;
          case 't': vector_push(p->tok.text, '\t'); break;
          case 'f': vector_push(p->tok.text, '\f'); break;
          case 'v': vector_push(p->tok.text, '\v'); break;
          case 'a': vector_push(p->tok.text, '\a'); break;
          case 'b': vector_push(p->tok.text, '\b'); break;
          case 'e': vector_push(p->tok.text, '\033'); break;
          case '0': vector_push(p->tok.text, '\0'); break;
          case '\'': vector_push(p->tok.text, '\''); break;
          case '\"': vector_push(p->tok.text, '\"'); break;
          case '\\': vector_push(p->tok.text, '\\'); break;
          default: ERR(p, "Invalid escape sequence");
        }
      }

      /// Just append the character if it’s not an escape sequence.
      else { vector_push(p->tok.text, p->lastc); }
      next_char(p);
    }
  }

  /// Make sure the string is terminated by the delimiter.
  if (p->lastc != delim) ERR(p, "Unterminated string literal");
  p->tok.type = TK_STRING;
  next_char(p);
}

void seek_location(span source, loc location, u32 *o_line, u32 *o_line_start, u32 *o_line_end) {
  /// Seek to the start of the line. Keep track of the line number.
  u32 line = 1;
  u32 line_start = 0;
  for (u32 i = location.start; i > 0; --i) {
    if (source.data[i] == '\n') {
      if (!line_start) line_start = i + 1;
      ++line;
    }
  }

  /// Don’t include the newline in the line.
  if (source.data[line_start] == '\n') ++line_start;

  /// Seek to the end of the line.
  u32 line_end = location.end;
  while (line_end < source.size && source.data[line_end] != '\n') line_end++;

  /// Return the results.
  if (o_line) *o_line = line;
  if (o_line_start) *o_line_start = line_start;
  if (o_line_end) *o_line_end = line_end;
}