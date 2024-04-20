/**************************************************************************
 *
 * Copyright (c) 2024 Stefan Möding
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 **************************************************************************/


#include <stdlib.h>
#include <wchar.h>

#include "tree_sitter/parser.h"
#include "tree_sitter/alloc.h"
#include "tree_sitter/array.h"


/**
 * The tokens that this scanner will detect. The order must be the same as
 * defined in the 'externals' field in the grammar.
 */

enum TokenType {
  QMARK,
  SELBRACE,
  FIXED_STRING,
  EXPANDABLE_STRING,
  HEREDOC_START,
  HEREDOC_BODY,
  HEREDOC_END,
  INTERPOLATION,
};


/**
 * Helper function to check if a character is valid for a Puppet variable
 * name ('a'..'z', '0'..'9', '_').
 */

static inline bool is_variable_name(int32_t c) {
  return ((iswalpha(c) && islower(c)) || iswdigit(c) || (c == U'_'));
}


/**
 * Manage UTF32 strings using an array.
 */

typedef struct UTF32String {
  int      items;  // number of UTF32 code points stored in the string
  int      alloc;  // number of UTF32 code points allocated
  int32_t *value;  // pointer to the allocated code points
} UTF32String;

static UTF32String *string_alloc(int initial_size) {
  UTF32String *string = malloc(sizeof(UTF32String));

  if (string) {
    string->items = 0;
    string->alloc = initial_size;
    string->value = malloc(initial_size * sizeof(*string->value));
  }

  return string;
}

static void string_free(UTF32String *string) {
  if (string) {
    if (string->value) free(string->value);
    free(string);
  }
}

static inline void string_clear(UTF32String *string) {
  string->items = 0;
}

static inline int string_len(UTF32String *string) {
  return (string->items);
}

static inline int32_t string_char(UTF32String *string, int index) {
  return (string->items > index) ? string->value[index] : 0;
}

static void string_append(UTF32String *string, int32_t c) {
  if (string->items == string->alloc) {
    int new_size = 2 * string->alloc;
    string->value = realloc(string->value, new_size * sizeof(*string->value));
    string->alloc = new_size;
  }

  string->value[string->items] = c;
  string->items++;
}


/**
 * The state of the scanner to keep track of the heredoc tag that we are
 * currently looking for.
 */

typedef struct ScannerState {
  UTF32String *heredoc_tag;
  bool         interpolation_allowed;
  bool         check_selbrace;
} ScannerState;


/**
 * Scan for the opening brace after a question mark to detect a selector.
 */

static bool scan_selbrace(TSLexer *lexer, ScannerState *state) {
  for(;;) {
    // We are done if the end of file is reached
    if (lexer->eof(lexer)) return false;

    if (iswspace(lexer->lookahead) || (lexer->lookahead == U'\n')) {
      // Skip whitespace
      lexer->advance(lexer, true);
    }
    else if (lexer->lookahead == U'?') {
      state->check_selbrace = true;
      lexer->result_symbol = QMARK;
      lexer->advance(lexer, false);
      return true;
    }
    else if (lexer->lookahead == U'{') {
      if (state->check_selbrace) {
        state->check_selbrace = false;
        lexer->result_symbol = SELBRACE;
        lexer->advance(lexer, false);
        return true;
      }

      return false;
    }
    else {
      state->check_selbrace = false;
      return false;
    }
  }
}


/**
 * Scan for content of fixed and expandable strings. The function uses the
 * lookahead value as the start token and scans until the same token is found
 * again.
 */

static bool skip_quoted_string(TSLexer *lexer) {
  int32_t quote = lexer->lookahead;

  lexer->advance(lexer, false);

  for(;;) {
    // We are done if the end of file is reached
    if (lexer->eof(lexer)) return false;

    if (lexer->lookahead == quote) {
      return true;
    }
    else if (lexer->lookahead == U'\\') {
      // Consume backslash and the following character
      lexer->advance(lexer, false);
    }

    lexer->advance(lexer, false);
  }
}


/**
 * The string/heredoc interpolation styles we recognize. The style is used to
 * decide when the interpolation should end.
 */

enum InterpolationStyle {
  NONE,     // initially value before the style has been detected
  BRACE,    // ${foo}
  VARIABLE, // $foo
};


/**
 * Scan for an interpolation.
 */

