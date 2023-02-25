#include <ast.h>
#include <ctype.h>
#include <errno.h>
#include <error.h>
#include <lexer.h>
#include <parser.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <vector.h>

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

/// ===========================================================================
///  Types and enums.
/// ===========================================================================
enum {
  PREFIX_PRECEDENCE = 10000,
};

typedef struct Parser {
  Lexer l;

  /// Whether we’re in a function.
  bool in_function;

  /// The AST of the program.
  AST *ast;
} Parser;

/// ===========================================================================
///  Lexer
/// ===========================================================================
/// All keywords.
static const struct {
  span kw;
  enum TokenType type;
} keywords[6] = {
    {literal_span_raw("if"), TK_IF},
    {literal_span_raw("else"), TK_ELSE},
    {literal_span_raw("while"), TK_WHILE},
    {literal_span_raw("ext"), TK_EXT},
    {literal_span_raw("as"), TK_AS},
    {literal_span_raw("type"), TK_TYPE},
};

/// Lex an identifier.
static void next_identifier(Lexer *p) {
  /// The start of the identifier.
  p->tok.type = TK_IDENT;
  next_char(p);

  /// Read the rest of the identifier.
  while (iscontinue(p->lastc)) {
    vector_push(p->tok.text, p->lastc);
    next_char(p);
  }
}

/// Lex the next token.
static void next_token(Lexer *p) {
  if (!lexer_start_token(p)) return;

  /// Lex the token.
  switch (p->lastc) {
    /// EOF.
    case 0:
      p->tok.type = TK_EOF;
      break;

    case '(':
      p->tok.type = TK_LPAREN;
      next_char(p);
      break;

    case ')':
      p->tok.type = TK_RPAREN;
      next_char(p);
      break;

    case '[':
      p->tok.type = TK_LBRACK;
      next_char(p);
      break;

    case ']':
      p->tok.type = TK_RBRACK;
      next_char(p);
      break;

    case '{':
      p->tok.type = TK_LBRACE;
      next_char(p);
      break;

    case '}':
      p->tok.type = TK_RBRACE;
      next_char(p);
      break;

    case ',':
      p->tok.type = TK_COMMA;
      next_char(p);
      break;

    case '@':
      p->tok.type = TK_AT;
      next_char(p);
      break;

    case ':':
      next_char(p);
      if (p->lastc == '=') {
        p->tok.type = TK_COLON_EQ;
        next_char(p);
      } else if (p->lastc == ':') {
        p->tok.type = TK_COLON_COLON;
        next_char(p);
      } else if (p->lastc == '>') {
        p->tok.type = TK_COLON_GT;
        next_char(p);
      } else {
        p->tok.type = TK_COLON;
      }
      break;

    case ';':
      lexer_skip_line(p);
      return next_token(p);

    case '#':
      next_char(p);
      p->tok.type = TK_HASH;
      break;

    case '.':
      next_char(p);
      p->tok.type = TK_DOT;
      break;

    case '+':
      next_char(p);
      p->tok.type = TK_PLUS;
      break;

    case '-':
      next_char(p);
      if (isdigit(p->lastc)) {
        next_number(p);
        p->tok.integer = -p->tok.integer;

        /// The character after a number must be a whitespace or delimiter.
        if (isalpha(p->lastc)) ERR(p, "Invalid integer literal");
      } else {
        p->tok.type = TK_MINUS;
      }
      break;

    case '*':
      next_char(p);
      p->tok.type = TK_STAR;
      break;

    case '/':
      next_char(p);
      p->tok.type = TK_SLASH;
      break;

    case '%':
      next_char(p);
      p->tok.type = TK_PERCENT;
      break;

    case '&':
      next_char(p);
      p->tok.type = TK_AMPERSAND;
      break;

    case '|':
      next_char(p);
      p->tok.type = TK_PIPE;
      break;

    case '^':
      next_char(p);
      p->tok.type = TK_CARET;
      break;

    case '~':
      next_char(p);
      p->tok.type = TK_TILDE;
      break;

    case '!':
      next_char(p);
      if (p->lastc == '=') {
        p->tok.type = TK_NE;
        next_char(p);
      } else {
        p->tok.type = TK_EXCLAM;
      }
      break;

    case '=':
      next_char(p);
      p->tok.type = TK_EQ;
      break;

    case '<':
      next_char(p);
      if (p->lastc == '=') {
        p->tok.type = TK_LE;
        next_char(p);
      } else if (p->lastc == '<') {
        p->tok.type = TK_SHL;
        next_char(p);
      } else {
        p->tok.type = TK_LT;
      }
      break;

    case '>':
      next_char(p);
      if (p->lastc == '=') {
        p->tok.type = TK_GE;
        next_char(p);
      } else if (p->lastc == '>') {
        p->tok.type = TK_SHR;
        next_char(p);
      } else {
        p->tok.type = TK_GT;
      }
      break;

    // String.
    case '"':
    case '\'':
      next_string(p);
      break;

    /// Number or identifier.
    default:
      /// Identifier.
      if (isstart(p->lastc)) {
        vector_clear(p->tok.text);
        vector_push(p->tok.text, p->lastc);
        next_identifier(p);

        for (size_t i = 0; i < sizeof keywords / sizeof *keywords; i++) {
          if (string_eq(keywords[i].kw, p->tok.text)) {
            p->tok.type = keywords[i].type;
            goto done;
          }
        }
        break;
      }

      /// Number.
      if (isdigit(p->lastc)) {
        next_number(p);

        /// The character after a number must be a whitespace or delimiter.
        if (isalpha(p->lastc)) ERR(p, "Invalid integer literal");
        break;
      }

      /// Anything else is invalid.
      ERR(p, "Invalid token");
  }

done:
  lexer_fin_token(p);
}

