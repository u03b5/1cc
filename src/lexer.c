/*
 * MIT License
 *
 * Copyright (c) 2022 u03b5
 *
 */

#include "1cc/lexer.h"

#define IS_UPPER_ASCII(c) ('A' <= c && c <= 'Z')
#define IS_LOWER_ASCII(c) ('a' <= c && c <= 'z')
#define IS_IDENT_ASCII(c) (IS_UPPER_ASCII(c) || IS_LOWER_ASCII(c) || c == '_')
#define IS_INTEGER(c) ('0' <= c && c <= '9')
#define IS_HEXADECIMAL(c)                                                      \
  (IS_INTEGER(c) || ('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z'))
#define IS_OCTAL(c) ('0' <= c && c <= '7')
#define IS_NEWLINE(c) (c == '\n' || c == 13)
#define IS_SKIP(c) (IS_NEWLINE(c) || c == ' ' || c == 9)
#define IS_OPERATOR(c)                                                         \
  ((33 <= c && c <= 47) || (58 <= c && c <= 64) || (91 <= c && c <= 94) ||     \
   (123 <= c && c <= 126))

struct lexer {
  // TODO: implement the vector and use as a stack. much more efficient and
  // we dont have to worry about implementing any more complex containers
  // as long as we have a vector.
  // stack to contain each of the sources
  // contain as stack as we have to account for include directive
  Allocator *token_allocator;
  Arena *token_arena;
  Hashmap *token_map;
};

// constant token table used internally within the lexer
const Token *g_token_table[] = {
#define token(id, key)                                                    \
  &(Token){.kind = id, .position = key, .length = sizeof(key), .value.v = 0},
#define keyword(id, key)                                                  \
  &(Token){.kind = id, .position = key, .length = sizeof(key), .value.v = 0},
#include "1cc/import.h"
#undef keyword
#undef token
    &(Token){0, 0, 0, 0}};

Lexer *make_lexer(Allocator *allocator) {
  Lexer *lexer = malloc(sizeof(Lexer));
  lexer->token_allocator = allocator;
  lexer->token_arena = allocator_register(allocator, sizeof(Token));
  lexer->token_map = make_hashmap(__TOKEN_LOAD_SIZE__ + 1);
#define token(id, key)                                                      \
  hashmap_insert(lexer->token_map, key, (void *)g_token_table[id]);
#define keyword(id, key)                                                    \
  hashmap_insert(lexer->token_map, key, (void *)g_token_table[id]);
#include "1cc/import.h"
#undef keyword
#undef token
  return lexer;
}

void lexer_destroy(Lexer *lexer) {
  if (lexer) {
    allocator_deregister(lexer->token_allocator, sizeof(Token));
    hashmap_destroy(lexer->token_map);
    free(lexer);
  }
  return;
}

static Token *tokenize_integer(Lexer *lexer, Source *source) {
  char *cursor = source->cursor;
  int base = 10, is_floating = 0;
  if (*cursor == '0') {
    if (cursor[1] == 'x') {
      if (IS_HEXADECIMAL(cursor[2])) {
        source->cursor += 2;
        base = 16;
      } else {
        LOG_ERROR("Invalid hexadecimal numeral %d:%s\n", source->line,
                  source->path);
        return 0;
      }
    } else if (IS_OCTAL(cursor[1])) {
      ++source->cursor;
      base = 8;
    } else if (IS_INTEGER(cursor[1]))
      LOG_ERROR(
          "Invalid numeral, may not start with 0 unless denoting base system "
          "%d:%s\n",
          source->line, source->path);
  }
  while (*source->cursor) {
    if (*source->cursor == '.') {
      if (is_floating || base != 10 || !IS_INTEGER(source->cursor[1])) {
        break;
      } else {
        ++source->cursor;
        is_floating = 1;
      }
    }
    if (IS_INTEGER(*source->cursor))
      ++source->cursor;
    else
      break;
  }
  return ALLOC(lexer->token_arena, Token,
               {.kind = (is_floating) ? FLOAT_LITERAL : INTEGER_LITERAL,
                .position = cursor,
                .length = source->cursor - cursor,
                .value.i = base,
                .line = source->line});
}

// TODO: finish this at a later stage
// its so boring implementing this. ill get at it someday
static int handle_escaped_char(Lexer *lexer, Source *source) {
  switch (*source->cursor) {}
  return 0;
}

