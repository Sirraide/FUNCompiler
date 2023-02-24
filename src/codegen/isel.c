#include <codegen/isel.h>
#include <lexer.h>
#include <uv/errno.h>

/// ===========================================================================
///  Lexer and macro expander.
/// ===========================================================================
#define ISSUE_DIAGNOSTIC(sev, loc, parser, ...)                                                              \
  do {                                                                                                       \
    issue_diagnostic((sev), (parser)->l.filename, (parser)->l.source, (loc), __VA_ARGS__);                       \
    foreach (MacroExpansion, e, (parser)->expansion_stack)                                                   \
      issue_diagnostic(DIAG_NOTE, (parser)->l.filename, (parser)->l.source, e->expanding_macro->source_location, \
                       "Expanded from macro '%S'", e->expanding_macro->name);                                \
    longjmp((parser)->l.error_buffer, 1);                                                                      \
  } while (0)
#define ERR_AT(p, loc, ...) ISSUE_DIAGNOSTIC(DIAG_ERR, loc, (p), __VA_ARGS__)
#define ERR(p, ...)         ERR_AT((p), (p)->l.tok.source_location, __VA_ARGS__)

struct Parser;
typedef Vector(Token) Tokens;

typedef struct Macro {
  string name;
  loc source_location;
  bool is_for_loop;
  Tokens parameters;
  Tokens expansion;

  /// Handler for builtin macros. This may be NULL.
  /// If called, it must overwrite the current token.
  void (*builtin_handler)(struct Parser *p, struct Macro *m);
} Macro;

typedef struct MacroExpansion {
  Macro *expanding_macro;
  usz token_index;
  usz for_loop_index;
  Tokens arguments;
} MacroExpansion;

typedef struct Parser {
  Lexer l;
  Vector(MacroExpansion) expansion_stack;
  Vector(Macro*) macros;
  ISelTable *table;

  /// If true, the parser is in raw mode and will not expand macros.
  bool raw_mode;
} Parser;

/// All keywords.
static const struct {
  span kw;
  enum TokenType type;
} keywords[28] = {
    {literal_span_raw("match"), TK_ISEL_MATCH},
    {literal_span_raw("where"), TK_ISEL_WHERE},
    {literal_span_raw("with"), TK_ISEL_WITH},
    {literal_span_raw("commutative"), TK_ISEL_COMMUTATIVE},
    {literal_span_raw("reg"), TK_ISEL_REG},
    {literal_span_raw("imm"), TK_ISEL_IMM},
    {literal_span_raw("name"), TK_ISEL_NAME},
    {literal_span_raw("block"), TK_ISEL_BLOCK},
    {literal_span_raw("result"), TK_ISEL_RESULT},
    {literal_span_raw("clobber"), TK_ISEL_CLOBBER},
    {literal_span_raw("out"), TK_ISEL_OUT},
    {literal_span_raw("is"), TK_ISEL_IS},
    {literal_span_raw("eq"), TK_ISEL_EQ},
    {literal_span_raw("ne"), TK_ISEL_NE},
    {literal_span_raw("lt"), TK_ISEL_LT},
    {literal_span_raw("gt"), TK_ISEL_GT},
    {literal_span_raw("le"), TK_ISEL_LE},
    {literal_span_raw("ge"), TK_ISEL_GE},
    {literal_span_raw("discard"), TK_ISEL_DISCARD},
    {literal_span_raw("any"), TK_ISEL_ANY},
    {literal_span_raw("emit"), TK_ISEL_EMIT},
    {literal_span_raw("macro"), TK_ISEL_MACRO},
    {literal_span_raw("undef"), TK_ISEL_UNDEF},
    {literal_span_raw("expands"), TK_ISEL_EXPANDS},
    {literal_span_raw("endmacro"), TK_ISEL_ENDMACRO},
    {literal_span_raw("for"), TK_ISEL_FOR},
    {literal_span_raw("do"), TK_ISEL_DO},
    {literal_span_raw("endfor"), TK_ISEL_ENDFOR},
  };

/// Copy a token.
static Token copy_token(Token t) {
  string_buffer buf = t.text;
  t.text = (string_buffer){0};
  vector_append_all(t.text, buf);
  return t;
}