/// ===========================================================================
///  Parser helpers.
/// ===========================================================================
/// Get the current scope.
static Scope *curr_scope(Parser *p) { return vector_back(p->ast->scope_stack); }

/// Consume a token; error if it's not the expected type.
static void consume(Parser *p, enum TokenType tt) {
  if (p->l.tok.type != tt) ERR(&p->l, "Expected %s, but got %s",
    token_type_to_string(tt), token_type_to_string(p->l.tok.type));
  next_token(&p->l);
}

/// Check if a token can be a postfix operator.
static bool is_postfix_operator(enum TokenType tt) {
  switch (tt) {
    default: return false;
  }
}

/// Get the binary precedence of a token.
/// TODO: User-defined operators.
static isz binary_operator_precedence(Parser *p, Token t) {
  (void) p;
  switch (t.type) {
    case TK_DOT: return 1000000000;
    case TK_AS: return 1000;

    case TK_STAR:
    case TK_SLASH:
    case TK_PERCENT:
      return 600;

    case TK_PLUS:
    case TK_MINUS:
      return 500;

    case TK_SHL:
    case TK_SHR:
      return 400;

    case TK_AMPERSAND:
    case TK_PIPE:
    case TK_CARET:
      return 300;

    case TK_EQ:
    case TK_NE:
    case TK_LT:
    case TK_GT:
    case TK_LE:
    case TK_GE:
      return 200;

    case TK_COLON_EQ:
    case TK_COLON_COLON:
      return 100;

    /// Not an operator.
    default: return -1;
  }
}

/// Check if an operator is right-associative.
/// TODO: User-defined operators.
static bool is_right_associative(Parser *p, Token t) {
  (void) p;
  switch (t.type) {
    case TK_STAR:
    case TK_SLASH:
    case TK_PERCENT:
    case TK_PLUS:
    case TK_MINUS:
    case TK_SHL:
    case TK_SHR:
    case TK_AMPERSAND:
    case TK_PIPE:
    case TK_CARET:
    case TK_EQ:
    case TK_NE:
    case TK_LT:
    case TK_GT:
    case TK_LE:
    case TK_GE:
      return false;

    case TK_COLON_EQ:
    case TK_COLON_COLON:
      return true;

    /// Not an operator.
    default: return false;
  }
}

/// ===========================================================================
///  Parser
/// ===========================================================================
static Node *parse_expr_with_precedence(Parser *p, isz current_precedence);
static Type *parse_type(Parser *p);
static Node *parse_expr(Parser *p) { return parse_expr_with_precedence(p, 0); }

/// <expr-block>     ::= "{" { <expression> } "}"
static Node *parse_block(Parser *p, bool create_new_scope) {
  loc pos = p->l.tok.source_location;
  consume(p, TK_LBRACE);

  /// Create a new scope.
  if (create_new_scope) scope_push(p->ast);

  /// Collect the children.
  Nodes children = {0};
  while (p->l.tok.type != TK_RBRACE) vector_push(children, parse_expr(p));
  consume(p, TK_RBRACE);

  /// Pop the scope.
  if (create_new_scope) scope_pop(p->ast);

  /// Create the node.
  return ast_make_block(p->ast, pos, children);
}