static Token *tokenize_char(Lexer *lexer, Source *source) {
  char *cursor = source->cursor;
  int result =
      (*cursor == '\\') ? handle_escaped_char(lexer, source) : *++cursor;
  if (*++cursor == '\'')
    return (result) ? ALLOC(lexer->token_arena, Token,
                            {.kind = CHAR_LITERAL,
                             .position = cursor,
                             .length = result,
                             .value.i = 0,
                             .line = source->line})
                    : 0;
  else
    return 0;
}

static Token *tokenize_string(Lexer *lexer, Source *source) {
  char *cursor = ++source->cursor;
  int length = 0;
  for (; *source->cursor != '\"'; ++source->cursor) {
    if (*source->cursor == 0 || *source->cursor == '\n') {
      LOG_ERROR("Unterminated string literal on line %d of %s\n", source->line,
                source->path);
      return 0;
    }
    if (*cursor == '\\') {
      ++source->cursor;
      int result = handle_escaped_char(lexer, source);
      if (result)
        length += result;
      else
        return 0;
    } else
      ++length;
  }
  ++source->cursor;
  return ALLOC(lexer->token_arena, Token,
               {.kind = STRING_LITERAL,
                .position = cursor,
                .length = length + 1,
                .value.v = 0,
                .line = source->line});
}

static Token *tokenize_ident(Lexer *lexer, Source *source) {
  char *cursor = source->cursor;
  size_t length;
  Token *kwrd_token;
  while (*source->cursor) {
    if (IS_IDENT_ASCII(*source->cursor))
      ++source->cursor;
    else
      break;
  }
  length = source->cursor - cursor;
  kwrd_token = (Token *)hashmap_nretrieve(lexer->token_map, cursor, length);
  return (kwrd_token) ? (kwrd_token)
                      : (ALLOC(lexer->token_arena, Token,
                               {.kind = IDENTIFIER,
                                .position = cursor,
                                .length = length,
                                .value.i = 0,
                                .line = source->line}));
}

static Token *tokenize_operator(Lexer *lexer, Source *source) {
  char *cursor = source->cursor;
  Token *tmp = 0;
  if (IS_OPERATOR(*cursor)) {
    ++source->cursor;
    switch (*(cursor + 1)) {
    case '=':
      goto token_retrieve;
    case '+':
      goto token_retrieve;
    case '-':
      goto token_retrieve;
    case '&':
      goto token_retrieve;
    case '|':
    token_retrieve:
      tmp = (Token *)hashmap_nretrieve(lexer->token_map, cursor, 2);
      break;
    default:
      break;
    }
    return (tmp) ? (++source->cursor, tmp)
                 : ((Token *)hashmap_nretrieve(lexer->token_map, cursor, 1));
  }
  return tmp;
}

static Token *lexer_get_internal(Lexer *lexer, Source *source) {
  char c;
  if (source->cursor) {
    while (IS_SKIP(*source->cursor)) {
      if (IS_NEWLINE(*source->cursor))
        ++source->line;
      ++source->cursor;
    }
    c = *source->cursor;
    switch (c) {
    case '\'':
      ++source->cursor;
      return tokenize_char(lexer, source);
    case '"':
      ++source->cursor;
      return tokenize_string(lexer, source);
    case 0:
      return 0;
    default:
      break;
    }
    if (IS_INTEGER(c))
      return tokenize_integer(lexer, source);
    if (IS_IDENT_ASCII(c))
      return tokenize_ident(lexer, source);
    if (IS_OPERATOR(c))
      return tokenize_operator(lexer, source);
    LOG_WARNING("Unrecognized Token %d found on line %d of %s %s:%d\n",
                *source->cursor, source->line, source->path, __FILE__,
                __LINE__);
  }
  return 0;
}

Token *lexer_get(Lexer *lexer, Source *source) {
  Token *tmp = 0;
  if (source) {
    if (source->peek_token_cache) {
      tmp = source->peek_token_cache;
      source->peek_token_cache = 0;
      return tmp;
    } else
      return source->prev_token_cache = lexer_get_internal(lexer, source);
  }
  return 0;
}

Token *lexer_peek(Lexer *lexer, Source *source) {
  if (lexer && source) {
    if (!source->peek_token_cache) {
      if (source->cursor >= (char *)(source->contents + source->size))
        return 0;
      source->peek_token_cache = lexer_get_internal(lexer, source);
      source->cursor -= source->peek_token_cache->length;
    }
    return source->peek_token_cache;
  }
  return 0;
}