static bool scan_interpolation(TSLexer *lexer) {
  int style = NONE;

  // The interpolation must start with a '$'
  if (lexer->lookahead != U'$') return false;

  lexer->result_symbol = INTERPOLATION;
  lexer->advance(lexer, false);

  for(;;) {
    // We are done if the end of file is reached
    if (lexer->eof(lexer)) return false;

    switch (style) {
    case NONE:
      // The style has not been defined yet so this must be the first
      // character after the '$'.
      if (lexer->lookahead == U'{') {
        style = BRACE;
      }
      else if (is_variable_name(lexer->lookahead)) {
        style = VARIABLE;
      }
      else {
        // The '$' is not followed by anything that looks like an
        // interpolation. The '$' has been already been consumed and
        // therefore will be treated as whatever the lexer finds next
        // (presumably a string content or a heredoc).
        return false;
      }
      break;

    case BRACE:
      if (lexer->lookahead == U'}') {
        lexer->advance(lexer, false);
        return true;
      }
      else if ((lexer->lookahead == U'\'') || (lexer->lookahead == U'"')) {
        if (!skip_quoted_string(lexer)) {
          // Skipping the quoted string failed. This happens when the string
          // is missing the ending quote.
          return false;
        }
      }
      break;

    case VARIABLE:
      if (!is_variable_name(lexer->lookahead)) {
        // Return if the end of a valid variable name is reached.
        return true;
      }
      break;
    }

    lexer->advance(lexer, false);
  }
}


/**
 * Scan over a single quoted string. Interpolation is not recognized in this
 * type of string.
 */

static bool scan_fixed_string(TSLexer *lexer) {
  lexer->result_symbol = FIXED_STRING;

  for(bool has_content=false;; has_content=true) {
    // We are done if the end of file is reached
    if (lexer->eof(lexer)) return false;

    if (lexer->lookahead == U'\'') {
      return has_content;
    }
    else if (lexer->lookahead == U'\\') {
      // Consume backslash and the following character
      lexer->advance(lexer, false);
    }
    lexer->advance(lexer, false);
  }
}


/**
 * Scan over a double quoted string. Interpolation is possible in this type
 * of string. If the '$' is detected we return to indicate that the string
 * content ends and the function scan_interpolation will continue.
 */

static bool scan_expandable_string(TSLexer *lexer) {
  lexer->result_symbol = EXPANDABLE_STRING;

  for(bool has_content=false;; has_content=true) {
    // We are done if the end of file is reached
    if (lexer->eof(lexer)) return false;

    if (lexer->lookahead == U'"') {
      return has_content;
    }
    else if (lexer->lookahead == U'$') {
      return has_content;
    }
    else if (lexer->lookahead == U'\\') {
      // Consume backslash and the following character
      lexer->advance(lexer, false);
    }
    lexer->advance(lexer, false);
  }
}


/**
 * Scan for a heredoc start tag: @( <endtag> [:<syntax>] [/<escapes>] )
 * The "@(" and ")" tokens are already defined in the grammar, so we do not
 * need to handle these here. The <endtag> token may be enclosed in double
 * quotes to indicate that interpolation is allowed.
 */

static bool scan_heredoc_start(TSLexer *lexer, ScannerState *state) {
  lexer->result_symbol = HEREDOC_START;

  for(;;) {
    // We are done if the end of file is reached
    if (lexer->eof(lexer)) return false;

    if (lexer->lookahead == U'\n') {
      // No match found when the end of the line is reached
      string_clear(state->heredoc_tag);
      return false;
    }
    else if (lexer->lookahead == U'"') {
      // The quote character indicates that interpolation can be used
      state->interpolation_allowed = true;
      lexer->advance(lexer, false);
    }
    else if (iswalnum(lexer->lookahead)) {
      // We seem to have a heredoc tag, so remember it to find the end
      string_append(state->heredoc_tag, lexer->lookahead);
      lexer->advance(lexer, false);
    }
    else if (lexer->lookahead == U':') {
      // Scan till the end of the syntax flags
      lexer->advance(lexer, false);
      while (iswalnum(lexer->lookahead)) {
        lexer->advance(lexer, false);
      }
    }
    else if (lexer->lookahead == U'/') {
      // Scan till the end of the escape flags
      lexer->advance(lexer, false);
      while (iswalpha(lexer->lookahead)) {
        lexer->advance(lexer, false);
      }
    }
    else if (lexer->lookahead == U')') {
      // We seem to have found the end of the heredoc tag
      return (string_len(state->heredoc_tag) > 0);
    }
    else if (iswspace(lexer->lookahead)) {
      // Skip whitespace
      lexer->advance(lexer, true);
    }
    else {
      // That doesn't look like a heredoc tag
      string_clear(state->heredoc_tag);
      return false;
    }
  }
}