/// <expr-if>        ::= IF <expression> <expr-block> [ ELSE <expr-block> ]
static Node *parse_if_expr(Parser *p) {
  /// Yeet "if".
  loc if_loc = p->l.tok.source_location;
  consume(p, TK_IF);

  /// Parse the condition.
  Node *cond = parse_expr(p);

  /// Parse the "then" block.
  scope_push(p->ast);
  Node *then_block = parse_expr(p);
  scope_pop(p->ast);

  /// Parse the "else" block if there is one.
  Node *else_block = NULL;
  if (p->l.tok.type == TK_ELSE) {
    next_token(&p->l);
    scope_push(p->ast);
    else_block = parse_expr(p);
    scope_pop(p->ast);
  }

  /// Done.
  return ast_make_if(p->ast, if_loc, cond, then_block, else_block);
}

/// <expr-while>     ::= WHILE <expression> <expression>
static Node *parse_while_expr(Parser *p) {
  /// Yeet "while".
  loc while_loc = p->l.tok.source_location;
  consume(p, TK_WHILE);

  /// Parse the condition.
  Node *cond = parse_expr(p);

  /// Parse the body.
  scope_push(p->ast);
  Node *body = parse_expr(p);
  scope_pop(p->ast);

  /// Done.
  return ast_make_while(p->ast, while_loc, cond, body);
}

/// <expr-call> ::= <expression> "(" { <expression> [ "," ] } ")"
static Node *parse_call_expr(Parser *p, Node *callee) {
  consume(p, TK_LPAREN);

  /// Collect the arguments.
  Nodes args = {0};
  while (p->l.tok.type != TK_RPAREN) {
    vector_push(args, parse_expr(p));
    if (p->l.tok.type == TK_COMMA) next_token(&p->l);
  }

  /// Done.
  Node *call = ast_make_call(p->ast, (loc){callee->source_location.start, p->l.tok.source_location.end}, callee, args);
  consume(p, TK_RPAREN);
  return call;
}

/// Parse the body of a function.
///
/// This is basically just a wrapper around `parse_block()` that
/// also injects declarations for all the function parameters.
static Node *parse_function_body(Parser *p, Type *function_type, Nodes *param_decls) {
  /// Save state.
  bool save_in_function = p->in_function;
  p->in_function = true;

  /// Yeet "=" if found.
  if (p->l.tok.type == TK_EQ) next_token(&p->l);

  /// Push a new scope for the body and parameters.
  scope_push(p->ast);

  /// Create a declaration for each parameter.
  foreach (Parameter , param, function_type->function.parameters) {
    Node *var = ast_make_declaration(p->ast, param->source_location, param->type, as_span(param->name), NULL);
    if (!scope_add_symbol(curr_scope(p), SYM_VARIABLE, as_span(var->declaration.name), var))
      ERR_AT(&p->l, var->source_location, "Redefinition of parameter '%S'", var->declaration.name);
    vector_push(*param_decls, var);
  }

  /// Parse the body.
  Node *block = parse_expr(p);

  /// Pop the scope.
  scope_pop(p->ast);
  p->in_function = save_in_function;

  /// Done.
  return block;
}

/// Parse an expression that starts with a type.
///
/// <expr-cast>      ::= <type> <expression>
/// <expr-lambda>    ::= <type-function> <expr-block>
static Node *parse_type_expr(Parser *p, Type *type) {
  /// If this is a function type, then this is a lambda expression.
  if (type->kind == TYPE_FUNCTION) {
    /// Parse the function body.
    Nodes params = {0};
    Node *body = parse_function_body(p, type, &params);

    /// Create a function for the lambda.
    string name = format("_XLambda_%Z", p->ast->counter++);
    Node *func = ast_make_function(p->ast, type->source_location, type, params, body, as_span(name));
    free(name.data);
    func->function.global = false;
    return func;
  }

  /// Otherwise, this is an error.
  /// TODO: Struct literals.
  ERR_AT(&p->l, type->source_location, "Expected expression, got type");
}

/// <param-decl> ::= <decl-start> <type>
static Parameter parse_param_decl(Parser *p) {
  loc start = p->l.tok.source_location;

  /// Parse the name, colon, and type.
  string name = string_dup(p->l.tok.text);
  consume(p, TK_IDENT);
  consume(p, TK_COLON);
  Type *type = parse_type(p);

  /// Function types are converted to their corresponding pointer type
  /// when used as a parameter type.
  if (type->kind == TYPE_FUNCTION) type = ast_make_type_pointer(p->ast, type->source_location, type);

  /// Done.
  return (Parameter){.source_location = start, .type = type, .name = name};
}