/// Parse a number.
static void parse_number(Parser *p, usz index, int base) {
  string_buf_zterm(&p->l.tok.text);

  /// Convert the number.
  char *end;
  errno = 0;
  p->l.tok.integer = (u64) strtoull(p->l.tok.text.data + index, &end, base);
  if (errno == ERANGE) ERR(p, "Integer literal too large");
  if (end != p->l.tok.text.data + p->l.tok.text.size) ERR(p, "Invalid integer literal");
}

static void next_token(Parser *p);

/// Lex a macro definition.
/// <macrodef> ::= MACRO MACRONAME MACROPARAM { "," MACROPARAM } EXPANDS { TOKEN } ENDMACRO
static void lex_macro_def(Parser *p) {
  /// Yeet "macro".
  next_token(p);

  /// Get the macro name.
  if (p->l.tok.type != TK_ISEL_MACRONAME) ERR(p, "Expected macro name");

  /// Check if the macro already exists.
  Macro **existing = NULL;
  vector_find_if(p->macros, existing, i, string_eq(p->macros.data[i]->name, p->l.tok.text));
  if (existing) ERR(p, "Macro '%S' already defined", p->l.tok.text);

  /// Create the macro.
  Macro *m = calloc(1, sizeof *m);
  m->name = string_dup(p->l.tok.text);
  m->source_location = p->l.tok.source_location;
  vector_push(p->macros, m);

  /// Yeet the macro name.
  next_token(p);

  /// Parse everything up to the "expands" keyword.
  while (p->l.tok.type != TK_ISEL_EXPANDS && p->l.tok.type != TK_EOF) {
    /// Macro parameters must start with "#" and contain at least one more character.
    if (p->l.tok.type != TK_ISEL_MACRONAME || p->l.tok.text.size < 2) ERR(p, "Expected macro argument");
    vector_push(m->parameters, copy_token(p->l.tok));
    next_token(p);
  }

  /// Yeet "expands".
  if (p->l.tok.type != TK_ISEL_EXPANDS) ERR(p, "Macro definition terminated by end of file");
  next_token(p);

  /// Parse the expansion.
  while (p->l.tok.type != TK_ISEL_ENDMACRO && p->l.tok.type != TK_EOF) {
    vector_push(m->expansion, copy_token(p->l.tok));
    next_token(p);
  }

  /// Yeet "endmacro".
  if (p->l.tok.type != TK_ISEL_ENDMACRO) ERR(p, "Macro definition terminated by end of file");
  next_token(p);
}

/// Free a macro.
static void free_macro(Macro *m) {
  free(m->name.data);
  foreach (Token, t, m->parameters) vector_delete(t->text);
  foreach (Token, t, m->expansion) vector_delete(t->text);
  vector_delete(m->parameters);
  vector_delete(m->expansion);
}

/// <macroundef> ::= UNDEF MACRONAME
static void lex_macro_undef(Parser *p) {
  /// Yeet "undef".
  next_token(p);

  /// Get the macro name.
  if (p->l.tok.type != TK_ISEL_MACRONAME) ERR(p, "Expected macro name");

  /// Check if the macro exists.
  Macro **existing = NULL;
  vector_find_if(p->macros, existing, i, string_eq(p->macros.data[i]->name, p->l.tok.text));
  if (!existing) ERR(p, "Macro '%S' not defined", p->l.tok.text);

  /// Delete the macro.
  free_macro(*existing);
  vector_remove_element_unordered(p->macros, *existing);

  /// Yeet the macro name.
  next_token(p);
}

/// <loop> ::= FOR TOKEN { "," TOKEN } DO { TOKEN } ENDFOR
static void lex_for_expansion(Parser *p) {
  /// Yeet "for".
  next_token(p);

  /// Create the macro.
  Macro *m = calloc(1, sizeof *m);

  /// Get the loop values.
  while (p->l.tok.type != TK_ISEL_DO && p->l.tok.type != TK_EOF) {
    vector_push(m->parameters, copy_token(p->l.tok));
    next_token(p);
  }

  /// Yeet "do".
  if (p->l.tok.type != TK_ISEL_DO) ERR(p, "For loop terminated by end of file");
  if (m->parameters.size == 0) ERR(p, "For loop must have at least one argument");
  next_token(p);

  /// Get the loop expansion.
  while (p->l.tok.type != TK_ISEL_ENDFOR && p->l.tok.type != TK_EOF) {
    vector_push(m->expansion, copy_token(p->l.tok));
    next_token(p);
  }

  /// Yeet "endfor".
  if (p->l.tok.type != TK_ISEL_ENDFOR) ERR(p, "For loop terminated by end of file");
  next_token(p);

  /// Create the expansion.
  MacroExpansion expansion = {0};
  expansion.expanding_macro = m;
  vector_push(p->expansion_stack, expansion);
}