/**
 * Scan a heredoc body. Set result symbol to HEREDOC_END if the tag from the
 * start of the heredoc has been found. Set result symbol to HEREDOC_BODY if
 * the heredoc supports interpolation and a '$' is found. The scanner then
 * uses the scan_interpolation function to continue scanning and returns here
 * later.
 */

static bool scan_heredoc_body(TSLexer *lexer, ScannerState *state) {
  int match_index = 0;

  for (;;) {
    // The final line might not have a newline at the end but we need to
    // check for the heredoc tag there as well
    if (lexer->eof(lexer)) {
      if (string_len(state->heredoc_tag) == match_index) {
        string_clear(state->heredoc_tag);
        lexer->result_symbol = HEREDOC_END;
        return true;
      }

      return false;
    }
    else if (lexer->lookahead == U'\n') {
      // Check for the heredoc tag when the end of the line is reached
      if (string_len(state->heredoc_tag) == match_index) {
        string_clear(state->heredoc_tag);
        lexer->result_symbol = HEREDOC_END;
        return true;
      }

      match_index = 0;
      lexer->advance(lexer, false);
    }
    else if (lexer->lookahead == U'$') {
      if (state->interpolation_allowed) {
        // Stop scanning the heredoc and allow the scanner to locate
        // interpolated values
        lexer->result_symbol = HEREDOC_BODY;
        return true;
      }

      lexer->advance(lexer, false);
    }
    else {
      // Check if the text in the current line matched the heredoc tag.
      // We start if the first alphanumeric character is found. The
      // comparison continues with the following characters.
      if (iswalnum(lexer->lookahead) && (match_index >= 0)) {
        if (string_char(state->heredoc_tag, match_index) == lexer->lookahead) {
          // The prefix matches so advance the index to the next character
          match_index++;
        }
        else {
          // It doesn't match so skip the rest of the line
          match_index = -1;
        }
      }

      lexer->advance(lexer, false);
    }
  }
}


/**
 * Our interface used by the tree-sitter parser
 */

void *tree_sitter_puppet_external_scanner_create() {
  ScannerState *state = ts_malloc(sizeof(ScannerState));

  if (state) {
    // Allocate a string for a heredoc tag with 16 slots. This can be
    // expanded later if the real tag is longer.
    state->heredoc_tag = string_alloc(16);
    state->interpolation_allowed = false;
    state->check_selbrace = false;
  }

  return state;
}

void tree_sitter_puppet_external_scanner_destroy(void *payload) {
  ScannerState *state = (ScannerState*)payload;

  if (state) {
    string_free(state->heredoc_tag);
    ts_free(state);
  }
}

unsigned tree_sitter_puppet_external_scanner_serialize(void *payload, char *buffer) {
  memcpy(buffer, payload, sizeof(ScannerState));

  return sizeof(ScannerState);
}

void tree_sitter_puppet_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
  memcpy(payload, buffer, length);
}

bool tree_sitter_puppet_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
  ScannerState *state = (ScannerState*)payload;

  if (valid_symbols[QMARK] || valid_symbols[SELBRACE]) {
    return scan_selbrace(lexer, state);
  }

  if (valid_symbols[INTERPOLATION] && scan_interpolation(lexer)) {
    return true;
  }

  if (valid_symbols[EXPANDABLE_STRING] && scan_expandable_string(lexer)) {
    return true;
  }

  if (valid_symbols[FIXED_STRING]) {
    return scan_fixed_string(lexer);
  }

  if (valid_symbols[HEREDOC_START]){
    return scan_heredoc_start(lexer, state);
  }

  if (valid_symbols[HEREDOC_BODY] || valid_symbols[HEREDOC_END]) {
    if (string_len(state->heredoc_tag) > 0) {
      return scan_heredoc_body(lexer, state);
    }
  }

  return false;
}