static Member parse_struct_member(Parser *p) {
  loc start = p->l.tok.source_location;

  /// Parse the name, colon, and type.
  string name = string_dup(p->l.tok.text);
  consume(p, TK_IDENT);
  consume(p, TK_COLON);
  Type *type = parse_type(p);

  /// Function types are converted to their corresponding pointer type
  /// when used as a parameter type.
  if (type->kind == TYPE_FUNCTION) type = ast_make_type_pointer(p->ast, type->source_location, type);

  /// Done.
  return (Member){.source_location = start, .type = type, .name = name, .byte_offset = 0};
}

/// <type-derived>  ::= <type-array> | <type-function>
/// <type-array>    ::= <type> "[" <expression> "]"
/// <type-function> ::= <type> "(" { <param-decl> [ "," ]  } ")"
static Type *parse_type_derived(Parser *p, Type *base) {
  ASSERT(base);

  /// Parse the rest of the type.
  for (;;) {
    switch (p->l.tok.type) {
      /// Array type.
      case TK_LBRACK: {
        next_token(&p->l);
        Node *size = parse_expr(p);

        /// TODO: Evaluate the size as a constant expression.
        if (size->kind != NODE_LITERAL || size->literal.type != TK_NUMBER)
          ISSUE_DIAGNOSTIC(DIAG_SORRY, size->source_location, &p->l,
            "Non-literal array size not supported");
        usz dim = size->literal.integer;

        /// Yeet "]" and record the location.
        loc l = {.start = base->source_location.start, .end = p->l.tok.source_location.end};
        consume(p, TK_RBRACK);

        /// Base type must not be incomplete.
        if (type_is_incomplete(base)) ERR_AT(&p->l, l, "Cannot create array of incomplete type: %T", base);

        /// Create the array type.
        base = ast_make_type_array(p->ast, l, base, dim);
      } break;

      /// Function type.
      case TK_LPAREN: {
        next_token(&p->l);

        /// Collect the arguments.
        Parameters args = {0};
        while (p->l.tok.type != TK_RPAREN) {
          Parameter decl = parse_param_decl(p);
          vector_push(args, decl);
          if (p->l.tok.type == TK_COMMA) next_token(&p->l);
        }


        /// Yeet ")".
        loc l = {.start = base->source_location.start, .end = p->l.tok.source_location.end};
        consume(p, TK_RPAREN);

        /// Create the function type.
        base = ast_make_type_function(p->ast, l, base, args);
      } break;

      /// Done.
      default: return base;
    }
  }
}

/// <type>           ::= <type-base> | <type-pointer> | <type-derived> | <type-struct>
/// <type-pointer>   ::= "@" { "@" } ( IDENTIFIER | "(" <type> ")" )
/// <type-struct>    ::= TYPE <struct-body>
/// <type-base>      ::= IDENTIFIER
static Type *parse_type(Parser *p) {
  loc start = p->l.tok.source_location;

  /// Collect pointers.
  usz level = 0;
  while (p->l.tok.type == TK_AT) {
    level++;
    next_token(&p->l);
  }

  /// Parse the base type.
  if (p->l.tok.type == TK_IDENT) {
    /// Make sure the identifier is a type.
    Symbol *sym = scope_find_or_add_symbol(curr_scope(p), SYM_TYPE, as_span(p->l.tok.text), false);
    if (sym->kind != SYM_TYPE) ERR(&p->l, "'%S' is not a type!", as_span(p->l.tok.text));

    /// Create a named type from it.
    Type *base = ast_make_type_named(p->ast, p->l.tok.source_location, sym);

    /// If we have pointer indirection levels, wrap the type in a pointer.
    while (level--) base = ast_make_type_pointer(p->ast, (loc){start.start--, p->l.tok.source_location.end}, base);

    /// Yeet the identifier and parse the rest of the type.
    base->source_location.start = start.start;
    next_token(&p->l);
    return parse_type_derived(p, base);
  }

  /// Alternatively, we allow any type, enclosed in parens.
  if (p->l.tok.type == TK_LPAREN) {
    next_token(&p->l);
    Type *base = parse_type(p);

    /// If we have pointer indirection levels, wrap the type in a pointer.
    while (level--) base = ast_make_type_pointer(p->ast, (loc){start.start--, p->l.tok.source_location.end}, base);

    /// Yeet ")" and parse the rest of the type.
    base->source_location.start = start.start;
    consume(p, TK_RPAREN);
    return parse_type_derived(p, base);
  }

  // Structure type definition
  if (p->l.tok.type == TK_TYPE) {
    loc type_kw_loc = p->l.tok.source_location;
    // Yeet TK_TYPE
    next_token(&p->l);
    consume(p, TK_LBRACE);
    Members members = {0};
    while (p->l.tok.type != TK_RBRACE) {
      Member member_decl = parse_struct_member(p);
      vector_push(members, member_decl);
      if (p->l.tok.type == TK_COMMA) next_token(&p->l);
    }
    consume(p, TK_RBRACE);
    return ast_make_type_struct(p->ast, type_kw_loc, members);
  }

  /// Invalid base type.
  ERR(&p->l, "Expected base type, got %s", token_type_to_string(p->l.tok.type));
}