/// Lexer main function for this parser.
static void next_token(Parser *p) {
  /// Pop a token off the expansion stack if we have one.
  while (p->expansion_stack.size) {
    MacroExpansion *m = &vector_back(p->expansion_stack);

    /// Current expansion is done.
    if (m->token_index == m->expanding_macro->expansion.size) {
      /// If this expansion is a for loop, and we have more items in
      /// the list, move to the next item and reset the expansion.
      if (m->expanding_macro->is_for_loop && ++m->for_loop_index < m->expanding_macro->parameters.size) {
        m->token_index = 0;
        /// fallthrough.
      } else {
        if (m->expanding_macro->is_for_loop) free_macro(m->expanding_macro);
        foreach (Token, t, m->expanding_macro->parameters) vector_delete(t->text);
        vector_delete(m->expanding_macro->parameters);
        vector_pop(p->expansion_stack);
        continue;
      }
    }

    p->l.tok = copy_token(m->expanding_macro->expansion.data[m->token_index++]);
    return;
  }

  lexer_start_token(&p->l);
  switch (p->l.lastc) {
    case 0:
      p->l.tok.type = TK_EOF;
      break;

    case ',':
      p->l.tok.type = TK_COMMA;
      break;

    case '.':
      p->l.tok.type = TK_DOT;
      break;

    case '|':
      p->l.tok.type = TK_PIPE;
      break;

    case '(':
      p->l.tok.type = TK_LPAREN;
      break;

    case ')':
      p->l.tok.type = TK_RPAREN;
      break;

    case '#': {
      /// Lex the macro name.
      vector_clear(p->l.tok.text);
      do {
        vector_push(p->l.tok.text, p->l.lastc);
        next_char(&p->l);
      } while (iscontinue(p->l.lastc));

      if (p->raw_mode) {
        p->l.tok.type = TK_ISEL_MACRONAME;
        break;
      }

      /// If the token is '#', find the current for loop expansion.
      if (string_eq(p->l.tok.text, literal_span("#"))) {
        MacroExpansion *m = NULL;
        vector_find_if(p->expansion_stack, m, i, m->expanding_macro->is_for_loop);
        if (!m) ERR(p, "Cannot use '#' outside of a for loop expansion");

        /// Replace the token with the current item in the for loop.
        p->l.tok = copy_token(m->expanding_macro->parameters.data[m->for_loop_index]);
        return;
      }

      /// This may be an argument of the currently expanding macro.
      if (p->expansion_stack.size) {
        Token *tk;
        MacroExpansion *m = &vector_back(p->expansion_stack);
        vector_find_if(m->expanding_macro->parameters, tk, i, string_eq(m->expanding_macro->parameters.data[i].text, p->l.tok.text));
        if (tk) {
          p->l.tok = copy_token(m->arguments.data[tk - m->expanding_macro->parameters.data]);
          return;
        }
      }

      /// Look up the macro.
      Macro **macro = NULL;
      vector_find_if(p->macros, macro, i, string_eq(p->macros.data[i]->name, p->l.tok.text));
      if (!macro) ERR(p, "Unknown macro '%S'", p->l.tok.text);

      /// If the macro has a custom handler, invoke it.
      if ((*macro)->builtin_handler) {
        (*macro)->builtin_handler(p, *macro);
        return;
      }

      /// Create the macro expansion.
      MacroExpansion m = {0};
      m.expanding_macro = *macro;
      m.token_index = 0;
      m.for_loop_index = 0;

      /// Discard the macro name and read the parameters.
      p->raw_mode = true;
      next_token(p);
      foreach_index (i, (*macro)->parameters) {
        if (p->l.tok.type == TK_EOF) ERR(p, "Macro invocation '%S' terminated by end of file", (*macro)->name);
        vector_push(m.arguments, copy_token(p->l.tok));
        next_token(p);
        if (p->l.tok.type == TK_COMMA) next_token(p);
        else break;
      }

      /// Make sure we have enough arguments.
      if (m.arguments.size < (*macro)->parameters.size) ERR(p, "Macro invocation '%S' has too few arguments", (*macro)->name);

      /// Push the expansion onto the stack.
      vector_push(p->expansion_stack, m);

      /// Lex the next token.
      next_token(p);
      return;
    }

    case '%':
      vector_clear(p->l.tok.text);
      do {
        vector_push(p->l.tok.text, p->l.lastc);
        next_char(&p->l);
      } while (iscontinue(p->l.lastc));

      p->l.tok.type = TK_ISEL_REGISTER;
      break;

    default:
      if (isdigit(p->l.lastc)) {
        next_number(&p->l);
        p->l.tok.type = TK_NUMBER;
        break;
      }

      if (isupper(p->l.lastc)) {
        vector_clear(p->l.tok.text);
        do {
          vector_push(p->l.tok.text, p->l.lastc);
          next_char(&p->l);
        } while (isalnum(p->l.lastc));

        p->l.tok.type = TK_ISEL_INSTRUCTION;
        break;
      }

      if (isstart(p->l.lastc)) {
        vector_clear(p->l.tok.text);
        do {
          vector_push(p->l.tok.text, p->l.lastc);
          next_char(&p->l);
        } while (iscontinue(p->l.lastc));

        for (size_t i = 0; i < sizeof keywords / sizeof *keywords; i++) {
          if (string_eq(keywords[i].kw, p->l.tok.text)) {
            if (!p->raw_mode) {
              /// Macro definition.
              if (keywords[i].type == TK_ISEL_MACRO) {
                p->raw_mode = true;
                lex_macro_def(p);
                p->raw_mode = false;
                next_token(p);
                return;
              }

              /// Macro undef.
              if (keywords[i].type == TK_ISEL_UNDEF) {
                p->raw_mode = true;
                lex_macro_undef(p);
                p->raw_mode = false;
                next_token(p);
                return;
              }

              /// For loop.
              if (keywords[i].type == TK_ISEL_FOR) {
                p->raw_mode = true;
                lex_for_expansion(p);
                p->raw_mode = false;
                next_token(p);
                return;
              }
            }

            p->l.tok.type = keywords[i].type;
            goto done;
          }
        }

        /// Instruction name.
        if (string_starts_with(p->l.tok.text, literal_span("i"))) {
          bool numbers;
          vector_all_of(p->l.tok.text, numbers, i, i == 0 || isdigit(p->l.tok.text.data[i]));
          if (numbers) {
            parse_number(p, 1, 10);
            p->l.tok.type = TK_ISEL_INAME;
            break;
          }
        }

        /// Operand name.
        if (string_starts_with(p->l.tok.text, literal_span("o"))) {
          /// 'o*'.
          if (p->l.tok.text.size == 1 || p->l.lastc == '*') {
            next_char(&p->l);
            p->l.tok.type = TK_ISEL_OSTAR;
            break;
          }

          bool numbers;
          vector_all_of(p->l.tok.text, numbers, i, i == 0 || isdigit(p->l.tok.text.data[i]));
          if (numbers) {
            parse_number(p, 1, 10);
            p->l.tok.type = TK_ISEL_ONAME;
            break;
          }
        }

        p->l.tok.type = TK_IDENT;
        break;
      }

      ERR(p, "Unexpected character '%c'", p->l.lastc);
  }

done:
  lexer_fin_token(&p->l);
}