/// <expr-decl>      ::= <decl-start> <decl-rest>
/// <decl-rest>      ::= <type-function> <expression>
///                    | <type> [ "=" <expression> ]
///                    | <decl-start> EXT <type-function>
static Node *parse_decl_rest(Parser *p, string ident, loc location) {
  /// If the next token is "ext", then this is an external declaration.
  bool is_ext = false;
  if (p->l.tok.type == TK_EXT) {
    is_ext = true;
    next_token(&p->l);
  }

  /// Parse the type.
  Type *type = parse_type(p);

  /// If the type is a function type, parse the body if it isn’t extern.
  if (type->kind == TYPE_FUNCTION) {
    /// Not external.
    if (!is_ext) {
      /// Create a symbol table entry before parsing the body.
      Symbol *sym = scope_add_symbol_unconditional(curr_scope(p), SYM_FUNCTION, as_span(ident), NULL);

      if (sym->kind != SYM_FUNCTION || sym->val.node)
        ERR_AT(&p->l, location, "Redefinition of symbol '%S'", as_span(ident));

      /// Parse the body, create the function, and update the symbol table.
      Nodes params = {0};
      Node *body = parse_function_body(p, type, &params);
      Node *func = ast_make_function(p->ast, location, type, params, body, as_span(ident));
      sym->val.node = func;
      Node *funcref = ast_make_function_reference(p->ast, location, as_span(ident));
      funcref->funcref.resolved = sym;
      return funcref;
    }

    /// External.
    else {
      /// Create a symbol table entry.
      Symbol *sym = scope_find_or_add_symbol(curr_scope(p), SYM_FUNCTION, as_span(ident), true);
      if (sym->kind != SYM_FUNCTION || sym->val.node)
        ERR_AT(&p->l, location, "Redefinition of symbol '%S'", as_span(ident));

      /// Create the function.
      Node *func = ast_make_function(p->ast, location, type, (Nodes){0}, NULL, as_span(ident));
      sym->val.node = func;
      Node *funcref = ast_make_function_reference(p->ast, location, as_span(ident));
      funcref->funcref.resolved = sym;
      return funcref;
    }
  }

  /// Create the declaration.
  Node *decl = ast_make_declaration(p->ast, location, type, as_span(ident), NULL);

  /// Make the variable static if we’re at global scope.
  decl->declaration.static_ = p->ast->scope_stack.size == 1;

  /// Add the declaration to the current scope.
  if (!scope_add_symbol(curr_scope(p), SYM_VARIABLE, as_span(ident), decl))
    ERR_AT(&p->l, location, "Redefinition of symbol '%S'", ident);

  /// A non-external declaration may have an initialiser.
  if (p->l.tok.type == TK_EQ) {
    if (is_ext) ERR(&p->l, "An \"ext\" declaration may not have an initialiser");
    next_token(&p->l);

    /// Need to do this manually because the symbol that is the variable
    /// may appear in its initialiser, albeit only in unevaluated contexts.
    decl->declaration.init = parse_expr(p);
    decl->declaration.init->parent = decl;
  }

  /// Done.
  return decl;
}