/// ===========================================================================
///  Parser
/// ===========================================================================
/// Check if we’re at a token.
bool at(Parser *p, enum TokenType tt) { return p->l.tok.type == tt; }
void consume (Parser *p, enum TokenType tt) {
  if (p->l.tok.type != tt)
    ERR(p, "Expected '%s', got '%s'", token_type_to_string(tt), token_type_to_string(p->l.tok.type));
  next_token(p);
}

/// Intern a register string.
usz intern_register(Parser *p, span regname) {
  string* reg = 0;
  vector_find_if(p->table->register_names, reg, i, string_eq(p->table->register_names.data[i], regname));

  /// Register not found, add it.
  if (!reg) {
    vector_push(p->table->register_names, string_dup(regname));
    return p->table->register_names.size - 1;
  }

  /// Register found, return its index.
  return (usz)(reg - p->table->register_names.data);
}

/// Intern an instruction string.
usz intern_instruction(Parser *p, span instname) {
  string* inst = 0;
  vector_find_if(p->table->instruction_names, inst, i, string_eq(p->table->instruction_names.data[i], instname));

  /// Instruction not found, add it.
  if (!inst) {
    vector_push(p->table->instruction_names, string_dup(instname));
    return p->table->instruction_names.size - 1;
  }

  /// Instruction found, return its index.
  return (usz)(inst - p->table->instruction_names.data);
}