/// Declaration, call, or cast.
///
/// <decl-start>   ::= IDENTIFIER ":"
/// <expr-primary> ::= NUMBER | IDENTIFIER
static Node *parse_ident_expr(Parser *p) {
  /// We know that we’re looking at an identifier; save it for later.
  string ident = string_dup(p->l.tok.text);
  loc location = p->l.tok.source_location;
  next_token(&p->l);

  /// If the next token is a colon, then this is some sort of declaration.
  if (p->l.tok.type == TK_COLON) {
    /// Parse the rest of the declaration.
    next_token(&p->l);
    Node *decl = parse_decl_rest(p, ident, location);
    free(ident.data);
    return decl;
  } else if (p->l.tok.type == TK_COLON_GT) {
    next_token(&p->l);

    /// Parse the type.
    Type *type = parse_type(p);

    if (type->kind == TYPE_STRUCT) {
      Symbol *struct_decl_sym = scope_find_or_add_symbol(curr_scope(p), SYM_TYPE, as_span(ident), true);
      struct_decl_sym->val.type = type;
      Node *struct_decl = ast_make_structure_declaration(p->ast, location, struct_decl_sym);
      type->structure.decl = struct_decl;
      struct_decl->type = type;
      free(ident.data);
      return struct_decl;
    }
    free(ident.data);
    TODO("Named type alias not implemented");
  } else if (p->l.tok.type == TK_COLON_COLON) {
    /// Create the declaration.
    Node *decl = ast_make_declaration(p->ast, location, NULL, as_span(ident), NULL);

    /// Make the variable static if we’re at global scope.
    decl->declaration.static_ = p->ast->scope_stack.size == 1;

    /// Add the declaration to the current scope.
    if (!scope_add_symbol(curr_scope(p), SYM_VARIABLE, as_span(ident), decl))
      ERR_AT(&p->l, location, "Redefinition of symbol '%S'", ident);

    /// A type-inferred declaration MUST have an initialiser.
    next_token(&p->l);

    /// Need to do this manually because the symbol that is the variable
    /// may appear in its initialiser, albeit only in unevaluated contexts.
    decl->declaration.init = parse_expr(p);
    decl->declaration.init->parent = decl;

    /// Done.
    free(ident.data);
    return decl;
  }

  /// Otherwise, check if the identifier is a declared symbol; if it isn’t,
  /// it can only be a function name, so add it as a symbol.
  Symbol *sym = scope_find_symbol(curr_scope(p), as_span(ident), false);

  /// If the symbol is a variable or function, then create a variable or
  /// function reference, and we’re done here.
  if (!sym || sym->kind == SYM_FUNCTION) {
    Node *ref = ast_make_function_reference(p->ast, location, as_span(ident));
    free(ident.data);
    return ref;
  }

  /// Identifier is no longer needed at this point.
  free(ident.data);
  if (sym->kind == SYM_VARIABLE) return ast_make_variable_reference(p->ast, location, sym);

  /// If the symbol is a type, then parse the rest of the type and delegate.
  if (sym->kind == SYM_TYPE) {
    Type *type = parse_type_derived(p, ast_make_type_named(p->ast, location, sym));
    return parse_type_expr(p, type);
  }

  /// Should never get here.
  UNREACHABLE();
}

/// Parse an expression. This function handles the following rules:
///
/// <expression> ::= <expr-decl>
///              | <expr-if>
///              | <expr-while>
///              | <expr-block>
///              | <expr-lambda>
///              | <expr-call>
///              | <expr-cast>
///              | <expr-subs>
///              | <expr-paren>
///              | <expr-prefix>
///              | <expr-binary>
///              | <expr-primary>
///
/// <expr-subs>    ::= <expression> "[" <expression> "]"
/// <expr-paren>   ::= "(" <expression> ")"
/// <expr-prefix>  ::= <prefix> <expression>
/// <expr-binary>  ::= <expression> <binary> <expression>
/// <expr-primary> ::= NUMBER | IDENTIFIER
static Node *parse_expr_with_precedence(Parser *p, isz current_precedence) {
  /// Left-hand side of operator.
  Node *lhs = NULL;

  /// Parse the LHS.
  switch (p->l.tok.type) {
    default: ERR(&p->l, "Expected expression, got %s", token_type_to_string(p->l.tok.type));

    /// An identifier can either be a declaration, function call, or cast.
    case TK_IDENT: lhs = parse_ident_expr(p); break;
    case TK_IF: lhs = parse_if_expr(p); break;
    case TK_ELSE: ERR(&p->l, "'else' without 'if'");
    case TK_WHILE: lhs = parse_while_expr(p); break;
    case TK_LBRACE: lhs = parse_block(p, true); break;
    case TK_NUMBER:
      lhs = ast_make_integer_literal(p->ast, p->l.tok.source_location, p->l.tok.integer);
      next_token(&p->l);
      break;
    case TK_STRING:
      lhs = ast_make_string_literal(p->ast, p->l.tok.source_location, as_span(p->l.tok.text));
      next_token(&p->l);
      break;
    case TK_LPAREN:
      next_token(&p->l);
      lhs = parse_expr(p);
      consume(p, TK_RPAREN);
      break;
    case TK_RPAREN: ERR(&p->l, "Unmatched ')'");
    case TK_RBRACK: ERR(&p->l, "Unmatched ']'");
    case TK_RBRACE: ERR(&p->l, "Unmatched '}'");

    /// '@' is complicated because it can either be a dereference or a cast.
    case TK_AT: {
      /// Collect all at signs.
      usz at_count = 0;
      do {
        at_count++;
        next_token(&p->l);
      } while (p->l.tok.type == TK_AT);

      /// If the next token can be the start of a <type-base>, then this is
      /// a type; parse the type and wrap it in a pointer type.
      if (p->l.tok.type == TK_IDENT) {
        Symbol *sym = scope_find_symbol(curr_scope(p), as_span(p->l.tok.text), false);
        if (sym && sym->kind == SYM_TYPE) {
          Type *type = ast_make_type_named(p->ast, p->l.tok.source_location, sym);
          next_token(&p->l);
          while (at_count--) type = ast_make_type_pointer(p->ast, p->l.tok.source_location, parse_type_derived(p, type));
          lhs = parse_type_expr(p, type);
          break;
        }
      }

      /// Otherwise, this is a dereference.
      lhs = parse_expr_with_precedence(p, PREFIX_PRECEDENCE);

      /// Wrap it in an appropriate number of dereferences.
      for (usz i = 0; i < at_count; i++) {
        loc l = lhs->source_location;
        lhs = ast_make_unary(p->ast, (loc){.start = l.start - 1, .end = l.end}, TK_AT, false, lhs);
      }
    } break;

    /// Unary operators.
    case TK_MINUS:
    case TK_AMPERSAND:
    case TK_TILDE:
    case TK_EXCLAM:
    case TK_STAR: {
      u32 start = p->l.tok.source_location.start;
      enum TokenType tt = p->l.tok.type;
      next_token(&p->l);
      Node *operand = parse_expr_with_precedence(p, PREFIX_PRECEDENCE);
      lhs = ast_make_unary(p->ast, (loc){.start = start, .end = operand->source_location.end}, tt, false, operand);
    } break;
  }

  /// This *must* not be NULL.
  if (!lhs) ICE("LHS is NULL");

  /// The rules for operator precedence parsing are as follows:
  ///     - unary prefix operators are unambiguously handled up above;
  ///     - if the current token is a, unary postfix operator, then the
  ///       current LHS is its operand;
  ///     - if the current token is a binary operator whose precedence is
  ///       higher than the current precedence, or higher than or equal to
  ///       the current precedence if the operator is right-associative, then
  ///       the current LHS is the LHS of that operator;
  ///     - if the current token is "(" or "[", then this is a call/subscript
  ///       expression. We handle these explicitly here since they usually have
  ///       the highest precedence anyway.
  ///     - otherwise, return the current LHS as its own expression.
  for (;;) {
    /// TODO: Postfix operators also need to respect precedence.
    /// Handle unary postfix operators.
    if (is_postfix_operator(p->l.tok.type)) {
      lhs = ast_make_unary(p->ast, (loc){.start = lhs->source_location.start, p->l.tok.source_location.end}, p->l.tok.type, true, lhs);
      next_token(&p->l);
      continue;
    }

    /// Handle calls.
    if (p->l.tok.type == TK_LPAREN) {
      lhs = parse_call_expr(p, lhs);
      continue;
    }

    /// Handle subscripts.
    if (p->l.tok.type == TK_LBRACK) {
      next_token(&p->l);
      Node *index = parse_expr(p);
      consume(p, TK_RBRACK);
      lhs = ast_make_binary(p->ast, (loc){.start = lhs->source_location.start, .end = index->source_location.end}, TK_LBRACK, lhs, index);
      continue;
    }

    /// Handle binary operators. We can just check if the precedence of the current
    /// token is less than the current precedence, even if the current token is not
    /// an operator because `binary_operator_precedence` returns -1 in that case.
    isz prec = binary_operator_precedence(p, p->l.tok);

    /// If the precedence of the current token is less than the current precedence,
    /// then we're done.
    if (prec < current_precedence) return lhs;

    /// If the precedence is the same, we’re done if the token is left-associative.
    if (prec == current_precedence && !is_right_associative(p, p->l.tok)) return lhs;

    /// Otherwise, we need to parse the RHS.
    enum TokenType tt = p->l.tok.type;
    next_token(&p->l);

    /// The `as` operator is special because its RHS is a type.
    if (tt == TK_AS) {
      Type *type = parse_type(p);
      lhs = ast_make_cast(p->ast, (loc){.start = lhs->source_location.start, .end = type->source_location.end}, type, lhs);
      continue;
    }

    /// The `as` operator is special because its RHS is a type.
    if (tt == TK_DOT) {
      if (p->l.tok.type != TK_IDENT) ERR(&p->l, "RHS of operator '.' must be an identifier.");
      lhs = ast_make_member_access(p->ast, (loc){.start = lhs->source_location.start, .end = p->l.tok.source_location.end}, as_span(p->l.tok.text), lhs);
      // Yeet identifier token
      next_token(&p->l);
      continue;
    }

    /// Otherwise, the RHS is a regular expression.
    else {
      Node *rhs = parse_expr_with_precedence(p, prec);

      /// Combine the LHS and RHS into a binary expression.
      lhs = ast_make_binary(p->ast, (loc){.start = lhs->source_location.start, .end = rhs->source_location.end}, tt, lhs, rhs);
    }
  }
}