/// Parse a filter.
///
/// <filter> ::= WHERE INAME [ INSTRUCTION ] [ <with-clause> ]
ISelFilter parse_filter(Parser *p, usz *ocount) {
  consume(p, TK_ISEL_WHERE);
  ISelFilter filter = {0};

  /// Parse the instruction name.
  if (!at(p, TK_ISEL_INAME)) ERR(p, "Expected instruction name");
  filter.iname = p->l.tok.integer;
  next_token(p);

  /// Parse the instruction type.
  if (at(p, TK_ISEL_INSTRUCTION)) {
    filter.instruction = intern_instruction(p, as_span(p->l.tok.text));
    next_token(p);
  }

  /// Parse the with clause.
  /// <with-clause> ::= WITH [ COMMUTATIVE ] [ <operand> { "," <operand> } ]
  if (!at(p, TK_ISEL_WITH)) return filter;
  next_token(p);

  /// Commutativity.
  if (at(p, TK_ISEL_COMMUTATIVE)) {
    filter.commutative = true;
    next_token(p);
  }

  /// Operands.
  /// <operand> ::= INAME | ONAME [ <type> ] [ <constraint> ]
  while (at(p, TK_ISEL_INAME) || at(p, TK_ISEL_ONAME) || at(p, TK_ISEL_OSTAR)) {
    ISelFilterOperand op = {0};

    /// Instruction reference.
    if (at(p, TK_ISEL_INAME)) {
      op.kind = ISEL_FILTER_OPERAND_INAME;
      op.name = p->l.tok.integer;

      /// IName must refer to a previous instruction.
      if (filter.iname == 1) ERR(p, "First filter cannot reference to other instructions");
      if (op.name >= filter.iname) ERR(p, "IName filter operand must refer to a previous instruction");
      next_token(p);
    }

    /// Operand.
    else {
      op.kind = at (p, TK_ISEL_OSTAR) ? ISEL_FILTER_OPERAND_REST : ISEL_FILTER_OPERAND_ONAME;

      /// A non-rest operand must have the expected OName.
      if (op.kind != ISEL_FILTER_OPERAND_REST) {
        op.name = ++*ocount;
        if (p->l.tok.integer != *ocount) ERR(p, "Expected OName o'%d', got o'%d'", *ocount, op.name);
      }

      /// Parse the type. Defaults to any.
      /// <type> ::= REG | IMM | NAME | BLOCK | ANY
      if (at(p, TK_ISEL_REG)) {
        op.type = ISEL_FILTER_OPERAND_TYPE_REG;
        next_token(p);
      } else if (at(p, TK_ISEL_IMM)) {
        op.type = ISEL_FILTER_OPERAND_TYPE_IMM;
        next_token(p);
      } else if (at(p, TK_ISEL_NAME)) {
        op.type = ISEL_FILTER_OPERAND_TYPE_NAME;
        next_token(p);
      } else if (at(p, TK_ISEL_BLOCK)) {
        op.type = ISEL_FILTER_OPERAND_TYPE_BLOCK;
        next_token(p);
      } else if (at(p, TK_ISEL_ANY)) {
        op.type = ISEL_FILTER_OPERAND_TYPE_ANY;
        next_token(p);
      } else {
        op.type = ISEL_FILTER_OPERAND_TYPE_ANY;
      }

      /// Parse the operand constraint if there is one. It must match the type.
      /// <constraint> ::= ANY | ( EQ | NE | LT | GT | LE | GE ) <value> { "|" <value> }
      if (at(p, TK_ISEL_ANY)) {
        op.constraint = ISEL_CONSTRAINT_ANY;
        next_token(p);
      } else if (at(p, TK_ISEL_EQ)) {
        op.constraint = ISEL_CONSTRAINT_EQ;
        next_token(p);
      } else if (at(p, TK_ISEL_NE)) {
        op.constraint = ISEL_CONSTRAINT_NE;
        next_token(p);
      } else if (at(p, TK_ISEL_LT)) {
        op.constraint = ISEL_CONSTRAINT_LT;
        next_token(p);
      } else if (at(p, TK_ISEL_GT)) {
        op.constraint = ISEL_CONSTRAINT_GT;
        next_token(p);
      } else if (at(p, TK_ISEL_LE)) {
        op.constraint = ISEL_CONSTRAINT_LE;
        next_token(p);
      } else if (at(p, TK_ISEL_GE)) {
        op.constraint = ISEL_CONSTRAINT_GE;
        next_token(p);
      } else {
        op.constraint = ISEL_CONSTRAINT_ANY;
      }

      /// Non-any constraints have operands.
      /// <value> ::= REGISTER | NUMBER | ONAME | INAME
      for (; op.constraint != ISEL_CONSTRAINT_ANY ;) {
        ISelConstraintParameter param =  {0};
        if (at(p, TK_ISEL_REGISTER)) {
          if (op.type != ISEL_FILTER_OPERAND_TYPE_REG) ERR(p, "Expected register constraint operand");
          param.kind = ISEL_PARAMETER_REGISTER;
          param.value = intern_register(p, as_span(p->l.tok.text));
          next_token(p);
        } else if (at(p, TK_ISEL_NUMBER)) {
          if (op.type != ISEL_FILTER_OPERAND_TYPE_IMM) ERR(p, "Expected immediate constraint operand");
          param.kind = ISEL_PARAMETER_IMMEDIATE;
          param.value = p->l.tok.integer;
          next_token(p);
        } else if (at(p, TK_ISEL_ONAME)) {
          if (op.type != ISEL_FILTER_OPERAND_TYPE_REG) ERR(p, "Expected register constraint operand");
          param.kind = ISEL_PARAMETER_ONAME;
          param.value = p->l.tok.integer;
          next_token(p);
        } else if (at(p, TK_ISEL_INAME)) {
          if (op.type != ISEL_FILTER_OPERAND_TYPE_REG) ERR(p, "Expected register constraint operand");
          param.kind = ISEL_PARAMETER_INAME;
          param.value = p->l.tok.integer;
          next_token(p);
        } else {
          ERR(p, "Expected constraint operand");
        }

        vector_push(op.constraint_parameters, param);
        if (at(p, TK_PIPE)) next_token(p);
        else break;
      }
    }

    vector_push(filter.operands, op);
  }

  /// Finally, we’re done.
  return filter;
}

/// Add a pattern to the table.
void add_pattern(Parser *p, ISelPattern pat) {
}

/// Parse a rule.
/// <rule> ::= MATCH INAME { "," INAME } { <filter> } { <side-effect> } <result> { <result> } "."
void parse_rule(Parser *p) {
  /// Yeet "match".
  consume(p, TK_ISEL_MATCH);
  next_token(p);
  ISelPattern pat = {0};

  /// Parse the instructions.
  u64 icount = 0;
  for (;;) {
    if (!at(p, TK_ISEL_NAME)) ERR(p, "Expected instruction name");
    if (p->l.tok.integer != icount + 1) ERR(p, "Expected instruction name 'i%d', got 'i%d'", icount + 1, p->l.tok.integer);
    icount++;
    if (at(p, TK_COMMA)) next_token(p);
    else break;
  }

  /// Parse the filters.
  u64 ocount = 0;
  while (at(p, TK_ISEL_WHERE))
    vector_push(pat.filters, parse_filter(p, &ocount));

  /// Parse the side effects.
  /// <side-effect> ::= { <clobber> | <out> }
  while (at(p, TK_ISEL_CLOBBER) || at(p, TK_ISEL_OUT)) {
    /// <clobber> ::= CLOBBER REGISTER { "," REGISTER }
    while (at(p, TK_ISEL_CLOBBER)) {
      next_token(p);
      if (!at(p, TK_ISEL_REGISTER)) ERR(p, "Expected register name");
      vector_push(pat.clobbers, p->l.tok.integer);
      next_token(p);

      if (at(p, TK_COMMA)) next_token(p);
      else break;
    }

    /// <out> ::= OUT ( REGISTER | ONAME | ANY )
    if (at(p, TK_ISEL_OUT)) {
      if (pat.result_kind != ISEL_RESULT_NONE) ERR(p, "Multiple out clauses");
      next_token(p);

      if (at(p, TK_ISEL_REGISTER)) {
        pat.result_kind = ISEL_RESULT_REGISTER;
        pat.result = intern_register(p, as_span(p->l.tok.text));
      } else if (at(p, TK_ISEL_ONAME)) {
        pat.result_kind = ISEL_RESULT_ONAME;
        pat.result = p->l.tok.integer;
      } else if (at(p, TK_ISEL_ANY)) {
        pat.result_kind = ISEL_RESULT_ANY;
      } else {
        ERR(p, "Expected register, oname, or 'any'");
      }
      next_token(p);
    }
  }

  /// Parse the results.
  /// <result> ::= <emit> | DISCARD
  do {
    /// Discard clause. Nothing may follow this.
    if (at (p, TK_ISEL_DISCARD)) {
      if (pat.emits.size) ERR(p, "'discard' must be the sole result of a pattern.");
      break;
    }

    /// <emit> ::= EMIT INSTRUCTION [ <emit-operand> { "," <emit-operand> } ]
    next_token(p);
    if (!at(p, TK_ISEL_INSTRUCTION)) ERR(p, "Expected instruction name");
    ISelEmit em = {0};
    em.instruction = intern_instruction(p, as_span(p->l.tok.text));
    next_token(p);

    /// <emit-operand> ::= ONAME | INAME | RESULT | NUMBER | REGISTER
    if (at(p, TK_ISEL_ONAME) || at(p, TK_ISEL_INAME) || at(p, TK_ISEL_RESULT) || at(p, TK_ISEL_NUMBER) || at(p, TK_ISEL_REGISTER)) {
      for (;;) {
        ISelEmitOperand op = {0};

        if (at(p, TK_ISEL_ONAME)) {
          op.kind = ISEL_PARAMETER_ONAME;
          op.value = p->l.tok.integer;
        } else if (at(p, TK_ISEL_INAME)) {
          op.kind = ISEL_PARAMETER_INAME;
          op.value = p->l.tok.integer;
        } else if (at(p, TK_ISEL_RESULT)) {
          op.kind = ISEL_PARAMETER_RESULT;
        } else if (at(p, TK_ISEL_NUMBER)) {
          op.kind = ISEL_PARAMETER_IMMEDIATE;
          op.value = p->l.tok.integer;
        } else if (at(p, TK_ISEL_REGISTER)) {
          op.kind = ISEL_PARAMETER_REGISTER;
          op.value = intern_register(p, as_span(p->l.tok.text));
        } else {
          ERR(p, "Expected emit operand");
        }

        next_token(p);
        vector_push(em.operands, op);
        if (at(p, TK_COMMA)) next_token(p);
        else break;
      }
    }
  } while (at(p, TK_ISEL_EMIT) || at(p, TK_ISEL_DISCARD));

  /// Yeet the dot.
  consume(p, TK_DOT);

  /// Add the rule.
  add_pattern(p, pat);
}

/// Parser entry point.
ISelTable *isel_table_parse(span filename, span data) {
  Parser p = {0};
  p.l.source = data;
  p.l.filename = filename.data;
  p.l.curr = data.data;
  p.l.end = data.data + data.size;
  p.l.lastc = ' ';
  p.table = calloc(1, sizeof(ISelTable));

  /// Set up error handling.
  if (setjmp(p.l.error_buffer)) {
    isel_table_free(p.table);
    return NULL;
  }

  /// Lex the first character and token.
  next_char(&p.l);
  next_token(&p);

  while (p.l.tok.type != TK_EOF) parse_rule(&p);
  return p.table;
}

/// ===========================================================================
///  ISel table.
/// ===========================================================================
/// Free an ISel table.
void isel_table_free(ISelTable *table) {
  free(table);
}

//// TODO: Use something like Aho-Corasick for instruction matching.