/// ===========================================================================
///  API
/// ===========================================================================
AST *parse(span source, const char *filename) {
  Parser p = {0};
  p.l.source = source;
  p.l.filename = filename;
  p.l.curr = source.data;
  p.l.end = source.data + source.size - 1;
  p.l.lastc = ' ';
  p.ast = ast_create();
  p.ast->filename = string_create(filename);
  p.ast->source = string_dup(source);

  /// Set up error handling.
  if (setjmp(p.l.error_buffer)) {
    ast_free(p.ast);
    return NULL;
  }

  /// Lex the first character and token.
  next_char(&p.l);
  next_token(&p.l);

  /// Parse the file.
  /// <file> ::= { <expression> }
  while (p.l.tok.type != TK_EOF) {
    Node *expr = parse_expr(&p);
    vector_push(p.ast->root->root.children, expr);
    expr->parent = p.ast->root;
  }
  return p.ast;
}

NODISCARD const char *token_type_to_string(enum TokenType type) {
  switch (type) {
    default: return "<unknown>";
    case TK_INVALID: return "invalid";
    case TK_EOF: return "EOF";
    case TK_IDENT: return "identifier";
    case TK_NUMBER: return "number";
    case TK_STRING: return "string";
    case TK_IF: return "if";
    case TK_ELSE: return "else";
    case TK_WHILE: return "while";
    case TK_EXT: return "ext";
    case TK_AS: return "as";
    case TK_TYPE: return "type";
    case TK_LPAREN: return "\"(\"";
    case TK_RPAREN: return "\")\"";
    case TK_LBRACK: return "\"[\"";
    case TK_RBRACK: return "\"]\"";
    case TK_LBRACE: return "\"{\"";
    case TK_RBRACE: return "\"}\"";
    case TK_COMMA: return "\",\"";
    case TK_COLON: return "\":\"";
    case TK_SEMICOLON: return "\";\"";
    case TK_DOT: return "\".\"";
    case TK_PLUS: return "\"+\"";
    case TK_MINUS: return "\"-\"";
    case TK_STAR: return "\"*\"";
    case TK_SLASH: return "\"/\"";
    case TK_PERCENT: return "\"%\"";
    case TK_AMPERSAND: return "\"&\"";
    case TK_PIPE: return "\"|\"";
    case TK_CARET: return "\"^\"";
    case TK_TILDE: return "\"~\"";
    case TK_EXCLAM: return "\"!\"";
    case TK_AT: return "\"@\"";
    case TK_HASH: return "\"#\"";
    case TK_SHL: return "\"<<\"";
    case TK_SHR: return "\">>\"";
    case TK_EQ: return "\"=\"";
    case TK_NE: return "\"!=\"";
    case TK_LT: return "\"<\"";
    case TK_GT: return "\">\"";
    case TK_LE: return "\"<=\"";
    case TK_GE: return "\">=\"";
    case TK_COLON_EQ: return "\":=\"";
    case TK_COLON_COLON: return "\"::\"";
    case TK_COLON_GT: return "\":>\"";
  }

  UNREACHABLE();
}

